# -*- coding: utf-8 -*-
"""本地复现：web-only 模式下加载页面、点启动、抓 Console 报错与按钮状态。"""
import json
import sys
import urllib.request

from playwright.sync_api import sync_playwright

BASE = "http://127.0.0.1:5000"


def fetch_json(path):
    with urllib.request.urlopen(BASE + path, timeout=5) as r:
        return json.loads(r.read().decode())


with sync_playwright() as p:
    browser = p.chromium.launch(headless=True)
    page = browser.new_page()
    console_msgs = []
    page.on("console", lambda m: console_msgs.append(f"[{m.type}] {m.text}"))
    page.on("pageerror", lambda e: console_msgs.append(f"[PAGEERROR] {e}"))

    page.goto(BASE, wait_until="domcontentloaded", timeout=30000)
    page.wait_for_timeout(2500)  # 等几轮数据轮询

    btn_start = page.locator("#btn-start")
    info_before = {
        "btn_start_disabled": btn_start.is_disabled(),
        "btn_start_text": btn_start.inner_text(),
        "dive_panel_exists": page.locator("#dive-control-panel").count(),
        "btn_dive_exists": page.locator("#btn-dive").count(),
        "badge": page.locator("#dive-state-badge").inner_text()
        if page.locator("#dive-state-badge").count() else "(无徽标元素)",
        "appjs_has_setDiveBadge": page.evaluate(
            "typeof setDiveBadge === 'function'"),
        "appjs_has_sendCommand": page.evaluate(
            "typeof sendCommand === 'function'"),
    }

    # 点启动
    try:
        btn_start.click(timeout=3000)
        click_result = "clicked"
    except Exception as exc:
        click_result = f"click failed: {exc}"
    page.wait_for_timeout(2000)

    info_after = {
        "btn_start_disabled": btn_start.is_disabled(),
        "sidebar_badge": page.locator("#status-badge").inner_text(),
        "is_running_api": fetch_json("/api/data")["is_running"],
    }

    page.screenshot(path="../复现_点击启动后.png", full_page=False)
    browser.close()

print("== 页面加载后 ==")
print(json.dumps(info_before, ensure_ascii=False, indent=2))
print("== 点击启动 ==", click_result)
print("== 点击后 ==")
print(json.dumps(info_after, ensure_ascii=False, indent=2))
print("== Console 消息 ==")
for m in console_msgs[:40]:
    print(m)
