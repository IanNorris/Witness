"""Synthetic TCP peer exercises the built exported DLL; no camera access."""
import concurrent.futures
import hashlib
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import tempfile
import unittest

HEADER = struct.Struct('<IIIIHH')
MAGIC = 0x0ABCDEF0
MODERN = 0x6414
LEGACY = 0x6514
NONCE = 'synthetic-nonce'
XOR_KEY = bytes.fromhex('1f2d3c4b5a6978ff')


def xor(data, channel):
    return bytes(value ^ XOR_KEY[(channel + index) % 8] ^ channel
                 for index, value in enumerate(data))


def modern_hash(value):
    return hashlib.md5(value.encode()).hexdigest().upper()[:31]


def exact(peer, length):
    data = b''
    while len(data) < length:
        chunk = peer.recv(length - len(data))
        if not chunk:
            raise EOFError('peer closed')
        data += chunk
    return data


def read(peer):
    magic, command, length, identity, status, kind = HEADER.unpack(exact(peer, 20))
    if magic != MAGIC or length > 1024 * 1024:
        raise AssertionError('invalid header')
    if kind in (MODERN, 0, 0x6482):
        exact(peer, 4)
    return command, identity, exact(peer, length)


def reply(peer, command, identity, status=200, kind=MODERN, body=b''):
    packet = HEADER.pack(MAGIC, command, len(body), identity, status, kind)
    if kind in (MODERN, 0, 0x6482):
        packet += struct.pack('<I', 0)
    peer.sendall(packet + body)


class TransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.probe = Path(os.environ['WITNESS_BAICHUAN_PTZ_PROBE']).resolve(strict=True)
        cls.openssl = (cls.probe.parent / 'openssl.exe').resolve(strict=True)

    def aes(self, data, decrypt=False):
        key = modern_hash(NONCE + '-fixture')[:16].encode().hex()
        result = subprocess.run([str(self.openssl), 'enc', '-aes-128-cfb',
                                 '-d' if decrypt else '-e', '-nopad', '-nosalt',
                                 '-K', key, '-iv', b'0123456789abcdef'.hex()],
                                input=data, capture_output=True, timeout=3, check=True)
        return result.stdout

    def authenticate(self, peer, encrypted):
        command, identity, body = read(peer)
        self.assertEqual(command, 1)
        self.assertEqual(body, b'')
        nonce_xml = f'<?xml version="1.0"?><body><nonce>{NONCE}</nonce></body>'.encode()
        reply(peer, command, identity, 0xDD01 if encrypted else 0xDD00, LEGACY,
              xor(nonce_xml, identity & 255) if encrypted else b'')
        command, identity, body = read(peer)
        self.assertEqual(command, 1)
        login = xor(body, identity & 255)
        credential = modern_hash('fixture' + NONCE) if encrypted else 'fixture'
        self.assertIn(f'<userName>{credential}</userName>'.encode(), login)
        self.assertIn(f'<password>{credential}</password>'.encode(), login)
        reply(peer, command, identity)
        self.assertEqual(read(peer)[0], 31)

    def run_case(self, operation, mode, status=200, encrypted=False, tls_context=None):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(10)

            def serve():
                if tls_context is not None:
                    # Deliberately reject initial plain negotiation. The real
                    # client immediately upgrades this specific negotiation failure.
                    with listener.accept()[0] as plain:
                        plain.settimeout(3)
                        self.assertEqual(read(plain)[0], 1)
                raw = listener.accept()[0]
                with tls_context.wrap_socket(raw, server_side=True) if tls_context is not None else raw as peer:
                    peer.settimeout(5)
                    self.authenticate(peer, encrypted)
                    command, identity, body = read(peer)
                    if encrypted:
                        body = self.aes(body, decrypt=True)
                    self.assertEqual(command, 19 if operation == 'preset' else 18)
                    self.assertEqual(identity & 255, 3)
                    self.assertIn(b'<channelId>2</channelId>', body)
                    self.assertIn(b'<command>toPos</command>' if operation == 'preset' else
                                  b'<command>Left</command>' if operation == 'move' else b'<command>Stop</command>', body)
                    if mode == 'mismatch':
                        reply(peer, command, identity ^ 1)
                        reply(peer, command, identity ^ 256)
                        reply(peer, command, identity ^ 65536)
                        reply(peer, 19 if command == 18 else 18, identity)
                    elif mode == 'legacy':
                        reply(peer, command, identity, 200, LEGACY)
                    elif mode == 'disconnect':
                        return
                    elif mode == 'reply':
                        # Reader must consume nonempty bodies too, then ignore duplicates.
                        reply(peer, command, identity, status, body=self.aes(b'ack') if encrypted else b'ack')
                    elif mode == 'event':
                        event = b'<?xml version="1.0"?><body><AlarmEvent><status>MD</status><AItype>people</AItype></AlarmEvent></body>'
                        reply(peer, 33, 251, 200 if encrypted else 0xDD00,
                              MODERN if encrypted else LEGACY, self.aes(event) if encrypted else event)
                        reply(peer, command, identity, status)
                    elif mode == 'header-only':
                        reply(peer, command, identity, status)
                    # Stay connected until probe shutdown; never accidentally ACK timeouts.
                    try:
                        while peer.recv(1024):
                            pass
                    except ConnectionResetError:
                        pass

            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                server = pool.submit(serve)
                result = subprocess.run([str(self.probe), str(listener.getsockname()[1]), operation],
                                        capture_output=True, text=True, timeout=15)
                server.result(timeout=6)
            line = next((line for line in result.stdout.splitlines() if line.startswith('RESULT ')), '')
            self.assertTrue(line, f'exit={result.returncode:#x}\n' + result.stdout + result.stderr)
            _, accepted, elapsed = line.split()
            self.assertEqual(result.returncode, 0 if accepted == '1' else 3)
            return accepted == '1', int(elapsed)

    def test_acknowledged_commands(self):
        for operation in ('move', 'stop', 'preset'):
            for status in (200, 201, 300):
                with self.subTest(operation=operation, status=status):
                    self.assertTrue(self.run_case(operation, 'header-only', status)[0])
        self.assertTrue(self.run_case('stop', 'reply')[0])

    def test_rejections(self):
        for status in (0, 400, 401):
            with self.subTest(status=status):
                self.assertFalse(self.run_case('stop', 'header-only', status)[0])

    def test_interleaved_alarm_event(self):
        self.assertTrue(self.run_case('event', 'event')[0])

    def test_timeout_and_mismatches(self):
        for mode in ('silent', 'mismatch', 'legacy'):
            with self.subTest(mode=mode):
                accepted, elapsed = self.run_case('stop', mode)
                self.assertFalse(accepted)
                self.assertGreaterEqual(elapsed, 900)
                self.assertLess(elapsed, 1800)

    def test_disconnect_and_stop_cancel(self):
        for operation, mode in (('stop', 'disconnect'), ('stop-during-wait', 'silent')):
            with self.subTest(operation=operation):
                accepted, elapsed = self.run_case(operation, mode)
                self.assertFalse(accepted)
                self.assertLess(elapsed, 700)

    def test_nonce_derived_aes(self):
        for operation in ('move', 'stop', 'preset'):
            with self.subTest(operation=operation):
                self.assertTrue(self.run_case(operation, 'reply', encrypted=True)[0])
        self.assertFalse(self.run_case('stop', 'header-only', 400, encrypted=True)[0])
        self.assertTrue(self.run_case('event', 'event', encrypted=True)[0])
        accepted, elapsed = self.run_case('stop', 'silent', encrypted=True)
        self.assertFalse(accepted)
        self.assertGreaterEqual(elapsed, 900)

    def test_stale_reply_after_reconnect(self):
        for status in (200, 400):
            with self.subTest(status=status), socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                listener.listen(1)
                listener.settimeout(10)

                def serve():
                    with listener.accept()[0] as peer:
                        peer.settimeout(5)
                        self.authenticate(peer, True)
                        command, old_identity, _ = read(peer)
                        self.assertEqual(command, 18)
                        # Close without acknowledgement, cancelling the first ticket.
                    with listener.accept()[0] as peer:
                        peer.settimeout(5)
                        encrypted = status == 200
                        self.authenticate(peer, encrypted)
                        command, identity, body = read(peer)
                        self.assertNotEqual(identity, old_identity)
                        self.assertIn(b'<command>Stop</command>', self.aes(body, decrypt=True) if encrypted else body)
                        # Old reply must neither accept nor reject this request.
                        reply(peer, command, old_identity, 400 if status == 200 else 200)
                        reply(peer, command, identity, status)
                        try:
                            while peer.recv(1024):
                                pass
                        except ConnectionResetError:
                            pass

                with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                    server = pool.submit(serve)
                    result = subprocess.run([str(self.probe), str(listener.getsockname()[1]), 'reconnect'],
                                            capture_output=True, text=True, timeout=15)
                    server.result(timeout=10)
                self.assertIn(f'RESULT {int(status == 200)} ', result.stdout, result.stdout + result.stderr)
                self.assertEqual(result.returncode, 0 if status == 200 else 3)

    def test_tls_acknowledgement(self):
        # Ephemeral synthetic self-signed certificate, never a camera credential.
        with tempfile.TemporaryDirectory(prefix='witness-baichuan-tls-') as directory:
            certificate = Path(directory) / 'certificate.pem'
            key = Path(directory) / 'key.pem'
            subprocess.run([str(self.openssl), 'req', '-x509', '-newkey', 'rsa:2048',
                            '-config', str(self.openssl.parent / 'openssl.cnf'),
                            '-nodes', '-days', '1', '-subj', '/CN=loopback-fixture',
                            '-keyout', str(key), '-out', str(certificate)],
                           capture_output=True, timeout=15, check=True)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(certificate, key)
            self.assertTrue(self.run_case('event', 'event', encrypted=True, tls_context=context)[0])
            accepted, elapsed = self.run_case('stop-during-wait', 'silent', encrypted=True, tls_context=context)
            self.assertFalse(accepted)
            self.assertLess(elapsed, 700)
            self.assertFalse(self.run_case('stop', 'disconnect', encrypted=True, tls_context=context)[0])


if __name__ == '__main__':
    unittest.main()
