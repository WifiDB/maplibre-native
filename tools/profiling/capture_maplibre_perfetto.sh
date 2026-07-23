#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONFIG="${CONFIG:-$ROOT_DIR/tools/profiling/maplibre_terrain_perfetto.pbtxt}"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/profiles}"
PACKAGE="${PACKAGE:-org.maplibre.android.testapp}"
ADB_SERVER_SOCKET="${ADB_SERVER_SOCKET:-tcp:192.168.122.1:5037}"
SERIAL="${SERIAL:-}"

if [[ -z "$SERIAL" ]]; then
  SERIAL="$(ADB_SERVER_SOCKET="$ADB_SERVER_SOCKET" adb devices | awk 'NR > 1 && $2 == "device" { print $1; exit }')"
fi

if [[ -z "$SERIAL" ]]; then
  echo "No adb device found via ADB_SERVER_SOCKET=$ADB_SERVER_SOCKET" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
stamp="$(date +%Y%m%d_%H%M%S)"
remote_trace="/data/misc/perfetto-traces/maplibre_terrain_${stamp}.perfetto-trace"
local_trace="$OUT_DIR/maplibre_terrain_${stamp}.perfetto-trace"
local_gfxinfo="$OUT_DIR/maplibre_terrain_${stamp}.gfxinfo.txt"

adb_cmd=(adb -s "$SERIAL")

echo "Using device: $SERIAL"
echo "Package: $PACKAGE"
echo "Trace config: $CONFIG"
echo "Output: $local_trace"

echo "Resetting gfxinfo counters..."
ADB_SERVER_SOCKET="$ADB_SERVER_SOCKET" "${adb_cmd[@]}" shell dumpsys gfxinfo "$PACKAGE" reset >/dev/null 2>&1 || true

echo "Start interacting with the terrain map now. Capturing 15 seconds..."
ADB_SERVER_SOCKET="$ADB_SERVER_SOCKET" "${adb_cmd[@]}" shell perfetto --txt -c - -o "$remote_trace" < "$CONFIG"

ADB_SERVER_SOCKET="$ADB_SERVER_SOCKET" "${adb_cmd[@]}" pull "$remote_trace" "$local_trace" >/dev/null
ADB_SERVER_SOCKET="$ADB_SERVER_SOCKET" "${adb_cmd[@]}" shell dumpsys gfxinfo "$PACKAGE" > "$local_gfxinfo" || true
ADB_SERVER_SOCKET="$ADB_SERVER_SOCKET" "${adb_cmd[@]}" shell rm -f "$remote_trace" >/dev/null 2>&1 || true

echo "Saved trace: $local_trace"
echo "Saved gfxinfo: $local_gfxinfo"
echo "Open trace in https://ui.perfetto.dev or run:"
echo "  traceconv systrace \"$local_trace\" \"$local_trace.html\""
