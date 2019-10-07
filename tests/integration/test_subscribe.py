import socket
import unittest
import sol_test
import base_testcase


class TestSubscribe(base_testcase.BaseTestcase):

    def test_subscribe(self):
        with self.connection() as conn:
            connect_packet = sol_test.create_connect()
            conn.send(connect_packet)
            packet = conn.recv(100)
            connack, rc = sol_test.read_connack(packet)
            self.assertEqual(rc, 0)
            subscribe_packet = sol_test.create_subscribe(1, 'test', 0)
            conn.send(subscribe_packet)
            packet = conn.recv(100)
            mid, granted_qos = sol_test.read_suback(packet)
            self.assertEqual(mid, 1)
            self.assertEqual(granted_qos, 0)

    def test_subscribe_with_qos_one(self):
        with self.connection() as conn:
            connect_packet = sol_test.create_connect()
            conn.send(connect_packet)
            packet = conn.recv(100)
            connack, rc = sol_test.read_connack(packet)
            self.assertEqual(rc, 0)
            subscribe_packet = sol_test.create_subscribe(1, 'test', 0)
            conn.send(subscribe_packet)
            packet = conn.recv(100)
            mid, granted_qos = sol_test.read_suback(packet)
            self.assertEqual(mid, 1)
            self.assertEqual(granted_qos, 0)
