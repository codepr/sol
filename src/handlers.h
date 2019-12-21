#ifndef HANDLERS_H
#define HANDLERS_H

#include "core.h"
#include "server.h"

void publish_message(struct mqtt_publish *, const struct topic *, int);

int handle_command(unsigned, struct io_event *);

#endif
