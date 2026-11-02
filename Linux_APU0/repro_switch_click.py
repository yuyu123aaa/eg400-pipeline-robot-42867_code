# -*- coding: utf-8 -*-
"""复现：页面切换 + 多按钮点击 + Console 报错抓取。"""
from playwright.sync_api import sync_playwright

BASE = "http://127.0.0.1:5000"

with sync_playwright() as p:
    browser = p.chromium.launch(headless=True)
    page = browser.new_page()
    msgs = []
    page.on("console", lambda m: msgs.append(f"[{m.type}] {m.text}"))
    page.on("pageerror", lambda e: msgs.append(f"[PAGEERROR] {e}"))

    page.goto(BASE, wait_until="domcontentloaded", timeout=30000)
    page.wait_for_timeout(3000)

    def probe(label):
        try:
            v = page.evaluate("""() => ({
                readyState: document.readyState,
                isSystemRunning: (typeof isSystemRunning !== 'undefined') ? isSystemRunning : '(undef)',
                switchViewType: typeof switchView,
                sendCommandType: typeof sendCommand,
                setDiveBadgeType: typeof setDiveBadge,
                currentView: (typeof currentDashboardView !== 'undefined') ? currentDashboardView : '(undef)',
                visibleSections: Array.from(document.querySelectorAll('section[id^="section-"]'))
                    .filter(s => !s.classList.contains('hidden') && !s.classList.contains('focus-hidden'))
                    .map(s => s.id),
            })""")
        except Exception as exc:
            v = {"eval_error": str(exc)}
        print(f"[{label}] {v}")

    probe("加载后")

    # 切换到各视图
    for view in ["camera", "gis", "3d", "corridor", "controls", "pid", "logs", "report", "history", "overview"]:
        try:
            page.evaluate(f"switchView('{view}')")
            page.wait_for_timeout(250)
        except Exception as exc:
            print(f"[切{view}] 异常: {exc}")
    probe("切完10视图")

    # 点启动
    try:
        page.locator("#btn-start").click(timeout=3000)
        print("[点启动] clicked")
    except Exception as exc:
        print(f"[点启动] {exc}")
    page.wait_for_timeout(1500)
    probe("点启动后")

    # 点左灯
    try:
        page.locator("#btn-light1").click(timeout=2000)
        print("[点左灯] clicked")
    except Exception as exc:
        print(f"[点左灯] {exc}")

    page.screenshot(path="../复现_切视图点按钮.png", full_page=False)
    browser.close()

print("\n== Console / PageError ==")
for m in msgs[:30]:
    print(" ", m)
