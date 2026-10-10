#!/usr/bin/env bash

# 文件职责：在阿里云 Linux 服务器上测试已运行的 hello_service。
# 调用流程：检查服务和压测工具 -> 基线压测 -> 延迟/过载压测 -> 保存 metrics、load、CPU/内存和连接状态。
# 说明：脚本不写业务数据；真实业务接口只有在传入 PERF_AUTH_TOKEN 时才会测试。

set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
OUT_DIR="${OUT_DIR:-/tmp/ue-game-backend-perf-$(date +%Y%m%d_%H%M%S)}"
BOMBARDIER="${BOMBARDIER:-$(command -v bombardier || true)}"
PERF_AUTH_TOKEN="${PERF_AUTH_TOKEN:-}"

if [[ -z "${BOMBARDIER}" || ! -x "${BOMBARDIER}" ]]; then
  echo "未找到 bombardier。请先安装后设置 BOMBARDIER=/opt/bombardier/bombardier。" >&2
  exit 1
fi

mkdir -p "${OUT_DIR}"

if ! curl --fail --silent --show-error --max-time 5 "${BASE_URL}/health" >/dev/null; then
  echo "服务不可用：${BASE_URL}" >&2
  exit 1
fi

run_case() {
  local name="$1"
  local path="$2"
  local connections="$3"
  local duration="$4"

  echo "==== ${name} ====" | tee "${OUT_DIR}/${name}.log"
  echo "url=${BASE_URL}${path} connections=${connections} duration=${duration}s" | tee -a "${OUT_DIR}/${name}.log"
  "${BOMBARDIER}" -c "${connections}" -d "${duration}s" -m GET -l "${BASE_URL}${path}" \
    2>&1 | tee -a "${OUT_DIR}/${name}.log"
  curl --fail --silent "${BASE_URL}/metrics" > "${OUT_DIR}/${name}.metrics.txt"
  curl --fail --silent "${BASE_URL}/debug/load" > "${OUT_DIR}/${name}.load.json"
}

echo "output=${OUT_DIR}"
echo "service=$(curl --fail --silent "${BASE_URL}/health")"
echo "mysql_redis_listeners="
ss -ltnp 2>/dev/null | grep -E ':3306|:6379' || true

{
  echo "timestamp=$(date --iso-8601=seconds)"
  echo "hostname=$(hostname)"
  echo "kernel=$(uname -a)"
  echo "cpu=$(nproc)"
  free -h
} > "${OUT_DIR}/host.txt"

run_case "baseline_health" "/health" 64 20
run_case "delay_50ms" "/health/delay?sleep_ms=50" 128 20
run_case "overload_delay_100ms" "/health/delay?sleep_ms=100" 512 20

if [[ -n "${PERF_AUTH_TOKEN}" ]]; then
  curl --fail --silent -H "Authorization: Bearer ${PERF_AUTH_TOKEN}" "${BASE_URL}/players/me" \
    > "${OUT_DIR}/authenticated.check.json"
  "${BOMBARDIER}" -c 64 -d 20s -m GET \
    -H "Authorization: Bearer ${PERF_AUTH_TOKEN}" -l "${BASE_URL}/players/me" \
    > "${OUT_DIR}/authenticated_player.log" 2>&1 || true
  curl --fail --silent "${BASE_URL}/metrics" > "${OUT_DIR}/authenticated.metrics.txt"
fi

ps -eo pid,ppid,comm,%cpu,%mem,rss,etime,args | grep -E 'hello_service|hello_task_worker|hello_chat_gateway' | grep -v grep \
  > "${OUT_DIR}/processes.txt" || true

echo "测试完成，结果目录：${OUT_DIR}"
echo "重点查看："
echo "  ${OUT_DIR}/baseline_health.log"
echo "  ${OUT_DIR}/delay_50ms.log"
echo "  ${OUT_DIR}/overload_delay_100ms.log"
echo "  ${OUT_DIR}/overload_delay_100ms.load.json"
