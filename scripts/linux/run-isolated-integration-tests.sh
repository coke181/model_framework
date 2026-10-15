#!/usr/bin/env bash

# Run HTTP, MySQL/Redis consistency, and WebSocket tests against disposable
# resources on a Linux host. Production services and database names are never
# reused; cleanup runs for success, failure, and interruption.

set -Eeuo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"
ENV_FILE="${ENV_FILE:-/etc/hello-service/hello-service.env}"
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build}"
HTTP_BIN="${HTTP_BIN:-${BUILD_DIR}/hello_service}"
CHAT_BIN="${CHAT_BIN:-${BUILD_DIR}/hello_chat_gateway}"
PYTHON_BIN="${PYTHON_BIN:-python3}"
HTTP_PORT="${INTEGRATION_HTTP_PORT:-18080}"
CHAT_PORT="${INTEGRATION_CHAT_PORT:-18090}"
CHAT_METRICS_PORT="${INTEGRATION_CHAT_METRICS_PORT:-19091}"
REDIS_DB="${INTEGRATION_REDIS_DB:-14}"
ARTIFACT_DIR="${INTEGRATION_ARTIFACT_DIR:-${PROJECT_ROOT}/tests/.artifacts}"

if [[ ! -f "${ENV_FILE}" ]]; then
  echo "integration environment file not found: ${ENV_FILE}" >&2
  exit 1
fi
if [[ ! -x "${HTTP_BIN}" || ! -x "${CHAT_BIN}" ]]; then
  echo "build hello_service and hello_chat_gateway before running integration tests" >&2
  exit 1
fi
if [[ ! -f "${PROJECT_ROOT}/tests/requirements-integration.txt" ]]; then
  echo "integration tests are not present under ${PROJECT_ROOT}/tests" >&2
  exit 1
fi
if [[ ! "${REDIS_DB}" =~ ^[1-9][0-9]*$ ]]; then
  echo "INTEGRATION_REDIS_DB must be a non-zero numeric Redis database" >&2
  exit 1
fi
for port in "${HTTP_PORT}" "${CHAT_PORT}" "${CHAT_METRICS_PORT}"; do
  if [[ ! "${port}" =~ ^[1-9][0-9]*$ ]] || ((port > 65535)); then
    echo "invalid integration port: ${port}" >&2
    exit 1
  fi
  if ss -ltnH | awk '{print $4}' | grep -qE "(^|:)${port}$"; then
    echo "integration port is already in use: ${port}" >&2
    exit 1
  fi
done

set -a
# shellcheck disable=SC1090
source "${ENV_FILE}"
set +a

MYSQL_HOST="${HELLO_SERVICE_MYSQL_HOST:-127.0.0.1}"
MYSQL_PORT="${HELLO_SERVICE_MYSQL_PORT:-3306}"
PRODUCTION_DATABASE="${HELLO_SERVICE_MYSQL_DATABASE:-game_backend}"
REDIS_HOST="${HELLO_SERVICE_REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${HELLO_SERVICE_REDIS_PORT:-6379}"
SOURCE_REDIS_PASSWORD="${HELLO_SERVICE_REDIS_PASSWORD:-}"
if [[ "${REDIS_DB}" == "${HELLO_SERVICE_REDIS_DB:-0}" ]]; then
  echo "test Redis DB must differ from the service environment Redis DB" >&2
  exit 1
fi

if [[ "${MYSQL_HOST}" != "127.0.0.1" && "${MYSQL_HOST}" != "localhost" ]] ||
   [[ "${REDIS_HOST}" != "127.0.0.1" && "${REDIS_HOST}" != "localhost" ]]; then
  echo "isolated runner requires local MySQL and Redis endpoints" >&2
  exit 1
fi

suffix="$(date +%Y%m%d%H%M%S)_$(openssl rand -hex 3)"
TEST_DATABASE="game_backend_it_${suffix}"
TEST_USER="it_$(openssl rand -hex 8)"
TEST_PASSWORD="It-A1!$(openssl rand -hex 20)"
ADMIN_LOGIN="it_admin_${suffix}"
ADMIN_PASSWORD="It-A1!$(openssl rand -hex 20)"
HTTP_PID=""
CHAT_PID=""
DATABASE_CREATED=0
USER_CREATED=0
REDIS_CLAIMED=0
REDIS_SELECTED_PASSWORD=""

# The lock persists as an empty file; only the flock lifetime claims this DB.
exec 9>"/tmp/hello-integration-redis-${REDIS_PORT}-${REDIS_DB}.lock"
if ! flock -n 9; then
  echo "Redis test DB ${REDIS_DB} is already claimed by another integration run" >&2
  exit 1
