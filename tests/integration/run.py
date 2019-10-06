#!/usr/bin/env python

import unittest


if __name__ == '__main__':
    tests = unittest.TestLoader().discover(
        'tests/integration/',
        pattern='test*.py'
    )
    unittest.TextTestRunner(verbosity=2).run(tests)
