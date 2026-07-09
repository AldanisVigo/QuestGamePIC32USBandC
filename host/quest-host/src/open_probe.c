#include "serial_port.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST_TIMEOUT_MS 5000

typedef struct
{
    const char *name;
    int flags;
    bool use_flock;
    bool use_app_open;
} OpenTest;

static long long now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);

    return ((long long)tv.tv_sec * 1000LL) + ((long long)tv.tv_usec / 1000LL);
}

static void run_child_test(const char *path, OpenTest test)
{
    long long start_ms = now_ms();
    int fd = -1;

    printf("  %-34s start\n", test.name);
    fflush(stdout);

    if(test.use_app_open)
    {
        char error[160];

        fd = pic_serial_open(path, 115200, error, sizeof(error));
        if(fd < 0)
        {
            printf("  %-34s FAIL after %lld ms: %s\n",
                   test.name,
                   now_ms() - start_ms,
                   error);
            fflush(stdout);
            _exit(1);
        }
    }
    else
    {
        fd = open(path, test.flags);
        if(fd < 0)
        {
            printf("  %-34s FAIL after %lld ms: %s\n",
                   test.name,
                   now_ms() - start_ms,
                   strerror(errno));
            fflush(stdout);
            _exit(1);
        }

        if(test.use_flock && flock(fd, LOCK_EX | LOCK_NB) != 0)
        {
            printf("  %-34s LOCK FAIL after %lld ms: %s\n",
                   test.name,
                   now_ms() - start_ms,
                   strerror(errno));
            fflush(stdout);
            close(fd);
            _exit(1);
        }
    }

    printf("  %-34s OK fd %d after %lld ms\n",
           test.name,
           fd,
           now_ms() - start_ms);
    fflush(stdout);

    close(fd);
    _exit(0);
}

static void run_timed_test(const char *path, OpenTest test)
{
    pid_t pid = fork();
    long long start_ms = now_ms();

    if(pid < 0)
    {
        printf("  %-34s could not fork: %s\n", test.name, strerror(errno));
        return;
    }

    if(pid == 0)
    {
        run_child_test(path, test);
    }

    while(true)
    {
        int status;
        pid_t done = waitpid(pid, &status, WNOHANG);

        if(done == pid)
        {
            return;
        }

        if(done < 0)
        {
            printf("  %-34s wait failed: %s\n", test.name, strerror(errno));
            return;
        }

        if((now_ms() - start_ms) >= TEST_TIMEOUT_MS)
        {
            kill(pid, SIGKILL);
            (void)waitpid(pid, &status, 0);
            printf("  %-34s TIMEOUT after %d ms\n", test.name, TEST_TIMEOUT_MS);
            return;
        }

        usleep(10000);
    }
}

static void run_tests_for_path(const char *path)
{
    OpenTest tests[] =
    {
        {
            "raw O_RDONLY|O_NONBLOCK",
            O_RDONLY | O_NOCTTY | O_NONBLOCK,
            false,
            false
        },
        {
            "raw O_WRONLY|O_NONBLOCK",
            O_WRONLY | O_NOCTTY | O_NONBLOCK,
            false,
            false
        },
        {
            "raw O_RDWR|O_NONBLOCK",
            O_RDWR | O_NOCTTY | O_NONBLOCK,
            false,
            false
        },
        {
            "raw O_RDWR + flock",
            O_RDWR | O_NOCTTY | O_NONBLOCK,
            true,
            false
        },
        {
            "app pic_serial_open",
            0,
            false,
            true
        }
    };

    printf("%s\n", path);

    for(size_t i = 0; i < (sizeof(tests) / sizeof(tests[0])); i++)
    {
        run_timed_test(path, tests[i]);
    }
}

int main(int argc, char **argv)
{
    if(argc > 1)
    {
        for(int i = 1; i < argc; i++)
        {
            run_tests_for_path(argv[i]);
        }

        return 0;
    }

    PicSerialPortInfo ports[16];
    size_t port_count = pic_serial_list_ports(ports, sizeof(ports) / sizeof(ports[0]));

    if(port_count == 0U)
    {
        printf("No USB CDC serial devices found.\n");
        return 1;
    }

    for(size_t i = 0; i < port_count; i++)
    {
        run_tests_for_path(ports[i].path);
    }

    return 0;
}
