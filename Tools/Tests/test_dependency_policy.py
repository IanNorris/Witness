"""Exercise production policy with real CMake, never real package installation."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

MODULE = Path(__file__).parents[1] / 'WitnessDependencyPolicy.cmake'
CMAKE = os.environ.get('WITNESS_CMAKE') or shutil.which('cmake')


@unittest.skipUnless(CMAKE and shutil.which('ninja'), 'CMake and Ninja required')
class DependencyPolicyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='witness-policy-')
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def script(self, body):
        script = self.root / 'check.cmake'
        script.write_text(f'cmake_minimum_required(VERSION 3.20)\ninclude("{MODULE.as_posix()}")\n' + body)
        return subprocess.run([CMAKE, '-P', str(script)], capture_output=True, text=True)

    def project(self):
        source = self.root / 'source'
        source.mkdir()
        (source / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.20)
option(WITNESS_MANAGED_DEPENDENCIES "managed" ON)
option(WITNESS_BOOTSTRAP_DEPENDENCIES "bootstrap" OFF)
include("{MODULE.as_posix()}")
witness_prepare_managed_dependencies()
project(PolicyFixture LANGUAGES NONE)
witness_finish_managed_dependency_configuration()
''')
        toolchain = self.root / 'toolchain.cmake'
        toolchain.write_text('''
if(VCPKG_MANIFEST_INSTALL)
    file(APPEND "${CMAKE_CURRENT_LIST_DIR}/install-intent.txt" "install\\n")
    if(FAIL_INSTALL)
        message(FATAL_ERROR "Synthetic install failure")
    endif()
endif()
''')
        build = self.root / 'build'
        def configure(*extra):
            return subprocess.run([CMAKE, '-S', str(source), '-B', str(build), '-G', 'Ninja',
                                   f'-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}', *extra],
                                  capture_output=True, text=True)
        return build, configure

    def test_successful_bootstrap_is_not_permission_for_ordinary_retry(self):
        build, configure = self.project()
        first = configure('-DWITNESS_BOOTSTRAP_DEPENDENCIES=ON')
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        marker = self.root / 'install-intent.txt'
        before = marker.read_bytes()
        self.assertTrue(before)
        cache = (build / 'CMakeCache.txt').read_text()
        self.assertIn('WITNESS_BOOTSTRAP_DEPENDENCIES:BOOL=OFF', cache)
        self.assertIn('VCPKG_MANIFEST_INSTALL:BOOL=OFF', cache)
        second = configure()
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(marker.read_bytes(), before)

    def test_failed_bootstrap_is_not_permission_for_ordinary_retry(self):
        build, configure = self.project()
        first = configure('-DWITNESS_BOOTSTRAP_DEPENDENCIES=ON', '-DFAIL_INSTALL=ON')
        self.assertNotEqual(first.returncode, 0)
        self.assertIn('Synthetic install failure', first.stderr)
        marker = self.root / 'install-intent.txt'
        before = marker.read_bytes()
        self.assertIn('WITNESS_BOOTSTRAP_DEPENDENCIES:BOOL=OFF', (build / 'CMakeCache.txt').read_text())
        # FAIL_INSTALL remains ON. Only a new install attempt would fail again.
        second = configure()
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        self.assertEqual(marker.read_bytes(), before)

    def test_normal_configure_overrides_stale_manifest_install_cache(self):
        build, configure = self.project()
        result = configure('-DVCPKG_MANIFEST_INSTALL=ON')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((self.root / 'install-intent.txt').exists())
        self.assertIn('VCPKG_MANIFEST_INSTALL:BOOL=OFF', (build / 'CMakeCache.txt').read_text())

    def test_compiler_family_servicing_and_exact_boundaries(self):
        for actual in ('19.51', '19.51.36257.0', '19.51.99999.0'):
            result = self.script(f'witness_require_msvc_family("{actual}" "19.51")')
            self.assertEqual(result.returncode, 0, result.stderr)
        for actual in ('19x51.0', '19.510.0', '19.50.0', '19.52.0', '19.51.invalid'):
            result = self.script(f'witness_require_msvc_family("{actual}" "19.51")')
            self.assertNotEqual(result.returncode, 0, actual)
            self.assertIn('outside the pinned', result.stderr)

    def test_invalid_configured_family_is_rejected(self):
        result = self.script('witness_require_msvc_family("19.51.0" "19.51.*")')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Invalid pinned', result.stderr)

    def test_stamp_matching_missing_mismatch_and_duplicate(self):
        stamp = self.root / 'stamp.txt'
        check = f'witness_require_dependency_stamp("{stamp.as_posix()}" "expected" "fixture-triplet")'
        self.assertNotEqual(self.script(check).returncode, 0)
        for content in ('signature=expected\ntriplet=fixture-triplet\n', 'signature=expected\r\ntriplet=fixture-triplet\r\ncompiler=changed-servicing\r\n'):
            stamp.write_bytes(content.encode())
            self.assertEqual(self.script(check).returncode, 0)
        for content in ('signature=wrong\ntriplet=fixture-triplet\n',
                        'signature=expected\nsignature=expected\ntriplet=fixture-triplet\n',
                        'signature=expected\ntriplet=another-triplet\n', 'signature=expected\n'):
            stamp.write_text(content)
            result = self.script(check)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('environment changed', result.stderr)

    def test_logical_text_fingerprint_ignores_line_endings_not_content(self):
        for name, content in [('a', b'a\nb\n'), ('b', b'a\r\nb\r\n'), ('c', b'a\nc\n')]:
            (self.root / name).write_bytes(content)
        body = '\n'.join(f'witness_normalized_text_sha256({name} "{(self.root / name).as_posix()}")' for name in 'abc')
        result = self.script(body + '\nif(NOT a STREQUAL b OR a STREQUAL c)\nmessage(FATAL_ERROR "text fingerprint mismatch")\nendif()')
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()
