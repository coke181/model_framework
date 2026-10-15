# Linux 部署说明

## 结论

这套工程原本偏 Windows，本轮已经补了几项 Linux 落地需要的基础支持：

- `src/env.cpp` / `src/storage_factory.cpp` 改成跨平台环境变量读取
- `examples/hello/chat_gateway_main.cpp` 支持用环境变量配置聊天网关监听地址

## 聊天网关连接池指标

`hello_chat_gateway` 使用独立的指标注册表。默认在 `127.0.0.1:9091` 提供只读 HTTP 指标端点：

```text
GET http://127.0.0.1:9091/health
GET http://127.0.0.1:9091/metrics
```

监控端口默认只监听本机，生产环境不应直接开放到公网。可通过以下环境变量调整：

```bash
HELLO_CHAT_GATEWAY_METRICS_HOST=127.0.0.1
HELLO_CHAT_GATEWAY_METRICS_PORT=9091
```

设置 `HELLO_CHAT_GATEWAY_METRICS_PORT=0` 可关闭该端点；如果 Prometheus 在另一台机器上抓取，应优先通过内网、防火墙或反向代理放行，而不是直接绑定公网地址。

重点指标：

```text
mf_mysql_pool_size                         MySQL 连接池容量
mf_mysql_pool_in_use                       当前借出的 MySQL 连接数
mf_mysql_pool_wait_total{state="wait"}    发生等待的次数
mf_mysql_pool_acquire_total                借连接成功/超时次数
mf_mysql_pool_wait_duration_ms             借连接等待时延
mf_mysql_pool_connection_events_total       MySQL 新建、复用、连接失败次数
mf_redis_pool_size                          Redis 连接池容量
mf_redis_pool_in_flight                     当前执行中的 Redis 操作数
mf_redis_operations_total                   Redis 操作成功/失败次数
mf_redis_phase_duration_ms                  Redis 锁等待和命令执行时延
```

判断方式：`pool_in_use` 长时间接近 `pool_size` 且 `pool_wait_duration_ms` 上升，说明池容量或数据库吞吐不足；连接池未满但 `*_operation_duration_ms` 上升，优先排查 SQL、索引、Redis 命令或网络；`mf_mysql_pool_connection_events_total{status="error"}` 增长则先检查数据库连接、凭据和网络。

## 生产上线前检查

Windows/本地可先检查环境文件：

```powershell
.\scripts\check-production-config.ps1 `
  -EnvFile .\scripts\linux\deploy.env
```

服务本身也会在 `production` 环境拒绝以下配置，即使绕过脚本也不能启动：

- `HELLO_SERVICE_PERF_MODE=1`
- `HELLO_SERVICE_INSECURE_FAST_PASSWORD_HASH=1`
- Argon2 内存低于 `19456 KiB`
- Argon2 `time_cost` 低于 `2`

建议 MySQL 管理员执行一次原生慢查询配置。该操作需要 root 或
`SYSTEM_VARIABLES_ADMIN`，普通 `game_backend` 用户不应拥有此权限：

```sql
SET PERSIST slow_query_log = ON;
SET PERSIST long_query_time = 0.5;
SET PERSIST log_output = 'FILE';
```

应用自身仍会通过 `mf_mysql_operation_duration_ms` 和
`mf_alert_events_total{category="slow_mysql_operation"}` 记录仓储级慢操作。

## 备份与恢复

Windows 本地备份默认写入 D 盘项目目录：

```powershell
.\scripts\backup-local-storage.ps1
```

备份目录包含 `game_backend.sql`、`dump.rdb` 和带 SHA256 的 `manifest.json`。
恢复会覆盖数据，脚本要求显式确认：

```powershell
.\scripts\restore-local-storage.ps1 `
  -BackupDirectory D:\code\model\backups\<时间戳> `
  -ConfirmRestore
