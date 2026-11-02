"""2026-10-05 垂向推进（下潜）Web 侧改造测试。

对接固件契约（EG400_下潜Web侧交接_20261005.md）：
- CTRL: 15 → 17 个浮点（末尾 dive_left / dive_right，±100，0=不转）
- GET /api/control/dive      → CMD:DIVE（未启动拒绝；手动模式允许）
- GET /api/control/dive_stop → CMD:DIVE:STOP（永远放行，与固件同规矩）
- 下潜速度随 set_motion_params 一起下发并落盘；不带时保持现值（旧客户端兼容）
- dive_state 由固件 ACK 文本更新（comm 侧），ERR: DIVE 不污染水平电机状态机
- 启停类动作（start/motor_start/stop/motor_stop/manual_*）复位下潜状态
- 前端：下潜/停按钮、±40 速度滑块、状态徽标、上浮提示；无"上浮"按钮
"""
import json
from pathlib import Path
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))

import config
import main


BASE_PARAMS = {
    "kp": 20.0,
    "ki": 0.0,
    "kd": 1.0,
    "heading_kp": 0.4,
    "yaw_rate_damping": 0.15,
    "integral_limit": 0.5,
    "turn_limit": 15.0,
    "base_speed": 20.0,
    "min_output": 5.0,
    "max_output": 35.0,
    "left_trim": 1.0,
    "right_trim": 1.0,
    "front_yaw_share": 80.0,
    "rear_yaw_share": 20.0,
    "front_max_output": 15.0,
}


@pytest.fixture()
def client(monkeypatch, tmp_path):
    main.app.config["TESTING"] = True
    # 强制非模拟、非回环：走真实下发分支（send_str_to_linux 打桩捕获）
    monkeypatch.setattr(config, "WEB_ONLY", False)
    monkeypatch.setattr(config, "SIMULATION_MODE", False)
    monkeypatch.setattr(config, "LOOPBACK_MODE", False)
    monkeypatch.setattr(config, "HARDWARE_CONTROL_ENABLED", True)
    monkeypatch.setattr(config, "MANUAL_MODE", False)
    monkeypatch.setattr(config, "DIVE_LEFT_PCT", 0.0)
    monkeypatch.setattr(config, "DIVE_RIGHT_PCT", 0.0)
    monkeypatch.setattr(config, "ROLL_KP", 0.0)
    monkeypatch.setattr(config, "ROLL_KI", 0.0)
    monkeypatch.setattr(config, "ROLL_KD", 0.0)
    monkeypatch.setattr(config, "ROLL_LIMIT", 20.0)
    monkeypatch.setattr(config, "ROLL_SIGN", 1.0)
    config.system_status["is_running"] = False
    config.system_status["manual_mode"] = False
    config.system_status["dive_state"] = "stopped"
    monkeypatch.setattr(
        main, "MOTION_PARAMS_FILE", str(tmp_path / "motion_control_params.json"))
    sent = []
    monkeypatch.setattr(main, "send_str_to_linux", sent.append)
    # 桩掉 motor_control 的真实副作用（建巡检目录等）
    monkeypatch.setattr(main, "start_motor", lambda: sent.append("CMD:MOTOR_START"))
    monkeypatch.setattr(main, "stop_motor", lambda: sent.append("CMD:MOTOR_STOP"))
    with main.app.test_client() as c:
        c.sent = sent
        yield c
    # 恢复全局状态，避免污染其他测试文件（system_status 是模块级共享 dict）
    config.system_status["is_running"] = False
    config.system_status["manual_mode"] = False
    config.system_status["dive_state"] = "stopped"
    config.MANUAL_MODE = False


def _start_and_drive(client):
    client.get("/api/control/start")
    client.sent.clear()
    client.get("/api/control/motor_start")
    client.sent.clear()


# ---------- D6：未启动就点「下潜」→ Web 先拦，不发帧 ----------

def test_dive_rejected_when_not_running(client):
    resp = client.get("/api/control/dive")
    assert resp.status_code == 200
    body = resp.get_json()
    assert body["status"] == "error"
    assert "not running" in body["message"]
    assert client.sent == []


def test_dive_stop_always_allowed_even_not_running(client):
    """CMD:DIVE:STOP 永远放行（与 CMD:STOP/MOTOR_STOP 同规矩），Web 不拦。"""
    resp = client.get("/api/control/dive_stop")
    body = resp.get_json()
    assert body["status"] == "pending"
    assert client.sent == ["CMD:DIVE:STOP"]


# ---------- D1：运行中下发 CMD:DIVE；手动模式【允许】（与 yaw 不同） ----------

def test_dive_sends_cmd_when_running(client):
    _start_and_drive(client)
    resp = client.get("/api/control/dive")
    body = resp.get_json()
    assert body["status"] == "pending"
    assert client.sent == ["CMD:DIVE"]


def test_dive_allowed_in_manual_mode(client, monkeypatch):
    _start_and_drive(client)
    client.get("/api/control/manual_start")
    client.sent.clear()
    monkeypatch.setattr(config, "MANUAL_MODE", True)
    resp = client.get("/api/control/dive")
    body = resp.get_json()
    assert body["status"] == "pending"
    assert client.sent == ["CMD:DIVE"]


