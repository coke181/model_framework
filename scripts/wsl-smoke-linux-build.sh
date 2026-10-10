#!/usr/bin/env bash
# 调用流程：进入 Linux 构建目录 -> 检查动态库 -> 启动临时内存版服务
#          -> 请求 /health -> 关闭临时服务 -> 返回测试结果。

set -euo pipefail

build_dir="${1:-/opt/model/build-linux}"
port="${2:-18080}"
log_file="/tmp/hello-wsl-smoke.log"
health_file="/tmp/hello-wsl-health.json"

cd "$build_dir"

file hello_service hello_chat_gateway hello_task_worker

if ldd hello_service | grep -q "not found"; then
    echo "发现缺失的动态库："
    ldd hello_service | grep "not found"
    exit 1
fi

export HELLO_SERVICE_ENV=development
export HELLO_SERVICE_STORAGE_MODE=memory
export HELLO_SERVICE_HOST=127.0.0.1
export HELLO_SERVICE_PORT="$port"
export HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN=0
export MF_ENABLE_OTEL=0

./hello_service >"$log_file" 2>&1 &
service_pid=$!

cleanup() {
    kill "$service_pid" 2>/dev/null || true
    wait "$service_pid" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 20); do
    if curl -fsS "http://127.0.0.1:${port}/health" >"$health_file"; then
        cat "$health_file"
        echo
        echo "SMOKE_OK"
        exit 0
    fi
    sleep 1
done

echo "临时服务未能通过健康检查，最近日志如下："
tail -n 100 "$log_file" || true
exit 1
