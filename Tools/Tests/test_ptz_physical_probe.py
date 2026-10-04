"""No camera access: validate manual confirmation, scope, privacy and read-only DB."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('physical', Path(__file__).with_name('run_ptz_physical_probe.py'))
physical = importlib.util.module_from_spec(spec)
spec.loader.exec_module(physical)


class PhysicalProbeTests(unittest.TestCase):
    def test_confirmation_and_camera_scope_before_database_access(self):
        for tail in (['--camera', '10'], ['--camera', '11', '--confirm-physical-movement']):
            with self.subTest(tail=tail), patch('sys.argv', ['probe', '--database', 'missing.db', '--probe', 'probe.exe'] + tail), \
                    patch.object(physical.sqlite3, 'connect') as connect, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit): physical.main()
                connect.assert_not_called()

    def test_readonly_config_credentials_and_failure_output(self):
        with tempfile.TemporaryDirectory(prefix='witness-physical-probe-') as directory:
            database = Path(directory) / 'test.db'
            executable = Path(directory) / 'probe.exe'
            executable.touch()
            with contextlib.closing(sqlite3.connect(database)) as db:
                db.executescript('CREATE TABLE Camera(CameraUID,LinkedCameraId,PtzEnabled,PtzApiHost,PtzApiPort,PtzUsername,PtzPassword,CameraString);')
                db.execute('INSERT INTO Camera VALUES(?,?,?,?,?,?,?,?)', (10, 0, 1, '', 443, '', '', 'rtsp://user:p%40ss@camera.invalid:554/main'))
                db.commit()
            original = database.read_bytes()
            def fake_run(argv, **kwargs):
                self.assertEqual(argv, [str(executable.resolve())])
                self.assertEqual(kwargs['env']['WITNESS_PTZ_PHYSICAL_CONFIRM'], '10')
                self.assertEqual(kwargs['env']['WITNESS_PTZ_PROBE_HOST'], 'camera.invalid')
                self.assertEqual(kwargs['env']['WITNESS_PTZ_PROBE_PASSWORD'], 'p@ss')
                self.assertNotIn('timeout', kwargs)
                return subprocess.CompletedProcess(argv, 5, 'secret=p@ss\npreflightHttpStop=1 preflightBaichuanStop=1 moveAck=1 baichuanStopAck=0 httpFallbackStopAck=0 stopDispatchMs=250 stopAckMs=9000\n', '')
            argv = ['probe', '--database', str(database), '--probe', str(executable), '--camera', '10', '--confirm-physical-movement']
            output = io.StringIO()
            with patch('sys.argv', argv), patch.dict(os.environ, {}, clear=True), patch.object(physical.subprocess, 'run', fake_run), contextlib.redirect_stdout(output):
                self.assertEqual(physical.main(), 5)
            self.assertNotIn('p@ss', output.getvalue())
            self.assertNotIn('camera.invalid', output.getvalue())
            self.assertIn('camera=10 probeExit=5', output.getvalue())
            self.assertEqual(database.read_bytes(), original)
            with contextlib.closing(sqlite3.connect(database)) as db:
                db.execute('UPDATE Camera SET LinkedCameraId=12'); db.commit()
            # A companion link must not redirect the explicitly approved endpoint.
            with patch('sys.argv', argv), patch.object(physical.subprocess, 'run', fake_run), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(physical.main(), 5)
            with contextlib.closing(sqlite3.connect(database)) as db:
                db.execute('UPDATE Camera SET PtzEnabled=0'); db.commit()
            with patch('sys.argv', argv), patch.object(physical.subprocess, 'run') as run, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit): physical.main()
                run.assert_not_called()


if __name__ == '__main__': unittest.main()
