#!/usr/bin/env bash
# perf 全自动：清 build → RelWithDebInfo 重编 → 起 server → wrk+perf → 火焰图
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
SERVER="${SERVER:-$BUILD_DIR/src/server/server}"
ARTIFACTS_DIR="${ARTIFACTS_DIR:-$ROOT/benchmark_log/artifacts}"
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$HOME/FlameGraph}"
SERVER_LOG="${SERVER_LOG:-/tmp/server.log}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}"

BENCH_NNN="003"
DURATION=30
WRK_THREADS=2
WRK_CONNECTIONS=20
WRK_URL="http://127.0.0.1:8888/"
PERF_FREQ=997
CALL_GRAPH="dwarf"
DO_WARMUP=1
DO_REBUILD=1
STOP_SERVER=0
SKIP_WRK=0
PERF_DATA=""
MODE="run"
SERVER_STARTED=0

usage() {
    cat <<'EOF'
用法: bash scripts/perf_bench.sh [选项] [子命令]

一条命令全自动（默认 run）:
  停 server → 删除 build/ → RelWithDebInfo 重编 → 起 server → wrk+perf → 火焰图

子命令:
  run          完整流程（默认）
  flamegraph   仅从已有 perf.data 生成 SVG + report
  check        检查依赖与当前二进制符号（不重编）

选项:
  -n <NNN>     BENCH 编号（默认 003）
  -d <秒>      wrk 与 perf 时长（默认 30）
  -t / -c      wrk 线程 / 连接数（默认 2 / 20）
  -u <url>     wrk URL（默认 http://127.0.0.1:8888/）
  -i <file>    perf.data 路径（flamegraph 子命令）
  -j <N>       cmake --build 并行数（默认 nproc）
  --skip-build 跳过删 build 与重编（已编好时）
  --fp         perf 用帧指针 -g（默认 dwarf）
  --no-warmup  跳过 wrk 5s 热身
  --stop-server  结束后 SIGTERM 停 server
  --skip-wrk   只 perf，不跑 wrk
  -h, --help

示例:
  bash scripts/perf_bench.sh -n 003
  bash scripts/perf_bench.sh --skip-build -n 003
  bash scripts/perf_bench.sh check
  bash scripts/perf_bench.sh flamegraph -n 003 -i benchmark_log/artifacts/BENCH-003_perf.data

环境变量: BUILD_DIR, BUILD_JOBS, FLAMEGRAPH_DIR, ARTIFACTS_DIR, SERVER_LOG
EOF
}

log() { printf '[perf_bench] %s\n' "$*"; }
warn() { printf '[perf_bench] 警告: %s\n' "$*" >&2; }
die() { printf '[perf_bench] 错误: %s\n' "$*" >&2; exit 1; }

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "未找到命令: $1"
}

ensure_flamegraph() {
    if [[ ! -x "$FLAMEGRAPH_DIR/stackcollapse-perf.pl" ]]; then
        if [[ -d "$FLAMEGRAPH_DIR/.git" ]]; then
            die "FlameGraph 目录不完整: $FLAMEGRAPH_DIR"
        fi
        log "克隆 FlameGraph → $FLAMEGRAPH_DIR"
        need_cmd git
        git clone --depth 1 https://github.com/brendangregg/FlameGraph.git "$FLAMEGRAPH_DIR"
    fi
    export PATH="$FLAMEGRAPH_DIR:$PATH"
    command -v stackcollapse-perf.pl >/dev/null || die "stackcollapse-perf.pl 不可用"
    command -v flamegraph.pl >/dev/null || die "flamegraph.pl 不可用"
}

check_memory() {
    local avail_kb
    avail_kb="$(awk '/MemAvailable:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    if [[ "$avail_kb" -gt 0 && "$avail_kb" -lt 204800 ]]; then
        warn "MemAvailable < 200 MiB ($(("avail_kb" / 1024)) MiB)，perf+wrk 可能 OOM"
    fi
}

verify_server_symbols() {
    [[ -x "$SERVER" ]] || die "未找到 server: $SERVER"
    if readelf -S "$SERVER" 2>/dev/null | grep -q '\.debug_info'; then
        log "server 含 .debug_info ✓"
    elif file "$SERVER" | grep -q 'with debug_info'; then
        log "server 含 debug_info ✓"
    else
        die "server 无调试符号，火焰图会出现 [unknown]；勿使用 --skip-build，或检查构建是否成功"
    fi
}

