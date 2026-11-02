from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read_dashboard_file(name):
    return (ROOT / "dashboard" / name).read_text(encoding="utf-8")


def test_overview_hides_pid_and_logs_but_keeps_dedicated_views():
    js = read_dashboard_file("app.js")
    overview_branch = js.split("if (viewMode === 'overview') {", 1)[1].split(
        "} else if (viewMode === 'camera') {", 1
    )[0]

    assert "pidSection.classList.add('focus-hidden')" in overview_branch
    assert "logsSection.classList.add('focus-hidden')" in overview_branch
    assert "} else if (viewMode === 'pid') {" in js
    assert "} else if (viewMode === 'logs') {" in js


def test_overview_sets_explicit_view_state_and_is_applied_on_startup():
    js = read_dashboard_file("app.js")

    assert "contentInner.classList.toggle('view-overview', viewMode === 'overview')" in js
    assert "window.switchView('overview');" in js


def test_overview_initial_visibility_is_safe_before_javascript_finishes():
    html = read_dashboard_file("index.html")

    assert 'class="content-inner view-overview"' in html
    assert 'class="motion-params-page focus-hidden" id="section-pid"' in html
    assert 'class="card focus-hidden"' in html
    assert 'id="section-logs"' in html


def test_desktop_overview_uses_remaining_viewport_height():
    css = read_dashboard_file("style.css")

    assert "@media (min-width: 1025px) and (hover: hover) and (pointer: fine)" in css
    assert ".content-inner.view-overview {" in css
    assert "grid-template-rows: auto auto minmax(320px, 1fr) auto;" in css
    assert "height: 100%;" in css
    assert "min-height: 760px;" in css
    assert ".content-inner.view-overview .middle-row {" in css
    assert ".content-inner.view-overview .middle-card {" in css
