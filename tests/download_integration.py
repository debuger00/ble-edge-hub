"""Exercise HTTP validation and export of live WAL data without touching real data."""
import importlib.util
import json
import sqlite3
import tempfile
import threading
import unittest
from pathlib import Path
from urllib.error import HTTPError
from urllib.request import urlopen

spec = importlib.util.spec_from_file_location("downloads", Path(__file__).resolve().parents[1] / "scripts/download_server.py")
downloads = importlib.util.module_from_spec(spec)
spec.loader.exec_module(downloads)


class DownloadsTest(unittest.TestCase):
    def test_exports_and_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            day = root / "2026_10_16"
            day.mkdir()
            db = sqlite3.connect(day / "sensor.db")
            db.execute("PRAGMA journal_mode=WAL")
            db.execute("CREATE TABLE sensor_data(ts REAL, mac TEXT, accel_x REAL)")
            db.executemany("INSERT INTO sensor_data VALUES (?, ?, ?)", [
                (1, "CE:43:75:25:96:BF", 3), (2, "54:6C:0E:B9:1A:4C", 4)])
            db.commit()
            server = downloads.make_server(root, 0)
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            base = f"http://127.0.0.1:{server.server_port}"
            try:
                with urlopen(base + "/api/dates") as response:
                    self.assertEqual(json.load(response)["dates"], ["2026_10_16"])
                with urlopen(base + "/api/download?date=2026_10_16&format=csv&mac=ce:43:75:25:96:bf") as response:
                    text = response.read().decode("utf-8-sig")
                    self.assertIn("CE:43:75:25:96:BF", text)
                    self.assertNotIn("54:6C", text)
                    self.assertIn("attachment", response.headers["Content-Disposition"])
                with urlopen(base + "/api/download?date=2026_10_16&format=sqlite") as response:
                    snapshot = root / "snapshot.db"
                    snapshot.write_bytes(response.read())
                copy = sqlite3.connect(snapshot)
                self.assertEqual(copy.execute("PRAGMA integrity_check").fetchone()[0], "ok")
                self.assertEqual(copy.execute("SELECT count(*) FROM sensor_data").fetchone()[0], 2)
                copy.close()
                for query, status in [
                    ("date=../../etc", 400), ("date=2026_10_15", 404),
                    ("date=2026_10_16&format=sqlite&mac=CE:43:75:25:96:BF", 400),
                    ("date=2026_10_16&mac=bad", 400)]:
                    with self.assertRaises(HTTPError) as error:
                        urlopen(base + "/api/download?" + query)
                    self.assertEqual(error.exception.code, status)
                downloads.EXPORT_SLOT.acquire()
                try:
                    with self.assertRaises(HTTPError) as error:
                        urlopen(base + "/api/download?date=2026_10_16")
                    self.assertEqual(error.exception.code, 503)
                finally:
                    downloads.EXPORT_SLOT.release()
            finally:
                server.shutdown()
                server.server_close()
                worker.join()
                db.close()


if __name__ == "__main__":
    unittest.main()
