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

struct command_descriptor {
  struct timespec timers[4] __rte_cache_aligned;
  ssize_t transfer_remaining;
  ssize_t transfer_size;
  ssize_t transfer_sequence;
  char *data;
  int fd;
  int first_recv_after_send;
};

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
    sent = send(cd->fd, &(cd->data[cd->transfer_size - cd->transfer_remaining]),
        cd->transfer_remaining, 0);
    if (sent < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        rte_ring_enqueue(eq, (void *)cd);
      } else {
        return -1;
      }
    } else if (sent < cd->transfer_remaining) {
      cd->transfer_remaining -= sent;
      rte_ring_enqueue(eq, (void *)cd);
    } else {
      clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cd->timers[1]);
      cd->transfer_remaining = cd->transfer_size;
      cd->transfer_sequence++;
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
  ssize_t rx = recv(cd->fd, &(cd->data[cd->transfer_size - cd->transfer_remaining]),
      cd->transfer_remaining, 0);
  if (rx < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    } else {
      perror("Unable to read file descriptor");
      return -1;
    }
  }
  if (rx - cd->transfer_remaining == 0) {
    cd->transfer_remaining = cd->transfer_size;
    return 1;
  } else {
    cd->transfer_remaining -= rx;
    return 0;
  }
}

static inline int allocate_command_ring(ssize_t count, ssize_t transfer_size,
    struct command_descriptor **cds, struct rte_ring **ring, char **data) {
  char *d = NULL;
  struct command_descriptor *c = NULL;
  struct rte_ring *r = NULL;
  if (posix_memalign((void **)&c, 64,
        (count - 1) * sizeof(struct command_descriptor)) < 0) {
    perror("Unable to allocate command descriptor memory");
    return -1;
  }
  ssize_t aligned_size = transfer_size - transfer_size % 64 + 64;
  if (posix_memalign((void **)&d, 64, aligned_size * (count - 1)) < 0) {
    perror("Unable to allocate data memory");
    return -1;
  }
  memset(d, 0, aligned_size * (count - 1));
  memset(c, 0, (count - 1) * sizeof(struct command_descriptor));
  for (int i = 0; i < count - 1; i++) {
    c[i].transfer_remaining = transfer_size;
    c[i].transfer_size = transfer_size;
    c[i].data = &d[i * aligned_size];
    c[i].first_recv_after_send = 1;
  }
  r = rte_ring_create("cmd_ring", count,
      RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (r == NULL) {
    perror("Unable to allocate command descriptor ring");
    return -1;
  }
  for (int i = 0; i < count - 1; i++) {
    do {
    } while (unlikely(rte_ring_enqueue(r, (void *)(&c[i])) != 0));
  }
  *data = d;
  *cds = c;
  *ring = r;
  return 0;
}

#endif // LATENCY_COMMON_H_
