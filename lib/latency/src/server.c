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
    struct rte_ring *ring) {
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

static int server_loop(const int server, const int eserver, struct epoll_event *events,
    void **responses, struct rte_ring *eq, struct rte_ring *ring, uint8_t *run) {
  struct command_descriptor *cd = NULL;
  ssize_t sent;
  struct epoll_event ev = {0};
  int n, i, ret;
  while (*run == 1) {
    process_eq(responses, eq);
    if (rte_ring_count(eq) == 0) {
      n = epoll_wait(eserver, events, LATENCY_MAX_EVENTS, -1);
    } else {
      n = epoll_wait(eserver, events, LATENCY_MAX_EVENTS, 0);
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
        if (epoll_ctl(eserver, EPOLL_CTL_DEL, cd->fd, &ev) < 0) {
          perror("Unable to remove epoll file descriptor");
          return -1;
        }
        if (close(cd->fd) < 0) {
          perror("Unable to close file descriptor");
          return -1;
        }
        rte_ring_enqueue(ring, (void *)cd);
        continue;
      } else {
        if (cd->fd == server) {
          while (1) {
            ret = accept_and_add_to_epoll_fd(server, eserver, ring);
            if (ret < 0) {
              return -1;
            } else if (ret) {
              break;
            }
          }
          continue;
        } else {
          ret = receive_data(cd);
          if (ret < 0) {
            return -1;
          } else if (ret) {
            sent = send(cd->fd, cd->data, cd->transfer_size, 0);
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
            }
          }
        }
      }
    }
  }
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

static int serve(const char *ip, const int port, ssize_t transfer_size, int wcpu, uint8_t *run) {
  unsigned cpu, node;
  if (getcpu(&cpu, &node) < 0) {
    perror("Unable to determine cpu/node");
    return -1;
  }
  fprintf(stdout, "Running server on cpu %u node %u \n", cpu, node);
  int server = create_server(ip, port);
  if (server < 0) {
      return -1;
  }
  int eserver = epoll_create1(0);
  if (eserver < 0) {
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
  void **responses = (void **)malloc(sizeof(void *) *
      (LATENCY_CONNECTIONS_MAX - 1));
  if (responses == NULL) {
    perror("Unable to create response area");
    return -1;
  }
  if (server_loop(server, eserver, events, responses, eq, ring, run) < 0) {
    return -1;
  }
  free(events);
  free(responses);
  free(eq);
  free(ring);
  free(data);
  free(cds);
}

static void *do_server(void *param) {
  struct server_args *p = (struct server_args *)param;
  p->run = 1;
  uint64_t r = serve(p->ip, p->port, p->transfer_size, p->wcpu, &p->run);
  pthread_exit((void *)r);
}

void kill_server(pthread_t *thread, struct server_args *p, void **ret) {
  p->run = 0;
  pthread_join(*thread, ret);
}

void run_server(pthread_t *thread, struct server_args *p) {
  pthread_attr_t attr;
  if (p->transfer_size % 8 != 0 || p->transfer_size < 8) {
    perror("Invalid size setting");
    abort();
  }
  if (set_thread_priority_max(&attr, p->rcpu) < 0) {
    perror("Unable to configure server");
    abort();
  }
  if (pthread_create(thread, &attr, do_server, (void *)p) < 0) {
    perror("Unable to start server");
    abort();
  }
  pthread_attr_destroy(&attr);
}
