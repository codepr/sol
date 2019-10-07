import socket
import unittest
import sol_test
import base_testcase


class TestConnect(base_testcase.BaseTestcase):

    def test_connect(self):
        with self.connection() as conn:
            connect_packet = sol_test.create_connect()
            conn.send(connect_packet)
            packet = conn.recv(100)
            connack, rc = sol_test.read_connack(packet)
            self.assertEqual(connack, 32)
            self.assertEqual(rc, 0)
            self.send_disconnect(conn)

    def test_connect_with_client_id(self):
        with self.connection() as conn:
            connect_packet = sol_test.create_connect('test-user')
            conn.send(connect_packet)
            packet = conn.recv(100)
            connack, rc = sol_test.read_connack(packet)
            self.assertEqual(connack, 32)
            self.assertEqual(rc, 0)
            self.send_disconnect(conn)
