#include "server.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *bind_host = "0.0.0.0";
    int port = 7000;

    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0) {
            fprintf(stderr, "invalid port: %s\n", argv[1]);
            return 1;
        }
    }

    return server_run(bind_host, port) == 0 ? 0 : 1;
}
