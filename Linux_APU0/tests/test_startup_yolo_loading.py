import importlib


def test_vision_import_does_not_require_ultralytics():
    vision = importlib.import_module("core.vision")

    assert hasattr(vision, "init_yolo")


def test_yolo_preload_is_enabled_by_default():
    config = importlib.import_module("config")

    assert config.PRELOAD_YOLO is True


def _replace_background_services_with_recorders(monkeypatch, main, calls):
    monkeypatch.setattr(main.config, "WEB_ONLY", False, raising=False)
    monkeypatch.setattr(main.config, "SIMULATION_MODE", False, raising=False)
    monkeypatch.setattr(main.config, "HARDWARE_CONTROL_ENABLED", True, raising=False)
    monkeypatch.setattr(main, "start_udp_listener", lambda: calls.append("udp"))
    monkeypatch.setattr(main, "start_mapping_thread", lambda: calls.append("mapping"))
    monkeypatch.setattr(main, "start_camera_thread", lambda: calls.append("camera"))
    monkeypatch.setattr(main, "start_gamepad_listener", lambda: calls.append("gamepad"))
    monkeypatch.setattr(main, "start_temperature_thread", lambda: calls.append("temperature"))


def test_background_services_preload_yolo_before_other_services(monkeypatch):
    main = importlib.import_module("main")

    calls = []
    monkeypatch.setattr(main.config, "PRELOAD_YOLO", True, raising=False)
    monkeypatch.setattr(main, "init_yolo", lambda: calls.append("init_yolo") or True)
    _replace_background_services_with_recorders(monkeypatch, main, calls)

    assert main.start_background_services() is True
    assert calls == ["init_yolo", "udp", "mapping", "camera", "gamepad", "temperature"]


def test_background_services_stop_when_yolo_preload_fails(monkeypatch):
    main = importlib.import_module("main")

    calls = []
    monkeypatch.setattr(main.config, "PRELOAD_YOLO", True, raising=False)
    monkeypatch.setattr(main, "init_yolo", lambda: calls.append("init_yolo") or False)
    _replace_background_services_with_recorders(monkeypatch, main, calls)

    assert main.start_background_services() is False
    assert calls == ["init_yolo"]


def test_background_services_can_skip_yolo_preload(monkeypatch):
    main = importlib.import_module("main")

    calls = []
    monkeypatch.setattr(main.config, "PRELOAD_YOLO", False, raising=False)
    monkeypatch.setattr(main, "init_yolo", lambda: calls.append("init_yolo") or True)
    _replace_background_services_with_recorders(monkeypatch, main, calls)

    assert main.start_background_services() is True
    assert calls == ["udp", "mapping", "camera", "gamepad", "temperature"]
