"""Synthetic TCP peer exercises the built exported DLL; no camera access."""
import concurrent.futures
import os
from pathlib import Path
import socket
import struct
import subprocess
import unittest

HEADER = struct.Struct('<IIIIHH')
MAGIC = 0x0ABCDEF0
MODERN = 0x6414
LEGACY = 0x6514


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

    def run_case(self, operation, mode, status=200):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(5)

            def serve():
                with listener.accept()[0] as peer:
                    peer.settimeout(5)
                    command, identity, body = read(peer)
                    self.assertEqual(command, 1)
                    self.assertEqual(body, b'')
                    # Empty nonce exercises the supported no-AES negotiation path.
                    reply(peer, command, identity, 0xDD00, LEGACY)
                    command, identity, body = read(peer)
                    self.assertEqual(command, 1)
                    self.assertTrue(body)
                    reply(peer, command, identity)
                    self.assertEqual(read(peer)[0], 31)
                    command, identity, body = read(peer)
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
                        reply(peer, command, identity, status, body=b'ack')
                    elif mode == 'event':
                        event = b'<?xml version="1.0"?><body><AlarmEvent><status>MD</status><AItype>people</AItype></AlarmEvent></body>'
                        reply(peer, 33, 251, 0xDD00, LEGACY, event)
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
                                        capture_output=True, text=True, timeout=8)
                server.result(timeout=6)
            line = next((line for line in result.stdout.splitlines() if line.startswith('RESULT ')), '')
            self.assertTrue(line, result.stdout + result.stderr)
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


if __name__ == '__main__':
    unittest.main()
