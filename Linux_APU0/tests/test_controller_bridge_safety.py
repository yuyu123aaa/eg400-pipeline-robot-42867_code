import importlib
import json
import sys
import types


def _import_controller_bridge(monkeypatch):
    monkeypatch.setitem(sys.modules, "pygame", types.SimpleNamespace())
    sys.modules.pop("controller_bridge", None)
    return importlib.import_module("controller_bridge")


def test_only_newly_pressed_buttons_are_reported(monkeypatch):
    bridge = _import_controller_bridge(monkeypatch)
    previous = {"a": True, "b": False, "x": False, "rb": False}
    current = {"a": True, "b": False, "x": True, "rb": True}

    assert bridge.get_newly_pressed_buttons(current, previous) == ["x", "rb"]


def test_entering_manual_mode_arms_baremetal_remote_control(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)
    monkeypatch.setattr(config, "MANUAL_MODE", False, raising=False)
    monkeypatch.setitem(config.system_status, "manual_mode", False)
    monkeypatch.setitem(config.system_status, "is_moving", True)

    gamepad._enter_manual()

    assert config.MANUAL_MODE is True
    assert config.system_status["manual_mode"] is True
    assert config.system_status["is_moving"] is False
    assert sent_commands == ["CMD:MANUAL_START"]


def test_disconnect_packet_stops_motion_without_pressing_emergency_stop(monkeypatch):
    bridge = _import_controller_bridge(monkeypatch)

    packet = bridge.build_disconnect_packet(seq=17, session_id="xbox-session")

    assert packet["seq"] == 17
    assert packet["session_id"] == "xbox-session"
    assert packet["connected"] is False
    assert packet["throttle"] == 0
    assert packet["turn"] == 0
    assert packet["buttons"]["b"] is False


def test_disconnect_notice_is_sent_redundantly(monkeypatch):
    bridge = _import_controller_bridge(monkeypatch)

    class RecordingSocket:
        def __init__(self):
            self.sent = []

        def sendto(self, data, target):
            self.sent.append((json.loads(data.decode("utf-8")), target))

    sock = RecordingSocket()
    bridge.send_disconnect_notice(
        sock,
        ("192.168.1.100", 9002),
        seq=17,
        session_id="xbox-session",
    )

    assert len(sock.sent) == 3
    assert all(packet["connected"] is False for packet, _ in sock.sent)
    assert all(packet["throttle"] == 0 for packet, _ in sock.sent)
    assert all(target == ("192.168.1.100", 9002) for _, target in sock.sent)


def test_disconnect_event_support_requires_pygame2_api(monkeypatch):
    bridge = _import_controller_bridge(monkeypatch)
    supported_pygame = types.SimpleNamespace(JOYDEVICEREMOVED=99)
    supported_joystick = types.SimpleNamespace(get_instance_id=lambda: 7)

    assert bridge.supports_disconnect_events(supported_pygame, supported_joystick) is True
    assert bridge.supports_disconnect_events(types.SimpleNamespace(), supported_joystick) is False
    assert bridge.supports_disconnect_events(
        supported_pygame, types.SimpleNamespace()
    ) is False


def test_main_removal_event_sends_disconnect_without_b_press(monkeypatch, capsys):
    bridge = _import_controller_bridge(monkeypatch)

    class RecordingSocket:
        def __init__(self):
            self.sent = []

        def sendto(self, data, target):
            self.sent.append((json.loads(data.decode("utf-8")), target))

        def close(self):
            pass

    class FakeJoystick:
        def init(self):
            pass

        def get_instance_id(self):
            return 7

        def get_name(self):
            return "Xbox Wireless Controller"

        def get_numaxes(self):
            return 6

        def get_numbuttons(self):
            return 10

    sock = RecordingSocket()
    joystick = FakeJoystick()
    removal_event = types.SimpleNamespace(type=2, instance_id=7)
    fake_pygame = types.SimpleNamespace(
        init=lambda: None,
        quit=lambda: None,
        QUIT=1,
        JOYDEVICEREMOVED=2,
        error=RuntimeError,
        joystick=types.SimpleNamespace(
            init=lambda: None,
            get_count=lambda: 1,
            Joystick=lambda _index: joystick,
        ),
        event=types.SimpleNamespace(
            get=lambda: [removal_event],
            pump=lambda: None,
        ),
        time=types.SimpleNamespace(
            Clock=lambda: types.SimpleNamespace(tick=lambda _hz: None)
        ),
    )
    monkeypatch.setattr(bridge, "pygame", fake_pygame)
    monkeypatch.setattr(bridge.socket, "socket", lambda *_args: sock)
    monkeypatch.setattr(
        sys,
        "argv",
        ["controller_bridge.py", "--host", "192.168.1.100"],
    )

    bridge.main()

    assert len(sock.sent) == 3
    assert all(packet["connected"] is False for packet, _ in sock.sent)
    assert all(packet["buttons"]["b"] is False for packet, _ in sock.sent)
    assert "手柄已断开" in capsys.readouterr().out