fi
RUNTIME_DIR="$(mktemp -d /tmp/hello-integration.XXXXXX)"

mysql_admin() {
  mysql --protocol=socket "$@"
}

redis_test() {
  if [[ -n "${REDIS_SELECTED_PASSWORD}" ]]; then
    REDISCLI_AUTH="${REDIS_SELECTED_PASSWORD}" redis-cli \
      -h "${REDIS_HOST}" -p "${REDIS_PORT}" -n "${REDIS_DB}" "$@"
  else
    redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" -n "${REDIS_DB}" "$@"
  fi
}

cleanup() {
  local status=$?
  trap - EXIT INT TERM
  set +e
  for pid in "${CHAT_PID}" "${HTTP_PID}"; do
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" 2>/dev/null
      for _ in $(seq 1 20); do
        kill -0 "${pid}" 2>/dev/null || break
        sleep 0.1
      done
      kill -KILL "${pid}" 2>/dev/null
      wait "${pid}" 2>/dev/null
    fi
  done
  if [[ "${REDIS_CLAIMED}" == "1" && "${REDIS_DB}" != "0" ]]; then
    redis_test FLUSHDB >/dev/null 2>&1 || echo "WARNING: test Redis cleanup failed" >&2
  fi
  if [[ "${DATABASE_CREATED}" == "1" && "${TEST_DATABASE}" == game_backend_it_* ]]; then
    mysql_admin -e \
      "DROP DATABASE IF EXISTS \`${TEST_DATABASE}\`;" >/dev/null 2>&1 || echo "WARNING: test database cleanup failed" >&2
  fi
  if [[ "${USER_CREATED}" == "1" && "${TEST_USER}" == it_* ]]; then
    mysql_admin -e "DROP USER IF EXISTS '${TEST_USER}'@'127.0.0.1';" \
      >/dev/null 2>&1 || echo "WARNING: test MySQL user cleanup failed" >&2
  fi
  case "${RUNTIME_DIR}" in
    /tmp/hello-integration.*) rm -rf -- "${RUNTIME_DIR}" ;;
    *) echo "refusing to remove unexpected runtime directory: ${RUNTIME_DIR}" >&2 ;;
  esac
  echo "isolated integration resources cleaned"
  exit "${status}"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ "$(redis-cli -h "${REDIS_HOST}" -p "${REDIS_PORT}" -n "${REDIS_DB}" PING 2>/dev/null)" == "PONG" ]]; then
  REDIS_SELECTED_PASSWORD=""
elif [[ -n "${SOURCE_REDIS_PASSWORD}" ]] && \
     [[ "$(REDISCLI_AUTH="${SOURCE_REDIS_PASSWORD}" redis-cli \
       -h "${REDIS_HOST}" -p "${REDIS_PORT}" -n "${REDIS_DB}" PING 2>/dev/null)" == "PONG" ]]; then
  REDIS_SELECTED_PASSWORD="${SOURCE_REDIS_PASSWORD}"
else
  echo "cannot authenticate to the configured Redis instance" >&2
  exit 1
fi

redis_key_count="$(redis_test DBSIZE)"
if [[ "${redis_key_count}" != "0" ]]; then
  echo "refusing to clear non-empty Redis DB ${REDIS_DB} (${redis_key_count} keys)" >&2
  exit 1
fi
REDIS_CLAIMED=1
if [[ "${TEST_DATABASE}" == "${PRODUCTION_DATABASE}" ]]; then
  echo "generated test database unexpectedly matches the production database" >&2
  exit 1
fi

mysql_admin -e \
  "CREATE DATABASE \`${TEST_DATABASE}\` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
DATABASE_CREATED=1
mysql_admin -e \
  "CREATE USER '${TEST_USER}'@'127.0.0.1' IDENTIFIED BY '${TEST_PASSWORD}';"
USER_CREATED=1
mysql_admin -e \
  "GRANT ALL PRIVILEGES ON \`${TEST_DATABASE}\`.* TO '${TEST_USER}'@'127.0.0.1';"

