cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(model_framework VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MF_ENABLE_OTEL "Enable OpenTelemetry exporters" ON)
option(MF_ENABLE_TRACY "Enable Tracy CPU profiling" OFF)

if(WIN32)
    set(MF_BOOST_ROOT "D:/boost_1_91_0" CACHE PATH "Local Boost root directory")
    set(MF_OPENSSL_ROOT "D:/OpenSSL-Win64" CACHE PATH "Local OpenSSL root directory")
    set(MF_VCPKG_ROOT "D:/vcpkg" CACHE PATH "Local vcpkg root directory")
else()
    set(MF_BOOST_ROOT "" CACHE PATH "Optional Boost source root directory")
    set(MF_OPENSSL_ROOT "" CACHE PATH "Optional OpenSSL root directory")
    set(MF_VCPKG_ROOT "" CACHE PATH "Optional vcpkg root directory")
endif()

if(MF_VCPKG_ROOT)
    if(WIN32 AND EXISTS "${MF_VCPKG_ROOT}/installed/x64-windows/share/opentelemetry-cpp/opentelemetry-cpp-config.cmake")
        list(PREPEND CMAKE_PREFIX_PATH "${MF_VCPKG_ROOT}/installed/x64-windows")
    elseif(UNIX AND EXISTS "${MF_VCPKG_ROOT}/installed/x64-linux/share/opentelemetry-cpp/opentelemetry-cpp-config.cmake")
        list(PREPEND CMAKE_PREFIX_PATH "${MF_VCPKG_ROOT}/installed/x64-linux")
    endif()
endif()

if(MF_BOOST_ROOT)
    if(NOT EXISTS "${MF_BOOST_ROOT}/boost/asio.hpp")
        message(FATAL_ERROR "Boost headers not found under MF_BOOST_ROOT=${MF_BOOST_ROOT}")
    endif()
else()
    find_package(Boost REQUIRED)
endif()

if(MF_OPENSSL_ROOT)
    if(NOT EXISTS "${MF_OPENSSL_ROOT}/include/openssl/conf.h")
        message(FATAL_ERROR "OpenSSL headers not found under MF_OPENSSL_ROOT=${MF_OPENSSL_ROOT}")
    endif()
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
if(MF_ENABLE_OTEL)
    find_package(opentelemetry-cpp CONFIG REQUIRED COMPONENTS api sdk ext_http_curl exporters_otlp_http)
endif()
if(MF_ENABLE_TRACY)
    find_package(Tracy CONFIG REQUIRED
        HINTS "${MF_VCPKG_ROOT}/installed/x64-windows" "${MF_VCPKG_ROOT}/installed/x64-linux")
endif()

set(MF_BOOST_CHARCONV_SOURCES)
if(MF_BOOST_ROOT AND EXISTS "${MF_BOOST_ROOT}/libs/charconv/src/from_chars.cpp")
    list(APPEND MF_BOOST_CHARCONV_SOURCES
        "${MF_BOOST_ROOT}/libs/charconv/src/from_chars.cpp"
        "${MF_BOOST_ROOT}/libs/charconv/src/to_chars.cpp")
endif()

add_library(model_framework
    ${MF_BOOST_CHARCONV_SOURCES}
    src/agent_model_provider.cpp
    src/agent_service.cpp
    src/agent_task_service.cpp
    src/agent_tool_registry.cpp
    src/agent_types.cpp
    src/api.cpp
    src/account_service.cpp
    src/application.cpp
    src/auth.cpp
    src/boost_json.cpp
    src/boost_mysql.cpp
    src/boost_redis.cpp
    src/chat_channels.cpp
    src/chat_gateway.cpp
    src/chat_state.cpp
    src/chat_service.cpp
    src/cached_repositories.cpp
    src/env.cpp
    src/gacha_service.cpp
    src/game_json.cpp
    src/game_services.cpp
    src/in_memory_repositories.cpp
    src/logger.cpp
    src/loot_service.cpp
    src/multiplayer_service.cpp
    src/quest_service.cpp
    src/voice_service.cpp
    src/voice_types.cpp
    src/livekit_token_provider.cpp
    src/mail_service.cpp
    src/metrics.cpp
    src/mysql_schema_migration.cpp
    src/middleware.cpp
    src/mysql_repositories.cpp
    src/password_hasher.cpp
    src/query_params.cpp
    src/redis_repositories.cpp
    src/shop_service.cpp
    src/storage_options.cpp
    src/storage_factory.cpp
    src/storage_review.cpp
    src/tracing.cpp
    src/router.cpp
    src/server.cpp
)

