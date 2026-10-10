# 性能测试统一说明

## 文档职责

这份文档是项目性能测试的入口，统一说明测试类型、运行条件、结果解释和复测命令。
详细历史数据仍保存在 `docs/performance-baseline-20260924.md`。

## 测试前提

- 固定构建类型。正式对比使用 `build-tracy\\RelWithDebInfo\\hello_service.exe`，不要把 Debug 结果和 Release 结果混在一起。
- 真实持久化测试使用 MySQL `127.0.0.1:3306` 和 Redis `127.0.0.1:6379`，当前本机实例和数据均放在 D 盘。
- 纯 HTTP 测试使用 `/health`，不访问 MySQL、Redis，也不能代表业务接口容量。
- 登录测试必须使用多个不同账号，重复同一个账号可能命中短期缓存，不能代表真实玩家同时登录。
- 结果至少记录吞吐、成功率、P50/P95/P99、CPU、业务队列、MySQL 连接池等待和 Redis 命令耗时。

## 当前实测汇总

### 纯 HTTP `/health`

优化构建、同机 Python `requests` 客户端、5000 个 GET 请求：

| 客户端并发 | 吞吐 | P99 | 成功率 |
|---:|---:|---:|---:|
| 16 | **1046/s** | 32ms | 100% |
| 64 | 852/s | 129ms | 100% |
| 128 | 851/s | 284ms | 100% |
| 256 | 794/s | 325ms | 100% |
| 512 | 757/s | 641ms | 100% |

这组数据包含 Python 客户端开销；使用 `wrk` 或 `bombardier` 可以进一步测 HTTP 层理论上限。当前可持续工作点约为 16 并发、1000 请求/秒，继续增加并发主要增加排队和尾延迟。

### Bombardier 专用客户端复测

使用 `D:\\tools\\bombardier\\bombardier.exe`、`build-tracy\\RelWithDebInfo` 服务和本机回环网络，对 `/health` 做 15 秒测试：

| HTTP 连接数 | 吞吐 | 平均延迟 | P99 | 成功率 |
|---:|---:|---:|---:|---:|
| 16 | **15820.8/s** | 1.01ms | 1.91ms | 100% |
| 64 | **15816.4/s** | 4.04ms | 5.56ms | 100% |
| 128 | **15353.5/s** | 8.33ms | 10.98ms | 100% |
| 256 | **15198.4/s** | 16.83ms | 19.96ms | 100% |
| 512 | **14771.5/s** | 34.61ms | 40.65ms | 100% |
| 1024 | **15292.3/s** | 66.79ms | 73.53ms | 100% |
| 2048 | **15378.7/s** | 132.50ms | 150.92ms | 100% |

128 连接持续 30 秒时达到 `15364.2/s`，`460968/460968` 请求成功。服务进程约消耗 `2.3` 个逻辑 CPU 核，未达到 CPU 饱和；因此这不是服务器极限测试，当前瓶颈可能转移到了单机回环网络、HTTP 连接调度、客户端或测试服务的观测开销。连接数超过 512 后吞吐没有增长，P99 明显恶化，建议以 `64~128` 连接作为当前短请求基线。

这组专用客户端数据解释了之前的差异：Python `requests` 约 `1046/s`、`httpx` 约 `304~639/s`，主要是压测客户端限制；C++ HTTP 层的同机短请求结果约 `15k/s`。不能把 `/health` 的 `15k/s` 直接套用到真实登录、注册、邮件或聊天业务。

### 异步 Python 客户端复核

本机额外安装到 `D:\\code\\model\\tools\\python-packages` 的 `httpx` 异步客户端复测结果如下：

| 客户端并发 | 吞吐 | P99 | 成功率 |
|---:|---:|---:|---:|
| 16 | 639/s | 67.7ms | 100% |
| 64 | 378/s | 1319ms | 100% |
| 128 | 304/s | 4407ms | 100% |

异步客户端没有在本机 Windows 环境下提高结果，且服务端 CPU 仍未接近满载，说明 `httpx`/Windows 事件循环/连接复用本身成为客户端瓶颈。该结果不能用于判断服务器极限；正式纯 HTTP 上限测试应优先使用 `wrk` 或 `bombardier`，并把压测端移到另一台机器。

### 真实 MySQL + Redis 登录

优化构建、400 个不同账号、真实密码校验、真实 MySQL 查询、Redis Session/风控：

