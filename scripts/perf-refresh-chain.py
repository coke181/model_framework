"""测试真实 refresh token 轮换链路，不重复使用已经撤销的旧 token。"""

import argparse
import time
from concurrent.futures import ThreadPoolExecutor

import requests


def percentile(values, percent):
    values = sorted(values)
    index = min(len(values) - 1, int((len(values) - 1) * percent / 100))
    return values[index]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--login-name", required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--requests", type=int, default=1000)
    parser.add_argument(
        "--replay-concurrency",
        type=int,
        default=0,
        help="结束后并发重放同一个 refresh token，验证只能有一个请求成功",
    )
    args = parser.parse_args()

    client = requests.Session()
    login_response = client.post(
        args.base_url + "/auth/login",
        json={"login_name": args.login_name, "password": args.password},
        timeout=30,
    )
    login_response.raise_for_status()
    refresh_token = login_response.json()["data"]["session"]["refresh_token"]

    latencies = []
    statuses = {}
    started = time.perf_counter()
    for _ in range(args.requests):
        request_started = time.perf_counter()
        response = client.post(
            args.base_url + "/auth/refresh",
            json={"refresh_token": refresh_token},
            timeout=30,
        )
        latencies.append((time.perf_counter() - request_started) * 1000)
        statuses[response.status_code] = statuses.get(response.status_code, 0) + 1
        if response.status_code != 200:
            raise SystemExit(
                f"refresh failed status={response.status_code} body={response.text[:500]}")
        # refresh token 每次都会轮换；下一次必须使用本次响应的新 token。
        refresh_token = response.json()["data"]["refresh_token"]

    elapsed = time.perf_counter() - started
    print(f"requests={args.requests} elapsed={elapsed:.3f}s throughput={args.requests / elapsed:.1f}/s")
    print(f"status={statuses}")
    print(
        "latency_ms "
        f"p50={percentile(latencies, 50):.2f} "
        f"p95={percentile(latencies, 95):.2f} "
        f"p99={percentile(latencies, 99):.2f} "
        f"max={max(latencies):.2f}")

    if args.replay_concurrency > 0:
        def replay_once(_):
            response = requests.post(
                args.base_url + "/auth/refresh",
                json={"refresh_token": refresh_token},
                timeout=30,
            )
            return response.status_code

        with ThreadPoolExecutor(max_workers=args.replay_concurrency) as executor:
            replay_statuses = list(executor.map(replay_once, range(args.replay_concurrency)))
        replay_counts = {}
        for status in replay_statuses:
            replay_counts[status] = replay_counts.get(status, 0) + 1
        print(f"replay_status={replay_counts}")
        if replay_counts.get(200, 0) != 1 or replay_counts.get(401, 0) != args.replay_concurrency - 1:
            raise SystemExit("refresh replay protection failed: expected one 200 and all remaining 401")


if __name__ == "__main__":
    main()
