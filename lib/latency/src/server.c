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

#define LATENCY_MAX_EVENTS 256
#define LATENCY_BACKLOG_SIZE 1

static char *input;
static char *output;

struct send_responses_param {
  struct rte_ring *ring;
  ssize_t size;
};

struct eq_descriptor {
  ssize_t remaining;
  int fd;
};

static inline int setfdnonblocking(int fd) {
  int flags, s;
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    perror("Unable to read file descriptor flags ");
    return -1;
  }
  flags |= O_NONBLOCK;
  if (fcntl (fd, F_SETFL, flags) < 0) {
    perror("Unable to set file descriptor flags ");
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
    perror("Unable to set socket option TCP_NODELAY ");
    return -1;
  }
  if (server) {
    if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("Unable to set socket option SO_REUSEADDR ");
    return -1;
    }
  }
  if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (char *)(&tv),
    sizeof(struct timeval)) < 0) {
    perror("Unable to set socket option SO_RCVTIMEO ");
    return -1;
  }
  return 0;
}

static inline int accept_and_add_to_epoll_fd(int server, int eserver, ssize_t size) {
  struct sockaddr_in client_addr;
  socklen_t len = sizeof(client_addr);
  int client = accept(server, (struct sockaddr *)&client_addr, &len);
  if (client < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 1;
    } else {
      perror("Unable to accept client ");
      return -1;
    }
  }
  if (prepare_socket(client, 0) < 0) {
    return -1;
  }
  if (setfdnonblocking(client) < 0) {
    return -1;
  }
  struct eq_descriptor *eqd = malloc(sizeof(struct eq_descriptor));
  if (eqd == NULL) {
    perror("Unable to create eq descriptor ");
    return -1;
  }
  eqd->fd = client;
  eqd->remaining = size;
  struct epoll_event ev = {
    .data.ptr = (void *)eqd,
    .events = EPOLLIN | EPOLLRDHUP | EPOLLPRI,
  };
  if (epoll_ctl(eserver, EPOLL_CTL_ADD, client, &ev) < 0) {
    perror("Unable to add connection to epoll file descriptor ");
    return -1;
  }
  return 0;
}

static inline int receive_data(struct eq_descriptor *eqd, ssize_t size) {
  ssize_t remaining = eqd->remaining;
  ssize_t rx = recv(eqd->fd, &input[size - remaining], remaining, 0);
  if (rx < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return 0;
    } else {
      perror("Unable to read file descriptor ");
      return -1;
    }
  }
  if (rx - remaining == 0) {
    eqd->remaining = size;
    return 1;
  } else {
    eqd->remaining -= rx;
    return 0;
  }
}

static void *send_responses(void *param) {
  struct rte_ring *ring = ((struct send_responses_param *)param)->ring;
  ssize_t size = ((struct send_responses_param *)param)->size;
  struct rte_ring *eq = rte_ring_create("tx_queue", 32,
      RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (eq == NULL) {
    perror("Unable to create eq ");
    abort();
  }
  struct eq_descriptor *eqd = NULL;
  void *responses[32];
  memset(responses, 0, sizeof(responses));
  unsigned dequeued;
  unsigned response;
  uint8_t eq_index = 0;
  int fd;
  int sent;
  while (1) {
    dequeued = rte_ring_dequeue_burst(eq, &responses[0], 32, NULL);
    for (response = 0; response < dequeued; response++) {
      eqd = (struct eq_descriptor *)(responses[response]);
      sent = send(eqd->fd, &output[size - eqd->remaining],
          eqd->remaining, 0);
      if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          rte_ring_enqueue(eq, responses[response]);
        } else {
          perror("Unable to send data ");
          abort();
        }
      } else if (sent < size) {
        eqd->remaining -= sent;
        rte_ring_enqueue(eq, responses[response]);
      } else {
        eqd->remaining = size;
      }
    }
    dequeued = rte_ring_dequeue_burst(ring, &responses[0],
        rte_ring_free_count(eq), NULL);
    for (response = 0; response < dequeued; response++) {
      eqd = (struct eq_descriptor *)(responses[response]);
      sent = send(eqd->fd, (void *)output, size, 0);
      if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          rte_ring_enqueue(eq, (void *)eqd);
        } else {
          perror("Unable to send data ");
          abort();
        }
      } else if (sent < size) {
        eqd->remaining = size - sent;
        rte_ring_enqueue(eq, (void *)eqd);
      }
    }
  }
}

