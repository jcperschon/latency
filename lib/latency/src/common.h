/*
 * Copyright 2017 HoneycombData
 */
#ifndef LATENCY_COMMON_H_
#define LATENCY_COMMON_H_
#include <sys/syscall.h>

#define LATENCY_CONNECTIONS_MAX 1024
#define LATENCY_MAX_EVENTS 256

static inline int getcpu(unsigned *cpu, unsigned *node) {
  *cpu = 0;
  *node = 0;
  #ifdef SYS_getcpu
  return syscall(SYS_getcpu, cpu, node, NULL);
  #else
  return -1; // unavailable
  #endif
}

struct send_responses_param {
  struct rte_ring *ring;
  ssize_t size;
  int run;
};

struct command_descriptor {
  struct timespec timers[4] __rte_cache_aligned;
  ssize_t remaining;
  ssize_t size;
  char *data;
  int fd;
  int valid;
};

static inline uint64_t ns_diff(struct timespec start, struct timespec end) {
  return (end.tv_sec - start.tv_sec) * 1000000000 + end.tv_nsec - start.tv_nsec;
}

static inline int setfdnonblocking(int fd) {
  int flags, s;
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    perror("Unable to read file descriptor flags");
    return -1;
  }
  flags |= O_NONBLOCK;
  if (fcntl (fd, F_SETFL, flags) < 0) {
    perror("Unable to set file descriptor flags");
    return -1;
  }
  return 0;
}

static inline int process_eq(void **responses, struct rte_ring *eq) {
  struct command_descriptor *cd;
  ssize_t sent;
  unsigned dequeued = rte_ring_dequeue_burst(eq, responses,
      LATENCY_CONNECTIONS_MAX - 1, NULL);
  for (unsigned response = 0; response < dequeued; response++) {
    cd = (struct command_descriptor *)responses[response];
    sent = send(cd->fd, &(cd->data[cd->size - cd->remaining]),
        cd->remaining, 0);
    if (sent < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        rte_ring_enqueue(eq, (void *)cd);
      } else {
        return -1;
      }
    } else if (sent < cd->remaining) {
      cd->remaining -= sent;
      rte_ring_enqueue(eq, (void *)cd);
    } else {
      clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cd->timers[3]);
      cd->remaining = cd->size;
      cd->valid++;
    }
  }
}

static inline int prepare_socket(int socket, int server) {
  int opt = 1;
  struct timeval tv = {
    .tv_sec = 5,
    .tv_usec = 0,
  };
  if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
    perror("Unable to set socket option TCP_NODELAY");
    return -1;
  }
  if (server) {
    if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("Unable to set socket option SO_REUSEADDR");
    return -1;
    }
  }
  if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (char *)(&tv),
    sizeof(struct timeval)) < 0) {
    perror("Unable to set socket option SO_RCVTIMEO");
    return -1;
  }
  return 0;
}

static inline int receive_data(struct command_descriptor *cd) {
  if (cd->remaining == cd->size) {
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cd->timers[0]);
  }
  ssize_t rx = recv(cd->fd, &(cd->data[cd->size - cd->remaining]),
      cd->remaining, 0);
  if (rx < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    } else {
      perror("Unable to read file descriptor");
      return -1;
    }
  }
  if (rx - cd->remaining == 0) {
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cd->timers[1]);
    cd->remaining = cd->size;
    return 1;
  } else {
    cd->remaining -= rx;
    return 0;
  }
}

static inline struct rte_ring *allocate_command_ring(ssize_t count,
    ssize_t size) {
  struct command_descriptor *cds = NULL;
  char *data = NULL;
  if (posix_memalign((void **)&cds, 64,
        (count - 1) * sizeof(struct command_descriptor)) < 0) {
    perror("Unable to allocate command descriptor memory");
    return NULL;
  }
  ssize_t aligned_size = size - size % 64 + 64;
  if (posix_memalign((void **)&data, 64, aligned_size * (count - 1)) < 0) {
    perror("Unable to allocate data memory");
  }
  memset(cds, 0, (count - 1) * sizeof(struct command_descriptor));
  for (int i = 0; i < count - 1; i++) {
    cds[i].remaining = size;
    cds[i].size = size;
    cds[i].data = &data[i * aligned_size];
  }
  struct rte_ring *ring = rte_ring_create("cmd_ring", count,
      RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (ring == NULL) {
    perror("Unable to allocate command descriptor ring");
    return NULL;
  }
  for (int i = 0; i < count - 1; i++) {
    do {
    } while (unlikely(rte_ring_enqueue(ring, (void *)(&cds[i])) != 0));
  }
  return ring;
}

#endif // LATENCY_COMMON_H_
