#!/usr/bin/env bash
# Start the Python backend server.
# The JUCE plugin connects to http://127.0.0.1:8765
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_DIR="$SCRIPT_DIR/../backend"

cd "$BACKEND_DIR"

if [ ! -d ".venv" ]; then
  echo "Creating virtual environment..."
  python3 -m venv .venv
  .venv/bin/pip install -r requirements.txt -q
fi

echo "Starting backend on http://127.0.0.1:8765 ..."
.venv/bin/uvicorn server:app --host 127.0.0.1 --port 8765 --reload
