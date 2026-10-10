#pragma once

#include <string>

// 文件职责：
// 1. 提供 hello 示例的管理员后台单页 HTML。
// 2. 页面直接复用当前服务已有的管理员接口，不单独引入前端工程。
// 3. 把常用运营入口集中到一个页面，便于你联调、review 和学习协议。
//
// 调用流程：
// 1. 浏览器访问 /admin 或 /admin/ui。
// 2. hello_routes.cpp 调用 hello_admin_ui_page_html() 组装页面字符串。
// 3. 服务端把这份 HTML 作为 text/html 响应返回。
// 4. 页面内脚本再调用 /admin/login、/admin/query/*、/mail/send 等接口。

inline std::string hello_admin_ui_page_html() {
    static constexpr const char* kPart1 = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>游戏后台管理台</title>
  <style>
    :root {
      --bg: #efe6d6;
      --paper: rgba(255, 250, 242, .94);
      --paper-strong: #fffaf2;
      --ink: #182426;
      --muted: #637274;
      --line: #d6c5ae;
      --accent: #a94130;
      --accent-2: #21464d;
      --ok: #22624c;
      --warn: #985500;
      --danger: #7e281d;
      --panel: #f7f0e5;
      --code: #1b2425;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      color: var(--ink);
      background:
        radial-gradient(circle at top left, rgba(169,65,48,.14), transparent 28%),
        radial-gradient(circle at top right, rgba(33,70,77,.14), transparent 32%),
        linear-gradient(180deg, #f7efe4 0%, var(--bg) 100%);
      font: 14px/1.5 "Segoe UI", "Microsoft YaHei", sans-serif;
    }
    .shell {
      width: min(1560px, calc(100vw - 28px));
      margin: 18px auto 40px;
    }
    .hero {
      padding: 24px 26px;
      border: 1px solid var(--line);
      border-radius: 24px;
      background:
        linear-gradient(135deg, rgba(169,65,48,.10), transparent 34%),
        linear-gradient(120deg, rgba(33,70,77,.08), transparent 42%),
        linear-gradient(180deg, var(--paper-strong), #fffdf8);
      box-shadow: 0 18px 44px rgba(71, 49, 27, .10);
    }
    .hero h1 {
      margin: 0 0 8px;
      font-size: 34px;
      letter-spacing: 1px;
    }
    .hero p {
      margin: 0;
      color: var(--muted);
      max-width: 980px;
    }
    .statusbar {
      display: grid;
      grid-template-columns: repeat(4, minmax(0, 1fr));
      gap: 12px;
      margin-top: 18px;
    }
    .badge {
      min-height: 76px;
      padding: 12px 14px;
      border: 1px solid var(--line);
      border-radius: 16px;
      background: rgba(255,255,255,.74);
    }
    .badge .k {
      color: var(--muted);
      font-size: 12px;
      text-transform: uppercase;
      letter-spacing: .08em;
    }
    .badge .v {
      margin-top: 6px;
      font-size: 15px;
      font-weight: 700;
      word-break: break-all;
    }
    .toolbar {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      margin-top: 16px;
    }
    .toolbar a {
      color: var(--accent-2);
      text-decoration: none;
      padding: 8px 12px;
      border-radius: 999px;
      border: 1px solid rgba(33,70,77,.18);
      background: rgba(255,255,255,.62);
    }
    .layout {
      display: grid;
      grid-template-columns: 1.2fr .88fr;
      gap: 18px;
      margin-top: 18px;
      align-items: start;
    }
    .stack {
      display: grid;
      gap: 16px;
    }
    .grid {
      display: grid;
      grid-template-columns: repeat(12, 1fr);
      gap: 16px;
    }
    .card {
      grid-column: span 6;
      border: 1px solid var(--line);
      border-radius: 20px;
      background: var(--paper);
      box-shadow: 0 8px 26px rgba(61, 44, 25, .06);
      overflow: hidden;
    }
    .card.wide { grid-column: span 12; }
    .card.third { grid-column: span 4; }
    .card-header {
      padding: 14px 16px 10px;
      border-bottom: 1px solid rgba(214,197,174,.72);
      background: linear-gradient(180deg, rgba(255,255,255,.7), rgba(255,250,242,0));
    }
    .card-title {
      margin: 0;
      font-size: 19px;
    }
    .card-note {
      margin: 6px 0 0;
      color: var(--muted);
      font-size: 13px;
    }
    .card-body {
      padding: 16px;
    }
    .form-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 12px;
    }
    .form-grid.one { grid-template-columns: 1fr; }
    .field {
      display: flex;
      flex-direction: column;
      gap: 6px;
    }
    .field.span-2 { grid-column: span 2; }
    label {
      font-size: 12px;
      color: var(--muted);
    }
    input, select, textarea {
      width: 100%;
      padding: 10px 12px;
      border-radius: 12px;
      border: 1px solid #cdbda7;
      background: #fffdfa;
      color: var(--ink);
      outline: none;
    }
    input:focus, select:focus, textarea:focus {
      border-color: var(--accent-2);
      box-shadow: 0 0 0 3px rgba(33,70,77,.12);
    }
    textarea {
      min-height: 104px;
      resize: vertical;
      font: 13px/1.5 Consolas, "Courier New", monospace;
    }
    .actions {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      margin-top: 14px;
    }
    button {
      border: 0;
      border-radius: 12px;
      padding: 10px 14px;
      color: #fff;
      background: var(--accent);
      cursor: pointer;
      font-weight: 700;
    }
    button.alt { background: var(--accent-2); }
    button.ok { background: var(--ok); }
    button.warn { background: var(--warn); }
    button.danger { background: var(--danger); }
    button.ghost {
      color: var(--ink);
      background: #efe3d1;
    }
    .side-card {
      border: 1px solid var(--line);
      border-radius: 20px;
      background: var(--paper);
      box-shadow: 0 8px 26px rgba(61, 44, 25, .06);
      overflow: hidden;
    }
    .side-head {
      padding: 14px 16px 10px;
      border-bottom: 1px solid rgba(214,197,174,.72);
      background: linear-gradient(180deg, rgba(255,255,255,.7), rgba(255,250,242,0));
    }
    .side-head h2 {
      margin: 0;
      font-size: 19px;
    }
    .side-head p {
      margin: 6px 0 0;
      color: var(--muted);
      font-size: 13px;
    }
    .side-body {
      padding: 16px;
      display: grid;
      gap: 14px;
    }
    .summary {
      padding: 12px 14px;
      border: 1px solid rgba(214,197,174,.72);
      border-radius: 14px;
      background: var(--panel);
    }
    .summary strong {
      display: block;
      margin-bottom: 4px;
    }
    .batch-box {
      padding: 12px 14px;
      border: 1px solid rgba(214,197,174,.72);
      border-radius: 16px;
      background: #fffdf9;
      display: grid;
      gap: 12px;
    }
    .sheet-box {
      margin-top: 14px;
      padding: 12px 14px;
      border: 1px solid rgba(214,197,174,.72);
      border-radius: 16px;
      background: #fffdf9;
      display: grid;
      gap: 12px;
    }
    .sheet-head {
      display: flex;
      justify-content: space-between;
      gap: 12px;
      align-items: center;
    }
    .sheet-head strong {
      font-size: 14px;
    }
    .sheet-head span {
      color: var(--muted);
      font-size: 12px;
    }
    .gm-workbench { display: grid; grid-template-columns: minmax(0,1.15fr) minmax(320px,.85fr); gap: 14px; align-items: stretch; }
    .excel-card, .gm-panel { border: 1px solid rgba(214,197,174,.72); border-radius: 18px; background: #fffdf9; overflow: hidden; }
    .gm-panel { padding: 14px; display: grid; gap: 12px; }
    .gm-panel h3 { margin: 0; font-size: 16px; }
    .excel-title { display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: 12px 14px; border-bottom: 1px solid rgba(214,197,174,.72); background: #f7f0e5; font-weight: 800; }
    .excel-grid { display: grid; grid-template-columns: 54px repeat(4, minmax(128px, 1fr)); overflow-x: auto; }
    .excel-cell { min-height: 48px; padding: 12px 14px; border-right: 1px solid rgba(214,197,174,.72); border-bottom: 1px solid rgba(214,197,174,.72); background: #fffdf9; white-space: nowrap; }
    .excel-cell.head, .excel-cell.index { background: #f1eadf; font-weight: 800; text-align: center; }
    .excel-cell.cmd { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; white-space: normal; }
    .gm-kpi { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 8px; }
    .gm-kpi div { padding: 10px; border: 1px solid rgba(214,197,174,.72); border-radius: 12px; background: var(--panel); }
    .gm-kpi strong { display: block; font-size: 18px; }
    .batch-count {
      font-size: 22px;
      font-weight: 800;
    }
    .detail-card {
      border: 1px solid rgba(214,197,174,.72);
      border-radius: 16px;
      background: #fffdf9;
      padding: 14px;
      display: grid;
      gap: 12px;
    }
    .detail-head {
      display: flex;
      justify-content: space-between;
      gap: 12px;
      align-items: center;
    }
    .detail-head strong {
      font-size: 15px;
    }
    .detail-grid {
      display: grid;
      grid-template-columns: repeat(2, minmax(0, 1fr));
      gap: 10px;
    }
    .detail-item {
      padding: 10px 12px;
      border: 1px solid rgba(214,197,174,.72);
      border-radius: 12px;
      background: var(--panel);
    }
    .detail-item .k {
      display: block;
      margin-bottom: 4px;
      color: var(--muted);
      font-size: 12px;
    }
    .detail-item .v {
      word-break: break-word;
      font: 12px/1.45 Consolas, "Courier New", monospace;
    }
    .detail-actions {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
    }
    .detail-extra {
      display: grid;
      gap: 10px;
    }
    .trace-tree {
      display: grid;
      gap: 10px;
    }
    .trace-node {
      border: 1px solid rgba(214,197,174,.72);
      border-radius: 14px;
      background: #fffdf9;
      padding: 10px 12px;
    }
    .trace-node.focus {
      border-color: rgba(169,65,48,.55);
      box-shadow: 0 0 0 2px rgba(169,65,48,.10);
    }
    .trace-node-head {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: 8px;
      margin-bottom: 8px;
    }
    .trace-indent {
      border-left: 2px solid rgba(33,70,77,.18);
      margin-left: 14px;
      padding-left: 12px;
    }
    .trace-attrs {
      display: grid;
      gap: 6px;
    }
    .trace-attr {
      padding: 8px 10px;
      border-radius: 10px;
      background: var(--panel);
      font: 12px/1.45 Consolas, "Courier New", monospace;
      word-break: break-word;
    }
    .hint {
      color: var(--muted);
      font-size: 12px;
    }
    pre.output {
      margin: 0;
      padding: 14px;
      border-radius: 16px;
      background: var(--code);
      color: #e7f4ef;
      font: 12px/1.5 Consolas, "Courier New", monospace;
      white-space: pre-wrap;
      word-break: break-word;
      min-height: 220px;
      max-height: 420px;
      overflow: auto;
    }
    .table-wrap {
      border: 1px solid rgba(214,197,174,.72);
      border-radius: 16px;
      overflow: auto;
      background: #fffdf9;
      max-height: 560px;
    }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 13px;
    }
    th, td {
      padding: 10px 12px;
      border-bottom: 1px solid rgba(214,197,174,.66);
      text-align: left;
      vertical-align: top;
    }
    th {
      position: sticky;
      top: 0;
      background: #f8efe2;
      z-index: 1;
    }
    #resultTable th:first-child {
      left: 0;
      z-index: 3;
      box-shadow: 1px 0 0 rgba(214,197,174,.72);
    }
    #resultTable td:first-child {
      position: sticky;
      left: 0;
      z-index: 2;
      background: #fffdf9;
      box-shadow: 1px 0 0 rgba(214,197,174,.72);
    }
    th.sortable {
      cursor: pointer;
      user-select: none;
    }
    th.sortable:hover {
      background: #f3e7d6;
    }
    .sheet-status {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      font: 12px/1.35 Consolas, "Courier New", monospace;
      padding: 3px 8px;
      border-radius: 999px;
      border: 1px solid rgba(214,197,174,.72);
      background: #fffdf9;
    }
    .sheet-status.active {
      color: var(--ok);
      border-color: rgba(34,98,76,.28);
      background: rgba(34,98,76,.08);
    }
    .sheet-status.frozen {
      color: var(--warn);
      border-color: rgba(152,85,0,.28);
      background: rgba(152,85,0,.08);
    }
    .sheet-status.banned {
      color: var(--danger);
      border-color: rgba(126,40,29,.28);
      background: rgba(126,40,29,.10);
    }
    td code {
      font: 12px Consolas, "Courier New", monospace;
    }
    .mini-btn {
      border: 0;
      border-radius: 999px;
      padding: 4px 8px;
      margin-right: 6px;
      color: #fff;
      background: var(--accent-2);
      cursor: pointer;
      font-size: 12px;
      font-weight: 700;
    }
    .mini-btn.alt {
      background: var(--accent);
    }
    .mono {
      font: 12px Consolas, "Courier New", monospace;
    }
    .muted {
      color: var(--muted);
    }
    .workbook {
      margin-top: 16px;
      border: 1px solid rgba(214,197,174,.88);
      border-radius: 14px;
      background: #fffdf9;
      overflow: hidden;
    }
    .workbook-title {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      padding: 12px 14px;
      border-bottom: 1px solid rgba(214,197,174,.88);
      background: #f1eadf;
      font-weight: 800;
    }
    .workbook-grid {
      display: grid;
      grid-template-columns: 56px 1.05fr 1.15fr 1.1fr 1.35fr;
      overflow-x: auto;
    }
    .workbook-cell {
      min-height: 50px;
      padding: 12px 14px;
      border-right: 1px solid rgba(214,197,174,.72);
      border-bottom: 1px solid rgba(214,197,174,.72);
      background: #fffdf9;
      white-space: nowrap;
    }
    .workbook-cell.head,
    .workbook-cell.index {
      background: #f6efe5;
      text-align: center;
      font-weight: 800;
    }
    .workbook-cell.cmd {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
      align-items: center;
      white-space: normal;
    }
)HTML";

    static constexpr const char* kPart1_sheet_css = R"HTML(
    .sheet-app { margin-top: 16px; display: grid; grid-template-columns: minmax(0,1.5fr) minmax(360px,.8fr); gap: 14px; align-items: start; }
    .sheet-panel, .sheet-detail { border: 1px solid rgba(214,197,174,.88); border-radius: 14px; background: #fffdf9; overflow: hidden; }
    .sheet-bar { display: flex; flex-wrap: wrap; gap: 10px; align-items: center; padding: 12px 14px; border-bottom: 1px solid rgba(214,197,174,.88); background: #f1eadf; }
    .sheet-bar strong { margin-right: auto; }
    .sheet-table-wrap { overflow: auto; max-height: 620px; }
    .sheet-table { min-width: 980px; border-collapse: collapse; width: 100%; }
    .sheet-table th, .sheet-table td { border-right: 1px solid rgba(214,197,174,.72); border-bottom: 1px solid rgba(214,197,174,.72); padding: 11px 12px; background: #fffdf9; white-space: nowrap; }
    .sheet-table th, .sheet-table .row-index { background: #f6efe5; text-align: center; font-weight: 800; }
    .sheet-table th { position: sticky; top: 0; z-index: 2; }
    .sheet-table .row-index { position: sticky; left: 0; z-index: 1; }
    .sheet-table tr.active td { background: rgba(33,70,77,.08); }
    .sheet-side, .sheet-detail-body { display: grid; gap: 14px; }
    .sheet-detail h3 { margin: 0; padding: 12px 14px; border-bottom: 1px solid rgba(214,197,174,.88); background: #f1eadf; font-size: 16px; }
    .sheet-detail-body { padding: 14px; gap: 10px; }
    .sheet-mini-grid { display: grid; grid-template-columns: repeat(2,minmax(0,1fr)); gap: 8px; }
    .sheet-mini-grid div { padding: 9px 10px; border: 1px solid rgba(214,197,174,.66); border-radius: 8px; background: var(--panel); min-width: 0; }
    .sheet-mini-grid span { display: block; color: var(--muted); font-size: 12px; }
    .sheet-mini-grid code { display: block; margin-top: 3px; overflow: hidden; text-overflow: ellipsis; }
    .legacy-layout { display: none; }
    .card {
      border-radius: 12px;
      box-shadow: none;
    }
    .card-header,
    .side-head {
      background: #f1eadf;
    }
    .card-title::before,
    .side-head h2::before {
      content: "表格 / ";
      color: var(--muted);
      font-weight: 500;
    }
    .field {
      padding: 8px;
      border: 1px solid rgba(214,197,174,.58);
      border-radius: 10px;
      background: #fffdf9;
    }
    input, select, textarea {
      border-radius: 6px;
      background: #fff;
    }
    button {
      border-radius: 8px;
    }
    @media (max-width: 1280px) {
      .layout { grid-template-columns: 1fr; }
      .sheet-app { grid-template-columns: 1fr; }
      .gm-workbench { grid-template-columns: 1fr; }
      .card.third { grid-column: span 6; }
    }
    @media (max-width: 980px) {
      .statusbar { grid-template-columns: repeat(2, minmax(0, 1fr)); }
      .card, .card.third { grid-column: span 12; }
    }
    @media (max-width: 720px) {
      .shell { width: min(100vw - 14px, 1560px); margin-top: 10px; }
      .hero { padding: 18px; }
      .hero h1 { font-size: 24px; }
      .statusbar { grid-template-columns: 1fr; }
      .form-grid { grid-template-columns: 1fr; }
      .field.span-2 { grid-column: span 1; }
      .detail-grid { grid-template-columns: 1fr; }
    }
  </style>
</head>
<body>
  <div class="shell">
    <section class="hero">
      <h1>游戏后台管理台</h1>
      <p>这一页把当前示例服务里已经有的管理员接口集中起来，做成一个可以直接打开的运营后台。你可以在这里做管理员登录、玩家检索、封号冻结、禁言、邮件、公告，以及邮件列表、奖励流水、聊天记录、封禁记录、审计日志、管理员列表和任务查询。</p>
      <div class="statusbar">
        <div class="badge">
          <div class="k">当前 Token</div>
          <div class="v" id="tokenState">未登录</div>
        </div>
        <div class="badge">
          <div class="k">当前管理员</div>
          <div class="v" id="adminState">未知</div>
        </div>
        <div class="badge">
          <div class="k">最近查询</div>
          <div class="v" id="queryState">无</div>
        </div>
        <div class="badge">
          <div class="k">接口基址</div>
          <div class="v" id="baseState"></div>
        </div>
      </div>
      <div class="toolbar">
        <a href="#sec-auth">登录与查号</a>
        <a href="#sec-player-list">玩家列表</a>
        <a href="#sec-mail-query">邮件与奖励</a>
        <a href="#sec-item-config">物品配置</a>
        <a href="#sec-gacha-config">抽卡配置</a>
        <a href="#sec-shop-config">商城配置</a>
        <a href="#sec-chat-query">聊天与封禁</a>
        <a href="#sec-audit-query">审计与后台用户</a>
        <a href="#sec-tracing">链路追踪</a>
        <a href="#sec-debug-load">负载调试</a>
        <a href="#sec-debug-announcement-cache">公告缓存</a>
      </div>
)HTML";

    static constexpr const char* kPart1_workbook = R"HTML(
      <div class="sheet-app">
        <section class="sheet-panel">
          <div class="sheet-bar">
            <strong>玩家信息表</strong>
            <input id="sheetLoginName" value="admin_demo" placeholder="管理员账号" />
            <input id="sheetLoginPassword" type="password" value="AdminDemo_123456" placeholder="管理员密码" />
            <input id="sheetLookupId" placeholder="8位玩家ID，例如 21706635" />
            <input id="sheetLimit" type="number" value="50" />
            <button id="sheetLoginBtn">登录</button>
            <button class="alt" id="sheetMeBtn">检查身份</button>
            <button class="alt" id="sheetPlayersBtn">查询</button>
            <button class="ghost" id="sheetClearBtn">清空选择</button>
            <span class="muted" id="sheetStatusLine">等待操作</span>
          </div>
          <div class="sheet-table-wrap">
            <table class="sheet-table">
              <thead>
                <tr>
                  <th></th>
                  <th>A player_id</th>
                  <th>B nickname</th>
                  <th>C account_id</th>
                  <th>D gold</th>
                  <th>E gem</th>
                  <th>F 状态</th>
                  <th>G 禁言到期</th>
                  <th>H 最后登录</th>
                  <th>I 操作</th>
                </tr>
              </thead>
              <tbody id="sheetPlayerBody">
                <tr><td class="row-index">1</td><td colspan="9" class="muted">输入 8 位玩家ID 后点“查询”，玩家信息会显示在这里。</td></tr>
              </tbody>
            </table>
          </div>
        </section>

        <aside class="sheet-side">
          <section class="sheet-detail">
            <h3>当前选中玩家</h3>
            <div class="sheet-detail-body">
              <div class="sheet-mini-grid" id="sheetSelectedSummary">
                <div><span>player_id</span><code>未选择</code></div>
                <div><span>nickname</span><code>未选择</code></div>
                <div><span>account_id</span><code>未选择</code></div>
                <div><span>资产</span><code>未选择</code></div>
              </div>
              <div class="actions" style="margin-top:0;">
                <button class="alt" id="sheetLoadInventoryBtn">读取仓库</button>
                <button class="warn" id="sheetMuteBtn">禁言</button>
                <button class="ok" id="sheetUnmuteBtn">取消禁言</button>
                <button class="danger" id="sheetStatusBtn">改状态</button>
              </div>
            </div>
          </section>

          <section class="sheet-detail">
            <h3>仓库 / 背包</h3>
            <div class="sheet-table-wrap" style="max-height:260px;">
              <table class="sheet-table" style="min-width:520px;">
                <thead><tr><th></th><th>A item_id</th><th>B quantity</th><th>C instance_id</th></tr></thead>
                <tbody id="sheetInventoryBody">
                  <tr><td class="row-index">1</td><td colspan="3" class="muted">选择玩家后读取仓库。</td></tr>
                </tbody>
              </table>
            </div>
          </section>

          <section class="sheet-detail">
            <h3>操作当前玩家</h3>
            <div class="sheet-detail-body">
              <div class="field"><label for="sheetMuteSeconds">禁言秒数</label><input id="sheetMuteSeconds" type="number" value="3600" /></div>
              <div class="field"><label for="sheetMuteReason">禁言原因</label><input id="sheetMuteReason" placeholder="刷屏 / 骚扰 / 敏感词" /></div>
              <div class="field"><label for="sheetStatusValue">账号状态</label><select id="sheetStatusValue"><option value="frozen">frozen</option><option value="banned">banned</option><option value="active">active</option></select></div>
              <div class="field"><label for="sheetStatusReason">状态原因</label><input id="sheetStatusReason" placeholder="外挂 / 解封 / 风控" /></div>
              <div class="field"><label for="sheetMailTitle">邮件标题</label><input id="sheetMailTitle" placeholder="维护补偿 / 活动奖励" /></div>
              <div class="field"><label for="sheetMailBody">邮件正文</label><textarea id="sheetMailBody" placeholder="邮件正文"></textarea></div>
              <div class="field"><label>可视化奖励</label>
                <select id="sheetRewardKind"><option value="gold">金币 gold</option><option value="gem">钻石 gem</option><option value="currency">其他货币 currency</option><option value="item">道具 item</option><option value="attachment_template">附件模板</option></select>
                <input id="sheetRewardResource" list="rewardItemOptions" placeholder="item_id / currency_id，gold/gem 可留空" />
                <input id="sheetRewardAmount" type="number" value="1000" min="1" />
                <div class="actions" style="margin-top:8px;"><button class="ok" id="sheetAddRewardBtn" type="button">添加奖励行</button><button class="ghost" id="sheetClearRewardsBtn" type="button">清空奖励</button></div>
              </div>
              <div class="field"><label>奖励明细</label><div class="sheet-table-wrap" style="max-height:170px;"><table class="sheet-table" style="min-width:420px;"><thead><tr><th>类型</th><th>资源ID</th><th>数量</th><th>操作</th></tr></thead><tbody id="sheetRewardBody"><tr><td colspan="4" class="muted">还没有奖励，添加后会自动生成 JSON。</td></tr></tbody></table></div></div>
              <div class="field"><label for="sheetMailRewards">奖励 JSON（自动生成，也可手动改）</label><textarea id="sheetMailRewards">[
  {"kind":"gold","amount":1000},
  {"kind":"item","resource_id":"item_potion_small","amount":5}
]</textarea></div>
              <div class="actions" style="margin-top:0;">
                <button class="ok" id="sheetMailSelectedBtn">发给当前/勾选</button>
                <button class="danger" id="sheetMailAllBtn">一键发给全部玩家</button>
              </div>
            </div>
          </section>

          <section class="sheet-detail" id="sec-item-config">
            <h3>物品配置管理</h3>
            <div class="sheet-detail-body">
              <div class="field"><label for="itemId">item_id</label><input id="itemId" placeholder="item_potion_small" /></div>
              <div class="field"><label for="itemName">物品名称</label><input id="itemName" placeholder="小型生命药水" /></div>
              <div class="field"><label for="itemDescription">物品描述</label><textarea id="itemDescription" placeholder="恢复少量生命值。"></textarea></div>
              <div class="field"><label for="itemIcon">图标资源</label><input id="itemIcon" placeholder="icons/items/potion_small.png 或 /Game/UI/Icons/T_PotionSmall" /></div>
              <div class="field"><label for="itemRarity">稀有度</label><select id="itemRarity"><option value="3星">3星</option><option value="4星">4星</option><option value="5星">5星</option></select></div>
              <div class="field"><label for="itemMaxStack">最大堆叠</label><input id="itemMaxStack" type="number" value="99" /></div>
              <div class="field"><label for="itemType">物品类型</label><input id="itemType" value="material" placeholder="material / consumable / equipment" /></div>
              <div class="field"><label for="itemEnabled">是否启用</label><select id="itemEnabled"><option value="true">启用</option><option value="false">停用</option></select></div>
              <div class="field"><label for="itemQueryRarity">查询稀有度</label><select id="itemQueryRarity"><option value="">全部</option><option value="3星">3星</option><option value="4星">4星</option><option value="5星">5星</option></select></div>
              <div class="field"><label for="itemQueryEnabled">查询启用状态</label><select id="itemQueryEnabled"><option value="">全部</option><option value="true">启用</option><option value="false">停用</option></select></div>
              <div class="actions" style="margin-top:0;">
                <button class="ok" id="itemSaveBtn">保存物品配置</button>
                <button class="alt" id="itemQueryBtn">查询物品配置</button>
              </div>
              <div class="sheet-table-wrap" style="max-height:300px;">
                <table class="sheet-table" style="min-width:860px;">
                  <thead><tr><th></th><th>A item_id</th><th>B 名称</th><th>C 稀有度</th><th>D 类型</th><th>E 堆叠</th><th>F 启用</th><th>G 图标</th><th>H 操作</th></tr></thead>
                  <tbody id="itemConfigBody">
                    <tr><td class="row-index">1</td><td colspan="8" class="muted">保存或查询物品后会显示在这里。</td></tr>
                  </tbody>
                </table>
              </div>
            </div>
          </section>

          <section class="sheet-detail" id="sec-gacha-config">
            <h3>抽卡配置管理</h3>
            <div class="sheet-detail-body">
              <div class="field"><label for="gachaPoolId">pool_id</label><input id="gachaPoolId" placeholder="standard_2026 或 limited_role_2026" /></div>
              <div class="field"><label for="gachaPoolType">池子种类</label><select id="gachaPoolType"><option value="standard">常驻池 standard</option><option value="limited_role">限定角色池 limited_role</option><option value="limited_weapon">限定武器池 limited_weapon</option><option value="newbie">新手池 newbie</option><option value="event">活动池 event</option></select></div>
              <div class="field"><label for="gachaPoolName">卡池名称</label><input id="gachaPoolName" placeholder="常驻祈愿 / 限定角色池" /></div>
              <div class="field"><label for="gachaPoolDescription">卡池描述</label><textarea id="gachaPoolDescription" placeholder="卡池说明、UP 信息、开放规则。"></textarea></div>
              <div class="field"><label for="gachaBannerIcon">卡池图标</label><input id="gachaBannerIcon" placeholder="/Game/UI/Banners/T_Banner" /></div>
              <div class="field"><label for="gachaCostCurrency">消耗货币</label><input id="gachaCostCurrency" value="gem" /></div>
              <div class="field"><label for="gachaCostAmount">每抽消耗</label><input id="gachaCostAmount" type="number" value="160" /></div>
              <div class="field"><label for="gachaPity5">5星硬保底</label><input id="gachaPity5" type="number" value="90" /></div>
              <div class="field"><label for="gachaPity4">4星硬保底</label><input id="gachaPity4" type="number" value="10" /></div>
              <div class="field"><label for="gachaEnabled">是否启用</label><select id="gachaEnabled"><option value="true">启用</option><option value="false">停用</option></select></div>
              <div class="field"><label for="gachaItemKey">奖品 item_key</label><input id="gachaItemKey" placeholder="char_hero_a / item_potion_small" /></div>
              <div class="field"><label for="gachaRarity">星级</label><input id="gachaRarity" type="number" value="3" /></div>
              <div class="field"><label for="gachaWeight">权重</label><input id="gachaWeight" type="number" value="100" /></div>
              <div class="field"><label for="gachaIsUp">是否 UP</label><select id="gachaIsUp"><option value="false">否</option><option value="true">是</option></select></div>
              <div class="field"><label for="gachaRewardKind">奖励类型</label><select id="gachaRewardKind"><option value="item">item</option><option value="currency">currency</option><option value="gold">gold</option><option value="attachment_template">attachment_template</option></select></div>
              <div class="field"><label for="gachaRewardItemSelect">从物品配置选择</label><select id="gachaRewardItemSelect"><option value="">先加载物品列表</option></select></div>
              <div class="field"><label for="gachaRewardItemKeyword">搜索物品</label><input id="gachaRewardItemKeyword" placeholder="输入 item_id 或名称过滤下面列表" /></div>
              <div class="field"><label for="gachaRewardResource">奖励资源 ID</label><input id="gachaRewardResource" placeholder="item_potion_small / gem" /></div>
              <div class="field"><label for="gachaRewardAmount">奖励数量</label><input id="gachaRewardAmount" type="number" value="1" /></div>
              <div class="actions" style="margin-top:0;">
                <button class="ok" id="gachaPoolSaveBtn">保存卡池</button>
                <button class="ok" id="gachaItemSaveBtn">保存奖品</button>
                <button class="alt" id="gachaQueryBtn">查询抽卡配置</button>
                <button class="alt" id="gachaLoadItemsBtn">加载物品列表</button>
                <button class="ghost" id="gachaSyncItemKeyBtn">用物品ID填奖品ID</button>
              </div>
              <div class="sheet-table-wrap" style="max-height:260px;">
                <table class="sheet-table" style="min-width:760px;">
                  <thead><tr><th></th><th>A item_id</th><th>B 名称</th><th>C 稀有度</th><th>D 类型</th><th>E 操作</th></tr></thead>
                  <tbody id="gachaRewardItemBody">
                    <tr><td class="row-index">1</td><td colspan="5" class="muted">点“加载物品下拉”后，这里会显示所有可加入卡池的物品。</td></tr>
                  </tbody>
                </table>
              </div>
              <div class="sheet-mini-grid" id="gachaRaritySummary">
                <div><span>当前池子</span><code>未选择</code></div>
                <div><span>总权重</span><code>0</code></div>
                <div><span>5星概率</span><code>0%</code></div>
                <div><span>4星概率</span><code>0%</code></div>
              </div>
              <div class="sheet-table-wrap" style="max-height:220px;">
                <table class="sheet-table" style="min-width:760px;">
                  <thead><tr><th></th><th>A pool_id</th><th>B 类型</th><th>C 名称</th><th>D 消耗</th><th>E 保底</th><th>F 启用</th><th>G 操作</th></tr></thead>
                  <tbody id="gachaPoolListBody">
                    <tr><td class="row-index">1</td><td colspan="7" class="muted">查询后这里显示所有卡池，点击“编辑池子”带入表单。</td></tr>
                  </tbody>
                </table>
              </div>
              <div class="sheet-table-wrap" style="max-height:360px;">
                <table class="sheet-table" style="min-width:960px;">
                  <thead><tr><th></th><th>A pool_id</th><th>B 类型</th><th>C 卡池</th><th>D item_key</th><th>E 星级</th><th>F 权重</th><th>G 概率</th><th>H UP</th><th>I 奖励</th><th>J 操作</th></tr></thead>
                  <tbody id="gachaConfigBody">
                    <tr><td class="row-index">1</td><td colspan="10" class="muted">保存或查询抽卡配置后会显示在这里。</td></tr>
                  </tbody>
                </table>
              </div>
            </div>
          </section>

 )HTML";

    static constexpr const char* kPart1_workbook_shop = R"HTML(
          <section class="sheet-detail" id="sec-shop-config">
            <h3>商城配置管理</h3>
            <div class="sheet-detail-body">
              <p class="muted">这里维护“商品是什么、卖多少钱、买后发什么”。UE 只读取商品和发起购买，不要在客户端写死奖励或本地扣货币。</p>
              <div class="field"><label for="shopProductId">product_id</label><input id="shopProductId" placeholder="daily_gem_pack_001 / gem_pack_1600" /></div>
              <div class="field"><label for="shopCategory">分类 category</label><input id="shopCategory" value="daily" placeholder="daily / pack / event" /></div>
              <div class="field"><label for="shopName">商品名称</label><input id="shopName" placeholder="1600 钻石礼包" /></div>
              <div class="field"><label for="shopDescription">商品描述</label><textarea id="shopDescription" placeholder="商品说明，展示给 UE 商城页。"></textarea></div>
              <div class="field"><label for="shopIcon">图标资源</label><input id="shopIcon" placeholder="icon_shop_gem_pack_1600 或 /Game/UI/Icons/T_ShopPack" /></div>
              <div class="field"><label for="shopPriceCurrency">价格货币</label><select id="shopPriceCurrency"><option value="gold">gold 金币</option><option value="gem">gem 钻石</option><option value="currency">其他货币，填写下方自定义 ID</option></select></div>
              <div class="field"><label for="shopPriceCurrencyCustom">自定义价格货币 ID</label><input id="shopPriceCurrencyCustom" placeholder="coin / ticket，gold/gem 可留空" /></div>
              <div class="field"><label for="shopPriceAmount">价格数量</label><input id="shopPriceAmount" type="number" value="1600" /></div>
              <div class="field"><label for="shopPurchaseLimit">限购次数</label><input id="shopPurchaseLimit" type="number" value="0" /></div>
              <div class="field"><label for="shopEnabled">是否上架</label><select id="shopEnabled"><option value="true">上架</option><option value="false">下架</option></select></div>
              <div class="field"><label for="shopRewardKind">奖励类型</label><select id="shopRewardKind"><option value="gold">金币 gold</option><option value="gem">钻石 gem</option><option value="currency">其他货币 currency</option><option value="item">道具 item</option><option value="attachment_template">附件模板</option></select></div>
              <div class="field"><label for="shopRewardItemSelect">从物品配置选择</label><select id="shopRewardItemSelect"><option value="">先加载物品列表</option></select></div>
              <div class="field"><label for="shopRewardResource">奖励资源 ID</label><input id="shopRewardResource" list="rewardItemOptions" placeholder="item_id / gem / currency_id" /></div>
              <div class="field"><label for="shopRewardAmount">奖励数量</label><input id="shopRewardAmount" type="number" value="1600" /></div>
              <div class="actions" style="margin-top:0;">
                <button class="ok" id="shopAddRewardBtn" type="button">添加奖励行</button>
                <button class="ghost" id="shopClearRewardsBtn" type="button">清空奖励</button>
                <button class="alt" id="shopLoadItemsBtn" type="button">加载物品列表</button>
              </div>
              <div class="sheet-table-wrap" style="max-height:180px;">
                <table class="sheet-table" style="min-width:560px;">
                  <thead><tr><th>类型</th><th>资源ID</th><th>数量</th><th>操作</th></tr></thead>
                  <tbody id="shopRewardBody">
                    <tr><td colspan="4" class="muted">还没有商品奖励，添加后会自动生成 JSON。</td></tr>
                  </tbody>
                </table>
              </div>
              <div class="field"><label for="shopRewardsJson">奖励 JSON（自动生成，也可手动改）</label><textarea id="shopRewardsJson">[
  {"kind":"currency","resource_id":"gem","amount":1600}
]</textarea></div>
              <div class="actions" style="margin-top:0;">
                <button class="ok" id="shopSaveBtn">保存商品</button>
                <button class="alt" id="shopQueryBtn">查询商品</button>
                <button class="danger" id="shopDeleteBtn">删除当前商品</button>
              </div>
              <div class="sheet-table-wrap" style="max-height:340px;">
                <table class="sheet-table" style="min-width:980px;">
                  <thead><tr><th></th><th>A product_id</th><th>B 分类</th><th>C 名称</th><th>D 价格</th><th>E 奖励</th><th>F 限购</th><th>G 上架</th><th>H 操作</th></tr></thead>
                  <tbody id="shopProductBody">
                    <tr><td class="row-index">1</td><td colspan="8" class="muted">保存或查询商城商品后会显示在这里。</td></tr>
                  </tbody>
                </table>
              </div>
            </div>
          </section>
)HTML";

    static constexpr const char* kPart1_workbook_after_shop = R"HTML(
        </aside>
      </div>
    </section>

    <div class="layout legacy-layout">
      <div class="stack">
        <div class="grid">
)HTML";

    static constexpr const char* kPart1_auth = R"HTML(
          <section class="card third" id="sec-auth">
            <div class="card-header">
              <h2 class="card-title">管理员登录</h2>
              <p class="card-note">先走 `/admin/login`，成功后把 bearer token 缓存在浏览器本地。</p>
            </div>
            <div class="card-body">
              <div class="form-grid one">
                <div class="field">
                  <label for="loginName">登录名</label>
                  <input id="loginName" value="admin_demo" />
                </div>
                <div class="field">
                  <label for="loginPassword">密码</label>
                  <input id="loginPassword" type="password" value="AdminDemo_123456" />
                </div>
              </div>
              <div class="actions">
                <button id="loginBtn">登录后台</button>
                <button class="ghost" id="logoutBtn">清空 Token</button>
                <button class="alt" id="meBtn">读取 /admin/me</button>
              </div>
            </div>
          </section>
)HTML";

    static constexpr const char* kPart1_query = R"HTML(
          <section class="card third">
            <div class="card-header">
              <h2 class="card-title">查账号 / 查玩家</h2>
              <p class="card-note">快速定位单个账号、玩家和背包，适合排查账号状态、玩家资产和基础资料。</p>
            </div>
            <div class="card-body">
              <div class="form-grid">
                <div class="field">
                  <label for="queryLoginName">登录名</label>
                  <input id="queryLoginName" placeholder="login_name" />
                </div>
                <div class="field">
                  <label for="queryAccountId">账号 ID</label>
                  <input id="queryAccountId" placeholder="account_id" />
                </div>
                <div class="field">
                  <label for="queryPlayerId">玩家 ID</label>
                  <input id="queryPlayerId" placeholder="player_id" />
                </div>
              </div>
              <div class="actions">
                <button class="alt" id="queryAccountBtn">查账号</button>
                <button class="alt" id="queryPlayerBtn">查玩家</button>
                <button class="ghost" id="queryInventoryBtn">查背包</button>
              </div>
            </div>
          </section>
)HTML";

    static constexpr const char* kPart1a = R"HTML(
          <section class="card third">
            <div class="card-header">
              <h2 class="card-title">封号 / 冻结</h2>
              <p class="card-note">复用 `/auth/admin/account-status`，可把账号设为 `active / frozen / banned`。</p>
            </div>
            <div class="card-body">
              <div class="form-grid one">
                <div class="field">
                  <label for="statusAccountId">账号 ID</label>
                  <input id="statusAccountId" placeholder="account_id" />
                </div>
                <div class="field">
                  <label for="statusValue">目标状态</label>
                  <select id="statusValue">
                    <option value="active">active</option>
                    <option value="frozen">frozen</option>
                    <option value="banned">banned</option>
                  </select>
                </div>
                <div class="field">
                  <label for="statusReason">原因</label>
                  <input id="statusReason" placeholder="例如：外挂 / 恶意刷号" />
                </div>
              </div>
              <div class="actions">
                <button class="warn" id="statusBtn">提交状态变更</button>
              </div>
            </div>
          </section>

          <section class="card wide" id="sec-player-list">
            <div class="card-header">
              <h2 class="card-title">玩家列表</h2>
              <p class="card-note">复用 `/admin/query/players`。主表按 Excel 风格固定展示 `player_id / account_id / nickname / gold / gem / 状态 / 禁言到期 / 最后登录时间 / 操作`。</p>
            </div>
            <div class="card-body">
              <div class="form-grid">
                <div class="field">
                  <label for="playersPlayerId">player_id 模糊过滤</label>
                  <input id="playersPlayerId" placeholder="player_" />
                </div>
                <div class="field">
                  <label for="playersAccountId">account_id 模糊过滤</label>
                  <input id="playersAccountId" placeholder="acc_" />
                </div>
                <div class="field">
                  <label for="playersNickname">nickname 模糊过滤</label>
                  <input id="playersNickname" placeholder="昵称关键字" />
                </div>
                <div class="field">
                  <label for="playersLimit">limit</label>
                  <input id="playersLimit" type="number" value="20" />
                </div>
                <div class="field">
                  <label for="playersOffset">offset</label>
                  <input id="playersOffset" type="number" value="0" />
                </div>
              </div>
              <div class="actions">
                <button class="alt" id="playersQueryBtn">查询玩家列表</button>
                <button class="ghost" id="playersNextBtn">下一页</button>
                <button class="ghost" id="playersPrevBtn">上一页</button>
              </div>
)HTML";

    static constexpr const char* kPart1b_gm = R"HTML(
          <div class="sheet-box" id="sec-gm-workbench">
            <div class="sheet-head">
              <strong>GM Excel 操作台</strong>
              <span>先查玩家列表，再像表格一样勾选行，统一处理禁言、封号、邮件和全服邮件。</span>
            </div>
              <div class="gm-workbench">
                <div class="excel-card">
                  <div class="excel-title">
                    <span>表格</span>
                    <span class="muted">玩家行勾选后可批量操作</span>
                  </div>
                  <div class="excel-grid">
                    <div class="excel-cell head"></div>
                    <div class="excel-cell head">A</div>
                    <div class="excel-cell head">B</div>
                    <div class="excel-cell head">C</div>
                    <div class="excel-cell head">D</div>

                    <div class="excel-cell index">1</div>
                    <div class="excel-cell">玩家查询</div>
                    <div class="excel-cell">分页筛选</div>
                    <div class="excel-cell">勾选目标</div>
                    <div class="excel-cell cmd">
                      <button class="alt" id="gmQueryPlayersBtn">刷新玩家表</button>
                      <button class="ghost" id="gmClearSelectionBtn">清空勾选</button>
                    </div>

                    <div class="excel-cell index">2</div>
                    <div class="excel-cell">聊天管控</div>
                    <div class="excel-cell">大厅禁言</div>
                    <div class="excel-cell">使用下方禁言秒数和原因</div>
                    <div class="excel-cell cmd">
                      <button class="warn" id="gmMuteSelectedBtn">禁言勾选玩家</button>
                    </div>

                    <div class="excel-cell index">3</div>
                    <div class="excel-cell">账号管控</div>
                    <div class="excel-cell">冻结/封禁/解封</div>
                    <div class="excel-cell">使用下方状态和原因</div>
                    <div class="excel-cell cmd">
                      <button class="danger" id="gmStatusSelectedBtn">改勾选账号状态</button>
                    </div>

                    <div class="excel-cell index">4</div>
                    <div class="excel-cell">邮件奖励</div>
                    <div class="excel-cell">勾选/全服</div>
                    <div class="excel-cell">使用邮件标题、正文和奖励 JSON</div>
                    <div class="excel-cell cmd">
                      <button class="ok" id="gmMailSelectedBtn">发给勾选玩家</button>
                      <button class="danger" id="gmMailAllBtn">一键发给全部玩家</button>
                    </div>
                  </div>
                </div>

                <div class="gm-panel">
                  <h3>当前批量目标</h3>
                  <div class="gm-kpi">
                    <div><span class="muted">玩家</span><strong id="gmSelectedPlayerCount">0</strong></div>
                    <div><span class="muted">邮件</span><strong id="gmSelectedMailCount">0</strong></div>
                    <div><span class="muted">审计</span><strong id="gmSelectedAuditCount">0</strong></div>
                  </div>
                  <div class="field">
                    <label for="gmMailMode">邮件目标模式</label>
                    <select id="gmMailMode">
                      <option value="selected">勾选玩家</option>
                      <option value="all">全部玩家</option>
                    </select>
                  </div>
                  <div class="field">
                    <label for="gmNote">操作备注</label>
                    <input id="gmNote" placeholder="例如：春节补偿 / 刷屏处罚 / 外挂封禁" />
                  </div>
                  <div class="hint">全服邮件会先查询当前玩家表里的全部 player_id，再调用 `/mail/send` 批量发送。数据量很大时应升级成后台任务。</div>
                </div>
              </div>
          </div>
)HTML";

    static constexpr const char* kPart1b = R"HTML(
              <div class="sheet-box">
                <div class="sheet-head">
                  <strong>表格视图控制</strong>
                  <span>这一块只作用于当前页玩家主表，尽量接近 Excel 的本页排序和筛选。</span>
                </div>
                <div class="form-grid">
                  <div class="field">
                    <label for="playersSheetKeyword">本页关键字筛选</label>
                    <input id="playersSheetKeyword" placeholder="player_id / account_id / nickname" />
                  </div>
                  <div class="field">
                    <label for="playersSheetStatus">本页状态筛选</label>
                    <select id="playersSheetStatus">
                      <option value="">全部</option>
                      <option value="active">active</option>
                      <option value="frozen">frozen</option>
                      <option value="banned">banned</option>
                    </select>
                  </div>
                  <div class="field">
                    <label for="playersSheetMutedOnly">本页禁言筛选</label>
                    <select id="playersSheetMutedOnly">
                      <option value="">全部</option>
                      <option value="muted">只看已禁言</option>
                      <option value="unmuted">只看未禁言</option>
                    </select>
                  </div>
                </div>
                <div class="actions">
                  <button class="ghost" id="playersSheetApplyBtn">应用本页筛选</button>
                  <button class="ghost" id="playersSheetResetBtn">重置本页筛选</button>
                </div>
              </div>
              <div class="sheet-box">
                <div class="sheet-head">
                  <strong>批量快捷编辑</strong>
                  <span>基于当前勾选玩家，直接批量改账号状态或大厅禁言。</span>
                </div>
                <div class="form-grid">
                  <div class="field">
                    <label for="playersQuickStatusValue">批量状态</label>
                    <select id="playersQuickStatusValue">
                      <option value="frozen">frozen</option>
                      <option value="banned">banned</option>
                      <option value="active">active</option>
                    </select>
                  </div>
                  <div class="field">
                    <label for="playersQuickStatusReason">批量状态原因</label>
                    <input id="playersQuickStatusReason" placeholder="例如：外挂批量封禁" />
                  </div>
                  <div class="field">
                    <label for="playersQuickMuteScope">大厅 scope_id</label>
                    <input id="playersQuickMuteScope" value="global" />
                  </div>
                  <div class="field">
                    <label for="playersQuickMuteSeconds">大厅禁言秒数</label>
                    <input id="playersQuickMuteSeconds" type="number" value="3600" />
                  </div>
                  <div class="field span-2">
                    <label for="playersQuickMuteReason">大厅禁言原因</label>
                    <input id="playersQuickMuteReason" placeholder="例如：大厅刷屏处理" />
                  </div>
                </div>
                <div class="actions">
                  <button class="warn" id="playersQuickApplyStatusBtn">对勾选玩家批量改状态</button>
                  <button class="warn" id="playersQuickApplyMuteBtn">对勾选玩家批量大厅禁言</button>
                </div>
              </div>
            </div>
          </section>
)HTML";

    static constexpr const char* kPart2 = R"HTML(
          <section class="card wide">
            <div class="card-header">
              <h2 class="card-title">禁言与公告</h2>
              <p class="card-note">这里覆盖 `/chat/mute` 和 `/chat/gm/announce`。频道支持大厅、房间、好友私聊。</p>
            </div>
            <div class="card-body">
              <div class="form-grid">
                <div class="field">
                  <label for="muteKind">禁言 channel_kind</label>
                  <select id="muteKind">
                    <option value="lobby">lobby</option>
                    <option value="room">room</option>
                    <option value="friend_direct">friend_direct</option>
                  </select>
                </div>
                <div class="field">
                  <label for="muteScope">禁言 scope_id</label>
                  <input id="muteScope" placeholder="大厅可留空，房间填 room_1001" />
                </div>
                <div class="field">
                  <label for="mutePlayerA">禁言玩家 A</label>
                  <input id="mutePlayerA" placeholder="好友私聊时使用" />
                </div>
                <div class="field">
                  <label for="mutePlayerB">禁言玩家 B</label>
                  <input id="mutePlayerB" placeholder="好友私聊时使用" />
                </div>
                <div class="field">
                  <label for="mutedPlayerId">muted_player_id</label>
                  <input id="mutedPlayerId" placeholder="要禁言的 player_id" />
                </div>
                <div class="field">
                  <label for="muteDuration">duration_seconds</label>
                  <input id="muteDuration" type="number" value="3600" />
                </div>
                <div class="field span-2">
                  <label for="muteReason">禁言原因</label>
                  <input id="muteReason" placeholder="例如：刷屏 / 敏感词 / 骚扰" />
                </div>
                <div class="field">
                  <label for="announceKind">公告 channel_kind</label>
                  <select id="announceKind">
                    <option value="lobby">lobby</option>
                    <option value="room">room</option>
                    <option value="friend_direct">friend_direct</option>
                  </select>
                </div>
                <div class="field">
                  <label for="announceScope">公告 scope_id</label>
                  <input id="announceScope" placeholder="global / room_1001" />
                </div>
                <div class="field">
                  <label for="announcePlayerA">公告玩家 A</label>
                  <input id="announcePlayerA" placeholder="好友频道时使用" />
                </div>
                <div class="field">
                  <label for="announcePlayerB">公告玩家 B</label>
                  <input id="announcePlayerB" placeholder="好友频道时使用" />
                </div>
                <div class="field span-2">
                  <label for="announceText">公告内容</label>
                  <textarea id="announceText" placeholder="例如：服务器将在 22:00 维护，请提前下线。"></textarea>
                </div>
              </div>
              <div class="actions">
                <button class="warn" id="muteBtn">提交禁言</button>
                <button class="ok" id="unmuteBtn">取消禁言</button>
                <button class="ok" id="announceBtn">发送公告</button>
              </div>
            </div>
          </section>

          <section class="card wide">
            <div class="card-header">
              <h2 class="card-title">发系统邮件</h2>
              <p class="card-note">复用 `/mail/send`。支持单人、批量和全服；奖励可以用表单逐行添加，不需要手写 JSON。</p>
            </div>
            <div class="card-body">
              <div class="form-grid">
                <div class="field">
                  <label for="mailPlayerId">单人 player_id</label>
                  <input id="mailPlayerId" placeholder="10000001" />
                </div>
                <div class="field">
                  <label for="mailPlayerIds">批量 player_ids</label>
                  <input id="mailPlayerIds" placeholder="10000001,10000002,10000003" />
                </div>
                <div class="field">
                  <label for="mailSender">sender_name</label>
                  <input id="mailSender" value="System" />
                </div>
                <div class="field">
                  <label for="mailExpire">expires_in_seconds</label>
                  <input id="mailExpire" type="number" value="604800" />
                </div>
                <div class="field span-2">
                  <label for="mailTitle">标题</label>
                  <input id="mailTitle" placeholder="维护补偿 / 活动奖励" />
                </div>
                <div class="field span-2">
                  <label for="mailBody">正文</label>
                  <textarea id="mailBody" placeholder="请输入邮件正文"></textarea>
                </div>
                <div class="field span-2">
                  <label>可视化奖励附件</label>
                  <div class="form-grid">
                    <div class="field">
                      <label for="mailRewardKind">奖励类型</label>
                      <select id="mailRewardKind">
                        <option value="gold">金币 gold</option>
                        <option value="gem">钻石 gem</option>
                        <option value="currency">其他货币 currency</option>
                        <option value="item">道具 item</option>
                        <option value="attachment_template">附件模板</option>
                      </select>
                    </div>
                    <div class="field">
                      <label for="mailRewardResource">资源 ID</label>
                      <input id="mailRewardResource" list="rewardItemOptions" placeholder="item_id / currency_id，gold/gem 可留空" />
                    </div>
                    <div class="field">
                      <label for="mailRewardAmount">数量</label>
                      <input id="mailRewardAmount" type="number" value="1000" min="1" />
                    </div>
                    <div class="field">
                      <label>操作</label>
                      <button class="ok" id="mailAddRewardBtn" type="button">添加奖励行</button>
                      <button class="ghost" id="mailClearRewardsBtn" type="button">清空</button>
                    </div>
                  </div>
                  <div class="sheet-table-wrap" style="max-height:180px; margin-top:10px;">
                    <table class="sheet-table" style="min-width:560px;">
                      <thead><tr><th>类型</th><th>资源ID</th><th>数量</th><th>操作</th></tr></thead>
                      <tbody id="mailRewardBody"><tr><td colspan="4" class="muted">还没有奖励，添加后会自动生成 JSON。</td></tr></tbody>
                    </table>
                  </div>
                </div>
                <div class="field span-2">
                  <label for="mailRewards">rewards JSON（自动生成，也可手动改）</label>
                  <textarea id="mailRewards">[
  {"kind":"gold","amount":1000},
  {"kind":"item","resource_id":"item_potion_small","amount":5}
]</textarea>
                </div>
                <datalist id="rewardItemOptions"></datalist>
              </div>
              <div class="actions">
                <button class="ok" id="mailBtn">发送邮件</button>
                <button class="danger" id="mailAllBtn">一键发送给全部玩家</button>
              </div>
            </div>
          </section>

)HTML";

    static constexpr const char* kPart2b = R"HTML(
          <section class="card wide" id="sec-mail-query">
            <div class="card-header">
              <h2 class="card-title">邮件列表 / 奖励流水</h2>
              <p class="card-note">复用 `/admin/query/mail` 和 `/admin/query/reward-grants`。两类数据都支持分页，奖励流水还支持 source_type / source_id 筛选。</p>
            </div>
            <div class="card-body">
              <div class="form-grid">
                <div class="field">
                  <label for="mailQueryPlayerId">邮件 player_id</label>
                  <input id="mailQueryPlayerId" placeholder="player_id" />
                </div>
                <div class="field">
                  <label for="mailQueryIncludeDeleted">include_deleted</label>
                  <select id="mailQueryIncludeDeleted">
                    <option value="false">false</option>
                    <option value="true">true</option>
                  </select>
                </div>
                <div class="field">
                  <label for="mailQueryClaimed">claimed</label>
                  <select id="mailQueryClaimed">
                    <option value="">全部</option>
                    <option value="true">true</option>
                    <option value="false">false</option>
                  </select>
                </div>
                <div class="field">
                  <label for="mailQueryRead">read</label>
                  <select id="mailQueryRead">
                    <option value="">全部</option>
                    <option value="true">true</option>
                    <option value="false">false</option>
                  </select>
                </div>
                <div class="field">
                  <label for="mailQueryLimit">邮件 limit</label>
                  <input id="mailQueryLimit" type="number" value="20" />
                </div>
                <div class="field">
                  <label for="mailQueryOffset">邮件 offset</label>
                  <input id="mailQueryOffset" type="number" value="0" />
                </div>
                <div class="field">
                  <label for="grantPlayerId">奖励流水 player_id</label>
                  <input id="grantPlayerId" placeholder="player_id" />
                </div>
                <div class="field">
                  <label for="grantSourceType">source_type</label>
                  <input id="grantSourceType" placeholder="mail / gm / activity" />
                </div>
                <div class="field">
                  <label for="grantSourceId">source_id</label>
                  <input id="grantSourceId" placeholder="mail_id / task_id / event_id" />
                </div>
                <div class="field">
                  <label for="grantLimit">奖励 limit</label>
                  <input id="grantLimit" type="number" value="20" />
                </div>
                <div class="field">
                  <label for="grantOffset">奖励 offset</label>
                  <input id="grantOffset" type="number" value="0" />
                </div>
              </div>
              <div class="actions">
                <button class="alt" id="mailQueryBtn">查询邮件列表</button>
                <button class="alt" id="grantQueryBtn">查询奖励流水</button>
              </div>
            </div>
          </section>

          <section class="card wide" id="sec-item-config-legacy">
            <div class="card-header">
              <h2 class="card-title">物品配置管理</h2>
              <p class="card-note">复用 `/admin/items/upsert` 和 `/admin/items/query`。这里维护“物品是什么”，玩家拥有数量仍由背包/发奖接口控制。</p>
            </div>
            <div class="card-body">
              <div class="form-grid">
                <div class="field">
                  <label for="legacyitemId">item_id</label>
                  <input id="legacyitemId" placeholder="item_potion_small" />
                </div>
                <div class="field">
                  <label for="legacyitemName">物品名称</label>
                  <input id="legacyitemName" placeholder="小型生命药水" />
                </div>
                <div class="field span-2">
                  <label for="legacyitemDescription">物品描述</label>
                  <textarea id="legacyitemDescription" placeholder="恢复少量生命值。"></textarea>
                </div>
                <div class="field">
                  <label for="legacyitemIcon">图标资源</label>
                  <input id="legacyitemIcon" placeholder="icons/items/potion_small.png 或 /Game/UI/Icons/T_PotionSmall" />
                </div>
                <div class="field">
                  <label for="legacyitemRarity">稀有度</label>
                  <select id="legacyitemRarity">
                    <option value="3星">3星</option>
                    <option value="4星">4星</option>
                    <option value="5星">5星</option>
                  </select>
                </div>
                <div class="field">
                  <label for="legacyitemMaxStack">最大堆叠</label>
                  <input id="legacyitemMaxStack" type="number" value="99" />
                </div>
                <div class="field">
                  <label for="legacyitemType">物品类型</label>
                  <input id="legacyitemType" value="material" placeholder="material / consumable / equipment" />
                </div>
                <div class="field">
                  <label for="legacyitemEnabled">是否启用</label>
                  <select id="legacyitemEnabled">
                    <option value="true">启用</option>
                    <option value="false">停用</option>
                  </select>
                </div>
                <div class="field">
                  <label for="legacyitemQueryRarity">查询稀有度</label>
                  <select id="legacyitemQueryRarity">
                    <option value="">全部</option>
                    <option value="3星">3星</option>
                    <option value="4星">4星</option>
                    <option value="5星">5星</option>
                  </select>
                </div>
                <div class="field">
                  <label for="legacyitemQueryEnabled">查询启用状态</label>
                  <select id="legacyitemQueryEnabled">
                    <option value="">全部</option>
                    <option value="true">启用</option>
                    <option value="false">停用</option>
                  </select>
                </div>
              </div>
              <div class="actions">
                <button class="ok" id="legacyitemSaveBtn">保存物品配置</button>
                <button class="alt" id="legacyitemQueryBtn">查询物品配置</button>
              </div>
            </div>
          </section>

          <section class="card wide" id="sec-chat-query">
            <div class="card-header">
              <h2 class="card-title">聊天记录 / 封禁记录</h2>
              <p class="card-note">复用 `/admin/query/chat-history` 和 `/admin/query/blacklists`。聊天记录按频道查询，封禁记录支持最近窗口查看。</p>
            </div>
            <div class="card-body">
              <div class="form-grid">
                <div class="field">
                  <label for="chatQueryKind">聊天 channel_kind</label>
                  <select id="chatQueryKind">
                    <option value="lobby">lobby</option>
                    <option value="room">room</option>
                    <option value="friend_direct">friend_direct</option>
                  </select>
                </div>
                <div class="field">
                  <label for="chatQueryScope">聊天 scope_id</label>
                  <input id="chatQueryScope" placeholder="global / room_1001" />
                </div>
                <div class="field">
                  <label for="chatQueryPlayerA">聊天玩家 A</label>
                  <input id="chatQueryPlayerA" placeholder="好友频道时使用" />
                </div>
                <div class="field">
                  <label for="chatQueryPlayerB">聊天玩家 B</label>
                  <input id="chatQueryPlayerB" placeholder="好友频道时使用" />
                </div>
                <div class="field">
                  <label for="chatQueryLimit">聊天 limit</label>
                  <input id="chatQueryLimit" type="number" value="50" />
                </div>
                <div class="field">
                  <label for="blacklistLimit">封禁记录 limit</label>
                  <input id="blacklistLimit" type="number" value="50" />
                </div>
              </div>
              <div class="actions">
                <button class="alt" id="chatQueryBtn">查询聊天记录</button>
                <button class="danger" id="blacklistQueryBtn">查询封禁记录</button>
              </div>
            </div>
          </section>
)HTML";

    static constexpr const char* kPart3 = R"HTML(
          <section class="card wide" id="sec-audit-query">
            <div class="card-header">
              <h2 class="card-title">审计日志 / 管理员列表 / 任务列表</h2>
              <p class="card-note">复用 `/admin/audit-logs/query`、`/admin/users/query`、`/admin/tasks/query`。适合查谁做了什么、有哪些管理员、最近有哪些后台任务。</p>
            </div>
            <div class="card-body">
              <div class="form-grid">
                <div class="field">
                  <label for="auditAction">audit action</label>
                  <input id="auditAction" placeholder="account_status_change / gm_announce ..." />
                </div>
                <div class="field">
                  <label for="auditOperatorId">audit operator_id</label>
                  <input id="auditOperatorId" placeholder="operator_name" />
                </div>
                <div class="field">
                  <label for="auditSuccess">audit success</label>
                  <select id="auditSuccess">
                    <option value="">全部</option>
                    <option value="true">true</option>
                    <option value="false">false</option>
                  </select>
                </div>
                <div class="field">
                  <label for="auditLimit">audit limit</label>
                  <input id="auditLimit" type="number" value="20" />
                </div>
                <div class="field">
                  <label for="auditOffset">audit offset</label>
                  <input id="auditOffset" type="number" value="0" />
                </div>
                <div class="field">
                  <label for="adminUserName">管理员 operator_name</label>
                  <input id="adminUserName" placeholder="operator_name 关键字" />
                </div>
                <div class="field">
                  <label for="adminUserActive">管理员 active</label>
                  <select id="adminUserActive">
                    <option value="">全部</option>
                    <option value="true">true</option>
                    <option value="false">false</option>
                  </select>
                </div>
                <div class="field">
                  <label for="adminUserLimit">管理员 limit</label>
                  <input id="adminUserLimit" type="number" value="20" />
                </div>
                <div class="field">
                  <label for="adminUserOffset">管理员 offset</label>
                  <input id="adminUserOffset" type="number" value="0" />
                </div>
                <div class="field">
                  <label for="taskKind">任务 kind</label>
                  <select id="taskKind">
                    <option value="">全部</option>
                    <option value="announce">announce</option>
                    <option value="batch_reward">batch_reward</option>
                    <option value="batch_mail">batch_mail</option>
                    <option value="batch_mute">batch_mute</option>
                    <option value="batch_account_status">batch_account_status</option>
                  </select>
                </div>
                <div class="field">
                  <label for="taskStatus">任务 status</label>
                  <input id="taskStatus" placeholder="scheduled / running / completed / failed" />
                </div>
                <div class="field">
                  <label for="taskLimit">任务 limit</label>
                  <input id="taskLimit" type="number" value="20" />
                </div>
                <div class="field">
                  <label for="taskOffset">任务 offset</label>
                  <input id="taskOffset" type="number" value="0" />
                </div>
                <div class="field">
                  <label for="traceQueryId">trace_id</label>
                  <input id="traceQueryId" placeholder="可选，留空表示看最近 traces" />
                </div>
                <div class="field">
                  <label for="traceQueryLimit">trace limit</label>
                  <input id="traceQueryLimit" type="number" value="100" />
                </div>
              </div>
              <div class="actions">
                <button class="alt" id="auditQueryBtn">查询审计日志</button>
                <button class="alt" id="adminUsersQueryBtn">查询管理员列表</button>
                <button class="alt" id="tasksQueryBtn">查询任务列表</button>
                <button class="alt" id="traceQueryBtn">查询 Tracing</button>
                <button class="warn" id="loadQueryBtn">查询负载状态</button>
                <button class="ghost" id="tasksFailedBtn">只看失败任务</button>
                <button class="ghost" id="tasksClearBtn">清空任务筛选</button>
              </div>
            </div>
          </section>
          <section class="card wide" id="sec-tracing">
            <div class="card-header">
              <h2 class="card-title">Tracing 查看</h2>
              <p class="card-note">复用 `/debug/traces`。列表先按 trace 聚合，再把 span 树和属性渲染到右侧卡片，方便直接排查一条请求或后台任务的完整链路。</p>
            </div>
            <div class="card-body">
              <div class="form-grid">
                <div class="field span-2">
                  <label for="traceViewerId">快速带入 trace_id</label>
                  <input id="traceViewerId" placeholder="可手填，也可从任务/列表动作带入" />
                </div>
              </div>
              <div class="actions">
                <button class="alt" id="traceViewerApplyBtn">带入到筛选</button>
                <button class="ghost" id="traceViewerOpenBtn">打开当前 Trace</button>
              </div>
            </div>
          </section>
          <section class="card wide" id="sec-debug-load">
            <div class="card-header">
              <h2 class="card-title">负载调试</h2>
              <p class="card-note">复用 `/debug/load`。这里直接看当前 inflight、业务队列积压，以及 429/503 两类拒绝原因累计值，方便判断背压是因为“在途太多”还是“业务队列打满”。</p>
            </div>
            <div class="card-body">
              <div class="actions">
                <button class="warn" id="loadCardQueryBtn">刷新负载状态</button>
              </div>
            </div>
          </section>
          <section class="card wide" id="sec-debug-announcement-cache">
            <div class="card-header">
              <h2 class="card-title">公告缓存调试</h2>
              <p class="card-note">复用 `/debug/announcement-cache`。直接查看公告 Redis 最近窗口的 key、预热状态和当前缓存内容，方便确认是否命中缓存。</p>
            </div>
            <div class="card-body">
              <div class="form-grid">
                <div class="field">
                  <label for="announcementCacheKind">channel_kind</label>
                  <select id="announcementCacheKind">
                    <option value="lobby">lobby</option>
                    <option value="room">room</option>
                    <option value="friend_direct">friend_direct</option>
                  </select>
                </div>
                <div class="field">
                  <label for="announcementCacheScope">scope_id</label>
                  <input id="announcementCacheScope" value="global" placeholder="global / room_1001 / 10000001:10000002" />
                </div>
                <div class="field">
                  <label for="announcementCacheLimit">limit</label>
                  <input id="announcementCacheLimit" type="number" value="5" />
                </div>
              </div>
              <div class="actions">
                <button class="warn" id="announcementCacheQueryBtn">刷新公告缓存</button>
              </div>
            </div>
          </section>
        </div>
      </div>

      <aside class="stack">
        <section class="side-card">
          <div class="side-head">
            <h2>当前响应</h2>
            <p>这里显示最近一次接口原始返回，方便你直接 review 协议结构。</p>
          </div>
          <div class="side-body">
            <pre class="output" id="output">等待操作...</pre>
            <div class="hint">提示：登录成功后会把 token 保存在浏览器 <code>localStorage</code> 的 <code>hello_admin_token</code> 下。</div>
          </div>
        </section>

        <section class="side-card">
          <div class="side-head">
            <h2>结果概览</h2>
            <p>把常见列表接口的 total 和当前页条数提炼出来，减少来回翻 JSON。</p>
          </div>
          <div class="side-body">
            <div class="summary">
              <strong id="summaryTitle">暂无查询</strong>
              <div class="muted" id="summaryMeta">等待操作</div>
            </div>
            <div class="batch-box">
              <strong>批量操作</strong>
              <div class="batch-count" id="selectedCount">0</div>
              <div class="muted" id="selectedPreview">还没有勾选玩家</div>
              <div class="actions" style="margin-top:0;">
                <button class="ghost" id="clearSelectionBtn">清空勾选</button>
                <button class="alt" id="fillSelectedMailBtn">带入批量邮件</button>
                <button class="ghost" id="fillSelectedAuditBtn">带入审计筛选</button>
              </div>
              <div class="field">
                <label for="batchRewardSourceType">批量发奖 source_type</label>
                <input id="batchRewardSourceType" value="admin_batch" />
              </div>
              <div class="field">
                <label for="batchRewardSourceId">批量发奖 source_id</label>
                <input id="batchRewardSourceId" placeholder="campaign_001 / compensation_20260604" />
              </div>
              <div class="field">
                <label for="batchRewardPayload">批量发奖 rewards JSON</label>
                <textarea id="batchRewardPayload">[
  {"kind":"gold","amount":500},
  {"kind":"item","resource_id":"item_potion_small","amount":2}
]</textarea>
              </div>
              <div class="actions" style="margin-top:0;">
                <button class="ok" id="batchRewardBtn">对勾选玩家批量发奖</button>
              </div>
              <div class="field">
                <label for="batchMuteDuration">批量禁言 duration_seconds</label>
                <input id="batchMuteDuration" type="number" value="3600" />
              </div>
              <div class="field">
                <label for="batchMuteReason">批量禁言 reason</label>
                <input id="batchMuteReason" placeholder="例如：批量刷屏处理" />
              </div>
              <div class="actions" style="margin-top:0;">
                <button class="warn" id="batchMuteBtn">对勾选玩家批量禁言</button>
              </div>
              <div class="field">
                <label for="batchAccountStatus">批量账号状态</label>
                <select id="batchAccountStatus">
                  <option value="frozen">frozen</option>
                  <option value="banned">banned</option>
                  <option value="active">active</option>
                </select>
              </div>
              <div class="field">
                <label for="batchAccountReason">批量封号/解封 reason</label>
                <input id="batchAccountReason" placeholder="例如：外挂批量封禁 / 误封恢复" />
              </div>
              <div class="actions" style="margin-top:0;">
                <button class="danger" id="batchAccountStatusBtn">对勾选玩家批量改账号状态</button>
              </div>
              <div class="hint">勾选支持玩家列表、邮件列表、审计日志。不同表格的勾选会在这里汇总。</div>
            </div>
            <div class="batch-box">
              <strong>筛选预设</strong>
              <div class="muted">把当前玩家/邮件/奖励/审计筛选条件保存到浏览器本地，方便重复排查。</div>
              <div class="actions" style="margin-top:0;">
                <button class="ghost" id="saveFiltersBtn">保存当前筛选</button>
                <button class="alt" id="loadFiltersBtn">恢复筛选</button>
                <button class="ghost" id="clearFiltersBtn">清空预设</button>
              </div>
            </div>
            <div class="detail-card" id="detailCard">
              <div class="detail-head">
                <strong id="detailTitle">单对象卡片</strong>
                <div class="muted" id="detailMeta">当接口返回单个账号、玩家、背包时，会在这里做结构化展示。</div>
              </div>
              <div class="detail-actions" id="detailActions"></div>
              <div class="detail-grid" id="detailGrid">
                <div class="detail-item">
                  <span class="k">状态</span>
                  <div class="v">等待查询</div>
                </div>
              </div>
              <div class="detail-extra" id="detailExtra"></div>
            </div>
            <div class="hint" id="tableHint">这里会根据查询类型切换成更适合 review 的列。</div>
            <div class="table-wrap">
              <table id="resultTable">
                <thead>
                  <tr id="resultHead">
                    <th>等待查询</th>
                  </tr>
                </thead>
                <tbody id="resultBody">
                  <tr><td class="muted">这里会渲染数组结果的前几列预览。</td></tr>
                </tbody>
              </table>
            </div>
          </div>
        </section>
      </aside>
    </div>
  </div>

  <script>
)HTML";

    static constexpr const char* kPart3_state = R"HTML(
    const state = {
      baseUrl: window.location.origin,
      token: localStorage.getItem("hello_admin_token") || "",
      adminName: localStorage.getItem("hello_admin_name") || "",
      lastAction: "无",
      selectedPlayers: [],
      selectedMails: [],
      selectedAudits: [],
      traceSpans: [],
      lastTableTitle: "",
      lastTablePayload: null,
      sheetPlayers: [],
      sheetSelectedPlayer: null,
      itemConfigs: [],
      gachaItemOptions: [],
      gachaConfigs: [],
      shopProducts: [],
      rewardRows: {
        mail: [],
        sheet: [],
        shop: []
      },
      playerSheet: {
        sortKey: "player_id",
        sortDir: "asc"
      }
    };

    const $ = (id) => document.getElementById(id);

    function refreshHeader() {
      $("baseState").textContent = state.baseUrl;
      $("tokenState").textContent = state.token ? state.token.slice(0, 48) + (state.token.length > 48 ? "..." : "") : "未登录";
      $("adminState").textContent = state.adminName || "未知";
      $("queryState").textContent = state.lastAction;
    }

    function pretty(value) {
      return typeof value === "string" ? value : JSON.stringify(value, null, 2);
    }

    function setOutput(title, value) {
      state.lastAction = title;
      refreshHeader();
      if ($("sheetStatusLine")) {
        $("sheetStatusLine").textContent = title;
      }
      $("output").textContent = title + "\n\n" + pretty(value);
    }

    function setSummary(title, meta) {
      $("summaryTitle").textContent = title;
      $("summaryMeta").textContent = meta;
    }

    function syncSelectionView() {
      const total = state.selectedPlayers.length + state.selectedMails.length + state.selectedAudits.length;
      $("selectedCount").textContent = String(total);
      if ($("gmSelectedPlayerCount")) $("gmSelectedPlayerCount").textContent = String(state.selectedPlayers.length);
      if ($("gmSelectedMailCount")) $("gmSelectedMailCount").textContent = String(state.selectedMails.length);
      if ($("gmSelectedAuditCount")) $("gmSelectedAuditCount").textContent = String(state.selectedAudits.length);
      const parts = [];
      if (state.selectedPlayers.length) {
        parts.push("玩家 " + state.selectedPlayers.length);
      }
      if (state.selectedMails.length) {
        parts.push("邮件 " + state.selectedMails.length);
      }
      if (state.selectedAudits.length) {
        parts.push("审计 " + state.selectedAudits.length);
      }
      $("selectedPreview").textContent = parts.length
        ? parts.join(" / ")
        : "还没有勾选玩家、邮件或审计记录";
    }

    function addSelectedPlayer(playerId) {
      if (!playerId || state.selectedPlayers.includes(playerId)) {
        return;
      }
      state.selectedPlayers.push(playerId);
      syncSelectionView();
    }

    function removeSelectedPlayer(playerId) {
      state.selectedPlayers = state.selectedPlayers.filter((value) => value !== playerId);
      syncSelectionView();
    }

    function setSelectedPlayer(playerId, checked) {
      if (checked) {
        addSelectedPlayer(playerId);
      } else {
        removeSelectedPlayer(playerId);
      }
    }

    function parseBatchRewardsOrThrow() {
      const raw = $("batchRewardPayload").value.trim();
      if (!raw) {
        return [];
      }
      return JSON.parse(raw);
    }

    function normalizeRewardRow(row) {
      const requestedKind = row?.kind || "gold";
      const kind = requestedKind === "gem" ? "currency" : requestedKind;
      const resourceId = requestedKind === "gem" ? "gem" : (row?.resource_id || "");
      const amount = Number(row?.amount || 0);
      const reward = { kind, amount: amount > 0 ? amount : 1 };
      if (resourceId) {
        reward.resource_id = resourceId;
      }
      return reward;
    }

    function syncRewardJson(prefix) {
      const jsonId = prefix === "sheet" ? "sheetMailRewards" : (prefix === "shop" ? "shopRewardsJson" : "mailRewards");
      if ($(jsonId)) {
        $(jsonId).value = JSON.stringify(state.rewardRows[prefix].map(normalizeRewardRow), null, 2);
      }
    }

    function renderRewardRows(prefix) {
      const bodyId = prefix === "sheet" ? "sheetRewardBody" : (prefix === "shop" ? "shopRewardBody" : "mailRewardBody");
      const rows = state.rewardRows[prefix] || [];
      if (!$(bodyId)) {
        return;
      }
      if (!rows.length) {
        $(bodyId).innerHTML = "<tr><td colspan=\"4\" class=\"muted\">还没有奖励，添加后会自动生成 JSON。</td></tr>";
        syncRewardJson(prefix);
        return;
      }
      $(bodyId).innerHTML = rows.map((row, index) => {
        const normalized = normalizeRewardRow(row);
        return "<tr>" +
          "<td>" + escapeHtml(row.kind === "gem" ? "gem(钻石)" : (normalized.kind || "")) + "</td>" +
          "<td><code>" + escapeHtml(normalized.resource_id || "") + "</code></td>" +
          "<td>" + escapeHtml(normalized.amount || 0) + "</td>" +
          "<td><button class=\"danger\" type=\"button\" data-action=\"remove-reward-row\" data-prefix=\"" + prefix + "\" data-index=\"" + index + "\">删除</button></td>" +
          "</tr>";
      }).join("");
      syncRewardJson(prefix);
    }

    function addRewardFromInputs(prefix) {
      const kindId = prefix === "sheet" ? "sheetRewardKind" : (prefix === "shop" ? "shopRewardKind" : "mailRewardKind");
      const resourceId = prefix === "sheet" ? "sheetRewardResource" : (prefix === "shop" ? "shopRewardResource" : "mailRewardResource");
      const amountId = prefix === "sheet" ? "sheetRewardAmount" : (prefix === "shop" ? "shopRewardAmount" : "mailRewardAmount");
      const kind = $(kindId).value;
      let resource = $(resourceId).value.trim();
      const amount = Number($(amountId).value || 1);
      if (amount <= 0) {
        throw { status: 400, data: { message: "奖励数量必须大于 0" } };
      }
      if (kind === "gem") {
        resource = "gem";
      }
      if (kind !== "gold" && !resource) {
        throw { status: 400, data: { message: "非 gold/gem 奖励必须填写资源 ID" } };
      }
      state.rewardRows[prefix].push(normalizeRewardRow({
        kind,
        resource_id: resource,
        amount
      }));
      renderRewardRows(prefix);
    }

    function rewardsFromEditorOrJson(prefix) {
      const rows = state.rewardRows[prefix] || [];
      if (rows.length) {
        return rows.map(normalizeRewardRow);
      }
      const jsonId = prefix === "sheet" ? "sheetMailRewards" : (prefix === "shop" ? "shopRewardsJson" : "mailRewards");
      const raw = $(jsonId).value.trim();
      return raw ? JSON.parse(raw) : [];
    }

    function renderRewardItemOptions() {
      if (!$("rewardItemOptions")) {
        return;
      }
      const options = new Map();
      [...state.itemConfigs, ...state.gachaItemOptions].forEach((item) => {
        if (item?.item_id) {
          options.set(item.item_id, item.name || item.item_id);
        }
      });
      $("rewardItemOptions").innerHTML = [...options.entries()].map(([itemId, name]) =>
        "<option value=\"" + escapeHtml(itemId) + "\">" + escapeHtml(name) + "</option>"
      ).join("");
    }

    function initializeRewardEditor(prefix) {
      const jsonId = prefix === "sheet" ? "sheetMailRewards" : "mailRewards";
      if (!$(jsonId)) {
        return;
      }
      try {
        const parsed = JSON.parse($(jsonId).value.trim() || "[]");
        state.rewardRows[prefix] = Array.isArray(parsed) ? parsed.map(normalizeRewardRow) : [];
      } catch (_) {
        state.rewardRows[prefix] = [];
      }
      renderRewardRows(prefix);
    }

    function uniqueSelectedPlayerIds() {
      return [...new Set([
        ...state.selectedPlayers,
        ...state.selectedMails.map((item) => item.player_id).filter(Boolean)
      ])];
    }

    async function queryAllPlayerIdsForGmMail() {
      const result = await api("/admin/query/players", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({ limit: 10000, offset: 0 })
      });
      const payload = normalizeEnvelope(result);
      return extractItems(payload)
        .map((item) => item.player_id)
        .filter(Boolean);
    }

    async function sendGmMailToPlayerIds(playerIds) {
      if (!playerIds.length) {
        throw { status: 400, data: { message: "没有可发送的玩家目标" } };
      }
      return api("/mail/send", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          player_ids: playerIds,
          sender_name: $("mailSender").value.trim() || "System",
          title: ($("sheetMailTitle")?.value.trim() || $("mailTitle").value.trim() || "系统奖励邮件"),
          body: ($("sheetMailBody")?.value.trim() || $("mailBody").value.trim() || "请领取下方附件奖励。感谢你的支持，祝你游戏愉快。"),
          rewards: $("sheetMailRewards") ? parseSheetRewardsOrThrow() : parseRewardsOrThrow(),
          expires_in_seconds: Number($("mailExpire").value || 0)
        })
      });
    }

    function sheetSelectedPlayerIds() {
      const ids = uniqueSelectedPlayerIds();
      if (!ids.length && state.sheetSelectedPlayer?.player_id) {
        ids.push(state.sheetSelectedPlayer.player_id);
      }
      return [...new Set(ids)];
    }

    function renderSheetSelected(player) {
      state.sheetSelectedPlayer = player || null;
      if (!player) {
        $("sheetSelectedSummary").innerHTML =
          "<div><span>player_id</span><code>未选择</code></div>" +
          "<div><span>nickname</span><code>未选择</code></div>" +
          "<div><span>account_id</span><code>未选择</code></div>" +
          "<div><span>资产</span><code>未选择</code></div>";
        return;
      }
      $("queryPlayerId").value = player.player_id || "";
      $("mailPlayerId").value = player.player_id || "";
      $("mailQueryPlayerId").value = player.player_id || "";
      $("mutedPlayerId").value = player.player_id || "";
      $("statusAccountId").value = player.account_id || "";
      $("sheetSelectedSummary").innerHTML =
        "<div><span>player_id</span><code>" + escapeHtml(player.player_id || "") + "</code></div>" +
        "<div><span>nickname</span><code>" + escapeHtml(player.nickname || "") + "</code></div>" +
        "<div><span>account_id</span><code>" + escapeHtml(player.account_id || "") + "</code></div>" +
        "<div><span>资产</span><code>gold=" + escapeHtml(player.gold || 0) + " / gem=" + escapeHtml(player.gem || 0) + "</code></div>";
    }

    function renderSheetPlayers(items) {
      state.sheetPlayers = Array.isArray(items) ? items : [];
      if (!state.sheetPlayers.length) {
        $("sheetPlayerBody").innerHTML = "<tr><td class=\"row-index\">1</td><td colspan=\"9\" class=\"muted\">没有查询到玩家。</td></tr>";
        renderSheetSelected(null);
        return;
      }
      $("sheetPlayerBody").innerHTML = state.sheetPlayers.map((item, index) => {
        const muted = item.muted_until ? (parseUnixSeconds(item.muted_until) || String(item.muted_until)) : "未禁言";
        const loginAt = item.last_login_at ? (parseUnixSeconds(item.last_login_at) || String(item.last_login_at)) : "无";
        return "<tr data-sheet-player=\"" + escapeHtml(item.player_id || "") + "\">" +
          "<td class=\"row-index\">" + (index + 1) + "</td>" +
          "<td><code>" + escapeHtml(item.player_id || "") + "</code></td>" +
          "<td>" + escapeHtml(item.nickname || "") + "</td>" +
          "<td><code>" + escapeHtml(item.account_id || "") + "</code></td>" +
          "<td>" + escapeHtml(item.gold || 0) + "</td>" +
          "<td>" + escapeHtml(item.gem || 0) + "</td>" +
          "<td>" + playerSheetStatusBadge(item.status || "") + "</td>" +
          "<td>" + escapeHtml(muted) + "</td>" +
          "<td>" + escapeHtml(loginAt) + "</td>" +
          "<td><button class=\"mini-btn\" data-sheet-select=\"" + escapeHtml(item.player_id || "") + "\">选择</button></td>" +
          "</tr>";
      }).join("");
      renderSheetSelected(state.sheetPlayers[0]);
    }

    async function sheetQueryPlayers() {
      const lookupId = $("sheetLookupId").value.trim();
      const params = new URLSearchParams();
      params.set("limit", String(Number($("sheetLimit").value || 50)));
      params.set("offset", "0");
      if (lookupId) {
        params.set("player_id", lookupId);
      }
      const result = await api("/admin/query/players?" + params.toString(), {
        method: "GET",
        headers: authHeaders()
      });
      const data = normalizeEnvelope(result);
      const items = lookupId
        ? extractItems(data).filter((item) => item.player_id === lookupId)
        : extractItems(data);
      renderSheetPlayers(items);
      if (lookupId && items.length) {
        renderSheetSelected(items[0]);
        setTimeout(() => run("读取玩家仓库", sheetLoadInventory), 0);
      }
      return result;
    }

    function renderSheetInventory(items) {
      if (!Array.isArray(items) || !items.length) {
        $("sheetInventoryBody").innerHTML = "<tr><td class=\"row-index\">1</td><td colspan=\"3\" class=\"muted\">仓库为空。</td></tr>";
        return;
      }
      $("sheetInventoryBody").innerHTML = items.map((item, index) => {
        return "<tr><td class=\"row-index\">" + (index + 1) + "</td>" +
          "<td><code>" + escapeHtml(item.item_id || "") + "</code></td>" +
          "<td>" + escapeHtml(item.quantity || 0) + "</td>" +
          "<td><code>" + escapeHtml(item.instance_id || "") + "</code></td></tr>";
      }).join("");
    }

)HTML";

    static constexpr const char* kPart3_state_items = R"HTML(
    function renderItemConfigTable(items) {
      state.itemConfigs = Array.isArray(items) ? items : [];
      renderRewardItemOptions();
      if (!state.itemConfigs.length) {
        $("itemConfigBody").innerHTML = "<tr><td class=\"row-index\">1</td><td colspan=\"8\" class=\"muted\">没有物品配置。保存或查询后会显示在这里。</td></tr>";
        return;
      }
      $("itemConfigBody").innerHTML = state.itemConfigs.map((item, index) => {
        return "<tr>" +
          "<td class=\"row-index\">" + (index + 1) + "</td>" +
          "<td><code>" + escapeHtml(item.item_id || "") + "</code></td>" +
          "<td>" + escapeHtml(item.name || "") + "</td>" +
          "<td><code>" + escapeHtml(item.rarity || "") + "</code></td>" +
          "<td><code>" + escapeHtml(item.item_type || "") + "</code></td>" +
          "<td>" + escapeHtml(item.max_stack || 0) + "</td>" +
          "<td>" + (item.enabled ? "启用" : "停用") + "</td>" +
          "<td><code>" + escapeHtml(item.icon || "") + "</code></td>" +
          "<td><button class=\"mini-btn\" data-action=\"fill-item-config\" data-index=\"" + index + "\">带入编辑</button></td>" +
          "</tr>";
      }).join("");
    }

    function fillItemConfigForm(item) {
      if (!item) {
        return;
      }
      $("itemId").value = item.item_id || "";
      $("itemName").value = item.name || "";
      $("itemDescription").value = item.description || "";
      $("itemIcon").value = item.icon || "";
      $("itemRarity").value = item.rarity || "3星";
      $("itemMaxStack").value = item.max_stack || 1;
      $("itemType").value = item.item_type || "material";
      $("itemEnabled").value = item.enabled ? "true" : "false";
    }

    function renderShopRewardItemSelect(items) {
      if (!$("shopRewardItemSelect")) {
        return;
      }
      const current = $("shopRewardResource").value.trim();
      const options = ["<option value=\"\">手动输入或选择物品</option>"].concat(
        (Array.isArray(items) ? items : []).map((item) => {
          const label = (item.item_id || "") + " | " + (item.name || "") + " | " + (item.rarity || "");
          return "<option value=\"" + escapeHtml(item.item_id || "") + "\"" +
            (current && current === item.item_id ? " selected" : "") + ">" +
            escapeHtml(label) + "</option>";
        })
      );
      $("shopRewardItemSelect").innerHTML = options.join("");
    }

    function renderGachaRewardItemSelect(items) {
      state.gachaItemOptions = Array.isArray(items) ? items : [];
      renderRewardItemOptions();
      const current = $("gachaRewardResource").value.trim();
      const options = ["<option value=\"\">手动输入或选择物品</option>"].concat(
        state.gachaItemOptions.map((item) => {
          const label = (item.item_id || "") + " | " + (item.name || "") + " | " + (item.rarity || "");
          return "<option value=\"" + escapeHtml(item.item_id || "") + "\"" +
            (current && current === item.item_id ? " selected" : "") + ">" +
            escapeHtml(label) + "</option>";
        })
      );
      $("gachaRewardItemSelect").innerHTML = options.join("");
      renderGachaRewardItemTable();
      renderShopRewardItemSelect(state.gachaItemOptions);
    }

    function renderGachaRewardItemTable() {
      const keyword = $("gachaRewardItemKeyword") ? $("gachaRewardItemKeyword").value.trim().toLowerCase() : "";
      let items = state.gachaItemOptions.slice();
      if (keyword) {
        items = items.filter((item) => {
          return [item.item_id, item.name, item.rarity, item.item_type]
            .some((value) => String(value || "").toLowerCase().includes(keyword));
        });
      }
      if (!items.length) {
        $("gachaRewardItemBody").innerHTML =
          "<tr><td class=\"row-index\">1</td><td colspan=\"5\" class=\"muted\">没有匹配的物品。先在物品配置里新增，或清空搜索条件。</td></tr>";
        return;
      }
      $("gachaRewardItemBody").innerHTML = items.map((item, index) => {
        return "<tr>" +
          "<td class=\"row-index\">" + (index + 1) + "</td>" +
          "<td>" + codeValue(item.item_id || "") + "</td>" +
          "<td>" + escapeHtml(item.name || "") + "</td>" +
          "<td>" + codeValue(item.rarity || "") + "</td>" +
          "<td>" + codeValue(item.item_type || "") + "</td>" +
          "<td>" + bindActionsHtml([
            { action: "use-gacha-reward-item", label: "带入", data: { item_id: item.item_id || "" } },
            { action: "add-gacha-reward-item", label: "加入池子", alt: true, data: { item_id: item.item_id || "" } }
          ]) + "</td>" +
          "</tr>";
      }).join("");
    }

    async function loadGachaRewardItems() {
      const result = await api("/admin/items/query", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          limit: 200,
          offset: 0,
          enabled: true
        })
      });
      const data = normalizeEnvelope(result);
      renderGachaRewardItemSelect(data.items || []);
      return result;
    }

    function applySelectedGachaRewardItem(syncItemKey = false) {
      const itemId = $("gachaRewardItemSelect").value;
      if (!itemId) {
        return;
      }
      $("gachaRewardKind").value = "item";
      $("gachaRewardResource").value = itemId;
      if (syncItemKey || !$("gachaItemKey").value.trim()) {
        $("gachaItemKey").value = itemId;
      }
      const item = state.gachaItemOptions.find((value) => value.item_id === itemId);
      if (item?.rarity === "5星") {
        $("gachaRarity").value = 5;
      } else if (item?.rarity === "4星") {
        $("gachaRarity").value = 4;
      } else if (item?.rarity === "3星") {
        $("gachaRarity").value = 3;
      }
    }

    function fillGachaRewardItemById(itemId, syncItemKey = false) {
      if (!itemId) {
        return;
      }
      $("gachaRewardItemSelect").value = itemId;
      applySelectedGachaRewardItem(syncItemKey);
    }
)HTML";

    static constexpr const char* kPart3_state_gacha = R"HTML(

    function renderGachaConfigTable(groups) {
      function percent(part, total) {
        if (!total) return "0%";
        return ((Number(part || 0) / Number(total || 1)) * 100).toFixed(3).replace(/\.?0+$/, "") + "%";
      }
      const rows = [];
      (Array.isArray(groups) ? groups : []).forEach((group) => {
        const pool = group.pool || group;
        const items = Array.isArray(group.items) && group.items.length ? group.items : [null];
        items.forEach((item) => rows.push({ pool, item }));
      });
      state.gachaConfigs = Array.isArray(groups) ? groups : [];
      const selectedPoolId = $("gachaPoolId").value.trim() || (state.gachaConfigs[0]?.pool?.pool_id || "");
      const selectedGroup = state.gachaConfigs.find((group) => group.pool?.pool_id === selectedPoolId) || state.gachaConfigs[0] || null;
      const selectedItems = selectedGroup?.items || [];
      const totalWeight = selectedItems.reduce((sum, item) => sum + Number(item.weight || 0), 0);
      const rarity5Weight = selectedItems.filter((item) => Number(item.rarity || 0) >= 5).reduce((sum, item) => sum + Number(item.weight || 0), 0);
      const rarity4Weight = selectedItems.filter((item) => Number(item.rarity || 0) === 4).reduce((sum, item) => sum + Number(item.weight || 0), 0);
      const rarity3Weight = selectedItems.filter((item) => Number(item.rarity || 0) <= 3).reduce((sum, item) => sum + Number(item.weight || 0), 0);
      $("gachaRaritySummary").innerHTML =
        "<div><span>当前池子</span><code>" + escapeHtml(selectedGroup?.pool?.pool_id || "未选择") + "</code></div>" +
        "<div><span>总权重</span><code>" + escapeHtml(totalWeight) + "</code></div>" +
        "<div><span>5星概率</span><code>" + escapeHtml(percent(rarity5Weight, totalWeight)) + "</code></div>" +
        "<div><span>4星概率</span><code>" + escapeHtml(percent(rarity4Weight, totalWeight)) + "</code></div>" +
        "<div><span>3星概率</span><code>" + escapeHtml(percent(rarity3Weight, totalWeight)) + "</code></div>" +
        "<div><span>说明</span><code>非保底概率</code></div>";

      if (!state.gachaConfigs.length) {
        $("gachaPoolListBody").innerHTML = "<tr><td class=\"row-index\">1</td><td colspan=\"7\" class=\"muted\">当前没有卡池。</td></tr>";
      } else {
        $("gachaPoolListBody").innerHTML = state.gachaConfigs.map((group, index) => {
          const pool = group.pool || {};
          return "<tr>" +
            "<td class=\"row-index\">" + (index + 1) + "</td>" +
            "<td>" + codeValue(pool.pool_id || "") + "</td>" +
            "<td>" + codeValue(pool.pool_type || "standard") + "</td>" +
            "<td>" + escapeHtml(pool.name || "") + "</td>" +
            "<td>" + codeValue((pool.cost_currency_id || "gem") + " x" + (pool.cost_amount_per_draw || 0)) + "</td>" +
            "<td>" + codeValue("5星 " + (pool.pity_5 || 0) + " / 4星 " + (pool.pity_4 || 0)) + "</td>" +
            "<td>" + codeValue(pool.enabled) + "</td>" +
            "<td>" + bindActionsHtml([
              { action: "fill-gacha-pool", label: "编辑池子", data: { pool_id: pool.pool_id || "" } },
              { action: "delete-gacha-pool", label: "删除池子", alt: true, data: { pool_id: pool.pool_id || "", name: pool.name || "" } }
            ]) + "</td>" +
            "</tr>";
        }).join("");
      }
      if (!rows.length) {
        $("gachaConfigBody").innerHTML = "<tr><td class=\"row-index\">1</td><td colspan=\"10\" class=\"muted\">当前没有抽卡配置。</td></tr>";
        return;
      }
      $("gachaConfigBody").innerHTML = rows.map((row, index) => {
        const pool = row.pool || {};
        const item = row.item || {};
        const reward = item.reward || {};
        const group = state.gachaConfigs.find((entry) => entry.pool?.pool_id === pool.pool_id);
        const itemTotalWeight = (group?.items || []).reduce((sum, value) => sum + Number(value.weight || 0), 0);
        const rewardText = reward.kind
          ? reward.kind + ":" + (reward.resource_id || "") + " x" + (reward.amount || 0)
          : "未配置奖品";
        return "<tr>" +
          "<td class=\"row-index\">" + (index + 1) + "</td>" +
          "<td>" + codeValue(pool.pool_id || "") + "</td>" +
          "<td>" + codeValue(pool.pool_type || "standard") + "</td>" +
          "<td>" + escapeHtml(pool.name || "") + "</td>" +
          "<td>" + codeValue(item.item_key || "") + "</td>" +
          "<td>" + codeValue(item.rarity || "") + "</td>" +
          "<td>" + codeValue(item.weight || "") + "</td>" +
          "<td>" + codeValue(item.item_key ? percent(item.weight || 0, itemTotalWeight) : "") + "</td>" +
          "<td>" + codeValue(item.is_up ? "UP" : "") + "</td>" +
          "<td>" + escapeHtml(rewardText) + "</td>" +
          "<td>" + bindActionsHtml([
            { action: "fill-gacha-pool", label: "编辑池子", data: { pool_id: pool.pool_id || "" } },
            { action: "fill-gacha-item", label: "编辑奖品", alt: true, data: { pool_id: pool.pool_id || "", item_key: item.item_key || "" } },
            { action: "delete-gacha-item", label: "删除奖品", data: { pool_id: pool.pool_id || "", item_key: item.item_key || "" } }
          ]) + "</td>" +
          "</tr>";
      }).join("");
    }

    function findGachaPool(poolId) {
      for (const group of state.gachaConfigs) {
        if (group.pool?.pool_id === poolId) return group.pool;
      }
      return null;
    }

    function findGachaItem(poolId, itemKey) {
      for (const group of state.gachaConfigs) {
        if (group.pool?.pool_id !== poolId) continue;
        return (group.items || []).find((item) => item.item_key === itemKey) || null;
      }
      return null;
    }

    function fillGachaPoolForm(pool) {
      if (!pool) return;
      $("gachaPoolId").value = pool.pool_id || "";
      $("gachaPoolType").value = pool.pool_type || "standard";
      $("gachaPoolName").value = pool.name || "";
      $("gachaPoolDescription").value = pool.description || "";
      $("gachaBannerIcon").value = pool.banner_icon || "";
      $("gachaCostCurrency").value = pool.cost_currency_id || "gem";
      $("gachaCostAmount").value = pool.cost_amount_per_draw ?? 160;
      $("gachaPity5").value = pool.pity_5 ?? 90;
      $("gachaPity4").value = pool.pity_4 ?? 10;
      $("gachaEnabled").value = pool.enabled ? "true" : "false";
    }

    function fillGachaItemForm(item) {
      if (!item) return;
      $("gachaPoolId").value = item.pool_id || $("gachaPoolId").value;
      $("gachaItemKey").value = item.item_key || "";
      $("gachaRarity").value = item.rarity ?? 3;
      $("gachaWeight").value = item.weight ?? 1;
      $("gachaIsUp").value = item.is_up ? "true" : "false";
      $("gachaRewardKind").value = item.reward?.kind || "item";
      $("gachaRewardResource").value = item.reward?.resource_id || "";
      $("gachaRewardAmount").value = item.reward?.amount ?? 1;
      renderGachaRewardItemSelect(state.gachaItemOptions);
    }

    function shopPriceCurrencyId() {
      const selected = $("shopPriceCurrency").value;
      if (selected === "currency") {
        return $("shopPriceCurrencyCustom").value.trim();
      }
      return selected || "gem";
    }

    function parseShopRewardsOrThrow() {
      return rewardsFromEditorOrJson("shop");
    }

    function renderShopProductTable(products) {
      state.shopProducts = Array.isArray(products) ? products : [];
      if (!state.shopProducts.length) {
        $("shopProductBody").innerHTML =
          "<tr><td class=\"row-index\">1</td><td colspan=\"8\" class=\"muted\">当前没有商城商品。填写上方表单后点“保存商品”。</td></tr>";
        return;
      }
      $("shopProductBody").innerHTML = state.shopProducts.map((product, index) => {
        const rewards = Array.isArray(product.rewards) ? product.rewards : [];
        const rewardText = rewards.map((reward) => {
          return (reward.kind || "") + ":" + (reward.resource_id || "") + " x" + (reward.amount || 0);
        }).join("；");
        return "<tr>" +
          "<td class=\"row-index\">" + (index + 1) + "</td>" +
          "<td>" + codeValue(product.product_id || "") + "</td>" +
          "<td>" + codeValue(product.category || "") + "</td>" +
          "<td>" + escapeHtml(product.name || "") + "</td>" +
          "<td>" + codeValue((product.price_currency_id || "") + " x" + (product.price_amount || 0)) + "</td>" +
          "<td>" + escapeHtml(rewardText || "未配置奖励") + "</td>" +
          "<td>" + codeValue(product.purchase_limit || 0) + "</td>" +
          "<td>" + codeValue(product.enabled ? "上架" : "下架") + "</td>" +
          "<td>" + bindActionsHtml([
            { action: "fill-shop-product", label: "编辑商品", data: { product_id: product.product_id || "" } },
            { action: "delete-shop-product", label: "删除商品", alt: true, data: { product_id: product.product_id || "", name: product.name || "" } }
          ]) + "</td>" +
          "</tr>";
      }).join("");
    }

    function findShopProduct(productId) {
      return state.shopProducts.find((product) => product.product_id === productId) || null;
    }

    function fillShopProductForm(product) {
      if (!product) {
        return;
      }
      $("shopProductId").value = product.product_id || "";
      $("shopCategory").value = product.category || "daily";
      $("shopName").value = product.name || "";
      $("shopDescription").value = product.description || "";
      $("shopIcon").value = product.icon || "";
      if (product.price_currency_id === "gold" || product.price_currency_id === "gem") {
        $("shopPriceCurrency").value = product.price_currency_id;
        $("shopPriceCurrencyCustom").value = "";
      } else {
        $("shopPriceCurrency").value = "currency";
        $("shopPriceCurrencyCustom").value = product.price_currency_id || "";
      }
      $("shopPriceAmount").value = product.price_amount ?? 0;
      $("shopPurchaseLimit").value = product.purchase_limit ?? 0;
      $("shopEnabled").value = product.enabled ? "true" : "false";
      state.rewardRows.shop = Array.isArray(product.rewards) ? product.rewards.map(normalizeRewardRow) : [];
      renderRewardRows("shop");
      renderShopRewardItemSelect(state.gachaItemOptions);
    }

    async function queryShopProducts() {
      const result = await api("/admin/shop/query", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          include_disabled: true
        })
      });
      const data = normalizeEnvelope(result);
      renderShopProductTable(data.products || []);
      return result;
    }

    async function sheetLoadInventory() {
      const player = state.sheetSelectedPlayer;
      if (!player?.player_id) {
        throw { status: 400, data: { message: "请先选择玩家" } };
      }
      const result = await api("/admin/query/inventory", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({ player_id: player.player_id })
      });
      const data = normalizeEnvelope(result);
      renderSheetInventory(data.inventory || []);
      return result;
    }

    function parseSheetRewardsOrThrow() {
      return rewardsFromEditorOrJson("sheet");
    }

    function upsertSelectedMail(mail) {
      if (!mail.mail_id) {
        return;
      }
      state.selectedMails = state.selectedMails.filter((item) => item.mail_id !== mail.mail_id);
      state.selectedMails.push(mail);
      syncSelectionView();
    }

    function removeSelectedMail(mailId) {
      state.selectedMails = state.selectedMails.filter((item) => item.mail_id !== mailId);
      syncSelectionView();
    }

    function upsertSelectedAudit(audit) {
      if (!audit.audit_id) {
        return;
      }
      state.selectedAudits = state.selectedAudits.filter((item) => item.audit_id !== audit.audit_id);
      state.selectedAudits.push(audit);
      syncSelectionView();
    }

    function removeSelectedAudit(auditId) {
      state.selectedAudits = state.selectedAudits.filter((item) => item.audit_id !== auditId);
      syncSelectionView();
    }

    function collectFilterPreset() {
      return {
        players: {
          player_id: $("playersPlayerId").value,
          account_id: $("playersAccountId").value,
          nickname: $("playersNickname").value,
          limit: $("playersLimit").value,
          offset: $("playersOffset").value
        },
        mail: {
          player_id: $("mailQueryPlayerId").value,
          include_deleted: $("mailQueryIncludeDeleted").value,
          claimed: $("mailQueryClaimed").value,
          read: $("mailQueryRead").value,
          limit: $("mailQueryLimit").value,
          offset: $("mailQueryOffset").value
        },
        grant: {
          player_id: $("grantPlayerId").value,
          source_type: $("grantSourceType").value,
          source_id: $("grantSourceId").value,
          limit: $("grantLimit").value,
          offset: $("grantOffset").value
        },
        audit: {
          action: $("auditAction").value,
          operator_id: $("auditOperatorId").value,
          success: $("auditSuccess").value,
          limit: $("auditLimit").value,
          offset: $("auditOffset").value
        },
        tracing: {
          trace_id: $("traceQueryId").value,
          limit: $("traceQueryLimit").value
        }
      };
    }
)HTML";

    static constexpr const char* kPart3a = R"HTML(
    function applyFilterPreset(preset) {
      const safe = preset || {};
      const players = safe.players || {};
      const mail = safe.mail || {};
      const grant = safe.grant || {};
      const audit = safe.audit || {};
      const tracing = safe.tracing || {};
      $("playersPlayerId").value = players.player_id || "";
      $("playersAccountId").value = players.account_id || "";
      $("playersNickname").value = players.nickname || "";
      $("playersLimit").value = players.limit || "20";
      $("playersOffset").value = players.offset || "0";
      $("mailQueryPlayerId").value = mail.player_id || "";
      $("mailQueryIncludeDeleted").value = mail.include_deleted || "false";
      $("mailQueryClaimed").value = mail.claimed || "";
      $("mailQueryRead").value = mail.read || "";
      $("mailQueryLimit").value = mail.limit || "20";
      $("mailQueryOffset").value = mail.offset || "0";
      $("grantPlayerId").value = grant.player_id || "";
      $("grantSourceType").value = grant.source_type || "";
      $("grantSourceId").value = grant.source_id || "";
      $("grantLimit").value = grant.limit || "20";
      $("grantOffset").value = grant.offset || "0";
      $("auditAction").value = audit.action || "";
      $("auditOperatorId").value = audit.operator_id || "";
      $("auditSuccess").value = audit.success || "";
      $("auditLimit").value = audit.limit || "20";
      $("auditOffset").value = audit.offset || "0";
      $("traceQueryId").value = tracing.trace_id || "";
      $("traceQueryLimit").value = tracing.limit || "100";
      $("traceViewerId").value = tracing.trace_id || "";
    }

    function setDetailCard(title, meta, fields, actions = []) {
      $("detailTitle").textContent = title;
      $("detailMeta").textContent = meta;
      $("detailActions").innerHTML = bindActionsHtml(actions);
      $("detailGrid").innerHTML = fields.map((field) => {
        return "<div class=\"detail-item\"><span class=\"k\">" + escapeHtml(field.key) +
          "</span><div class=\"v\">" + escapeHtml(shortText(field.value, field.max || 120)) + "</div></div>";
      }).join("");
      $("detailExtra").innerHTML = "";
    }

    function setDetailExtraHtml(html) {
      $("detailExtra").innerHTML = html || "";
    }
)HTML";

    static constexpr const char* kPart3b = R"HTML(
    function authHeaders(extra = {}) {
      const headers = { "Content-Type": "application/json", ...extra };
      if (!state.token) {
        state.token = localStorage.getItem("hello_admin_token") || "";
      }
      if (state.token) {
        headers["Authorization"] = "Bearer " + state.token;
      }
      return headers;
    }

    async function api(path, options = {}) {
      const response = await fetch(state.baseUrl + path, options);
      const text = await response.text();
      let data = text;
      try {
        data = JSON.parse(text);
      } catch (_) {}
      if (!response.ok) {
        throw { status: response.status, data };
      }
      return data;
    }

    function normalizeEnvelope(result) {
      if (result && typeof result === "object" && "data" in result) {
        return result.data;
      }
      return result;
    }
)HTML";

    static constexpr const char* kPart3c = R"HTML(
    function parseBoolOrNull(value) {
      if (value === "true") return true;
      if (value === "false") return false;
      return null;
    }

    function addIfPresent(target, key, value) {
      if (value !== "" && value !== null && value !== undefined) {
        target[key] = value;
      }
      return target;
    }

    function buildChannel(prefix) {
      const kind = $(prefix + "Kind").value;
      const payload = { channel_kind: kind };
      if (kind === "room") {
        payload.scope_id = $(prefix + "Scope").value.trim();
      } else if (kind === "lobby") {
        const scope = $(prefix + "Scope").value.trim();
        if (scope) payload.scope_id = scope;
      } else if (kind === "friend_direct") {
        payload.player_a = $(prefix + "PlayerA").value.trim();
        payload.player_b = $(prefix + "PlayerB").value.trim();
      }
      return payload;
    }

    function parseRewardsOrThrow() {
      return rewardsFromEditorOrJson("mail");
    }

    function extractItems(payload) {
      if (Array.isArray(payload)) {
        return payload;
      }
      if (payload && Array.isArray(payload.items)) {
        return payload.items;
      }
      return [];
    }

    function tableCell(content, className = "") {
      const klass = className ? " class=\"" + className + "\"" : "";
      return "<td" + klass + ">" + content + "</td>";
    }

    function codeValue(value) {
      return "<code>" + escapeHtml(flatValue(value)) + "</code>";
    }

    function shortText(value, max = 48) {
      const text = flatValue(value);
      return text.length > max ? text.slice(0, max) + "..." : text;
    }

    function flatValue(value) {
      if (value === null || value === undefined) {
        return "";
      }
      if (typeof value === "object") {
        return JSON.stringify(value);
      }
      return String(value);
    }

    function parseJsonObject(value) {
      if (!value || typeof value !== "string") {
        return null;
      }
      try {
        const parsed = JSON.parse(value);
        return parsed && typeof parsed === "object" ? parsed : null;
      } catch (_) {
        return null;
      }
    }

    // 解析任务执行结果里的汇总字段，统一给列表和右侧详情卡复用。
    function parseTaskResultSummary(resultJson) {
      const result = parseJsonObject(resultJson);
      if (!result) {
        return { raw: null, success: null, failure: null, total: null, itemCount: 0 };
      }
      const success = Number.isFinite(Number(result.success_count)) ? Number(result.success_count) : null;
      const failure = Number.isFinite(Number(result.failure_count)) ? Number(result.failure_count) : null;
      const itemCount = Array.isArray(result.items) ? result.items.length : 0;
      const total = Number.isFinite(Number(result.player_count))
        ? Number(result.player_count)
        : (success !== null || failure !== null ? (success || 0) + (failure || 0) : itemCount || null);
      return { raw: result, success, failure, total, itemCount };
    }

    // 只截取前几条失败项，避免任务详情直接展示整块大 JSON。
    function collectTaskFailurePreview(result, limit = 5) {
      if (!result || !Array.isArray(result.items)) {
        return [];
      }
      return result.items.filter((item) => item && item.success === false).slice(0, limit);
    }

    // 把 request_json 压缩成简短预览，便于表格里快速看任务目标。
    function summarizeTaskRequest(requestJson) {
      const request = parseJsonObject(requestJson);
      if (!request) {
        return "(无请求体)";
      }
      if (Array.isArray(request.player_ids)) {
        const preview = request.player_ids.slice(0, 3).join(", ");
        return "player_ids=" + request.player_ids.length + (preview ? " [" + preview + "]" : "");
      }
      if (request.channel_kind) {
        return "channel=" + request.channel_kind + (request.scope_id ? ":" + request.scope_id : "");
      }
      if (request.status) {
        return "status=" + request.status;
      }
      return shortText(JSON.stringify(request), 48);
    }

    function parseUnixSeconds(value) {
      const number = Number(value);
      if (!Number.isFinite(number) || number <= 0) {
        return "";
      }
      return new Date(number * 1000).toLocaleString("zh-CN", { hour12: false });
    }

    function normalizeTraceAttributes(span) {
      const list = Array.isArray(span && span.attributes) ? span.attributes : [];
      return list.map((item) => ({
        key: item && item.key ? String(item.key) : "",
        value: item && item.value !== undefined && item.value !== null ? String(item.value) : ""
      })).filter((item) => item.key);
    }

    function groupTraceSpans(spans) {
      const groups = new Map();
      (Array.isArray(spans) ? spans : []).forEach((span) => {
        const traceId = span && span.trace_id ? String(span.trace_id) : "";
        if (!traceId) {
          return;
        }
        if (!groups.has(traceId)) {
          groups.set(traceId, []);
        }
        groups.get(traceId).push(span);
      });
      return [...groups.entries()].map(([trace_id, items]) => {
        const startedValues = items.map((span) => Number(span.started_at)).filter(Number.isFinite);
        const endedValues = items.map((span) => Number(span.ended_at)).filter(Number.isFinite);
        const rootCount = items.filter((span) => !span.parent_span_id).length;
        const attrCount = items.reduce((sum, span) => sum + normalizeTraceAttributes(span).length, 0);
        const failed = items.some((span) => (span.status || "") === "error");
        return {
          trace_id,
          spans: items.slice().sort((a, b) => Number(a.started_at || 0) - Number(b.started_at || 0)),
          span_count: items.length,
          root_count: rootCount,
          attr_count: attrCount,
          started_at: startedValues.length ? Math.min(...startedValues) : 0,
          ended_at: endedValues.length ? Math.max(...endedValues) : 0,
          status: failed ? "error" : "ok"
        };
      }).sort((a, b) => Number(b.started_at || 0) - Number(a.started_at || 0));
    }

    function findTraceGroup(traceId) {
      return groupTraceSpans(state.traceSpans).find((item) => item.trace_id === traceId) || null;
    }

    function buildTraceTreeNodes(spans) {
      const byParent = new Map();
      const byId = new Map();
      spans.forEach((span) => {
        const spanId = span && span.span_id ? String(span.span_id) : "";
        const parentId = span && span.parent_span_id ? String(span.parent_span_id) : "";
        if (spanId) {
          byId.set(spanId, span);
        }
        if (!byParent.has(parentId)) {
          byParent.set(parentId, []);
        }
        byParent.get(parentId).push(span);
      });
      byParent.forEach((items) => {
        items.sort((a, b) => Number(a.started_at || 0) - Number(b.started_at || 0));
      });
      const roots = byParent.get("") || [];
      if (roots.length) {
        return { roots, byParent, byId };
      }
      return {
        roots: spans.slice(0, 1),
        byParent,
        byId
      };
    }

    function renderTraceNode(span, byParent, focusSpanId, depth = 0) {
      const spanId = span && span.span_id ? String(span.span_id) : "";
      const attrs = normalizeTraceAttributes(span);
      const extraClass = spanId && spanId === focusSpanId ? " focus" : "";
      const children = byParent.get(spanId) || [];
      const header = [
        "<div class=\"trace-node-head\">",
        "<strong>" + escapeHtml(span.name || "(unnamed span)") + "</strong>",
        "<code>" + escapeHtml(span.status || "unset") + "</code>",
        "<code>" + escapeHtml(spanId) + "</code>",
        bindActionsHtml([
          { action: "show-span-detail", label: "看 Span", data: { trace_id: span.trace_id || "", span_id: spanId } },
          { action: "fill-trace-id", label: "带入 trace", alt: true, data: { trace_id: span.trace_id || "" } }
        ]),
        "</div>"
      ].join("");
      const meta = [
        "<div class=\"trace-attrs\">",
        "<div class=\"trace-attr\">traceparent=" + escapeHtml(span.traceparent || "") + "</div>",
        "<div class=\"trace-attr\">started_at=" + escapeHtml(parseUnixSeconds(span.started_at) || String(span.started_at || "")) +
          " / ended_at=" + escapeHtml(parseUnixSeconds(span.ended_at) || String(span.ended_at || "")) + "</div>",
        attrs.map((item) => "<div class=\"trace-attr\">" + escapeHtml(item.key + "=" + item.value) + "</div>").join(""),
        "</div>"
      ].join("");
      const childrenHtml = children.map((child) => renderTraceNode(child, byParent, focusSpanId, depth + 1)).join("");
      return "<div class=\"trace-node" + extraClass + "\">" + header + meta + "</div>" +
        (childrenHtml ? "<div class=\"trace-indent\">" + childrenHtml + "</div>" : "");
    }

    function showTraceDetail(traceId, focusSpanId = "") {
      const trace = findTraceGroup(traceId);
      if (!trace) {
        setDetailCard("Tracing 卡片", "当前内存窗口里没有找到这个 trace_id。", [
          { key: "trace_id", value: traceId || "(空)" },
          { key: "span_count", value: 0 }
        ]);
        return;
      }
      const tree = buildTraceTreeNodes(trace.spans);
      setDetailCard("Tracing 卡片", "右侧直接展开 trace span 树和属性，便于排查 HTTP 请求、后台任务和 MySQL 子调用。", [
        { key: "trace_id", value: trace.trace_id, max: 160 },
        { key: "span_count", value: trace.span_count },
        { key: "root_count", value: trace.root_count },
        { key: "attribute_count", value: trace.attr_count },
        { key: "started_at", value: parseUnixSeconds(trace.started_at) || trace.started_at },
        { key: "ended_at", value: parseUnixSeconds(trace.ended_at) || trace.ended_at }
      ], [
        { action: "fill-trace-id", label: "带入 trace", data: { trace_id: trace.trace_id } },
        { action: "query-trace-id", label: "只看这个 trace", alt: true, data: { trace_id: trace.trace_id } }
      ]);
      const html = "<div class=\"trace-tree\">" +
        tree.roots.map((span) => renderTraceNode(span, tree.byParent, focusSpanId)).join("") +
        "</div>";
      setDetailExtraHtml(html);
      $("traceViewerId").value = trace.trace_id;
    }

    function showSpanDetail(traceId, spanId) {
      const trace = findTraceGroup(traceId);
      if (!trace) {
        return;
      }
      const span = trace.spans.find((item) => String(item.span_id || "") === String(spanId || ""));
      if (!span) {
        return;
      }
      setDetailCard("Span 卡片", "先看单个 span 的核心字段，再在下方继续查看整条 trace 的树。", [
        { key: "trace_id", value: traceId, max: 160 },
        { key: "span_id", value: spanId },
        { key: "parent_span_id", value: span.parent_span_id || "(root)" },
        { key: "name", value: span.name || "" },
        { key: "status", value: span.status || "unset" },
        { key: "attribute_count", value: normalizeTraceAttributes(span).length },
        { key: "traceparent", value: span.traceparent || "", max: 160 }
      ], [
        { action: "show-trace-tree", label: "回到整条链路", data: { trace_id: traceId, span_id: spanId } },
        { action: "fill-trace-id", label: "带入 trace", alt: true, data: { trace_id: traceId } }
      ]);
      const tree = buildTraceTreeNodes(trace.spans);
      const html = "<div class=\"trace-tree\">" +
        tree.roots.map((item) => renderTraceNode(item, tree.byParent, spanId)).join("") +
        "</div>";
      setDetailExtraHtml(html);
      $("traceViewerId").value = trace.trace_id;
    }

    function chooseColumns(items) {
      const columns = [];
      for (const item of items.slice(0, 5)) {
        if (!item || typeof item !== "object") {
          continue;
        }
        for (const key of Object.keys(item)) {
          if (!columns.includes(key)) {
            columns.push(key);
          }
          if (columns.length >= 6) {
            return columns;
          }
        }
      }
      return columns.length ? columns : ["value"];
    }

    function renderEmptyTable(message) {
      $("tableHint").textContent = "当前结果没有可展示的列表数据。";
      $("resultHead").innerHTML = "<th>无列表结果</th>";
      $("resultBody").innerHTML = "<tr><td class=\"muted\">" + escapeHtml(message) + "</td></tr>";
    }

    function renderDefaultDetail(payload) {
      if (!payload || typeof payload !== "object" || Array.isArray(payload)) {
        setDetailCard("单对象卡片", "当前查询主要返回列表，所以这里保留默认提示。", [
          { key: "状态", value: "当前没有单对象结果" }
        ]);
        return;
      }
      const entries = Object.entries(payload).slice(0, 10).map(([key, value]) => ({
        key,
        value: typeof value === "object" ? JSON.stringify(value) : String(value)
      }));
      setDetailCard("单对象卡片", "当前结果使用通用对象展示。", entries);
    }

    function renderGenericTable(payload) {
      $("tableHint").textContent = "当前类型没有专门模板，使用通用列预览。";
      const items = extractItems(payload);
      if (!items.length) {
        renderEmptyTable("当前接口没有返回 items 数组。");
        return;
      }

      const columns = chooseColumns(items);
      $("resultHead").innerHTML = columns.map((column) => "<th>" + column + "</th>").join("");
      $("resultBody").innerHTML = items.slice(0, 20).map((item) => {
        const cells = columns.map((column) => tableCell(codeValue(item[column]))).join("");
        return "<tr>" + cells + "</tr>";
      }).join("");
    }
)HTML";

    static constexpr const char* kPart3c2 = R"HTML(
    function isPlayerSheetTitle(title) {
      return String(title || "").includes("玩家列表");
    }

    function isPlayerMuted(item) {
      return !!(item && item.muted_until);
    }

    function playerSheetSortMarker(key) {
      if (state.playerSheet.sortKey !== key) {
        return "";
      }
      return state.playerSheet.sortDir === "asc" ? " ▲" : " ▼";
    }

    function comparePlayerSheetValues(left, right) {
      const leftEmpty = left === null || left === undefined || left === "";
      const rightEmpty = right === null || right === undefined || right === "";
      if (leftEmpty && rightEmpty) {
        return 0;
      }
      if (leftEmpty) {
        return 1;
      }
      if (rightEmpty) {
        return -1;
      }

      const leftNumber = Number(left);
      const rightNumber = Number(right);
      if (Number.isFinite(leftNumber) && Number.isFinite(rightNumber)) {
        return leftNumber - rightNumber;
      }
      return String(left).localeCompare(String(right), "zh-CN");
    }

    function playerSheetStatusBadge(status) {
      const text = String(status || "");
      const klass = text === "banned" ? "banned" : (text === "frozen" ? "frozen" : "active");
      return "<span class=\"sheet-status " + klass + "\">" + escapeHtml(text || "unknown") + "</span>";
    }

    function applyPlayerSheetView(items) {
      const keyword = $("playersSheetKeyword").value.trim().toLowerCase();
      const status = $("playersSheetStatus").value;
      const mutedOnly = $("playersSheetMutedOnly").value;
      let rows = (Array.isArray(items) ? items : []).slice();

      if (keyword) {
        rows = rows.filter((item) => {
          return [item.player_id, item.account_id, item.nickname]
            .some((value) => String(value || "").toLowerCase().includes(keyword));
        });
      }

      if (status) {
        rows = rows.filter((item) => String(item.status || "") === status);
      }

      if (mutedOnly === "muted") {
        rows = rows.filter((item) => isPlayerMuted(item));
      } else if (mutedOnly === "unmuted") {
        rows = rows.filter((item) => !isPlayerMuted(item));
      }

      const sortKey = state.playerSheet.sortKey || "player_id";
      const sortDir = state.playerSheet.sortDir === "desc" ? "desc" : "asc";
      rows.sort((left, right) => {
        const compared = comparePlayerSheetValues(left ? left[sortKey] : "", right ? right[sortKey] : "");
        return sortDir === "desc" ? -compared : compared;
      });
      return rows;
    }
)HTML";

    static constexpr const char* kPart3c3 = R"HTML(
    function bindActionsHtml(actions) {
      return actions.map((action) => {
        const attrs = Object.entries(action.data || {}).map(([key, value]) => {
          return " data-" + key + "=\"" + escapeHtml(String(value)) + "\"";
        }).join("");
        const klass = action.alt ? "mini-btn alt" : "mini-btn";
        return "<button class=\"" + klass + "\" data-action=\"" + action.action + "\"" + attrs + ">" +
          escapeHtml(action.label) + "</button>";
      }).join("");
    }

    function renderCustomTable(config, items) {
      if (!items.length) {
        renderEmptyTable("当前筛选条件下没有数据。");
        return;
      }
      $("tableHint").textContent = config.hint;
      $("resultHead").innerHTML = config.columns.map((column) => "<th>" + column.title + "</th>").join("");
      $("resultBody").innerHTML = items.slice(0, 30).map((item) => {
        const cells = config.columns.map((column) => {
          const html = column.render(item);
          return tableCell(html, column.className || "");
        }).join("");
        return "<tr>" + cells + "</tr>";
      }).join("");
    }
)HTML";

    static constexpr const char* kPart3d = R"HTML(
    function renderPlayerSheet(items) {
      const rows = applyPlayerSheetView(items);
      if (!rows.length) {
        $("tableHint").textContent = "玩家主表经过本页筛选后没有数据。";
        $("resultHead").innerHTML = "";
        $("resultBody").innerHTML = "<tr><td class=\"muted\">当前本页筛选后没有玩家数据。</td></tr>";
        return;
      }

      $("tableHint").textContent =
        "玩家主表：点击表头排序；本页筛选只作用于当前页；“行状态/行禁言”会直接执行后台接口。当前本页 " +
        rows.length + " 条。";
      const columns = [
        { key: "select", title: "勾选" },
        { key: "player_id", title: "player_id" },
        { key: "account_id", title: "account_id" },
        { key: "nickname", title: "nickname" },
        { key: "gold", title: "gold" },
        { key: "gem", title: "gem" },
        { key: "status", title: "状态" },
        { key: "muted_until", title: "禁言到期" },
        { key: "last_login_at", title: "最后登录时间" },
        { key: "actions", title: "操作" }
      ];
      $("resultHead").innerHTML = columns.map((column) => {
        if (column.key === "select" || column.key === "actions") {
          return "<th>" + column.title + "</th>";
        }
        return "<th class=\"sortable\" data-sort-key=\"" + column.key + "\">" +
          column.title + playerSheetSortMarker(column.key) + "</th>";
      }).join("");
      $("resultBody").innerHTML = rows.slice(0, 50).map((item) => {
        const cells = [
          tableCell("<input type=\"checkbox\" data-select-player=\"" + escapeHtml(item.player_id || "") + "\"" +
            (state.selectedPlayers.includes(item.player_id) ? " checked" : "") + " />"),
          tableCell(codeValue(item.player_id)),
          tableCell(codeValue(item.account_id)),
          tableCell(escapeHtml(shortText(item.nickname, 24))),
          tableCell(codeValue(item.gold)),
          tableCell(codeValue(item.gem)),
          tableCell(playerSheetStatusBadge(item.status || "")),
          tableCell(item.muted_until
            ? "<code>" + escapeHtml(parseUnixSeconds(item.muted_until) || String(item.muted_until || "")) + "</code>"
            : "<span class=\"muted\">未禁言</span>"),
          tableCell(item.last_login_at
            ? "<code>" + escapeHtml(parseUnixSeconds(item.last_login_at) || String(item.last_login_at || "")) + "</code>"
            : "<span class=\"muted\">无</span>"),
          tableCell(bindActionsHtml([
            { action: "fill-player", label: "带入玩家", data: { player_id: item.player_id, account_id: item.account_id, nickname: item.nickname } },
            { action: "query-inventory", label: "查背包", alt: true, data: { player_id: item.player_id } },
            { action: "prepare-mail", label: "发邮件", data: { player_id: item.player_id } },
            { action: "apply-row-status", label: "行状态", alt: true, data: { account_id: item.account_id || "", player_id: item.player_id || "" } },
            { action: "apply-row-mute", label: "行禁言", data: { player_id: item.player_id || "" } }
          ]))
        ].join("");
        return "<tr>" + cells + "</tr>";
      }).join("");
    }
)HTML";

    static constexpr const char* kPart4 = R"HTML(
    function renderTable(title, payload) {
      const items = extractItems(payload);
      if (!items.length) {
        renderEmptyTable("当前接口没有返回 items 数组。");
        return;
      }

      if (isPlayerSheetTitle(title)) {
        renderPlayerSheet(items);
        return;
      }

      const tableConfigs = [
        {
          match: (name) => name.includes("查询邮件列表"),
          hint: "邮件列表突出 mail_id、读/领/删状态和奖励数，方便运营排查。",
          columns: [
            {
              title: "选择",
              render: (item) => "<input type=\"checkbox\" data-select-mail=\"" + escapeHtml(item.mail_id || "") + "\" data-mail-player=\"" + escapeHtml(item.player_id || "") + "\" data-mail-title=\"" + escapeHtml(item.title || "") + "\"" +
                (state.selectedMails.some((value) => value.mail_id === item.mail_id) ? " checked" : "") + " />"
            },
            { title: "mail_id", render: (item) => codeValue(item.mail_id) },
            { title: "player_id", render: (item) => codeValue(item.player_id) },
            { title: "标题", render: (item) => escapeHtml(shortText(item.title, 20)) },
            { title: "发送人", render: (item) => escapeHtml(shortText(item.sender_name, 16)) },
            { title: "状态", render: (item) => escapeHtml("读:" + item.read + " / 领:" + item.claimed + " / 删:" + item.deleted) },
            { title: "附件数", render: (item) => codeValue(Array.isArray(item.rewards) ? item.rewards.length : 0) },
            {
              title: "快捷操作",
              render: (item) => bindActionsHtml([
                { action: "fill-mail-player", label: "带入玩家", data: { player_id: item.player_id } },
                { action: "show-mail-rewards", label: "看附件", alt: true, data: { player_id: item.player_id, title: item.title || "", rewards: JSON.stringify(item.rewards || []) } }
              ])
            }
          ]
        },
)HTML";

    static constexpr const char* kPart5 = R"HTML(
        {
          match: (name) => name.includes("查询奖励流水"),
          hint: "奖励流水会突出来源类型、来源单号和奖励内容。",
          columns: [
            { title: "grant_id", render: (item) => codeValue(item.grant_id) },
            { title: "player_id", render: (item) => codeValue(item.player_id) },
            { title: "source_type", render: (item) => codeValue(item.source_type) },
            { title: "source_id", render: (item) => codeValue(item.source_id) },
            { title: "奖励数", render: (item) => codeValue(Array.isArray(item.rewards) ? item.rewards.length : 0) },
            { title: "granted_at", render: (item) => codeValue(item.granted_at) },
            {
              title: "快捷操作",
              render: (item) => bindActionsHtml([
                { action: "fill-grant-player", label: "带入玩家", data: { player_id: item.player_id || "" } },
                { action: "fill-grant-source", label: "带入来源", alt: true, data: { source_type: item.source_type || "", source_id: item.source_id || "" } },
                { action: "show-grant-rewards", label: "看奖励", data: { player_id: item.player_id || "", rewards: JSON.stringify(item.rewards || []), source_type: item.source_type || "", source_id: item.source_id || "" } }
              ])
            }
          ]
        },
        {
          match: (name) => name.includes("查询聊天记录"),
          hint: "聊天记录会突出发送人、消息类型和消息正文。",
          columns: [
            { title: "message_id", render: (item) => codeValue(item.message_id) },
            { title: "sender_player_id", render: (item) => codeValue(item.sender_player_id) },
            { title: "sender_nickname", render: (item) => escapeHtml(shortText(item.sender_nickname, 16)) },
            { title: "kind", render: (item) => codeValue(item.kind) },
            { title: "text", render: (item) => escapeHtml(shortText(item.text, 60)) },
            { title: "sent_at", render: (item) => codeValue(item.sent_at) },
            {
              title: "快捷操作",
              render: (item) => bindActionsHtml([
                { action: "fill-chat-sender", label: "带入玩家", data: { player_id: item.sender_player_id || "" } },
                { action: "show-chat-message", label: "看详情", alt: true, data: { sender_player_id: item.sender_player_id || "", sender_nickname: item.sender_nickname || "", kind: item.kind || "", text: item.text || "", sent_at: item.sent_at || "", message_id: item.message_id || "" } }
              ])
            }
          ]
        },
        {
          match: (name) => name.includes("查询封禁记录"),
          hint: "封禁记录会突出封禁对象、原因和创建时间。",
          columns: [
            { title: "entry_id", render: (item) => codeValue(item.entry_id) },
            { title: "subject_type", render: (item) => codeValue(item.subject_type) },
            { title: "subject_value", render: (item) => codeValue(item.subject_value) },
            { title: "reason", render: (item) => escapeHtml(shortText(item.reason, 28)) },
            { title: "expires_at", render: (item) => codeValue(item.expires_at) },
            { title: "created_at", render: (item) => codeValue(item.created_at) }
          ]
        },
        {
          match: (name) => name.includes("查询审计日志"),
          hint: "审计日志会突出操作人、动作、成功状态和目标对象。",
          columns: [
            {
              title: "选择",
              render: (item) => "<input type=\"checkbox\" data-select-audit=\"" + escapeHtml(item.audit_id || "") + "\" data-audit-operator=\"" + escapeHtml(item.operator_id || "") + "\" data-audit-target-type=\"" + escapeHtml(item.target_type || "") + "\" data-audit-target-id=\"" + escapeHtml(item.target_id || "") + "\"" +
                (state.selectedAudits.some((value) => value.audit_id === item.audit_id) ? " checked" : "") + " />"
            },
            { title: "audit_id", render: (item) => codeValue(item.audit_id) },
            { title: "operator_id", render: (item) => codeValue(item.operator_id) },
            { title: "action", render: (item) => escapeHtml(shortText(item.action, 24)) },
            { title: "target", render: (item) => escapeHtml(shortText((item.target_type || "") + ":" + (item.target_id || ""), 30)) },
            { title: "success", render: (item) => codeValue(item.success) },
            { title: "created_at", render: (item) => codeValue(item.created_at) },
            {
              title: "快捷操作",
              render: (item) => bindActionsHtml([
                { action: "fill-audit-operator", label: "带入操作人", data: { operator_id: item.operator_id || "" } },
                { action: "fill-audit-target", label: "带入目标", alt: true, data: { target_type: item.target_type || "", target_id: item.target_id || "" } }
              ])
            }
          ]
        },
        {
          match: (name) => name.includes("查询管理员列表"),
          hint: "管理员列表会突出账号、操作人、启用状态和权限数。",
          columns: [
            { title: "account_id", render: (item) => codeValue(item.account_id) },
            { title: "operator_name", render: (item) => escapeHtml(shortText(item.operator_name, 18)) },
            { title: "active", render: (item) => codeValue(item.active) },
            { title: "权限数", render: (item) => codeValue(Array.isArray(item.permissions) ? item.permissions.length : 0) },
            { title: "updated_at", render: (item) => codeValue(item.updated_at) },
            {
              title: "快捷操作",
              render: (item) => bindActionsHtml([
                { action: "fill-account", label: "带入账号", data: { account_id: item.account_id } }
              ])
            }
          ]
        },
        {
          match: (name) => name.includes("查询任务列表"),
          hint: "任务列表会突出类型、状态、执行统计，并支持把结果展开到右侧卡片。",
          columns: [
            { title: "task_id", render: (item) => codeValue(item.task_id) },
            { title: "kind", render: (item) => codeValue(item.kind) },
            { title: "status", render: (item) => codeValue(item.status) },
            { title: "target", render: (item) => escapeHtml(shortText((item.target_type || "") + ":" + (item.target_id || ""), 30)) },
            { title: "请求预览", render: (item) => escapeHtml(shortText(summarizeTaskRequest(item.request_json || ""), 36)) },
            {
              title: "统计",
              render: (item) => {
                const summary = parseTaskResultSummary(item.result_json || "");
                if (!summary.raw) {
                  return "<code>pending</code>";
                }
                const success = summary.success ?? "-";
                const failure = summary.failure ?? "-";
                return "<code>S:" + escapeHtml(String(success)) + " / F:" + escapeHtml(String(failure)) + "</code>";
              }
            },
            {
              title: "失败预览",
              render: (item) => {
                const summary = parseTaskResultSummary(item.result_json || "");
                const preview = collectTaskFailurePreview(summary.raw, 1);
                if (!preview.length) {
                  return "<span class=\"muted\">无</span>";
                }
                return "<code>" + escapeHtml(shortText(JSON.stringify(preview[0]), 42)) + "</code>";
              }
            },
            { title: "execute_at", render: (item) => codeValue(item.execute_at) },
            { title: "executed_at", render: (item) => codeValue(item.executed_at) },
            {
              title: "快捷操作",
              render: (item) => bindActionsHtml([
                { action: "show-task-result", label: "看结果", data: { task_id: item.task_id || "", kind: item.kind || "", status: item.status || "", result_json: item.result_json || "", request_json: item.request_json || "", failure_reason: item.failure_reason || "", target_id: item.target_id || "" } },
                { action: "fill-task-kind-failed", label: "查失败同类", alt: true, data: { kind: item.kind || "" } }
              ])
            }
          ]
        },
        {
          match: (name) => name.includes("查询 Tracing"),
          hint: "Tracing 列表会先按 trace_id 聚合，便于从单条请求或后台任务追整条 span 链路。",
          columns: [
            { title: "trace_id", render: (item) => codeValue(item.trace_id) },
            { title: "span_count", render: (item) => codeValue(item.span_count) },
            { title: "root_count", render: (item) => codeValue(item.root_count) },
            { title: "attribute_count", render: (item) => codeValue(item.attr_count) },
            { title: "status", render: (item) => codeValue(item.status) },
            { title: "started_at", render: (item) => codeValue(parseUnixSeconds(item.started_at) || item.started_at) },
            {
              title: "快捷操作",
              render: (item) => bindActionsHtml([
                { action: "show-trace-tree", label: "看链路", data: { trace_id: item.trace_id || "" } },
                { action: "fill-trace-id", label: "带入 trace", alt: true, data: { trace_id: item.trace_id || "" } },
                { action: "query-trace-id", label: "只看这个", data: { trace_id: item.trace_id || "" } }
              ])
            }
          ]
        }
      ];

      if (title.includes("查询 Tracing")) {
        renderCustomTable(tableConfigs[tableConfigs.length - 1], groupTraceSpans(items));
        return;
      }

      const config = tableConfigs.find((entry) => entry.match(title));
      if (config) {
        renderCustomTable(config, items);
        return;
      }
      renderGenericTable(payload);
    }
)HTML";

    static constexpr const char* kPart6 = R"HTML(
    function renderObjectCard(title, payload) {
      const cardConfigs = [
        {
          match: (name, value) => name.includes("查账号") && value && value.account_id,
          title: "账号卡片",
          meta: "适合快速 review 登录身份、状态和登录风控字段。",
          fields: (value) => [
            { key: "account_id", value: value.account_id },
            { key: "login_name", value: value.login_name },
            { key: "display_name", value: value.display_name },
            { key: "status", value: value.status },
            { key: "status_reason", value: value.status_reason },
            { key: "failed_login_attempts", value: value.failed_login_attempts },
            { key: "last_login_ip", value: value.last_login_ip },
            { key: "last_login_at", value: value.last_login_at }
          ],
          actions: (value) => [
            { action: "fill-account", label: "带入账号", data: { account_id: value.account_id } }
          ]
        },
        {
          match: (name, value) => name.includes("查玩家") && value && value.player_id,
          title: "玩家卡片",
          meta: "适合快速 review 玩家主资料和基础资产。",
          fields: (value) => [
            { key: "player_id", value: value.player_id },
            { key: "account_id", value: value.account_id },
            { key: "nickname", value: value.nickname },
            { key: "gold", value: value.gold },
            { key: "gem", value: value.gem },
            { key: "status", value: value.status || "" },
            { key: "muted_until", value: value.muted_until || "" },
            { key: "last_login_at", value: value.last_login_at || "" },
            { key: "currencies", value: JSON.stringify(value.currencies || []) },
            { key: "inventory_size", value: Array.isArray(value.inventory) ? value.inventory.length : 0 }
          ],
          actions: (value) => [
            { action: "fill-player", label: "带入玩家", data: { player_id: value.player_id, account_id: value.account_id, nickname: value.nickname } },
            { action: "query-inventory", label: "查背包", alt: true, data: { player_id: value.player_id } },
            { action: "prepare-mail", label: "发邮件", data: { player_id: value.player_id } },
            { action: "prepare-mute", label: "禁言", alt: true, data: { player_id: value.player_id } }
          ]
        },
        {
          match: (name, value) => name.includes("查背包") && value && value.player_id,
          title: "背包卡片",
          meta: "适合直接 review 某个玩家的库存结构。",
          fields: (value) => [
            { key: "player_id", value: value.player_id },
            { key: "inventory_count", value: Array.isArray(value.inventory) ? value.inventory.length : 0 },
            { key: "inventory_preview", value: JSON.stringify((value.inventory || []).slice(0, 5)) }
          ],
          actions: (value) => [
            { action: "fill-player-only", label: "带入玩家", data: { player_id: value.player_id } },
            { action: "prepare-mail", label: "发邮件", alt: true, data: { player_id: value.player_id } }
          ]
        },
        {
          match: (name, value) => name.includes("提交批量") && value && value.task_id,
          title: "任务提交卡片",
          meta: "当前返回的是新建任务本身，适合立刻记下 task_id 并去任务列表追踪。",
          fields: (value) => [
            { key: "task_id", value: value.task_id },
            { key: "kind", value: value.kind },
            { key: "status", value: value.status },
            { key: "target_type", value: value.target_type },
            { key: "target_id", value: value.target_id },
            { key: "trace_id", value: value.trace_id }
          ],
          actions: (value) => [
            { action: "fill-task-kind", label: "带入任务类型", data: { kind: value.kind || "" } },
            { action: "show-task-result", label: "看结果", alt: true, data: { task_id: value.task_id || "", kind: value.kind || "", status: value.status || "", result_json: value.result_json || "", request_json: value.request_json || "", failure_reason: value.failure_reason || "", target_id: value.target_id || "" } },
            { action: "query-trace-id", label: "看链路", data: { trace_id: value.trace_id || "" } }
          ]
        }
      ];

      const config = cardConfigs.find((entry) => entry.match(title, payload));
      if (config) {
        setDetailCard(config.title, config.meta, config.fields(payload), config.actions(payload));
        return;
      }
      renderDefaultDetail(payload);
    }

    function escapeHtml(value) {
      return String(value ?? "")
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll("\"", "&quot;")
        .replaceAll("'", "&#39;");
    }

    function updateSummary(title, result) {
      state.lastTableTitle = title;
      state.lastTablePayload = result;
      const payload = normalizeEnvelope(result);
      const items = extractItems(payload);
      if (Array.isArray(payload)) {
        setSummary(title, "返回数组 " + payload.length + " 条");
      } else if (payload && typeof payload === "object" && "total" in payload) {
        setSummary(title, "total=" + payload.total + "，当前页 " + items.length + " 条");
      } else {
        setSummary(title, "返回单对象结果");
      }
      renderObjectCard(title, payload);
      renderTable(title, payload);
      if (title.includes("查询 Tracing")) {
        const traces = groupTraceSpans(extractItems(payload));
        if (traces.length) {
          showTraceDetail(traces[0].trace_id);
        }
      }
    }

    function rerenderLastTable() {
      if (!state.lastTableTitle || !state.lastTablePayload) {
        return;
      }
      renderTable(state.lastTableTitle, normalizeEnvelope(state.lastTablePayload));
    }

    async function run(title, fn) {
      try {
        const result = await fn();
        setOutput(title + " 成功", result);
        updateSummary(title, result);
      } catch (error) {
        const message =
          error?.data?.error?.message ||
          error?.data?.message ||
          error?.data?.error?.code ||
          error?.message ||
          error?.status ||
          "未知错误";
        setOutput(title + " 失败", error);
        if ($("sheetStatusLine")) {
          $("sheetStatusLine").textContent = title + " 失败：" + message;
        }
        setSummary(title + " 失败", "请查看右侧原始响应");
        if (title.includes("玩家")) {
          $("sheetPlayerBody").innerHTML =
            "<tr><td class=\"row-index\">1</td><td colspan=\"9\" class=\"muted\">" +
            escapeHtml(title + " 失败：" + message + "。请先点“检查身份”，确认管理员权限有效。") +
            "</td></tr>";
        } else {
          renderEmptyTable("请求失败，请查看右侧原始响应。");
        }
      }
    }
)HTML";

    static constexpr const char* kPart6a = R"HTML(
    function handleActionClick(event) {
      const button = event.target.closest("[data-action]");
      if (!button) {
        return;
      }
      const action = button.dataset.action;
      if (action === "remove-reward-row") {
        const prefix = button.dataset.prefix || "mail";
        const index = Number(button.dataset.index || -1);
        if (state.rewardRows[prefix] && index >= 0) {
          state.rewardRows[prefix].splice(index, 1);
          renderRewardRows(prefix);
        }
        return;
      }
      if (action === "fill-player") {
        $("queryPlayerId").value = button.dataset.player_id || "";
        $("mailPlayerId").value = button.dataset.player_id || "";
        $("mailQueryPlayerId").value = button.dataset.player_id || "";
        $("grantPlayerId").value = button.dataset.player_id || "";
        $("mutedPlayerId").value = button.dataset.player_id || "";
        $("queryAccountId").value = button.dataset.account_id || "";
        $("playersNickname").value = button.dataset.nickname || "";
        return;
      }
      if (action === "fill-player-only") {
        $("queryPlayerId").value = button.dataset.player_id || "";
        $("mailPlayerId").value = button.dataset.player_id || "";
        $("mailQueryPlayerId").value = button.dataset.player_id || "";
        $("grantPlayerId").value = button.dataset.player_id || "";
        return;
      }
      if (action === "fill-mail-player") {
        $("mailPlayerId").value = button.dataset.player_id || "";
        $("mailQueryPlayerId").value = button.dataset.player_id || "";
        $("grantPlayerId").value = button.dataset.player_id || "";
        return;
      }
      if (action === "query-inventory") {
        $("queryPlayerId").value = button.dataset.player_id || "";
        $("queryInventoryBtn").click();
        return;
      }
      if (action === "prepare-mail") {
        $("mailPlayerId").value = button.dataset.player_id || "";
        $("mailQueryPlayerId").value = button.dataset.player_id || "";
        return;
      }
      if (action === "prepare-mute") {
        $("mutedPlayerId").value = button.dataset.player_id || "";
        return;
      }
      if (action === "apply-row-status") {
        run("玩家行内状态变更", async () => {
          const result = await api("/auth/admin/account-status", {
            method: "POST",
            headers: authHeaders(),
            body: JSON.stringify({
              account_id: button.dataset.account_id || "",
              status: $("playersQuickStatusValue").value,
              reason: $("playersQuickStatusReason").value.trim()
            })
          });
          if (isPlayerSheetTitle(state.lastTableTitle)) {
            setTimeout(() => $("playersQueryBtn").click(), 250);
          }
          return result;
        });
        return;
      }
      if (action === "apply-row-mute") {
        run("玩家行内大厅禁言", async () => {
          const result = await api("/chat/mute", {
            method: "POST",
            headers: authHeaders(),
            body: JSON.stringify({
              channel_kind: "lobby",
              scope_id: $("playersQuickMuteScope").value.trim() || "global",
              muted_player_id: button.dataset.player_id || "",
              duration_seconds: Number($("playersQuickMuteSeconds").value || 3600),
              reason: $("playersQuickMuteReason").value.trim()
            })
          });
          if (isPlayerSheetTitle(state.lastTableTitle)) {
            setTimeout(() => $("playersQueryBtn").click(), 250);
          }
          return result;
        });
        return;
      }
      if (action === "show-mail-rewards") {
        $("mailQueryPlayerId").value = button.dataset.player_id || "";
        const title = button.dataset.title || "邮件附件";
        let rewards = [];
        try {
          rewards = JSON.parse(button.dataset.rewards || "[]");
        } catch (_) {}
        setDetailCard("邮件附件卡片", "来自邮件列表行内操作，方便直接 review 奖励结构。", [
          { key: "player_id", value: button.dataset.player_id || "" },
          { key: "title", value: title },
          { key: "reward_count", value: Array.isArray(rewards) ? rewards.length : 0 },
          { key: "rewards", value: JSON.stringify(rewards) }
        ], [
          { action: "fill-mail-player", label: "带入玩家", data: { player_id: button.dataset.player_id || "" } }
        ]);
        return;
      }
      if (action === "fill-audit-operator") {
        $("auditOperatorId").value = button.dataset.operator_id || "";
        return;
      }
      if (action === "fill-audit-target") {
        const targetType = button.dataset.target_type || "";
        const targetId = button.dataset.target_id || "";
        if (targetType === "account") {
          $("queryAccountId").value = targetId;
          $("statusAccountId").value = targetId;
        } else if (targetType === "player") {
          $("queryPlayerId").value = targetId;
          $("mailPlayerId").value = targetId;
          $("mailQueryPlayerId").value = targetId;
          $("grantPlayerId").value = targetId;
          $("mutedPlayerId").value = targetId;
        } else if (targetType === "channel") {
          $("chatQueryScope").value = targetId;
        }
        return;
      }
      if (action === "fill-grant-player") {
        $("queryPlayerId").value = button.dataset.player_id || "";
        $("mailPlayerId").value = button.dataset.player_id || "";
        $("mailQueryPlayerId").value = button.dataset.player_id || "";
        $("grantPlayerId").value = button.dataset.player_id || "";
        return;
      }
      if (action === "fill-grant-source") {
        $("grantSourceType").value = button.dataset.source_type || "";
        $("grantSourceId").value = button.dataset.source_id || "";
        return;
      }
      if (action === "show-grant-rewards") {
        let rewards = [];
        try {
          rewards = JSON.parse(button.dataset.rewards || "[]");
        } catch (_) {}
        setDetailCard("奖励流水卡片", "来自奖励流水行内操作，方便直接 review 这笔奖励的来源和内容。", [
          { key: "player_id", value: button.dataset.player_id || "" },
          { key: "source_type", value: button.dataset.source_type || "" },
          { key: "source_id", value: button.dataset.source_id || "" },
          { key: "reward_count", value: Array.isArray(rewards) ? rewards.length : 0 },
          { key: "rewards", value: JSON.stringify(rewards) }
        ], [
          { action: "fill-grant-player", label: "带入玩家", data: { player_id: button.dataset.player_id || "" } },
          { action: "fill-grant-source", label: "带入来源", alt: true, data: { source_type: button.dataset.source_type || "", source_id: button.dataset.source_id || "" } }
        ]);
        return;
      }
      if (action === "fill-chat-sender") {
        $("queryPlayerId").value = button.dataset.player_id || "";
        $("mailPlayerId").value = button.dataset.player_id || "";
        $("mailQueryPlayerId").value = button.dataset.player_id || "";
        $("grantPlayerId").value = button.dataset.player_id || "";
        $("mutedPlayerId").value = button.dataset.player_id || "";
        return;
      }
      if (action === "show-chat-message") {
        setDetailCard("聊天消息卡片", "来自聊天记录行内操作，方便单独检查这条消息。", [
          { key: "message_id", value: button.dataset.message_id || "" },
          { key: "sender_player_id", value: button.dataset.sender_player_id || "" },
          { key: "sender_nickname", value: button.dataset.sender_nickname || "" },
          { key: "kind", value: button.dataset.kind || "" },
          { key: "sent_at", value: button.dataset.sent_at || "" },
          { key: "text", value: button.dataset.text || "" }
        ], [
          { action: "fill-chat-sender", label: "带入玩家", data: { player_id: button.dataset.sender_player_id || "" } }
        ]);
        return;
      }
      if (action === "fill-account") {
        $("queryAccountId").value = button.dataset.account_id || "";
        $("statusAccountId").value = button.dataset.account_id || "";
        return;
      }
      if (action === "fill-task-kind") {
        $("taskKind").value = button.dataset.kind || "";
        return;
      }
      if (action === "fill-task-kind-failed") {
        $("taskKind").value = button.dataset.kind || "";
        $("taskStatus").value = "failed";
        $("tasksQueryBtn").click();
        return;
      }
      if (action === "fill-item-config") {
        fillItemConfigForm(state.itemConfigs[Number(button.dataset.index || 0)]);
        return;
      }
      if (action === "fill-gacha-pool") {
        fillGachaPoolForm(findGachaPool(button.dataset.pool_id || ""));
        return;
      }
      if (action === "fill-gacha-item") {
        fillGachaPoolForm(findGachaPool(button.dataset.pool_id || ""));
        fillGachaItemForm(findGachaItem(button.dataset.pool_id || "", button.dataset.item_key || ""));
        return;
      }
      if (action === "delete-gacha-pool") {
        const poolId = button.dataset.pool_id || "";
        const name = button.dataset.name || "";
        if (!window.confirm("确认删除卡池 " + poolId + (name ? "（" + name + "）" : "") + "？该卡池的奖品配置也会一起删除，抽卡历史会保留。")) {
          return;
        }
        run("删除抽卡卡池", async () => {
          const result = await api("/admin/gacha/pools/delete", {
            method: "POST",
            headers: authHeaders(),
            body: JSON.stringify({ pool_id: poolId })
          });
          if ($("gachaPoolId").value.trim() === poolId) {
            $("gachaPoolId").value = "";
          }
          await queryGachaConfigs();
          return result;
        });
        return;
      }
      if (action === "delete-gacha-item") {
        const poolId = button.dataset.pool_id || "";
        const itemKey = button.dataset.item_key || "";
        if (!itemKey || !window.confirm("确认从卡池 " + poolId + " 删除奖品 " + itemKey + "？")) {
          return;
        }
        run("删除抽卡奖品", async () => {
          const result = await api("/admin/gacha/items/delete", {
            method: "POST",
            headers: authHeaders(),
            body: JSON.stringify({ pool_id: poolId, item_key: itemKey })
          });
          await queryGachaConfigs();
          return result;
        });
        return;
      }
      if (action === "use-gacha-reward-item") {
        fillGachaRewardItemById(button.dataset.item_id || "", true);
        return;
      }
      if (action === "add-gacha-reward-item") {
        fillGachaRewardItemById(button.dataset.item_id || "", true);
        run("加入抽卡奖品", saveGachaItemConfig);
        return;
      }
      if (action === "fill-shop-product") {
        fillShopProductForm(findShopProduct(button.dataset.product_id || ""));
        return;
      }
      if (action === "delete-shop-product") {
        const productId = button.dataset.product_id || "";
        const name = button.dataset.name || "";
        if (!productId || !window.confirm("确认删除商品 " + productId + (name ? "（" + name + "）" : "") + "？购买流水会保留。")) {
          return;
        }
        run("删除商城商品", async () => {
          const result = await api("/admin/shop/products/delete", {
            method: "POST",
            headers: authHeaders(),
            body: JSON.stringify({ product_id: productId })
          });
          if ($("shopProductId").value.trim() === productId) {
            $("shopProductId").value = "";
          }
          await queryShopProducts();
          return result;
        });
        return;
      }
      if (action === "fill-trace-id") {
        $("traceQueryId").value = button.dataset.trace_id || "";
        $("traceViewerId").value = button.dataset.trace_id || "";
        return;
      }
      if (action === "query-trace-id") {
        $("traceQueryId").value = button.dataset.trace_id || "";
        $("traceViewerId").value = button.dataset.trace_id || "";
        $("traceQueryBtn").click();
        return;
      }
      if (action === "show-trace-tree") {
        showTraceDetail(button.dataset.trace_id || "", button.dataset.span_id || "");
        return;
      }
      if (action === "show-span-detail") {
        showSpanDetail(button.dataset.trace_id || "", button.dataset.span_id || "");
        return;
      }
      if (action === "show-task-result") {
        const summary = parseTaskResultSummary(button.dataset.result_json || "");
        const failedItems = collectTaskFailurePreview(summary.raw, 5);
        setDetailCard("任务结果卡片", "把后台任务的汇总统计和前几条失败项直接展开，便于快速判断是否需要重跑。", [
          { key: "task_id", value: button.dataset.task_id || "" },
          { key: "kind", value: button.dataset.kind || "" },
          { key: "status", value: button.dataset.status || "" },
          { key: "request_preview", value: summarizeTaskRequest(button.dataset.request_json || "") },
          { key: "success_count", value: summary.raw ? (summary.success ?? "(无)") : "(待执行或无结果)" },
          { key: "failure_count", value: summary.raw ? (summary.failure ?? "(无)") : "(待执行或无结果)" },
          { key: "item_count", value: summary.raw ? summary.itemCount : "(无)" },
          { key: "failure_reason", value: button.dataset.failure_reason || "(无)" },
          { key: "failed_preview", value: failedItems.length ? JSON.stringify(failedItems) : "(无失败项预览)" },
          { key: "result_preview", value: summary.raw ? shortText(JSON.stringify(summary.raw), 160) : "(无结果体)" }
        ], [
          { action: "fill-task-kind", label: "带入任务类型", data: { kind: button.dataset.kind || "" } },
          { action: "fill-task-kind-failed", label: "查失败同类", data: { kind: button.dataset.kind || "" } }
        ]);
        return;
      }
    }

 )HTML";

    static constexpr const char* kPart6a_bind = R"HTML(
    $("resultBody").onclick = handleActionClick;
    $("detailActions").onclick = handleActionClick;
    $("detailExtra").onclick = handleActionClick;
    [
      "mailRewardBody",
      "sheetRewardBody",
      "shopRewardBody",
      "itemConfigBody",
      "gachaRewardItemBody",
      "gachaPoolListBody",
      "gachaConfigBody",
      "shopProductBody"
    ].forEach((id) => {
      if ($(id)) {
        $(id).onclick = handleActionClick;
      }
    });
    $("resultHead").onclick = (event) => {
      const header = event.target.closest("[data-sort-key]");
      if (!header) {
        return;
      }
      const key = header.dataset.sortKey || "player_id";
      if (state.playerSheet.sortKey === key) {
        state.playerSheet.sortDir = state.playerSheet.sortDir === "asc" ? "desc" : "asc";
      } else {
        state.playerSheet.sortKey = key;
        state.playerSheet.sortDir = "asc";
      }
      rerenderLastTable();
    };

    $("resultBody").onchange = (event) => {
      const checkbox = event.target.closest("[data-select-player]");
      if (checkbox) {
        setSelectedPlayer(checkbox.dataset.selectPlayer || "", checkbox.checked);
        return;
      }
      const mailCheckbox = event.target.closest("[data-select-mail]");
      if (mailCheckbox) {
        if (mailCheckbox.checked) {
          upsertSelectedMail({
            mail_id: mailCheckbox.dataset.selectMail || "",
            player_id: mailCheckbox.dataset.mailPlayer || "",
            title: mailCheckbox.dataset.mailTitle || ""
          });
        } else {
          removeSelectedMail(mailCheckbox.dataset.selectMail || "");
        }
        return;
      }
      const auditCheckbox = event.target.closest("[data-select-audit]");
      if (auditCheckbox) {
        if (auditCheckbox.checked) {
          upsertSelectedAudit({
            audit_id: auditCheckbox.dataset.selectAudit || "",
            operator_id: auditCheckbox.dataset.auditOperator || "",
            target_type: auditCheckbox.dataset.auditTargetType || "",
            target_id: auditCheckbox.dataset.auditTargetId || ""
          });
        } else {
          removeSelectedAudit(auditCheckbox.dataset.selectAudit || "");
        }
      }
    };
)HTML";

    static constexpr const char* kPart7 = R"HTML(
    $("loginBtn").onclick = () => run("管理员登录", async () => {
      const loginName = $("sheetLoginName") ? $("sheetLoginName").value.trim() : $("loginName").value.trim();
      const password = $("sheetLoginPassword") ? $("sheetLoginPassword").value : $("loginPassword").value;
      $("loginName").value = loginName;
      $("loginPassword").value = password;
      const result = await api("/admin/login", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          login_name: loginName,
          password,
          single_session: true
        })
      });
      const payload = normalizeEnvelope(result);
      const token = payload?.session?.token || "";
      if (!token) {
        throw { status: 500, data: { message: "管理员登录成功但响应里没有 session.token，无法保存登录态" } };
      }
      state.token = token;
      state.adminName = payload?.admin?.operator_name || "";
      localStorage.setItem("hello_admin_token", state.token);
      localStorage.setItem("hello_admin_name", state.adminName);
      refreshHeader();
      if ($("sheetPlayerBody")) {
        setTimeout(() => run("自动加载全部玩家", sheetQueryPlayers), 0);
      }
      return result;
    });

    $("logoutBtn").onclick = () => {
      state.token = "";
      state.adminName = "";
      state.selectedPlayers = [];
      state.selectedMails = [];
      state.selectedAudits = [];
      localStorage.removeItem("hello_admin_token");
      localStorage.removeItem("hello_admin_name");
      refreshHeader();
      syncSelectionView();
      setOutput("本地 Token 已清空", { ok: true });
      setSummary("本地 Token 已清空", "等待下一次查询");
      renderEmptyTable("等待下一次查询。");
    };
)HTML";

    static constexpr const char* kPart8 = R"HTML(

    $("meBtn").onclick = () => run("读取当前管理员", async () => {
      const result = await api("/admin/me", {
        method: "GET",
        headers: authHeaders()
      });
      state.adminName = result?.data?.admin?.operator_name || state.adminName;
      localStorage.setItem("hello_admin_name", state.adminName);
      refreshHeader();
      return result;
    });

    if ($("sheetLoginBtn")) $("sheetLoginBtn").onclick = () => $("loginBtn").click();
    if ($("sheetMeBtn")) $("sheetMeBtn").onclick = () => $("meBtn").click();

    $("queryAccountBtn").onclick = () => run("查账号", async () => {
      const payload = {};
      addIfPresent(payload, "account_id", $("queryAccountId").value.trim());
      addIfPresent(payload, "login_name", $("queryLoginName").value.trim());
      return api("/admin/query/account", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify(payload)
      });
    });

    $("queryPlayerBtn").onclick = () => run("查玩家", async () => {
      return api("/admin/query/player", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          player_id: $("queryPlayerId").value.trim()
        })
      });
    });

    $("queryInventoryBtn").onclick = () => run("查背包", async () => {
      return api("/admin/query/inventory", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          player_id: $("queryPlayerId").value.trim()
        })
      });
    });

    function playerListPayload() {
      const payload = {
        limit: Number($("playersLimit").value || 20),
        offset: Number($("playersOffset").value || 0)
      };
      addIfPresent(payload, "player_id", $("playersPlayerId").value.trim());
      addIfPresent(payload, "account_id", $("playersAccountId").value.trim());
      addIfPresent(payload, "nickname", $("playersNickname").value.trim());
      return payload;
    }

    async function requestPlayersQuery() {
      return api("/admin/query/players", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify(playerListPayload())
      });
    }

    $("playersQueryBtn").onclick = () => run("查询玩家列表", requestPlayersQuery);

    $("playersNextBtn").onclick = () => {
      $("playersOffset").value = Number($("playersOffset").value || 0) + Number($("playersLimit").value || 20);
      $("playersQueryBtn").click();
    };

    $("playersPrevBtn").onclick = () => {
      const nextOffset = Number($("playersOffset").value || 0) - Number($("playersLimit").value || 20);
      $("playersOffset").value = Math.max(0, nextOffset);
      $("playersQueryBtn").click();
    };

    $("gmQueryPlayersBtn").onclick = () => $("playersQueryBtn").click();

    $("gmClearSelectionBtn").onclick = () => $("clearSelectionBtn").click();

    $("gmMuteSelectedBtn").onclick = () => $("playersQuickApplyMuteBtn").click();

    $("gmStatusSelectedBtn").onclick = () => $("playersQuickApplyStatusBtn").click();

    $("mailAddRewardBtn").onclick = () => run("添加邮件奖励行", async () => {
      addRewardFromInputs("mail");
      return { rewards: state.rewardRows.mail };
    });

    $("mailClearRewardsBtn").onclick = () => {
      state.rewardRows.mail = [];
      renderRewardRows("mail");
    };

    $("sheetAddRewardBtn").onclick = () => run("添加表格邮件奖励行", async () => {
      addRewardFromInputs("sheet");
      return { rewards: state.rewardRows.sheet };
    });

    $("sheetClearRewardsBtn").onclick = () => {
      state.rewardRows.sheet = [];
      renderRewardRows("sheet");
    };

    $("gmMailSelectedBtn").onclick = () => run("GM 发邮件给勾选玩家", async () => {
      const playerIds = uniqueSelectedPlayerIds();
      $("mailPlayerIds").value = playerIds.join(",");
      return sendGmMailToPlayerIds(playerIds);
    });

    $("gmMailAllBtn").onclick = () => run("GM 一键发邮件给全部玩家", async () => {
      $("gmMailMode").value = "all";
      const playerIds = await queryAllPlayerIdsForGmMail();
      $("mailPlayerIds").value = playerIds.join(",");
      return sendGmMailToPlayerIds(playerIds);
    });

    $("mailAllBtn").onclick = () => $("gmMailAllBtn").click();

    $("sheetPlayersBtn").onclick = () => run("表格查询玩家", sheetQueryPlayers);

    $("sheetPlayerBody").onclick = (event) => {
      const row = event.target.closest("[data-sheet-player]");
      if (!row) {
        return;
      }
      const player = state.sheetPlayers.find((item) => item.player_id === row.dataset.sheetPlayer);
      if (!player) {
        return;
      }
      $("sheetPlayerBody").querySelectorAll("tr").forEach((item) => item.classList.remove("active"));
      row.classList.add("active");
      renderSheetSelected(player);
      addSelectedPlayer(player.player_id);
      run("读取玩家仓库", sheetLoadInventory);
    };

    $("sheetClearBtn").onclick = () => {
      state.selectedPlayers = [];
      $("sheetLookupId").value = "";
      renderSheetSelected(null);
      renderSheetInventory([]);
      syncSelectionView();
      $("sheetPlayerBody").querySelectorAll("tr").forEach((item) => item.classList.remove("active"));
    };

    $("sheetLoadInventoryBtn").onclick = () => run("读取玩家仓库", sheetLoadInventory);

    $("sheetMuteBtn").onclick = () => run("表格禁言玩家", async () => {
      const playerIds = sheetSelectedPlayerIds();
      if (!playerIds.length) {
        throw { status: 400, data: { message: "请先选择玩家" } };
      }
      const results = [];
      for (const playerId of playerIds) {
        results.push(await api("/chat/mute", {
          method: "POST",
          headers: authHeaders(),
          body: JSON.stringify({
            channel_kind: "lobby",
            scope_id: "global",
            muted_player_id: playerId,
            duration_seconds: Number($("sheetMuteSeconds").value || 3600),
            reason: $("sheetMuteReason").value.trim()
          })
        }));
      }
      return { player_ids: playerIds, results };
    });

    $("sheetUnmuteBtn").onclick = () => run("表格取消禁言", async () => {
      const playerIds = sheetSelectedPlayerIds();
      if (!playerIds.length) {
        throw { status: 400, data: { message: "请先选择玩家" } };
      }
      const results = [];
      for (const playerId of playerIds) {
        results.push(await api("/chat/mute/clear", {
          method: "POST",
          headers: authHeaders(),
          body: JSON.stringify({
            channel_kind: "lobby",
            scope_id: "global",
            muted_player_id: playerId
          })
        }));
      }
      return { player_ids: playerIds, results };
    });

    $("sheetStatusBtn").onclick = () => run("表格修改账号状态", async () => {
      const player = state.sheetSelectedPlayer;
      if (!player?.account_id) {
        throw { status: 400, data: { message: "请先选择玩家" } };
      }
      return api("/auth/admin/account-status", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          account_id: player.account_id,
          status: $("sheetStatusValue").value,
          reason: $("sheetStatusReason").value.trim()
        })
      });
    });

    $("sheetMailSelectedBtn").onclick = () => run("表格发送邮件", async () => {
      return sendGmMailToPlayerIds(sheetSelectedPlayerIds());
    });

    $("sheetMailAllBtn").onclick = () => run("表格一键全服邮件", async () => {
      const playerIds = await queryAllPlayerIdsForGmMail();
      return sendGmMailToPlayerIds(playerIds);
    });

    $("playersSheetApplyBtn").onclick = () => {
      if (!isPlayerSheetTitle(state.lastTableTitle)) {
        setDetailCard("本页筛选", "请先查询一次玩家列表，再对当前页应用本地筛选和排序。", [
          { key: "last_table", value: state.lastTableTitle || "(空)" }
        ]);
        return;
      }
      rerenderLastTable();
    };

    $("playersSheetResetBtn").onclick = () => {
      $("playersSheetKeyword").value = "";
      $("playersSheetStatus").value = "";
      $("playersSheetMutedOnly").value = "";
      state.playerSheet.sortKey = "player_id";
      state.playerSheet.sortDir = "asc";
      rerenderLastTable();
    };

)HTML";

    static constexpr const char* kPart8b = R"HTML(
    $("statusBtn").onclick = () => run("账号状态变更", async () => {
      return api("/auth/admin/account-status", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          account_id: $("statusAccountId").value.trim(),
          status: $("statusValue").value,
          reason: $("statusReason").value.trim()
        })
      });
    });

    $("playersQuickApplyStatusBtn").onclick = () => run("玩家主表批量改状态", async () => {
      const playerIds = uniqueSelectedPlayerIds();
      if (!playerIds.length) {
        throw { status: 400, data: { message: "no selected players" } };
      }
      const result = await api("/admin/tasks/account-status", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          player_ids: playerIds,
          status: $("playersQuickStatusValue").value,
          reason: $("playersQuickStatusReason").value.trim()
        })
      });
      if (isPlayerSheetTitle(state.lastTableTitle)) {
        setTimeout(() => $("playersQueryBtn").click(), 250);
      }
      return result;
    });

    $("playersQuickApplyMuteBtn").onclick = () => run("玩家主表批量大厅禁言", async () => {
      const playerIds = uniqueSelectedPlayerIds();
      if (!playerIds.length) {
        throw { status: 400, data: { message: "no selected players" } };
      }
      const result = await api("/admin/tasks/batch-mute", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          channel_kind: "lobby",
          scope_id: $("playersQuickMuteScope").value.trim() || "global",
          player_ids: playerIds,
          duration_seconds: Number($("playersQuickMuteSeconds").value || 3600),
          reason: $("playersQuickMuteReason").value.trim()
        })
      });
      if (isPlayerSheetTitle(state.lastTableTitle)) {
        setTimeout(() => $("playersQueryBtn").click(), 250);
      }
      return result;
    });

    $("muteBtn").onclick = () => run("提交禁言", async () => {
      return api("/chat/mute", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          ...buildChannel("mute"),
          muted_player_id: $("mutedPlayerId").value.trim(),
          duration_seconds: Number($("muteDuration").value || 3600),
          reason: $("muteReason").value.trim()
        })
      });
    });

    $("unmuteBtn").onclick = () => run("取消禁言", async () => {
      return api("/chat/mute/clear", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          ...buildChannel("mute"),
          muted_player_id: $("mutedPlayerId").value.trim()
        })
      });
    });

    $("announceBtn").onclick = () => run("发送公告", async () => {
      return api("/chat/gm/announce", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          ...buildChannel("announce"),
          text: $("announceText").value
        })
      });
    });

    $("mailBtn").onclick = () => run("发送邮件", async () => {
      const payload = {
        sender_name: $("mailSender").value.trim() || "System",
        title: $("mailTitle").value.trim() || "系统奖励邮件",
        body: $("mailBody").value.trim() || "请领取下方附件奖励。感谢你的支持，祝你游戏愉快。",
        rewards: parseRewardsOrThrow(),
        expires_in_seconds: Number($("mailExpire").value || 0)
      };
      const playerId = $("mailPlayerId").value.trim();
      const playerIds = $("mailPlayerIds").value.trim();
      if (playerIds) {
        payload.player_ids = playerIds.split(",").map((s) => s.trim()).filter(Boolean);
      } else {
        payload.player_id = playerId;
      }
      return api("/mail/send", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify(payload)
      });
    });

    $("mailQueryBtn").onclick = () => run("查询邮件列表", async () => {
      const payload = {
        player_id: $("mailQueryPlayerId").value.trim(),
        include_deleted: $("mailQueryIncludeDeleted").value === "true",
        limit: Number($("mailQueryLimit").value || 20),
        offset: Number($("mailQueryOffset").value || 0)
      };
      const claimed = parseBoolOrNull($("mailQueryClaimed").value);
      const read = parseBoolOrNull($("mailQueryRead").value);
      if (claimed !== null) payload.claimed = claimed;
      if (read !== null) payload.read = read;
      return api("/admin/query/mail", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify(payload)
      });
    });

    $("clearSelectionBtn").onclick = () => {
      state.selectedPlayers = [];
      state.selectedMails = [];
      state.selectedAudits = [];
      syncSelectionView();
      const checkboxes = $("resultBody").querySelectorAll("[data-select-player], [data-select-mail], [data-select-audit]");
      checkboxes.forEach((checkbox) => {
        checkbox.checked = false;
      });
      setDetailCard("批量勾选已清空", "当前批量目标已重置。", [
        { key: "selected_count", value: 0 },
        { key: "selected_players", value: 0 },
        { key: "selected_mails", value: 0 },
        { key: "selected_audits", value: 0 }
      ]);
    };

    $("fillSelectedMailBtn").onclick = () => {
      const playerIds = uniqueSelectedPlayerIds();
      $("mailPlayerIds").value = playerIds.join(",");
      setDetailCard("批量邮件目标已带入", "已把当前勾选玩家写入批量邮件输入框。", [
        { key: "selected_count", value: playerIds.length },
        { key: "player_ids", value: playerIds.join(",") || "(空)" }
      ]);
    };

    $("fillSelectedAuditBtn").onclick = () => {
      const operatorIds = [...new Set(state.selectedAudits.map((item) => item.operator_id).filter(Boolean))];
      const target = state.selectedAudits.find((item) => item.target_id);
      if (operatorIds.length) {
        $("auditOperatorId").value = operatorIds[0];
      }
      if (target) {
        if (target.target_type === "account") {
          $("queryAccountId").value = target.target_id;
          $("statusAccountId").value = target.target_id;
        } else if (target.target_type === "player") {
          $("queryPlayerId").value = target.target_id;
          $("mailPlayerId").value = target.target_id;
          $("mailQueryPlayerId").value = target.target_id;
          $("grantPlayerId").value = target.target_id;
        } else if (target.target_type === "channel") {
          $("chatQueryScope").value = target.target_id;
        }
      }
      setDetailCard("审计勾选已带入", "已把勾选审计记录的操作人和首个目标对象带入到筛选区。", [
        { key: "selected_audits", value: state.selectedAudits.length },
        { key: "operator_ids", value: operatorIds.join(",") || "(空)" },
        { key: "target_preview", value: target ? ((target.target_type || "") + ":" + (target.target_id || "")) : "(空)" }
      ]);
    };

    $("saveFiltersBtn").onclick = () => {
      localStorage.setItem("hello_admin_filter_preset", JSON.stringify(collectFilterPreset()));
      setDetailCard("筛选预设已保存", "当前玩家/邮件/奖励/审计筛选条件已写入浏览器本地。", [
        { key: "storage_key", value: "hello_admin_filter_preset" }
      ]);
    };

    $("loadFiltersBtn").onclick = () => {
      const raw = localStorage.getItem("hello_admin_filter_preset");
      if (!raw) {
        setDetailCard("没有筛选预设", "本地还没有保存过筛选条件。", [
          { key: "storage_key", value: "hello_admin_filter_preset" }
        ]);
        return;
      }
      try {
        applyFilterPreset(JSON.parse(raw));
        setDetailCard("筛选预设已恢复", "已把浏览器本地保存的筛选条件重新填回表单。", [
          { key: "storage_key", value: "hello_admin_filter_preset" }
        ]);
      } catch (_) {
        setDetailCard("筛选预设损坏", "本地筛选预设不是合法 JSON，建议清空后重新保存。", [
          { key: "storage_key", value: "hello_admin_filter_preset" }
        ]);
      }
    };

    $("clearFiltersBtn").onclick = () => {
      localStorage.removeItem("hello_admin_filter_preset");
      setDetailCard("筛选预设已清空", "浏览器本地保存的筛选条件已删除。", [
        { key: "storage_key", value: "hello_admin_filter_preset" }
      ]);
    };
)HTML";

    static constexpr const char* kPart9 = R"HTML(
    $("grantQueryBtn").onclick = () => run("查询奖励流水", async () => {
      const payload = {
        player_id: $("grantPlayerId").value.trim(),
        limit: Number($("grantLimit").value || 20),
        offset: Number($("grantOffset").value || 0)
      };
      addIfPresent(payload, "source_type", $("grantSourceType").value.trim());
      addIfPresent(payload, "source_id", $("grantSourceId").value.trim());
      return api("/admin/query/reward-grants", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify(payload)
      });
    });

    $("itemSaveBtn").onclick = () => run("保存物品配置", async () => {
      const result = await api("/admin/items/upsert", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          item_id: $("itemId").value.trim(),
          name: $("itemName").value.trim(),
          description: $("itemDescription").value,
          icon: $("itemIcon").value.trim(),
          rarity: $("itemRarity").value || "3星",
          max_stack: Number($("itemMaxStack").value || 1),
          item_type: $("itemType").value.trim() || "material",
          enabled: $("itemEnabled").value === "true"
        })
      });
      const saved = normalizeEnvelope(result);
      const items = [saved, ...state.itemConfigs.filter((item) => item.item_id !== saved.item_id)];
      renderItemConfigTable(items);
      return result;
    });

    $("itemQueryBtn").onclick = () => run("查询物品配置", async () => {
      const payload = {
        item_id: $("itemId").value.trim(),
        name: $("itemName").value.trim(),
        rarity: $("itemQueryRarity").value,
        limit: 50,
        offset: 0
      };
      const enabled = parseBoolOrNull($("itemQueryEnabled").value);
      if (enabled !== null) payload.enabled = enabled;
      const result = await api("/admin/items/query", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify(payload)
      });
      const data = normalizeEnvelope(result);
      renderItemConfigTable(data.items || []);
      return result;
    });

    async function queryGachaConfigs() {
      if (!state.gachaItemOptions.length) {
        try {
          await loadGachaRewardItems();
        } catch (_) {
          // 物品下拉加载失败不影响卡池配置查询，保留手动输入兜底。
        }
      }
      const result = await api("/admin/gacha/query", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          pool_id: $("gachaPoolId").value.trim(),
          include_disabled: true
        })
      });
      const data = normalizeEnvelope(result);
      renderGachaConfigTable(data.items || []);
      return result;
    }

    $("gachaPoolSaveBtn").onclick = () => run("保存抽卡卡池", async () => {
      const result = await api("/admin/gacha/pools/upsert", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          pool_id: $("gachaPoolId").value.trim(),
          pool_type: $("gachaPoolType").value,
          name: $("gachaPoolName").value.trim(),
          description: $("gachaPoolDescription").value,
          banner_icon: $("gachaBannerIcon").value.trim(),
          cost_currency_id: $("gachaCostCurrency").value.trim() || "gem",
          cost_amount_per_draw: Number($("gachaCostAmount").value || 0),
          pity_5: Number($("gachaPity5").value || 90),
          pity_4: Number($("gachaPity4").value || 10),
          enabled: $("gachaEnabled").value === "true"
        })
      });
      await queryGachaConfigs();
      return result;
    });

    async function saveGachaItemConfig() {
      if ($("gachaRewardKind").value === "item" && $("gachaRewardItemSelect").value) {
        applySelectedGachaRewardItem(false);
      }
      const result = await api("/admin/gacha/items/upsert", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          pool_id: $("gachaPoolId").value.trim(),
          item_key: $("gachaItemKey").value.trim(),
          rarity: Number($("gachaRarity").value || 3),
          weight: Number($("gachaWeight").value || 1),
          is_up: $("gachaIsUp").value === "true",
          reward_kind: $("gachaRewardKind").value,
          reward_resource_id: $("gachaRewardResource").value.trim(),
          reward_amount: Number($("gachaRewardAmount").value || 1)
        })
      });
      await queryGachaConfigs();
      return result;
    }

    $("gachaItemSaveBtn").onclick = () => run("保存抽卡奖品", saveGachaItemConfig);

    $("gachaQueryBtn").onclick = () => run("查询抽卡配置", queryGachaConfigs);
    $("gachaLoadItemsBtn").onclick = () => run("加载抽卡物品下拉", loadGachaRewardItems);
    $("gachaRewardItemSelect").onchange = () => applySelectedGachaRewardItem(false);
    $("gachaRewardItemKeyword").oninput = renderGachaRewardItemTable;
    $("gachaSyncItemKeyBtn").onclick = () => {
      applySelectedGachaRewardItem(true);
      setDetailCard("已同步奖品ID", "已把当前选择的物品 ID 写入奖励资源 ID 和奖品 item_key。", [
        { key: "item_id", value: $("gachaRewardItemSelect").value || "(未选择)" }
      ]);
    };

)HTML";

    static constexpr const char* kPart9_shop = R"HTML(
    $("shopRewardItemSelect").onchange = () => {
      const itemId = $("shopRewardItemSelect").value;
      if (!itemId) return;
      $("shopRewardKind").value = "item";
      $("shopRewardResource").value = itemId;
    };

    $("shopAddRewardBtn").onclick = () => run("添加商城奖励行", async () => {
      addRewardFromInputs("shop");
      return { rewards: state.rewardRows.shop.map(normalizeRewardRow) };
    });

    $("shopClearRewardsBtn").onclick = () => {
      state.rewardRows.shop = [];
      renderRewardRows("shop");
      setDetailCard("商城奖励已清空", "已清空当前商品表单里的奖励行。", [
        { key: "reward_count", value: 0 }
      ]);
    };

    $("shopLoadItemsBtn").onclick = () => run("加载商城物品下拉", loadGachaRewardItems);

    $("shopSaveBtn").onclick = () => run("保存商城商品", async () => {
      const priceCurrencyId = shopPriceCurrencyId();
      if (!priceCurrencyId) {
        throw { status: 400, data: { message: "价格货币不能为空" } };
      }
      const rewards = parseShopRewardsOrThrow();
      if (!rewards.length) {
        throw { status: 400, data: { message: "商品至少需要一个奖励" } };
      }
      const result = await api("/admin/shop/products/upsert", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          product_id: $("shopProductId").value.trim(),
          category: $("shopCategory").value.trim() || "default",
          name: $("shopName").value.trim(),
          description: $("shopDescription").value,
          icon: $("shopIcon").value.trim(),
          price_currency_id: priceCurrencyId,
          price_amount: Number($("shopPriceAmount").value || 0),
          rewards,
          purchase_limit: Number($("shopPurchaseLimit").value || 0),
          enabled: $("shopEnabled").value === "true"
        })
      });
      await queryShopProducts();
      return result;
    });

    $("shopQueryBtn").onclick = () => run("查询商城商品", queryShopProducts);

    $("shopDeleteBtn").onclick = () => run("删除当前商城商品", async () => {
      const productId = $("shopProductId").value.trim();
      if (!productId) {
        throw { status: 400, data: { message: "请先填写 product_id" } };
      }
      const result = await api("/admin/shop/products/delete", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({ product_id: productId })
      });
      await queryShopProducts();
      return result;
    });

)HTML";

    static constexpr const char* kPart9b = R"HTML(
    $("chatQueryBtn").onclick = () => run("查询聊天记录", async () => {
      return api("/admin/query/chat-history", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          ...buildChannel("chatQuery"),
          limit: Number($("chatQueryLimit").value || 50)
        })
      });
    });

    $("blacklistQueryBtn").onclick = () => run("查询封禁记录", async () => {
      return api("/admin/query/blacklists", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          limit: Number($("blacklistLimit").value || 50)
        })
      });
    });

    $("auditQueryBtn").onclick = () => run("查询审计日志", async () => {
      const payload = {
        limit: Number($("auditLimit").value || 20),
        offset: Number($("auditOffset").value || 0)
      };
      addIfPresent(payload, "action", $("auditAction").value.trim());
      addIfPresent(payload, "operator_id", $("auditOperatorId").value.trim());
      const success = parseBoolOrNull($("auditSuccess").value);
      if (success !== null) payload.success = success;
      return api("/admin/audit-logs/query", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify(payload)
      });
    });

    $("adminUsersQueryBtn").onclick = () => run("查询管理员列表", async () => {
      const payload = {
        limit: Number($("adminUserLimit").value || 20),
        offset: Number($("adminUserOffset").value || 0)
      };
      addIfPresent(payload, "operator_name", $("adminUserName").value.trim());
      const active = parseBoolOrNull($("adminUserActive").value);
      if (active !== null) payload.active = active;
      return api("/admin/users/query", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify(payload)
      });
    });

    $("tasksQueryBtn").onclick = () => run("查询任务列表", async () => {
      const payload = {
        limit: Number($("taskLimit").value || 20),
        offset: Number($("taskOffset").value || 0)
      };
      addIfPresent(payload, "kind", $("taskKind").value.trim());
      addIfPresent(payload, "status", $("taskStatus").value.trim());
      return api("/admin/tasks/query", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify(payload)
      });
    });

    $("traceQueryBtn").onclick = () => run("查询 Tracing", async () => {
      const limit = Number($("traceQueryLimit").value || 100);
      const traceId = $("traceQueryId").value.trim();
      const result = await api("/debug/traces", {
        method: "GET",
        headers: authHeaders()
      });
      const payload = normalizeEnvelope(result);
      const spans = extractItems(payload);
      state.traceSpans = traceId ? spans.filter((item) => (item.trace_id || "") === traceId) : spans.slice(0, limit);
      $("traceViewerId").value = traceId;
      return { items: state.traceSpans };
    });

    function renderLoadDetail(payload) {
      const data = normalizeEnvelope(payload)?.data || payload?.data || payload || {};
      const inflight = Number(data.inflight_requests || 0);
      const inflightMax = Number(data.max_inflight_requests || 0);
      const pending = Number(data.pending_business_tasks || 0);
      const pendingMax = Number(data.max_pending_business_tasks || 0);
      const rejected429 = Number(data.rejected_too_many_inflight || 0);
      const rejected503 = Number(data.rejected_business_queue_full || 0);
      setDetailCard("负载调试卡片", "直接展示当前背压状态和两类拒绝原因累计值。", [
        { key: "inflight_requests", value: inflight },
        { key: "max_inflight_requests", value: inflightMax },
        { key: "pending_business_tasks", value: pending },
        { key: "max_pending_business_tasks", value: pendingMax },
        { key: "rejected_too_many_inflight(429)", value: rejected429 },
        { key: "rejected_business_queue_full(503)", value: rejected503 },
        { key: "inflight_utilization", value: inflightMax > 0 ? ((inflight / inflightMax) * 100).toFixed(1) + "%" : "0%" },
        { key: "pending_utilization", value: pendingMax > 0 ? ((pending / pendingMax) * 100).toFixed(1) + "%" : "0%" }
      ]);
    }

    const runLoadQuery = () => run("查询负载状态", async () => {
      const result = await api("/debug/load", {
        method: "GET",
        headers: authHeaders()
      });
      renderLoadDetail(result);
      return result;
    });

    function renderAnnouncementCacheDetail(payload) {
      const data = normalizeEnvelope(payload)?.data || payload?.data || payload || {};
      const items = Array.isArray(data.items) ? data.items : [];
      setDetailCard("公告缓存调试卡片", "展示公告 Redis 最近窗口 key、预热状态和当前缓存条数。", [
        { key: "storage_mode", value: data.storage_mode || "" },
        { key: "cache_enabled", value: data.cache_enabled },
        { key: "channel_kind", value: data.channel_kind || "" },
        { key: "scope_id", value: data.scope_id || "" },
        { key: "recent_key", value: data.recent_key || "" },
        { key: "warm_key", value: data.warm_key || "" },
        { key: "warm", value: data.warm },
        { key: "item_count", value: Number(data.item_count || items.length || 0) }
      ], items.length ? JSON.stringify(items[0], null, 2) : "当前缓存窗口为空");
    }

    const runAnnouncementCacheQuery = () => run("查询公告缓存", async () => {
      const kind = $("announcementCacheKind").value;
      const scopeId = $("announcementCacheScope").value.trim() || "global";
      const limit = Number($("announcementCacheLimit").value || 5);
      const query = "/debug/announcement-cache?channel_kind=" + encodeURIComponent(kind)
        + "&scope_id=" + encodeURIComponent(scopeId)
        + "&limit=" + encodeURIComponent(String(limit));
      const result = await api(query, {
        method: "GET",
        headers: authHeaders()
      });
      renderAnnouncementCacheDetail(result);
      return result;
    });

    $("loadQueryBtn").onclick = runLoadQuery;
    $("loadCardQueryBtn").onclick = runLoadQuery;
    $("announcementCacheQueryBtn").onclick = runAnnouncementCacheQuery;

    $("tasksFailedBtn").onclick = () => {
      $("taskStatus").value = "failed";
      $("tasksQueryBtn").click();
    };

    $("tasksClearBtn").onclick = () => {
      $("taskKind").value = "";
      $("taskStatus").value = "";
      $("taskOffset").value = "0";
      setDetailCard("任务筛选已清空", "已把任务 kind / status / offset 清空，下一次查询会按默认条件执行。", [
        { key: "taskKind", value: "" },
        { key: "taskStatus", value: "" },
        { key: "taskOffset", value: "0" }
      ]);
    };

    $("traceViewerApplyBtn").onclick = () => {
      $("traceQueryId").value = $("traceViewerId").value.trim();
      setDetailCard("Trace 条件已带入", "已把 tracing 查看卡中的 trace_id 同步到查询表单。", [
        { key: "trace_id", value: $("traceQueryId").value || "(空)" }
      ]);
    };

    $("traceViewerOpenBtn").onclick = () => {
      const traceId = $("traceViewerId").value.trim();
      if (!traceId) {
        setDetailCard("Tracing 卡片", "当前还没有填写 trace_id。", [
          { key: "trace_id", value: "(空)" }
        ]);
        return;
      }
      showTraceDetail(traceId);
    };

    $("batchRewardBtn").onclick = () => run("批量发奖", async () => {
      const playerIds = uniqueSelectedPlayerIds();
      if (!playerIds.length) {
        throw { status: 400, data: { message: "no selected players" } };
      }
      return api("/admin/batch/rewards", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          player_ids: playerIds,
          source_type: $("batchRewardSourceType").value.trim() || "admin_batch",
          source_id: $("batchRewardSourceId").value.trim(),
          rewards: parseBatchRewardsOrThrow()
        })
      });
    });

    $("batchMuteBtn").onclick = () => run("提交批量禁言任务", async () => {
      const playerIds = uniqueSelectedPlayerIds();
      if (!playerIds.length) {
        throw { status: 400, data: { message: "no selected players" } };
      }
      return api("/admin/tasks/batch-mute", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          ...buildChannel("mute"),
          player_ids: playerIds,
          duration_seconds: Number($("batchMuteDuration").value || 3600),
          reason: $("batchMuteReason").value.trim()
        })
      });
    });

    $("batchAccountStatusBtn").onclick = () => run("提交批量账号状态任务", async () => {
      const playerIds = uniqueSelectedPlayerIds();
      if (!playerIds.length) {
        throw { status: 400, data: { message: "no selected players" } };
      }
      return api("/admin/tasks/account-status", {
        method: "POST",
        headers: authHeaders(),
        body: JSON.stringify({
          player_ids: playerIds,
          status: $("batchAccountStatus").value,
          reason: $("batchAccountReason").value.trim()
        })
      });
    });

    syncSelectionView();
    initializeRewardEditor("mail");
    initializeRewardEditor("sheet");
    initializeRewardEditor("shop");
    renderRewardItemOptions();
    refreshHeader();
    if (state.token && $("sheetPlayerBody")) {
      setTimeout(() => run("自动加载全部玩家", sheetQueryPlayers), 0);
    }
  </script>
