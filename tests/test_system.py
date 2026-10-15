# -*- coding: utf-8 -*-
import pytest


pytestmark = pytest.mark.smoke


def test_health_ok(api):
    body = api.json(api.get("/health", expected=200))
    assert body["ok"] is True
    assert body["data"]["status"] == "ok"


def test_config_reports_in_memory(api):
    config = api.data(api.get("/config", expected=200))
    assert config["storage_mode"] == "in_memory"


def test_metrics_contains_request_counter(api):
    response = api.get("/metrics", expected=200)
    assert "mf_http_requests_total" in response.text
