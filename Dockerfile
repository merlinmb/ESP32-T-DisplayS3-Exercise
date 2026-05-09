# ─────────────────────────────────────────────────────────────────────────────
# Strava Exercise Load Bridge  –  Dockerfile
# Python 3.12 slim image; no pip dependencies (stdlib only).
# ─────────────────────────────────────────────────────────────────────────────
FROM python:3.12-slim

LABEL maintainer="Mark Beets"
LABEL description="Serves statistics-for-strava SQLite exercise data as JSON for the ESP32 T-Display S3"

WORKDIR /app

# Copy the single-file bridge server
COPY strava_bridge.py .

# The database is mounted at runtime via a volume (read-only)
VOLUME ["/data"]

EXPOSE 8082

# Health-check: hit the endpoint every 30 s, allow 15 s per attempt
HEALTHCHECK --interval=30s --timeout=15s --start-period=10s --retries=3 \
    CMD python3 -c \
        "import urllib.request; urllib.request.urlopen('http://localhost:8082/api/exercise-load')" \
    || exit 1

ENTRYPOINT ["python3", "strava_bridge.py"]
CMD ["--db", "/data/database/strava.db", "--port", "8082"]