| 客户端并发 | 吞吐 | P99 | 成功率 |
|---:|---:|---:|---:|
| 16 | **845.3/s** | 25.1ms | 100% |
| 32 | 562.9/s | 73.4ms | 100% |
| 64 | 563.0/s | 122.0ms | 100% |

这次指标确认执行了 400 次 `upsert_for_login`、455 次密码账号查询和 2400 次 Redis 登录成功写入。当前单机登录较合适的并发点约为 16。

本轮真实持久化登录复测使用注册出来的 1200 个不同账号：

| 客户端并发 | 吞吐 | P50 | P95 | P99 | 成功率 |
|---:|---:|---:|---:|---:|---:|
| 16 | **753.2/s** | 18.7ms | 29.2ms | 33.5ms | 400/400 |
| 32 | 581.1/s | 47.7ms | 64.2ms | 71.4ms | 400/400 |
| 64 | 581.4/s | 86.2ms | 109.7ms | 117.6ms | 400/400 |

本轮 MySQL 连接池等待累计 170 次，Redis 互斥等待累计约 `4.2ms`，Redis 命令阶段累计约 `10894.2ms`。32/64 并发没有增加吞吐而只提高尾延迟，当前主要限制更接近密码哈希 CPU 和业务线程排队，而不是 Redis 锁竞争。下一步应优先做登录/注册进程拆分或多进程横向扩展哈希 CPU，并单独压测密码哈希线程数；不要为了追求吞吐而降低正式密码哈希成本。MySQL 连接池只有在等待持续主导 P99 时才需要继续扩大。

### 真实 MySQL + Redis 注册

唯一账号注册、正式密码哈希参数：

| 客户端并发 | 吞吐 | P99 | 成功率 |
|---:|---:|---:|---:|
| 32 | **52.8/s** | 764.6ms | 100% |

注册比登录慢是正常的：每次都要执行密码哈希、账号写入、角色初始化和 Session 创建。不能通过缓存明文密码或降低正式哈希安全参数来换吞吐。

本轮在注册限流调高、请求日志采样调低、`PERF_MODE=1` 的开发压测配置下复测：

| 请求 | 并发 | 吞吐 | P50 | P95 | P99 | 成功率 |
|---|---:|---:|---:|---:|---:|---:|
| 唯一账号注册 | 32 | **155.2/s** | 195.2ms | 278.5ms | 309.5ms | 1200/1200 |

指标显示注册阶段的 MySQL 操作累计耗时约为：`create_password_account` `23091.8ms`、`create_for_registration` `34206.5ms`、异步 `record_login_success_without_fetch` `15448.5ms`。这说明注册同时受到密码哈希和多次持久化写入影响，不能只增加 HTTP 线程解决。

### 2026-09-30 冷密码校验和登录热路径复测

为避免重复账号命中进程内成功校验缓存，本轮把 `HELLO_SERVICE_PASSWORD_VERIFY_CACHE_TTL_MS` 设置为 `0`，使用 800 个不同账号，每个服务进程配置 8 个密码哈希线程。登录查询现在通过一次 `LEFT JOIN player_profile` 同时取得 `player_id`，已有角色不再额外查询玩家主表；没有角色的历史账号仍回退原流程。

| 服务进程数 | 总并发 | 吞吐 | P50 | P95 | P99 | 成功率 |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 16 | **159.3/s** | 98.6ms | 109.1ms | 132.7ms | 800/800 |
| 2 | 32 | **191.1/s** | 160.7ms | 221.8ms | 246.2ms | 800/800 |
| 4 | 64 | **210.5/s** | 267.8ms | 446.9ms | 495.8ms | 800/800 |

单进程登录吞吐从之前约 `154/s` 小幅提升到 `159/s`，说明减少一次玩家查询有效但不是主瓶颈。2/4 进程可以扩展总吞吐，但受到本机共享 CPU、MySQL 和 Redis 资源限制，不能线性增长；P99 随总并发增加明显上升。当前最有效的扩展方向仍是合理配置密码哈希线程后横向增加机器/进程，而不是继续增加 HTTP 业务线程。

### 2026-09-30 密码哈希线程数复测

本轮使用同一批 800 个不同账号、密码校验缓存关闭、单个 RelWithDebInfo 服务进程，
客户端并发固定为 16。结果用于寻找本机的线程数平台点，不代表跨机器容量：

