from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_mobile_media_cards_have_explicit_responsive_layout():
    css = (ROOT / "dashboard" / "style.css").read_text(encoding="utf-8")

    assert "@media (max-width: 1024px)" in css
    assert "@media (max-width: 768px)" in css
    assert ".content-inner {" in css
    assert "#section-gis .middle-card-body" in css
    assert "#section-camera .middle-card-body" in css
    assert "height: 52vh;" in css
    assert "max-height: 420px;" in css


def test_mobile_gis_header_can_wrap_without_pushing_canvas_wide():
    css = (ROOT / "dashboard" / "style.css").read_text(encoding="utf-8")

    assert ".gis-header-tools {" in css
    assert ".gis-legend {" in css
    assert "flex-wrap: wrap;" in css
    assert "white-space: normal;" in css
    assert "margin-left: 0;" in css


def test_gis_and_3d_surfaces_resize_after_mobile_layout_changes():
    js = (ROOT / "dashboard" / "app.js").read_text(encoding="utf-8")

    assert "function resizeWorkSurfaces()" in js
    assert "requestAnimationFrame(function ()" in js
    assert "renderer.setSize(threedContainer.clientWidth, threedContainer.clientHeight)" in js


def test_tablet_landscape_uses_touch_capability_layout_without_desktop_narrowing():
    css = (ROOT / "dashboard" / "style.css").read_text(encoding="utf-8")

    assert "@media (orientation: landscape) and (pointer: coarse) and (hover: none)" in css
    assert ".main-content .content-inner {" in css
    assert "max-width: 100%;" in css
    assert "grid-template-columns: repeat(2, minmax(0, 1fr));" in css
    assert ".sidebar-overlay.active {" in css
    assert "transform: translateX(-100%);" in css
    assert "transform: translateX(0);" in css
    assert "#section-camera .middle-card-body {" in css
    assert "#section-gis .middle-card-body {" in css
    assert "height: 56vh;" in css
    assert "max-height: 520px;" in css


def test_tablet_landscape_sidebar_logic_uses_media_query_not_pixel_width_only():
    js = (ROOT / "dashboard" / "app.js").read_text(encoding="utf-8")

    assert "function isTouchLandscape()" in js
    assert "orientation: landscape" in js
    assert "pointer: coarse" in js
    assert "hover: none" in js
    assert "function usesOverlaySidebar()" in js
    assert "isTouchLandscape()" in js


def test_tablet_landscape_camera_container_matches_fixed_camera_ratio():
    css = (ROOT / "dashboard" / "style.css").read_text(encoding="utf-8")
    media_query = "@media (orientation: landscape) and (pointer: coarse) and (hover: none)"
    tablet_css = css.split(media_query, 1)[1].split("/* ============================================================", 1)[0]

    assert media_query in css
    assert "#section-camera .middle-card-body {" in tablet_css
    assert "aspect-ratio: 4 / 3;" in tablet_css
    assert "height: auto;" in tablet_css
    assert "aspect-ratio: auto;" not in tablet_css


def test_mobile_toolbar_exposes_sidebar_navigation_entry():
    html = (ROOT / "dashboard" / "index.html").read_text(encoding="utf-8")
    css = (ROOT / "dashboard" / "style.css").read_text(encoding="utf-8")
    js = (ROOT / "dashboard" / "app.js").read_text(encoding="utf-8")

    assert 'id="mobile-menu-button"' in html
    assert 'onclick="toggleSidebar()"' in html
    assert html.index('id="mobile-menu-button"') > html.index('<main id="main-content"')
    assert 'id="mobile-view-title"' in html
    assert ".mobile-toolbar {" in css
    assert ".mobile-menu-button {" in css
    assert "mobileViewTitle" in js
