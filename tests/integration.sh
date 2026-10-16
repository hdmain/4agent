#!/usr/bin/env bash
set -euo pipefail
exec python3 "$(dirname "$0")/integration.py" "${1:-/usr/local/sbin/4agent}"