| 密码哈希线程数 | 吞吐 | P50 | P95 | P99 | 成功率 |
|---:|---:|---:|---:|---:|---:|
| 4 | 103.4/s | 153.1ms | 170.6ms | 178.8ms | 800/800 |
| 8 | 166.7/s | 94.5ms | 105.9ms | 122.4ms | 800/800 |
| 12 | 213.3/s | 73.1ms | 89.7ms | 106.1ms | 800/800 |
| 16 | **227.4/s** | 67.7ms | 82.7ms | 138.3ms | 800/800 |

两进程各 8 个密码哈希线程、总并发 32 时为 `216.6/s`，P99 `227.7ms`，没有超过
单进程 16 线程的结果。原因是两个进程共享本机 CPU、MySQL 和 Redis；多进程只有在有
额外 CPU 资源或部署到多台机器时才会继续产生明显收益。当前本机建议先从
`PasswordHashThreads=8~12` 开始，16 线程虽然吞吐更高，但尾延迟已经出现回升。

同一参数下，真实注册使用 800 个全新账号、并发 32，结果为 `175.4/s`，P50 `163.4ms`，
P95 `270.5ms`，P99 `295.7ms`，成功率 `800/800`。注册每次都包含 Argon2/PBKDF2
密码哈希、账号和玩家初始化、Session 创建及相关持久化写入，因此不能和缓存命中登录比较。

本轮指标显示 MySQL 连接池等待约为 `1.2ms/804 次`，Redis 互斥等待约 `0.4ms/800 次`，
两者都不是主要排队点；Redis command 阶段累计约 `16.6s/800 次`，后续应继续拆分登录
成功路径的 Redis 命令耗时，而不是盲目扩大 MySQL 连接池。密码哈希安全参数不应为了吞吐降低。

为区分 Redis 是否是端到端瓶颈，使用同一批账号做了开发诊断：真实 MySQL + Redis 为
`192.0/s`；仅关闭 Redis Session 为 `187.7/s`；同时关闭 Redis Session 和登录风控为
`188.9/s`。结果差异处于本机调度噪声范围，不能证明关闭 Redis 有收益；生产环境仍应
保持 Session 和登录风控使用 Redis，确保多进程共享会话、限流、黑名单和验证码状态。

### 内存仓储登录/注册

内存仓储只适合测 HTTP、线程池和密码哈希，不代表生产容量：

- 唯一注册：约 `187.9/s`，P99 `192ms`。
- 之前约 `10000/s` 的登录属于缓存命中或非完整持久化链路，不能与不同账号真实登录比较。

### 业务读写

历史数据见 `docs/performance-baseline-20260924.md`：

- 内存仓储小数据读接口曾达到约 `1.4 万/s`，不代表真实生产容量。
- 真实 MySQL/Redis 读接口曾达到约 `4200/s` 的同机测试吞吐。
- 邮件写入约 `1100~1200/s`，具体取决于接口和并发。
- 同一玩家背包写入受数据库行锁保护，正确性优先于吞吐；高并发时应观察锁等待。

## 复测命令

### 启动 D 盘 MySQL/Redis

```powershell
powershell -ExecutionPolicy Bypass -File .\\scripts\\start-local-mysql.ps1
powershell -ExecutionPolicy Bypass -File .\\scripts\\start-local-redis.ps1
```

### 启动真实存储服务

```powershell
powershell -ExecutionPolicy Bypass -File .\\scripts\\run-hello-mysql-redis.ps1 `
  -ServiceExePath D:\\code\\model\\build-tracy\\RelWithDebInfo\\hello_service.exe
```

如优化构建启动失败，确认以下 DLL 位于可执行文件目录：

```text
TracyClient.dll
libssl-3-x64.dll
libcrypto-3-x64.dll
```

### 纯 HTTP 基线和过载

如果安装了 `wrk` 或 `bombardier`，直接运行：

```powershell
powershell -ExecutionPolicy Bypass -File .\\scripts\\load-test-baseline.ps1 -Tool auto
powershell -ExecutionPolicy Bypass -File .\\scripts\\load-test-overload.ps1 -Tool auto
```

脚本会在结束后读取 `/metrics`，关注：

```text
mf_http_requests_total
mf_http_request_duration_ms_*
mf_http_slow_requests_total
mf_http_rejected_requests_total
mf_inflight_requests
mf_pending_business_tasks
```

### 真实登录/注册

```powershell
python .\\scripts\\perf-auth-cluster.py `
  --mode register --ports 8080 --concurrency 32 --limit 800 `
  --login-prefix perf_reg_ --display-prefix PerfReg

