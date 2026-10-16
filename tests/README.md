# 后端自动化测试

面向游戏运营管理后台与 C++ 服务端的项目级自动化测试框架。pytest 负责任务发现、fixture 和报告；`framework/` 负责统一 HTTP 调用、JSON 契约断言、鉴权客户端和测试数据模型。

## 快速运行

```bash
python -m pip install -r tests/requirements.txt
python -m pytest -q
```

默认在当前操作系统可执行的候选路径中选择修改时间最新的 `hello_service`（Windows不选择Linux ELF）：

1. `build/RelWithDebInfo/hello_service.exe`
2. `build/Debug/hello_service.exe`
3. `build-otel-off/Debug/hello_service.exe`
4. `build-linux-local/hello_service`

也可以显式指定：

```powershell
$env:HELLO_SERVICE_TEST_EXE='D:\code\model\build\RelWithDebInfo\hello_service.exe'
python -m pytest -q
```

测试会自动选择空闲端口，以 `in_memory + development` 模式启动独立服务，结束后关闭进程，不连接开发数据库。

## 分层运行

```bash
python -m pytest -m smoke -q
python -m pytest -m auth -q
python -m pytest -m admin -q
python -m pytest -m business -q
python -m pytest -m ui -q
python -m pytest tests/e2e -m browser -q
```

默认命令排除 `browser` 和 `integration`，保持快速回归稳定。浏览器套件使用本机 Edge/Chrome，不下载额外浏览器；可通过 `HELLO_SERVICE_TEST_BROWSER_EXECUTABLE` 指定路径。

## 测试报告

```bash
python -m pytest \
  --html=tests/.artifacts/report.html --self-contained-html \
  --junitxml=tests/.artifacts/junit.xml
```

Windows PowerShell 可以把换行符替换为反引号，或直接写成一行执行。运行日志和报告统一写入 `tests/.artifacts/`，该目录不会提交到 Git。

## 持续集成

`.github/workflows/backend-tests.yml` 在 push、pull request 或手动触发时执行。使用 Ubuntu 24.04、Python 3.12、固定版本及 SHA256 的 Boost 1.91.0，关闭可选 OpenTelemetry 后构建服务与网关。浏览器使用 Playwright 下载的 Chromium，通过 `HELLO_SERVICE_TEST_BROWSER_EXECUTABLE` 显式指定。

CI 启动运行器本机 MySQL/Redis，再调用隔离脚本创建临时测试库、账号和独立 Redis DB，不需要云服务器 SSH 凭据。PyMySQL 安装 RSA 扩展以支持 MySQL 8 默认认证。三层测试都会生成 HTML/JUnit，浏览器用例另存截图；结果上传到 `backend-test-reports`，保留14天，最终检查禁止将跳过用例当作通过。工作流需提交到 GitHub 后才能在 Actions 页面实际运行。上报平台后，`report_to_platform.py` 会自动把失败用例的截图（`tests/.artifacts/screenshots/<用例名>.png`）关联到具体失败用例，并把 `service.log` 作为运行级证据上传；`regression_check.py` 对比最近两次运行输出「新增失败 / 仍失败 / 已修复」，有失败时让 CI 步骤变红触发通知。工作流每天 00:00（北京时间）定时跑一次，持续积累回归数据。

## 框架结构

- `framework/api_client.py`：封装 `requests.Session`、Bearer Token、超时、状态码检查和统一 `{ok,data,error}` 响应契约。
- `framework/models.py`：封装测试用户的账号、玩家、会话与已鉴权客户端。
- `framework/concurrency.py`：独立HTTP会话、屏障同步起跑的有界并发请求辅助器，当前功能验证使用2/8个工作线程。
- `conftest.py`：管理服务进程、动态端口、管理员登录和唯一测试用户工厂。
- `test_system.py`：健康检查、配置和 Prometheus 指标。
- `test_auth.py`：注册、登录、重名、错误密码、改密、刷新 Token、会话和玩家资料。
- `test_admin.py`：管理页面、管理员身份、越权、玩家/背包、分页查询和审计日志。
- `test_*.py`：聊天、好友、邮件、奖励、任务、商城、抽卡和掉落业务闭环。
- `e2e/test_admin_ui_e2e.py`：Playwright真实浏览器登录、玩家/背包查询、发邮件和审计查询。
- `integration/test_mysql_redis_consistency.py`：接口结果与MySQL事实、Redis Session索引交叉校验。
- `test_security_boundaries.py`：跨用户邮件操作、管理员接口权限、奖励数值边界及拒绝后资产不变。
- `integration/test_asset_concurrency.py`：8并发邮件领取、同ID发奖、余额/限购竞争；在临时库定向注入发奖失败并验证购买全量回滚与恢复重试。
- `integration/test_websocket_chat.py`：真实网关连接、收发顺序、落库、重连及Token撤销验证。
- `../scripts/linux/run-isolated-integration-tests.sh`：在Linux主机用临时资源自动运行集成套件并清理，详见 `integration/README.md`。
- `../.github/workflows/backend-tests.yml`：GitHub临时运行器自动构建、运行快速/浏览器/独立MySQL与Redis集成测试并上传报告；不使用生产服务器与生产凭据。

## 当前边界

2026-10-15最新验证：快速38项、真实浏览器4项、云端隔离集成19项通过，共61项；不是61个不同接口。详细报告见 `INTEGRATION_REPORT.md`。
简历表述、面试展示依据与结果边界见 `RESUME_EVIDENCE.md`。

- 默认套件是进程级接口与业务集成测试，不依赖 MySQL、Redis、LiveKit 或外部聊天网关。
- `ui` 包含HTTP页面契约检查；`browser` 是独立执行的真实浏览器交互测试。
- MySQL/Redis 数据一致性测试已经实现，但必须连接显式指定的测试环境，配置见 `integration/README.md`。
- WebSocket集成测试与性能测试在独立环境运行，避免把外部依赖引入快速回归套件。
- 详细范围与后续计划见 `TEST_PLAN.md`。
- 离线消息领取使用Redis Lua原子出队；领取不清除未读，显式 `/chat/read` 或加入频道才确认已读。队列上限200条，无客户端ACK时不保证响应丢失后的重投。
