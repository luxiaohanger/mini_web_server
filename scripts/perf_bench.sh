#!/usr/bin/env bash
# perf 全自动：清 build → RelWithDebInfo 重编 → 起 server → wrk+perf → 符号表 + 火焰图
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
SERVER="${SERVER:-$BUILD_DIR/src/server/server}"
ARTIFACTS_DIR="${ARTIFACTS_DIR:-$ROOT/benchmark_log/artifacts}"
FLAMEGRAPH_DIR="${FLAMEGRAPH_DIR:-$HOME/FlameGraph}"
SERVER_LOG="${SERVER_LOG:-/tmp/server.log}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc 2>/dev/null || echo 2)}"

DESIGN_VER=""
DURATION=30
WRK_THREADS=2
WRK_CONNECTIONS=20
WRK_URL="http://127.0.0.1:8888/"
PERF_FREQ=997
CALL_GRAPH="dwarf"
PERF_REPORT_PERCENT_LIMIT="${PERF_REPORT_PERCENT_LIMIT:-0.1}"
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

子命令（默认 run）:
  run          停 server → 删 build → RelWithDebInfo 重编 → wrk+perf → 符号表 + SVG
  flamegraph   仅从已有 perf.data 生成符号表 + SVG（不重编、不重采样）
  check        检查依赖、server 调试符号、FlameGraph（不采样）

选项:
  -v <版本>      设计版本（run/flamegraph 必填），如 v10.0
  -d <秒>        wrk 与 perf 时长（默认 30）
  -t / -c        wrk 线程数 / 连接数（默认 2 / 20）
  -u <url>       wrk URL（默认 http://127.0.0.1:8888/）
  -i <file>      perf.data 路径（flamegraph；默认 artifacts/{版本}_perf.data）
  -j <N>         cmake --build 并行数（默认 nproc）
  --skip-build   跳过重编（仍 wrk+perf 或 flamegraph）
  --fp           perf 栈展开用帧指针 -g（默认 dwarf）
  --no-warmup    跳过 wrk 5s 热身
  --stop-server  结束后 SIGTERM 停 server
  --skip-wrk     只 perf record，不跑 wrk
  -h, --help     本帮助

环境变量:
  BUILD_DIR                  构建目录（默认 build/）
  ARTIFACTS_DIR              产物目录（默认 benchmark_log/artifacts）
  FLAMEGRAPH_DIR             FlameGraph 路径（默认 ~/FlameGraph）
  SERVER_LOG                 server 日志（默认 /tmp/server.log）
  PERF_REPORT_PERCENT_LIMIT  符号表阈值 %（默认 0.1）
  PERF_BENCH_FORCE=1           跳过覆盖确认

符号表: 分层（§1 用户发起→内核类别 + §2 server All/Self）；调用链见 SVG。
详情: scripts/SCRIPTS.md

示例:
  bash scripts/perf_bench.sh -v v10.0
  bash scripts/perf_bench.sh --skip-build -v v10.0
  bash scripts/perf_bench.sh flamegraph -v v10.0
  bash scripts/perf_bench.sh check
EOF
}

log() { printf '[perf_bench] %s\n' "$*"; }
warn() { printf '[perf_bench] 警告: %s\n' "$*" >&2; }
die() { printf '[perf_bench] 错误: %s\n' "$*" >&2; exit 1; }

need_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "未找到命令: $1"
}

require_design_version() {
    [[ -n "$DESIGN_VER" ]] || die "缺少必需参数 -v <版本>（如 -v v10.0）"
    [[ "$DESIGN_VER" =~ ^v[0-9]+(\.[0-9]+)?$ ]] || die "版本格式应为 vN 或 v10.x: $DESIGN_VER"
}

artifact_base() {
    echo "$DESIGN_VER"
}

# 列出本命令即将写入的产物路径（每行一个）
artifact_output_paths() {
    local mode="$1"
    local base
    base="$(artifact_base)"
    if [[ "$mode" == "flamegraph" ]]; then
        echo "$ARTIFACTS_DIR/${base}_perf_report.txt"
        echo "$ARTIFACTS_DIR/${base}_flamegraph.svg"
        return
    fi
    echo "$ARTIFACTS_DIR/${base}_perf.data"
    echo "$ARTIFACTS_DIR/${base}_perf_report.txt"
    echo "$ARTIFACTS_DIR/${base}_flamegraph.svg"
    if [[ "$SKIP_WRK" -eq 0 ]]; then
        echo "$ARTIFACTS_DIR/${base}_wrk.txt"
    fi
}

