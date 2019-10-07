#!/usr/bin/env python

import unittest
import sol_test


if __name__ == '__main__':
    broker = sol_test.start_broker()
    tests = unittest.TestLoader().discover(
        'tests/integration/',
        pattern='test*.py'
    )
    unittest.TextTestRunner(verbosity=2).run(tests)
    sol_test.kill_broker(broker)
