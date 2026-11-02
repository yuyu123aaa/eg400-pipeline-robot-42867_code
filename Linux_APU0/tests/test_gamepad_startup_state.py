import json


def test_first_gamepad_packet_does_not_toggle_system_start(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []

    monkeypatch.setattr(gamepad, "_last_buttons", None, raising=False)
    monkeypatch.setattr(gamepad, "_last_packet_time", 0.0, raising=False)
    monkeypatch.setattr(gamepad, "_active_session_id", None, raising=False)
    monkeypatch.setattr(gamepad, "_last_session_seq", -1, raising=False)
    monkeypatch.setattr(gamepad, "_closed_session_ids", set(), raising=False)
    monkeypatch.setattr(gamepad, "_modern_session_seen", False, raising=False)
    monkeypatch.setattr(config, "MANUAL_MODE", False, raising=False)
    monkeypatch.setitem(config.system_status, "is_running", False)
    monkeypatch.setitem(config.system_status, "manual_mode", False)
    monkeypatch.setitem(config.system_status, "gamepad_online", False)
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)

    packet = {
        "buttons": {"a": True, "b": False, "dpad_up": False},
        "throttle": 0,
        "turn": 0,
    }

    gamepad._handle_packet(json.dumps(packet).encode("utf-8"))

    assert config.system_status["gamepad_online"] is True
    assert config.system_status["is_running"] is False
    assert sent_commands == []
    assert gamepad._last_buttons == packet["buttons"]


def test_sonar_fault_blocks_manual_mode_and_remote_speed(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []
    buttons = {"a": False, "b": False, "x": True, "dpad_up": False}

    monkeypatch.setattr(gamepad, "_last_buttons",
                        {name: False for name in buttons}, raising=False)
    monkeypatch.setattr(gamepad, "_active_session_id",
                        gamepad._LEGACY_SESSION_ID, raising=False)
    monkeypatch.setattr(gamepad, "_closed_session_ids", set(), raising=False)
    monkeypatch.setattr(gamepad, "_modern_session_seen", False, raising=False)
    monkeypatch.setattr(config, "MANUAL_MODE", False, raising=False)
    monkeypatch.setitem(config.system_status, "is_running", True)
    monkeypatch.setitem(config.system_status, "is_moving", True)
    monkeypatch.setitem(config.system_status, "manual_mode", False)
    monkeypatch.setitem(config.system_status, "sonar_fault", True)
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)

    gamepad._handle_packet(json.dumps({
        "buttons": buttons, "throttle": 80, "turn": 20,
    }).encode("utf-8"))

    assert config.MANUAL_MODE is False
    assert config.system_status["manual_mode"] is False
    assert config.system_status["is_moving"] is False
    assert sent_commands == []


def test_lb_sends_semantic_left_light_command(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []
    buttons = {
        "a": False, "b": False, "x": False, "y": False,
        "lb": True, "rb": False, "dpad_up": False,
    }

    monkeypatch.setattr(gamepad, "_last_buttons",
                        {name: False for name in buttons}, raising=False)
    monkeypatch.setattr(gamepad, "_active_session_id",
                        gamepad._LEGACY_SESSION_ID, raising=False)
    monkeypatch.setattr(gamepad, "_closed_session_ids", set(), raising=False)
    monkeypatch.setattr(gamepad, "_modern_session_seen", False, raising=False)
    monkeypatch.setattr(config, "MANUAL_MODE", False, raising=False)
    monkeypatch.setitem(config.system_status, "is_running", True)
    monkeypatch.setitem(config.system_status, "light1_on", False)
    monkeypatch.setitem(config.system_status, "light2_on", False)
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)

    gamepad._handle_packet(json.dumps({
        "buttons": buttons, "throttle": 0, "turn": 0,
    }).encode("utf-8"))

    assert sent_commands == ["CMD:LED1ON"]
    assert config.system_status["light1_on"] is True
    assert config.system_status["light2_on"] is False


def test_rb_sends_semantic_right_light_command(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []
    buttons = {
        "a": False, "b": False, "x": False, "y": False,
        "lb": False, "rb": True, "dpad_up": False,
    }

    monkeypatch.setattr(gamepad, "_last_buttons",
                        {name: False for name in buttons}, raising=False)
    monkeypatch.setattr(gamepad, "_active_session_id",
                        gamepad._LEGACY_SESSION_ID, raising=False)
    monkeypatch.setattr(gamepad, "_closed_session_ids", set(), raising=False)
    monkeypatch.setattr(gamepad, "_modern_session_seen", False, raising=False)
    monkeypatch.setattr(config, "MANUAL_MODE", False, raising=False)
    monkeypatch.setitem(config.system_status, "is_running", True)
    monkeypatch.setitem(config.system_status, "light1_on", False)
    monkeypatch.setitem(config.system_status, "light2_on", False)
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)

    gamepad._handle_packet(json.dumps({
        "buttons": buttons, "throttle": 0, "turn": 0,
    }).encode("utf-8"))

    assert sent_commands == ["CMD:LED2ON"]
    assert config.system_status["light1_on"] is False
    assert config.system_status["light2_on"] is True
