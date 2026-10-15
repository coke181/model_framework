# 简历项目表述与验证依据

校验日期：2026-10-15。个人项目：游戏运营管理系统（已有Web管理页面与C++服务端），独立开发与测试。以下结果来自当前本地工作区构建和隔离测试，不是生产容量结论。

## 可用于简历的项目表述

- 基于pytest封装鉴权客户端、响应契约、数据工厂和并发辅助器；完成38项接口/业务、4项Playwright浏览器、19项云端集成回归，共61项通过，输出HTML/JUnit报告。
- 以8并发竞争领取邮件、同ID发奖及购买商品，交叉核验接口响应和MySQL余额、背包、奖励/购买流水，验证重复请求不重复增资、余额不足及限购拦截。
- 在临时库定向阻断发奖写入，复现失败购买仍扣费、成功响应却未发奖；定位事务遗漏和INSERT IGNORE忽略约束错误，修复后失败操作全量回滚，恢复后重试成功。
- 验证WebSocket登出/自然过期后的广播拒绝、离线分页和2/8并发领取；修复拉取通知提前清空未读，核验90条真实消息集合无重复或缺失。

## 面试时可展示的依据

| 结论 | 实现或证据 |
| --- | --- |
| 38项快速回归 | `.artifacts/fast-20261015-concurrency.{xml,html}`，38通过，无跳过 |
| 4项真实浏览器 | `e2e/test_admin_ui_e2e.py`；`.artifacts/browser-20261015-concurrency.{xml,html}` |
| 19项云端集成 | `.artifacts/integration-20261015174411_69547a.{xml,html}`，19通过，无跳过 |
| 8并发资产/流水 | `integration/test_asset_concurrency.py`：HTTP结果之外直接查询真实MySQL事实 |
| 故障前后对照 | `DEFECTS.md` DEF-006；修复前余额71变64，修复后失败保持71，恢复重试后64 |
| 离线消息语义 | `integration/test_websocket_chat.py`：领取不代表阅读，显式已读才清除计数 |
| 隔离与复跑 | `../scripts/linux/run-isolated-integration-tests.sh`；`integration/README.md` |

原始报告是本地生成物，默认不提交Git。完整环境、命令、清理结果与边界见 `INTEGRATION_REPORT.md`。GitHub Actions只有配置和构建/脚本校验，尚未实际在GitHub运行；不要写“CI已稳定运行”。61是测试项数，包含参数化场景，不是61个接口。

本轮被测Linux二进制SHA256（提交源码后应再关联Git revision）：

- `hello_service`: `DD7C3C2A87D99E05CFCB0A2C39428C397DBD55ECB824518785C5B555E90C341B`
- `hello_chat_gateway`: `20AAAE3F84A2975B7A8EFB779F67705CAC3BC8DA23B01372D2BE95FBB34C6EC0`

## 不应扩大表述的范围

- 并发功能通过不等于高并发容量验证，不能由8并发推导QPS或生产稳定性。
- 离线领取是原子出队，无客户端ACK，不能写“端到端恰好一次”或“弱网绝不丢消息”。
- 本轮没有重新执行UE测试。86/86 UE Automation与28/28原生单测是此前核对过的验证记录，应与本轮后端61项分开。
- 面试时需要能解释并演示相关脚本、断言及根因，不能仅背诵工具名和数字。
