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

static inline int receive_data(struct command_descriptor *cd, ssize_t size) {
  ssize_t remaining = cd->remaining;
  if (remaining == size) {
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cd->timers[0]);
  }
  ssize_t rx = recv(cd->fd, &(cd->data[size - remaining]), remaining, 0);
  if (rx < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    } else {
      perror("Unable to read file descriptor");
      return -1;
    }
  }
  if (rx - remaining == 0) {
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cd->timers[1]);
    cd->remaining = size;
    return 1;
  } else {
    cd->remaining -= rx;
    return 0;
  }
}

static void *send_responses(void *param) {
  unsigned cpu, node;
  if (getcpu(&cpu, &node) < 0) {
    perror("Unable to determine cpu/node");
    abort();
  }
  printf("Running writer on cpu %u node %u\n", cpu, node);
  struct send_responses_param *p = (struct send_responses_param *)param;
  struct rte_ring *ring = p->ring;
  ssize_t size = p->size;
  struct rte_ring *eq = rte_ring_create("tx_queue", 256,
      RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (eq == NULL) {
    perror("Unable to create eq");
    abort();
  }
  struct command_descriptor *cd = NULL;
  void *responses[256];
  memset(responses, 0, sizeof(responses));
  unsigned dequeued;
  unsigned response;
  uint8_t eq_index = 0;
  int sent;
  while (1) {
    dequeued = rte_ring_dequeue_burst(eq, &responses[0], 256, NULL);
    for (response = 0; response < dequeued; response++) {
      cd = (struct command_descriptor *)(responses[response]);
      sent = send(cd->fd, &(cd->data[size - cd->remaining]),
          cd->remaining, 0);
      if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          rte_ring_enqueue(eq, responses[response]);
        } else {
          perror("Unable to send data");
          abort();
        }
      } else if (sent < size) {
        cd->remaining -= sent;
        rte_ring_enqueue(eq, responses[response]);
      } else {
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cd->timers[3]);
        cd->valid = 1;
        cd->remaining = size;
      }
    }
    dequeued = rte_ring_dequeue_burst(ring, &responses[0],
        rte_ring_free_count(eq), NULL);
    for (response = 0; response < dequeued; response++) {
      cd = (struct command_descriptor *)(responses[response]);
      clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cd->timers[2]);
      sent = send(cd->fd, cd->data, size, 0);
      if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          rte_ring_enqueue(eq, (void *)cd);
        } else {
          perror("Unable to send data");
          abort();
        }
      } else if (sent < size) {
        cd->remaining = size - sent;
        rte_ring_enqueue(eq, (void *)cd);
      } else{
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cd->timers[3]);
        cd->valid = 1;
      }
    }
    if ((dequeued == 0) && (rte_ring_count(eq) == 0)) {
      _mm_pause();
    }
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
