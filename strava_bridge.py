#!/usr/bin/env python3
"""
Strava Exercise Load Bridge Server
====================================
Reads the statistics-for-strava SQLite database and serves daily exercise
load data as JSON for the ESP32 T-Display S3 to consume.

Usage:
    python3 strava_bridge.py [--db /path/to/database] [--port 8082]

The default database path is the standard portainer volume mount location.
The endpoint is: GET http://<host>:8082/api/exercise-load

JSON response format:
{
  "generated_at": "2026-05-08T12:00:00",
  "start": "2025-06-01",
  "weeks": 53,
    "days": [[0,45.5,320],[3,120.3,860],...],
  "total_load": 12345.0,
  "total_activities": 156,
  "busiest_date": "2025-08-15",
  "busiest_load": 285.3
}

"days" contains only active days as [offset, load, calories] triples where
offset is days since "start". Zero-load days are omitted.

"load" is total moving time for that day in minutes.
"calories" is the summed calories burned for that day when the source schema
exposes a calories column, otherwise 0.
Level thresholds (matching the display colours):
  0  = no activity
  1  = 0-50 min  (Low,  green)
  2  = 50-100    (Medium, amber)
  3  = 100-150   (High, orange)
  4  = >150      (Very high, red)
"""

import sqlite3
import json
import argparse
from contextlib import closing
from datetime import datetime, timedelta, timezone
from http.server import HTTPServer, ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse

DB_PATH_DEFAULT = "/home/merlin/portainer_data/strava/storage/database"
DEFAULT_PORT    = 8082
WEEKS           = 53


def _quote_identifier(identifier: str) -> str:
    """Safely quote a SQLite identifier discovered from schema metadata."""
    if not identifier:
        raise ValueError("Empty SQL identifier")
    return '"' + identifier.replace('"', '""') + '"'