target_include_directories(model_framework
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

if(MF_BOOST_ROOT)
    target_include_directories(model_framework PUBLIC ${MF_BOOST_ROOT})
endif()

if(MF_OPENSSL_ROOT)
    target_include_directories(model_framework PUBLIC ${MF_OPENSSL_ROOT}/include)
endif()

if(WIN32 AND MF_OPENSSL_ROOT)
    target_link_directories(model_framework
        PUBLIC
            ${MF_OPENSSL_ROOT}/lib/VC/x64/MD
    )
endif()

target_link_libraries(model_framework
    PUBLIC
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)

if(MF_ENABLE_OTEL)
    target_link_libraries(model_framework
        PUBLIC
            opentelemetry-cpp::api
            opentelemetry-cpp::sdk
            opentelemetry-cpp::http_client_curl
            opentelemetry-cpp::otlp_http_exporter
            opentelemetry-cpp::otlp_http_metric_exporter
    )
endif()

if(MF_ENABLE_TRACY)
    target_link_libraries(model_framework PUBLIC Tracy::TracyClient)
endif()

target_compile_definitions(model_framework
    PUBLIC
        BOOST_ALL_NO_LIB
        BOOST_ERROR_CODE_HEADER_ONLY
        BOOST_MYSQL_SEPARATE_COMPILATION
        $<$<BOOL:${MF_ENABLE_OTEL}>:MF_ENABLE_OTEL=1>
        $<$<NOT:$<BOOL:${MF_ENABLE_OTEL}>>:MF_ENABLE_OTEL=0>
        $<$<BOOL:${MF_ENABLE_TRACY}>:MF_ENABLE_TRACY=1>
        $<$<NOT:$<BOOL:${MF_ENABLE_TRACY}>>:MF_ENABLE_TRACY=0>
        $<$<BOOL:${MF_ENABLE_TRACY}>:TRACY_ENABLE=1>
)

if(WIN32)
    target_compile_definitions(model_framework
        PUBLIC
            NOMINMAX
            WIN32_LEAN_AND_MEAN
            _WIN32_WINNT=0x0A00
    )
endif()

target_compile_options(model_framework
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic>
)

add_executable(hello_service
    examples/hello/main.cpp
    examples/hello/hello_agent_routes.cpp
    examples/hello/hello_admin_tasks.cpp
    examples/hello/hello_auth_routes.cpp
    examples/hello/hello_admin_routes.cpp
    examples/hello/hello_admin_more_routes.cpp
    examples/hello/hello_admin_task_routes.cpp
    examples/hello/hello_player_routes.cpp
    examples/hello/hello_reward_routes.cpp
    examples/hello/hello_gacha_routes.cpp
    examples/hello/hello_shop_routes.cpp
    examples/hello/hello_loot_routes.cpp
    examples/hello/hello_multiplayer_routes.cpp
    examples/hello/hello_quest_routes.cpp
    examples/hello/hello_voice_routes.cpp
    examples/hello/hello_route_support.cpp
    examples/hello/hello_routes.cpp
)

target_link_libraries(hello_service
    PRIVATE
        model_framework
)

add_executable(hello_task_worker
    examples/hello/task_worker_main.cpp
    examples/hello/hello_admin_tasks.cpp
    examples/hello/hello_route_support.cpp
)

target_link_libraries(hello_task_worker
    PRIVATE
        model_framework
)

add_executable(hello_chat_gateway
    examples/hello/chat_gateway_main.cpp
)

target_link_libraries(hello_chat_gateway
    PRIVATE
        model_framework
)

if(WIN32)
    set_target_properties(hello_service PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        VS_DEBUGGER_ENVIRONMENT
            "PATH=${MF_OPENSSL_ROOT}/bin;${MF_VCPKG_ROOT}/installed/x64-windows/debug/bin;${MF_VCPKG_ROOT}/installed/x64-windows/bin;%PATH%\nOTEL_EXPORTER_OTLP_ENDPOINT=http://127.0.0.1:4318\nOTEL_EXPORTER_OTLP_TRACES_ENDPOINT=http://127.0.0.1:4318/v1/traces\nOTEL_EXPORTER_OTLP_METRICS_ENDPOINT=http://127.0.0.1:4318/v1/metrics"
    )

    set_target_properties(hello_task_worker PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        VS_DEBUGGER_ENVIRONMENT
            "PATH=${MF_OPENSSL_ROOT}/bin;${MF_VCPKG_ROOT}/installed/x64-windows/debug/bin;${MF_VCPKG_ROOT}/installed/x64-windows/bin;%PATH%\nOTEL_EXPORTER_OTLP_ENDPOINT=http://127.0.0.1:4318\nOTEL_EXPORTER_OTLP_TRACES_ENDPOINT=http://127.0.0.1:4318/v1/traces\nOTEL_EXPORTER_OTLP_METRICS_ENDPOINT=http://127.0.0.1:4318/v1/metrics"
    )

    set_target_properties(hello_chat_gateway PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        VS_DEBUGGER_ENVIRONMENT
            "PATH=${MF_OPENSSL_ROOT}/bin;${MF_VCPKG_ROOT}/installed/x64-windows/debug/bin;${MF_VCPKG_ROOT}/installed/x64-windows/bin;%PATH%\nOTEL_EXPORTER_OTLP_ENDPOINT=http://127.0.0.1:4318\nOTEL_EXPORTER_OTLP_TRACES_ENDPOINT=http://127.0.0.1:4318/v1/traces\nOTEL_EXPORTER_OTLP_METRICS_ENDPOINT=http://127.0.0.1:4318/v1/metrics"
    )
endif()