```

恢复 Redis 还要传 `-RestoreRedis`，并且必须先停止 `hello_service` 和 Redis。正式服务器应把
备份同步到另一块磁盘或对象存储；只保留在同一台机器上不能防止整机或磁盘故障。
- `CMakeLists.txt` 去掉纯 Windows 假设，允许 Linux 通过系统 OpenSSL / vcpkg x64-linux 查依赖
- 新增 Linux 启动脚本、`systemd` 模板、Nginx 模板

## 当前限制

以下是通用部署示例，不会自动修改线上配置。当前已核实服务器实际部署位于 `/opt/hello-service`，环境文件为 `/etc/hello-service/hello-service.env`；下面示例中的 `/opt/ue-game-backend` 仅是新部署建议路径，不能直接当作现有服务路径使用。

## 目录建议

建议放到：

`/opt/ue-game-backend`

目录结构参考：

```text
/opt/ue-game-backend
  ├─ CMakeLists.txt
  ├─ include/
  ├─ src/
  ├─ examples/
  ├─ db/
  ├─ scripts/linux/
  └─ deploy/
```

## 服务器基础依赖

以阿里云 Ubuntu 22.04 为例：

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build git curl unzip pkg-config \
  libssl-dev libcurl4-openssl-dev zlib1g-dev \
  mysql-client redis-tools nginx
```

如果你想用系统 MySQL / Redis：

```bash
sudo apt install -y mysql-server redis-server
```

## vcpkg 依赖

这套工程仍然依赖 `opentelemetry-cpp`。Linux 上建议也走 `vcpkg`：

```bash
cd /opt
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh
./vcpkg install opentelemetry-cpp[otlp-http]:x64-linux
```

构建时带上：

```bash
-DMF_VCPKG_ROOT=/opt/vcpkg
```

## 构建

如果服务器上另有 Boost 源码目录，例如 `/opt/boost_1_91_0`：

```bash
cd /opt/ue-game-backend
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMF_VCPKG_ROOT=/opt/vcpkg \
  -DMF_BOOST_ROOT=/opt/boost_1_91_0
cmake --build build -j
```

