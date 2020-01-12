[![Build Status](https://travis-ci.org/codepr/sol.svg?branch=master)](https://travis-ci.org/codepr/sol)

Sol
===

Oversimplified MQTT broker written from scratch, which mimic mosquitto
features. Implemented to learning how the protocol works, it supports
almost all MQTT v3.1.1 commands on linux platform and relies on EPOLL interface
for multiplexing I/O. Development process is documented in this [series of posts](https://codepr.github.io/posts/sol-mqtt-broker).
**Not for production use**.

### Features

It's still a work in progress but it already handles the most of the basic
features expected from an MQTT broker. It does not leak memory as of now,
there's probably some corner cases, not deeply investigated yet.

- QoS 0, 1, 2
- Retained messages
- Periodic stats publishing
- Wildcards (#) on subscriptions
- Authentication through username and password
- SSL/TLS connections (almost ready)
- Logging
- Multiplexing IO with abstraction over backend, currently supports
  select/poll/epoll choosing the better implementation.

### To be implemented

- Session present check and handling, already started
- Last will & Testament, already started
- Wildcards (+) on subscriptions
- Persistence on disk for inflight messages on disconnected clients
- Check on max memory used

### Maybe

- MQTT 5.0 support
- Porting for BSD/MAC osx
- Scaling (feeling brave?)

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

# Comment ip_address and ip_port, set unix_socket and
# UNIX family socket will be used instead

ip_address 127.0.0.1
ip_port 8883

# unix_socket /tmp/sol.sock

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

# TLS certs paths, cafile act as a flag as well to set TLS/SSL ON
# cafile /etc/sol/certs/ca.crt
# certfile /etc/sol/certs/cert.crt
# keyfile /etc/sol/certs/cert.key

# Authentication
# allow_anonymous false
# password_file /etc/sol/passwd

# TLS protocols, supported versions should be listed comma separated
# example:
# tls_protocols tlsv1_2,tlsv1_3
```

If `allow_anonymous` if false, a password file have to be specyfied. The
password file follow the standard format of username:password line by line.
To generate one just add all entries needed in a file and run passwd.py after
it:

```sh
$ cat sample_passwd_file
user1:pass1
user2:pass2
$ python passwd.py sample_passwd_file
$ cat sample_passwd_file
user1:$6$69qVAELLWuKXWQPQ$oO7lP/hNS4WPABTyK4nkJs4bcRLYFi365YX13cEc/QBJtQgqf2d5rOIUdqoUin.YVGXC3OXY9MSz7Z66ZDkCW/
user2:$6$vtHdafhGhxpXwgBa$Y3Etz8koC1YPSYhXpTnhz.2vJTZvCUGk3xUdjyLr9z9XgE8asNwfYDRLIKN4Apz48KKwKz0YntjHsPRiE6r3g/
```

## Contributing

Pull requests are welcome, just create an issue and fork it.
