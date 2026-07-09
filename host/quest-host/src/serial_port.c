#include "serial_port.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/time.h>
#include <unistd.h>

static void set_error(char *error, size_t error_size, const char *format, ...)
{
    va_list args;

    if(error == NULL || error_size == 0U)
    {
        return;
    }

    va_start(args, format);
    vsnprintf(error, error_size, format, args);
    va_end(args);
}

static bool add_unique_port(PicSerialPortInfo *ports, size_t capacity, size_t *count, const char *path)
{
    for(size_t i = 0; i < *count; i++)
    {
        if(strcmp(ports[i].path, path) == 0)
        {
            return true;
        }
    }

    if(*count >= capacity)
    {
        return false;
    }

    snprintf(ports[*count].path, sizeof(ports[*count].path), "%s", path);
    *count += 1U;

    return true;
}

static void scan_pattern(PicSerialPortInfo *ports, size_t capacity, size_t *count, const char *pattern)
{
    glob_t matches;

    memset(&matches, 0, sizeof(matches));

    if(glob(pattern, 0, NULL, &matches) == 0)
    {
        for(size_t i = 0; i < matches.gl_pathc; i++)
        {
            if(!add_unique_port(ports, capacity, count, matches.gl_pathv[i]))
            {
                break;
            }
        }
    }

    globfree(&matches);
}

size_t pic_serial_list_ports(PicSerialPortInfo *ports, size_t capacity)
{
    size_t count = 0;

    if(ports == NULL || capacity == 0U)
    {
        return 0;
    }

    scan_pattern(ports, capacity, &count, "/dev/cu.usbmodem*");
    scan_pattern(ports, capacity, &count, "/dev/cu.usbserial*");
    scan_pattern(ports, capacity, &count, "/dev/ttyACM*");
    scan_pattern(ports, capacity, &count, "/dev/ttyUSB*");

    return count;
}

int pic_serial_open(const char *path, int baud_rate, char *error, size_t error_size)
{
    int fd;

    (void)baud_rate;

    if(path == NULL || path[0] == '\0')
    {
        set_error(error, error_size, "No serial device selected");
        return -1;
    }

    fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if(fd < 0)
    {
        set_error(error, error_size, "Could not open %s: %s", path, strerror(errno));
        return -1;
    }

    if(flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        if(errno == EWOULDBLOCK || errno == EAGAIN)
        {
            set_error(error, error_size, "Could not lock %s: serial port is already in use", path);
        }
        else
        {
            set_error(error, error_size, "Could not lock %s: %s", path, strerror(errno));
        }

        close(fd);
        return -1;
    }

    return fd;
}

void pic_serial_close(int fd)
{
    if(fd >= 0)
    {
        close(fd);
    }
}

static long long now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);

    return ((long long)tv.tv_sec * 1000LL) + ((long long)tv.tv_usec / 1000LL);
}

int pic_serial_write_all(int fd, const char *data, size_t length, int timeout_ms, char *error, size_t error_size)
{
    size_t offset = 0;
    long long deadline = now_ms() + timeout_ms;

    while(offset < length)
    {
        ssize_t written = write(fd, data + offset, length - offset);

        if(written > 0)
        {
            offset += (size_t)written;
            continue;
        }

        if(written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            set_error(error, error_size, "Serial write failed: %s", strerror(errno));
            return -1;
        }

        {
            long long remaining = deadline - now_ms();
            struct pollfd pfd;
            int poll_result;

            if(remaining <= 0)
            {
                set_error(error, error_size, "Serial write timed out");
                return -1;
            }

            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;

            poll_result = poll(&pfd, 1, remaining > 100 ? 100 : (int)remaining);
            if(poll_result < 0 && errno != EINTR)
            {
                set_error(error, error_size, "Serial write wait failed: %s", strerror(errno));
                return -1;
            }
        }
    }

    return 0;
}

ssize_t pic_serial_read_available(int fd, char *buffer, size_t buffer_size, char *error, size_t error_size)
{
    ssize_t bytes_read;

    if(buffer == NULL || buffer_size == 0U)
    {
        return 0;
    }

    bytes_read = read(fd, buffer, buffer_size);
    if(bytes_read >= 0)
    {
        return bytes_read;
    }

    if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
    {
        return 0;
    }

    set_error(error, error_size, "Serial read failed: %s", strerror(errno));
    return -1;
}
