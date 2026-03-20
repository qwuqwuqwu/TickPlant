#!/usr/bin/env bash
# run_benchmarks.sh
#
# Rebuilds all benchmark targets in Release mode, runs every harness,
# and writes a timestamped report to benchmark_results/.
#
# Usage:
#   ./run_benchmarks.sh                     # 3 repetitions (default)
#   ./run_benchmarks.sh --reps 5            # custom repetition count
#   ./run_benchmarks.sh --no-rebuild        # skip cmake/make, just run
#   ./run_benchmarks.sh --filter BM_MPSC   # run only matching benchmarks

set -euo pipefail

# ── Argument parsing ──────────────────────────────────────────────────────────
REPS=3
REBUILD=true
FILTER=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --reps)       REPS="$2";    shift 2 ;;
        --no-rebuild) REBUILD=false; shift   ;;
        --filter)     FILTER="$2"; shift 2   ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ── Paths ─────────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/benchmark_results"
TIMESTAMP="$(date +%Y%m%d_%H%M%S)"
REPORT_TXT="$RESULTS_DIR/report_${TIMESTAMP}.txt"
REPORT_JSON_DIR="$RESULTS_DIR/${TIMESTAMP}_json"

mkdir -p "$BUILD_DIR" "$RESULTS_DIR" "$REPORT_JSON_DIR"

# ── Validate environment ──────────────────────────────────────────────────────
if [[ -z "${VCPKG_ROOT:-}" ]]; then
    echo "ERROR: VCPKG_ROOT is not set. Export it and retry."
    exit 1
fi

# macOS uses sysctl; Linux uses nproc
NPROC="$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)"

BENCHMARKS=(
    bench_mpsc_queue
    bench_mutex_queue
    bench_spsc_queue
    bench_timing_overhead
)

# ── Build ─────────────────────────────────────────────────────────────────────
if [[ "$REBUILD" == true ]]; then
    echo "==> Configuring (Release, BUILD_BENCHMARKS=ON)..."
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DBUILD_BENCHMARKS=ON \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        2>&1

    echo ""
    echo "==> Building benchmark targets (j=$NPROC)..."
    cmake --build "$BUILD_DIR" --target "${BENCHMARKS[@]}" -- -j"$NPROC"
    echo ""
fi

# ── Verify binaries exist ─────────────────────────────────────────────────────
for bench in "${BENCHMARKS[@]}"; do
    bin="$BUILD_DIR/$bench"
    if [[ ! -x "$bin" ]]; then
        echo "ERROR: $bin not found or not executable. Run without --no-rebuild."
        exit 1
    fi
done

# ── Run benchmarks ────────────────────────────────────────────────────────────
COMMON_FLAGS=(
    "--benchmark_repetitions=$REPS"
    "--benchmark_report_aggregates_only=true"
)
[[ -n "$FILTER" ]] && COMMON_FLAGS+=("--benchmark_filter=$FILTER")

run_benchmark() {
    local bench="$1"
    local json_out="$REPORT_JSON_DIR/${bench}.json"

    echo ""
    echo "════════════════════════════════════════════════════════════════"
    echo "  $bench"
    echo "════════════════════════════════════════════════════════════════"

    "$BUILD_DIR/$bench" \
        "${COMMON_FLAGS[@]}" \
        --benchmark_out_format=json \
        --benchmark_out="$json_out" \
        2>&1

    echo ""
    echo "  JSON → $json_out"
}

{
    echo "TickPlant Benchmark Report"
    echo "Generated : $(date)"
    echo "Host      : $(hostname)"
    echo "Repetitions: $REPS"
    echo "Build type : Release"
    echo ""
    echo "Benchmarks : ${BENCHMARKS[*]}"
    [[ -n "$FILTER" ]] && echo "Filter     : $FILTER"
    echo ""
    uname -a
    echo ""

    for bench in "${BENCHMARKS[@]}"; do
        run_benchmark "$bench"
    done

    echo ""
    echo "════════════════════════════════════════════════════════════════"
    echo "  Done. Text report : $REPORT_TXT"
    echo "  JSON reports      : $REPORT_JSON_DIR/"
    echo "════════════════════════════════════════════════════════════════"
} 2>&1 | tee "$REPORT_TXT"