perf_cmd() {
    if [[ "$(id -u)" -eq 0 ]]; then
        perf "$@"
    elif perf record -e cycles -- sleep 0.01 >/dev/null 2>&1; then
        perf "$@"
    else
        sudo perf "$@"
    fi
}

server_pid() {
    pgrep -x server 2>/dev/null || true
}

stop_server() {
    local pid
    pid="$(server_pid)"
    if [[ -n "$pid" ]]; then
        log "停止 server PID=$pid"
        kill -SIGTERM "$pid" 2>/dev/null || true
        for _ in $(seq 1 20); do
            [[ -z "$(server_pid)" ]] && return 0
            sleep 0.25
        done
        warn "server 未在 5s 内退出，继续后续步骤"
    fi
}

rebuild_for_perf() {
    need_cmd cmake
    stop_server
    log "删除 $BUILD_DIR"
    rm -rf "$BUILD_DIR"
    log "RelWithDebInfo 配置（含 -fno-omit-frame-pointer -g）"
    cmake -S "$ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer -g" \
        -DCMAKE_C_FLAGS="-fno-omit-frame-pointer -g"
    log "编译 server（-j${BUILD_JOBS}）..."
    cmake --build "$BUILD_DIR" -j"$BUILD_JOBS"
    verify_server_symbols
}

wait_server_ready() {
    local i
    for i in $(seq 1 40); do
        if curl -sf -o /dev/null -m 1 "$WRK_URL"; then
            return 0
        fi
        sleep 0.25
    done
    die "server 未在 $WRK_URL 响应（见 $SERVER_LOG）"
}

start_server() {
    local pid
    pid="$(server_pid)"
    if [[ -n "$pid" ]]; then
        log "server 已运行 PID=$pid"
        return 0
    fi
    log "后台启动 server → $SERVER_LOG"
    "$SERVER" >"$SERVER_LOG" 2>&1 &
    SERVER_STARTED=1
    wait_server_ready
    pid="$(server_pid)"
    log "server PID=$pid"
}

run_wrk() {
    local out="$1"
    need_cmd wrk
    log "wrk -t${WRK_THREADS} -c${WRK_CONNECTIONS} -d${DURATION}s $WRK_URL"
    wrk -t"$WRK_THREADS" -c"$WRK_CONNECTIONS" -d"${DURATION}s" "$WRK_URL" | tee "$out"
}

run_sample() {
    local pid="$1"
    local perf_out="$ARTIFACTS_DIR/BENCH-${BENCH_NNN}_perf.data"
    local perf_args=(record -F "$PERF_FREQ" -p "$pid" -o "$perf_out")

    if [[ "$CALL_GRAPH" == "dwarf" ]]; then
        perf_args+=(--call-graph dwarf)
    else
        perf_args+=(-g)
    fi

    mkdir -p "$ARTIFACTS_DIR"
    log "perf 采样 ${DURATION}s → $perf_out (call-graph=$CALL_GRAPH)"
    # shellcheck disable=SC2068
    perf_cmd "${perf_args[@]}" -- sleep "$DURATION"
    PERF_DATA="$perf_out"
}

generate_flamegraph() {
    local data="$1"
    local svg="$ARTIFACTS_DIR/BENCH-${BENCH_NNN}_flamegraph.svg"
    [[ -f "$data" ]] || die "perf.data 不存在: $data"
    ensure_flamegraph
    log "生成火焰图 → $svg"
    perf_cmd script -i "$data" | stackcollapse-perf.pl | flamegraph.pl >"$svg"
    log "火焰图: $svg ($(wc -c <"$svg" | tr -d ' ') bytes)"
}

generate_report() {
    local data="$1"
    local report="$ARTIFACTS_DIR/BENCH-${BENCH_NNN}_perf_report.txt"
    log "perf report → $report"
    perf_cmd report -i "$data" --stdio -g --no-children | tee "$report" | head -80
    log "完整报告: $report"
}

