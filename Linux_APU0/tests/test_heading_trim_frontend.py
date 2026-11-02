from pathlib import Path


ROOT = Path(__file__).resolve().parents[1] / "app"
HTML = (ROOT / "dashboard" / "index.html").read_text(encoding="utf-8")
JS = (ROOT / "dashboard" / "app.js").read_text(encoding="utf-8")
MAIN = (ROOT / "main.py").read_text(encoding="utf-8")

PARAMETER_IDS = (
    "pid-kp",
    "pid-ki",
    "pid-kd",
    "heading-kp",
    "yaw-rate-damping",
    "integral-limit",
    "turn-limit",
    "base-speed",
    "min-output",
    "max-output",
    "left-trim",
    "right-trim",
    "front-yaw-share",
    "rear-yaw-share",
    "front-max-output",
)


def test_motion_parameter_page_exposes_every_supported_parameter():
    for parameter_id in PARAMETER_IDS:
        assert f'id="{parameter_id}"' in HTML
        assert f"'{parameter_id}'" in JS

    assert "function updateMotionParams" in JS
    assert "function loadMotionParams" in JS
    assert "/api/control/set_motion_params" in JS
    assert "/api/control/motion_params" in JS
    assert "CTRL:" in MAIN


def test_obsolete_pid_controls_and_chart_are_removed():
    for obsolete in (
        'id="cpid-kp"',
        'id="cpid-ki"',
        'id="cpid-kd"',
        'id="custom-cmd-input"',
        'id="pid-chart"',
        "function updateCurvePID",
        "function sendCustomCmd",
        "cmdTemplates",
        "PID_CHART_ENABLED",
    ):
        assert obsolete not in HTML
        assert obsolete not in JS


def test_web_defaults_match_baremetal_defaults():
    expected_defaults = {
        "pid-kp": "20.0",
        "pid-ki": "0.0",
        "pid-kd": "1.0",
        "heading-kp": "0.40",
        "yaw-rate-damping": "0.15",
        "integral-limit": "0.5",
        "turn-limit": "15.0",
        "base-speed": "20.0",
        "min-output": "5.0",
        "max-output": "35.0",
        "left-trim": "1.00",
        "right-trim": "1.00",
        "front-yaw-share": "80",
        "rear-yaw-share": "20",
        "front-max-output": "15.0",
    }
    for parameter_id, value in expected_defaults.items():
        field_tail = HTML.split(f'id="{parameter_id}"', 1)[1].split(">", 1)[0]
        assert f'value="{value}"' in field_tail


def test_web_renders_authoritative_motor_feedback():
    assert 'id="motor-control-message"' in HTML
    assert "data.motor_command_state" in JS
    assert "data.motor_control_message" in JS
