/*
 * Copyright (c) 2017, The Linux Foundation. All rights reserved.
 *
 * SPDX-License-Identifier:    BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE
#include <sched.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>

#include "lockhammer.h"

#include ATOMIC_TEST

uint64_t test_lock = 0;
uint64_t sync_lock = 0;
uint64_t ready_lock = 0;

void* hmr(void *);

void print_usage (char *invoc) {
    fprintf(stderr,
            "Usage: %s\n\t[-t threads]\n\t[-a acquires per thread]\n\t"
            "[-c critical iterations]\n\t[-p parallelizable iterations]\n\t"
            "[-- <test specific arguments>]\n", invoc);
}

int main(int argc, char** argv)
{
    struct sched_param sparam;

    unsigned long i;
    unsigned long num_cores;
    unsigned long result;
    unsigned long sched_elapsed = 0, real_elapsed = 0;
    unsigned long start_ns = 0;
    double avg_lock_depth = 0.0;

    num_cores = sysconf(_SC_NPROCESSORS_ONLN);

    /* Set defaults for all command line options */
    test_args args = { .nthrds = num_cores,
                       .nacqrs = 50000,
                       .ncrit = 0,
                       .nparallel = 0 };

    opterr = 0;
    while ((i = getopt(argc, argv, "t:a:c:p:")) != -1)
    {
        long optval = 0;
        switch (i) {
          case 't':
            optval = strtol(optarg, (char **) NULL, 10);
            /* Do not allow number of threads to exceed online cores
               in order to prevent deadlock ... */
            if (optval < 0) {
                fprintf(stderr, "ERROR: thread count must be positive.\n");
                return 1;
            }
            else if (optval <= num_cores) {
                args.nthrds = optval;
            }
            else {
                fprintf(stderr, "WARNING: limiting thread count to online cores (%d).\n", num_cores);
            }
            break;
          case 'a':
            optval = strtol(optarg, (char **) NULL, 10);
            if (optval < 0) {
                fprintf(stderr, "ERROR: acquire count must be positive.\n");
                return 1;
            }
            else {
                args.nacqrs = optval;
            }
            break;
          case 'c':
            optval = strtol(optarg, (char **) NULL, 10);
            if (optval < 0) {
                fprintf(stderr, "ERROR: critical iteration count must be positive.\n");
                return 1;
            }
            else {
                args.ncrit = optval;
            }
            break;
          case 'p':
            optval = strtol(optarg, (char **) NULL, 10);
            if (optval < 0) {
                fprintf(stderr, "ERROR: parallel iteration count must be positive.\n");
                return 1;
            }
            else {
                args.nparallel = optval;
            }
            break;
          case '?':
          default:
            print_usage(argv[0]);
            return 1;
        }
    }

    parse_test_args(args, argc - optind, &argv[optind]);

    pthread_t hmr_threads[args.nthrds];
    pthread_attr_t hmr_attr;
    unsigned long hmrs[args.nthrds];
    unsigned long hmrtime[args.nthrds]; /* can't touch this */
    unsigned long hmrdepth[args.nthrds];
    struct timespec tv_time;

    /* Hack: Run one thread less, this is done to move TH0 to Core1,
     * allowing core 0 to only run main function.
     */
    args.nthrds--;

    /* Select the FIFO scheduler.  This prevents interruption of the
       lockhammer test threads allowing for more precise measuremnet of
       lock acquisition rate, especially for mutex type locks where
       a lock-holding or queued thread might significantly delay forward
       progress if it is rescheduled.  Additionally the FIFO scheduler allows
       for a better guarantee of the requested contention level by ensuring
       that a fixed number of threads are executing simultaneously for
       the duration of the test.  This comes at the significant cost of
       reduced responsiveness of the system under test and the possibility
       for system instability if the FIFO scheduled threads remain runnable
       for too long, starving other processes.  Care should be taken in
       invocation to ensure that a given instance of lockhammer runs for
       no more than a few milliseconds and lockhammer should never be run
       on an already-deplayed system. */

    pthread_attr_init(&hmr_attr);
    pthread_attr_setinheritsched(&hmr_attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&hmr_attr, SCHED_FIFO);
    sparam.sched_priority = 1;
    pthread_attr_setschedparam(&hmr_attr, &sparam);

    initialize_lock(&test_lock, num_cores);

    thread_args t_args[args.nthrds];
    for (i = 0; i < args.nthrds; ++i) {
        hmrs[i] = 0;
        t_args[i].ncores = num_cores;
        t_args[i].nthrds = args.nthrds;
        t_args[i].iter = args.nacqrs;
        t_args[i].lock = &test_lock;
        t_args[i].rst = &hmrs[i];
        t_args[i].nsec = &hmrtime[i];
        t_args[i].depth = &hmrdepth[i];
        t_args[i].nstart = &start_ns;
        t_args[i].hold = args.ncrit;
        t_args[i].post = args.nparallel;
        fprintf(stderr, "Th#%d \r\n", i);
        pthread_create(&hmr_threads[i], &hmr_attr, hmr, (void*)(&t_args[i]));
    }

    for (i = 0; i < args.nthrds; ++i) {
        result = pthread_join(hmr_threads[i], NULL);
    }
    /* "Marshal" thread will collect start time once all threads have
        reported ready so we only need to collect the end time here */
    clock_gettime(CLOCK_MONOTONIC, &tv_time);
    real_elapsed = (1000000000ul * tv_time.tv_sec + tv_time.tv_nsec) - start_ns;

    pthread_attr_destroy(&hmr_attr);

    result = 0;
    for (i = 0; i < args.nthrds; ++i) {
        result += hmrs[i];
        sched_elapsed += hmrtime[i];
        /* Average lock "depth" is an algorithm-specific auxiliary metric
           whereby each algorithm can report an approximation of the level
           of contention it observes.  This estimate is returned from each
           call to lock_acquire and accumulated per-thread.  These results
           are then aggregated and averaged here so that an overall view
           of the run's contention level can be determined. */
        avg_lock_depth += ((double) hmrdepth[i] / (double) hmrs[i]) / (double) args.nthrds;
    }

    fprintf(stderr, "%ld lock loops\n", result);
    fprintf(stderr, "%ld ns scheduled\n", sched_elapsed);
    fprintf(stderr, "%ld ns elapsed (~%f cores)\n", real_elapsed, ((float) sched_elapsed / (float) real_elapsed));
    fprintf(stderr, "%lf ns per access\n", ((double) sched_elapsed)/ ((double) result));
    fprintf(stderr, "%lf ns access rate\n", ((double) real_elapsed) / ((double) result));
    fprintf(stderr, "%lf average depth\n", avg_lock_depth);

    printf("%ld, %f, %lf, %lf, %lf\n",
           args.nthrds,
           ((float) sched_elapsed / (float) real_elapsed),
           ((double) sched_elapsed)/ ((double) result),
           ((double) real_elapsed) / ((double) result),
           avg_lock_depth);
}

