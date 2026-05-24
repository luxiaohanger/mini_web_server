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
  PERF_REPORT_PERCENT_LIMIT  符号表阈值 %（默认 0.1，§0 不受限）
  PERF_BENCH_FORCE=1           跳过覆盖确认

符号表: §0 预算 + §1 server + §2/§3 kernel + §4 libc；调用链见 SVG。
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

# 从 perf report flat（--sort comm,dso,symbol -g none）一次解析并写入 §0～§4。
render_unified_perf_report() {
    local flat="$1" limit="$2" report="$3"
    local server_dso
    server_dso="$(basename "$SERVER")"

    awk -v limit="$limit" -v server_dso="$server_dso" '
    function pct_val(s,    t) {
        t = s
        sub(/%/, "", t)
        return t + 0
    }
    function sym_clean(s,    t) {
        t = s
        sub(/^\[\.\][[:space:]]+/, "", t)
        sub(/^\[k\][[:space:]]+/, "", t)
        sub(/\+0x[0-9a-fA-F]+$/, "", t)
        return t
    }
    function kern_base(sym,    s) {
        s = sym_clean(sym)
        sub(/\.isra\.[0-9]+$/, "", s)
        sub(/\.constprop\.[0-9]+$/, "", s)
        sub(/\.part\.[0-9]+$/, "", s)
        return s
    }
    function sym_from_fields(start,    i, sym) {
        sym = $start
        for (i = start + 1; i <= NF; i++) {
            if ($i == "-" && i < NF && $(i + 1) == "-") {
                break
            }
            sym = sym " " $i
        }
        return sym_clean(sym)
    }
    function is_dso_token(s) {
        if (s == "" || s ~ /%/) {
            return 0
        }
        return (s ~ /^\// || s ~ /^\[/ || s ~ /\.so/ || s == server_dso || s ~ /^server$/)
    }
    function is_server_dso(dso,    n, base) {
        if (dso == server_dso) {
            return 1
        }
        n = split(dso, parts, "/")
        base = parts[n]
        return (base == server_dso)
    }
    function dso_bucket(dso,    n, base) {
        if (dso ~ /\[kernel\.kallsyms\]|vmlinux|\[kernel\]|kallsyms/) {
            return "kernel"
        }
        if (dso ~ /\[vdso\]|vdso\.so/) {
            return "vdso"
        }
        if (dso ~ /libpthread/) {
            return "libpthread"
        }
        if (dso ~ /libstdc\+\+/) {
            return "libstdc++"
        }
        if (dso ~ /libc\.|\/libc-/) {
            return "libc"
        }
        n = split(dso, parts, "/")
        base = parts[n]
        if (base == server_dso || dso ~ /\/server$/) {
            return "server"
        }
        if (dso ~ /ld-linux|ld\.so/) {
            return "ldso"
        }
        return "other"
    }
    function is_kernel_dso(dso) {
        return (dso ~ /\[kernel\.kallsyms\]|vmlinux|\[kernel\]|kallsyms/)
    }
    function is_libc_dso(dso) {
        return (dso ~ /libc\.|\/libc-/)
    }
    function kernel_category(sym,    s) {
        s = kern_base(sym)
        if (s ~ /^__x64_sys_|^__arm64_sys_|^__se_sys_|^__do_sys_|^__do_compat_sys_|^syscall|^do_syscall_64$|^x64_sys_call$|^ksys_/) {
            return "syscall"
        }
        if (s ~ /^tcp_|^udp_|^ip_|^sock_|^sk_|^netif_|^dev_hard_start_xmit|^loopback_xmit|^eth_|^napi_|^inet_|^nf_|^net_rx_action$|^__dev_queue_xmit$|^process_backlog$/) {
            return "network"
        }
        if (s ~ /^futex|^do_futex|^get_futex_key/) {
            return "futex"
        }
        if (s ~ /^schedule$|^__schedule|^context_switch|^rq_|^pick_next_task|^finish_task_switch/) {
            return "sched"
        }
        return "other"
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
    function top_syms_for_cat(cat,    s, n, i, line) {
        n = 0
        for (s in ksym_self) {
            if (kernel_category(s) != cat) {
                continue
            }
            order_t[++n] = s
            pct_t[s] = ksym_self[s]
        }
        if (n == 0) {
            return "-"
        }
        sort_order(pct_t, order_t, n)
        if (n > 3) {
            n = 3
        }
        line = ""
        for (i = 1; i <= n; i++) {
            if (line != "") {
                line = line ", "
            }
            line = line sym_clean(order_t[i])
        }
        delete order_t
        delete pct_t
        return line
    }
    function ingest_row(allpct, selfpct, dso, sym) {
        if (dso == "" || !is_dso_token(dso)) {
            return
        }
        b = dso_bucket(dso)
        bucket_self[b] += selfpct
        total_self += selfpct
        if (is_server_dso(dso) && allpct + 0 >= limit + 0) {
            srv_all[sym] = allpct
            srv_self[sym] = selfpct
        }
        if (is_kernel_dso(dso)) {
            if (selfpct + 0 > 0) {
                ksym_self[sym] += selfpct
            }
            cat = kernel_category(sym)
            kcat_self[cat] += selfpct
        }
        if (is_libc_dso(dso) && selfpct + 0 > 0) {
            libsym_self[sym] += selfpct
        }
    }
    BEGIN {
        hdr = 0
        current_dso = ""
        bucket_list = "server kernel libc libpthread libstdc++ vdso ldso other"
    }
    /^# (Overhead|Children|All)/ {
        if ($0 ~ /Shared Object/) {
            hdr = 1
        } else if ($0 ~ /Self/) {
            hdr = 2
        } else if ($0 ~ /^# Overhead/) {
            hdr = 3
        }
        next
    }
    /^# \./ { next }
    /^# \(Cannot load tips/ { exit }
    hdr > 0 && /^[[:space:]]+[0-9]/ {
        if (hdr == 1) {
            ingest_row(pct_val($1), pct_val($2), $4, sym_from_fields(5))
        } else if (hdr == 2) {
            ingest_row(pct_val($1), pct_val($2), current_dso, sym_from_fields(3))
        } else if (hdr == 3) {
            ingest_row(pct_val($1), pct_val($1), current_dso, sym_from_fields(2))
        }
        next
    }
    /^[[:space:]]+[0-9.]+%[[:space:]]+/ && NF == 2 && is_dso_token($2) {
        current_dso = $2
    }
    END {
        if (total_self <= 0) {
            print "# (无符号数据；检查 perf.data 与 perf report 表头)"
            exit
        }
        print "# === §0 CPU 预算（互斥）==="
        print "# 含义: 采样时 PC 落在哪个共享库/模块（Self），每个样本只计一次。"
        print "# 分母: 全部 CPU 样本（本表各行 Self 相加 ≈ 100%）。"
        print "# 读法: kernel 高 → 看 §2/§3；server 高 → 看 §1。"
        print "# Self%   层级"
        split(bucket_list, bl, " ")
        nb = 0
        for (i = 1; i <= 8; i++) {
            b = bl[i]
            if (bucket_self[b] + 0 > 0) {
                order_b[++nb] = b
                pct_b[b] = bucket_self[b]
            }
        }
        sort_order(pct_b, order_b, nb)
        for (i = 1; i <= nb; i++) {
            b = order_b[i]
            printf " %7.2f%%  %s\n", pct_b[b], b
        }
        print ""
        print "# === §1 server（All / Self）==="
        print "# 含义: server 二进制符号；All=inclusive（含 libc/内核 callee），Self=仅函数体内。"
        print "# 分母: 全部 CPU 样本。定 src 优化优先级看 All 降序；Self 高表示热点在自身。"
        print "# 注意: 父子行 All 重叠，各行勿相加；All 与 Self 勿加在同一行。"
        print "# All      Self  Symbol"
        ns = 0
        for (s in srv_all) {
            order_s[++ns] = s
            pct_sa[s] = srv_all[s]
        }
        sort_order(pct_sa, order_s, ns)
        if (ns == 0) {
            print "# (无 ≥ " limit "% 的 server 符号)"
        } else {
            for (i = 1; i <= ns; i++) {
                s = order_s[i]
                printf " %7.2f%%  %7.2f%%  %s\n", srv_all[s], srv_self[s], s
            }
        }
        print ""
        print "# === §2 kernel（Self）==="
        print "# 含义: PC 落在 [kernel.kallsyms] 的样本占比（已去掉 [k] 前缀显示）。"
        print "# 分母: 全部 CPU 样本。定内核热点看 Self 降序。"
        print "# Self%   Symbol"
        nk = 0
        for (s in ksym_self) {
            if (ksym_self[s] + 0 >= limit + 0) {
                order_k[++nk] = s
                pct_k[s] = ksym_self[s]
            }
        }
        sort_order(pct_k, order_k, nk)
        if (nk == 0) {
            print "# (无 ≥ " limit "% 的内核符号)"
        } else {
            for (i = 1; i <= nk; i++) {
                s = order_k[i]
                printf " %7.2f%%  %s\n", pct_k[s], sym_clean(s)
            }
        }
        print ""
        print "# === §3 kernel 分类（Self，互斥）==="
        print "# 含义: 内核样本按符号归入 syscall/network/futex/sched/other。"
        print "# 分母: 全部 CPU 样本；各行 Self 相加 ≈ §0 的 kernel 行。"
        print "# Self%   类别        代表符号（该类别 Self top3）"
        split("syscall network futex sched other", cl, " ")
        nc = 0
        for (i = 1; i <= 5; i++) {
            c = cl[i]
            if (kcat_self[c] + 0 > 0) {
                order_c[++nc] = c
                pct_c[c] = kcat_self[c]
            }
        }
        sort_order(pct_c, order_c, nc)
        if (nc == 0) {
            print "# (无内核样本)"
        } else {
            for (i = 1; i <= nc; i++) {
                c = order_c[i]
                printf " %7.2f%%  %-10s  %s\n", pct_c[c], c, top_syms_for_cat(c)
            }
        }
        print ""
        print "# === §4 libc（Self）==="
        print "# 含义: PC 落在 libc 的样本（read/write/epoll 等，原始符号名）。"
        print "# 分母: 全部 CPU 样本。与 §1 All 不同：此处不含 server inclusive。"
        print "# Self%   Symbol"
        nl = 0
        for (s in libsym_self) {
            if (libsym_self[s] + 0 >= limit + 0) {
                order_l[++nl] = s
                pct_l[s] = libsym_self[s]
            }
        }
        sort_order(pct_l, order_l, nl)
        if (nl == 0) {
            print "# (无 ≥ " limit "% 的 libc 符号)"
        } else {
            for (i = 1; i <= nl; i++) {
                s = order_l[i]
                printf " %7.2f%%  %s\n", pct_l[s], s
            }
        }
    }
    ' "$flat" >>"$report"
}

generate_report() {
    local data="$1"
    local base report limit lines flat
    base="$(artifact_base)"
    report="$ARTIFACTS_DIR/${base}_perf_report.txt"
    limit="$PERF_REPORT_PERCENT_LIMIT"
    flat="$(mktemp "${report}.flat.XXXXXX")"

    log "perf 符号表 → $report（§0～§4，阈值 ≥ ${limit}%）"
    # 必须与 §1 相同排序，保证每行带 Shared Object（DSO）列；--sort dso,symbol 会缺 DSO 列导致 §0 全落 other
    if ! run_perf_report_flat "$data" "$flat" --stdio --sort comm,dso,symbol \
        --percent-limit 0 -g none; then
        rm -f "$flat"
        die "perf report (符号表) 失败"
    fi

    {
        printf '# %s perf 符号表\n' "$base"
        printf '# 源文件: %s\n' "$(basename "$data")"
        printf '# 火焰图: %s_flamegraph.svg\n' "$base"
        printf '#\n'
        printf '# 阅读顺序: §0 预算 → §1 server → §2/§3 内核 → §4 libc → SVG 看路径\n'
        printf '# 全局分母: 全部 CPU 样本（perf record 采样总数）。\n'
        printf '# 行过滤: §0/§3 不过滤；§1/§2/§4 仅输出 ≥ %s%% 的符号行。\n' "$limit"
        printf '#\n'
    } >"$report"
    render_unified_perf_report "$flat" "$limit" "$report"
    {
        printf '\n# 调用链详见: %s_flamegraph.svg\n' "$base"
    } >>"$report"
    rm -f "$flat"
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
    log "  report:     ${base}_perf_report.txt（§0～§4，≥${PERF_REPORT_PERCENT_LIMIT}%）"
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