def test_dive_commands_are_exact_strings_without_payload(client):
    """命令整串精确匹配、不带数据：只有 CMD:DIVE / CMD:DIVE:STOP。"""
    _start_and_drive(client)
    client.get("/api/control/dive")
    client.get("/api/control/dive_stop")
    assert client.sent == ["CMD:DIVE", "CMD:DIVE:STOP"]


# ---------- CTRL: 17 字段契约（§1.1 / §4.2：应用参数必须带上第 16/17 字段） ----------

def test_ctrl_frame_always_carries_two_dive_fields(client):
    """不带下潜字段的旧客户端载荷：CTRL 仍输出 22 字段（速度/横滚保持现值）。"""
    _start_and_drive(client)
    client.sent.clear()
    resp = client.get("/api/control/motion_params")
    payload = BASE_PARAMS | {
        "dive_left": resp.get_json()["dive_left"],
        "dive_right": resp.get_json()["dive_right"],
    }
    resp = client.post("/api/control/set_motion_params", json=payload)
    assert resp.get_json()["status"] == "pending"
    assert len(client.sent) == 1
    ctrl = client.sent[0]
    assert ctrl.count(",") == 21          # 21 个逗号 = 22 个字段
    assert ctrl.endswith(",0.000,0.000,0.000,0.000,0.000,20.000,1.000")  # 横滚默认


def test_set_motion_params_updates_and_sends_dive_speeds(client):
    _start_and_drive(client)
    client.sent.clear()
    payload = BASE_PARAMS | {"dive_left": 30.0, "dive_right": -45.0}
    resp = client.post("/api/control/set_motion_params", json=payload)
    assert resp.get_json()["status"] == "pending"
    assert client.sent == [
        "CTRL:20.000,0.000,1.000,0.400,0.150,0.500,15.000,"
        "20.000,5.000,35.000,1.000,1.000,0.800,0.200,15.000,30.000,-45.000,"
        "0.000,0.000,0.000,20.000,1.000"
    ]
    assert config.DIVE_LEFT_PCT == pytest.approx(30.0)
    assert config.DIVE_RIGHT_PCT == pytest.approx(-45.0)
    # 落盘持久化（下次开机仍用）
    stored = json.loads(Path(main.MOTION_PARAMS_FILE).read_text(encoding="utf-8"))
    assert stored["dive_left"] == 30.0
    assert stored["dive_right"] == -45.0


def test_saved_dive_speeds_are_restored_into_new_ctrl(client, monkeypatch):
    params_file = Path(main.MOTION_PARAMS_FILE)
    params_file.write_text(
        json.dumps(BASE_PARAMS | {"dive_left": 60.0, "dive_right": 60.0}),
        encoding="utf-8",
    )
    # start 前置下发保存的参数：CTRL 必须带上落盘的 60/60
    client.get("/api/control/start")
    ctrl = next(c for c in client.sent if c.startswith("CTRL:"))
    assert ctrl.endswith(",60.000,60.000,0.000,0.000,0.000,20.000,1.000")
    # motor_start 同样前置下发
    client.sent.clear()
    client.get("/api/control/motor_start")
    ctrl = next(c for c in client.sent if c.startswith("CTRL:"))
    assert ctrl.endswith(",60.000,60.000,0.000,0.000,0.000,20.000,1.000")


def test_partial_dive_params_rejected_atomically(client):
    """只带一个下潜字段 → 整条拒绝（固件 17 字段一起原子生效）。"""
    _start_and_drive(client)
    client.sent.clear()
    resp = client.post(
        "/api/control/set_motion_params",
        json=BASE_PARAMS | {"dive_left": 30.0},
    )
    assert resp.status_code == 400
    assert resp.get_json()["status"] == "error"
    assert client.sent == []


def test_dive_speed_out_of_range_rejected(client):
    _start_and_drive(client)
    client.sent.clear()
    resp = client.post(
        "/api/control/set_motion_params",
        json=BASE_PARAMS | {"dive_left": 150.0, "dive_right": 0.0},
    )
    assert resp.status_code == 400
    assert client.sent == []
    assert config.DIVE_LEFT_PCT == pytest.approx(0.0)   # 原值不动


def test_old_params_file_without_dive_keys_still_loads(client):
    """旧 motion_control_params.json（无下潜字段）→ 正常加载，速度保持现值。"""
    params_file = Path(main.MOTION_PARAMS_FILE)
    params_file.write_text(json.dumps(BASE_PARAMS), encoding="utf-8")
    resp = client.get("/api/control/motion_params")
    body = resp.get_json()
    assert body["kp"] == 20.0
    assert body["dive_left"] == 0.0
    assert body["dive_right"] == 0.0


# ---------- D8：启停类动作复位下潜状态（六路联动停车） ----------

