/*
 * Copyright 2017 HoneycombData
 */
#ifndef LATENCY_SCHEDULER_H_
#define LATENCY_SCHEDULER_H_

#include <pthread.h>
#include <sched.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int set_thread_priority_max(pthread_attr_t *attr, int cpuid) {
  struct sched_param max = {
    .sched_priority = sched_get_priority_max(SCHED_FIFO),
  };
  if (pthread_attr_init(attr) < 0) {
    perror("Unable to init pthread attribute");
    return -1;
  }
  if (pthread_attr_setdetachstate(attr,PTHREAD_CREATE_JOINABLE) < 0) {
    perror("Unable to set joinable state");
    return -1;
  }
  if (pthread_attr_setschedpolicy(attr, SCHED_FIFO) < 0) {
    perror("Unable to set scheduling policity");
    return -1;
  }
  if (pthread_attr_setinheritsched(attr, PTHREAD_EXPLICIT_SCHED) < 0) {
    perror("Unable to set explicit secheduling");
    return -1;
  }
  if (pthread_attr_setschedparam(attr, &max) < 0) {
    perror("Unable to set scheduling priority");
    return -1;
  }
  if (pthread_attr_setscope(attr, PTHREAD_SCOPE_SYSTEM) < 0) {
    perror("Unable to set scheduling scope");
    return -1;
  }
  if (cpuid >= 0) {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(cpuid, &cpus);
    if (pthread_attr_setaffinity_np(attr, sizeof(cpu_set_t), &cpus) < 0) {
      perror("Unable to set thread affinity");
      return -1;
    }
  }
  return 0;
}

#ifdef __cplusplus
}
#endif

#endif // LATENCY_SCHEDULER_H_
