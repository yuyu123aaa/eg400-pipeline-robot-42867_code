from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read_dashboard_file(name):
    return (ROOT / "dashboard" / name).read_text(encoding="utf-8")


def test_dashboard_has_persistent_sonar_fault_banner():
    html = read_dashboard_file("index.html")

    assert 'id="sonar-fault-banner"' in html
    assert 'role="alert"' in html
    assert 'aria-live="assertive"' in html
    assert 'id="sonar-fault-message"' in html


def test_polling_syncs_alarm_and_marks_faulty_channels():
    js = read_dashboard_file("app.js")

    assert "const SONAR_DAMAGE_VALUE = 65535;" in js
    assert "function syncSonarFault(data)" in js
    assert "data.sonar_fault_channels" in js
    assert "syncSonarFault(data);" in js
    assert "faulty ? '异常'" in js


def test_65535_is_never_used_for_wall_plotting():
    js = read_dashboard_file("app.js")

    assert (
        "Number(data.sonar_left) !== SONAR_DAMAGE_VALUE"
        in js
    )
    assert (
        "Number(data.sonar_right) !== SONAR_DAMAGE_VALUE"
        in js
    )


def test_alarm_and_faulty_channels_have_danger_styling():
    css = read_dashboard_file("style.css")

    assert ".sonar-fault-banner" in css
    assert ".sonar-fault-banner[hidden]" in css
    assert ".sensor-data-item.sonar-fault" in css
