Sol
===

Oversimplified MQTT broker written from scratch, which mimic mosquitto
features. Implemented to learning how the protocol works, for now it supports
almost all MQTT v3.1.1 commands on linux platform; it relies on EPOLL interface
for multiplexing I/O. Development process is documented in this [series of posts](https://codepr.github.io/posts/sol-mqtt-broker).
**Not for production use**.

## Build

```sh
$ cmake .
$ make
```

## Quickstart play

The broker can be tested using `mosquitto_sub` and `mosquitto_pub` or with
`paho-mqtt` python driver.

To run the broker with DEBUG logging on:

```sh
$ ./sol -v
```

A simple configuration can be passed in with `-c` flag:

```sh
$ ./sol -c path/to/sol.conf
```

As of now the configuration is very small and it's possible to set some of the
most common parameters, default path is located to `/etc/sol/sol.conf`:

```sh
# Sol configuration file, uncomment and edit desired configuration

# Network configuration

# Uncomment ip_address and ip_port to set socket family to TCP, if unix_socket
# is set, UNIX family socket will be used

# ip_address 127.0.0.1
# ip_port 9090

unix_socket /tmp/sol.sock

# Logging configuration

# Could be either DEBUG, INFO/INFORMATION, WARNING, ERROR
log_level DEBUG

log_path /tmp/sol.log

# Max memory to be used, after which the system starts to reclaim memory by
# freeing older items stored
max_memory 2GB

# Max memory that will be allocated for each request
max_request_size 2MB

# TCP backlog, size of the complete connection queue
tcp_backlog 128

# Interval of time between one stats publish on $SOL topics and the subsequent
stats_publish_interval 10s

# SSL certs paths, certfile act as a flag as well to set TLS/SSL ON
certfile /etc/sol/certs/cert.pem

keyfile /etc/sol/certs/key.pem
```

## Features roadmap

It's still a work in progress but it already handles the most of the basic
features expected from an MQTT broker.

- [X] Unix/TCP sockets
- [X] Pub/Sub working fine
- [X] QoS 0, 1, 2 are correctly parsed, 0 and 1 handled as well
- [X] Retained messages
- [X] Trie as underlying structure to handle topic hierarchies
- [X] Periodic tasks like stats publishing
- [X] Wildcards (#) on subscriptions
- [ ] Wildcards (+) on subscriptions
- [ ] QoS 2 tracking of pending clients and re-send
- [ ] Session present check and handling
- [ ] Authentication
- [ ] SSL/TLS connections
- [ ] Last will & Testament
- [ ] Check on max memory used

It apparently not leak memory as of now, there's probably some corner cases,
not deeply investigated yet.

## Contributing

Pull requests are welcome, just create an issue and fork it.