python .\\scripts\\perf-auth-cluster.py `
  --mode login --ports 8080 --concurrency 16 --limit 800 `
  --login-prefix perf_auth_
```

脚本默认使用本机真实 MySQL 账号 `game_backend` 和数据库 `game_backend`，也可通过
`--mysql-host`、`--mysql-port`、`--mysql-user`、`--mysql-password`、`--mysql-database`
覆盖，或设置对应的 `PERF_MYSQL_*` 环境变量。登录模式从数据库读取指定前缀的已有账号；
注册模式每轮自动加入时间后缀，避免重复注册冲突。测试数据只能放在可丢弃的开发数据库中。

### 业务延迟和 CPU

```powershell
powershell -ExecutionPolicy Bypass -File .\\scripts\\profile-business-latency.ps1 `
  -StorageMode mysql_redis -Concurrency 24 -DurationSeconds 30
powershell -ExecutionPolicy Bypass -File .\\scripts\\profile-cpu.ps1
```

## 指标判读

- `P50` 代表典型请求，`P95/P99` 代表高峰尾延迟；优化不能只看平均值。
- 吞吐不随并发增加而增加、P99 升高，通常说明 CPU、密码哈希、数据库锁或业务线程池已经饱和。
- `mf_mysql_pool_wait_total` 持续增加，说明连接池可能不足或 SQL 长时间占用连接。
- Redis `mutex_wait` 高说明单连接复用层竞争明显；Redis `command` 高说明 Redis 本身或网络/命令数量需要优化。
- HTTP 429/503 是背压生效，不应简单当成服务崩溃；需要同时记录拒绝原因和队列深度。
- 同机客户端测试会混入客户端 CPU 和回环网络因素，正式容量测试应把压测端移到另一台机器。

## 下一步优化顺序

1. 固定 RelWithDebInfo 构建，使用同一批次不同账号重新建立登录基线。
2. 用 `wrk` 或 `bombardier` 测 `/health`，区分 Python 客户端瓶颈和服务器 HTTP 上限。
3. 登录继续保持密码安全参数，使用多个服务进程横向扩展密码哈希 CPU。
4. 观察 MySQL 慢 SQL、行锁等待和连接池等待，不要盲目扩大连接池。
5. 登录成功后的资料、好友、背包、公告等数据继续使用懒加载，不放回登录主链路。

## 2026-09-30 单机真实链路复测

本轮目标是确认单台 Windows 机器在不考虑多机扩展时的实际边界。测试主机为
Intel i7-13700H，14 个物理核心、20 个逻辑处理器，MySQL 和 Redis 都运行在本机
回环地址。服务使用关闭 Tracy 的 `RelWithDebInfo` 构建，正式 Argon2id 参数保持不变：
`m=19456 KiB,t=2,p=1`。登录压测关闭成功密码校验缓存
（`HELLO_SERVICE_PASSWORD_VERIFY_CACHE_TTL_MS=0`），因此结果不是缓存命中结果。

| 场景 | 并发 | 请求数/时长 | 吞吐 | P50 | P95 | P99 | 结果 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `/health` 纯 HTTP | 1000 | 10 秒 | **22434 req/s** | 42.4 ms | 55.9 ms | 62.1 ms | 全部 2xx |
| 固定账号真实登录，MySQL + Redis | 80 | 10 秒 | **233.6/s** | 346.2 ms | 374.8 ms | 407.1 ms | 全部 2xx |
| 不同账号真实登录，MySQL + Redis | 20 | 800 次 | **225.8/s** | 86.7 ms | 105.6 ms | 113.7 ms | 800/800 200 |
| 唯一账号真实注册，MySQL + Redis | 20 | 200 次 | **166.9/s** | 90.6 ms | 287.9 ms | 332.4 ms | 200/200 201 |

固定账号和不同账号的吞吐接近，说明本轮关闭密码校验缓存后，账号查询缓存不是主要
决定因素。MySQL 查询和连接池等待仍处于毫秒级，登录主耗时来自每个请求独立执行的
Argon2id。注册还需要账号写入、玩家初始化和会话持久化，因此不能用登录吞吐推算注册
容量。

### 线程扫描结论

