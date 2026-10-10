#include <boost/redis/src.hpp>

// 文件职责：
// 提供 Boost.Redis 的单独编译单元。
// 调用流程：
// CMake 把本文件编进框架库 ->
// Redis 仓储实现共享这里产出的 Boost.Redis 符号。