remove_artifacts_to_overwrite() {
    local mode="$1"
    local p
    while IFS= read -r p; do
        if [[ -f "$p" ]]; then
            rm -f "$p"
            log "已删除旧产物: $p"
        fi
    done < <(artifact_output_paths "$mode")
}

confirm_overwrite_if_needed() {
    local mode="$1"
    local existing=() p ans
    while IFS= read -r p; do
        [[ -f "$p" ]] && existing+=("$p")
    done < <(artifact_output_paths "$mode")

    [[ ${#existing[@]} -eq 0 ]] && return 0

    if [[ "${PERF_BENCH_FORCE:-0}" == "1" ]]; then
        warn "PERF_BENCH_FORCE=1，将覆盖 ${#existing[@]} 个已有产物"
    else
        log "以下产物已存在:"
        for p in "${existing[@]}"; do
            printf '  %s\n' "$p"
        done

        while true; do
            if ! read -r -p '[perf_bench] 覆盖? [y/N] ' ans; then
                die "已取消（非交互环境请删除产物或设 PERF_BENCH_FORCE=1）"
            fi
            case "$ans" in
                y | Y) break ;;
                n | N | "") die "已取消" ;;
                *) warn "请输入 y 或 n" ;;
            esac
        done
    fi

    remove_artifacts_to_overwrite "$mode"
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
        die "server 无调试符号；请勿 --skip-build，或检查 RelWithDebInfo 构建"
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
    log "wrk -t${WRK_THREADS} -c${WRK_CONNECTIONS} -d${DURATION}s $WRK_URL → $out"
    wrk -t"$WRK_THREADS" -c"$WRK_CONNECTIONS" -d"${DURATION}s" "$WRK_URL" >"$out"
}

