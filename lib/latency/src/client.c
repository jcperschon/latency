#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <rte/rte_ring.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "latency/client.h"
#include "latency/scheduler.h"
#include "common.h"

#define NANOSECONDS_PER_SECOND 1000000000

static inline uint64_t time_delta_in_ns(struct timespec start,
    struct timespec end) {
  return (end.tv_sec - start.tv_sec) * NANOSECONDS_PER_SECOND +
      end.tv_nsec - start.tv_nsec;
}

static int create_clients(const char *ip, const int port, const int count,
    int epoll_fd, struct rte_ring *cds, struct rte_ring *eq) {
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_aton(ip, &address.sin_addr);
  struct command_descriptor *cd = NULL;
  struct epoll_event ev = {
    .events = EPOLLIN | EPOLLRDHUP | EPOLLPRI,
  };
  for (int i = 0; i < count; i++) {
    int client = socket(AF_INET, SOCK_STREAM, 0);
    if (prepare_socket(client, 0) < 0) {
      return -1;
    }
    if (connect(client, (struct sockaddr *)&address, sizeof(address)) < 0) {
      perror("Unable to connect to server");
      return -1;
    }
    if (setfdnonblocking(client) < 0) {
      return -1;
    }
    rte_ring_dequeue(cds, (void **)&cd);
    cd->fd = client;
    ev.data.ptr = (void *)cd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &ev) < 0) {
      perror("Unable to add client to epoll file descriptor");
      return -1;
    }
    do {
    } while (unlikely(rte_ring_enqueue(eq, (void *)cd) != 0));
  }
  return 0;
}

static int shutdown_connections(const int epoll_fd, const int count,
    struct epoll_event *events, void **responses,
    struct rte_ring *eq) {
  int i, n, closed = 0;
  struct command_descriptor *cd = NULL;
  while (rte_ring_count(eq) > 0) {
    process_eq(responses, eq);
  };
  do {
    n = epoll_wait(epoll_fd, events, LATENCY_MAX_EVENTS, -1);
    if (n < 0) {
      perror("Epoll wait error");
    }
    for (i = 0; i < n; i++) {
      cd = (struct command_descriptor *)events[i].data.ptr;
      if ((events[i].events & EPOLLERR) ||
          (events[i].events & EPOLLHUP) ||
          (!(events[i].events & EPOLLIN))) {
        perror("Connection error");
        return -1;
      } else {
        if (shutdown(cd->fd, SHUT_RDWR) < 0) {
          perror("Could not shutdown connection");
          return -1;
        }
        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, cd->fd, events) < 0) {
          perror("Unable to remove epoll file descriptor");
          return -1;
        }
        closed++;
      }
    }
  } while (closed < count);
  return 0;
}

static inline void record_sample(uint64_t *records, int sample,
    struct command_descriptor *cd) {
  records[sample * 3 + 0] = time_delta_in_ns(cd->timers[0], cd->timers[1]);
  records[sample * 3 + 1] = time_delta_in_ns(cd->timers[1], cd->timers[2]);
  records[sample * 3 + 2] = time_delta_in_ns(cd->timers[2], cd->timers[3]);
}

