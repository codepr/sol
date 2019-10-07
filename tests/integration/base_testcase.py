import socket
import unittest
import contextlib
import sol_test


class BaseTestcase(unittest.TestCase):

    @contextlib.contextmanager
    def connection(self, addr=('127.1', 1883)):
        sock = self.get_connection(addr)
        yield sock
        sock.close()

    def get_connection(self, addr=('127.1', 1883)):
        sock = socket.socket()
        sock.connect(addr)
        return sock

    def send_disconnect(self, conn):
        disconnect_packet = sol_test.create_disconnect(0)
        conn.send(disconnect_packet)