void* hmr(void *ptr)
{
    unsigned long nlocks = 0;
    thread_args *x = (thread_args*)ptr;
    int rval;
    unsigned long *lock = x->lock;
    unsigned long target_locks = x->iter;
    unsigned long ncores = x->ncores;
    unsigned long nthrds = x->nthrds;
    unsigned long hold_count = x->hold;
    unsigned long post_count = x->post;

    unsigned long mycore = 0;

    struct timespec tv_monot_start, tv_start, tv_end;
    unsigned long ns_elap;
    unsigned long total_depth = 0;

    cpu_set_t affin_mask;

    CPU_ZERO(&affin_mask);

    /* Coordinate synchronized start of all lock threads to maximize
       time under which locks are stressed to the requested contention
       level */
    mycore = fetchadd64_acquire(&sync_lock, 2) >> 1;

    if (mycore == 0) {
        /* First core to register is a "marshal" who waits for subsequent
           cores to become ready and starts all cores with a write to the
           shared memory location */
        /* Hack set affinity to Core 1 */
        int mcore = mycore + 1;
        CPU_SET(((mcore >> 1)) + ((ncores >> 1) * (mcore & 1)), &affin_mask);

        sched_setaffinity(0, sizeof(cpu_set_t), &affin_mask);

        /* Spin until the appropriate numer of threads have become ready */
        fprintf(stderr, "b:wait \r\n");
        wait64(&ready_lock, nthrds - 1);
        fprintf(stderr, "a:wait \r\n");
        clock_gettime(CLOCK_MONOTONIC, &tv_monot_start);
        fetchadd64_release(&sync_lock, 1);
    }
    else {
        /* Set Affinity to mycore +1 */
        int mcore = mycore + 1;
        /* Calculate affinity mask for my core and set affinity */
        CPU_SET(((mcore >> 1)) + ((ncores >> 1) * (mcore & 1)), &affin_mask);
        sched_setaffinity(0, sizeof(cpu_set_t), &affin_mask);
        fetchadd64_release(&ready_lock, 1);

        /* Spin until the "marshal" sets the appropriate bit */
        wait64(&sync_lock, (nthrds * 2) | 1);
    }

    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tv_start);

    while (!target_locks || nlocks < target_locks) {
        /* Do a lock thing */
        prefetch64(lock);
        total_depth += lock_acquire(lock, mycore);
        spin_wait(hold_count);
        lock_release(lock, mycore);
        spin_wait(post_count);

        nlocks++;
    }
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &tv_end);

    if (mycore == 0)
        *(x->nstart) = (1000000000ul * tv_monot_start.tv_sec + tv_monot_start.tv_nsec);

    ns_elap = (1000000000ul * tv_end.tv_sec + tv_end.tv_nsec) - (1000000000ul * tv_start.tv_sec + tv_start.tv_nsec);

    *(x->rst) = nlocks;
    *(x->nsec) = ns_elap;
    *(x->depth) = total_depth;

    return NULL;
}
