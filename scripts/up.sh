#!/usr/bin/env bash
# 在项目根目录启动 server + HTTP 测试 client（tmux 双窗格）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
SERVER="$BUILD_DIR/src/server/server"
CLIENT="$BUILD_DIR/src/client/client"
SESSION_NAME="${SESSION_NAME:-mini_web_server}"

REPEAT=1
HOST="127.0.0.1"
PORT=8888
DO_BUILD=0

usage() {
    cat <<'EOF'
用法: bash scripts/up.sh [选项] [repeat] [host] [port]

  在 tmux 中同时启动 server 与 HTTP 测试 client。
  client 参数与 src/client/main.cpp 一致：repeat 为全套用例重复轮数。

选项:
  -b, --build    启动前先执行 cmake --build build
  -h, --help     显示本帮助

示例:
  bash scripts/up.sh                 # 1 轮测试
  bash scripts/up.sh 5               # 5 轮
  bash scripts/up.sh --build 3       # 先编译再跑 3 轮
  bash scripts/up.sh 2 127.0.0.1 8888

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

if [[ $# -ge 1 ]]; then
    if ! [[ "$1" =~ ^[0-9]+$ ]] || [[ "$1" -lt 1 ]]; then
        echo "repeat 须为正整数: $1" >&2
        exit 1
    fi
    REPEAT="$1"
fi
if [[ $# -ge 2 ]]; then HOST="$2"; fi
if [[ $# -ge 3 ]]; then PORT="$3"; fi

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
client_cmd="cd $(printf '%q' "$ROOT") && sleep 0.5 && $(printf '%q' "$CLIENT") $(printf '%q' "$REPEAT") $(printf '%q' "$HOST") $(printf '%q' "$PORT"); echo; echo '[client 已结束] 按回车关闭...'; read"

if ! command -v tmux >/dev/null 2>&1; then
    echo "未检测到 tmux。请开两个终端手动执行："
    echo
    echo "  终端 1: $SERVER"
    echo "  终端 2: $CLIENT $REPEAT $HOST $PORT"
    exit 0
fi

tmux kill-session -t "$SESSION_NAME" 2>/dev/null || true
tmux new-session -d -s "$SESSION_NAME" -c "$ROOT" bash -c "$server_cmd"
tmux split-window -h -t "$SESSION_NAME:" -c "$ROOT" bash -c "$client_cmd"
tmux select-layout -t "$SESSION_NAME:" even-horizontal >/dev/null 2>&1 || true

echo "[up.sh] tmux session: $SESSION_NAME  (repeat=$REPEAT  host=$HOST  port=$PORT)"
echo "[up.sh] 分离会话: Ctrl+b d    结束: 各窗格内 Ctrl+C 或关闭 server"
tmux attach -t "$SESSION_NAME"
