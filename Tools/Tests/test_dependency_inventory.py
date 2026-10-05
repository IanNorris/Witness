import importlib.util
from pathlib import Path
import tempfile
import unittest
import sys
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('inventory', Path(__file__).parents[1] / 'dependency_inventory.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class DependencyInventoryTests(unittest.TestCase):
    def test_linked_root_is_rejected_before_resolution(self):
        # Mock the platform link query so this regression also runs on Windows
        # without requiring the privilege to create a directory symlink.
        with patch.object(Path, 'is_symlink', return_value=True), \
                patch.object(Path, 'resolve') as resolve:
            with self.assertRaisesRegex(ValueError, 'linked root'):
                module.inventory(Path('linked-installed-root'))
            resolve.assert_not_called()

    def test_junction_root_is_rejected_before_resolution(self):
        with patch.object(Path, 'is_symlink', return_value=False), \
                patch.object(Path, 'is_junction', return_value=True, create=True), \
                patch.object(Path, 'resolve') as resolve:
            with self.assertRaisesRegex(ValueError, 'linked root'):
                module.inventory(Path('junction-installed-root'))
            resolve.assert_not_called()

    def test_capture_cli_does_not_resolve_away_the_link_check(self):
        with patch.object(sys, 'argv', ['inventory', 'capture', 'linked-installed-root', 'snapshot.json']), \
                patch.object(Path, 'is_symlink', return_value=True), \
                patch.object(Path, 'resolve') as resolve:
            with self.assertRaisesRegex(ValueError, 'linked root'):
                module.main()
            resolve.assert_not_called()

    def test_content_metadata_changes_additions_and_removals(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'package.dll').write_bytes(b'binary')
            (root / 'status').write_bytes(b'installed')
            before = module.inventory(root)
            self.assertFalse(any(module.differences(before, module.inventory(root)).values()))
            (root / 'package.dll').write_bytes(b'modify') # same length, changed content
            (root / 'status').unlink()
            (root / 'new.h').write_bytes(b'header')
            delta = module.differences(before, module.inventory(root))
            self.assertEqual(delta, {'added': ['new.h'], 'removed': ['status'], 'changed': ['package.dll']})

    def test_empty_or_invalid_inventory_cannot_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(ValueError):
                module.inventory(Path(temp))
        with self.assertRaises(ValueError):
            module.differences({'schema': 1, 'algorithm': 'sha256', 'files': {}}, {})

    def test_all_nested_headers_and_metadata_are_included(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / 'share' / 'package').mkdir(parents=True)
            (root / 'share' / 'package' / 'config.cmake').write_bytes(b'configuration')
            snapshot = module.inventory(root)
            self.assertEqual(list(snapshot['files']), ['share/package/config.cmake'])

    def test_identical_malformed_snapshots_cannot_claim_unchanged_content(self):
        for name, record in [('file', {'bytes': 1, 'sha256': 'invalid'}),
                             ('../escape', {'bytes': 1, 'sha256': 'a' * 64}),
                             ('file', {'bytes': -1, 'sha256': 'a' * 64})]:
            snapshot = {'schema': 1, 'algorithm': 'sha256', 'files': {name: record}}
            with self.assertRaises(ValueError):
                module.differences(snapshot, snapshot)


if __name__ == '__main__':
    unittest.main()