</body>
</html>)HTML";

    std::string html;
    html.reserve(
        std::char_traits<char>::length(kPart1) +
        std::char_traits<char>::length(kPart1_sheet_css) +
        std::char_traits<char>::length(kPart1_workbook) +
        std::char_traits<char>::length(kPart1_workbook_shop) +
        std::char_traits<char>::length(kPart1_workbook_after_shop) +
        std::char_traits<char>::length(kPart1_auth) +
        std::char_traits<char>::length(kPart1_query) +
        std::char_traits<char>::length(kPart1a) +
        std::char_traits<char>::length(kPart1b_gm) +
        std::char_traits<char>::length(kPart1b) +
        std::char_traits<char>::length(kPart2) +
        std::char_traits<char>::length(kPart2b) +
        std::char_traits<char>::length(kPart3) +
        std::char_traits<char>::length(kPart3_state) +
        std::char_traits<char>::length(kPart3_state_items) +
        std::char_traits<char>::length(kPart3_state_gacha) +
        std::char_traits<char>::length(kPart3a) +
        std::char_traits<char>::length(kPart3b) +
        std::char_traits<char>::length(kPart3c) +
        std::char_traits<char>::length(kPart3c2) +
        std::char_traits<char>::length(kPart3c3) +
        std::char_traits<char>::length(kPart3d) +
        std::char_traits<char>::length(kPart4) +
        std::char_traits<char>::length(kPart5) +
        std::char_traits<char>::length(kPart6) +
        std::char_traits<char>::length(kPart6a) +
        std::char_traits<char>::length(kPart6a_bind) +
        std::char_traits<char>::length(kPart7) +
        std::char_traits<char>::length(kPart8) +
        std::char_traits<char>::length(kPart8b) +
        std::char_traits<char>::length(kPart9) +
        std::char_traits<char>::length(kPart9_shop) +
        std::char_traits<char>::length(kPart9b));
    html += kPart1;
    html += kPart1_sheet_css;
    html += kPart1_workbook;
    html += kPart1_workbook_shop;
    html += kPart1_workbook_after_shop;
    html += kPart1_auth;
    html += kPart1_query;
    html += kPart1a;
    html += kPart1b_gm;
    html += kPart1b;
    html += kPart2;
    html += kPart2b;
    html += kPart3;
    html += kPart3_state;
    html += kPart3_state_items;
    html += kPart3_state_gacha;
    html += kPart3a;
    html += kPart3b;
    html += kPart3c;
    html += kPart3c2;
    html += kPart3c3;
    html += kPart3d;
    html += kPart4;
    html += kPart5;
    html += kPart6;
    html += kPart6a;
    html += kPart6a_bind;
    html += kPart7;
    html += kPart8;
    html += kPart8b;
    html += kPart9;
    html += kPart9_shop;
    html += kPart9b;
    return html;
}