int serve(const char *host, const int port, ssize_t size) {
  struct sockaddr_in address;
  input = malloc(size);
  output = malloc(size);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  inet_aton(host, &address.sin_addr);
  int server = socket(AF_INET, SOCK_STREAM, 0);
  if (prepare_socket(server, 1) < 0) {
    return -1;
  }
  if (setfdnonblocking(server) < 0) {
    return -1;
  }
  if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0) {
    perror("Unable to bind to address ");
    return -1;
  }
  if (listen(server, LATENCY_BACKLOG_SIZE) < 0) {
    perror("Unable to listen for new connections ");
    return -1;
  }
  int eserver = epoll_create1(0);
  if (eserver < 0) {
    perror("Unable to create epoll file descriptor ");
    return -1;
  }
  struct eq_descriptor *eqd = malloc(sizeof(struct eq_descriptor));
  if (eqd == NULL) {
    perror("Unable to allocate eq descriptor ");
    return -1;
  }
  eqd->fd = server;
  eqd->remaining = 0;
  struct epoll_event ev = {
    .data.ptr = (void *)eqd,
    .events = EPOLLIN | EPOLLRDHUP | EPOLLPRI,
  };
  if (epoll_ctl(eserver, EPOLL_CTL_ADD, server, &ev) < 0) {
    perror("Unable to add server to epoll file descriptor ");
    return -1;
  }
  struct epoll_event *events = malloc(sizeof(struct epoll_event) * LATENCY_MAX_EVENTS);
  if (events == NULL) {
    perror("Unable to allocate memory for events ");
    return -1;
  }
  struct send_responses_param param = {
    .ring = rte_ring_create("tx_queue", 1024, RING_F_SP_ENQ | RING_F_SC_DEQ),
    .size = size,
  };
  if (param.ring == NULL) {
    perror("Unable to create ring ");
    return -1;
  }
  pthread_t worker;
  pthread_create(&worker, NULL, send_responses, (void *)&param);
  while (1) {
    int n, i, fd, ret;
    n = epoll_wait(eserver, events, LATENCY_MAX_EVENTS, 0);
    if (n < 0) {
      perror("Epoll wait error ");
      return -1;
    }
    for (i = 0; i < n; i++) {
      eqd = (struct eq_descriptor *)events[i].data.ptr;
      fd = eqd->fd;
      if ((events[i].events & EPOLLERR) ||
          (events[i].events & EPOLLHUP) ||
          (!(events[i].events & EPOLLIN))) {
        fprintf(stderr, "epoll error\n");
        if (close(fd) < 0) {
          perror("Unable to close file descriptor ");
          return -1;
        }
        if (epoll_ctl(eserver, EPOLL_CTL_DEL, fd, &ev) < 0) {
          perror("Unable to remove epoll file descriptor ");
          return -1;
        }
        free(eqd);
        continue;
      }
      if (fd == server) {
        while (1) {
          ret = accept_and_add_to_epoll_fd(server, eserver, size);
          if (ret < 0) {
            return -1;
          } else if (ret) {
            break;
          }
        }
        continue;
      } else {
        ret = receive_data(eqd, size);
        if (ret < 0) {
          return -1;
        } else if (ret) {
          do {
          } while (!unlikely(rte_ring_enqueue(param.ring, (void *)eqd)));
        }
      }
    }
  }
}
