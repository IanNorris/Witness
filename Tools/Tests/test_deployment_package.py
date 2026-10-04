import importlib.util
from pathlib import Path
import shutil
import tempfile
import unittest

MODULE_PATH = Path(__file__).resolve().parents[1] / 'verify_deployment_package.py'
SPEC = importlib.util.spec_from_file_location('package_verification', MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class DeploymentPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='witness-package-test-')
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.runtime, self.package = root / 'runtime', root / 'package'
        self.runtime.mkdir()
        self.native, self.web = '0123456789ab', 'abcdef012345'
        content = {'WitnessServer.exe': self.native, 'WitnessCamera.dll': self.native,
            'WitnessCommon.dll': 'common', 'openssl.exe': 'openssl', 'openssl.cnf': 'config',
            'Web/index.html': 'index', 'Web/build-hash.txt': self.web,
            'Web/assets/index.js': f'const build = "{self.web}"', 'models/sample.onnx': 'model'}
        for name, value in content.items():
            path = self.runtime / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(value, encoding='utf8')
        shutil.copytree(self.runtime, self.package)

    def verify(self):
        return MODULE.verify(self.runtime, self.package, self.native)

    def test_matching_package_has_hashes_and_independent_identities(self):
        result = self.verify()
        self.assertEqual(result['fileCount'], 9)
        self.assertEqual(result['nativeBuildId'], self.native)
        self.assertEqual(result['webBuildId'], self.web)
        self.assertEqual(len(result['sha256']), 9)

    def test_same_size_changed_file_is_rejected(self):
        (self.package / 'WitnessCommon.dll').write_text('broken', encoding='utf8')
        with self.assertRaisesRegex(ValueError, 'differs'):
            self.verify()

    def test_extra_database_or_probe_is_rejected(self):
        for name in ['server.db', 'LivePreviewProbe.exe']:
            with self.subTest(name=name):
                path = self.package / name
                path.write_text('not deployable', encoding='utf8')
                with self.assertRaisesRegex(ValueError, 'extra='):
                    self.verify()
                path.unlink()

    def test_missing_asset_is_rejected(self):
        (self.package / 'Web/assets/index.js').unlink()
        with self.assertRaisesRegex(ValueError, 'missing='):
            self.verify()

    def test_wrong_native_id_is_rejected_even_when_bytes_match(self):
        with self.assertRaisesRegex(ValueError, 'identity absent'):
            MODULE.verify(self.runtime, self.package, '111111111111')

    def test_web_identity_must_exist_in_compiled_assets(self):
        for root in [self.runtime, self.package]:
            (root / 'Web/assets/index.js').write_text('no identity', encoding='utf8')
        with self.assertRaisesRegex(ValueError, 'Web build identity absent'):
            self.verify()

    def test_private_database_is_rejected_even_when_in_both_trees(self):
        for root in [self.runtime, self.package]:
            (root / 'models/server.db').write_text('private', encoding='utf8')
        with self.assertRaisesRegex(ValueError, 'Private state'):
            self.verify()

    def test_optional_symbols_must_match(self):
        for root in [self.runtime, self.package]:
            (root / 'WitnessServer.pdb').write_text('symbols', encoding='utf8')
        self.assertEqual(self.verify()['fileCount'], 10)
        (self.package / 'WitnessServer.pdb').write_text('wrong', encoding='utf8')
        with self.assertRaisesRegex(ValueError, 'differs'):
            self.verify()

    def test_same_tree_and_invalid_identity_are_rejected(self):
        with self.assertRaisesRegex(ValueError, 'separate'):
            MODULE.verify(self.runtime, self.runtime, self.native)
        with self.assertRaisesRegex(ValueError, 'hexadecimal'):
            MODULE.verify(self.runtime, self.package, '../123456789')

    def test_required_common_dll_cannot_be_omitted_from_both_trees(self):
        for root in [self.runtime, self.package]:
            (root / 'WitnessCommon.dll').unlink()
        with self.assertRaisesRegex(ValueError, 'Required build output missing'):
            self.verify()

    def test_linked_package_directory_is_rejected(self):
        link = self.package / 'linked-assets'
        try:
            link.symlink_to(self.runtime / 'Web', target_is_directory=True)
        except OSError:
            self.skipTest('Creating symlinks unavailable on this Windows host')
        with self.assertRaisesRegex(ValueError, 'Linked package entry'):
            self.verify()


if __name__ == '__main__':
    unittest.main()