密码校验线程不是越多越快。本轮在同一台机器上用 80 并发扫描了 12、16、20、24
线程；在系统负载较干净时，20 线程约 `234/s`，24 线程一次测试降到约 `135/s`，
P99 升到约 `714ms`。后续测试期间发现桌面游戏进程和后台程序会显著污染 CPU/内存带宽，
所以短时扫描只能用于找明显的过度并发点，最终容量应在空闲主机上连续跑多轮。

当前单机建议：

- 正式环境默认会保留约 20% 逻辑 CPU 给 HTTP、MySQL 和 Redis；本机 20 线程处理器会自动选择约 16 个密码线程。仍可使用 `PasswordHashThreads=12~20` 做 P99 对照。
- 不要为了追求 5000/s 或 10000/s 降低 Argon2id 内存/迭代参数，也不要缓存失败密码。
- 20 个逻辑线程不是 20 个独立物理核心，继续增加密码线程会争用 CPU、缓存和内存带宽。
- 登录成功后的资料、背包、好友、公告等信息继续懒加载；登录只返回身份和会话必须字段。
- Session、风控和登录审计保持独立的 Redis/后台队列设计；本机极限测试可以临时使用
  内存 Session，但该数据不能作为生产容量。

### 可复现命令

关闭 Tracy 的正式优化构建：

```powershell
cmake --build .\build --config RelWithDebInfo --target hello_service -j 20
```

真实 MySQL + Redis 登录：

```powershell
.\scripts\start-hello-service-cluster.ps1 `
  -ServiceExePath D:\code\model\build\RelWithDebInfo\hello_service.exe `
  -LogDirectory D:\code\model\build `
  -StopExisting -Instances 1 -BasePort 8080 `
  -PasswordHashThreads 20 -PasswordHashMaxPending 512 `
  -PasswordVerifyCacheTtlMs 0 -BusinessThreads 20 -PerfMode

D:\tools\bombardier\bombardier.exe `
  -c 80 -d 10s -l -m POST `
  -H "Content-Type: application/json" `
  -f .\scripts\perf-login-body.json `
  http://127.0.0.1:8080/auth/login
```

不同账号登录和注册使用：

```powershell
python .\scripts\perf-auth-cluster.py `
  --mode login --ports 8080 --concurrency 20 --limit 800 `
  --login-prefix perf_auth_

python .\scripts\perf-auth-cluster.py `
  --mode register --ports 8080 --concurrency 20 --limit 200 `
  --login-prefix r_ --display-prefix Perf
```

注册压测脚本现在会在开始前检查生成的登录名是否超过后端 32 字符限制，并给出明确
错误；不要使用带很长固定前缀再叠加时间戳的名字。

### 优化判断

本轮没有发现 MySQL 连接池或 Redis mutex 是登录吞吐的主瓶颈，因此没有继续盲目扩大
连接池，也没有修改数据库协议。下一步真正有价值的是：在空闲机器上重复多轮并采集
CPU/温度/内存带宽，增加 Argon2 校验阶段和密码队列等待指标，并验证登录成功后的
异步审计队列是否在长时间运行中增长。若目标仍是千级以上的真实不同账号登录，必须
增加可用 CPU 资源或采用受控的密码校验缓存策略；仅增加 HTTP worker 数不会解决
Argon2 的计算成本。

## 2026-09-30 C++20 协程与异步 Redis refresh 复测

本轮没有把所有接口一次性改成协程，而是先选择纯 Redis I/O 的 `POST /auth/refresh`
验证收益。同步路由继续使用业务线程池；异步路由新增 `AsyncHandler/AsyncMiddleware`，
仍经过 tracing、metrics 和请求日志。调用链如下：

```text
HttpServer 异步分发
  -> Router::dispatch_async
  -> TokenService::refresh_async
  -> SessionRepository::consume_refresh_token_async
  -> Boost.Redis async_exec
  -> SessionRepository::save_async
  -> 写回 HTTP Response
