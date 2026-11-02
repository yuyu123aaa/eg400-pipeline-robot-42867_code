import config
import core.comm as comm


def _reset_status(monkeypatch):
    monkeypatch.setattr(
        comm,
        "system_status",
        config.system_status,
        raising=False,
    )
    monkeypatch.setattr(config, "MANUAL_MODE", True, raising=False)
    monkeypatch.setitem(config.system_status, "is_moving", True)
    monkeypatch.setitem(config.system_status, "manual_mode", True)
    monkeypatch.setitem(config.system_status, "motor_command_state", "running")
    monkeypatch.setitem(config.system_status, "motor_control_message", "")
    monkeypatch.setitem(config.system_status, "sonar_fault", False)
    monkeypatch.setitem(config.system_status, "sonar_fault_message", "")
    monkeypatch.setitem(config.system_status, "sonar_fault_channels", [])
    monkeypatch.setitem(config.system_status, "light1_on", True)
    monkeypatch.setitem(config.system_status, "light2_on", True)


def test_fault_stops_motion_and_exposes_all_masked_channels(monkeypatch):
    _reset_status(monkeypatch)
    sent_commands = []
    monkeypatch.setattr(comm, "send_str_to_linux", sent_commands.append)

    comm.apply_sonar_fault_feedback(
        {"sonar_fault": True, "sonar_fault_mask": 5}
    )

    assert config.system_status["sonar_fault"] is True
    assert config.system_status["sonar_fault_channels"] == [
        "front",
        "right",
    ]
    assert "推进器已停止" in config.system_status["sonar_fault_message"]
    assert "照明已关闭" in config.system_status["sonar_fault_message"]
    assert "遥控已锁定" in config.system_status["sonar_fault_message"]
    assert config.system_status["is_moving"] is False
    assert config.system_status["manual_mode"] is False
    assert config.system_status["motor_command_state"] == "stopped"
    assert config.MANUAL_MODE is False
    assert config.system_status["light1_on"] is False
    assert config.system_status["light2_on"] is False
    assert sent_commands == [
        "CMD:STOP",
        "CMD:LED1OFF",
        "CMD:LED2OFF",
    ]


def test_duplicate_sonar_fault_does_not_repeat_shutdown_commands(monkeypatch):
    _reset_status(monkeypatch)
    sent_commands = []
    monkeypatch.setattr(comm, "send_str_to_linux", sent_commands.append)

    comm.apply_sonar_fault_feedback(
        {"sonar_fault": True, "sonar_fault_mask": 1}
    )
    comm.apply_sonar_fault_feedback(
        {"sonar_fault": True, "sonar_fault_mask": 3}
    )

    assert sent_commands == [
        "CMD:STOP",
        "CMD:LED1OFF",
        "CMD:LED2OFF",
    ]


def test_recovery_only_clears_alarm_and_keeps_motor_stopped(monkeypatch):
    _reset_status(monkeypatch)
    monkeypatch.setitem(config.system_status, "is_moving", False)
    monkeypatch.setitem(config.system_status, "motor_command_state", "stopped")
    monkeypatch.setitem(config.system_status, "sonar_fault", True)
    monkeypatch.setitem(
        config.system_status,
        "sonar_fault_message",
        "超声波传感器异常，裸机已制动",
    )
    monkeypatch.setitem(
        config.system_status,
        "sonar_fault_channels",
        ["left"],
    )
    monkeypatch.setitem(config.system_status, "light1_on", False)
    monkeypatch.setitem(config.system_status, "light2_on", False)

    comm.apply_sonar_fault_feedback(
        {"sonar_fault": False, "sonar_fault_mask": 0}
    )

    assert config.system_status["sonar_fault"] is False
    assert config.system_status["sonar_fault_channels"] == []
    assert config.system_status["sonar_fault_message"] == ""
    assert config.system_status["is_moving"] is False
    assert config.system_status["motor_command_state"] == "stopped"
    assert config.system_status["light1_on"] is False
    assert config.system_status["light2_on"] is False


def test_unrelated_udp_payload_does_not_change_sonar_fault_state(monkeypatch):
    _reset_status(monkeypatch)

    comm.apply_sonar_fault_feedback({"roll": 1.0})

    assert config.system_status["sonar_fault"] is False
    assert config.system_status["is_moving"] is True
