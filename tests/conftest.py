# -*- coding: utf-8 -*-
"""Shared fixtures for isolated backend integration tests.

The suite starts one in-memory service process, chooses a free local port by
default, and exposes typed API clients and user factories to all test modules.
"""

import os
import shutil
import socket
import subprocess
import time
import uuid
from pathlib import Path

import pytest

from framework import ApiClient, TestUser


PROJECT_ROOT = Path(__file__).resolve().parents[1]
ADMIN_LOGIN = "pytest_admin"
ADMIN_PASSWORD = "PytestAdmin_123!"


def _find_service_executable():
    configured = os.environ.get("HELLO_SERVICE_TEST_EXE")
    if configured:
        path = Path(configured).resolve()
        if not path.is_file():
            pytest.fail("HELLO_SERVICE_TEST_EXE does not exist: {}".format(path))
        return path

    if os.name == "nt":
        candidates = [
            PROJECT_ROOT / "build" / "RelWithDebInfo" / "hello_service.exe",
            PROJECT_ROOT / "build" / "Debug" / "hello_service.exe",
            PROJECT_ROOT / "build-otel-off" / "Debug" / "hello_service.exe",
        ]
    else:
        candidates = [
            PROJECT_ROOT / "build-linux-local" / "hello_service",
            PROJECT_ROOT / "build" / "hello_service",
        ]
    existing = [path for path in candidates if path.is_file()]
    if not existing:
        pytest.fail(
            "No hello_service executable found. Build the service or set "
            "HELLO_SERVICE_TEST_EXE."
        )
    return max(existing, key=lambda path: path.stat().st_mtime)


def _test_port():
    configured = os.environ.get("HELLO_SERVICE_TEST_PORT")
    if configured:
        return int(configured)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


@pytest.fixture(scope="session")
def service_process():
    """Start one isolated in-memory backend and stop it after the test run."""
    executable = _find_service_executable()
    port = _test_port()
    base_url = "http://127.0.0.1:{}".format(port)
    env = os.environ.copy()
    env.update({
        "HELLO_SERVICE_PORT": str(port),
        "HELLO_SERVICE_HOST": "127.0.0.1",
        "HELLO_SERVICE_STORAGE_MODE": "in_memory",
        "HELLO_SERVICE_ENV": "development",
        "HELLO_SERVICE_AUTO_BOOTSTRAP_DEV_ADMIN": "1",
        "HELLO_SERVICE_DEV_ADMIN_LOGIN": ADMIN_LOGIN,
        "HELLO_SERVICE_DEV_ADMIN_PASSWORD": ADMIN_PASSWORD,
        # The suite creates more than 30 unique users on one loopback IP.
        "HELLO_SERVICE_REGISTER_MAX_PER_IP": "1000",
    })

    artifact_dir = Path(__file__).parent / ".artifacts"
    artifact_dir.mkdir(exist_ok=True)
    log_path = artifact_dir / "service.log"
    log = log_path.open("w", encoding="utf-8", errors="replace")
    creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
    process = subprocess.Popen(
        [str(executable)],
        cwd=str(executable.parent),
        env=env,
        stdout=log,
        stderr=subprocess.STDOUT,
        creationflags=creation_flags,
    )

    probe = ApiClient(base_url, timeout=1)
    deadline = time.time() + 20
    started = False
    while time.time() < deadline:
        if process.poll() is not None:
            break
        try:
            if probe.get("/health").status_code == 200:
                started = True
                break
        except Exception:
            pass
        time.sleep(0.2)

    if not started:
        probe.close()
        if process.poll() is None:
            process.kill()
        log.close()
        pytest.fail("Service did not become ready; see {}".format(log_path))

    yield {"process": process, "base_url": base_url, "executable": executable}

    probe.close()
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
    log.close()


@pytest.fixture(scope="session")
def base_url(service_process):
    return service_process["base_url"]


@pytest.fixture(scope="session")
def service_executable(service_process):
    return service_process["executable"]


@pytest.fixture(scope="session")
def api(base_url):
    client = ApiClient(base_url)
    yield client
    client.close()


@pytest.fixture
def admin_api(api):
    response = api.post(
        "/admin/login",
        json={"login_name": ADMIN_LOGIN, "password": ADMIN_PASSWORD},
        expected=200,
    )
    token = api.data(response)["session"]["token"]
    client = api.with_token(token)
    yield client
    client.close()


@pytest.fixture(scope="session")
def make_user(api):
    """Create a unique user and return current login plus player profile data."""
    clients = []

    def _make(prefix="user"):
        login_name = "{}_{}".format(prefix, uuid.uuid4().hex[:8])
        password = "Passw0rd!"
        api.post(
            "/auth/register",
            json={
                "login_name": login_name,
                "display_name": login_name,
                "password": password,
            },
            expected=201,
        )
        login = api.data(api.post(
            "/auth/login",
            json={"login_name": login_name, "password": password},
            expected=200,
        ))
        user_api = api.with_token(login["session"]["token"])
        clients.append(user_api)
        player = user_api.data(user_api.get("/players/me", expected=200))
        return TestUser(
            login_name=login_name,
            password=password,
            account=login["account"],
            player=player,
            session=login["session"],
            api=user_api,
        )

    yield _make
    for client in clients:
        client.close()


@pytest.fixture(scope="session")
def admin_credentials():
    return {"login_name": ADMIN_LOGIN, "password": ADMIN_PASSWORD}


@pytest.fixture(scope="session")
def browser():
    """Launch an installed Edge/Chrome browser without downloading binaries."""
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        pytest.skip("Playwright is not installed; run pip install -r tests/requirements.txt")

    configured = os.environ.get("HELLO_SERVICE_TEST_BROWSER_EXECUTABLE")
    candidates = [
        Path(configured) if configured else None,
        Path(r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"),
        Path(r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"),
        Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe"),
    ]
    executable = next((path for path in candidates if path and path.is_file()), None)
    if executable is None:
        system_browser = shutil.which("microsoft-edge") or shutil.which("google-chrome")
        executable = Path(system_browser) if system_browser else None
    if executable is None:
        pytest.skip(
            "No Edge/Chrome executable found; set HELLO_SERVICE_TEST_BROWSER_EXECUTABLE"
        )

    with sync_playwright() as playwright:
        instance = playwright.chromium.launch(
            executable_path=str(executable),
            headless=True,
        )
        yield instance
        instance.close()


@pytest.fixture
def browser_page(browser, request):
    context = browser.new_context(
        locale="zh-CN",
        viewport={"width": 1440, "height": 1000},
    )
    page = context.new_page()
    yield page

    artifact_dir = Path(__file__).parent / ".artifacts" / "screenshots"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    try:
        page.screenshot(path=str(artifact_dir / (request.node.name + ".png")), full_page=True)
    except Exception:
        pass
    context.close()
