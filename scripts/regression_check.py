#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""回归对比与告警：拉取平台最近两次运行，输出对比摘要。

用法：
    python scripts/regression_check.py --project model

返回码：
    0 = 最新运行无失败 / 错误（回归通过）
    1 = 最新运行存在失败或错误（用于让 CI 步骤变红，触发通知）

对比维度以 classname::name 作为用例主键，分类：
    新增失败 / 仍失败 / 已修复 / 已移除
"""
import argparse
import json
import os
import sys
import urllib.parse
import urllib.request


def fetch(url, token=""):
    headers = {}
    if token:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request, timeout=20) as resp:
        return json.loads(resp.read().decode("utf-8"))


def case_key(case):
    classname = (case.get("classname") or "").strip()
    name = (case.get("name") or "").strip()
    return (classname + "::" + name) if classname else name


def failed_set(cases):
    return {case_key(c) for c in cases if c.get("status") in ("failed", "error")}


def summarize_run(run):
    return "name=%s run_id=%s total=%s passed=%s failed=%s errors=%s skipped=%s" % (
        run.get("name"), run.get("id"), run.get("total"), run.get("passed"),
        run.get("failed"), run.get("errors"), run.get("skipped"),
    )


def print_lines(title, items):
    print("%s: %d" % (title, len(items)))
    for item in items:
        print("  " + item)


def main():
    parser = argparse.ArgumentParser(description="regression compare and alert against test platform")
    parser.add_argument("--project", default="model")
    parser.add_argument("--api", default=os.environ.get("TEST_PLATFORM_URL", ""))
    parser.add_argument("--token", default=os.environ.get("TEST_PLATFORM_TOKEN", ""))
    args = parser.parse_args()
    api = (args.api or "").strip().rstrip("/")
    token = (args.token or "").strip().lstrip("\ufeff")

    if not api:
        print("[SKIP] TEST_PLATFORM_URL not set; skip regression check.")
        return 0

    runs = fetch(api + "/api/runs?project=" + urllib.parse.quote(args.project), token)
    if not runs:
        print("[SKIP] no runs found for project %s" % args.project)
        return 0

    latest = runs[0]
    latest_detail = fetch(api + "/api/runs/%d" % latest["id"], token)
    latest_failed = failed_set(latest_detail.get("cases", []))
    print("最新运行:", summarize_run(latest))

    if len(runs) >= 2:
        previous = runs[1]
        previous_detail = fetch(api + "/api/runs/%d" % previous["id"], token)
        previous_failed = failed_set(previous_detail.get("cases", []))
        print("对比基准:", summarize_run(previous))
        print_lines("新增失败", sorted(latest_failed - previous_failed))
        print_lines("仍失败", sorted(latest_failed & previous_failed))
        print_lines("已修复", sorted(previous_failed - latest_failed))
    else:
        print("只有一次运行，无法对比；当前失败用例：")
        print_lines("失败 / 错误", sorted(latest_failed))

    if latest_failed:
        print("::error::项目 %s 存在 %d 个失败/错误用例" % (args.project, len(latest_failed)))
        return 1

    print("回归通过：最新运行无失败/错误。")
    return 0


if __name__ == "__main__":
    sys.exit(main())