def test_gamepad_disconnect_exits_manual_mode_and_stops_motors(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []
    monkeypatch.setattr(gamepad, "_last_buttons", {"a": False}, raising=False)
    monkeypatch.setattr(gamepad, "_last_packet_time", 0.0, raising=False)
    monkeypatch.setattr(gamepad, "_active_session_id", None, raising=False)
    monkeypatch.setattr(gamepad, "_last_session_seq", -1, raising=False)
    monkeypatch.setattr(gamepad, "_closed_session_ids", set(), raising=False)
    monkeypatch.setattr(config, "MANUAL_MODE", True, raising=False)
    monkeypatch.setitem(config.system_status, "is_running", True)
    monkeypatch.setitem(config.system_status, "is_moving", True)
    monkeypatch.setitem(config.system_status, "manual_mode", True)
    monkeypatch.setitem(config.system_status, "gamepad_online", True)
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)

    packet = {
        "seq": 17,
        "session_id": "xbox-session",
        "connected": False,
        "throttle": 0,
        "turn": 0,
        "buttons": {"b": False},
    }
    gamepad._handle_packet(json.dumps(packet).encode("utf-8"))

    assert config.MANUAL_MODE is False
    assert config.system_status["manual_mode"] is False
    assert config.system_status["is_moving"] is False
    assert config.system_status["gamepad_online"] is False
    assert config.system_status["is_running"] is True
    assert sent_commands == ["CMD:STOP"]


def test_explicit_b_press_still_stops_the_whole_system(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []
    monkeypatch.setattr(gamepad, "_last_buttons", {"b": False}, raising=False)
    monkeypatch.setattr(gamepad, "_active_session_id", None, raising=False)
    monkeypatch.setattr(gamepad, "_last_session_seq", -1, raising=False)
    monkeypatch.setattr(gamepad, "_closed_session_ids", set(), raising=False)
    monkeypatch.setattr(config, "MANUAL_MODE", True, raising=False)
    monkeypatch.setitem(config.system_status, "is_running", True)
    monkeypatch.setitem(config.system_status, "manual_mode", True)
    monkeypatch.setitem(config.system_status, "gamepad_online", True)
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)

    packet = {
        "seq": 1,
        "session_id": "xbox-session",
        "connected": True,
        "throttle": 0,
        "turn": 0,
        "buttons": {"b": True},
    }
    gamepad._handle_packet(json.dumps(packet).encode("utf-8"))

    assert config.MANUAL_MODE is False
    assert config.system_status["is_running"] is False
    assert sent_commands == ["CMD:STOP"]


def test_duplicate_disconnect_notices_send_only_one_stop(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []
    monkeypatch.setattr(gamepad, "_last_buttons", {"a": False}, raising=False)
    monkeypatch.setattr(gamepad, "_active_session_id", "xbox-session", raising=False)
    monkeypatch.setattr(gamepad, "_last_session_seq", 16, raising=False)
    monkeypatch.setattr(gamepad, "_closed_session_ids", set(), raising=False)
    monkeypatch.setattr(config, "MANUAL_MODE", True, raising=False)
    monkeypatch.setitem(config.system_status, "is_running", True)
    monkeypatch.setitem(config.system_status, "manual_mode", True)
    monkeypatch.setitem(config.system_status, "gamepad_online", True)
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)

    packet = {
        "seq": 17,
        "session_id": "xbox-session",
        "connected": False,
        "throttle": 0,
        "turn": 0,
        "buttons": {"b": False},
    }
    encoded = json.dumps(packet).encode("utf-8")
    gamepad._handle_packet(encoded)
    gamepad._handle_packet(encoded)
    gamepad._handle_packet(encoded)

    assert sent_commands == ["CMD:STOP"]