run_sample() {
    local pid="$1"
    local base perf_out
    base="$(artifact_base)"
    perf_out="$ARTIFACTS_DIR/${base}_perf.data"
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

# 将 perf script（含调用栈）写入文件；优先 -F/-f trace，不可用时回退默认（与火焰图同口径）
perf_script_with_stacks() {
    local data="$1" dest="$2"
    local err="${dest}.err"
    local fields=comm,pid,tid,cpu,time,event,ip,sym,dso,trace

    if perf_cmd script -i "$data" -F "$fields" >"$dest" 2>"$err"; then
        rm -f "$err"
        return 0
    fi
    if perf_cmd script -i "$data" -f "$fields" >"$dest" 2>"$err"; then
        log "perf script 使用 -f 字段格式（本机不支持 -F trace）"
        rm -f "$err"
        return 0
    fi
    if perf_cmd script -i "$data" >"$dest" 2>"$err"; then
        log "perf script 使用默认输出（本机不支持 trace 字段，与火焰图相同）"
        rm -f "$err"
        return 0
    fi
    warn "perf script stderr: $(head -5 "$err" 2>/dev/null || true)"
    rm -f "$err"
    return 1
}

generate_flamegraph() {
    local data="$1"
    local base svg tmp
    base="$(artifact_base)"
    svg="$ARTIFACTS_DIR/${base}_flamegraph.svg"
    [[ -f "$data" ]] || die "perf.data 不存在: $data"
    ensure_flamegraph
    log "生成火焰图 → $svg"
    tmp="$(mktemp "${svg}.script.XXXXXX")"
    perf_script_with_stacks "$data" "$tmp" || die "perf script 失败"
    stackcollapse-perf.pl <"$tmp" | flamegraph.pl >"$svg"
    rm -f "$tmp"
    log "火焰图: $svg ($(wc -c <"$svg" | tr -d ' ') bytes)"
}

# 判断 perf report 是否为 flat 符号表（非调用树）
# 部分 perf 版本表头为 Overhead；部分为 Children/Self（提取后 Children 归一为 All）
is_flat_symbol_report() {
    local file="$1"
    [[ -f "$file" ]] || return 1
    if grep -qE '^[[:space:]]+\|--' "$file"; then
        return 1
    fi
    if grep -qE '^# Overhead[[:space:]]' "$file"; then
        return 0
    fi
    if grep -qE '^# Children[[:space:]]+Self' "$file"; then
        return 0
    fi
    return 1
}

# 从 perf report 输出提取表头与数据行；Children 列名归一为 All
extract_perf_table() {
    local file="$1"
    awk '
        /^# (Overhead|Children)/ {
            if ($0 ~ /^# Children/) {
                sub(/^# Children/, "# All")
            }
            show = 1
        }
        show && /^# \(Cannot load tips/ { exit }
        show { print }
    ' "$file"
}

# §2 精简为 All / Self / Symbol；只保留 server DSO 行（All 来自全量 inclusive，含内核路径）
compact_user_symbol_table() {
    local server_dso="${1:-server}"
    awk -v server_dso="$server_dso" '
    function is_server_dso(dso,    n, base) {
        if (dso == server_dso) {
            return 1
        }
        n = split(dso, parts, "/")
        base = parts[n]
        return (base == server_dso)
    }
    BEGIN { hdr = 0 }
    /^# (Overhead|Children|All)/ {
        if ($0 ~ /Shared Object/) {
            print "# All      Self  Symbol"
            hdr = 1
        } else if ($0 ~ /Self/) {
            print "# All      Self  Symbol"
            hdr = 2
        } else if ($0 ~ /^# Overhead/) {
            print "# All      Self  Symbol"
            hdr = 3
        }
        next
    }
    /^# \./ { next }
    /^# \(Cannot load tips/ { exit }
    hdr == 1 && /^[[:space:]]+[0-9]/ {
        if (!is_server_dso($4)) {
            next
        }
        sym = $5
        for (i = 6; i <= NF; i++) {
            if ($i == "-" && i < NF && $(i + 1) == "-") {
                break
            }
            sym = sym " " $i
        }
        sub(/^\[\.\][[:space:]]+/, "", sym)
        printf " %7s  %7s  %s\n", $1, $2, sym
        next
    }
    hdr == 2 && /^[[:space:]]+[0-9]/ {
        sym = $3
        for (i = 4; i <= NF; i++) {
            if ($i == "-" && i < NF && $(i + 1) == "-") {
                break
            }
            sym = sym " " $i
        }
        sub(/^\[\.\][[:space:]]+/, "", sym)
        printf " %7s  %7s  %s\n", $1, $2, sym
        next
    }
    hdr == 3 && /^[[:space:]]+[0-9]/ {
        sym = $2
        for (i = 3; i <= NF; i++) {
            if ($i == "-" && i < NF && $(i + 1) == "-") {
                break
            }
            sym = sym " " $i
        }
        sub(/^\[\.\][[:space:]]+/, "", sym)
        printf " %7s  %7s  %s\n", $1, "0.00%", sym
    }
    '
}

run_perf_report_flat() {
    local data="$1" dest="$2"
    shift 2
    local tmp err
    tmp="$(mktemp "${dest}.XXXXXX")"
    err="${tmp}.err"

    if ! perf_cmd report -i "$data" "$@" >"$tmp" 2>"$err"; then
        warn "perf report stderr: $(head -5 "$err" 2>/dev/null || true)"
        rm -f "$tmp" "$err"
        return 1
    fi
    rm -f "$err"
    if ! is_flat_symbol_report "$tmp"; then
        warn "perf report 前几行:"
        head -20 "$tmp" >&2 || true
        rm -f "$tmp"
        return 1
    fi
    extract_perf_table "$tmp" >"$dest"
    rm -f "$tmp"
    return 0
}

