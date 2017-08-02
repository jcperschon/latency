/*
 * Copyright 2017 HoneycombData
 */
#ifndef LATENCY_SERVER_H_
#define LATENCY_SERVER_H_

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct server_args {
  const char *ip;
  const int port;
  ssize_t size;
  int rcpu;
  int wcpu;
};

void run_server(pthread_t *thread, struct server_args *p);

#ifdef __cplusplus
}
#endif

#endif // LATENCY_SERVER_H_