如果你没有单独的 Boost 源码目录，可以先试：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DMF_VCPKG_ROOT=/opt/vcpkg
cmake --build build -j
```

## 环境变量

复制模板：

```bash
cp scripts/linux/deploy.env.example scripts/linux/deploy.env
```

至少改这些：

- `HELLO_SERVICE_ENV=production`
- `HELLO_SERVICE_MYSQL_PASSWORD`
- `HELLO_SERVICE_MYSQL_AUTO_MIGRATE=1`
- `HELLO_SERVICE_MYSQL_SCHEMA_FILE=db/mysql/001_game_backend.sql`
- `HELLO_SERVICE_GM_TOKEN`
- `HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN=0`
  - `HELLO_SERVICE_HOST=127.0.0.1`，只允许 Nginx 访问 HTTP 服务
  - `HELLO_CHAT_GATEWAY_HOST=127.0.0.1`，只允许 Nginx `/ws` 访问聊天网关

运行环境说明：

- `HELLO_SERVICE_ENV=development`：本地开发环境，未显式关闭时会自动创建开发管理员，允许 `/auth/quick-login`。
- `HELLO_SERVICE_ENV=staging`：测试/预发布环境，默认不自动创建开发管理员，仍可保留部分调试接口。
- `HELLO_SERVICE_ENV=production`：正式环境，Linux 启动脚本默认值。强制禁用 `/auth/quick-login`，并强制关闭开发管理员自动创建，即使误配 `HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN=1` 也不会创建默认管理员。

服务器已有部署可使用下面的脚本切换为生产监听方式。脚本会备份环境文件，重启三个服务并检查健康接口：

```bash
sudo bash scripts/linux/harden-production.sh
```

切换后预期监听关系：

- `127.0.0.1:8080`：HTTP 后端，仅 Nginx 可访问
- `127.0.0.1:8090`：聊天网关，仅 Nginx `/ws` 可访问
- `0.0.0.0:80`：唯一公网 HTTP 入口
- `127.0.0.1:3306`：MySQL，不开放公网
- `127.0.0.1:6379`：Redis，不开放公网

MySQL schema 自动迁移说明：

- `HELLO_SERVICE_MYSQL_AUTO_MIGRATE=1`：服务启动时自动执行 `HELLO_SERVICE_MYSQL_SCHEMA_FILE`。
- 默认 schema 文件是 `db/mysql/001_game_backend.sql`。
- 这份 SQL 必须保持幂等，也就是用 `CREATE TABLE IF NOT EXISTS` 和条件 `ALTER TABLE`。
- HTTP 服务、聊天网关、后台 worker 同时启动时，会通过 MySQL `GET_LOCK` 串行执行 migration，避免多个进程同时改表。
- 如果你后续新增商城、拾取、语音房间、活动配置等表，把建表 SQL 加到 `db/mysql/001_game_backend.sql`，下次服务启动会自动补齐。
- 如果你要手动控库，可以设置 `HELLO_SERVICE_MYSQL_AUTO_MIGRATE=0`，然后自己执行 SQL。
- 详细说明见：[MySQL Schema 自动迁移说明](mysql_schema_migration.md)。

## 初始化数据库

```bash
mysql -uroot -p -e "CREATE DATABASE IF NOT EXISTS game_backend CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
mysql -uroot -p game_backend < db/mysql/001_game_backend.sql
```

如果你准备单独给服务创建账号：

```bash
mysql -uroot -p <<'SQL'
CREATE USER IF NOT EXISTS 'game_backend'@'127.0.0.1' IDENTIFIED BY 'change_me';
GRANT ALL PRIVILEGES ON game_backend.* TO 'game_backend'@'127.0.0.1';
FLUSH PRIVILEGES;
SQL
```

## systemd

创建运行用户：

```bash
sudo useradd -r -s /usr/sbin/nologin game || true
sudo chown -R game:game /opt/ue-game-backend
```

安装服务：

```bash
sudo cp deploy/systemd/hello-service.service /etc/systemd/system/
sudo cp deploy/systemd/hello-chat-gateway.service /etc/systemd/system/
sudo cp deploy/systemd/hello-task-worker.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now hello-service hello-chat-gateway hello-task-worker
```

注意：

- 如果你的项目目录实际是 `/opt/ue-game-backend/model`，请确保单元文件里的：
  - `WorkingDirectory=/opt/ue-game-backend/model`
  - `EnvironmentFile=/opt/ue-game-backend/model/scripts/linux/deploy.env`
  - `ExecStart=/opt/ue-game-backend/model/scripts/linux/start-*.sh`
- 当前服务器实际目录为 `/opt/hello-service`，正式环境文件在 `/etc/hello-service/hello-service.env`；部署前请读取已安装的systemd单元文件确认 `WorkingDirectory`、`EnvironmentFile` 和 `ExecStart`，不要照抄上面的示例路径。

查看日志：

```bash
sudo journalctl -u hello-service -f
sudo journalctl -u hello-chat-gateway -f
sudo journalctl -u hello-task-worker -f
```

## Nginx

安装配置：

```bash
sudo cp deploy/nginx/hello-admin.conf /etc/nginx/sites-available/hello-admin.conf
sudo ln -sf /etc/nginx/sites-available/hello-admin.conf /etc/nginx/sites-enabled/hello-admin.conf
sudo nginx -t
sudo systemctl reload nginx
```

### Nginx 高并发配置

`deploy/nginx/hello-admin.conf` 已为 HTTP 后端配置 `upstream game_backend` 和
`keepalive 256`，让 Nginx 复用到 `127.0.0.1:8080` 的连接；`/ws` 仍单独转发到
聊天网关并保留 WebSocket Upgrade 头。

服务器的 `/etc/nginx/nginx.conf` 还应将 `events` 段设置为：

```nginx
events {
    worker_connections 8192;
}
```

修改主配置后必须先执行 `sudo nginx -t`，确认成功再执行
`sudo systemctl reload nginx`。项目提供了安全应用脚本：

```bash
sudo bash scripts/linux/apply-nginx-tuning.sh /tmp/hello-admin.conf
```

脚本会把备份放到 `/etc/nginx/backups`，不会将备份文件放入
`sites-enabled`，并在配置校验失败时自动恢复。

本机回环压测的参考结果：后端直连 `8080` 约 2.7 万 req/s；经过优化后的
Nginx `80` 端口约 1.85 万 req/s。实际公网吞吐还会受到网络 RTT、带宽和客户端
连接复用方式影响。

## 服务器告警

项目提供轻量级 systemd 监控器，不需要把 Prometheus 端口暴露到公网：

```bash
sudo cp deploy/systemd/hello-backend-monitor.service /etc/systemd/system/
sudo cp deploy/systemd/hello-backend-monitor.timer /etc/systemd/system/
sudo cp scripts/linux/monitor-backend.sh /opt/hello-service/scripts/linux/
sudo chmod 0755 /opt/hello-service/scripts/linux/monitor-backend.sh
sudo systemctl daemon-reload
sudo systemctl enable --now hello-backend-monitor.timer
```

监控器每分钟检查：

- 根分区使用率，默认 85% 告警、95% 临界告警
- CPU 使用率，默认 90% 告警
- `nginx`、`hello-service`、`hello-chat-gateway`、`hello-task-worker` 状态
- HTTP `/health` 是否可用
- MySQL 连接池占用率和连接错误
- Redis 操作错误
- Chat Outbox 重试耗尽和队列积压

告警和恢复事件写入 journald，状态变化才通知一次，避免每分钟刷屏：

```bash
sudo journalctl -t ue-game-monitor -f
systemctl list-timers hello-backend-monitor.timer
```

可在 `/etc/hello-service/hello-service.env` 增加
`HELLO_MONITOR_WEBHOOK_URL=https://...` 接收 JSON Webhook。Webhook 地址属于敏感配置，
不要提交到 Git。

