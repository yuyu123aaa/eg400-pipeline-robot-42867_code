def test_start_waits_for_baremetal_ack(monkeypatch):
    import config
    import core.motor_control as motor_control

    sent = []
    monkeypatch.setattr(motor_control, "begin_inspection", lambda: None)
    monkeypatch.setattr(motor_control, "send_str_to_linux", sent.append)
    config.system_status["is_moving"] = False

    motor_control.start_motor()

    assert sent == ["CMD:MOTOR_START"]
    assert config.system_status["motor_command_state"] == "pending"
    assert config.system_status["is_moving"] is False


def test_start_ack_is_the_only_transition_to_running():
    import config
    from core.comm import apply_control_feedback

    apply_control_feedback({
        "control_ack": "ACK: 10HZ CENTER CONTROL STARTED"
    })

    assert config.system_status["motor_command_state"] == "running"
    assert config.system_status["is_moving"] is True
    assert config.system_status["nav_fault"] == ""


def test_rejected_ack_and_navigation_fault_clear_motion():
    import config
    from core.comm import apply_control_feedback

    config.system_status["is_moving"] = True
    apply_control_feedback({
        "control_ack": "ACK: REJECTED, HOST HEARTBEAT LOST"
    })
    assert config.system_status["motor_command_state"] == "rejected"
    assert config.system_status["is_moving"] is False

    config.system_status["is_moving"] = True
    apply_control_feedback({
        "nav_fault": "FAULT:NAV:2,IMU_STALE"
    })
    assert config.system_status["motor_command_state"] == "rejected"
    assert config.system_status["is_moving"] is False
    assert config.system_status["nav_fault"] == "FAULT:NAV:2,IMU_STALE"
