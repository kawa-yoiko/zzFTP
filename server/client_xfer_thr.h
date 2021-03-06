#ifndef SLOW_DATA
  #define BUF_SIZE 1024
#else
  #define BUF_SIZE 8
#endif

#include <poll.h>

#include <sys/socket.h>

static inline void double_and_limit(int *x, int limit)
{
  int new_x = (*x) * 2;
  *x = (new_x < limit ? new_x : limit);
}

// 0 - Continue
// 1 - Completed normally
// 2 - Aborted abnormally
static inline int process_block(
  client *c,
  int conn_fd, enum dat_type_t dat_type, FILE *fp, void *buf,
  int *process_sleep)
{
  bool xfer_complete;
  if (dat_type == DATA_SEND_FILE || dat_type == DATA_SEND_PIPE) {
    size_t bytes_read = fread(buf, 1, BUF_SIZE, fp);
    if (bytes_read > 0) {
      write_all(conn_fd, buf, bytes_read);
    #ifdef SLOW_DATA
      usleep(300000);
    #endif
    }
    xfer_complete = feof(fp);
  } else /* if (dat_type == DATA_RECV_FILE) */ {
    ssize_t bytes_read = read(conn_fd, buf, BUF_SIZE);
    if (bytes_read > 0) {
      *process_sleep = 1000;
      fwrite(buf, 1, bytes_read, fp);
    } else if (bytes_read == -1) {
      if (errno == EAGAIN) {
        usleep(*process_sleep);
        double_and_limit(process_sleep, 200000);
      } else {
        warn("read() failed");
        bytes_read = 0;   // Treat transfer as complete
      }
    }
    xfer_complete = (bytes_read == 0);
  }
  if (xfer_complete) {
    return 1;
  } else if (ferror(fp) != 0) {
    return 2;
  }
  return 0;
}

static inline void cleanup(
  client *c,
  int conn_fd, enum dat_type_t dat_type, FILE *fp, void *buf, int st)
{
  if (conn_fd != -1) close(conn_fd);

  free(buf);
  if (fp != NULL) {
    if (dat_type == DATA_SEND_PIPE) pclose(fp);
    else fclose(fp);
  }

  crit({ c->dat_fp = NULL; });
  c->state = CLST_READY;

  info("data thread terminated");

  if (st == 1)
    mark(226, "Transfer complete.");
  else if (st == 2)
    mark(451, "Transfer aborted by internal I/O error.");
}

// Active mode

static void *active_data(void *arg)
{
  client *c = (client *)arg;

  int conn_fd = -1;
  FILE *fp = NULL;
  enum dat_type_t dat_type = DATA_UNDEFINED;
  void *buf = malloc(BUF_SIZE);
  int st = 0;

  // Wait for the file
  crit({
    pthread_cond_wait(&c->cond_dat, &c->mutex_dat);
    fp = c->dat_fp; dat_type = c->dat_type;
  });

  // Establish connection
  conn_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (conn_fd == -1) {
    mark(425, "Cannot establish connection: socket() failed.");
    goto _cleanup;
  }

  // Fill in the address from client record
  struct sockaddr_in addr = { 0 };
  addr.sin_family = AF_INET;
  memcpy(&addr.sin_addr.s_addr, c->addr, 4);
  addr.sin_port = htons(c->port);
  // TODO: Connect with a timeout
  if (connect(conn_fd, (struct sockaddr *)&addr, sizeof addr) == -1) {
    mark(425, "Cannot establish connection.");
    goto _cleanup;
  }
  fcntl(conn_fd, F_SETFL, fcntl(conn_fd, F_GETFL, 0) | O_NONBLOCK);

  int process_sleep = 1000;

  while (1) {
    bool running;
    crit({ running = c->thr_dat_running; });
    if (!running) break;

    if ((st = process_block(c, conn_fd, dat_type, fp, buf, &process_sleep))
        != 0)
      break;
  }

_cleanup:
  cleanup(c, conn_fd, dat_type, fp, buf, st);
  return NULL;
}

// Passive mode

struct passive_data_arg {
  int sock_fd;
  client *c;
};

static void *passive_data(void *arg)
{
  int sock_fd = ((struct passive_data_arg *)arg)->sock_fd;
  client *c = ((struct passive_data_arg *)arg)->c;
  free(arg);

  int conn_fd = -1;
  FILE *fp = NULL;
  enum dat_type_t dat_type = DATA_UNDEFINED;
  void *buf = malloc(BUF_SIZE);
  int st = 0;

  int accept_sleep = 1000;
  int process_sleep = 1000;

  while (1) {
    bool running;
    crit({ running = c->thr_dat_running; });
    if (!running) break;

    if (conn_fd == -1) {
      if ((conn_fd = accept(sock_fd, NULL, NULL)) == -1) {
        if (errno == EAGAIN) {
          usleep(accept_sleep);
          double_and_limit(&accept_sleep, 200000);
          continue;
        } else {
          panic("accept() failed");
        }
      }
    }

    if (conn_fd != -1) {
      if (fp == NULL)
        crit({ fp = c->dat_fp; dat_type = c->dat_type; });
      if (fp != NULL) {
        if ((st = process_block(c, conn_fd, dat_type, fp, buf, &process_sleep))
            != 0)
          break;
      } else {
        // Connected and no file present. Detect disconnection.
        // Ref: http://stefan.buettcher.org/cs/conn_closed.html
        struct pollfd poll_fd;
        char c;
        poll_fd.fd = conn_fd;
        poll_fd.events = POLLIN | POLLHUP;
        poll_fd.revents = 0;
        if (poll(&poll_fd, 1, 100) > 0 && (poll_fd.revents & POLLHUP)
            && recv(conn_fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0) {
          close(conn_fd);
          conn_fd = -1;
        }
      }
    }
  }

  close(sock_fd);
  cleanup(c, conn_fd, dat_type, fp, buf, st);
  return NULL;
}

#undef BUF_SIZE
