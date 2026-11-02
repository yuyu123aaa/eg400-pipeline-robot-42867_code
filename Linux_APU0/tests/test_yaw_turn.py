"""2026-10-04 航向基准角（yaw_left/yaw_right）接口测试。

覆盖：
- 未启动系统时拒绝（system not running）
- 手动模式下拒绝（manual mode on）
- 自动巡航中左转/右转下发 CMD:YAW:LEFT / CMD:YAW:RIGHT
- ±90 累计与 wrap（+90×4 = 360 → wrap 0）
- 启动/停止重置基准角
- /api/data 暴露 heading_offset_deg
- UI 不出现"90°/九十度"字样（演示话术约束）
"""
import pytest
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))

import config
import main


@pytest.fixture()
def client(monkeypatch):
    main.app.config["TESTING"] = True
    # 强制非模拟、非回环：走真实下发分支（send_str_to_linux 打桩捕获）
    monkeypatch.setattr(config, "WEB_ONLY", False)
    monkeypatch.setattr(config, "SIMULATION_MODE", False)
    monkeypatch.setattr(config, "LOOPBACK_MODE", False)
    monkeypatch.setattr(config, "HARDWARE_CONTROL_ENABLED", True)
    # 重置全局状态，避免被其他测试文件残留的 is_running=True 污染
    config.system_status["is_running"] = False
    config.system_status["manual_mode"] = False
    config.MANUAL_MODE = False
    sent = []
    monkeypatch.setattr(main, "send_str_to_linux", sent.append)
    with main.app.test_client() as c:
        c.sent = sent
        yield c
    # teardown 恢复
    config.system_status["is_running"] = False
    config.system_status["manual_mode"] = False
    config.MANUAL_MODE = False


def _start_and_drive(client):
    client.get("/api/control/start")
    client.sent.clear()
    client.get("/api/control/motor_start")
    client.sent.clear()


def test_yaw_rejected_when_not_running(client):
    resp = client.get("/api/control/yaw_left")
    assert resp.status_code == 200
    assert resp.get_json()["status"] == "error"
    assert "not running" in resp.get_json()["message"]
    assert client.sent == []


def test_yaw_rejected_in_manual_mode(client, monkeypatch):
    _start_and_drive(client)
    monkeypatch.setattr(config, "MANUAL_MODE", True)
    client.get("/api/control/manual_start")
    client.sent.clear()
    resp = client.get("/api/control/yaw_right")
    assert resp.get_json()["status"] == "error"
    assert "manual" in resp.get_json()["message"]
    assert client.sent == []


def test_yaw_left_sends_cmd_and_accumulates(client):
    _start_and_drive(client)
    resp = client.get("/api/control/yaw_left")
    body = resp.get_json()
    assert body["status"] == "pending"
    assert body["heading_offset_deg"] == pytest.approx(90.0)
    assert client.sent == ["CMD:YAW:LEFT"]


def test_yaw_right_wraps_negative(client):
    _start_and_drive(client)
    client.get("/api/control/yaw_right")
    resp = client.get("/api/control/yaw_right")
    assert resp.get_json()["heading_offset_deg"] == pytest.approx(-180.0)
    assert client.sent == ["CMD:YAW:RIGHT", "CMD:YAW:RIGHT"]


def test_yaw_full_circle_wraps_to_zero(client):
    _start_and_drive(client)
    for _ in range(4):
        client.get("/api/control/yaw_left")
    data = client.get("/api/data").get_json()
    assert data["heading_offset_deg"] == pytest.approx(0.0)


def test_stop_resets_heading_offset(client):
    _start_and_drive(client)
    client.get("/api/control/yaw_left")
    client.get("/api/control/stop")
    data = client.get("/api/data").get_json()
    assert data["heading_offset_deg"] == 0.0


def test_ui_does_not_expose_angle_text():
    """演示话术约束：页面不得出现 90° 相关文案。"""
    html_path = ROOT / "app" / "dashboard" / "index.html"
    html = html_path.read_text(encoding="utf-8")
    assert "90°" not in html
    assert "九十度" not in html
    assert "btn-yaw-left" in html
    assert "btn-yaw-right" in html

