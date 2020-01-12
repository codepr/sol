/* BSD 2-Clause License
 *
 * Copyright (c) 2019, Andrea Giacomo Baldan All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include "util.h"
#include "config.h"
#include "server.h"

// Stops epoll_wait loops by sending an event
static void sigint_handler(int signum) {
    (void) signum;
    eventfd_write(conf->run, 1);
}

static const char *flag_description[] = {
    "Print this help",
    "Set a configuration file to load and use",
    "Set the listening host address",
    "Set the listening port",
    "Enable all logs, setting log level to DEBUG",
    "Run in daemon mode"
};

void print_help(char *me) {
    printf("\nSol v%s MQTT broker 3.1.1\n\n", VERSION);
    printf("Usage: %s [-a addr] [-p port] [-c conf] [-v|-d|-h]\n\n", me);
    const char flags[6] = "hcapvd";
    for (int i = 0; i < 6; ++i)
        printf(" -%c: %s\n", flags[i], flag_description[i]);
    printf("\n");
}

int main (int argc, char **argv) {

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    char *addr = DEFAULT_HOSTNAME;
    char *port = DEFAULT_PORT;
    char *confpath = DEFAULT_CONF_PATH;
    int debug = 0, daemon = 0;
    int opt;

    // Set default configuration
    config_set_default();

    while ((opt = getopt(argc, argv, "a:c:p:m:vhdn:")) != -1) {
        switch (opt) {
            case 'a':
                addr = optarg;
                strcpy(conf->hostname, addr);
                break;
            case 'c':
                confpath = optarg;
                break;
            case 'p':
                port = optarg;
                strcpy(conf->port, port);
                break;
            case 'v':
                debug = 1;
                break;
            case 'd':
                daemon = 1;
                break;
            case 'h':
                print_help(argv[0]);
                exit(EXIT_SUCCESS);
            default:
                fprintf(stderr,
                        "Usage: %s [-a addr] [-p port] [-c conf] [-v]\n",
                        argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // Override default DEBUG mode
    conf->loglevel = debug == 1 ? DEBUG : WARNING;

    // Try to load a configuration, if found
    config_load(confpath);

    sol_log_init(conf->logpath);

    if (daemon == 1)
        daemonize();

    // Print configuration
    config_print();

    start_server(conf->hostname, conf->port);

    sol_log_close();

    return 0;
}
