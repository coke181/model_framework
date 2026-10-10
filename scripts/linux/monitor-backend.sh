#!/usr/bin/env bash

# 文件职责：每分钟检查生产后端的基础设施和关键业务指标。
# 调用流程：systemd timer -> 本脚本 -> CPU/磁盘/systemd/health/连接池 -> journald/可选 Webhook。
# 告警只在状态变化时发送，避免每分钟重复刷屏；恢复时会发送一条恢复消息。

set -uo pipefail

ENV_FILE="${ENV_FILE:-/etc/hello-service/hello-service.env}"
STATE_DIR="${STATE_DIR:-/var/lib/ue-game-monitor}"
HEALTH_URL="${HEALTH_URL:-http://127.0.0.1:8080/health}"
METRICS_URL="${METRICS_URL:-http://127.0.0.1:8080/metrics}"
CHAT_METRICS_URL="${CHAT_METRICS_URL:-http://127.0.0.1:9091/metrics}"
DISK_WARN_PERCENT="${DISK_WARN_PERCENT:-85}"
DISK_CRIT_PERCENT="${DISK_CRIT_PERCENT:-95}"
CPU_WARN_PERCENT="${CPU_WARN_PERCENT:-90}"
POOL_WARN_PERCENT="${POOL_WARN_PERCENT:-90}"

mkdir -p "${STATE_DIR}"

if [[ -f "${ENV_FILE}" ]]; then
    # shellcheck disable=SC1090
    source "${ENV_FILE}"
fi

log_alert() {
    local message="$1"
    logger -t ue-game-monitor -- "${message}"
    if [[ -n "${HELLO_MONITOR_WEBHOOK_URL:-}" ]]; then
        curl --silent --show-error --max-time 5 -X POST \
            -H 'Content-Type: application/json' \
            --data "$(printf '%s' "${message}" | python3 -c 'import json,sys; print(json.dumps({"text":sys.stdin.read()}))')" \
            "${HELLO_MONITOR_WEBHOOK_URL}" >/dev/null 2>&1 || true
    fi
}

check_state() {
    local key="$1"
    local active="$2"
    local message="$3"
    local marker="${STATE_DIR}/${key}"
    if [[ "${active}" == "1" && ! -e "${marker}" ]]; then
        touch "${marker}"
        log_alert "告警: ${message}"
    elif [[ "${active}" == "0" && -e "${marker}" ]]; then
        rm -f "${marker}"
        log_alert "恢复: ${message}"
    fi
}

check_service() {
    local unit="$1"
    if systemctl is-active --quiet "${unit}"; then
        check_state "service_${unit}" 0 "服务 ${unit} 异常"
    else
        check_state "service_${unit}" 1 "服务 ${unit} 未处于 active 状态"
    fi
}

check_service hello-service
check_service hello-chat-gateway
check_service hello-task-worker
check_service nginx

if curl --silent --show-error --fail --max-time 5 "${HEALTH_URL}" >/dev/null 2>&1; then
    check_state health 0 "HTTP 健康检查失败"
else
    check_state health 1 "HTTP 健康检查失败: ${HEALTH_URL}"
fi

disk_percent="$(df -P / | awk 'NR==2 {gsub(/%/, "", $5); print $5}')"
if [[ "${disk_percent:-0}" -ge "${DISK_CRIT_PERCENT}" ]]; then
    check_state disk_critical 1 "根分区使用率 ${disk_percent}% >= ${DISK_CRIT_PERCENT}%"
    check_state disk_warning 0 "根分区使用率超过 ${DISK_WARN_PERCENT}%"
elif [[ "${disk_percent:-0}" -ge "${DISK_WARN_PERCENT}" ]]; then
    check_state disk_critical 0 "根分区使用率 ${disk_percent}% >= ${DISK_CRIT_PERCENT}%"
    check_state disk_warning 1 "根分区使用率 ${disk_percent}% >= ${DISK_WARN_PERCENT}%"
