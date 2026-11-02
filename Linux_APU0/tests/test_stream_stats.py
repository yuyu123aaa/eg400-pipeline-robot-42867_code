from pathlib import Path


def test_index_passes_stream_stats_toggle(monkeypatch):
    import main

    captured = {}

    def fake_render_template(template_name, **context):
        captured["template_name"] = template_name
        captured.update(context)
        return context

    monkeypatch.setattr(main.config, "SHOW_STREAM_STATS", True, raising=False)
    monkeypatch.setattr(main, "render_template", fake_render_template)

    response = main.index()

    assert captured["template_name"] == "dashboard/index.html"
    assert response["show_stream_stats"] is True


def test_api_data_defaults_to_stopped(monkeypatch):
    import config
    import main

    monkeypatch.setitem(config.system_status, "is_running", False)

    response = main.app.test_client().get("/api/data")

    assert response.status_code == 200
    assert response.get_json()["is_running"] is False


def test_video_feed_frames_include_id_and_timestamp(monkeypatch):
    import core.vision as vision

    monkeypatch.setitem(vision.config.system_status, "is_running", True)
    monkeypatch.setattr(vision.config, "global_frame", b"jpeg-bytes", raising=False)
    monkeypatch.setattr(vision.config, "global_frame_id", 42, raising=False)
    monkeypatch.setattr(vision.config, "global_frame_timestamp_ms", 123456789, raising=False)

    generator = vision.gen_frames()
    frame = next(generator)
    generator.close()

    assert b"X-Frame-Id: 42\r\n" in frame
    assert b"X-Frame-Time-Ms: 123456789\r\n" in frame
    assert b"Content-Length: 10\r\n" in frame
    assert frame.endswith(b"jpeg-bytes\r\n")


def test_video_feed_tracks_connected_stream_clients(monkeypatch):
    import core.vision as vision

    monkeypatch.setitem(vision.config.system_status, "is_running", True)
    monkeypatch.setattr(vision.config, "global_frame", b"jpeg-bytes", raising=False)
    monkeypatch.setattr(vision.config, "global_frame_id", 42, raising=False)
    monkeypatch.setattr(vision.config, "global_frame_timestamp_ms", 123456789, raising=False)
    monkeypatch.setattr(vision, "_stream_client_count", 0, raising=False)

    generator = vision.gen_frames()
    assert vision._get_stream_client_count() == 0

    next(generator)
    assert vision._get_stream_client_count() == 1

    generator.close()
    assert vision._get_stream_client_count() == 0


def test_video_feed_response_disables_http_buffering():
    import main

    response = main.video_feed()
    try:
        assert response.headers["Cache-Control"] == "no-store, no-cache, must-revalidate, max-age=0"
        assert response.headers["Pragma"] == "no-cache"
        assert response.headers["Expires"] == "0"
        assert response.headers["X-Accel-Buffering"] == "no"
    finally:
        response.close()


def test_publish_stream_frame_uses_capture_timestamp(monkeypatch):
    import core.vision as vision

    monkeypatch.setattr(vision.config, "global_frame_id", 7, raising=False)

    vision._publish_stream_frame(b"jpeg-bytes", capture_timestamp_ms=222333)

    assert vision.config.global_frame == b"jpeg-bytes"
    assert vision.config.global_frame_id == 8
    assert vision.config.global_frame_timestamp_ms == 222333


def test_publish_stream_frame_updates_server_video_fps(monkeypatch):
    import core.vision as vision

    monotonic_times = iter([0.4, 1.0])
    monkeypatch.setattr(vision.time, "monotonic", lambda: next(monotonic_times))
    monkeypatch.setattr(vision, "_stream_fps_window_started_at", 0.0, raising=False)
    monkeypatch.setattr(vision, "_stream_fps_frame_count", 0, raising=False)
    monkeypatch.setitem(vision.config.system_status, "stream_fps", 0.0)

    vision._publish_stream_frame(b"frame-1", capture_timestamp_ms=100)
    assert vision.config.system_status["stream_fps"] == 0.0

    vision._publish_stream_frame(b"frame-2", capture_timestamp_ms=200)
    assert vision.config.system_status["stream_fps"] == 2.0


def test_template_contains_stream_stats_panel_and_measured_stream_client():
    html = Path("dashboard/index.html").read_text(encoding="utf-8")
    js = Path("dashboard/app.js").read_text(encoding="utf-8")

    assert '<script src="/dashboard/app.js"></script>' in html
    assert "stream-stats-panel" in html
    assert "stream-fps-value" in html
    assert "STREAM_STATS_ENABLED = {{" not in html
    assert "STREAM_STATS_ENABLED = {{" not in js
    assert "FPS" in html
    assert "网页 FPS" not in html
    assert "stream-frame-value" not in html
    assert "帧 --" not in html
    assert "stream-latency-value" not in html
    assert "画面延迟" not in html
    assert "/api/stream_time" not in html
    assert 'data-stream-stats-enabled="true"' not in html


def test_camera_stream_uses_native_mjpeg_and_api_fps_by_default():
    js = Path("dashboard/app.js").read_text(encoding="utf-8")

    assert "const USE_BROWSER_MJPEG_PARSER = false;" in js
    assert "if (USE_BROWSER_MJPEG_PARSER)" in js
    assert "startMeasuredVideoStream();" in js
    assert 'img.src = "/video_feed?" + new Date().getTime();' in js
    assert "data.stream_fps" in js
    assert "STREAM_STATS_ENABLED" not in js


def test_camera_stream_only_runs_on_overview_and_camera_views():
    js = Path("dashboard/app.js").read_text(encoding="utf-8")

    assert "let isSystemRunning = false;" in js
    assert "let cameraVideoRunning = false;" in js
    assert "function isCameraStreamView()" in js
    assert "currentDashboardView === 'overview'" in js
    assert "currentDashboardView === 'camera'" in js
    assert "function syncCameraVideoForView()" in js
    assert "isSystemRunning && isCameraStreamView()" in js
    assert "syncCameraVideoForView();" in js


def test_stream_stats_panel_is_in_camera_header_not_next_to_video():
    html = Path("dashboard/index.html").read_text(encoding="utf-8")
    camera_start = html.index('id="section-camera"')
    header_start = html.index('middle-card-header', camera_start)
    panel_start = html.index('stream-stats-panel', camera_start)
    display_start = html.index('camera-display-row', camera_start)

    assert header_start < panel_start < display_start
    assert "FPS" in html