static int client_loop(const int samples, const int epoll_fd,
    struct epoll_event *events, void **responses, uint64_t *records,
    struct rte_ring *eq) {
  int sample = 0;
  struct command_descriptor *cd = NULL;
  struct timespec now;
  ssize_t sent;
  int n, i, ret;
  do {
    process_eq(responses, eq);
    if (rte_ring_count(eq) == 0) {
      n = epoll_wait(epoll_fd, events, LATENCY_MAX_EVENTS, -1);
    } else {
      n = epoll_wait(epoll_fd, events, LATENCY_MAX_EVENTS, 0);
    }
    if (n < 0) {
      perror("Epoll wait error");
      return -1;
    }
    for (i = 0; i < n; i++) {
      cd = (struct command_descriptor *)events[i].data.ptr;
      if ((events[i].events & EPOLLERR) ||
          (events[i].events & EPOLLHUP) ||
          (!(events[i].events & EPOLLIN))) {
        perror("connection error");
        return -1;
      } else {
        if (cd->first_recv_after_send) {
          clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cd->timers[2]);
          cd->first_recv_after_send = 0;
        }
        ret = receive_data(cd);
        if (ret < 0) {
          return -1;
        } else if (ret) {
          cd->first_recv_after_send = 1;
          clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cd->timers[3]);
          if (sample < samples) {
            record_sample(records, sample, cd);
            sample++;
          }
          clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cd->timers[0]);
          sent = send(cd->fd, cd->data, cd->transfer_remaining, 0);
          if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
              rte_ring_enqueue(eq, (void *)cd);
            } else {
              perror("Unable to send data");
              return -1;
            }
          } else if (sent < cd->transfer_remaining) {
            cd->transfer_remaining -= sent;
            rte_ring_enqueue(eq, (void *)cd);
          } else {
            clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cd->timers[1]);
            cd->transfer_sequence++;
          }
        }
      }
    }
  } while (sample < samples);
  fprintf(stdout, "d0 (us), d1 (us), d2 (us)\n");
  for (i = 0; i < samples; i++) {
    fprintf(stdout, "%lu,%lu,%lu\n",
        records[3 * i + 0]/1000,
        records[3 * i + 1]/1000,
        records[3 * i + 2]/1000);
  }
  return 0;
}

static int benchmark(const char *ip, const int port, const int count,
    int samples, ssize_t transfer_size, int wcpu) {
  unsigned cpu, node;
  if (getcpu(&cpu, &node) < 0) {
    perror("Unable to determine cpu/node");
    return -1;
  }
  fprintf(stdout, "Running client on cpu %u node %u \n", cpu, node);
  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    perror("Unable to create epoll file descriptor");
    return -1;
  }
  char *data = NULL;
  struct command_descriptor *cds = NULL;
  struct rte_ring *ring = NULL;
  if (allocate_command_ring(LATENCY_CONNECTIONS_MAX, transfer_size, &cds, &ring, &data) < 0) {
    return -1;
  }
  struct rte_ring *eq = rte_ring_create("cmd_ring", LATENCY_CONNECTIONS_MAX,
      RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (eq  == NULL) {
    return -1;
  }
  if (create_clients(ip, port, count, epoll_fd, ring, eq) < 0) {
    return -1;
  }
  uint64_t *records = (uint64_t *)malloc(sizeof(uint64_t) * samples * 3);
  if (records == NULL) {
    perror("Unable to buffer records");
    return -1;
  }
  void **responses = (void **)malloc(sizeof(void *) *
      (LATENCY_CONNECTIONS_MAX - 1));
  if (responses == NULL) {
    perror("Unable to create response area");
    return -1;
  }
  struct epoll_event *events = malloc(sizeof(struct epoll_event) *
      LATENCY_MAX_EVENTS);
  if (events == NULL) {
    perror("Unable to create events area");
    return -1;
  }
  if (client_loop(samples, epoll_fd, events, responses, records, eq) < 0) {
    return -1;
  }
  if (shutdown_connections(epoll_fd, count, events, responses, eq) < 0) {
    return -1;
  }
  free(events);
  free(responses);
  free(records);
  free(eq);
  free(ring);
  free(data);
  free(cds);
  return 0;
}

static void *do_client(void *param) {
  struct client_args *p = (struct client_args *)param;
  uint64_t r = benchmark(p->ip, p->port, p->count, p->samples,
      p->transfer_size, p->wcpu);
  pthread_exit((void *)r);
}

void run_client(pthread_t *thread, struct client_args *p) {
  if (p->transfer_size % 8 > 0 || p->transfer_size < 8) {
    perror("Invalid size setting");
    abort();
  }
  pthread_attr_t attr;
  if (set_thread_priority_max(&attr, p->rcpu) < 0) {
    perror("Unable to configure client");
    abort();
  }
  if (pthread_create(thread, &attr, do_client, (void *)p) < 0) {
    perror("Unable to start client");
    abort();
  }
  pthread_attr_destroy(&attr);
}