export HELLO_SERVICE_NAME=ue-game-backend-integration
export HELLO_SERVICE_ENV=development
export HELLO_SERVICE_HOST=127.0.0.1
export HELLO_SERVICE_PORT="${HTTP_PORT}"
export HELLO_SERVICE_STORAGE_MODE=mysql_redis
export HELLO_SERVICE_MYSQL_HOST="${MYSQL_HOST}"
export HELLO_SERVICE_MYSQL_PORT="${MYSQL_PORT}"
export HELLO_SERVICE_MYSQL_USER="${TEST_USER}"
export HELLO_SERVICE_MYSQL_PASSWORD="${TEST_PASSWORD}"
export HELLO_SERVICE_MYSQL_DATABASE="${TEST_DATABASE}"
export HELLO_SERVICE_MYSQL_SCHEMA_FILE="${PROJECT_ROOT}/db/mysql/001_game_backend.sql"
export HELLO_SERVICE_MYSQL_AUTO_MIGRATE=1
export HELLO_SERVICE_REDIS_HOST="${REDIS_HOST}"
export HELLO_SERVICE_REDIS_PORT="${REDIS_PORT}"
export HELLO_SERVICE_REDIS_DB="${REDIS_DB}"
export HELLO_SERVICE_REDIS_PASSWORD="${REDIS_SELECTED_PASSWORD}"
export HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN=1
export HELLO_SERVICE_DEV_ADMIN_LOGIN="${ADMIN_LOGIN}"
export HELLO_SERVICE_DEV_ADMIN_PASSWORD="${ADMIN_PASSWORD}"
export HELLO_SERVICE_DEV_ADMIN_DISPLAY_NAME=IntegrationAdmin
export HELLO_SERVICE_INSECURE_FAST_PASSWORD_HASH=0
export HELLO_SERVICE_PERF_MODE=0
# Functional fixtures share loopback; registration rate limiting is a separate concern.
export HELLO_SERVICE_REGISTER_MAX_PER_IP=1000
export HELLO_CHAT_GATEWAY_HOST=127.0.0.1
export HELLO_CHAT_GATEWAY_PORT="${CHAT_PORT}"
export HELLO_CHAT_GATEWAY_METRICS_HOST=127.0.0.1
export HELLO_CHAT_GATEWAY_METRICS_PORT="${CHAT_METRICS_PORT}"

cd "${PROJECT_ROOT}"
"${HTTP_BIN}" >"${RUNTIME_DIR}/hello-service.log" 2>&1 &
HTTP_PID=$!

http_ready=0
for _ in $(seq 1 80); do
  if curl -fsS "http://127.0.0.1:${HTTP_PORT}/health" >/dev/null 2>&1; then
    http_ready=1
    break
  fi
  kill -0 "${HTTP_PID}" 2>/dev/null || break
  sleep 0.25
done
if [[ "${http_ready}" != "1" ]]; then
  echo "temporary HTTP service failed to start; check binary dependencies and test configuration" >&2
  exit 1
fi

"${CHAT_BIN}" >"${RUNTIME_DIR}/hello-chat-gateway.log" 2>&1 &
CHAT_PID=$!

chat_ready=0
for _ in $(seq 1 80); do
  if curl -fsS "http://127.0.0.1:${CHAT_METRICS_PORT}/health" >/dev/null 2>&1; then
    chat_ready=1
    break
  fi
  kill -0 "${CHAT_PID}" 2>/dev/null || break
  sleep 0.25
done
if [[ "${chat_ready}" != "1" ]]; then
  echo "temporary chat gateway failed to start; check binary dependencies and test configuration" >&2
  exit 1
fi

"${PYTHON_BIN}" -m pip install --disable-pip-version-check -q \
  --target "${RUNTIME_DIR}/pydeps" \
  -r "${PROJECT_ROOT}/tests/requirements-integration.txt"

export PYTHONPATH="${RUNTIME_DIR}/pydeps:${PROJECT_ROOT}/tests"
export HELLO_SERVICE_INTEGRATION_BASE_URL="http://127.0.0.1:${HTTP_PORT}"
export HELLO_SERVICE_INTEGRATION_WS_URL="ws://127.0.0.1:${CHAT_PORT}"
export HELLO_SERVICE_INTEGRATION_ADMIN_LOGIN="${ADMIN_LOGIN}"
export HELLO_SERVICE_INTEGRATION_ADMIN_PASSWORD="${ADMIN_PASSWORD}"

mkdir -p "${ARTIFACT_DIR}"
REPORT_PATH="${ARTIFACT_DIR}/integration-${suffix}.xml"
set +e
"${PYTHON_BIN}" -m pytest tests/integration -m integration -v --tb=short \
  --junitxml="${REPORT_PATH}" \
  --html="${REPORT_PATH%.xml}.html" --self-contained-html
test_status=$?
set -e

echo "integration report: ${REPORT_PATH}"
exit "${test_status}"
