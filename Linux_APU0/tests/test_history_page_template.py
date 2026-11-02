from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read_dashboard_file(name):
    return (ROOT / "dashboard" / name).read_text(encoding="utf-8")


def test_history_uses_sidebar_page_not_modal():
    html = read_dashboard_file("index.html")
    js = read_dashboard_file("app.js")
    css = read_dashboard_file("style.css")

    assert 'onclick="switchView(\'history\');"' in html
    assert 'id="section-history"' in html
    assert 'id="history-modal"' not in html
    assert "openHistoryModal" not in js
    assert "closeHistoryModal" not in js
    assert ".history-page" in css


def test_history_images_preview_in_page_without_new_tab():
    html = read_dashboard_file("index.html")
    js = read_dashboard_file("app.js")
    css = read_dashboard_file("style.css")

    assert 'id="image-preview"' in html
    assert "openImagePreview(currentHistoryImages" in js
    assert "window.open" not in js
    assert ".image-preview-body" in css
    assert ".image-preview-footer" in css
