"""测试多个不同账号并发恢复会话的吞吐，不把首次密码登录计入 refresh 结果。"""

import argparse
import os
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

import pymysql
import requests


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
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--login-prefix", default="perf_auth_")
    parser.add_argument("--password", default=os.getenv("PERF_TEST_PASSWORD", "change_me"))
    parser.add_argument("--accounts", type=int, default=800)
    parser.add_argument("--login-concurrency", type=int, default=20)
    parser.add_argument("--refresh-concurrency", type=int, default=80)
    parser.add_argument("--mysql-host", default=os.getenv("PERF_MYSQL_HOST", "127.0.0.1"))
    parser.add_argument("--mysql-port", type=int, default=int(os.getenv("PERF_MYSQL_PORT", "3306")))
    parser.add_argument("--mysql-user", default=os.getenv("PERF_MYSQL_USER", "game_backend"))
    parser.add_argument("--mysql-password", default=os.getenv("PERF_MYSQL_PASSWORD", "change_me"))
    parser.add_argument("--mysql-database", default=os.getenv("PERF_MYSQL_DATABASE", "game_backend"))
    args = parser.parse_args()

    connection = pymysql.connect(
        host=args.mysql_host,
        port=args.mysql_port,
        user=args.mysql_user,
        password=args.mysql_password,
        database=args.mysql_database,
        autocommit=True,
    )
    try:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT login_name FROM account WHERE login_name LIKE %s "
                "ORDER BY created_at DESC LIMIT %s",
                (args.login_prefix + "%", args.accounts),
            )
            accounts = [row[0] for row in cursor.fetchall()]
    finally:
        connection.close()

    if len(accounts) < args.accounts:
        raise SystemExit(f"need {args.accounts} accounts; found {len(accounts)}")

    def login_one(login_name):
        response = session().post(
            args.base_url + "/auth/login",
            json={"login_name": login_name, "password": args.password},
            timeout=30,
        )
        response.raise_for_status()
        return response.json()["data"]["session"]["refresh_token"]

    # 建立测试 Session 只是准备阶段，不计入 refresh 吞吐。
    with ThreadPoolExecutor(max_workers=args.login_concurrency) as executor:
        refresh_tokens = list(executor.map(login_one, accounts))

    def refresh_one(refresh_token):
        started = time.perf_counter()
        response = session().post(
            args.base_url + "/auth/refresh",
            json={"refresh_token": refresh_token},
            timeout=30,
        )
        return response.status_code, (time.perf_counter() - started) * 1000

    started = time.perf_counter()
    results = []
    with ThreadPoolExecutor(max_workers=args.refresh_concurrency) as executor:
        futures = [executor.submit(refresh_one, token) for token in refresh_tokens]
        for future in as_completed(futures):
            results.append(future.result())
    elapsed = time.perf_counter() - started

    statuses = {}
    for status, _ in results:
        statuses[status] = statuses.get(status, 0) + 1
    latencies = [latency for _, latency in results]
    print(
        f"accounts={len(results)} concurrency={args.refresh_concurrency} "
        f"elapsed={elapsed:.3f}s throughput={len(results) / elapsed:.1f}/s status={statuses}")
    print(
        "latency_ms "
        f"p50={percentile(latencies, 50):.2f} "
        f"p95={percentile(latencies, 95):.2f} "
        f"p99={percentile(latencies, 99):.2f} "
        f"max={max(latencies):.2f}")


if __name__ == "__main__":
    main()
