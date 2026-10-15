# -*- coding: utf-8 -*-
import json

import pytest
from playwright.sync_api import expect


pytestmark = [pytest.mark.browser, pytest.mark.ui]


def login_admin(page, base_url, credentials):
    page.goto(base_url + "/admin", wait_until="domcontentloaded")
    expect(page.locator("h1")).to_contain_text("游戏后台管理台")
    page.locator("#sheetLoginName").fill(credentials["login_name"])
    page.locator("#sheetLoginPassword").fill(credentials["password"])
    page.locator("#sheetLoginBtn").click()
    page.wait_for_function(
        "Boolean(localStorage.getItem('hello_admin_token'))",
        timeout=10000,
    )
    expect(page.locator("#adminState")).not_to_have_text("未知", timeout=10000)
    token = page.evaluate("localStorage.getItem('hello_admin_token')")
    assert token


def select_player(page, player_id):
    page.locator("#sheetLookupId").fill(player_id)
    page.locator("#sheetPlayersBtn").click()
    row = page.locator('[data-sheet-player="{}"]'.format(player_id))
    expect(row).to_be_visible(timeout=10000)
    row.click()
    expect(page.locator("#sheetSelectedSummary")).to_contain_text(player_id)


def test_admin_can_login_in_browser(browser_page, base_url, admin_credentials):
    login_admin(browser_page, base_url, admin_credentials)
    expect(browser_page.locator("#adminState")).not_to_have_text("未知")


def test_admin_can_query_player_and_inventory(
    browser_page, base_url, admin_credentials, make_user
):
    user = make_user("ui_player")
    login_admin(browser_page, base_url, admin_credentials)
    select_player(browser_page, user.player_id)

    browser_page.locator("#sheetLoadInventoryBtn").click()
    expect(browser_page.locator("#output")).to_contain_text("读取玩家仓库 成功", timeout=10000)
    expect(browser_page.locator("#sheetInventoryBody")).to_be_visible()


def test_admin_can_send_mail_from_browser(
    browser_page, base_url, admin_credentials, make_user
):
    user = make_user("ui_mail")
    title = "playwright_mail_{}".format(user.player_id)
    login_admin(browser_page, base_url, admin_credentials)
    select_player(browser_page, user.player_id)

    browser_page.locator("#sheetMailTitle").fill(title)
    browser_page.locator("#sheetMailBody").fill("Playwright end-to-end mail")
    browser_page.locator("#sheetMailRewards").fill(json.dumps([
        {"kind": "gold", "amount": 5}
    ]))
    browser_page.locator("#sheetMailSelectedBtn").click()
    expect(browser_page.locator("#output")).to_contain_text("表格发送邮件 成功", timeout=10000)

    browser_page.locator('.toolbar a[href="#sec-mail-query"]').click()
    expect(browser_page.locator("#mailQueryPlayerId")).to_be_visible()
    browser_page.locator("#mailQueryPlayerId").fill(user.player_id)
    browser_page.locator("#mailQueryBtn").click()
    expect(browser_page.locator("#output")).to_contain_text("查询邮件列表 成功", timeout=10000)
    expect(browser_page.locator("#resultBody")).to_contain_text(title)


def test_admin_can_query_audit_log_in_browser(
    browser_page, base_url, admin_credentials, admin_api, make_user
):
    user = make_user("ui_audit")
    admin_api.post("/rewards/grant", json={
        "player_id": user.player_id,
        "source_type": "ui_e2e",
        "source_id": user.player_id,
        "rewards": [{"kind": "currency", "resource_id": "gold", "amount": 1}],
    }, expected=200)

    login_admin(browser_page, base_url, admin_credentials)
    browser_page.locator('.toolbar a[href="#sec-audit-query"]').click()
    expect(browser_page.locator("#auditQueryBtn")).to_be_visible()
    browser_page.locator("#auditQueryBtn").click()
    expect(browser_page.locator("#output")).to_contain_text("查询审计日志 成功", timeout=10000)
    expect(browser_page.locator("#summaryTitle")).to_contain_text("查询审计日志")
    expect(browser_page.locator("#resultBody tr").first).to_be_visible()
