#include "io_utils.h"
#include "client.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

int main(int argc, char *argv[])
{
  signal(SIGPIPE, SIG_IGN);

  // Allocate socket
  int sock_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock_fd == -1)
    panic("socket() failed");

  // Allow successive runs without waiting
  if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR,
      &(int){1}, sizeof(int)) == -1)
    panic("setsockopt() failed");

  // Bind to address and start listening
  struct sockaddr_in addr = { 0 };
  addr.sin_family = AF_INET;
  addr.sin_port = htons(1800);
  addr.sin_addr.s_addr = INADDR_ANY;
  if (bind(sock_fd, (struct sockaddr *)&addr, sizeof addr) == -1)
    panic("bind() failed");
  if (listen(sock_fd, 1024) == -1)
    panic("listen() failed");

  // Accept connections
  struct sockaddr_in cli_addr;
  socklen_t cli_addr_len;
  while (1) {
    cli_addr_len = sizeof cli_addr;
    int conn_fd = accept(sock_fd, (struct sockaddr *)&cli_addr, &cli_addr_len);
    if (conn_fd == -1)
      panic("accept() failed");

    client *c = client_create(conn_fd);
    client_run_loop(c);
    client_close(c);
  }

  return 0;
}