def test_packets_from_closed_session_cannot_reenter_manual_mode(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []
    monkeypatch.setattr(gamepad, "_last_buttons", None, raising=False)
    monkeypatch.setattr(gamepad, "_active_session_id", None, raising=False)
    monkeypatch.setattr(gamepad, "_last_session_seq", -1, raising=False)
    monkeypatch.setattr(gamepad, "_closed_session_ids", set(), raising=False)
    monkeypatch.setattr(config, "MANUAL_MODE", True, raising=False)
    monkeypatch.setitem(config.system_status, "is_running", True)
    monkeypatch.setitem(config.system_status, "is_moving", False)
    monkeypatch.setitem(config.system_status, "manual_mode", True)
    monkeypatch.setitem(config.system_status, "gamepad_online", True)
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)

    first_packet = {
        "seq": 0,
        "session_id": "xbox-session",
        "connected": True,
        "throttle": 0,
        "turn": 0,
        "buttons": {"b": False, "dpad_up": False},
    }
    disconnect_packet = {
        "seq": 2,
        "session_id": "xbox-session",
        "connected": False,
        "throttle": 0,
        "turn": 0,
        "buttons": {"b": False, "dpad_up": False},
    }
    stale_released = {
        "seq": 1,
        "session_id": "xbox-session",
        "connected": True,
        "throttle": 0,
        "turn": 0,
        "buttons": {"b": False, "dpad_up": False},
    }
    stale_pressed = {
        **stale_released,
        "buttons": {"b": False, "dpad_up": True},
    }

    for packet in (first_packet, disconnect_packet, stale_released, stale_pressed):
        gamepad._handle_packet(json.dumps(packet).encode("utf-8"))

    assert config.MANUAL_MODE is False
    assert config.system_status["manual_mode"] is False
    assert config.system_status["gamepad_online"] is False
    assert sent_commands == ["CMD:STOP"]


def test_new_bridge_session_stops_old_manual_session_before_takeover(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []
    monkeypatch.setattr(gamepad, "_last_buttons", {"b": False}, raising=False)
    monkeypatch.setattr(gamepad, "_active_session_id", "old-session", raising=False)
    monkeypatch.setattr(gamepad, "_last_session_seq", 20, raising=False)
    monkeypatch.setattr(gamepad, "_closed_session_ids", set(), raising=False)
    monkeypatch.setattr(config, "MANUAL_MODE", True, raising=False)
    monkeypatch.setitem(config.system_status, "is_running", True)
    monkeypatch.setitem(config.system_status, "is_moving", True)
    monkeypatch.setitem(config.system_status, "manual_mode", True)
    monkeypatch.setitem(config.system_status, "gamepad_online", True)
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)

    packet = {
        "seq": 0,
        "session_id": "new-session",
        "connected": True,
        "throttle": 100,
        "turn": 0,
        "buttons": {"b": False, "dpad_up": False},
    }
    gamepad._handle_packet(json.dumps(packet).encode("utf-8"))

    assert config.MANUAL_MODE is False
    assert config.system_status["manual_mode"] is False
    assert config.system_status["is_moving"] is False
    assert gamepad._active_session_id == "new-session"
    assert "old-session" in gamepad._closed_session_ids
    assert sent_commands == ["CMD:STOP"]


def test_timeout_closes_active_session_and_stops_manual_control(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []
    monkeypatch.setattr(gamepad, "_last_packet_time", 10.0, raising=False)
    monkeypatch.setattr(gamepad, "_active_session_id", "xbox-session", raising=False)
    monkeypatch.setattr(gamepad, "_last_session_seq", 20, raising=False)
    monkeypatch.setattr(gamepad, "_closed_session_ids", set(), raising=False)
    monkeypatch.setattr(config, "MANUAL_MODE", True, raising=False)
    monkeypatch.setattr(config, "GAMEPAD_TIMEOUT_S", 0.5, raising=False)
    monkeypatch.setitem(config.system_status, "is_running", True)
    monkeypatch.setitem(config.system_status, "is_moving", True)
    monkeypatch.setitem(config.system_status, "manual_mode", True)
    monkeypatch.setitem(config.system_status, "gamepad_online", True)
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)

    gamepad._check_timeout(now=11.0)

    assert config.MANUAL_MODE is False
    assert config.system_status["gamepad_online"] is False
    assert gamepad._active_session_id is None
    assert "xbox-session" in gamepad._closed_session_ids
    assert sent_commands == ["CMD:STOP"]


