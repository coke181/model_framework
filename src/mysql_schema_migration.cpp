#include "mf/storage/mysql_schema_migration.hpp"

// 文件职责：
// 实现 MySQL 启动迁移。
// 调用流程：
// 1. 先连接 MySQL 服务端，CREATE DATABASE IF NOT EXISTS。
// 2. 再连接目标数据库，获取 MySQL 命名锁，避免多个进程同时 ALTER TABLE。
// 3. 读取 db/mysql/001_game_backend.sql，并按分号拆成单条语句逐条执行。
// 4. 全部成功后释放锁，仓储层再开始使用这些表。

#include <boost/asio/io_context.hpp>
#include <boost/mysql/any_connection.hpp>
#include <boost/mysql/connect_params.hpp>
#include <boost/mysql/results.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mf::storage {
namespace mysql = boost::mysql;
namespace asio = boost::asio;

namespace {

std::string trim(std::string value) {
    const auto not_space = [](unsigned char ch) {
        return !std::isspace(ch);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

std::string quote_identifier(const std::string& identifier) {
    if (identifier.empty()) {
        throw std::runtime_error("mysql database name is empty");
    }
    std::string quoted = "`";
    for (const char ch : identifier) {
        if (ch == '`') {
            quoted += "``";
        } else {
            quoted += ch;
        }
    }
    quoted += "`";
    return quoted;
}

std::string read_file(const std::string& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        throw std::runtime_error("cannot open mysql schema file: " + path);
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// 这里不用简单 split(';')，因为 SQL 里可能有字符串字面量。
// 当前 schema 没有存储过程，所以只需要识别引号和注释即可。
std::vector<std::string> split_sql_statements(const std::string& sql) {
    std::vector<std::string> statements;
    std::string current;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool in_backtick = false;
    bool in_line_comment = false;
    bool in_block_comment = false;

    for (std::size_t i = 0; i < sql.size(); ++i) {
        const char ch = sql[i];
        const char next = (i + 1 < sql.size()) ? sql[i + 1] : '\0';

        if (in_line_comment) {
            if (ch == '\n') {
                in_line_comment = false;
                current.push_back(ch);
            }
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && next == '/') {
                in_block_comment = false;
                ++i;
            }
            continue;
        }

        if (!in_single_quote && !in_double_quote && !in_backtick) {
            if (ch == '-' && next == '-') {
                in_line_comment = true;
                ++i;
                continue;
            }
            if (ch == '#') {
                in_line_comment = true;
                continue;
            }
            if (ch == '/' && next == '*') {
                in_block_comment = true;
                ++i;
                continue;
            }
        }

        current.push_back(ch);

        if (ch == '\'' && !in_double_quote && !in_backtick) {
            const bool escaped = i > 0 && sql[i - 1] == '\\';
            if (!escaped) {
                in_single_quote = !in_single_quote;
            }
        } else if (ch == '"' && !in_single_quote && !in_backtick) {
            const bool escaped = i > 0 && sql[i - 1] == '\\';
            if (!escaped) {
                in_double_quote = !in_double_quote;
            }
        } else if (ch == '`' && !in_single_quote && !in_double_quote) {
            in_backtick = !in_backtick;
        } else if (ch == ';' && !in_single_quote && !in_double_quote && !in_backtick) {
            auto statement = trim(current.substr(0, current.size() - 1));
            if (!statement.empty()) {
                statements.push_back(std::move(statement));
            }
            current.clear();
        }
    }

    auto tail = trim(current);
    if (!tail.empty()) {
        statements.push_back(std::move(tail));
    }
    return statements;
}

mysql::connect_params make_connect_params(const MySqlOptions& options, bool include_database) {
    mysql::connect_params params;
    params.server_address.emplace_host_and_port(options.host, options.port);
    params.username = options.username;
    params.password = options.password;
    if (include_database) {
        params.database = options.database;
    }
    return params;
}

void execute_sql(mysql::any_connection& connection, const std::string& sql) {
    mysql::results result;
    connection.execute(sql, result);
}

bool acquire_migration_lock(mysql::any_connection& connection) {
    mysql::results result;
    connection.execute("SELECT GET_LOCK('mf_game_backend_schema_migration', 30)", result);
    return !result.rows().empty() && !result.rows().at(0).at(0).is_null() &&
           result.rows().at(0).at(0).as_int64() == 1;
}

void release_migration_lock(mysql::any_connection& connection) {
    mysql::results ignored;
    connection.execute("DO RELEASE_LOCK('mf_game_backend_schema_migration')", ignored);
}

}  // 匿名命名空间

void run_mysql_schema_migration_if_enabled(
    const MySqlOptions& options,
    const std::shared_ptr<mf::core::Logger>& logger) {
    if (!options.auto_migrate) {
        if (logger) {
            logger->log(mf::core::LogLevel::info, "mysql schema auto migration disabled");
        }
        return;
    }

    const auto schema_path = std::filesystem::path{options.schema_file};
    const auto schema_sql = read_file(schema_path.string());
    const auto statements = split_sql_statements(schema_sql);
    if (statements.empty()) {
        throw std::runtime_error("mysql schema file has no executable statements: " + schema_path.string());
    }

    if (logger) {
        logger->log(
            mf::core::LogLevel::info,
            "mysql schema migration starting file=" + schema_path.string() +
                " statements=" + std::to_string(statements.size()));
    }

    asio::io_context server_context{1};
    mysql::any_connection server_connection{server_context};
    server_connection.connect(make_connect_params(options, false));
    execute_sql(
        server_connection,
        "CREATE DATABASE IF NOT EXISTS " + quote_identifier(options.database) +
            " CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci");

    asio::io_context database_context{1};
    mysql::any_connection database_connection{database_context};
    database_connection.connect(make_connect_params(options, true));

    if (!acquire_migration_lock(database_connection)) {
        throw std::runtime_error("cannot acquire mysql schema migration lock");
    }

    try {
        for (const auto& statement : statements) {
            execute_sql(database_connection, statement);
        }
        release_migration_lock(database_connection);
    } catch (...) {
        release_migration_lock(database_connection);
        throw;
    }

    if (logger) {
        logger->log(
            mf::core::LogLevel::info,
            "mysql schema migration completed file=" + schema_path.string() +
                " statements=" + std::to_string(statements.size()));
    }
}

}  // namespace mf::storage 命名空间