```

Redis 连接池做了两项必要修正：连接使用 `std::call_once` 保证并发首次访问时只启动一个
I/O 线程；连接池创建时主动启动连接，避免首批请求承担握手延迟。`async_exec` 固定在该
Redis 连接自己的 executor 上发起，完成后恢复 HTTP 协程，不再使用 `future.get()` 阻塞
业务线程。refresh token 仍由 Lua 原子消费，并发重放仍然只能有一个请求成功。

Token 本地缓存新增 `session_id -> token` 和 `player_id -> token 集合` 二级索引。
刷新、踢会话和按玩家撤销不再扫描整个 token cache，分别降为 O(1) 和 O(该玩家会话数)。

真实 MySQL + Redis、`RelWithDebInfo`、Redis pool=16 的本机结果：

| 场景 | 并发 | 请求数/时长 | 吞吐 | P50 | P95 | P99 | 结果 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| refresh 连续轮换 | 1 | 1000 次 | **507.7/s** | 1.93 ms | 2.30 ms | 2.78 ms | 1000/1000 200 |
| 多账号 refresh | 20 | 800 次 | **698.3/s** | 26.73 ms | 42.91 ms | 49.61 ms | 800/800 200 |
| 多账号 refresh | 40 | 800 次 | **1082.1/s** | 32.50 ms | 51.34 ms | 59.09 ms | 800/800 200 |
| 多账号 refresh | 80 | 800 次 | **1035.5/s** | 44.96 ms | 82.87 ms | 100.71 ms | 800/800 200 |
| `/health` 纯 HTTP | 256 | 10 秒 | **19895/s** | 12.97 ms | 16.06 ms | 17.10 ms | 199149 次 2xx、无错误 |
| 不同账号真实登录 | 16 | 400 次 | **200.9/s** | 69.7 ms | 123.5 ms | 158.9 ms | 400/400 200 |
| 唯一账号真实注册 | 16 | 400 次 | **160.9/s** | 85.0 ms | 193.4 ms | 293.7 ms | 400/400 201 |

重放保护复测为 `replay_status={200: 1, 401: 15}`。串行 refresh 相比改造前约
`435/s` 提升到约 `508/s`；40 并发稳定在约 `1.08k/s`。继续把 Redis 鉴权、在线状态、
聊天状态等 I/O 型接口迁到同一异步模型有意义，但 Argon2id 登录/注册是 CPU 和内存带宽
任务，不能在 I/O 协程线程里直接执行，仍应保留独立有界密码线程池。协程减少的是等待
期间占用的线程，不会让密码哈希算法本身变快。

### Argon2id 调度与成功缓存优化

保持正式安全参数 `m=19456 KiB,t=2,p=1` 不变，对本机密码 worker 数做了重新扫描：

| 密码 worker | 不同账号冷登录吞吐 | P99 |
| ---: | ---: | ---: |
| 12 | 207.6/s | 196.2 ms |
| 16 | **233.7/s** | **108.8 ms** |
| 20 | 229.0/s | 118.1 ms |

本机 20 个逻辑处理器下，16 个密码 worker 仍是较好的吞吐和尾延迟平衡点。继续增加
线程不会增加内存带宽，只会让多个 19 MiB Argon2 工作区争用缓存和内存控制器。

成功密码缓存命中现在会在进入 `PasswordWorkDispatcher` 前直接返回，不再排在冷 Argon2
任务后面，也不占用密码 worker。失败密码仍不缓存。30 秒 TTL 下，固定账号重复登录实测
约 **7198/s**、P99 **13.33 ms**；不同账号冷登录仍约 **231/s**，说明缓存快速路径没有
降低真实 Argon2 校验能力。

生产客户端应优先保存 refresh token，并在 access token 失效后调用 `/auth/refresh`。
只有 refresh token 也无效、用户主动退出或安全策略要求重新认证时，才重新提交密码。
这是减少 Argon2 CPU/内存消耗最有效且不降低密码参数的方式。

### 去掉 Argon2id 的链路上限基准

为了单独测量 HTTP、MySQL、Redis 和会话写入能力，增加了受保护的基准开关：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\start-hello-service-cluster.ps1 `
  -ServiceExePath D:\code\model\build\RelWithDebInfo\hello_service.exe `
  -Instances 1 -BasePort 8080 -PasswordHashThreads 16 `
  -PasswordVerifyCacheTtlMs 0 -BusinessThreads 20 -AcceptConcurrency 40 `
  -PerfMode -InsecureFastPasswordHash `
  -RegisterMaxPerIp 100000 -RegisterMaxPerLoginIp 100000 -StopExisting