append_kernel_initiator_table() {
    local data="$1" limit="$2" report="$3"
    local tmp folded sc_err
    ensure_flamegraph
    tmp="$(mktemp "${report}.script.XXXXXX")"
    folded="$(mktemp "${report}.folded.XXXXXX")"
    sc_err="${tmp}.sc.err"

    if ! perf_script_with_stacks "$data" "$tmp"; then
        rm -f "$tmp" "$folded" "$sc_err"
        die "perf script (§1 用户→内核) 失败"
    fi

    if ! stackcollapse-perf.pl --kernel <"$tmp" >"$folded" 2>"$sc_err"; then
        warn "stackcollapse-perf.pl stderr: $(head -5 "$sc_err" 2>/dev/null || true)"
        rm -f "$tmp" "$folded" "$sc_err"
        die "stackcollapse-perf.pl (§1) 失败"
    fi
    rm -f "$sc_err" "$tmp"

    # §1：每条样本栈 — 找最靠 syscall 的 server 发起帧 + syscall 类别（read/write/epoll/futex…）
    # 栈序与火焰图一致：左=root caller，右=leaf（采样点）
    awk -v limit="$limit" '
    function strip_frame(name) {
        sub(/_\[k\]$/, "", name)
        sub(/_\[k\]\+0x[0-9a-fA-F]+$/, "", name)
        return name
    }
    function stack_has_kernel(frames, n,    i) {
        for (i = 1; i <= n; i++) {
            if (frames[i] ~ /_\[k\]$/) {
                return 1
            }
        }
        return 0
    }
    function is_syscall_entry(sym) {
        return (sym ~ /^__x64_sys_/ || sym ~ /^__arm64_sys_/ || sym ~ /^__se_sys_/ \
            || sym ~ /^__do_sys_/ || sym ~ /^__do_compat_sys_/)
    }
    function is_server_frame(sym) {
        if (sym ~ /_\[k\]$/) {
            return 0
        }
        if (sym ~ /^__GI_/ || sym ~ /^__libc_/ || sym ~ /^pthread_/ || sym ~ /^std::/) {
            return 0
        }
        if (sym ~ /^read$|^write$|^readv$|^writev$|^close$|^ioctl$/) {
            return 0
        }
        if (sym ~ /::/) {
            return 1
        }
        if (sym ~ /^(Connection|EventLoop|Buffer|Epoll|TimerQueue|ThreadPool|Socket|Channel|HttpProcess|Acceptor|MainReactor|SubReactor|Server|Timer)/) {
            return 1
        }
        return 0
    }
    function find_syscall(frames, n,    i, sym) {
        for (i = 1; i <= n; i++) {
            if (frames[i] !~ /_\[k\]$/) {
                continue
            }
            sym = strip_frame(frames[i])
            if (is_syscall_entry(sym)) {
                return sym SUBSEP i
            }
        }
        for (i = n; i >= 1; i--) {
            if (frames[i] !~ /_\[k\]$/) {
                continue
            }
            sym = strip_frame(frames[i])
            if (is_syscall_entry(sym)) {
                return sym SUBSEP i
            }
        }
        return SUBSEP 0
    }
    function find_initiator(frames, sc_idx,    j, sym) {
        for (j = sc_idx - 1; j >= 1; j--) {
            sym = strip_frame(frames[j])
            if (is_server_frame(sym)) {
                return sym
            }
        }
        return ""
    }
    function syscall_category(sc_sym,    s) {
        s = sc_sym
        sub(/^__x64_sys_/, "", s)
        sub(/^__arm64_sys_/, "", s)
        sub(/^__se_sys_/, "", s)
        sub(/^__do_sys_/, "", s)
        sub(/^__do_compat_sys_/, "", s)
        if (s ~ /^readv/) {
            return "readv"
        }
        if (s ~ /^read$/) {
            return "read"
        }
        if (s ~ /^write$|^pwrite/) {
            return "write"
        }
        if (s ~ /^epoll_wait|^epoll_pwait/) {
            return "epoll"
        }
        if (s ~ /^futex/) {
            return "futex"
        }
        if (s ~ /^accept4?$/) {
            return "accept"
        }
        if (s ~ /^sendmsg|^sendto|^send/) {
            return "send"
        }
        if (s ~ /^recvmsg|^recvfrom|^recv/) {
            return "recv"
        }
        if (s ~ /^ppoll|^poll/) {
            return "poll"
        }
        if (s ~ /^ioctl/) {
            return "ioctl"
        }
        if (s ~ /^clock_gettime/) {
            return "clock"
        }
        if (s ~ /^close/) {
            return "close"
        }
        if (s ~ /^setsockopt|^getsockopt/) {
            return "sockopt"
        }
        return s
    }
    function find_user_sync(frames, n, sc_idx,    j, sym) {
        for (j = (sc_idx > 0 ? sc_idx - 1 : n); j >= 1; j--) {
            sym = strip_frame(frames[j])
            if (sym ~ /^pthread_mutex|^pthread_cond|^__lll_lock|^__pthread_mutex/) {
                return "futex"
            }
        }
        return ""
    }
    function sort_order(pct, order, nout,    i, j, t) {
        for (i = 1; i <= nout; i++) {
            for (j = i + 1; j <= nout; j++) {
                if (pct[order[j]] > pct[order[i]]) {
                    t = order[i]; order[i] = order[j]; order[j] = t
                }
            }
        }
    }
    {
        wt = $2 + 0
        if (wt <= 0) {
            wt = 1
        }
        total_samples += wt
        n = split($1, frames, ";")
        if (!stack_has_kernel(frames, n)) {
            pure_user += wt
            next
        }
        sc_pair = find_syscall(frames, n)
        split(sc_pair, sc_parts, SUBSEP)
        sc_sym = sc_parts[1]
        sc_idx = sc_parts[2] + 0
        category = ""
        if (sc_sym != "") {
            category = syscall_category(sc_sym)
        } else {
            category = find_user_sync(frames, n, sc_idx)
        }
        if (category == "") {
            unpaired_kernel += wt
            next
        }
        initiator = find_initiator(frames, sc_idx)
        if (initiator == "") {
            initiator = "[no-server-frame]"
        }
        detail_key = initiator SUBSEP category
        detail_wt[detail_key] += wt
        cat_wt[category] += wt
    }
    END {
        if (total_samples == 0) {
            print "# (无调用栈样本；检查 perf.data 是否含 call-graph)"
            exit
        }
        printf "# Overhead  Category  Initiator (server)\n"
        nd = 0
        for (k in detail_wt) {
            split(k, parts, SUBSEP)
            pct_d[k] = detail_wt[k] * 100.0 / total_samples
            if (pct_d[k] + 0 >= limit + 0) {
                detail_cat[k] = parts[2]
                detail_init[k] = parts[1]
                order_d[++nd] = k
            }
        }
        sort_order(pct_d, order_d, nd)
        if (nd == 0) {
            print "# (无达到阈值的 发起函数→类别 配对；见下方汇总与脚注)"
        } else {
            for (i = 1; i <= nd; i++) {
                k = order_d[i]
                printf " %7.2f%%  %-8s  %s\n", pct_d[k], detail_cat[k], detail_init[k]
            }
        }
        printf "#\n# --- 按类别合并（read / write / epoll / futex …）---\n"
        printf "# Overhead  Category\n"
        nc = 0
        for (c in cat_wt) {
            pct_c[c] = cat_wt[c] * 100.0 / total_samples
            if (pct_c[c] + 0 >= limit + 0) {
                order_c[++nc] = c
            }
        }
        sort_order(pct_c, order_c, nc)
        for (i = 1; i <= nc; i++) {
            c = order_c[i]
            printf " %7.2f%%  %s\n", pct_c[c], c
        }
        printf "#\n"
        if (pure_user + 0 > 0) {
            printf "# 纯用户态（栈无内核帧）: %.2f%%\n", pure_user * 100.0 / total_samples
        }
        if (unpaired_kernel + 0 > 0) {
            printf "# 未配对（有内核帧但无 syscall/同步类别）: %.2f%%\n", \
                unpaired_kernel * 100.0 / total_samples
        }
        printf "# 说明: read 含 eventfd/timerfd 的 read(2)；write 含 socket 写与 eventfd 唤醒；futex 含 mutex/cv\n"
    }
    ' "$folded" >>"$report"
    rm -f "$folded"
}

