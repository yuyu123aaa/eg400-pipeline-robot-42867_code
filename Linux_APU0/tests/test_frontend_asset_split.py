import re
from pathlib import Path


def test_dashboard_uses_external_css_and_js_assets():
    html = Path("dashboard/index.html").read_text(encoding="utf-8")

    assert Path("dashboard/style.css").is_file()
    assert Path("dashboard/app.js").is_file()
    assert '<link rel="stylesheet" href="/dashboard/style.css">' in html
    assert '<script src="/dashboard/app.js"></script>' in html
    assert "<style>" not in html
    assert re.search(r"<script(?![^>]*\bsrc=)[^>]*>", html) is None


def test_dashboard_runtime_scripts_are_available_offline():
    html = Path("dashboard/index.html").read_text(encoding="utf-8")
    script_sources = re.findall(r'<script[^>]+\bsrc="([^"]+)"', html)

    assert script_sources
    assert all(not source.startswith(("http://", "https://")) for source in script_sources)

    static_sources = [
        source
        for source in script_sources
        if source.startswith("/static/")
    ]
    assert "/static/GLTFLoader.js" in static_sources
    for source in static_sources:
        assert Path(source.lstrip("/")).is_file(), f"缺少离线脚本资源: {source}"


def test_dashboard_assets_keep_core_hooks():
    css = Path("dashboard/style.css").read_text(encoding="utf-8")
    js = Path("dashboard/app.js").read_text(encoding="utf-8")

    assert ".sensor-bar" in css
    assert "#section-camera .middle-card-body" in css
    assert ".attitude-group .sc-unit" in css
    assert "function fetchData()" in js
    assert "function startMeasuredVideoStream()" in js
    assert "function sendCommand(action)" in js


def test_sidebar_view_switch_resets_middle_row_visibility():
    js = Path("dashboard/app.js").read_text(encoding="utf-8")

    assert "middleRow.classList.add('focus-hidden')" in js
    assert "middleRow.classList.remove('single-col', 'focus-expand', 'focus-hidden')" in js


def test_dashboard_asset_routes_are_served():
    import main

    client = main.app.test_client()

    css_response = client.get("/dashboard/style.css")
    js_response = client.get("/dashboard/app.js")
    blocked_response = client.get("/dashboard/index.html")

    assert css_response.status_code == 200
    assert css_response.content_type.startswith("text/css")
    assert js_response.status_code == 200
    assert "javascript" in js_response.content_type
    assert blocked_response.status_code == 404
