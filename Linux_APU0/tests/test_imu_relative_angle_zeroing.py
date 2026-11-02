import json
from pathlib import Path
import re
import shutil
import subprocess


ROOT = Path(__file__).resolve().parents[1] / "app"
APP_JS = (ROOT / "dashboard" / "app.js").read_text(encoding="utf-8")


def test_wrap_angle_180_handles_boundary_crossings():
    match = re.search(
        r"function wrapAngle180\(angle\)\s*\{(?P<body>.*?)\n\s*\}",
        APP_JS,
        re.DOTALL,
    )
    assert match is not None, "dashboard must define wrapAngle180(angle)"

    node = shutil.which("node")
    assert node is not None
    script = f"""
function wrapAngle180(angle) {{
{match.group("body")}
}}
console.log(JSON.stringify([
    wrapAngle180(-179 - 179),
    wrapAngle180(179 - (-179)),
    wrapAngle180(0),
]));
"""
    result = subprocess.run(
        [node, "-e", script],
        capture_output=True,
        text=True,
    )
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == [2, -2, 0]


def test_relative_angles_are_wrapped_before_display_and_rendering():
    assert (
        "let relRoll = wrapAngle180(latestAngles.roll - baseAngles.roll);"
        in APP_JS
    )
    assert (
        "let relPitch = wrapAngle180(latestAngles.pitch - baseAngles.pitch);"
        in APP_JS
    )
    assert (
        "let relYaw = wrapAngle180(latestAngles.yaw - baseAngles.yaw);"
        in APP_JS
    )


def test_recalibrate_captures_the_latest_valid_angles_as_software_zero():
    assert (
        "let latestAngles = { roll: null, pitch: null, yaw: null };"
        in APP_JS
    )
    software_calibration = APP_JS.split(
        "function applySoftwareCalibration()", 1
    )[1].split("function ", 1)[0]
    recalibrate = APP_JS.split("function reCalibrate()", 1)[1]
    recalibrate = recalibrate.split("function ", 1)[0]
    assert "baseAngles = { ...latestAngles };" in software_calibration
    assert "fetch('/api/control/calibrate')" in recalibrate
    assert "syncHeadingCalibration(" in recalibrate
    assert "true" in recalibrate
    assert (
        "baseAngles = { roll: null, pitch: null, yaw: null };"
        not in recalibrate
    )