append_user_server_table() {
    local data="$1" limit="$2" report="$3"
    local tmp_user server_dso
    tmp_user="$(mktemp "${report}.user.XXXXXX")"
    server_dso="$(basename "$SERVER")"

    # 全量 inclusive（不用 --dsos），再筛 server 行，保证 All 含 libc/内核 callees
    if ! run_perf_report_flat "$data" "$tmp_user" --stdio --sort comm,dso,symbol \
        --percent-limit "$limit" -g none; then
        rm -f "$tmp_user"
        die "perf report (用户态) 失败"
    fi
    compact_user_symbol_table "$server_dso" <"$tmp_user" >>"$report"
    rm -f "$tmp_user"
}

generate_report() {
    local data="$1"
    local base report limit lines
    base="$(artifact_base)"
    report="$ARTIFACTS_DIR/${base}_perf_report.txt"
    limit="$PERF_REPORT_PERCENT_LIMIT"
    log "perf 符号表 → $report（分层：§1 用户→内核类别 + §2 server，≥ ${limit}%）"
    {
        printf '# mini_web_server perf 符号表（分层摘要）\n'
        printf '# 版本: %s\n' "$base"
        printf '#\n'
        printf '# 结构:\n'
        printf '#   §1 内核态 — 按 server 发起函数 → 类别（read/write/readv/epoll/futex…）配对统计\n'
        printf '#            同火焰图栈（perf script + stackcollapse --kernel）；分母=全部 perf 样本\n'
        printf '#            明细表 + 「按类别合并」；纯用户态/未配对见 §1 脚注\n'
        printf '#   §2 用户态 (server) — 本程序符号，含 All + Self\n'
        printf '#\n'
        printf '# §1 口径: Overhead = 该配对样本数 / 全部 perf 样本数（每样本只归一条发起→类别）\n'
        printf '# §2 口径: 全量 inclusive 后筛 server 行（不用 --dsos）；All/Overhead 分母为全部样本\n'
        printf '#   All  = 含子函数 + 内核/ libc 路径的总占比（排序依据；perf 原始列名 Children）\n'
        printf '#   Self = 仅 PC 落在该函数 server 体内（参考）；勿把 Self+All 相加\n'
        printf '# 过滤: >= %s%%  |  -g none（§2 无 |--- 调用树）\n' "$limit"
        printf '# 调用链: %s_flamegraph.svg\n' "$base"
        printf '# 原始: %s\n#\n' "$(basename "$data")"
        printf '# === §1 内核态（用户发起函数 → 类别） ===\n'
        printf '# 方法: 栈上最靠 syscall 的 server 帧 + __x64_sys_* 归类；read 含 eventfd/timerfd\n#\n'
    } >"$report"
    append_kernel_initiator_table "$data" "$limit" "$report"
    {
        printf '\n# === §2 用户态明细 (Shared Object: server) ===\n'
        printf '# 命令: perf report --sort comm,dso,symbol --percent-limit %s -g none（筛 Shared Object=%s）\n#\n' \
            "$limit" "$(basename "$SERVER")"
    } >>"$report"
    append_user_server_table "$data" "$limit" "$report"
    lines="$(wc -l <"$report" | tr -d ' ')"
    log "符号表: $report (${lines} 行)"
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
    require_design_version
    local data="${PERF_DATA:-}"
    local base
    base="$(artifact_base)"
    if [[ -z "$data" ]]; then
        if [[ -f "$ARTIFACTS_DIR/${base}_perf.data" ]]; then
            data="$ARTIFACTS_DIR/${base}_perf.data"
        elif [[ -f "$ROOT/perf.data" ]]; then
            data="$ROOT/perf.data"
        else
            die "未找到 perf.data: $ARTIFACTS_DIR/${base}_perf.data"
        fi
    fi
    mkdir -p "$ARTIFACTS_DIR"
    confirm_overwrite_if_needed flamegraph
    log "使用 perf.data: $data"
    generate_flamegraph "$data"
    generate_report "$data"
}

cmd_run() {
    local wrk_out pid base

    require_design_version
    base="$(artifact_base)"
    log "设计版本: $DESIGN_VER"

    need_cmd cmake
    need_cmd perf
    need_cmd wrk
    need_cmd curl
    ensure_flamegraph
    check_memory
    mkdir -p "$ARTIFACTS_DIR"
    confirm_overwrite_if_needed run

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

    wrk_out="$ARTIFACTS_DIR/${base}_wrk.txt"

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
    log "  wrk:        ${base}_wrk.txt"
    log "  perf.data:  ${base}_perf.data"
    log "  report:     ${base}_perf_report.txt（§1 用户→内核 + §2 server，≥${PERF_REPORT_PERCENT_LIMIT}%）"
    log "  flamegraph: ${base}_flamegraph.svg（调用链）"
    log "请更新或新建 benchmark_log/${DESIGN_VER}_YYYYMMDD_bench.md（wrk + perf 同一份）"

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
        -v)
            DESIGN_VER="$2"
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
        -s | -n)
            die "已移除 -s / -n 参数；请仅用 -v <版本>，产物为 {版本}_*"
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
