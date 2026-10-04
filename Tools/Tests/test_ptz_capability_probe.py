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

spec = importlib.util.spec_from_file_location('probe', Path(__file__).with_name('run_ptz_capability_probe.py'))
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class CapabilityProbeTests(unittest.TestCase):
    def test_linked_readonly_configuration_and_credential_safe_output(self):
        with tempfile.TemporaryDirectory(prefix='witness-ptz-probe-') as directory:
            database = Path(directory) / 'test.db'
            with contextlib.closing(sqlite3.connect(database)) as db:
                db.executescript('CREATE TABLE Camera(CameraUID,LinkedCameraId,PtzEnabled,PtzApiHost,PtzApiPort,PtzUsername,PtzPassword,CameraString);')
                db.execute('INSERT INTO Camera VALUES(?,?,?,?,?,?,?,?)', (10,0,1,'',443,'','','rtsp://user:p%40ss@camera.invalid:554/main'))
                db.execute('INSERT INTO Camera VALUES(?,?,?,?,?,?,?,?)', (11,10,0,'',0,'','',''))
                db.commit()
            original = database.read_bytes()
            def fake_run(argv, **kwargs):
                self.assertEqual(argv, [str(Path('probe.exe').resolve())])
                self.assertEqual(kwargs['env']['WITNESS_PTZ_PROBE_HOST'], 'camera.invalid')
                self.assertEqual(kwargs['env']['WITNESS_PTZ_PROBE_USERNAME'], 'user')
                self.assertEqual(kwargs['env']['WITNESS_PTZ_PROBE_PASSWORD'], 'p@ss')
                self.assertEqual(kwargs['env']['WITNESS_PTZ_PROBE_PORT'], '443')
                self.assertEqual(kwargs['timeout'], 100)
                return subprocess.CompletedProcess(argv,0,'secret=123\nhttps://user:p@ss@camera.invalid\nauthenticated=1 positionValid=0 zoomValid=1 zoomMin=0 zoomMax=33 zoomPosition=10 presetCount=2 enabledPresets=1 elapsedMs=200\n','')
            output = io.StringIO()
            with patch('sys.argv',['probe','--database',str(database),'--probe','probe.exe','--camera','11']), patch.dict(os.environ,{},clear=True), patch.object(probe.subprocess,'run',fake_run), contextlib.redirect_stdout(output):
                self.assertEqual(probe.main(),0)
            self.assertNotIn('secret',output.getvalue())
            self.assertNotIn('camera.invalid',output.getvalue())
            self.assertNotIn('p@ss',output.getvalue())
            self.assertIn('authenticated=1',output.getvalue())
            self.assertEqual(database.read_bytes(),original)


if __name__ == '__main__': unittest.main()
