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
        self.db.execute(re.search(r'CREATE TABLE IF NOT EXISTS Clip\((.*?)\);', source, re.S).group(0))
        for column in ('Lighting', 'Reviewed', 'RecordingFinalized', 'AudioDetectionVersion'):
            migration = re.search(r'ALTER TABLE Clip ADD COLUMN ' + column + r'[^;]*;', source).group(0)
            self.db.execute(migration)
        rows = [(1, 0, 0, 0), (2, 0, None, None), (3, None, 0, 0),
                (4, -1, None, None), (5, 12, 0, 0), (6, 12, None, None),
                (7, 12, 16, 1), (8, 12, 17, 1)]
        self.db.executemany('INSERT INTO Clip (ClipUID,Timestamp,Camera,DetectionVersion,RecordMode,Tags,Duration,Lighting) '
                            'VALUES (?,100,1,?,0,\'pir\',?,?)',
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

    def test_explicit_state_distinguishes_short_completed_active_and_failed_clips(self):
        self.db.executemany("INSERT INTO Clip (ClipUID,Timestamp,Camera,DetectionVersion,RecordMode,Tags,Duration,Lighting,RecordingFinalized) "
                            "VALUES (?,100,1,0,0,'pir',?,0,?)",
                            [(9, 0, 1), (10, 12, 0), (11, 12, -1)])
        self.assertEqual({row[0] for row in self.query('SelectReprocessQueue')}, {5, 6, 9})
        self.assertEqual({row[0] for row in self.query('SelectClipsNeedingLighting')}, {5, 6, 9})
        self.assertEqual(self.query('CountClipsToReprocess'), [(3,)])
        self.assertEqual(self.db.execute('SELECT Tags FROM Clip WHERE ClipUID=11').fetchone(), ('pir',))

    def test_production_create_update_and_migration_preserve_finalization_semantics(self):
        values = dict(Timestamp=200, Camera=1, MotionTimestamp=0, ActiveDuration=0,
                      Duration=12, RecordMode=0, MaxMotion=0, Description='', Save=0, Tags='pir')
        self.db.execute(self.queries['CreateClip'], values)
        uid = self.db.execute('SELECT ClipUID FROM Clip WHERE Timestamp=200').fetchone()[0]
        self.assertEqual(self.db.execute('SELECT RecordingFinalized FROM Clip WHERE ClipUID=?', (uid,)).fetchone(), (0,))
        self.assertNotIn(uid, {row[0] for row in self.query('SelectReprocessQueue')})
        values.update(Duration=0, RecordingFinalized=1)
        self.db.execute(self.queries['UpdateClip'], values)
        self.assertIn(uid, {row[0] for row in self.query('SelectReprocessQueue')})
        values.update(Duration=12, RecordingFinalized=-1)
        self.db.execute(self.queries['UpdateClip'], values)
        self.assertNotIn(uid, {row[0] for row in self.query('SelectReprocessQueue')})
        self.assertEqual(self.db.execute('SELECT Tags FROM Clip WHERE ClipUID=?', (uid,)).fetchone(), ('pir',))
        columns = [row[1] for row in self.db.execute('PRAGMA table_info(Clip)')]
        self.assertEqual(columns[11:14], ['DetectionVersion', 'Lighting', 'Reviewed'])

    def test_audio_queue_uses_same_finalization_state(self):
        self.db.execute('CREATE TABLE Camera (CameraUID INTEGER, AudioIntelligenceEnabled INTEGER, AudioConfidence REAL)')
        self.db.execute('INSERT INTO Camera VALUES (1,1,0.5)')
        self.db.execute('UPDATE Clip SET AudioDetectionVersion=16')
        self.db.executemany("INSERT INTO Clip (ClipUID,Timestamp,Camera,RecordMode,Duration,RecordingFinalized,AudioDetectionVersion) "
                            "VALUES (?,100,1,0,?,?,0)", [(9, 0, 1), (10, 12, 0), (11, 12, -1)])
        result = self.db.execute(self.queries['SelectClipForAudioIntelligence'], {'AudioDetectionVersion': 16}).fetchall()
        self.assertEqual([row[0] for row in result], [9])

    def test_failed_initialization_uses_cleanup_and_terminal_publication_path(self):
        source = (ROOT / 'WitnessServer/Source/CameraWorker.cpp').read_text(encoding='utf8')
        failure = source.split('LOG_ERROR("Recording init failed for camera', 1)[1].split('}', 1)[0]
        self.assertIn('OnClipFinished(false);', failure)
        self.assertNotIn('RecordStream.reset();', failure)

    def test_finalization_precedes_publication_static_invariant(self):
        source = (ROOT / 'WitnessServer/Source/CameraWorker.cpp').read_text(encoding='utf8')
        body = source.split('void CameraWorker::OnClipFinished(bool ManualStop)', 1)[1]
        self.assertLess(body.index('RecordStream->CloseFile()'), body.index('MessageBusObject->SendToClient'))
        self.assertLess(body.index('RecordStream.reset()'), body.index('MessageBusObject->SendToClient'))


if __name__ == '__main__':
    unittest.main()
