#include <boost/json/src.hpp>
#include <boost/json/value_to.hpp>

// value_to is a header template, but some Linux/Boost combinations do not
// emit the std::string specialization from the consuming translation units.
// Emit the specialization once in the JSON implementation unit.
template std::string boost::json::value_to<std::string>(
    boost::json::value const&);

// 文件职责：
// 提供 Boost.JSON 的单独编译单元，避免只靠头文件模式导致链接问题。
// 调用流程：
// CMake 把本文件编进 model_framework ->
// 其余所有使用 Boost.JSON 的模块共享这里产出的实现符号。