cmd_check() {
    log "项目根: $ROOT"
    need_cmd cmake
    need_cmd perf
    need_cmd wrk
    need_cmd curl
    if [[ -x "$SERVER" ]]; then
        verify_server_symbols
    else
        warn "server 未构建: $SERVER（run 子命令会自动重编）"
    fi
    ensure_flamegraph
    check_memory
    local pid
    pid="$(server_pid)"
    [[ -n "$pid" ]] && log "server 运行中 PID=$pid" || log "server 未运行"
    log "检查通过"
}

cmd_flamegraph() {
    local data="${PERF_DATA:-}"
    if [[ -z "$data" ]]; then
        if [[ -f "$ARTIFACTS_DIR/BENCH-${BENCH_NNN}_perf.data" ]]; then
            data="$ARTIFACTS_DIR/BENCH-${BENCH_NNN}_perf.data"
        elif [[ -f "$ROOT/perf.data" ]]; then
            data="$ROOT/perf.data"
        else
            die "未找到 perf.data，请用 -i 指定"
        fi
    fi
    log "使用 perf.data: $data"
    mkdir -p "$ARTIFACTS_DIR"
    generate_flamegraph "$data"
    generate_report "$data"
}

cmd_run() {
    local wrk_out pid

    need_cmd cmake
    need_cmd perf
    need_cmd wrk
    need_cmd curl
    ensure_flamegraph
    check_memory
    mkdir -p "$ARTIFACTS_DIR"

    if [[ "$DO_REBUILD" -eq 1 ]]; then
        rebuild_for_perf
    else
        verify_server_symbols
        stop_server
    fi

    start_server

    pid="$(server_pid)"
    [[ -n "$pid" ]] || die "server 启动失败"
    log "采样目标 server PID=$pid"

    wrk_out="$ARTIFACTS_DIR/BENCH-${BENCH_NNN}_wrk.txt"

    if [[ "$DO_WARMUP" -eq 1 && "$SKIP_WRK" -eq 0 ]]; then
        log "wrk 热身 5s ..."
        wrk -t1 -c10 -d5s "$WRK_URL" >/dev/null
    fi

    if [[ "$SKIP_WRK" -eq 0 ]]; then
        log "wrk 与 perf 并行 ${DURATION}s ..."
        run_wrk "$wrk_out" &
        local wrk_pid=$!
        sleep 1
        run_sample "$pid"
        wait "$wrk_pid" || warn "wrk 退出码非 0"
        log "wrk 输出: $wrk_out"
    else
        run_sample "$pid"
    fi

    generate_flamegraph "$PERF_DATA"
    generate_report "$PERF_DATA"

    log "完成 → $ARTIFACTS_DIR"
    log "  wrk:        BENCH-${BENCH_NNN}_wrk.txt"
    log "  perf.data:  BENCH-${BENCH_NNN}_perf.data"
    log "  report:     BENCH-${BENCH_NNN}_perf_report.txt"
    log "  flamegraph: BENCH-${BENCH_NNN}_flamegraph.svg"

    if [[ "$STOP_SERVER" -eq 1 ]]; then
        stop_server
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        run | flamegraph | check)
            MODE="$1"
            shift
            ;;
        -n)
            BENCH_NNN="$2"
            shift 2
            ;;
        -d)
            DURATION="$2"
            shift 2
            ;;
        -t)
            WRK_THREADS="$2"
            shift 2
            ;;
        -c)
            WRK_CONNECTIONS="$2"
            shift 2
            ;;
        -u)
            WRK_URL="$2"
            shift 2
            ;;
        -i)
            PERF_DATA="$2"
            shift 2
            ;;
        -j)
            BUILD_JOBS="$2"
            shift 2
            ;;
        --skip-build)
            DO_REBUILD=0
            shift
            ;;
        --fp)
            CALL_GRAPH="fp"
            shift
            ;;
        --no-warmup)
            DO_WARMUP=0
            shift
            ;;
        --stop-server)
            STOP_SERVER=1
            shift
            ;;
        --skip-wrk)
            SKIP_WRK=1
            shift
            ;;
        --start-server)
            warn "--start-server 已废弃：run 默认自动停服、重编、起 server"
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        -*)
            die "未知选项: $1（-h 查看帮助）"
            ;;
        *)
            die "未知参数: $1"
            ;;
    esac
done

case "$MODE" in
    check) DO_REBUILD=0; cmd_check ;;
    flamegraph) DO_REBUILD=0; cmd_flamegraph ;;
    run) cmd_run ;;
    *) usage >&2; exit 1 ;;
esac
