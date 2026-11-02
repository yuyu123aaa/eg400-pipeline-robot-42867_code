import time, json, urllib.request as u
from playwright.sync_api import sync_playwright

# 1) 先确认服务活着 + 走廊在生长
B = "http://127.0.0.1:5000"
try:
    d = json.load(u.urlopen(B + "/api/corridor/current"))
    print(f"service OK: sections={len(d['sections'])} mileage={d['total_mileage']:.2f}")
except Exception as e:
    print("service check failed:", e)

# 2) 用真实浏览器打开，截图
with sync_playwright() as pw:
    browser = pw.chromium.launch(headless=True)
    page = browser.new_page(viewport={"width": 1440, "height": 900})
    page.goto(B, wait_until="load", timeout=20000)
    time.sleep(3)
    print("page loaded:", page.title())

    # 切到 3D 走廊视图
    page.evaluate("switchView('corridor')")
    print("switched to corridor view")

    # 等 8 秒让走廊继续长 + three.js 渲染稳定
    time.sleep(8)

    # 截图全页
    out = r"E:\水下机器人\eg400_linux_app\corridor_v2_render.png"
    page.screenshot(path=out, full_page=False)
    print("screenshot saved:", out)

    # 再等几秒拍第二张（走廊更长）
    time.sleep(10)
    out2 = r"E:\水下机器人\eg400_linux_app\corridor_v2_render2.png"
    page.screenshot(path=out2, full_page=False)
    print("screenshot2 saved:", out2)

    # 看控制台有没有报错
    errs = page.evaluate("(() => { try { return JSON.stringify(window.__corridorErrors || 'none'); } catch(e) { return e.message; } })()")
    print("console errors:", errs)

    browser.close()
