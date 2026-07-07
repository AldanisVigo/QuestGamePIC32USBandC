#ifndef PIC_SERIAL_PORT_H
#define PIC_SERIAL_PORT_H

#include <stddef.h>
#include <sys/types.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct
{
    char path[PATH_MAX];
} PicSerialPortInfo;

size_t pic_serial_list_ports(PicSerialPortInfo *ports, size_t capacity);
int pic_serial_open(const char *path, int baud_rate, char *error, size_t error_size);
void pic_serial_close(int fd);
int pic_serial_write_all(int fd, const char *data, size_t length, int timeout_ms, char *error, size_t error_size);
ssize_t pic_serial_read_available(int fd, char *buffer, size_t buffer_size, char *error, size_t error_size);

#endif
