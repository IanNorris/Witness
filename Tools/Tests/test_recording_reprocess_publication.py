"""Execute the production SQL against active and finalized recording rows."""
from pathlib import Path
import re
import sqlite3
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RecordingPublicationTests(unittest.TestCase):
    def setUp(self):
        source = (ROOT / 'WitnessServer/Source/Database.cpp').read_text(encoding='utf8')
        self.queries = dict(re.findall(r'std::string (\w+) = R"RAW\((.*?)\)RAW";', source, re.S))
        self.db = sqlite3.connect(':memory:')
        self.addCleanup(self.db.close)
        self.db.execute('CREATE TABLE Clip (ClipUID INTEGER, Timestamp INTEGER, Camera INTEGER, '
                        'DetectionVersion INTEGER, RecordMode INTEGER, Tags TEXT, Duration INTEGER, Lighting INTEGER)')
        rows = [(1, 0, 0, 0), (2, 0, None, None), (3, None, 0, 0),
                (4, -1, None, None), (5, 12, 0, 0), (6, 12, None, None),
                (7, 12, 16, 1), (8, 12, 17, 1)]
        self.db.executemany('INSERT INTO Clip VALUES (?,100,1,?,0,\'pir\',?,?)',
                            [(uid, version, duration, lighting) for uid, duration, version, lighting in rows])

    def query(self, name):
        return self.db.execute(self.queries[name], {'DetectionVersion': 16}).fetchall()

    def test_worker_selects_only_finalized_outdated_recordings(self):
        self.assertEqual([row[0] for row in self.query('SelectClipForReprocess')], [6])

    def test_count_and_queue_agree_and_do_not_include_active_null_version(self):
        self.assertEqual(self.query('CountClipsToReprocess'), [(2,)])
        self.assertEqual({row[0] for row in self.query('SelectReprocessQueue')}, {5, 6})

    def test_lighting_backfill_does_not_open_active_recordings(self):
        self.assertEqual({row[0] for row in self.query('SelectClipsNeedingLighting')}, {5, 6})

    def test_finalization_precedes_publication_static_invariant(self):
        source = (ROOT / 'WitnessServer/Source/CameraWorker.cpp').read_text(encoding='utf8')
        body = source.split('void CameraWorker::OnClipFinished(bool ManualStop)', 1)[1]
        self.assertLess(body.index('RecordStream->CloseFile()'), body.index('MessageBusObject->SendToClient'))
        self.assertLess(body.index('RecordStream.reset()'), body.index('MessageBusObject->SendToClient'))


if __name__ == '__main__':
    unittest.main()
