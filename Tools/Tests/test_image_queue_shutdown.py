"""Opt-in negative control and fixed-DLL check for the same public-API executable."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


@unittest.skipUnless(sys.platform == 'win32' and os.environ.get('WITNESS_TEST_QUEUE_EXE'),
                     'Set WITNESS_TEST_QUEUE_EXE and the WITNESS_TEST_QUEUE_* identity/runtime variables')
class ImageQueueShutdownTests(unittest.TestCase):
    def test_same_executable_rejects_legacy_and_accepts_fixed_dll(self):
        executable = Path(os.environ['WITNESS_TEST_QUEUE_EXE']).resolve(strict=True)
        legacy = Path(os.environ['WITNESS_TEST_QUEUE_LEGACY_RUNTIME']).resolve(strict=True)
        fixed_id = os.environ['WITNESS_TEST_QUEUE_FIXED_ID']
        legacy_id = os.environ['WITNESS_TEST_QUEUE_LEGACY_ID']
        self.assertEqual(executable.name, 'ImageQueueShutdownTests.exe')
        self.assertRegex(fixed_id, r'^[0-9a-f]{12}$')
        self.assertRegex(legacy_id, r'^[0-9a-f]{12}$')
        self.assertNotEqual(fixed_id, legacy_id)
        self.assertIn(fixed_id.encode(), (executable.parent / 'WitnessCamera.dll').read_bytes())
        self.assertIn(legacy_id.encode(), (legacy / 'WitnessCamera.dll').read_bytes())
        artifacts = Path(__file__).resolve().parents[2] / 'artifacts'
        with tempfile.TemporaryDirectory(prefix='queue-legacy-', dir=artifacts) as directory:
            runtime = Path(directory)
            # Copy only runtime DLLs, never private configuration/databases or
            # mutate either the verified legacy package or current build output.
            for source in legacy.glob('*.dll'):
                self.assertFalse(source.is_symlink())
                shutil.copyfile(source, runtime / source.name)
            probe = runtime / executable.name
            shutil.copyfile(executable, probe)
            old = subprocess.run([str(probe)], cwd=runtime, stdin=subprocess.DEVNULL,
                                 capture_output=True, text=True, timeout=15,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
            self.assertEqual(old.returncode, 1, old.stdout + old.stderr)
            self.assertIn('Camera build=' + legacy_id, old.stdout)
            self.assertIn('late entry=0', old.stderr)
        fixed = subprocess.run([str(executable)], cwd=executable.parent,
                               stdin=subprocess.DEVNULL, capture_output=True,
                               text=True, timeout=15,
                               creationflags=subprocess.CREATE_NO_WINDOW)
        self.assertEqual(fixed.returncode, 0, fixed.stdout + fixed.stderr)
        self.assertIn('Camera build=' + fixed_id, fixed.stdout)
        self.assertIn('PASS: queue stop', fixed.stdout)
        print('Same executable: legacy loses stop request; fixed DLL passes', flush=True)


if __name__ == '__main__':
    unittest.main()
