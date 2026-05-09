import json
import os
import sqlite3
import threading
import unittest
import urllib.error
import urllib.request
from contextlib import contextmanager
from datetime import datetime, timedelta
from pathlib import Path
from tempfile import NamedTemporaryFile

import sys


REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

import strava_bridge


REMOTE_BRIDGE_URL = os.environ.get(
    "STRAVA_BRIDGE_REMOTE_URL",
    "http://192.168.1.54:8082/api/exercise-load",
)


def _current_window():
    today = datetime.utcnow().date()
    days_since_sunday = (today.weekday() + 1) % 7
    current_sunday = today - timedelta(days=days_since_sunday)
    start_date = current_sunday - timedelta(weeks=strava_bridge.WEEKS - 1)
    end_date = current_sunday + timedelta(days=6)
    return start_date, current_sunday, end_date


@contextmanager
def _temp_activity_db():
    start_date, current_sunday, _ = _current_window()
    first_day = start_date + timedelta(days=2)
    busiest_day = current_sunday + timedelta(days=1)

    with NamedTemporaryFile(suffix=".sqlite3", delete=False) as temp_db:
        db_path = temp_db.name

    conn = sqlite3.connect(db_path)
    try:
        conn.execute(
            """
            CREATE TABLE activities (
                id INTEGER PRIMARY KEY,
                start_date_local TEXT NOT NULL,
                moving_time_in_seconds INTEGER NOT NULL
            )
            """
        )
        conn.executemany(
            "INSERT INTO activities (start_date_local, moving_time_in_seconds) VALUES (?, ?)",
            [
                (f"{first_day.isoformat()}T06:00:00", 1800),
                (f"{first_day.isoformat()}T18:30:00", 900),
                (f"{busiest_day.isoformat()}T07:15:00", 7200),
            ],
        )
        conn.commit()
        yield db_path, first_day.isoformat(), busiest_day.isoformat()
    finally:
        conn.close()
        Path(db_path).unlink(missing_ok=True)


@contextmanager
def _temp_activity_db_camel_case():
    start_date, current_sunday, _ = _current_window()
    first_day = start_date + timedelta(days=1)
    busiest_day = current_sunday + timedelta(days=2)

    with NamedTemporaryFile(suffix=".sqlite3", delete=False) as temp_db:
        db_path = temp_db.name

    conn = sqlite3.connect(db_path)
    try:
        conn.execute(
            """
            CREATE TABLE activities (
                activityId INTEGER PRIMARY KEY,
                startDateTime TEXT NOT NULL,
                movingTimeInSeconds INTEGER NOT NULL
            )
            """
        )
        conn.executemany(
            "INSERT INTO activities (startDateTime, movingTimeInSeconds) VALUES (?, ?)",
            [
                (f"{first_day.isoformat()}T06:30:00", 1500),
                (f"{busiest_day.isoformat()}T17:45:00", 5400),
            ],
        )
        conn.commit()
        yield db_path, first_day.isoformat(), busiest_day.isoformat()
    finally:
        conn.close()
        Path(db_path).unlink(missing_ok=True)


@contextmanager
def _serve_api(db_path):
    class TestHandler(strava_bridge.Handler):
        pass

    TestHandler.db_path = db_path
    server = strava_bridge.HTTPServer(("127.0.0.1", 0), TestHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        thread.join(timeout=5)
        server.server_close()


class StravaBridgeApiTests(unittest.TestCase):
    def _assert_bridge_payload_shape(self, payload):
        self.assertEqual(payload["weeks"], strava_bridge.WEEKS)
        self.assertEqual(len(payload["days"]), strava_bridge.WEEKS * 7)
        self.assertIn("generated_at", payload)
        datetime.fromisoformat(payload["generated_at"])
        self.assertIn("total_load", payload)
        self.assertIn("total_activities", payload)
        self.assertIn("busiest_date", payload)
        self.assertIn("busiest_load", payload)
        self.assertTrue(all("date" in day and "load" in day for day in payload["days"]))

    def test_get_exercise_load_returns_expected_aggregates(self):
        with _temp_activity_db() as (db_path, first_day, busiest_day):
            payload = strava_bridge.get_exercise_load(db_path)

        self._assert_bridge_payload_shape(payload)
        self.assertEqual(payload["total_load"], 165.0)
        self.assertEqual(payload["total_activities"], 2)
        self.assertEqual(payload["busiest_date"], busiest_day)
        self.assertEqual(payload["busiest_load"], 120.0)

        loads_by_day = {entry["date"]: entry["load"] for entry in payload["days"]}
        self.assertEqual(loads_by_day[first_day], 45.0)
        self.assertEqual(loads_by_day[busiest_day], 120.0)

    def test_http_endpoint_serves_json_results(self):
        with _temp_activity_db() as (db_path, _, busiest_day):
            with _serve_api(db_path) as server:
                url = f"http://127.0.0.1:{server.server_port}/api/exercise-load"
                with urllib.request.urlopen(url) as response:
                    body = response.read().decode("utf-8")
                    payload = json.loads(body)

                self.assertEqual(response.status, 200)
                self.assertEqual(response.headers["Content-Type"], "application/json")
                self.assertEqual(response.headers["Access-Control-Allow-Origin"], "*")
                self._assert_bridge_payload_shape(payload)
                self.assertEqual(payload["busiest_date"], busiest_day)
                self.assertGreater(payload["total_load"], 0.0)
                self.assertTrue(any(day["load"] > 0 for day in payload["days"]))

    def test_get_exercise_load_supports_camel_case_schema(self):
        with _temp_activity_db_camel_case() as (db_path, first_day, busiest_day):
            payload = strava_bridge.get_exercise_load(db_path)

        self._assert_bridge_payload_shape(payload)
        self.assertEqual(payload["total_load"], 115.0)
        self.assertEqual(payload["total_activities"], 2)
        self.assertEqual(payload["busiest_date"], busiest_day)
        self.assertEqual(payload["busiest_load"], 90.0)

        loads_by_day = {entry["date"]: entry["load"] for entry in payload["days"]}
        self.assertEqual(loads_by_day[first_day], 25.0)
        self.assertEqual(loads_by_day[busiest_day], 90.0)

    def test_mithril_docker_endpoint_is_available(self):
        try:
            with urllib.request.urlopen(REMOTE_BRIDGE_URL, timeout=5) as response:
                body = response.read().decode("utf-8")
                payload = json.loads(body)
        except urllib.error.HTTPError as exc:
            error_body = exc.read().decode("utf-8", errors="replace")
            self.fail(
                f"Mithril bridge endpoint at {REMOTE_BRIDGE_URL} returned HTTP {exc.code}: {error_body}"
            )
        except urllib.error.URLError as exc:
            self.fail(f"Could not reach Mithril bridge endpoint at {REMOTE_BRIDGE_URL}: {exc}")

        self.assertEqual(response.status, 200)
        self.assertEqual(response.headers["Content-Type"], "application/json")
        self._assert_bridge_payload_shape(payload)
        self.assertGreaterEqual(payload["total_load"], 0.0)
        self.assertGreaterEqual(payload["total_activities"], 0)
        self.assertTrue(any(day["load"] >= 0 for day in payload["days"]))

    def test_unknown_route_returns_404(self):
        with _temp_activity_db() as (db_path, _, _):
            with _serve_api(db_path) as server:
                url = f"http://127.0.0.1:{server.server_port}/not-found"
                with self.assertRaises(urllib.error.HTTPError) as exc_info:
                    urllib.request.urlopen(url)

        self.assertEqual(exc_info.exception.code, 404)


if __name__ == "__main__":
    unittest.main()