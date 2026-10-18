#!/usr/bin/env python3
"""Local download API, exposed through nginx. Python standard library only."""
import argparse
from contextlib import closing
import csv
import json
import re
import shutil
import sqlite3
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

EXPORT_SLOT = threading.BoundedSemaphore(1)


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        url = urlsplit(self.path)
        if url.path == "/api/dates":
            dates = sorted((p.name for p in self.server.data_dir.iterdir()
                            if re.fullmatch(r"\d{4}_\d{2}_\d{2}", p.name)
                            and not p.is_symlink() and (p / "sensor.db").is_file()), reverse=True)
            body = json.dumps({"dates": dates}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        if url.path != "/api/download":
            self.send_error(404)
            return
        query = parse_qs(url.query)
        day = query.get("date", [""])[0]
        fmt = query.get("format", ["csv"])[0]
        mac = query.get("mac", [""])[0]
        if (not re.fullmatch(r"\d{4}_\d{2}_\d{2}", day) or fmt not in ("csv", "sqlite")
                or (mac and not re.fullmatch(r"(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}", mac))
                or (fmt == "sqlite" and mac)):
            self.send_error(400, "Invalid date, format or MAC")
            return
        source = (self.server.data_dir / day / "sensor.db").resolve()
        if not source.is_relative_to(self.server.data_dir) or not source.is_file():
            self.send_error(404, "No data for this date")
            return
        if not EXPORT_SLOT.acquire(blocking=False):
            self.send_error(503, "Another export is running; retry later")
            return
        headers_sent = False
        try:
            with tempfile.TemporaryDirectory(prefix="iot-export-") as directory:
                snapshot = Path(directory) / "sensor.db"
                deadline = time.monotonic() + 120
                def progress(*_):
                    if time.monotonic() > deadline:
                        raise TimeoutError("Snapshot timed out")
                # SQLite backup includes committed WAL records consistently.
                with closing(sqlite3.connect(source.as_uri() + "?mode=ro", uri=True)) as src:
                    with closing(sqlite3.connect(snapshot)) as dst:
                        src.backup(dst, pages=256, progress=progress)
                output = snapshot
                if fmt == "csv":
                    output = Path(directory) / "sensor.csv"
                    with closing(sqlite3.connect(snapshot)) as db, output.open("w", encoding="utf-8-sig", newline="") as stream:
                        cursor = db.execute("SELECT * FROM sensor_data" +
                                            (" WHERE mac = ? COLLATE NOCASE" if mac else ""),
                                            (mac,) if mac else ())
                        writer = csv.writer(stream)
                        writer.writerow([column[0] for column in cursor.description])
                        writer.writerows(cursor)
                filename = f"sensor_{day}" + ("_" + mac.replace(":", "") if mac else "") + (".csv" if fmt == "csv" else ".db")
                self.send_response(200)
                self.send_header("Content-Type", "text/csv; charset=utf-8" if fmt == "csv" else "application/octet-stream")
                self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
                self.send_header("Content-Length", str(output.stat().st_size))
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                headers_sent = True
                with output.open("rb") as stream:
                    shutil.copyfileobj(stream, self.wfile)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except (OSError, sqlite3.Error, TimeoutError) as error:
            self.log_error("Export failed: %s", error)
            if not headers_sent:
                self.send_error(500, "Export failed; check server log")
        finally:
            EXPORT_SLOT.release()


def make_server(data_dir, port):
    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.data_dir = Path(data_dir).resolve()
    return server


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--data-dir", default="data")
    parser.add_argument("--port", type=int, default=8766)
    args = parser.parse_args()
    make_server(args.data_dir, args.port).serve_forever()
