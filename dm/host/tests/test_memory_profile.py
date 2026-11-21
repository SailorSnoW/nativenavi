"""The dispatcher's memory diagnostics: the heap probe's log line and the alias size override."""
import os
import subprocess
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


class MemoryProfileTests(unittest.TestCase):
    def test_dispatcher_logs(self):
        result = subprocess.run([str(HERE / os.environ.get('DM_TEST_BUILD', 'build') / 't_svc_memory'), '--heap-probe'],
                                capture_output=True, text=True, check=True)
        self.assertIn('[mem] SetHeapSize 0x1000000 > app pool 0x1000000 less used memory 0x200000', result.stderr)

    def test_alias_size_override(self):   # the probe asserts the 256 MB alias region and the layout around it
        subprocess.run([str(HERE / os.environ.get('DM_TEST_BUILD', 'build') / 't_svc_memory'), '--alias-probe'],
                       env=dict(os.environ, DM_ALIAS_SIZE='0x10000000'), capture_output=True, check=True)


if __name__ == '__main__':
    unittest.main()
