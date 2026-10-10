"""对本机 hello_service 做真实 MySQL + Redis 登录/注册压测。"""

import argparse
import os
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import pymysql
import requests


PASSWORD = os.getenv("PERF_TEST_PASSWORD", "change_me")
local = threading.local()


def session():
    if not hasattr(local, "session"):
        local.session = requests.Session()
    return local.session


def percentile(values, percent):
    values = sorted(values)
    index = min(len(values) - 1, int((len(values) - 1) * percent / 100))
    return values[index]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("login", "register"), default="login")
    parser.add_argument("--ports", required=True, help="comma-separated service ports")
    parser.add_argument("--concurrency", type=int, default=16)
    parser.add_argument("--limit", type=int, default=800)
    parser.add_argument("--login-prefix", default="perf_auth_")
    parser.add_argument("--password", default=PASSWORD)
    parser.add_argument("--display-prefix", default="Perf")
    parser.add_argument("--mysql-host", default=os.getenv("PERF_MYSQL_HOST", "127.0.0.1"))
    parser.add_argument("--mysql-port", type=int, default=int(os.getenv("PERF_MYSQL_PORT", "3306")))
    parser.add_argument("--mysql-user", default=os.getenv("PERF_MYSQL_USER", "game_backend"))
    parser.add_argument("--mysql-password", default=os.getenv("PERF_MYSQL_PASSWORD", "change_me"))
    parser.add_argument("--mysql-database", default=os.getenv("PERF_MYSQL_DATABASE", "game_backend"))
    args = parser.parse_args()
    bases = [f"http://127.0.0.1:{port}" for port in args.ports.split(",")]

    mysql = {
        "host": args.mysql_host,
        "port": args.mysql_port,
        "user": args.mysql_user,
        "password": args.mysql_password,
        "database": args.mysql_database,
        "autocommit": True,
    }

    if args.mode == "login":
        connection = pymysql.connect(**mysql)
        try:
            with connection.cursor() as cursor:
                cursor.execute(
                    "SELECT login_name FROM account "
                    "WHERE login_name LIKE %s ORDER BY created_at DESC LIMIT %s",
                    (args.login_prefix + "%", args.limit),
                )
                accounts = [row[0] for row in cursor.fetchall()]
        finally:
            connection.close()

        if len(accounts) < args.limit:
            raise SystemExit(
                f"need {args.limit} accounts with prefix {args.login_prefix!r}; found {len(accounts)}")
    else:
        # 每轮注册使用新的时间后缀，避免和上一轮测试账号发生唯一键冲突。
        batch_prefix = f"{args.login_prefix}{time.strftime('%Y%m%d%H%M%S')}_"
        # 后端登录名上限是 32 个字符。提前检查，避免压测跑完后才发现
        # 所有请求都因为参数校验返回 400，被误认为注册吞吐量。
        longest_login_name = f"{batch_prefix}{args.limit - 1:06d}"
        if len(longest_login_name) > 32:
            raise SystemExit(
                "register login prefix is too long: "
                f"the generated login name would be {len(longest_login_name)} characters; "
                "use a shorter --login-prefix (the backend limit is 32)")
        accounts = [f"{batch_prefix}{index:06d}" for index in range(args.limit)]

    def request_one(index, login_name):
        endpoint = "/auth/login" if args.mode == "login" else "/auth/register"
        payload = {"login_name": login_name, "password": args.password}
        if args.mode == "register":
            payload["display_name"] = f"{args.display_prefix}{index:06d}"
        url = bases[index % len(bases)] + endpoint
        started = time.perf_counter()
        try:
            response = session().post(
                url,
                json=payload,
                timeout=30,
            )
            return response.status_code, (time.perf_counter() - started) * 1000
        except Exception as error:
            return -1, (time.perf_counter() - started) * 1000, str(error)

    started = time.perf_counter()
    results = []
    with ThreadPoolExecutor(max_workers=args.concurrency) as executor:
        futures = [executor.submit(request_one, index, name) for index, name in enumerate(accounts)]
        for future in as_completed(futures):
            results.append(future.result())
    elapsed = time.perf_counter() - started
    latencies = [result[1] for result in results]
    statuses = {}
    for result in results:
        statuses[result[0]] = statuses.get(result[0], 0) + 1

    print(
        f"mode={args.mode} instances={len(bases)} "
        f"concurrency={args.concurrency} requests={len(results)}")
    print(f"elapsed={elapsed:.3f}s throughput={len(results) / elapsed:.1f}/s status={statuses}")
    print(
        "latency_ms "
        f"p50={percentile(latencies, 50):.1f} "
        f"p95={percentile(latencies, 95):.1f} "
        f"p99={percentile(latencies, 99):.1f} "
        f"max={max(latencies):.1f}"
    )
    errors = [result[2] for result in results if len(result) > 2]
    if errors:
        print(f"sample_error={errors[0]}")


if __name__ == "__main__":
    main()
