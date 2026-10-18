"""Exercise scheduled startup, retries and late ready events with a fake gateway."""
import datetime
import functools
import operator
import pathlib
import socket
import subprocess
import sys
import tempfile
import time


def port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def receive(sock):
    def exact(count):
        result = b''
        while len(result) < count:
            chunk = sock.recv(count - len(result))
            assert chunk, 'gateway connection closed unexpectedly'
            result += chunk
        return result
    header = exact(4)
    return header + exact(int.from_bytes(header[2:4], 'big') - 4)


def send_event(sock, kind, payload):
    frame = bytes.fromhex('ACCA') + (14 + len(payload)).to_bytes(2, 'big')
    frame += bytes.fromhex('FFFFFFFFFFFF0001') + bytes([kind]) + payload
    sock.sendall(frame + bytes([functools.reduce(operator.xor, frame, 0)]))


def run(binary, enabled, inside, device_type="imu948"):
    now = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8)))
    start = now + datetime.timedelta(minutes=-5 if inside else 5)
    stop = now + datetime.timedelta(minutes=5 if inside else 10)
    tcp, ws = port(), port()
    with tempfile.TemporaryDirectory() as folder:
        cfg = pathlib.Path(folder) / 'config.yaml'
        cfg.write_text(f'''tcp:
  host: "127.0.0.1"
  port: {tcp}
websocket:
  host: "127.0.0.1"
  port: {ws}
data_dir: "{folder}/data"
reconnect_interval_seconds: 1
inference:
  enabled: false
collection_schedule:
  enabled: {str(enabled).lower()}
  start_time: "{start:%H:%M}"
  stop_time: "{stop:%H:%M}"
sensor_types:
  imu948:
    frequency: 25
    subscribe_tag: 0x000F
    notify_handle: 0x0008
    cccd_handle: 0x0009
    write_handle: 0x0006
  md9930:
    frequency: 25
    notify_handle: 0x000B
    cccd_handle: 0x000C
    write_handle: 0x0015
    command_handle: 0x0012
devices:
  - mac: "AA:BB:CC:DD:EE:FF"
    type: {device_type}
''')
        with open(pathlib.Path(folder) / 'log', 'w+') as log:
            process = subprocess.Popen([binary, '--config', str(cfg)], stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        sock = socket.create_connection(('127.0.0.1', tcp), timeout=2)
                        break
                    except ConnectionRefusedError:
                        if process.poll() is not None or time.monotonic() > deadline:
                            log.seek(0)
                            raise AssertionError(log.read())
                        time.sleep(.05)
                with sock:
                    if not enabled:
                        sock.settimeout(.4)
                        try:
                            sock.recv(1)
                            raise AssertionError('disabled schedule sent automatic command')
                        except socket.timeout:
                            return
                    expected = 0x10 if inside else 0x11
                    initial = receive(sock)
                    assert initial[12] == expected, 'wrong initial schedule command'
                    if inside:
                        assert initial[20:22] == b'\x00\x00', 'collection must request a long BLE connection'
                    assert receive(sock)[12] == expected, 'missing schedule retry'
                    # A late ready event must never reconfigure a stopped sensor.
                    payload = bytes.fromhex('AABBCCDDEEFF') + bytes([6])
                    frame = bytes.fromhex('ACCA') + (14 + len(payload)).to_bytes(2, 'big')
                    frame += bytes.fromhex('FFFFFFFFFFFF000121') + payload
                    frame += bytes([functools.reduce(operator.xor, frame, 0)])
                    sock.sendall(frame)
                    ready = receive(sock)
                    assert ready[12] == (0x12 if inside else 0x11), 'wrong ready handling'
                    if inside:
                        command = receive(sock)
                        ids = [ready[10:12], command[10:12]]
                        if device_type == "md9930":
                            assert ready[20:22] == bytes.fromhex('0015')
                            assert ready[24:-1] == bytes.fromhex('002800000000'), 'MD9930 must not receive IMU948 0x29'
                            assert command[20:22] == bytes.fromhex('0012')
                            assert command[24:-1] == b'\x0a'
                        else:
                            assert ready[24:-1] == b'\x29'
                            assert command[-3:-1] == b'\x0f\x00', 'configured IMU subscription mask ignored'
                            for ack in (0x29, 0x12):
                                send_event(sock, 0x20, bytes.fromhex('AABBCCDDEEFF000801') + bytes([ack]))
                        # Duplicate ready notifications must not reset sensor parameters.
                        sock.sendall(frame)
                        sock.settimeout(.4)
                        try:
                            sock.recv(1)
                            raise AssertionError('duplicate ready event reconfigured device')
                        except socket.timeout:
                            pass
                        # A real reconnect must reconfigure with fresh message IDs.
                        send_event(sock, 0x21, bytes.fromhex('AABBCCDDEEFF02'))
                        sock.sendall(frame)
                        sock.settimeout(2)
                        ids.extend([receive(sock)[10:12], receive(sock)[10:12]])
                        assert len(set(ids)) == 4, 'configuration commands reused message IDs'
                        log.flush()
                        log.seek(0)
                        output = log.read()
                        assert 'malformed sensor payload' not in output, output
            finally:
                process.terminate()
                process.wait(timeout=5)


for enabled, inside in [(True, True), (True, False), (False, False)]:
    run(sys.argv[1], enabled, inside)
run(sys.argv[1], True, True, "md9930")
print('Schedule integration tests passed: long connection, retries, stop, MD9930 commands')

# Interactive shutdown and battery polling use a PTY to exercise the real CLI path.
def test_idle_cli_and_battery(binary, device_type="imu948"):
    from websocket_integration import Client
    import os
    import pty
    tcp, ws = port(), port()
    with tempfile.TemporaryDirectory() as folder:
        cfg = pathlib.Path(folder) / 'config.yaml'
        cfg.write_text(f'''tcp:
  host: "127.0.0.1"
  port: {tcp}
websocket:
  host: "127.0.0.1"
  port: {ws}
data_dir: "{folder}/data"
battery_interval_seconds: 1
inference:
  enabled: false
collection_schedule:
  enabled: false
sensor_types:
  imu948:
    notify_handle: 8
    cccd_handle: 9
    write_handle: 6
  md9930:
    notify_handle: 11
    cccd_handle: 12
    write_handle: 21
    command_handle: 18
    battery_handle: 15
devices:
  - id: "005"
    mac: "AA:BB:CC:DD:EE:FF"
    type: {device_type}
''')
        master, slave = pty.openpty()
        with open(pathlib.Path(folder) / 'log', 'w+') as log:
            process = subprocess.Popen([binary, '--config', str(cfg)], stdin=slave, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 5
                while True:
                    try:
                        sock = socket.create_connection(('127.0.0.1', tcp), timeout=2)
                        break
                    except ConnectionRefusedError:
                        assert process.poll() is None and time.monotonic() < deadline
                        time.sleep(.05)
                with sock:
                    send_event(sock, 0x21, bytes.fromhex('AABBCCDDEEFF02'))
                    sock.settimeout(1.3)
                    try:
                        sock.recv(1)
                        raise AssertionError('battery queried while device was searching')
                    except socket.timeout:
                        pass
                    send_event(sock, 0x21, bytes.fromhex('AABBCCDDEEFF06'))
                    ids = [receive(sock)[10:12], receive(sock)[10:12]]
                    battery = receive(sock)
                    if device_type == "md9930":
                        assert battery[12] == 0x12
                        assert battery[13:-1] == bytes.fromhex('01AABBCCDDEEFF000F020100'), battery.hex()
                        client = Client(ws)
                        try:
                            # Invalid percentages, empty and multi-byte replies must not be published.
                            for value in (b'\xff', b'', b'\x32\x33'):
                                send_event(sock, 0x20, bytes.fromhex('AABBCCDDEEFF000F') + bytes([len(value)]) + value)
                            for level in (0, 72, 100):
                                send_event(sock, 0x20, bytes.fromhex('AABBCCDDEEFF000F01') + bytes([level]))
                                reply = client.message()
                                assert reply['type'] == 'battery' and reply['level'] == level, reply
                                assert reply['mac'] == 'AA:BB:CC:DD:EE:FF' and reply['timestamp'] > 0
                        finally:
                            client.close()
                    else:
                        assert battery[24:-1] == b'\x10', 'ready device battery was not queried' 
                    assert battery[10:12] not in ids, 'battery command ID reused'
                    if device_type == "md9930":
                        # Exercise CLI ID resolution and cached battery display after disconnect.
                        os.write(master, b'disconnect 0 5\n')
                        while receive(sock)[12] != 0x11:
                            pass
                        send_event(sock, 0x21, bytes.fromhex('AABBCCDDEEFF00'))
                        time.sleep(.1)
                        os.write(master, b'list\n')
                        deadline = time.monotonic() + 3
                        while True:
                            log.seek(0)
                            output = log.read()
                            if '最近电量=100%' in output:
                                break
                            assert time.monotonic() < deadline, output
                            time.sleep(.05)
                        assert 'ID=005  MAC=AA:BB:CC:DD:EE:FF' in output, output
                        assert '更新时间=' in output and '状态=未连接' in output, output
                        assert '设备 ID=005 MAC=AA:BB:CC:DD:EE:FF' in output, output

                process.terminate()
                assert process.wait(timeout=3) == 0, 'interactive shutdown failed'
                log.seek(0)
                assert 'all services stopped cleanly' in log.read()
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
                os.close(master)
                os.close(slave)


test_idle_cli_and_battery(sys.argv[1])
print('Interactive shutdown and ready-only battery tests passed')

test_idle_cli_and_battery(sys.argv[1], "md9930")
print("MD9930 battery read and WebSocket delivery passed")
