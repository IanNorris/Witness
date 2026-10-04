"""Exercise production policy with real CMake, never real package installation."""
import os
import json
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

    def test_executable_pin_validation(self):
        expected = '1' * 40
        result = self.script(f'witness_validate_vcpkg_version("vcpkg version {expected}" "{expected}")')
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.script(f'witness_validate_vcpkg_version("vcpkg version {"2" * 40}" "{expected}")')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('executable does not match', result.stderr)

    def test_independent_checkout_and_tool_pins(self):
        environment = {'schema': 2, 'vcpkgCheckoutCommit': '1' * 40,
                       'vcpkgToolCommit': '2' * 40}
        body = f'''witness_read_vcpkg_pins([=[{json.dumps(environment)}]=] checkout tool)
if(NOT checkout STREQUAL "{'1' * 40}" OR NOT tool STREQUAL "{'2' * 40}")
    message(FATAL_ERROR "Independent pins were conflated")
endif()
witness_validate_vcpkg_version("version {'2' * 40}" "${{tool}}")
'''
        result = self.script(body)
        self.assertEqual(result.returncode, 0, result.stderr)
        for invalid in ({'schema': 1, 'vcpkgCommit': '1' * 40},
                        {**environment, 'vcpkgCheckoutCommit': '1' * 39},
                        {**environment, 'vcpkgToolCommit': 'A' * 40},
                        {**environment, 'vcpkgToolCommit': None},
                        {'schema': 2, 'vcpkgCheckoutCommit': '1' * 40}):
            with self.subTest(environment=invalid):
                result = self.script(f'witness_read_vcpkg_pins([=[{json.dumps(invalid)}]=] checkout tool)')
                self.assertNotEqual(result.returncode, 0, result.stdout)

    @unittest.skipUnless(os.environ.get('WITNESS_TEST_VCPKG_ROOT'),
                         'Set WITNESS_TEST_VCPKG_ROOT for read-only real installation validation')
    def test_real_pinned_checkout_and_executable_without_installation(self):
        environment = MODULE.parent.parent / 'build-environment.json'
        root = Path(os.environ['WITNESS_TEST_VCPKG_ROOT']).resolve().as_posix()
        result = self.script(f'''
file(READ "{environment.as_posix()}" environment)
witness_read_vcpkg_pins("${{environment}}" checkout tool)
witness_require_vcpkg_checkout("{root}" "${{checkout}}")
witness_require_vcpkg_version("{root}" "${{tool}}" version)
''')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

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

    def checkout(self):
        root = self.root / 'vcpkg'
        root.mkdir()
        commands = [['init'], ['config', 'user.name', 'Witness Policy Fixture'],
                    ['config', 'user.email', 'fixture@example.invalid']]
        for command in commands:
            subprocess.run(['git', '-C', str(root), *command], capture_output=True, check=True)
        (root / 'toolchain.cmake').write_text('# pinned toolchain\n')
        subprocess.run(['git', '-C', str(root), 'add', 'toolchain.cmake'], capture_output=True, check=True)
        subprocess.run(['git', '-C', str(root), '-c', 'commit.gpgsign=false', 'commit', '-m', 'fixture'], capture_output=True, check=True)
        head = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
        return root, head

    @unittest.skipUnless(shutil.which('git'), 'Git required')
    def test_checkout_identity_and_tracked_state(self):
        root, head = self.checkout()
        command = f'witness_require_vcpkg_checkout("{root.as_posix()}" "{head}")'
        self.assertEqual(self.script(command).returncode, 0)
        (root / 'downloads').mkdir() # untracked package/cache files are allowed
        self.assertEqual(self.script(command).returncode, 0)
        wrong = self.script(f'witness_require_vcpkg_checkout("{root.as_posix()}" "{"0" * 40}")')
        self.assertNotEqual(wrong.returncode, 0)
        self.assertIn('checkout does not match', wrong.stderr)
        child = root / 'downloads'
        nested = self.script(f'witness_require_vcpkg_checkout("{child.as_posix()}" "{head}")')
        self.assertNotEqual(nested.returncode, 0) # parent-repository discovery is not identity
        (root / 'toolchain.cmake').write_text('# modified scripts\n')
        changed = self.script(command)
        self.assertNotEqual(changed.returncode, 0)
        self.assertIn('tracked changes', changed.stderr)

    @unittest.skipUnless(shutil.which('git'), 'Git required')
    def test_wrong_checkout_rejected_before_toolchain_with_bootstrap_requested(self):
        root, head = self.checkout()
        build, configure = self.project()
        source = self.root / 'source' / 'CMakeLists.txt'
        source.write_text(source.read_text().replace('project(PolicyFixture LANGUAGES NONE)',
            f'witness_require_vcpkg_checkout("{root.as_posix()}" "{"0" * 40}")\nproject(PolicyFixture LANGUAGES NONE)'))
        result = configure('-DWITNESS_BOOTSTRAP_DEPENDENCIES=ON')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('checkout does not match', result.stderr)
        self.assertFalse((self.root / 'install-intent.txt').exists())
        self.assertIn('WITNESS_BOOTSTRAP_DEPENDENCIES:BOOL=OFF', (build / 'CMakeCache.txt').read_text())


if __name__ == '__main__':
    unittest.main()
