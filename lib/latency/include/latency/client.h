/*
 * Copyright 2017 HoneycombData
 */
#ifndef LATENCY_CLIENT_H_
#define LATENCY_CLIENT_H_

#include <pthread.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct client_args {
  const char *ip;
  ssize_t transfer_size;
  const int port;
  int count;
  int samples;
  int rcpu;
  int wcpu;
};

void run_client(pthread_t *thread, struct client_args *p);

#ifdef __cplusplus
}
#endif

#endif // LATENCY_CLIENT_H_
