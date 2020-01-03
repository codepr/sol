#ifndef HANDLERS_H
#define HANDLERS_H

#include "core.h"
#include "server.h"

void publish_message(struct mqtt_packet *, const struct topic *, struct ev_ctx *);

int handle_command(unsigned, struct io_event *);

#endif