else
    check_state disk_critical 0 "根分区使用率超过临界值"
    check_state disk_warning 0 "根分区使用率超过 ${DISK_WARN_PERCENT}%"
fi

read -r cpu_total_1 cpu_idle_1 < <(awk '/^cpu / {print $2+$3+$4+$5+$6+$7+$8, $5; exit}' /proc/stat)
sleep 1
read -r cpu_total_2 cpu_idle_2 < <(awk '/^cpu / {print $2+$3+$4+$5+$6+$7+$8, $5; exit}' /proc/stat)
cpu_busy=$(( (cpu_total_2 - cpu_total_1) - (cpu_idle_2 - cpu_idle_1) ))
cpu_total=$(( cpu_total_2 - cpu_total_1 ))
cpu_percent=0
if [[ "${cpu_total}" -gt 0 ]]; then
    cpu_percent=$(( cpu_busy * 100 / cpu_total ))
fi
if [[ "${cpu_percent}" -ge "${CPU_WARN_PERCENT}" ]]; then
    check_state cpu 1 "CPU 使用率 ${cpu_percent}% >= ${CPU_WARN_PERCENT}%"
else
    check_state cpu 0 "CPU 使用率超过 ${CPU_WARN_PERCENT}%"
fi

metrics_file="$(mktemp)"
chat_metrics_file="$(mktemp)"
trap 'rm -f "${metrics_file}" "${chat_metrics_file}"' EXIT
curl --silent --max-time 5 "${METRICS_URL}" >"${metrics_file}" || true
curl --silent --max-time 5 "${CHAT_METRICS_URL}" >"${chat_metrics_file}" || true

metric_value() {
    local file="$1"
    local name="$2"
    awk -v prefix="${name}" '$0 ~ "^" prefix "([ {]|$)" && $0 !~ /^#/ {print $NF; exit}' "${file}"
}

pool_check() {
    local prefix="$1"
    local label="$2"
    local size in_use ratio
    size="$(metric_value "${metrics_file}" "${prefix}_pool_size" | head -n 1)"
    in_use="$(metric_value "${metrics_file}" "${prefix}_pool_in_use" | head -n 1)"
    if [[ "${size:-0}" =~ ^[0-9]+([.][0-9]+)?$ && "${in_use:-0}" =~ ^[0-9]+([.][0-9]+)?$ && "${size%.*}" -gt 0 ]]; then
        ratio=$(( ${in_use%.*} * 100 / ${size%.*} ))
        if [[ "${ratio}" -ge "${POOL_WARN_PERCENT}" ]]; then
            check_state "${prefix}_pool" 1 "${label}连接池使用率约 ${ratio}%"
        else
            check_state "${prefix}_pool" 0 "${label}连接池使用率过高"
        fi
    fi
}

pool_check mf_mysql mysql

mysql_errors="$(grep '^mf_mysql_pool_connection_events_total' "${metrics_file}" | grep 'status="error"' | awk '{sum += $NF} END {print sum+0}')"
redis_errors="$(cat "${metrics_file}" "${chat_metrics_file}" | grep '^mf_redis_operations_total' | grep 'status="error"' | awk '{sum += $NF} END {print sum+0}')"
if [[ "${mysql_errors}" -gt 0 ]]; then
    check_state mysql_errors 1 "MySQL 连接池累计出现连接错误"
else
    check_state mysql_errors 0 "MySQL 连接池连接错误"
fi
if [[ "${redis_errors}" -gt 0 ]]; then
    check_state redis_errors 1 "Redis 操作累计出现错误"
else
    check_state redis_errors 0 "Redis 操作错误"
fi

outbox_failed="$(metric_value "${metrics_file}" mf_chat_persistence_failed_after_retries_total)"
if [[ "${outbox_failed:-0}" =~ ^[0-9]+([.][0-9]+)?$ && "${outbox_failed%.*}" -gt 0 ]]; then
    check_state outbox_failed 1 "聊天 Outbox 存在重试耗尽消息"
else
    check_state outbox_failed 0 "聊天 Outbox 存在重试耗尽消息"
fi
