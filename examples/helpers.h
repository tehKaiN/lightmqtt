#ifndef _EXAMPLES_HELPERS_H
#define _EXAMPLES_HELPERS_H

#include <stddef.h>
#include "lightmqtt/core.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#if !defined(O_NONBLOCK)
// No non-blocking, sorry!
#define O_NONBLOCK 0
#endif
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#endif

lmqtt_io_result_t get_time(long *secs, long *nsecs);
lmqtt_io_result_t file_read(void *data, void *buf, size_t buf_len,
    size_t *bytes_read, int *os_error);
lmqtt_io_result_t file_write(void *data, void *buf, size_t buf_len,
    size_t *bytes_read, int *os_error);
lmqtt_io_result_t socket_read(void *data, void *buf, size_t buf_len,
    size_t *bytes_read, int *os_error);
lmqtt_io_result_t socket_write(void *data, void *buf, size_t buf_len,
    size_t *bytes_written, int *os_error);
int socket_init(void);
void socket_cleanup(void);
int socket_open(const char *address, unsigned short port);
void socket_close(int fd);
int make_temporary_file(const char *template);

#endif
