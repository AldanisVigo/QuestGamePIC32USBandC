#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

#include "config/default/definitions.h"

int main(void)
{
    SYS_Initialize(NULL);

    while (true)
    {
        SYS_Tasks();
    }

    return EXIT_FAILURE;
}