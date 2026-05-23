#!/usr/bin/env bash
# perf + wrk 一键采样并生成火焰图（见 benchmark_log/README.md「perf 采样」）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
SERVER="${SERVER:-$BUILD_DIR/src/server/server}"
ARTIFACTS_DIR="${ARTIFACTS_DIR:-$ROOT/benchmark_log/artifacts}"
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$HOME/FlameGraph}"
SERVER_LOG="${SERVER_LOG:-/tmp/server.log}"

BENCH_NNN="003"
DURATION=30
WRK_THREADS=2
WRK_CONNECTIONS=20
WRK_URL="http://127.0.0.1:8888/"
PERF_FREQ=997
CALL_GRAPH="dwarf"
DO_WARMUP=1
START_SERVER=0
STOP_SERVER=0
SKIP_WRK=0
PERF_DATA=""
MODE="run"

usage() {
    cat <<'EOF'
用法: bash scripts/perf_bench.sh [选项] [子命令]

子命令:
  run          完整流程：检查依赖 → wrk 压测 + perf 采样 → 火焰图 + 文本报告（默认）
  flamegraph   仅从已有 perf.data 生成 SVG（须 -i 或项目根目录下的 perf.data）
  check        只检查 perf / wrk / FlameGraph / server 二进制

选项:
  -n <NNN>     BENCH 编号，产物前缀 BENCH-NNN（默认 003）
  -d <秒>      wrk 与 perf 采样时长（默认 30）
  -t <threads> wrk -t（默认 2）
  -c <conn>    wrk -c（默认 20）
  -u <url>     wrk 目标 URL（默认 http://127.0.0.1:8888/）
  -i <file>    已有 perf.data 路径（flamegraph 子命令）
  --fp         perf 使用 -g（帧指针栈）；默认 --call-graph dwarf，符号更全
  --no-warmup  跳过 wrk 5s 热身
  --start-server  若 server 未运行则后台启动并重定向日志到 /tmp/server.log
  --stop-server   采样结束后 SIGTERM 停掉 server（仅当本脚本 --start-server 拉起时）
  --skip-wrk   不跑 wrk（server 已在其他负载下时可试）
  -h, --help   显示本帮助

示例:
  bash scripts/perf_bench.sh
  bash scripts/perf_bench.sh -n 003 -d 30
  bash scripts/perf_bench.sh --start-server
  bash scripts/perf_bench.sh check
  bash scripts/perf_bench.sh flamegraph -n 003 -i ./perf.data

环境变量:
  BUILD_DIR, FLAMEGRAPH_DIR, ARTIFACTS_DIR, SERVER_LOG

说明:
  - 默认 dwarf 栈展开，减轻火焰图 [unknown]
  - 建议二进制为 RelWithDebInfo；无符号时会警告但仍继续
  - 产物: benchmark_log/artifacts/BENCH-NNN_flamegraph.svg 等（大文件默认 gitignore）
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
        log "克隆 FlameGraph 到 $FLAMEGRAPH_DIR ..."
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
        warn "MemAvailable < 200 MiB ($(("$avail_kb" / 1024)) MiB)，perf+wrk 可能导致 swap/OOM"
    fi
}

check_server_binary() {
    [[ -x "$SERVER" ]] || die "未找到 server: $SERVER（请先 RelWithDebInfo 构建）"
    if readelf -S "$SERVER" 2>/dev/null | grep -q '\.debug_info'; then
        log "server 含 .debug_info"
    elif file "$SERVER" | grep -q 'with debug_info'; then
        log "server 含 debug_info"
    else
        warn "server 可能无调试符号；火焰图易出现 [unknown]。建议:"
        warn "  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \\"
        warn "    -DCMAKE_CXX_FLAGS=\"-fno-omit-frame-pointer -g\" && cmake --build build -j2"
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

wait_server_ready() {
    local i
    for i in $(seq 1 20); do
        if curl -sf -o /dev/null -m 1 "$WRK_URL"; then
            return 0
        fi
        sleep 0.25
    done
    die "server 未在 $WRK_URL 响应（见 $SERVER_LOG）"
}

start_server_if_needed() {
    local pid
    pid="$(server_pid)"
    if [[ -n "$pid" ]]; then
        log "server 已运行 PID=$pid"
        return 0
    fi
    [[ "$START_SERVER" -eq 1 ]] || die "server 未运行。请先启动 server，或加 --start-server"
    log "后台启动 server → $SERVER_LOG"
    "$SERVER" >"$SERVER_LOG" 2>&1 &
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
    need_cmd stackcollapse-perf.pl
    need_cmd flamegraph.pl
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
    need_cmd perf
    need_cmd wrk
    need_cmd curl
    check_server_binary
    ensure_flamegraph
    log "stackcollapse-perf.pl=$(command -v stackcollapse-perf.pl)"
    log "flamegraph.pl=$(command -v flamegraph.pl)"
    check_memory
    local pid
    pid="$(server_pid)"
    if [[ -n "$pid" ]]; then
        log "server 运行中 PID=$pid"
    else
        warn "server 未运行"
    fi
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
            die "未找到 perf.data，请用 -i 指定路径"
        fi
    fi
    log "使用 perf.data: $data"
    mkdir -p "$ARTIFACTS_DIR"
    generate_flamegraph "$data"
    generate_report "$data"
}

cmd_run() {
    local wrk_out report started=0
    local pid

    need_cmd perf
    need_cmd wrk
    need_cmd curl
    check_server_binary
    ensure_flamegraph
    check_memory
    mkdir -p "$ARTIFACTS_DIR"

    if [[ "$START_SERVER" -eq 1 ]]; then
        start_server_if_needed
        started=1
    fi

    pid="$(server_pid)"
    [[ -n "$pid" ]] || die "server 未运行（加 --start-server 或手动启动）"
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

    log "完成。产物目录: $ARTIFACTS_DIR"
    log "  wrk:        BENCH-${BENCH_NNN}_wrk.txt"
    log "  perf.data:  BENCH-${BENCH_NNN}_perf.data"
    log "  report:     BENCH-${BENCH_NNN}_perf_report.txt"
    log "  flamegraph: BENCH-${BENCH_NNN}_flamegraph.svg"

    if [[ "$started" -eq 1 && "$STOP_SERVER" -eq 1 ]]; then
        local spid
        spid="$(server_pid)"
        if [[ -n "$spid" ]]; then
            log "停止本脚本拉起的 server PID=$spid"
            kill -SIGTERM "$spid" 2>/dev/null || true
            wait "$spid" 2>/dev/null || true
        fi
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
        --fp)
            CALL_GRAPH="fp"
            shift
            ;;
        --no-warmup)
            DO_WARMUP=0
            shift
            ;;
        --start-server)
            START_SERVER=1
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
    check) cmd_check ;;
    flamegraph) cmd_flamegraph ;;
    run) cmd_run ;;
    *) usage >&2; exit 1 ;;
esac
