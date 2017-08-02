/*
 * Copyright 2017 HoneycombData
 */
#ifndef LATENCY_SERVER_H_
#define LATENCY_SERVER_H_

#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

int serve(const char *host, const int port, ssize_t size);

#ifdef __cplusplus
}
#endif

#endif // LATENCY_SERVER_H_
