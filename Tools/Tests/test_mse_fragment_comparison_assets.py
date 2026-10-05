import hashlib
from pathlib import Path
import tempfile
import unittest

from serve_mse_fragment_comparison import freeze_assets


class ComparisonAssetsTests(unittest.TestCase):
    def test_freezes_exact_hashed_bytes_and_rejects_changed_fixture(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'fixture.mp4'
            path.write_bytes(b'original fixture')
            assets = {'/adts.mp4': (path, 'video/mp4')}
            hashes = {'/adts.mp4': hashlib.sha256(b'original fixture').hexdigest()}
            frozen = freeze_assets(assets, hashes)
            path.write_bytes(b'changed fixture')
            self.assertEqual(frozen['/adts.mp4'], (b'original fixture', 'video/mp4'))
            with self.assertRaisesRegex(ValueError, 'Fixture changed'):
                freeze_assets(assets, hashes)

    def test_missing_or_oversized_asset_fails_before_serving(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'fixture.mp4'
            with self.assertRaises(ValueError):
                freeze_assets({'/raw.mp4': (path, 'video/mp4')}, {})
            with path.open('wb') as output:
                output.seek(16 * 1024**2)
                output.write(b'x')
            with self.assertRaises(ValueError):
                freeze_assets({'/raw.mp4': (path, 'video/mp4')}, {})


if __name__ == '__main__':
    unittest.main()
