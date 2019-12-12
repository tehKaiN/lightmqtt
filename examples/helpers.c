#include "helpers.h"

#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

lmqtt_io_result_t get_time(long *secs, long *nsecs)
{
    struct timespec tim;

    if (clock_gettime(CLOCK_MONOTONIC, &tim) == 0) {
        *secs = tim.tv_sec;
        *nsecs = tim.tv_nsec;
        return LMQTT_IO_SUCCESS;
    }

    return LMQTT_IO_ERROR;
}

lmqtt_io_result_t file_read(void *data, void *buf, size_t buf_len,
    size_t *bytes_read, int *os_error)
{
    int socket_fd = *((int *) data);
    int res;

    res = read(socket_fd, buf, buf_len);
    if (res >= 0) {
        *bytes_read = res;
        return LMQTT_IO_SUCCESS;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        *bytes_read = 0;
        return LMQTT_IO_WOULD_BLOCK;
    }

    *bytes_read = 0;
    *os_error = errno;
    return LMQTT_IO_ERROR;
}

lmqtt_io_result_t file_write(void *data, void *buf, size_t buf_len,
    size_t *bytes_written, int *os_error)
{
    int socket_fd = *((int *) data);
    int res;

    res = write(socket_fd, buf, buf_len);
    if (res >= 0) {
        *bytes_written = res;
        return LMQTT_IO_SUCCESS;
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EPIPE) {
        *bytes_written = 0;
        return LMQTT_IO_WOULD_BLOCK;
    }

    *bytes_written = 0;
    *os_error = errno;
    return LMQTT_IO_ERROR;
}

lmqtt_io_result_t socket_read(void *data, void *buf, size_t buf_len,
    size_t *bytes_read, int *os_error)
{
    int socket_fd = *((int *) data);
    int res;

    res = recv(socket_fd, buf, buf_len, 0);
    if (res >= 0) {
        *bytes_read = res;
        return LMQTT_IO_SUCCESS;
    }

#if defined(_WIN32)
    *os_error = WSAGetLastError();
    if(*os_error == WSAEWOULDBLOCK || *os_error == WSATRY_AGAIN) {
#else
    *os_error = errno;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
#endif
        *bytes_read = 0;
        return LMQTT_IO_WOULD_BLOCK;
    }

    *bytes_read = 0;
    return LMQTT_IO_ERROR;
}

lmqtt_io_result_t socket_write(void *data, void *buf, size_t buf_len,
    size_t *bytes_written, int *os_error)
{
    int socket_fd = *((int *) data);
    int res;

    res = send(socket_fd, buf, buf_len, 0);
    if (res >= 0) {
        *bytes_written = res;
        return LMQTT_IO_SUCCESS;
    }

#if defined(_WIN32)
    *os_error = WSAGetLastError();
    if(*os_error == WSAEWOULDBLOCK || *os_error == WSATRY_AGAIN) {
#else
    *os_error = errno;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EPIPE) {
#endif
        *bytes_written = 0;
        return LMQTT_IO_WOULD_BLOCK;
    }

    *bytes_written = 0;
    return LMQTT_IO_ERROR;
}

int socket_init(void)
{
#if defined(_WIN32)
    int result;
    WSADATA wsa_data;

    result = WSAStartup(MAKEWORD(2,2), &wsa_data);
    if (result != 0) {
        return -1;
    }
#endif
    return 0;
}

void socket_cleanup(void)
{
    WSACleanup();
}

int socket_open(const char *address, unsigned short port)
{
    struct sockaddr_in sin;
    int sock;
    unsigned long is_nonblocking;

    sock = socket(AF_INET, SOCK_STREAM, 0);
#if defined(_WIN32)
    is_nonblocking = 1;
    ioctlsocket(sock, FIONBIO, &is_nonblocking);
#else
    fcntl(sock, F_SETFL, O_NONBLOCK);
#endif

    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    if (inet_pton(AF_INET, address, &sin.sin_addr) == 0) {
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *) &sin, sizeof(sin)) != 0) {
#if defined(_WIN32)
        if(WSAGetLastError() == WSAEWOULDBLOCK) {
            // Wait for completion
            fd_set write, err;
            TIMEVAL Timeout;

            FD_ZERO(&write);
            FD_ZERO(&err);
            FD_SET(sock, &write);
            FD_SET(sock, &err);

            Timeout.tv_sec = 10;
            Timeout.tv_usec = 0;
            if(select(0, NULL, &write, &err, &Timeout) == 0) {
                close(sock);
                return -1;
            }
            else {
                if(FD_ISSET(sock, &err)) {
                    close(sock);
                    return -1;
                }
            }
        }
        else {
            close(sock);
            return -1;
        }
#else
        if(errno != EINPROGRESS) {
            close(sock);
            return -1;
        }
#endif
    }

    return sock;
}

void socket_close(int fd)
{
    close(fd);
}

int make_temporary_file(const char *template)
{
    char filename[256];
    int fd, err;

#if defined(_WIN32)
    strcpy(filename, template);
    err = _mktemp_s(filename, 256);
    if(err != 0)
        return -1;
    fd = open(filename, O_RDWR | O_NONBLOCK, 0);
#else
    strcpy(filename, "/tmp/")
    strcat(filename, template);
    fd = mkostemp(filename, O_NONBLOCK);
#endif
    return fd;
}
