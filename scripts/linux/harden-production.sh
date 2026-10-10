#!/usr/bin/env bash

# 文件职责：把已经部署的实例切换为安全的生产监听方式。
# 调用流程：备份 deploy.env -> 更新生产配置 -> 重启三个服务 -> 检查监听端口和健康接口。
# 说明：脚本不修改数据库数据，也不会生成或输出密码、token 等敏感值。

set -euo pipefail

PROJECT_ROOT="${PROJECT_ROOT:-/opt/hello-service}"
ENV_FILE="${ENV_FILE:-/etc/hello-service/hello-service.env}"
BACKUP_DIR="${PROJECT_ROOT}/backups/config"
STAMP="$(date +%Y%m%d-%H%M%S)"
CHAT_DROPIN_DIR="/etc/systemd/system/hello-chat-gateway.service.d"
CHAT_DROPIN="${CHAT_DROPIN_DIR}/30-production-network.conf"

if [[ ! -f "${ENV_FILE}" ]]; then
  echo "找不到环境文件：${ENV_FILE}" >&2
  exit 1
fi

mkdir -p "${BACKUP_DIR}"
cp -a "${ENV_FILE}" "${BACKUP_DIR}/deploy.env.${STAMP}.bak"

set_env() {
  local key="$1"
  local value="$2"
  if grep -q "^${key}=" "${ENV_FILE}"; then
    sed -i "s#^${key}=.*#${key}=${value}#" "${ENV_FILE}"
  else
    printf '\n%s=%s\n' "${key}" "${value}" >> "${ENV_FILE}"
  fi
}

# Nginx 已经代理到本机端口，因此业务端口不需要对公网监听。
set_env HELLO_SERVICE_ENV production
set_env HELLO_SERVICE_HOST 127.0.0.1
set_env HELLO_CHAT_GATEWAY_HOST 127.0.0.1
set_env HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN 0
set_env HELLO_SERVICE_PERF_MODE 0
set_env HELLO_SERVICE_INSECURE_FAST_PASSWORD_HASH 0

mkdir -p "${CHAT_DROPIN_DIR}"
cat > "${CHAT_DROPIN}" <<'EOF'
[Service]
# Nginx 的 /ws 负责公网入口，聊天网关只监听本机。
Environment=HELLO_CHAT_GATEWAY_HOST=127.0.0.1
EOF

systemctl daemon-reload
systemctl restart hello-service hello-chat-gateway hello-task-worker
sleep 2

systemctl is-active --quiet hello-service
systemctl is-active --quiet hello-chat-gateway
systemctl is-active --quiet hello-task-worker
curl --fail --silent --show-error --max-time 5 http://127.0.0.1:8080/health >/dev/null

echo "生产配置已生效。"
echo "配置备份：${BACKUP_DIR}/deploy.env.${STAMP}.bak"
echo "监听端口："
ss -lntp | grep -E ':(80|8080|8090) ' || true
