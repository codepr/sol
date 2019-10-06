import socket
import unittest
import sol_test


class ConnectTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.conn = socket.socket()
        cls.broker = sol_test.start_broker()
        cls.conn.connect(('127.0.0.1', 1883))

    @classmethod
    def tearDownClass(cls):
        cls.conn.close()
        sol_test.kill_broker(cls.broker)

    def test_connect(self):
        connect_packet = sol_test.create_connect()
        self.conn.send(connect_packet)
        packet = self.conn.recv(100)
        connack, rc = sol_test.read_connack(packet)
        self.assertEqual(rc, 0)

    def test_connect_with_username(self):
        connect_packet = sol_test.create_connect('test-user')
        self.conn.send(connect_packet)
        packet = self.conn.recv(100)
        connack, rc = sol_test.read_connack(packet)
        self.assertEqual(rc, 0)

    def test_disconnect(self):
        disconnect_packet = sol_test.create_disconnect()
        self.conn.send(disconnect_packet)
