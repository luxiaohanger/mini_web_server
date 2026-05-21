#!/usr/bin/env bash
# 在项目根目录启动 server + HTTP 测试 client（tmux 双窗格）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
SERVER="$BUILD_DIR/src/server/server"
CLIENT="$BUILD_DIR/src/client/client"
SESSION_NAME="${SESSION_NAME:-mini_web_server}"

MODE="n"   # n: HTTP 四用例多轮；t: idle 超时验收
REPEAT=1
HOST="127.0.0.1"
PORT=8888
DO_BUILD=0

usage() {
    cat <<'EOF'
用法: bash scripts/up.sh [选项] [-n repeat | -t] [host] [port]

  在 tmux 中同时启动 server 与 HTTP 测试 client。

选项:
  -n <repeat>    HTTP 四用例重复轮数（默认 1）
  -t             idle 超时验收（同连接等待约 65s，勿与 -n 同用）
  -b, --build    启动前先执行 cmake --build build
  -h, --help     显示本帮助

示例:
  bash scripts/up.sh                 # HTTP 1 轮
  bash scripts/up.sh -n 5            # HTTP 5 轮
  bash scripts/up.sh -t              # idle 验收
  bash scripts/up.sh --build -n 3
  bash scripts/up.sh 2 127.0.0.1 8888   # 兼容：等同 -n 2

无 tmux 时仅打印手动启动命令，不自动起进程。
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b | --build)
            DO_BUILD=1
            shift
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        -t)
            if [[ "$MODE" == "t" ]]; then
                echo "重复指定 -t" >&2
                exit 1
            fi
            MODE="t"
            shift
            ;;
        -n)
            if [[ "$MODE" == "t" ]]; then
                echo "-t 与 -n 不能同时使用" >&2
                exit 1
            fi
            if [[ $# -lt 2 ]]; then
                echo "-n 需要 repeat 参数" >&2
                exit 1
            fi
            if ! [[ "$2" =~ ^[0-9]+$ ]] || [[ "$2" -lt 1 ]]; then
                echo "repeat 须为正整数: $2" >&2
                exit 1
            fi
            REPEAT="$2"
            shift 2
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo "未知选项: $1" >&2
            usage >&2
            exit 1
            ;;
        *)
            break
            ;;
    esac
done

# 兼容旧用法：第一个位置参数为正整数且无 -n/-t 时视为 -n repeat
if [[ $# -ge 1 && "$MODE" != "t" ]]; then
    if [[ "$1" =~ ^[0-9]+$ && "$1" -ge 1 ]]; then
        REPEAT="$1"
        shift
    fi
fi

if [[ $# -ge 1 ]]; then HOST="$1"; fi
if [[ $# -ge 2 ]]; then PORT="$2"; fi
if [[ $# -gt 2 ]]; then
    echo "参数过多" >&2
    usage >&2
    exit 1
fi

if [[ "$DO_BUILD" -eq 1 ]]; then
    echo "[up.sh] cmake --build $BUILD_DIR"
    cmake --build "$BUILD_DIR"
fi

if [[ ! -x "$SERVER" ]]; then
    echo "未找到 server: $SERVER" >&2
    echo "请先: cmake -S . -B build && cmake --build build" >&2
    exit 1
fi
if [[ ! -x "$CLIENT" ]]; then
    echo "未找到 client: $CLIENT" >&2
    echo "请先: cmake -S . -B build && cmake --build build" >&2
    exit 1
fi

server_cmd="cd $(printf '%q' "$ROOT") && $(printf '%q' "$SERVER"); echo; echo '[server 已退出] 按回车关闭...'; read"

if [[ "$MODE" == "t" ]]; then
    client_args=(-t "$HOST" "$PORT")
    mode_desc="idle-timeout"
else
    client_args=(-n "$REPEAT" "$HOST" "$PORT")
    mode_desc="http repeat=$REPEAT"
fi

client_cmd="cd $(printf '%q' "$ROOT") && sleep 0.5 && $(printf '%q' "$CLIENT")"
for a in "${client_args[@]}"; do
    client_cmd+=" $(printf '%q' "$a")"
done
client_cmd+='; echo; echo '\''[client 已结束] 按回车关闭...'\''; read'

if ! command -v tmux >/dev/null 2>&1; then
    echo "未检测到 tmux。请开两个终端手动执行："
    echo
    echo "  终端 1: $SERVER"
    echo -n "  终端 2: $CLIENT"
    for a in "${client_args[@]}"; do echo -n " $a"; done
    echo
    exit 0
fi

tmux kill-session -t "$SESSION_NAME" 2>/dev/null || true
tmux new-session -d -s "$SESSION_NAME" -c "$ROOT" bash -c "$server_cmd"
tmux split-window -h -t "$SESSION_NAME:" -c "$ROOT" bash -c "$client_cmd"
tmux select-layout -t "$SESSION_NAME:" even-horizontal >/dev/null 2>&1 || true

echo "[up.sh] tmux session: $SESSION_NAME  ($mode_desc  host=$HOST  port=$PORT)"
if [[ "$MODE" == "t" ]]; then
    echo "[up.sh] idle 模式约需 65s，请耐心等待 client 窗格"
fi
echo "[up.sh] 分离会话: Ctrl+b d    结束: 各窗格内 Ctrl+C 或关闭 server"
tmux attach -t "$SESSION_NAME"
