#include "mf/storage/review.hpp"

// 文件职责：
// 提供一段 review 导向的存储架构说明文本。
// 调用流程：
// 开发者阅读源码、文档或未来调试接口需要说明存储职责时 ->
// 调用 mysql_redis_review_notes() 获取集中定义的架构说明。

namespace mf::storage {

std::string_view mysql_redis_review_notes() {
    return
        "MySQL 负责账号、密码哈希、重置口令、账号状态、玩家资料、好友关系、聊天事实持久化；"
        "Redis 负责 Session、会话索引、验证码、登录限流、黑名单、聊天最近窗口缓存、热点缓存、频道在线状态。";
}

}  // namespace mf::storage 命名空间
