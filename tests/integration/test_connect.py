import socket
import unittest
import sol_test


class ConnectTest(unittest.TestCase):

    def setUp(self):
        self.conn = socket.socket()
        self.conn.connect(('127.0.0.1', 1883))

    def tearDown(self):
        self.conn.close()

    def test_connect(self):
        connect_packet = sol_test.create_connect()
        self.conn.send(connect_packet)
        packet = self.conn.recv(100)
        connack, rc = sol_test.read_connack(packet)
        self.assertEqual(rc, 0)
