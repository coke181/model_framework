#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把多个 JUnit XML 合并后上报到测试平台 /api/import，并自动上传失败取证。

CI 用法：
    TEST_PLATFORM_URL=https://example.com python scripts/report_to_platform.py \
        --project model --name backend-ci \
        --evidence-dir tests/.artifacts \
        tests/.artifacts/fast.xml tests/.artifacts/browser.xml tests/.artifacts/integration-*.xml

上报成功后：
1. 查询平台本次 run 的失败 / 错误用例；
2. 在 evidence-dir/screenshots/<用例名>.png 中匹配截图并关联到具体用例；
3. 把 evidence-dir/service.log 作为运行级证据上传。

证据上传失败只打印告警，不阻塞 CI；如果 TEST_PLATFORM_URL 未配置则跳过上报。
"""
import argparse
import glob
import json
import mimetypes
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


def _multipart_body(fields, filename, content, content_type):
    boundary = "----codex" + uuid.uuid4().hex
    body = b""
    for key, value in fields.items():
        body += ("--%s\r\n" % boundary).encode("utf-8")
        body += ("Content-Disposition: form-data; name=\"%s\"\r\n\r\n" % key).encode("utf-8")
        body += ("%s\r\n" % value).encode("utf-8")
    body += ("--%s\r\n" % boundary).encode("utf-8")
    body += ("Content-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n" % filename).encode("utf-8")
    body += ("Content-Type: %s\r\n\r\n" % content_type).encode("utf-8")
    body += content + b"\r\n"
    body += ("--%s--\r\n" % boundary).encode("utf-8")
    return body, boundary


def upload_attachment(api, run_id, case_id, path, token=""):
    filename = os.path.basename(path)
    content_type = mimetypes.guess_type(path)[0] or "application/octet-stream"
    with open(path, "rb") as f:
        content = f.read()
    fields = {}
    if run_id is not None:
        fields["run_id"] = str(run_id)
    if case_id is not None:
        fields["case_id"] = str(case_id)
    body, boundary = _multipart_body(fields, filename, content, content_type)
    headers = {"Content-Type": "multipart/form-data; boundary=" + boundary}
    if token:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(
        api.rstrip("/") + "/api/attachments", data=body, headers=headers
    )
    with urllib.request.urlopen(request, timeout=30) as resp:
        return json.loads(resp.read().decode("utf-8"))


def fetch_failed_cases(api, run_id):
    url = api.rstrip("/") + "/api/runs/%d" % run_id
    with urllib.request.urlopen(url, timeout=15) as resp:
        payload = json.loads(resp.read().decode("utf-8"))
    return [c for c in payload.get("cases", []) if c.get("status") in ("failed", "error")]


def collect_evidence(api, run_id, evidence_dir, token):
    failed = fetch_failed_cases(api, run_id)
    if not failed:
        print("[evidence] no failed/error cases; skip case-level evidence.")
    shots_dir = os.path.join(evidence_dir, "screenshots")
    attached = 0
    for case in failed:
        name = case.get("name", "")
        case_id = case.get("id")
        if not name or case_id is None:
            continue
        candidate = os.path.join(shots_dir, name + ".png")
        if not os.path.isfile(candidate):
            continue
        try:
            upload_attachment(api, run_id, case_id, candidate, token)
            attached += 1
            print("[evidence] attached %s -> case %s (%s)" % (os.path.basename(candidate), case_id, name))
        except Exception as exc:
            print("[WARN] failed to attach %s: %s" % (candidate, exc))

    log_path = os.path.join(evidence_dir, "service.log")
    if os.path.isfile(log_path):
        try:
            upload_attachment(api, run_id, None, log_path, token)
            print("[evidence] attached service.log -> run %s" % run_id)
        except Exception as exc:
            print("[WARN] failed to attach service.log: %s" % exc)
    print("[evidence] attached %d case-level screenshot(s)" % attached)


def main():
    parser = argparse.ArgumentParser(description="merge JUnit XML and report to test platform")
    parser.add_argument("files", nargs="+", help="JUnit XML 文件或 glob 表达式，可传多个")
    parser.add_argument("--api", default=os.environ.get("TEST_PLATFORM_URL", ""))
    parser.add_argument("--token", default=os.environ.get("TEST_PLATFORM_TOKEN", ""))
    parser.add_argument("--project", default="model")
    parser.add_argument("--name", default="backend-ci")
    parser.add_argument("--evidence-dir", default="", help="证据目录，默认 tests/.artifacts")
    args = parser.parse_args()
    args.api = (args.api or "").strip().rstrip("/")
    args.token = (args.token or "").strip().lstrip("\ufeff")

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
    request = urllib.request.Request(args.api + "/api/import", data=body, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=30) as resp:
            result = json.loads(resp.read().decode("utf-8"))
        print("reported:", result)
    except Exception as exc:
        print("[ERROR] report failed:", exc)
        return 1

    run_id = result.get("run_id")
    if run_id is None:
        print("[WARN] no run_id in response; skip evidence upload.")
        return 0

    if args.evidence_dir and os.path.isdir(args.evidence_dir):
        try:
            collect_evidence(args.api, run_id, args.evidence_dir, args.token)
        except Exception as exc:
            print("[WARN] evidence collection failed:", exc)
    return 0


if __name__ == "__main__":
    sys.exit(main())