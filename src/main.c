#define _GNU_SOURCE

/**
 * MIT License
 * Copyright (c) 2020 Mitosis-Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/mempolicy.h>
#include <numa.h>
#include <numaif.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <inttypes.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>     /* open */
#include <unistd.h>    /* exit */
#include <sys/ioctl.h> /* ioctl */
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>

#ifdef _OPENMP
#    include <omp.h>
#endif

#include "config.h"

FILE *opt_file_out = NULL;  ///< standard outpu
int benchmark_mem_node = 0;

extern int real_main(int argc, char *argv[]);

void signalhandler(int sig)
{
    fprintf(opt_file_out, "<sig>Signal %i caught!</sig>\n", sig);

    FILE *fd3 = fopen(CONFIG_SHM_FILE_NAME ".done", "w");

    if (fd3 == NULL) {
        fprintf(stderr, "ERROR: could not create the shared memory file descriptor\n");
        exit(-1);
    }

    usleep(250);

    fprintf(opt_file_out, "</benchmark>\n");

    exit(0);
}

static void pin_to_numa_node_cpus(int node) {
    struct bitmask *cpus = numa_allocate_cpumask();
    if (!cpus) {
        perror("numa_allocate_cpumask");
        exit(1);
    }

    if (numa_node_to_cpus(node, cpus) != 0) {
        /* libnuma returns -1 on error, but errno is not always useful here. */
        fprintf(stderr, "numa_node_to_cpus(%d) failed\n", node);
        exit(1);
    }

    cpu_set_t set;
    CPU_ZERO(&set);

    int any = 0;
    for (unsigned i = 0; i < cpus->size; i++) {
        if (numa_bitmask_isbitset(cpus, i)) {
            CPU_SET((int)i, &set);
            any = 1;
        }
    }

    if (!any) {
        fprintf(stderr, "No CPUs found for NUMA node %d\n", node);
        exit(1);
    }

    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("sched_setaffinity");
        exit(1);
    }

    numa_free_cpumask(cpus);
}

static void set_mem_policy_bind_node(int node) {
    struct bitmask *nodes = numa_allocate_nodemask();
    if (!nodes) {
        perror("numa_allocate_nodemask");
        exit(1);
    }

    numa_bitmask_clearall(nodes);
    numa_bitmask_setbit(nodes, node);

    /* maxnode is the number of bits in the nodemask. */
    unsigned long maxnode = nodes->size;

    if (set_mempolicy(MPOL_BIND, nodes->maskp, maxnode) != 0) {
        perror("set_mempolicy(MPOL_BIND)");
        exit(1);
    }

    numa_free_nodemask(nodes);
}

int main(int argc, char *argv[])
{
    struct timeval tstart, tend;
    gettimeofday(&tstart, NULL);
    int cpu_node = 0;
    int mem_node = 0;

    for (int i = 0; i < argc; i++) {
        printf("%s ", argv[i]);
    }
    printf("\n");

    /* check if NUMA is available, otherwise we don't know how to allocate memory */
    if (numa_available() == -1) {
        fprintf(stderr, "ERROR: Numa not available on this machine.\n");
        return -1;
    }

    opt_file_out = stdout;
    int c;
    while ((c = getopt(argc, argv, "ho:p:m:")) != -1) {
        switch (c) {
        case 'h':
            printf("usage: %s [-o FILE] [-p CPU_NODE] [-m MEM_NODE]\n", argv[0]);
            return 0;
        case 'o':
            opt_file_out = fopen(optarg, "a");
            if (opt_file_out == NULL) {
                fprintf(stderr, "Could not open the file '%s' switching to stdout\n", optarg);
                opt_file_out = stdout;
            }
            break;
        case 'p':
            cpu_node = (int)strtol(optarg, NULL, 10);
            break;
        case 'm':
            mem_node = (int)strtol(optarg, NULL, 10);
            break;
        case '?':
            switch (optopt) {
            case 'o':
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                return -1;
            case 'p':
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                return -1;
            case 'm':
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
                return -1;
            default:
                fprintf(stderr, "Unknown option.\n");
                return -1;
            }
        }
    }

    if (cpu_node > numa_max_node() || !numa_bitmask_isbitset(numa_all_nodes_ptr, cpu_node)) {
        fprintf(stderr, "ERROR: CPU NUMA node %d is not available on this machine.\n", cpu_node);
        return -1;
    }

    if (mem_node > numa_max_node() || !numa_bitmask_isbitset(numa_all_nodes_ptr, mem_node)) {
        fprintf(stderr, "ERROR: Memory NUMA node %d is not available on this machine.\n", mem_node);
        return -1;
    }

    benchmark_mem_node = mem_node;

    int prog_argc = 0;
    char **prog_argv = NULL;

    prog_argv = &argv[0];
    prog_argc = argc;

    optind = 1;

    for (int i = 0; i < argc; i++) {
        if (strcmp("--", argv[i]) == 0) {
            // overwrite -- with executable name
            argv[i] = argv[0];
            prog_argv = &argv[i];
            prog_argc = argc - i;
            break;
        }
    }

    /* start with output */
    fprintf(opt_file_out, "<benchmark exec=\"%s\">\n", argv[0]);

    fprintf(opt_file_out, "<config>\n");
#ifdef _OPENMP
    fprintf(opt_file_out, "  <openmp>on</openmp>");
#else
    fprintf(opt_file_out, "  <openmp>off</openmp>");
#endif
    fprintf(opt_file_out, "</config>\n");

    /* setting the CPU and memory bind policy */
    pin_to_numa_node_cpus(cpu_node);

    set_mem_policy_bind_node(mem_node);

    struct sigaction sigact;
    sigset_t block_set;

    sigfillset(&block_set);
    sigdelset(&block_set, SIGUSR1);

    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigact.sa_handler = signalhandler;
    sigaction(SIGUSR1, &sigact, NULL);

    fprintf(opt_file_out, "<run>\n");
    real_main(prog_argc, prog_argv);
    fprintf(opt_file_out, "</run>\n");

    gettimeofday(&tend, NULL);
    printf("Total time: %zu.%03zu\n", tend.tv_sec - tstart.tv_sec,
           (tend.tv_usec - tstart.tv_usec) / 1000);

    fprintf(opt_file_out, "</benchmark>\n");
    return 0;
}