阈值可通过 systemd 环境覆盖，例如：

```ini
Environment=CPU_WARN_PERCENT=85
Environment=DISK_WARN_PERCENT=80
Environment=POOL_WARN_PERCENT=90
```

## 长时间业务压测

压测脚本会自动注册唯一前缀的测试账号，然后依次测试真实登录、玩家邮件发送/读取、
经 Nginx `/ws` 代理的聊天连接。它不会使用 quick-login，也不会输出密码或 token：

```bash
python3 /opt/hello-service/scripts/linux/long-business-load.py \
  --duration 180 \
  --accounts 8 \
  --login-workers 2 \
  --mail-workers 2 \
  --chat-clients 4 \
  --output /tmp/ue-game-long-load
```

每个阶段的 `duration` 单位是秒。报告目录包含 `report.txt`、压测前后 metrics、
进程、内存和磁盘快照。登录阶段会命中项目配置的成功校验缓存；如需专门测 Argon2
每次校验，应使用足够多的不同账号并关闭成功校验缓存，而不是把本轮登录接口吞吐
直接当作 Argon2 吞吐。

## 隔离功能回归

MySQL/Redis与WebSocket的8项真实链路测试可以通过 `scripts/linux/run-isolated-integration-tests.sh` 执行。该脚本需要MySQL本地管理员socket权限、空闲的非生产Redis DB和本地编译的两个服务二进制；只启动监听回环地址的临时进程，不重启线上服务。配置、前置检查、清理方式和报告位置见 `tests/integration/README.md`。当前云服务器实际部署路径与示例目录不同，测试请使用独立临时源码目录和 `HTTP_BIN`/`CHAT_BIN` 指向待验证二进制，勿直接构建覆盖生产二进制。

## 阿里云安全组

至少放行：

- `80`
- `8080` 如果你还想直接调后端
- `8090` 如果聊天网关走公网直连

## 推荐公网访问方式

- 管理台和 HTTP API：`http://120.26.173.47/` 或 `http://120.26.173.47/admin`
- HTTP 直连：`http://120.26.173.47:8080`
- 聊天网关：`ws://120.26.173.47:8090`

## 强烈建议

- 不要暴露 MySQL 3306 到公网
- 不要暴露 Redis 6379 到公网
- 服务器明确配置：
  - `HELLO_SERVICE_ENV=production`
- 关闭开发态自动管理员：
  - `HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN=0`
- 上线前把后台走 HTTPS