@pytest.mark.parametrize("stop_action", [
    "stop", "motor_stop", "manual_start", "manual_stop", "motor_start",
])
def test_stop_family_resets_dive_state(client, stop_action):
    _start_and_drive(client)
    config.system_status["dive_state"] = "on"   # 假设此前下潜开着
    resp = client.get(f"/api/control/{stop_action}")
    assert resp.status_code == 200
    assert config.system_status["dive_state"] == "stopped"


def test_api_data_exposes_dive_state(client):
    _start_and_drive(client)
    data = client.get("/api/data").get_json()
    assert data["dive_state"] == "stopped"


# ---------- §2.5：ACK 文本更新 dive_state；ERR: DIVE 不污染水平电机状态机 ----------

@pytest.fixture()
def preserve_status():
    """保存/恢复 system_status 中本组测试会改动的键，避免污染其他测试。"""
    keys = ("dive_state", "motor_command_state", "is_moving", "nav_fault",
            "motor_control_message")
    saved = {k: config.system_status[k] for k in keys}
    yield
    config.system_status.update(saved)


def test_dive_state_updated_from_firmware_acks(preserve_status):
    from core.comm import apply_control_feedback

    apply_control_feedback({"control_ack": "ACK: DIVE ON"})
    assert config.system_status["dive_state"] == "on"
    assert config.system_status["motor_command_state"] != "rejected"

    apply_control_feedback({"control_ack": "ACK: DIVE STOP"})
    assert config.system_status["dive_state"] == "stopped"


def test_dive_err_keeps_state_and_motor_state_machine(preserve_status):
    from core.comm import apply_control_feedback

    config.system_status["dive_state"] = "on"
    config.system_status["motor_command_state"] = "running"
    apply_control_feedback(
        {"control_ack": "ERR: DIVE STRENGTH ZERO (set CTRL: fields 16,17)"})
    # ERR: DIVE 不落入 ACK: REJECTED 分支：下潜保持原状态、水平电机不受影响
    assert config.system_status["dive_state"] == "on"
    assert config.system_status["motor_command_state"] == "running"

    apply_control_feedback({"control_ack": "ERR: DIVE NOT IN RUN MODE"})
    assert config.system_status["dive_state"] == "on"
    assert config.system_status["motor_command_state"] == "running"


def test_unknown_dive_variant_ack_does_not_flip_state(preserve_status):
    """拼错的命令（如 CMD:DIVE:UP）走未知指令回显，不得命中 DIVE ON/STOP。"""
    from core.comm import apply_control_feedback

    config.system_status["dive_state"] = "stopped"
    apply_control_feedback({"control_ack": "ACK: 收到未知指令 [CMD:DIVE:UP]"})
    assert config.system_status["dive_state"] == "stopped"
    apply_control_feedback({"control_ack": "ACK: 收到未知指令 [CMD:DIVE:STOP]"})
    assert config.system_status["dive_state"] == "stopped"


def test_six_motor_stop_ack_clears_dive_state(preserve_status):
    from core.comm import apply_control_feedback

    config.system_status["dive_state"] = "on"
    apply_control_feedback({"control_ack": "ACK: MOTOR STOPPED"})
    assert config.system_status["dive_state"] == "stopped"


# ---------- 前端结构（§2.4）：按钮 / 滑块 / 徽标 / 提示；无"上浮"按钮 ----------

HTML = (ROOT / "app" / "dashboard" / "index.html").read_text(encoding="utf-8")
JS = (ROOT / "app" / "dashboard" / "app.js").read_text(encoding="utf-8")


def test_frontend_has_dive_buttons_sliders_and_badge():
    assert 'id="btn-dive"' in HTML
    assert "sendCommand('dive')" in HTML
    assert 'id="btn-dive-stop"' in HTML
    assert "sendCommand('dive_stop')" in HTML
    assert 'id="dive-left"' in HTML and 'id="dive-right"' in HTML
    assert 'id="dive-state-badge"' in HTML
    # 速度滑块范围与后端/固件一致：±100
    for slider_id in ("dive-left", "dive-right"):
        tail = HTML.split(f'id="{slider_id}"', 1)[1].split(">", 1)[0]
        assert 'min="-100"' in tail
        assert 'max="100"' in tail


def test_frontend_has_no_ascend_button():
    """§2.4：不做"上浮"按钮（上浮 = 调小速度或按停），只有提示文案。"""
    assert 'id="btn-dive-up"' not in HTML
    assert 'id="btn-ascend"' not in HTML
    assert "上浮" in HTML   # 提示文案存在：上浮 = 把下潜速度调小或按「停」


def test_frontend_dive_speeds_ride_ctrl_path():
    """速度不为下潜新增任何固件命令：前端只走 sendCommand('dive'/'dive_stop')，
    不自行拼接 CMD: 帧或直连 dive URL。"""
    assert "/api/control/dive" not in JS     # 不直连 dive API，统一走 sendCommand
    # 速度走 set_motion_params 同一条路径（PID 页应用 + 下潜面板应用速度）
    assert "function applyDiveSpeed" in JS
    assert JS.count("/api/control/set_motion_params") >= 2
    # 徽标由 dive_state 驱动
    assert "setDiveBadge(data.dive_state)" in JS
    assert "function setDiveBadge" in JS
