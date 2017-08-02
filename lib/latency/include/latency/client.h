/*
 * Copyright 2017 HoneycombData
 */
#ifndef LATENCY_CLIENT_H_
#define LATENCY_CLIENT_H_

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

int benchmark(const char *host, const int port, const int count, int samples, ssize_t size);

#ifdef __cplusplus
}
#endif

#endif // LATENCY_CLIENT_H_