def test_gamepad_timeout_does_not_stop_web_automatic_motion(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []
    monkeypatch.setattr(gamepad, "_last_packet_time", 10.0, raising=False)
    monkeypatch.setattr(gamepad, "_active_session_id", "xbox-session", raising=False)
    monkeypatch.setattr(gamepad, "_last_session_seq", 20, raising=False)
    monkeypatch.setattr(gamepad, "_closed_session_ids", set(), raising=False)
    monkeypatch.setattr(config, "MANUAL_MODE", False, raising=False)
    monkeypatch.setattr(config, "GAMEPAD_TIMEOUT_S", 0.5, raising=False)
    monkeypatch.setitem(config.system_status, "is_running", True)
    monkeypatch.setitem(config.system_status, "is_moving", True)
    monkeypatch.setitem(config.system_status, "manual_mode", False)
    monkeypatch.setitem(config.system_status, "gamepad_online", True)
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)

    gamepad._check_timeout(now=11.0)

    assert config.MANUAL_MODE is False
    assert config.system_status["is_moving"] is True
    assert config.system_status["gamepad_online"] is False
    assert gamepad._active_session_id is None
    assert "xbox-session" in gamepad._closed_session_ids
    assert sent_commands == []


def test_legacy_packet_is_rejected_after_modern_session_disconnect(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []
    monkeypatch.setattr(gamepad, "_last_buttons", None, raising=False)
    monkeypatch.setattr(gamepad, "_active_session_id", None, raising=False)
    monkeypatch.setattr(gamepad, "_last_session_seq", -1, raising=False)
    monkeypatch.setattr(gamepad, "_closed_session_ids", set(), raising=False)
    monkeypatch.setattr(gamepad, "_modern_session_seen", False, raising=False)
    monkeypatch.setattr(config, "MANUAL_MODE", True, raising=False)
    monkeypatch.setitem(config.system_status, "is_running", True)
    monkeypatch.setitem(config.system_status, "is_moving", True)
    monkeypatch.setitem(config.system_status, "manual_mode", True)
    monkeypatch.setitem(config.system_status, "gamepad_online", True)
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)

    disconnect_packet = {
        "seq": 1,
        "session_id": "modern-session",
        "connected": False,
        "throttle": 0,
        "turn": 0,
        "buttons": {"b": False, "dpad_up": False},
    }
    legacy_packet = {
        "connected": True,
        "throttle": 100,
        "turn": 0,
        "buttons": {"b": False, "dpad_up": True},
    }

    gamepad._handle_packet(json.dumps(disconnect_packet).encode("utf-8"))
    gamepad._handle_packet(json.dumps(legacy_packet).encode("utf-8"))

    assert config.MANUAL_MODE is False
    assert config.system_status["gamepad_online"] is False
    assert sent_commands == ["CMD:STOP"]


def test_modern_session_stops_active_legacy_manual_session(monkeypatch):
    import config
    import core.gamepad as gamepad

    sent_commands = []
    monkeypatch.setattr(gamepad, "_last_buttons", {"b": False}, raising=False)
    monkeypatch.setattr(
        gamepad, "_active_session_id", gamepad._LEGACY_SESSION_ID, raising=False
    )
    monkeypatch.setattr(gamepad, "_last_session_seq", -1, raising=False)
    monkeypatch.setattr(gamepad, "_closed_session_ids", set(), raising=False)
    monkeypatch.setattr(gamepad, "_modern_session_seen", False, raising=False)
    monkeypatch.setattr(config, "MANUAL_MODE", True, raising=False)
    monkeypatch.setitem(config.system_status, "is_running", True)
    monkeypatch.setitem(config.system_status, "is_moving", True)
    monkeypatch.setitem(config.system_status, "manual_mode", True)
    monkeypatch.setitem(config.system_status, "gamepad_online", True)
    monkeypatch.setattr(gamepad, "send_str_to_linux", sent_commands.append)

    packet = {
        "seq": 0,
        "session_id": "modern-session",
        "connected": True,
        "throttle": 100,
        "turn": 0,
        "buttons": {"b": False, "dpad_up": False},
    }
    gamepad._handle_packet(json.dumps(packet).encode("utf-8"))

    assert config.MANUAL_MODE is False
    assert gamepad._active_session_id == "modern-session"
    assert gamepad._LEGACY_SESSION_ID in gamepad._closed_session_ids
    assert sent_commands == ["CMD:STOP"]
