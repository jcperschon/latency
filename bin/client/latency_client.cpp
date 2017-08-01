/*
 * Copyright 2017 HoneycombData
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <latency/client.h>

static struct option long_options[] = {
  {"help", no_argument, NULL, 'h'},
  {"ip", required_argument, NULL, 'i'},
  {"port", required_argument, NULL, 'p'},
  {"connections", required_argument, NULL, 'c'},
  {"samples", required_argument, NULL, 'n'},
  {"size", required_argument, NULL, 's'},
  {0, 0, 0, 0},
};

static char help[][80] = {
  "Display this help message",
  "IP address of remote latency service (default 127.0.0.1)",
  "TCP port used by remote latency service (default 1234)",
  "Number of connections to run in parallel (default 1)",
  "Number of samples to collect (default 1)",
  "Round trip payload size (default 1)",
};

void usage() {
  int count = sizeof(long_options)/sizeof(struct option) - 1;
  char opt[31];
  struct option *p;
  std::cout << "latency_client - Benchmark network performance using epoll.\n\n";
  std::cout << "usage: latency_client\n";
  for (int i = 0; i < count; i++) {
    p = &long_options[i];
    if (p->has_arg == no_argument) {
      std::cout << " -" << static_cast<char>(p->val);
    } else {
      std::cout << " [-" << static_cast<char>(p->val) << " " << p->name << "]";
    }
    if (i != count - 1) {
      std::cout << " |";
    }
  }
  std::cout << "\n\n";
  for (int i = 0; i < count; i++) {
    p = &long_options[i];
    int index = snprintf(opt, sizeof(opt), "  -%c, --%s",
        static_cast<char>(p->val), p->name);
    if (index > 0) {
      memset(opt + index, ' ', sizeof(opt) - index);
    }
    opt[sizeof(opt) - 1] = 0;
    std::cout << opt << help[i] << "\n";
  }
  std::cout << "\n";
}

int main(int argc, char **argv) {
  int c = 0;
  std::string ip = "127.0.0.1";
  int port = 1234;
  int connections = 1;
  int samples = 1;
  int size = 1;
  while (1) {
    int option_index = 0;
    c = getopt_long(argc, argv, "hi:p:c:n:s:",
        long_options, &option_index);
    if (c == -1) {
      break;
    }
    switch (c) {
      case 'h':
        usage();
        return 0;
      case 'i':
        ip = std::string(optarg);
        break;
      case 'p':
        port = std::stoi(optarg);
        break;
      case 'c':
        connections = std::stoi(optarg);
        break;
      case 'n':
        samples = std::stoi(optarg);
        break;
      case 's':
        size = std::stoi(optarg);
        break;
      default:
        usage();
        return -1;
    }
  }
  return benchmark(ip.c_str(), port, connections, samples, size);
}