def get_exercise_load(db_path: str) -> dict:
    """Query the SQLite database and return exercise load data for the last 53 weeks."""
    with closing(sqlite3.connect(db_path)) as conn:
        conn.row_factory = sqlite3.Row
        cursor = conn.cursor()

        # --- Discover tables ---
        cursor.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name;")
        tables = [row[0] for row in cursor.fetchall()]
        print(f"[Bridge] Tables: {tables}")

        # Find the activities table (first match on 'activit')
        activity_table = next((t for t in tables if "activit" in t.lower()), None)
        if not activity_table:
            return {"error": "No activities table found", "tables": tables}

        # --- Discover columns ---
        quoted_activity_table = _quote_identifier(activity_table)
        cursor.execute(f"PRAGMA table_info({quoted_activity_table});")
        columns = {row[1]: row[2] for row in cursor.fetchall()}
        col_names = list(columns.keys())
        print(f"[Bridge] Columns in '{activity_table}': {col_names}")

        # Date column
        date_col = next(
            (c for c in [
                "start_date_local",
                "startDateTime",
                "start_date",
                "startDate",
                "date",
                "activity_date",
                "activityDate",
            ]
             if c in columns),
            None
        )
        # Moving-time column (seconds preferred, minutes accepted)
        time_col = next(
            (c for c in [
                "moving_time_in_seconds",
                "movingTimeInSeconds",
                "moving_time",
                "elapsed_time_in_seconds",
                "elapsedTimeInSeconds",
                "elapsed_time",
                "elapsedTime",
                "duration_in_seconds",
                "durationInSeconds",
                "duration",
            ]
             if c in columns),
            None
        )
        calorie_col = next(
            (c for c in [
                "calories",
                "calories_burned",
                "caloriesBurned",
                "kilocalories",
                "kiloCalories",
            ]
             if c in columns),
            None
        )

        if not date_col or not time_col:
            return {
                "error": f"Required columns not found. Available: {col_names}",
                "hint": "Expected a date column (start_date_local) and a time column (moving_time_in_seconds)"
            }

        # Decide whether the time column is in seconds or minutes.
        # Columns ending in '_in_seconds' or with name 'moving_time' from Strava are seconds.
        is_seconds = "second" in time_col.lower() or time_col in ("moving_time", "elapsed_time")
        divisor = 60.0 if is_seconds else 1.0
        print(
            f"[Bridge] Using date='{date_col}', time='{time_col}', calories='{calorie_col}' "
            f"(divisor={divisor})"
        )

        # --- Date range: last WEEKS weeks, aligned to Sunday ---
        today = datetime.now(timezone.utc).date()
        # weekday(): Mon=0 ... Sun=6  ->  days since last Sunday
        days_since_sunday = (today.weekday() + 1) % 7
        current_sunday = today - timedelta(days=days_since_sunday)
        start_date = current_sunday - timedelta(weeks=WEEKS - 1)
        end_date = current_sunday + timedelta(days=6)

        quoted_date_col = _quote_identifier(date_col)
        quoted_time_col = _quote_identifier(time_col)
        calories_expr = "0.0 AS total_calories"
        if calorie_col:
            quoted_calorie_col = _quote_identifier(calorie_col)
            calories_expr = f"SUM(COALESCE(CAST({quoted_calorie_col} AS FLOAT), 0.0)) AS total_calories"

        # --- Query ---
        query = f"""
            SELECT
                date({quoted_date_col})                   AS activity_date,
                SUM(CAST({quoted_time_col} AS FLOAT) / ?) AS total_minutes,
                {calories_expr}
            FROM {quoted_activity_table}
            WHERE date({quoted_date_col}) BETWEEN ? AND ?
            GROUP BY date({quoted_date_col})
            ORDER BY activity_date ASC
        """
        cursor.execute(query, (divisor, start_date.isoformat(), end_date.isoformat()))
        rows = cursor.fetchall()

    load_by_date = {
        row["activity_date"]: {
            "load": round(float(row["total_minutes"]), 1),
            "calories": round(float(row["total_calories"] or 0.0), 1),
        }
        for row in rows
    }

    # --- Build compact day list (active days only, [offset, load, calories] triples) ---
    days         = []
    busiest_date = ""
    busiest_load = 0.0
    total_load   = 0.0
    total_active = 0
    offset       = 0

    current = start_date
    while current <= end_date:
        ds   = current.isoformat()
        day_totals = load_by_date.get(ds, {"load": 0.0, "calories": 0.0})
        load = day_totals["load"]
        calories = day_totals["calories"]
        if load > 0:
            days.append([offset, round(load, 1), round(calories, 1)])
            total_active += 1
        total_load += load
        if load > busiest_load:
            busiest_load = load
            busiest_date = ds
        current += timedelta(days=1)
        offset  += 1

    return {
        "generated_at":    datetime.now(timezone.utc).isoformat(),
        "start":           start_date.isoformat(),
        "weeks":           WEEKS,
        "days":            days,
        "total_load":      round(total_load, 1),
        "total_activities": total_active,
        "busiest_date":    busiest_date,
        "busiest_load":    round(busiest_load, 1),
    }


class Handler(BaseHTTPRequestHandler):
    db_path: str = DB_PATH_DEFAULT

    def do_GET(self):
        path = urlparse(self.path).path.rstrip("/")
        if path == "/api/exercise-load":
            try:
                data = get_exercise_load(self.db_path)
                body = json.dumps(data).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type",   "application/json")
                self.send_header("Content-Length", len(body))
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(body)
                print(f"[Bridge] Served {len(data.get('days', []))} days to {self.client_address[0]}")
            except Exception as exc:
                body = json.dumps({"error": str(exc)}).encode("utf-8")
                self.send_response(500)
                self.send_header("Content-Type",   "application/json")
                self.send_header("Content-Length", len(body))
                self.end_headers()
                self.wfile.write(body)
                print(f"[Bridge] ERROR: {exc}")
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, fmt, *args):
        pass  # suppress default access log; we print our own


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Strava Exercise Load Bridge Server")
    parser.add_argument(
        "--db",
        default=DB_PATH_DEFAULT,
        help=f"Path to statistics-for-strava SQLite database (default: {DB_PATH_DEFAULT})",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help=f"HTTP port to listen on (default: {DEFAULT_PORT})",
    )
    args = parser.parse_args()

    Handler.db_path = args.db

    print("=" * 60)
    print("  Strava Exercise Load Bridge Server")
    print("=" * 60)
    print(f"  Database : {args.db}")
    print(f"  Endpoint : http://0.0.0.0:{args.port}/api/exercise-load")
    print("=" * 60)

    httpd = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("[Bridge] Shutting down")
