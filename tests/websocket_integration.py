"""Real TCP/WebSocket regression tests; Python standard library only."""
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time


class Client:
    def __init__(self, port):
        self.socket = socket.create_connection(('127.0.0.1', port), timeout=3)
        self.buffer = bytearray()
        key = base64.b64encode(os.urandom(16)).decode()
        self.socket.sendall((f'GET / HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n'
                             f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n'
                             'Sec-WebSocket-Version: 13\r\n\r\n').encode())
        while b'\r\n\r\n' not in self.buffer:
            self.buffer.extend(self.socket.recv(4096))
        header, rest = self.buffer.split(b'\r\n\r\n', 1)
        self.buffer = bytearray(rest)
        expected = base64.b64encode(hashlib.sha1(
            (key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest())
        assert b'101 Switching Protocols' in header and expected in header
        assert self.message()['type'] == 'device_list'

    def read(self, size):
        while len(self.buffer) < size:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise EOFError('WebSocket closed')
            self.buffer.extend(chunk)
        result = bytes(self.buffer[:size])
        del self.buffer[:size]
        return result

    def frame(self):
        first, second = self.read(2)
        assert first & 0x80 and not second & 0x80
        size = second & 127
        if size == 126:
            size = struct.unpack('!H', self.read(2))[0]
        elif size == 127:
            size = struct.unpack('!Q', self.read(8))[0]
        return first & 15, self.read(size)

    def message(self):
        opcode, payload = self.frame()
        assert opcode == 1
        return json.loads(payload)

    @staticmethod
    def encode(payload, opcode=1, final=True):
        if isinstance(payload, str):
            payload = payload.encode()
        mask = b'\x10\x20\x30\x40'
        header = bytes([(128 if final else 0) | opcode])
        header += (bytes([128 | len(payload)]) if len(payload) < 126
                   else bytes([128 | 126]) + struct.pack('!H', len(payload)))
        return header + mask + bytes(c ^ mask[i % 4] for i, c in enumerate(payload))

    def subscribe(self, mac, hz=0, fragmented=False):
        payload = json.dumps({'sample_hz': hz, 'mac': mac.lower(), 'type': 'subscribe'})
        if fragmented:
            # Split a command across continuation frames and TCP writes, with an
            # interleaved ping to exercise control-frame handling.
            data = (self.encode(payload[:20], final=False) + self.encode('ping', opcode=9)
                    + self.encode(payload[20:], opcode=0))
            for offset in range(0, len(data), 3):
                self.socket.sendall(data[offset:offset + 3])
            assert self.frame() == (10, b'ping')
        else:
            self.socket.sendall(self.encode(payload))
        response = self.message()
        assert response == {'type': 'subscribed', 'mac': mac.upper(), 'sample_hz': hz}, response

    def drain(self):
        messages = []
        while True:
            message = self.message()
            if message['type'] == 'barrier':
                return messages
            messages.append(message)

    def close(self):
        self.socket.close()


def main():
    a, b = 'AA:BB:CC:DD:EE:01', 'AA:BB:CC:DD:EE:02'
    with tempfile.TemporaryDirectory(prefix='iot-websocket-test-') as directory:
        process = subprocess.Popen([sys.argv[1], directory], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, text=True)
        clients = []
        total_samples = 0
        try:
            port = int(process.stdout.readline())
            clients = [Client(port) for _ in range(3)]
            first, second, observer = clients
            first.subscribe(a, fragmented=True)
            second.subscribe(b)

            def send(commands):
                process.stdin.write('\n'.join(commands + ['barrier']) + '\n')
                process.stdin.flush()
                return [client.drain() for client in clients]

            timestamp = int(time.time() * 1000)
            messages = send([f'sample {a} {timestamp} 1', f'sample {b} {timestamp} 2',
                             f'prediction {a}', f'prediction {b}', f'battery {b}'])
            total_samples += 2
            for group, mac in zip(messages[:2], [a, b]):
                streams = [m for m in group if m['type'] in ('sensor_data', 'prediction')]
                assert len(streams) == 2 and all(m['mac'] == mac for m in streams)
            assert all(m['type'] in ('device_activity', 'battery') for m in messages[2])
            assert {m['mac'] for m in messages[2] if m['type'] == 'device_activity'} == {a, b}
            assert any(m['type'] == 'battery' for m in messages[0])

            # Subscription changes take effect before the acknowledgement.
            first.subscribe(b)
            messages = send([f'sample {a} {timestamp+40} 3', f'sample {b} {timestamp+40} 4'])
            total_samples += 2
            assert [m['mac'] for m in messages[0] if m['type'] == 'sensor_data'] == [b]

            first.subscribe(a, 10)
            second.subscribe(a, 0)
            commands = [f'sample {a} {timestamp+1000+i*40} {i}' for i in range(25)]
            messages = send(commands + [f'prediction {a}'])
            total_samples += 25
            limited = [m for m in messages[0] if m['type'] == 'sensor_data']
            full = [m for m in messages[1] if m['type'] == 'sensor_data']
            assert len(limited) == 10, len(limited)
            assert len(full) == 25, len(full)
            assert any(m['type'] == 'prediction' for m in messages[0])
            assert not any(m['type'] in ('sensor_data', 'prediction') for m in messages[2])

            # Reject bad commands without changing the previous subscription.
            first.socket.sendall(Client.encode('{"type":"subscribe","mac":"bad"}'))
            assert first.message()['type'] == 'error'
            first.socket.sendall(Client.encode('{"type":"subscribe","mac":"","sample_hz":26}'))
            assert first.message()['type'] == 'error'
            first.subscribe('')
            messages = send([f'sample {a} {timestamp+3000} 100'])
            total_samples += 1
            assert not any(m['type'] == 'sensor_data' for m in messages[0])

            # Reconnect begins with no device stream, then accepts a new subscription.
            first.close()
            clients[0] = first = Client(port)
            messages = send([f'sample {a} {timestamp+3040} 101'])
            total_samples += 1
            assert not any(m['type'] == 'sensor_data' for m in messages[0])
            first.subscribe(a)
            messages = send([f'sample {a} {timestamp+3080} 102'])
            total_samples += 1
            assert len([m for m in messages[0] if m['type'] == 'sensor_data']) == 1

            # 30-device fan-out: one browser still only receives its selected stream.
            devices = [a] + [f'00:00:00:00:00:{i:02X}' for i in range(29)]
            messages = send([f'sample {mac} {timestamp+4000+i*40} {i}'
                             for i in range(25) for mac in devices])
            total_samples += 750
            assert len([m for m in messages[0] if m['type'] == 'sensor_data']) == 25
            assert len([m for m in messages[1] if m['type'] == 'sensor_data']) == 25

            first.socket.sendall(Client.encode(b'\x03\xe8', opcode=8))
            assert first.frame() == (8, b'\x03\xe8')
            process.stdin.write('stop\n')
            process.stdin.flush()
            assert process.wait(timeout=5) == 0
            count = 0
            for path in Path(directory).glob('*/sensor.db'):
                with sqlite3.connect(path) as db:
                    count += db.execute('SELECT COUNT(*) FROM sensor_data').fetchone()[0]
            assert count == total_samples, (count, total_samples)
            print(f'WebSocket integration passed: isolation, switching, 10 Hz, reconnect, '
                  f'30 devices, all {count} raw samples persisted.')
        finally:
            for client in clients:
                client.close()
            if process.poll() is None:
                process.kill()
                process.wait()


if __name__ == '__main__':
    main()