```

该模式使用 `PBKDF2-SHA256(iterations=1)`，仍然带随机盐且不保存明文，但不具备实际的
密码抗暴力破解能力。代码强制要求 `PERF_MODE`，production 环境禁止启动；普通模式也会
拒绝验证这种低迭代测试哈希。测试完成后必须重启普通 Argon2 配置。

本机真实 MySQL + Redis 结果：

| 场景 | 并发 | 吞吐 | P50 | P99 |
| --- | ---: | ---: | ---: | ---: |
| 快速哈希注册 | 40 | **630.0/s** | 46.6 ms | 198.9 ms |
| 快速哈希登录 | 20 | **1093.3/s** | 17.3 ms | 31.7 ms |
| 快速哈希登录 | 40 | **1072.2/s** | 33.9 ms | 63.0 ms |
| 快速哈希登录 | 80 | **1082.6/s** | 44.0 ms | 103.6 ms |
| 快速哈希登录 | 160 | **1012.2/s** | 46.2 ms | 110.7 ms |

结论是：去掉 Argon2id 后，单机不同账号登录从约 `230/s` 提升到约 `1.09k/s`，但仍没有
达到 5000/s 或 10000/s，说明后续瓶颈已经转移到 MySQL 查询、Redis Session 写入、审计更新、
连接池和压测客户端。这个基准不能用于决定降低生产密码参数；生产仍应使用 Argon2id，
并通过 refresh token、密码线程池调度和增加 CPU 实例扩展容量。

### HTTP accept 与每连接独立 strand 优化

旧 Listener 同时只保持一个 `async_accept`，并复用同一个 strand executor 创建所有 socket。
这会让连接突发只能逐个接收，而且不同客户端的读写完成回调可能被同一个 strand 串行化。

当前实现改为：

- 默认保持 `2 × I/O线程数` 个挂起 accept，本机 20 个 I/O 线程对应 40 个 accept。
- 每个新连接创建独立 strand，不再让所有 HTTP 连接共享 Listener strand。
- 已接收 socket 开启 `TCP_NODELAY`，降低小型请求/响应的等待。
- 可通过 `HELLO_SERVICE_ACCEPT_CONCURRENCY` 或启动脚本的 `-AcceptConcurrency` 覆盖。

同一台 Windows 本机、同一个 `RelWithDebInfo` 构建的 `/health` 结果：

| 长连接数 | 吞吐 | P50 | P95 | P99 | 结果 |
| ---: | ---: | ---: | ---: | ---: | --- |
| 256 | **119363/s** | 1.90 ms | 4.38 ms | 6.87 ms | 1197218 次 2xx、无错误 |
| 512 | **104288/s** | 4.55 ms | 8.42 ms | 11.96 ms | 1044489 次 2xx、31 次本机拨号拒绝 |
| 1000 | **90467/s** | 4.81 ms | 9.79 ms | 20.19 ms | 服务持续运行，但本机客户端出现 7669 次拨号拒绝 |

256 连接下相较旧实现约 `19895/s` 提升到约 `119363/s`。1000 连接测试中的
`connection refused` 不代表服务进程退出；测试后服务仍在监听且健康检查正常。该现象还会
受到 Windows 本机客户端临时端口、连接突发速度和监听队列影响。正式测试“可保持多少连接”
应使用另一台压测机，并把长连接保持能力与每秒请求吞吐分开测试。

可复现命令：

```powershell
python .\scripts\perf-refresh-chain.py `
  --login-name <测试账号> --password 'change_me' `
  --requests 1000 --replay-concurrency 16

python .\scripts\perf-refresh-multi.py `
  --accounts 800 --login-concurrency 20 --refresh-concurrency 40

powershell -ExecutionPolicy Bypass -File .\scripts\load-test-http.ps1 `
  -Tool auto -Path /health -Connections 256 -DurationSeconds 10
```

## 2026-09-30 Redis Session key 收敛优化

固定账号高频登录测试暴露了两个独立问题：

1. 本机原 `6379` Redis 已积累约 `534 万` 个带 TTL 的测试 key，占用约 `1.93GB`。
2. Redis 默认 RDB 规则会在 `60 秒内 10000 次变更` 后触发后台快照；本机 MSYS Redis
   最近一次保存耗时约 26 秒，期间服务观测到约 `548ms` 的 Redis 命令停顿。

压测不要直接复用开发数据 Redis。现在可在 D 盘启动独立、仅绑定回环地址、关闭 RDB/AOF
的性能实例：

```powershell
.\scripts\start-local-redis.ps1 `
  -Port 6380 `
  -RedisDataDir D:\redis\perf-data `
  -RedisLogPath D:\redis\redis-perf.log `
  -DisablePersistence

