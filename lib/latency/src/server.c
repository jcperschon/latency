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
#include "latency/server.h"
#include "latency/scheduler.h"
#include "common.h"

#define LATENCY_BACKLOG_SIZE 32

static inline int accept_and_add_to_epoll_fd(int server, int eserver,
    ssize_t size, struct rte_ring *ring) {
  struct sockaddr_in client_addr;
  socklen_t len = sizeof(client_addr);
  int client = accept(server, (struct sockaddr *)&client_addr, &len);
  if (client < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 1;
    } else {
      perror("Unable to accept client");
      return -1;
    }
  }
  if (prepare_socket(client, 0) < 0) {
    return -1;
  }
  if (setfdnonblocking(client) < 0) {
    return -1;
  }
  struct command_descriptor *cd = NULL;
  if (unlikely(rte_ring_dequeue(ring, (void **)&cd) != 0)) {
    perror("No more command descriptors");
    return -1;
  }
  cd->fd = client;
  cd->remaining = size;
  struct epoll_event ev = {
    .data.ptr = (void *)cd,
    .events = EPOLLIN | EPOLLRDHUP | EPOLLPRI,
  };
  if (epoll_ctl(eserver, EPOLL_CTL_ADD, client, &ev) < 0) {
    perror("Unable to add connection to epoll file descriptor");
    return -1;
  }
  return 0;
}

static inline int create_server(const char *ip, const int port) {
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_aton(ip, &address.sin_addr);
  int server = socket(AF_INET, SOCK_STREAM, 0);
  if (prepare_socket(server, 1) < 0) {
    return -1;
  }
  if (setfdnonblocking(server) < 0) {
    return -1;
  }
  if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("Unable to bind to address");
    return -1;
  }
  if (listen(server, LATENCY_BACKLOG_SIZE) < 0) {
    perror("Unable to listen for new connections");
    return -1;
  }
  return server;
}

static int serve(const char *ip, const int port, ssize_t size, int wcpu) {
  unsigned cpu, node;
  if (getcpu(&cpu, &node) < 0) {
    perror("Unable to determine cpu/node");
    return -1;
  }
  printf("Running server reader on cpu %u node %u \n", cpu, node);
  int server = create_server(ip, port);
  if (server < 0) {
      return -1;
  }
  int eserver = epoll_create1(0);
  if (eserver < 0) {
    perror("Unable to create epoll file descriptor");
    return -1;
  }
  struct rte_ring *ring = allocate_command_ring(LATENCY_CONNECTIONS_MAX, size);
  if (ring == NULL) {
    return -1;
  }
  struct command_descriptor *cd = NULL;
  do {
  } while (unlikely(rte_ring_dequeue(ring, (void **)&cd) != 0));
  cd->fd = server;
  struct epoll_event ev = {
    .data.ptr = (void *)cd,
    .events = EPOLLIN | EPOLLRDHUP | EPOLLPRI,
  };
  if (epoll_ctl(eserver, EPOLL_CTL_ADD, server, &ev) < 0) {
    perror("Unable to add server to epoll file descriptor");
    return -1;
  }
  struct epoll_event *events = malloc(sizeof(struct epoll_event) * LATENCY_MAX_EVENTS);
  if (events == NULL) {
    perror("Unable to allocate memory for events");
    return -1;
  }
  struct send_responses_param prm = {
    .ring = rte_ring_create("tx_queue", 1024, RING_F_SP_ENQ | RING_F_SC_DEQ),
    .size = size,
    .run = 1,
  };
  if (prm.ring == NULL) {
    perror("Unable to create ring");
    return -1;
  }
  pthread_t worker;
  pthread_attr_t attr;
  if (set_thread_priority_max(&attr, wcpu)) {
    perror("Unable to configure server writer");
    return -1;
  }
  if (pthread_create(&worker, &attr, send_responses, (void *)&prm) < 0) {
    perror("Unable to create server writer");
    return -1;
  }
  int n, i, c, ret;
  void *completed[16];
  while (1) {
    c = 0;
    n = epoll_wait(eserver, events, LATENCY_MAX_EVENTS, -1);
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
        if (epoll_ctl(eserver, EPOLL_CTL_DEL, cd->fd, &ev) < 0) {
          perror("Unable to remove epoll file descriptor");
          return -1;
        }
        rte_ring_enqueue(ring, (void *)cd);
        continue;
      }
      if (cd->fd == server) {
        while (1) {
          ret = accept_and_add_to_epoll_fd(server, eserver, size, ring);
          if (ret < 0) {
            return -1;
          } else if (ret) {
            break;
          }
        }
        continue;
      } else {
        ret = receive_data(cd, size);
        if (ret < 0) {
          return -1;
        } else if (ret) {
          completed[c++] = (void *)cd;
          if (c == 16 || (i == n - 1 && c > 0)) {
            rte_ring_enqueue_bulk(prm.ring, &completed[0], c, NULL);
            c = 0;
          }
        }
      }
    }
  }
}

static void *do_server(void *param) {
  struct server_args *p = (struct server_args *)param;
  uint64_t r = serve(p->ip, p->port, p->size, p->wcpu);
  pthread_exit((void *)r);
}

void run_server(pthread_t *thread, struct server_args *p) {
  pthread_attr_t attr;
  if (set_thread_priority_max(&attr, p->rcpu) < 0) {
    perror("Unable to configure server");
    abort();
  }
  if (pthread_create(thread, &attr, do_server, (void *)p) < 0) {
    perror("Unable to start server");
    abort();
  }
}
