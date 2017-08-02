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
#include "common.h"

int create_clients(const char *host, const int port, const int count,
    ssize_t size, int eserver, struct rte_ring *cds, struct rte_ring *worker) {
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_aton(host, &address.sin_addr);
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
    do {
    } while (unlikely(rte_ring_dequeue(cds, (void **)&cd) != 0));
    cd->fd = client;
    cd->remaining = size;
    ev.data.ptr = (void *)cd;
    if (epoll_ctl(eserver, EPOLL_CTL_ADD, client, &ev) < 0) {
      perror("Unable to add client to epoll file descriptor");
      return -1;
    }
    do {
    } while (unlikely(rte_ring_enqueue(worker, (void *)cd) != 0));
  }
  return 0;
}

int benchmark(const char *host, const int port, const int count,
    int samples, ssize_t size) {
  int collected = -count;
  int eserver = epoll_create1(0);
  if (eserver < 0) {
    perror("Unable to create epoll file descriptor");
    return -1;
  }
  struct rte_ring *ring = allocate_command_ring(LATENCY_CONNECTIONS_MAX, size);
  if (ring == NULL) {
    return -1;
  }
  struct send_responses_param param = {
    .ring = rte_ring_create("tx_queue", 1024, RING_F_SP_ENQ | RING_F_SC_DEQ),
    .size = size,
    .run = 1,
  };
  if (param.ring == NULL) {
    perror("Unable to create ring");
    return -1;
  }
  if (create_clients(host, port, count, size, eserver, ring, param.ring) < 0) {
    return -1;
  }
  uint64_t *results = malloc(sizeof(uint64_t) * samples);
  if (results == NULL) {
    perror("Unable to buffer results");
    return -1;
  }
  pthread_t worker;
  pthread_create(&worker, NULL, send_responses, (void *)&param);
  struct command_descriptor *cd = NULL;
  struct epoll_event *events = malloc(sizeof(struct epoll_event) *
      LATENCY_MAX_EVENTS);
  do {
    int n, i, ret;
    struct timespec now;
    n = epoll_wait(eserver, events, LATENCY_MAX_EVENTS, 0);
    if (n < 0) {
      perror("Epoll wait error");
      return -1;
    }
    for (i = 0; i < n; i++) {
      cd = (struct command_descriptor *)events[i].data.ptr;
      if ((events[i].events & EPOLLERR) ||
          (events[i].events & EPOLLHUP) ||
          (!(events[i].events & EPOLLIN))) {
        fprintf(stderr,"epoll error\n");
        if (close(cd->fd) < 0) {
          perror("Unable to close file descriptor");
          return -1;
        }
        if (epoll_ctl(eserver, EPOLL_CTL_DEL, cd->fd, events) < 0) {
          perror("Unable to remove epoll file descriptor");
          return -1;
        }
        free(cd);
        continue;
      }
      ret = receive_data(cd, size);
      if (ret < 0) {
        return -1;
      } else if (ret) {
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
        if (collected >= 0) {
          results[collected] = ns_diff(cd->timer, now);
        }
        cd->timer = now;
        collected++;
        do {
        } while (unlikely(rte_ring_enqueue(param.ring, (void *)cd) != 0));
      }
    }
  } while (collected < samples);
  printf("rtt (us)\n");
  for (int i = 0; i < samples; i++) {
    printf("%lu\n", results[i]/1000);
  }
  return 0;
}