.\scripts\start-hello-service-cluster.ps1 `
  -ServiceExePath D:\code\model\build\RelWithDebInfo\hello_service.exe `
  -Instances 1 -BasePort 8080 `
  -RedisPort 6380 -RedisPoolSize 16 `
  -PasswordHashThreads 16 -BusinessThreads 20 -AcceptConcurrency 40 `
  -PerfMode -InsecureFastPasswordHash -StopExisting
```

`-DisablePersistence` 只适用于独立性能实例。正式 Redis 是否持久化应按会话丢失容忍度、
风控状态要求和灾备方案决定，不能直接照搬压测配置。

Session token 新格式为：

```text
<32位随机session_id>.<64位随机秘密>
```

客户端仍必须把 access token 和 refresh token 当作不透明字符串，不能解析、拼接或自行生成。
服务端利用前缀直接定位 `session:id:<session_id>`，然后比对完整 token，所以单独知道
`session_id` 不能通过鉴权。旧版纯十六进制 token 仍通过原反向索引读取，升级不会立刻踢掉
已有会话。

新会话保存从 5 条 Redis 命令降为 3 条：

```text
SET session:id:<id> <payload> EX <ttl>
SADD session:player:<player_id> <id>
EXPIRE session:player:<player_id> <ttl>
```

相同 Windows 本机、80 并发、快速哈希链路的复测结果：

| 模型 | 吞吐 | P99 | 约 30 秒后 key 数 | Redis 内存 |
| --- | ---: | ---: | ---: | ---: |
| 旧 5 命令/3 lookup key | 约 **13.07k/s** | 9.09 ms | 约 120 万 | 约 456 MiB |
| 新 3 命令/直接定位 | **16.05k~16.37k/s** | 7.58~7.69 ms | 约 53 万 | 约 348 MiB |

按保守基线计算，吞吐提升约 23%，P99 下降约 16%。功能回归同时确认：篡改 token 返回
401、旧 token 仍可鉴权、`single_session=true` 会撤销旧会话、refresh token 并发重放仍然
只有一个请求成功。连续 1000 次 refresh 约 `507.6/s`，重放结果为 `1 个 200 + 15 个 401`。

## 独立压测机执行方式

本机同时运行服务端和压测客户端会争用 CPU、内存带宽和临时端口。最终容量测试应在另一台
机器执行，`BaseUrl` 指向服务器内网地址。若 `/metrics` 只允许服务器本机访问，可跳过抓取：

```powershell
.\scripts\load-test-http.ps1 `
  -BaseUrl http://<服务器内网IP>:8080 `
  -Path /health -Connections 256 -DurationSeconds 30 `
  -SkipMetrics
```

如果压测机能通过监控内网访问单独的 metrics 地址：

```powershell
.\scripts\load-test-http.ps1 `
  -BaseUrl http://<业务地址>:8080 `
  -MetricsBaseUrl http://<监控内网地址>:8080 `
  -Path /health -Connections 256 -DurationSeconds 30
```

不要直接对正式公网执行过载测试；先在预发布环境验证，再逐级提高并发。

## 2026-09-30 稳定性收尾结果

- 只针对 `fast_`、`perf_`、`r_`、`load_` 测试账号对应的 player_id 清理 Redis Session。
- dry-run 识别 `16260` 个测试账号、`1878822` 个测试 Session。
- 实际异步释放 `5337446` 个 Session/lookup/set key，未删除 MySQL 账号、玩家或业务数据。
- Redis 从约 `534 万 key / 1.89GB` 降到约 `1733 key / 1.64MB`。
- 清理期间暂停自动 RDB，完成后主动 BGSAVE 成功，并恢复原快照规则。
- 本地 Redis 启动脚本默认设置 `maxmemory=2GB`、`maxmemory-policy=volatile-ttl`。
- `storage-health-report.ps1` 可汇总应用连接池指标、Redis 内存和 MySQL 慢查询配置。
- 已生成并校验一次 MySQL + Redis 备份：`backups/20260930_180823`。
- 生产环境现在强制拒绝 PERF_MODE、快速密码哈希和过低 Argon2 参数。

安全清理脚本默认只做 dry-run：

```powershell
python .\scripts\cleanup-redis-test-sessions.py `
  --players-file .\build\perf-test-players.tsv `
  --port 6379
```

只有确认清单后才添加 `--execute`。不要把普通玩家 player_id 放入该文件。
