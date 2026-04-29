#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT_DIR="${ROOT_DIR}/tools/demo/out"
BIN_DIR="${ROOT_DIR}/tools/demo/bin"
DEMO_BIN="${BIN_DIR}/live_validation_demo"
REPORT_TXT="${OUT_DIR}/live_validation_report.txt"
REPORT_JSON="${OUT_DIR}/live_validation_report.json"

mkdir -p "${BIN_DIR}" "${OUT_DIR}"

if [[ ! -x "${DEMO_BIN}" ]]; then
  g++ -std=c++20 -Wall -Wextra -Werror -I"${ROOT_DIR}" -I"${ROOT_DIR}/src" \
    "${ROOT_DIR}/tools/demo/live_validation_demo.cpp" \
    "${ROOT_DIR}/src/scene/GeoMath.cpp" "${ROOT_DIR}/src/scene/CameraController.cpp" \
    "${ROOT_DIR}/src/ingest/SnapshotParser.cpp" "${ROOT_DIR}/src/ipc/PipeProtocol.cpp" \
    "${ROOT_DIR}/src/overlay/EventStore.cpp" "${ROOT_DIR}/src/overlay/OverlayThrottle.cpp" \
    "${ROOT_DIR}/src/overlay/DirtyRegionGenerator.cpp" \
    "${ROOT_DIR}/src/input/InputRouter.cpp" "${ROOT_DIR}/src/input/ArbitrationEngine.cpp" \
    "${ROOT_DIR}/src/render/DirtyRectCoalescer.cpp" \
    -o "${DEMO_BIN}"
fi

"${DEMO_BIN}" --json-out "${REPORT_JSON}" | tee "${REPORT_TXT}"
cat > "${OUT_DIR}/perf_metrics_template.json" <<'JSON'
{
  "idle": {
    "idle_cpu_percent_total": 0.8,
    "idle_render_present_rate_hz": 0.3,
    "idle_pipeline_activity_per_sec": 2.1
  },
  "active": {
    "active_p95_frame_ms": 12.5,
    "active_p99_frame_ms": 21.7,
    "ingest_queue_peak_depth": 6.0,
    "control_loop_p95_ms": 2.8
  },
  "burst": {
    "burst_input_events": 120.0,
    "burst_throttled_events": 92.0,
    "ingest_queue_peak_depth": 14.0
  }
}
JSON
python3 "${ROOT_DIR}/tools/perf/check_thresholds.py" \
  --input "${OUT_DIR}/perf_metrics_template.json" \
  --output-json "${OUT_DIR}/perf_sanity_gate_results.json"

echo "Demo outputs:"
echo "  ${REPORT_TXT}"
echo "  ${REPORT_JSON}"
echo "  ${OUT_DIR}/perf_metrics_template.json"
echo "  ${OUT_DIR}/perf_sanity_gate_results.json"
