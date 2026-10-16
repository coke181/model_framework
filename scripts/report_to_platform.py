#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把多个 JUnit XML 合并后上报到测试平台 /api/import。

CI 用法：
    TEST_PLATFORM_URL=https://example.com python scripts/report_to_platform.py \
        --project model --name backend-ci \
        tests/.artifacts/fast.xml tests/.artifacts/browser.xml tests/.artifacts/integration-*.xml

如果 TEST_PLATFORM_URL 未配置，则跳过上报并以 0 退出，不阻塞 CI。
"""
import argparse
import glob
import os
import sys
import urllib.parse
import urllib.request
import uuid
import xml.etree.ElementTree as ET


def merge_junit(paths):
    root = ET.Element("testsuites")
    count = 0
    for path in paths:
        try:
            tree = ET.parse(path)
        except (OSError, ET.ParseError) as exc:
            print("[WARN] skip %s: %s" % (path, exc))
            continue
        node = tree.getroot()
        if node.tag == "testsuites":
            for child in list(node):
                root.append(child)
                count += 1
        elif node.tag == "testsuite":
            root.append(node)
            count += 1
        else:
            print("[WARN] skip non-JUnit file: %s" % path)
    if count == 0:
        return None
    return root


def main():
    parser = argparse.ArgumentParser(description="merge JUnit XML and report to test platform")
    parser.add_argument("files", nargs="+", help="JUnit XML 文件或 glob 表达式，可传多个")
    parser.add_argument("--api", default=os.environ.get("TEST_PLATFORM_URL", ""))
    parser.add_argument("--token", default=os.environ.get("TEST_PLATFORM_TOKEN", ""))
    parser.add_argument("--project", default="model")
    parser.add_argument("--name", default="backend-ci")
    args = parser.parse_args()

    if not args.api:
        print("[SKIP] TEST_PLATFORM_URL not set; skip reporting to platform.")
        return 0

    paths = []
    for pattern in args.files:
        matched = glob.glob(pattern)
        if matched:
            paths.extend(matched)
        elif os.path.isfile(pattern):
            paths.append(pattern)

    merged = merge_junit(paths)
    if merged is None:
        print("[SKIP] no JUnit report found; nothing to report.")
        return 0

    xml_text = ET.tostring(merged, encoding="unicode")
    boundary = "----codex" + uuid.uuid4().hex
    body = b""
    for key, value in (("project", args.project), ("name", args.name), ("xml", xml_text)):
        body += ("--%s\r\n" % boundary).encode("utf-8")
        body += ("Content-Disposition: form-data; name=\"%s\"\r\n\r\n" % key).encode("utf-8")
        body += value.encode("utf-8") + b"\r\n"
    body += ("--%s--\r\n" % boundary).encode("utf-8")
    headers = {"Content-Type": "multipart/form-data; boundary=" + boundary}
    if args.token:
        headers["Authorization"] = "Bearer " + args.token
    request = urllib.request.Request(args.api.rstrip("/") + "/api/import", data=body, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=30) as resp:
            print("reported:", resp.read().decode("utf-8"))
    except Exception as exc:
        print("[ERROR] report failed:", exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())