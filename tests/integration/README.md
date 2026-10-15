# MySQL + Redis + WebSocket 集成测试

这组测试只连接显式指定的测试环境，不会自动连接默认开发库，也不会由 pytest 启动数据库。

## 环境要求

- 后端以 `HELLO_SERVICE_STORAGE_MODE=mysql_redis` 运行。
- 测试数据库已完成 schema migration。
- 提供可登录的测试管理员。
- Python 已安装 `tests/requirements-integration.txt`。
- WebSocket网关可从测试机访问，并设置 `HELLO_SERVICE_INTEGRATION_WS_URL`。
- MySQL 8.0.16及以上，测试账号需要临时库内的ALTER权限。购买故障用例会添加定向CHECK约束，验证失败回滚后移除，仅允许数据库名以 `game_backend_it_` 开头；请使用下方隔离脚本运行完整套件。

## 云服务器隔离运行（推荐）

将当前源码、测试与重新构建的 `hello_service`、`hello_chat_gateway` 准备在Linux主机上，使用MySQL本地socket管理员权限运行：

```bash
sudo PROJECT_ROOT=/path/to/model \
  BUILD_DIR=/path/to/model/build \
  ENV_FILE=/etc/hello-service/hello-service.env \
  bash /path/to/model/scripts/linux/run-isolated-integration-tests.sh
```

脚本不重启生产进程；只接受本地MySQL/Redis，创建随机命名的临时数据库和账号、使用空的非生产Redis DB（默认14）及回环端口18080/18090/19091，执行完自动清理；拒绝清空已有键的Redis DB。执行前确认这些资源没有其他用途。JUnit和HTML报告保存在 `tests/.artifacts/`。可用 `INTEGRATION_REDIS_DB`、`INTEGRATION_HTTP_PORT` 等环境变量调整。请勿把生产凭据或运行日志提交仓库。

## PowerShell 示例

```powershell
$env:HELLO_SERVICE_INTEGRATION_BASE_URL='http://127.0.0.1:18080'
$env:HELLO_SERVICE_INTEGRATION_WS_URL='ws://127.0.0.1:18090'
$env:HELLO_SERVICE_INTEGRATION_ADMIN_LOGIN='integration_admin'
$env:HELLO_SERVICE_INTEGRATION_ADMIN_PASSWORD='replace_me'
$env:HELLO_SERVICE_MYSQL_HOST='127.0.0.1'
$env:HELLO_SERVICE_MYSQL_PORT='3306'
$env:HELLO_SERVICE_MYSQL_USER='game_backend'
$env:HELLO_SERVICE_MYSQL_PASSWORD='replace_me'
$env:HELLO_SERVICE_MYSQL_DATABASE='game_backend_it_manual'
$env:HELLO_SERVICE_REDIS_HOST='127.0.0.1'
$env:HELLO_SERVICE_REDIS_PORT='6379'
$env:HELLO_SERVICE_REDIS_DB='1'

python -m pytest tests/integration -m integration -v
```

必须使用测试数据库和独立Redis DB，不要把生产环境凭据写入仓库或用于自动化测试。
