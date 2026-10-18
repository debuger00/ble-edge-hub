"""Local-only SMTP and gateway integration; never contacts an external mailbox."""
import datetime
import email
import email.policy
import functools
import operator
import pathlib
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time


class SMTP(socketserver.StreamRequestHandler):
    def handle(self):
        self.wfile.write(b'220 local.test ESMTP\r\n')
        while True:
            line = self.rfile.readline()
            if not line:
                return
            command = line.split(b' ', 1)[0].strip().upper()
            if command in (b'EHLO', b'HELO'):
                self.wfile.write(b'250-local.test\r\n250 8BITMIME\r\n')
            elif command == b'MAIL':
                self.server.attempts += 1
                self.wfile.write(b'451 retry later\r\n' if self.server.attempts == 1 else b'250 OK\r\n')
            elif command in (b'RCPT', b'RSET'):
                self.wfile.write(b'250 OK\r\n')
            elif command == b'DATA':
                self.wfile.write(b'354 send data\r\n')
                data = []
                while True:
                    line = self.rfile.readline()
                    if line in (b'.\r\n', b''):
                        break
                    data.append(line[1:] if line.startswith(b'..') else line)
                self.server.messages.append(email.message_from_bytes(b''.join(data), policy=email.policy.default))
                self.wfile.write(b'250 accepted\r\n')
            elif command == b'QUIT':
                self.wfile.write(b'221 bye\r\n')
                return
            else:
                self.wfile.write(b'502 unsupported\r\n')


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def frame(mac, sample=True):
    sensor = bytes.fromhex('110500785634120100feff64000a00ecff1e00') if sample else bytes([0x10]) + bytes(32)
    payload = bytes.fromhex(mac) + b'\x00\x08' + bytes([len(sensor)]) + sensor
    raw = b'\xac\xca' + (14 + len(payload)).to_bytes(2, 'big') + bytes.fromhex('FFFFFFFFFFFF000120') + payload
    return raw + bytes([functools.reduce(operator.xor, raw, 0)])


def wait_until(check, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.05)
    raise AssertionError('timed out waiting for expected alert')


def scenario(binary, server, enabled, outside):
    tcp, ws = free_port(), free_port()
    now = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8)))
    start, stop = now + datetime.timedelta(minutes=5), now + datetime.timedelta(minutes=10)
    with tempfile.TemporaryDirectory() as folder:
        cfg = pathlib.Path(folder) / 'test.yaml'
        cfg.write_text(f'''tcp:
  host: "127.0.0.1"
  port: {tcp}
websocket:
  host: "127.0.0.1"
  port: {ws}
data_dir: "{folder}/data"
inference:
  enabled: false
collection_schedule:
  enabled: {str(outside).lower()}
  start_time: "{start:%H:%M}"
  stop_time: "{stop:%H:%M}"
alerts:
  enabled: {str(enabled).lower()}
  timeout_seconds: 2
  retry_interval_seconds: 1
  smtp_timeout_seconds: 3
  smtp_host: "127.0.0.1"
  smtp_port: {server.server_address[1]}
  smtp_security: none
  from_email: "sender@example.test"
  to_email: "receiver@example.test"
sensor_types:
  imu948:
    frequency: 25
    notify_handle: 0x0008
    cccd_handle: 0x0009
    write_handle: 0x0006
devices:
  - id: "001"
    mac: "AA:BB:CC:DD:EE:FF"
    type: imu948
  - id: "002"
    mac: "AA:BB:CC:DD:EE:00"
    type: imu948
''')
        with open(pathlib.Path(folder) / 'log', 'w+') as log:
            proc = subprocess.Popen([binary, '--config', str(cfg)], stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            done, recovered = threading.Event(), threading.Event()
            pump = None
            try:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        sock = socket.create_connection(('127.0.0.1', tcp), timeout=1)
                        break
                    except ConnectionRefusedError:
                        if proc.poll() is not None or time.monotonic() > deadline:
                            log.seek(0)
                            raise AssertionError(log.read())
                        time.sleep(.05)
                with sock:
                    def samples():
                        while not done.is_set():
                            sock.sendall(frame('AABBCCDDEE00') + frame('AABBCCDDEEFF', recovered.is_set()))
                            done.wait(.1)
                    pump = threading.Thread(target=samples)
                    pump.start()
                    initial = len(server.messages)
                    if not enabled or outside:
                        time.sleep(3.2)
                        assert len(server.messages) == initial, 'master switch / schedule did not suppress alert'
                    else:
                        wait_until(lambda: len(server.messages) == initial + 1)
                        assert server.attempts >= 2, 'SMTP failure was not retried'
                        msg = server.messages[-1]
                        assert msg['To'] == 'receiver@example.test'
                        body = msg.get_content()
                        assert '001' in body and 'AA:BB:CC:DD:EE:FF' in body and 'UTC+8' in body
                        assert 'AA:BB:CC:DD:EE:00' not in body, 'healthy sensor reported as failed'
                        time.sleep(3.2)
                        assert len(server.messages) == initial + 1, 'duplicate incident email'
                        recovered.set()
                        time.sleep(1.2)
                        recovered.clear()
                        wait_until(lambda: len(server.messages) == initial + 2)
                    done.set()
                    pump.join(timeout=2)
            finally:
                done.set()
                if pump:
                    pump.join(timeout=2)
                proc.terminate()
                proc.wait(timeout=5)


with socketserver.ThreadingTCPServer(('127.0.0.1', 0), SMTP) as server:
    server.attempts, server.messages = 0, []
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        scenario(sys.argv[1], server, True, False)
        scenario(sys.argv[1], server, False, False)
        scenario(sys.argv[1], server, True, True)
    finally:
        server.shutdown()
        thread.join()
print('Local SMTP alert integration passed')
