from pathlib import Path
import socket
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "app"))

import config
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


def test_web_only_startup_skips_all_background_services(monkeypatch):
    calls = []
    monkeypatch.setattr(config, "WEB_ONLY", True)
    monkeypatch.setattr(main, "init_yolo", lambda: calls.append("yolo"))
    monkeypatch.setattr(main, "start_udp_listener", lambda: calls.append("udp"))
    monkeypatch.setattr(main, "start_mapping_thread", lambda: calls.append("mapping"))
    monkeypatch.setattr(main, "start_camera_thread", lambda: calls.append("camera"))
    monkeypatch.setattr(main, "start_gamepad_listener", lambda: calls.append("gamepad"))
    monkeypatch.setattr(main, "start_temperature_thread", lambda: calls.append("temperature"))

    assert main.start_background_services() is True
    assert calls == []


def test_simulation_mode_does_not_send_udp(monkeypatch):
    sent = []
    monkeypatch.setattr(config, "WEB_ONLY", True)
    monkeypatch.setattr(config, "SIMULATION_MODE", True)
    monkeypatch.setattr(config, "HARDWARE_CONTROL_ENABLED", True)

    class FailingSocket:
        def __enter__(self):
            raise AssertionError("simulation mode must not create a UDP socket")

        def __exit__(self, exc_type, exc, tb):
            return False

    monkeypatch.setattr(socket, "socket", FailingSocket)
    from core.comm import send_str_to_linux

    assert send_str_to_linux("CMD:START") is False
    assert sent == []


def test_api_data_exposes_runtime_mode(monkeypatch):
    monkeypatch.setattr(config, "WEB_ONLY", True)
    monkeypatch.setattr(config, "SIMULATION_MODE", True)
    monkeypatch.setattr(config, "HARDWARE_CONTROL_ENABLED", True)

    payload = main.app.test_client().get("/api/data").get_json()

    assert payload["web_only"] is True
    assert payload["simulation_mode"] is True
    assert payload["hardware_control_enabled"] is False
    assert payload["runtime_mode"] == "web-only"


def test_web_only_control_is_explicitly_simulated(monkeypatch):
    sent = []
    monkeypatch.setattr(config, "WEB_ONLY", True)
    monkeypatch.setattr(config, "SIMULATION_MODE", True)
    monkeypatch.setattr(config, "HARDWARE_CONTROL_ENABLED", False)
    monkeypatch.setattr(main, "send_str_to_linux", sent.append)
    monkeypatch.setattr(main, "start_motor", lambda: sent.append("motor_start"))

    client = main.app.test_client()
    for action in ("start", "motor_start", "led1on", "stop"):
        response = client.get(f"/api/control/{action}")
        assert response.status_code == 200
        assert response.get_json()["status"] == "simulation"

    assert sent == []
    assert config.system_status["is_moving"] is False


def test_motion_params_are_saved_without_claiming_bare_metal_delivery(
    monkeypatch,
    tmp_path,
):
    params_file = tmp_path / "motion_control_params.json"
    monkeypatch.setattr(main, "MOTION_PARAMS_FILE", str(params_file))
    monkeypatch.setattr(config, "WEB_ONLY", True)
    monkeypatch.setattr(config, "SIMULATION_MODE", True)
    monkeypatch.setattr(config, "HARDWARE_CONTROL_ENABLED", False)
    sent = []
    monkeypatch.setattr(main, "send_str_to_linux", sent.append)

    response = main.app.test_client().post(
        "/api/control/set_motion_params",
        json=VALID_PARAMS,
    )

    assert response.status_code == 200
    assert response.get_json()["status"] == "simulation"
    assert "未下发到裸机" in response.get_json()["message"]
    assert params_file.is_file()
    assert sent == []
