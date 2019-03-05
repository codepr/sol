Sol
===

Oversimplified MQTT broker written from scratch, which mimick mosquitto
features. Implemented for learning how the protocol works, for now it supports
almost all MQTT v3.1.1 commands on linux platform; it relies on EPOLL interface
introduced on kernel 2.5.44.

## Build

```sh
$ cmake .
$ make
```

## Quickstart play

The broker can be tested using `mosquitto_sub` and `mosquitto_pub` or with
`paho-mqtt` python driver.

To run the broker with DEBUG loggin on:

```sh
$ ./sol -v
```

A simple configuration can be passed in with `-c` flag:

```sh
$ ./sol -c path/to/sol.conf
```

## Features roadmap

It's still a work in progress but it already handle the most of the basic
features expected from a MQTT broker.

- [X] Unix/TCP sockets
- [X] All main commands are handled correctly
- [X] QoS 0, 1, 2 are handled
- [X] Trie as underlying structure to handle topic hierarchies
- [X] Periodic tasks like stats publishing
- [X] Wildcards on subscriptions (though simple to implement, will be added soon)
- [ ] QoS 1 and 2 tracking of pending clients and re-send
- [ ] Session present check and handling
- [ ] Authentication
- [ ] SSL/TLS connections
- [ ] Last will & Testament
- [ ] Check on max memory used

It apparently not leak memory as of now, there's probably some corner cases,
not deeply investigated yet.

## Contributing

Pull requests are welcome, just create an issue and fork it.
