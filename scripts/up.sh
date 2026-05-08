#!/bin/bash

D=$(cd "$(dirname "$0")" && pwd)
SESSION_NAME="server_test"

# 程序结束后显示提示信息，按回车后窗口才会关闭
tmux new-session -d -s "$SESSION_NAME" "$D/../build/src/server/server; echo '按回车退出...'; read"
tmux split-window -t "$SESSION_NAME" "$D/../build/src/client/client; echo '按回车退出...'; read"
tmux attach-session -t "$SESSION_NAME"