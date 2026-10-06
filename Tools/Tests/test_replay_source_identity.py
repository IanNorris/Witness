import copy
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import replay_source_identity as identity


class ReplaySourceIdentityTests(unittest.TestCase):
    def test_entire_file_and_repeated_source_mapping_are_preserved_without_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'private-name.mp4'
            source.write_bytes(b'whole media contents')
            rows = identity.fingerprint_inputs([source, source])
            self.assertEqual(rows, [{'cameraId': i, 'bytes': 20,
                'sha256': hashlib.sha256(b'whole media contents').hexdigest()} for i in (1, 2)])
            report = {'scope': identity.SCOPE, 'before': rows, 'after': copy.deepcopy(rows), 'unchanged': True}
            self.assertEqual(identity.validate_identity(report, [1, 2]), rows)
            self.assertNotIn(str(source), str(report))

    def test_changed_input_fails_even_when_caller_claims_unchanged(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'media.mp4'
            source.write_bytes(b'original')
            before = identity.fingerprint_inputs([source])
            source.write_bytes(b'different')
            after = identity.fingerprint_inputs([source])
            with self.assertRaises(ValueError):
                identity.validate_identity({'scope': identity.SCOPE, 'before': before,
                    'after': after, 'unchanged': True}, [1])

    def test_bounds_and_nonfiles_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'media.mp4'
            source.write_bytes(b'oversize')
            with patch.object(identity, 'MAXIMUM_BYTES', 4), self.assertRaises(ValueError):
                identity.fingerprint_inputs([source])
            source.write_bytes(b'')
            with self.assertRaises(ValueError): identity.fingerprint_inputs([source])
            with self.assertRaises(ValueError): identity.fingerprint_inputs([Path(directory)])
            for inputs in ([], [source] * 17):
                with self.assertRaises(ValueError): identity.fingerprint_inputs(inputs)

    def test_fingerprints_must_cover_exact_camera_membership_and_have_typed_fields(self):
        row = {'cameraId': 1, 'bytes': 1000, 'sha256': 'a' * 64}
        for change in (lambda r: r.update(cameraId=True), lambda r: r.update(cameraId=2),
                       lambda r: r.update(bytes=True), lambda r: r.update(bytes=0),
                       lambda r: r.update(sha256='a'), lambda r: r.update(path='private')):
            changed = dict(row)
            change(changed)
            with self.assertRaises(ValueError):
                identity.validate_identity({'scope': identity.SCOPE, 'before': [changed],
                    'after': [dict(changed)], 'unchanged': True}, [1])
        with self.assertRaises(ValueError):
            identity.validate_identity({'scope': identity.SCOPE, 'before': [row, row],
                'after': [row, row], 'unchanged': True}, [1, 2])
        for report in (None, {}, {'scope': 'unverified'}):
            with self.assertRaises(ValueError): identity.validate_identity(report, [1])
        valid = {'scope': identity.SCOPE, 'before': [row], 'after': [dict(row)], 'unchanged': True}
        for camera_ids in ([], [True], [1.0], [1, 1]):
            with self.assertRaises(ValueError): identity.validate_identity(valid, camera_ids)


if __name__ == '__main__':
    unittest.main()
