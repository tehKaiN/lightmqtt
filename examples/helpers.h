#ifndef _EXAMPLES_HELPERS_H
#define _EXAMPLES_HELPERS_H

#include <stddef.h>
#include "lightmqtt/core.h"

lmqtt_io_result_t get_time(long *secs, long *nsecs);
lmqtt_io_result_t file_read(void *data, void *buf, size_t buf_len,
    size_t *bytes_read, int *os_error);
lmqtt_io_result_t file_write(void *data, void *buf, size_t buf_len,
    size_t *bytes_written, int *os_error);
int socket_open(const char *address, unsigned short port);
void socket_close(int fd);

#endif
