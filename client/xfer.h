#ifndef zzftp_client__xfer_h
#define zzftp_client__xfer_h

#include "io_utils.h"

#include <stdint.h>

typedef struct xfer_s {
  int fd;
  rlb b;
  uint8_t local_addr[6];
} xfer;

#define XFER_ERR_SOCKET   (-1)
#define XFER_ERR_THREAD   (-2)
#define XFER_ERR_CONNECT  (-3)
#define XFER_ERR_IO       (-4)

void xfer_init(xfer *x, const char *host, int port, void (*next)(int));
void xfer_deinit(xfer *x);

void xfer_write(xfer *x, const char *str, void (*next)(int));
void xfer_read_mark(xfer *x, void (*next)(int, char *));

#endif
