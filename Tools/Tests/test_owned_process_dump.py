"""Windows smoke using only a new test-owned child and temporary output."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from owned_process_dump import capture_owned_process_dump


@unittest.skipUnless(os.name == 'nt', 'Windows minidumps')
class OwnedProcessDumpTests(unittest.TestCase):
    def test_owned_child_dump_and_exclusive_output(self):
        artifacts = Path(__file__).resolve().parents[2] / 'artifacts'
        with tempfile.TemporaryDirectory(prefix='owned-dump-', dir=artifacts) as directory:
            child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'],
                                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                output = Path(directory) / 'child.dmp'
                capture_owned_process_dump(child, output)
                with output.open('rb') as stream:
                    self.assertEqual(stream.read(4), b'MDMP')
                self.assertGreater(output.stat().st_size, 32)
                with self.assertRaises(FileExistsError):
                    capture_owned_process_dump(child, output)
                self.assertIsNone(child.poll())
            finally:
                if child.poll() is None:
                    child.kill()
                child.wait(timeout=5)
            with self.assertRaises(ValueError):
                capture_owned_process_dump(child, Path(directory) / 'dead.dmp')


if __name__ == '__main__':
    unittest.main()
