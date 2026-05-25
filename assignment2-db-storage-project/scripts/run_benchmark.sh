#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
DATASET="${1:-synthetic}"
LIMIT="${2:-1000000}"
TPCH_DIR="${3:-${PROJECT_DIR}/..}"

mkdir -p "${BUILD_DIR}" "${PROJECT_DIR}/results/figures"
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}"
cmake --build "${BUILD_DIR}" -j

if [[ "${DATASET}" == "tpch" ]]; then
  "${BUILD_DIR}/db_storage_bench" \
    --schema "${PROJECT_DIR}/schema/orders.schema.txt" \
    --dataset tpch \
    --tpch-dir "${TPCH_DIR}" \
    --limit "${LIMIT}" \
    --benchmark all \
    --output "${PROJECT_DIR}/results/benchmark_result.csv"
else
  "${BUILD_DIR}/db_storage_bench" \
    --schema "${PROJECT_DIR}/schema/student.schema.txt" \
    --dataset synthetic \
    --limit "${LIMIT}" \
    --benchmark all \
    --output "${PROJECT_DIR}/results/benchmark_result.csv"
fi

python3 "${PROJECT_DIR}/scripts/plot_metrics.py" \
  "${PROJECT_DIR}/results/benchmark_result.csv" \
  "${PROJECT_DIR}/results/figures"
