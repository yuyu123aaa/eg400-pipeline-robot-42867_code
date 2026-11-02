from pathlib import Path
import json
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import main


VALID_PARAMS = {
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


def test_motion_params_api_sends_one_atomic_control_command(monkeypatch):
    monkeypatch.setattr(main.config, "WEB_ONLY", False, raising=False)
    monkeypatch.setattr(main.config, "SIMULATION_MODE", False, raising=False)
    monkeypatch.setattr(main.config, "HARDWARE_CONTROL_ENABLED", True, raising=False)
    # 2026-10-05: CTRL: 已扩为 17 字段（末尾两个下潜速度），锁定为 0 保证期望串确定
    monkeypatch.setattr(main.config, "DIVE_LEFT_PCT", 0.0, raising=False)
    monkeypatch.setattr(main.config, "DIVE_RIGHT_PCT", 0.0, raising=False)
    # 2026-10-05: 横滚参数也锁定为默认值保证期望串确定
    monkeypatch.setattr(main.config, "ROLL_KP", 0.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_KI", 0.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_KD", 0.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_LIMIT", 20.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_SIGN", 1.0, raising=False)
    sent = []
    monkeypatch.setattr(
        main,
        "MOTION_PARAMS_FILE",
        str(Path(main.app.root_path) / "unused-test-motion-params.json"),
        raising=False,
    )
    monkeypatch.setattr(main, "_save_motion_params", lambda values: None, raising=False)
    monkeypatch.setattr(main, "send_str_to_linux", sent.append)

    response = main.app.test_client().post(
        "/api/control/set_motion_params",
        json=VALID_PARAMS,
    )

    assert response.status_code == 200
    assert response.get_json()["status"] == "pending"
    assert len(sent) == 1
    assert sent[0] == (
        "CTRL:20.000,0.000,1.000,0.400,0.150,0.500,15.000,"
        "20.000,5.000,35.000,1.000,1.000,0.800,0.200,15.000,0.000,0.000,"
        "0.000,0.000,0.000,20.000,1.000"
    )


def test_motion_params_api_rejects_invalid_combinations(monkeypatch):
    sent = []
    monkeypatch.setattr(main, "send_str_to_linux", sent.append)
    client = main.app.test_client()

    bad_share = {**VALID_PARAMS, "rear_yaw_share": 19.0}
    bad_limits = {**VALID_PARAMS, "min_output": 25.0}
    bad_front_limit = {**VALID_PARAMS, "front_max_output": 101.0}

    for payload in (bad_share, bad_limits, bad_front_limit):
        response = client.post(
            "/api/control/set_motion_params",
            json=payload,
        )
        assert response.status_code == 400
        assert response.get_json()["status"] == "error"

    assert sent == []


def test_motion_params_api_requires_every_parameter(monkeypatch):
    sent = []
    monkeypatch.setattr(main, "send_str_to_linux", sent.append)
    incomplete = dict(VALID_PARAMS)
    incomplete.pop("kd")

    response = main.app.test_client().post(
        "/api/control/set_motion_params",
        json=incomplete,
    )

    assert response.status_code == 400
    assert sent == []


def test_motion_params_are_saved_and_loaded_from_disk(monkeypatch, tmp_path):
    params_file = tmp_path / "motion_control_params.json"
    monkeypatch.setattr(
        main,
        "MOTION_PARAMS_FILE",
        str(params_file),
        raising=False,
    )
    monkeypatch.setattr(main, "send_str_to_linux", lambda command: None)
    # 2026-10-05: 下潜速度 + 横滚参数随基础参数一起落盘 / 回读
    monkeypatch.setattr(main.config, "DIVE_LEFT_PCT", 0.0, raising=False)
    monkeypatch.setattr(main.config, "DIVE_RIGHT_PCT", 0.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_KP", 0.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_KI", 0.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_KD", 0.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_LIMIT", 20.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_SIGN", 1.0, raising=False)

    save_response = main.app.test_client().post(
        "/api/control/set_motion_params",
        json=VALID_PARAMS,
    )
    load_response = main.app.test_client().get(
        "/api/control/motion_params"
    )

    expected_with_extras = {
        **VALID_PARAMS,
        "dive_left": 0.0,
        "dive_right": 0.0,
        "roll_kp": 0.0,
        "roll_ki": 0.0,
        "roll_kd": 0.0,
        "roll_limit": 20.0,
        "roll_sign": 1.0,
    }
    assert save_response.status_code == 200
    assert params_file.is_file()
    assert json.loads(params_file.read_text(encoding="utf-8")) == expected_with_extras
    assert load_response.status_code == 200
    assert load_response.get_json() == expected_with_extras


def test_saved_motion_params_are_reapplied_before_system_and_motor_start(
    monkeypatch,
    tmp_path,
):
    monkeypatch.setattr(main.config, "WEB_ONLY", False, raising=False)
    monkeypatch.setattr(main.config, "SIMULATION_MODE", False, raising=False)
    monkeypatch.setattr(main.config, "HARDWARE_CONTROL_ENABLED", True, raising=False)
    monkeypatch.setattr(main.config, "DIVE_LEFT_PCT", 0.0, raising=False)
    monkeypatch.setattr(main.config, "DIVE_RIGHT_PCT", 0.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_KP", 0.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_KI", 0.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_KD", 0.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_LIMIT", 20.0, raising=False)
    monkeypatch.setattr(main.config, "ROLL_SIGN", 1.0, raising=False)
    params_file = tmp_path / "motion_control_params.json"
    params_file.write_text(
        json.dumps(VALID_PARAMS),
        encoding="utf-8",
    )
    monkeypatch.setattr(
        main,
        "MOTION_PARAMS_FILE",
        str(params_file),
        raising=False,
    )

    sent = []
    monkeypatch.setattr(main, "send_str_to_linux", sent.append)
    monkeypatch.setattr(
        main,
        "start_motor",
        lambda: sent.append("CMD:MOTOR_START"),
    )

    client = main.app.test_client()
    assert client.get("/api/control/start").status_code == 200
    assert sent[-2:] == [
        (
            "CTRL:20.000,0.000,1.000,0.400,0.150,0.500,15.000,"
            "20.000,5.000,35.000,1.000,1.000,0.800,0.200,15.000,0.000,0.000,"
            "0.000,0.000,0.000,20.000,1.000"
        ),
        "CMD:START",
    ]

    assert client.get("/api/control/motor_start").status_code == 200
    assert sent[-2:] == [
        (
            "CTRL:20.000,0.000,1.000,0.400,0.150,0.500,15.000,"
            "20.000,5.000,35.000,1.000,1.000,0.800,0.200,15.000,0.000,0.000,"
            "0.000,0.000,0.000,20.000,1.000"
        ),
        "CMD:MOTOR_START",
    ]
