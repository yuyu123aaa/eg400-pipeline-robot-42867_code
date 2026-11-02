import threading
import time

import numpy as np


def _wait_until(predicate, timeout=1.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.005)
    return predicate()


class _BlockingModel:
    def __init__(self):
        self.calls = []
        self.first_started = threading.Event()
        self.release_first = threading.Event()

    def predict(self, source, conf=0.3, verbose=False):
        value = int(source[0, 0, 0])
        self.calls.append(value)
        if len(self.calls) == 1:
            self.first_started.set()
            assert self.release_first.wait(timeout=1.0)
        return [f"result-{value}"]


def test_async_yolo_worker_replaces_queued_frame_with_latest():
    import core.vision as vision

    worker = vision._LatestYoloWorker()
    model = _BlockingModel()
    worker.start()

    try:
        worker.submit(model, np.full((2, 2, 3), 1, dtype=np.uint8), 0.3)
        assert model.first_started.wait(timeout=1.0)

        worker.submit(model, np.full((2, 2, 3), 2, dtype=np.uint8), 0.3)
        worker.submit(model, np.full((2, 2, 3), 3, dtype=np.uint8), 0.3)
        model.release_first.set()

        assert _wait_until(lambda: worker.latest()[0] == 2)
        assert model.calls == [1, 3]
        assert worker.latest() == (2, "result-3")
    finally:
        model.release_first.set()
        worker.stop()


def test_async_yolo_worker_discards_result_invalidated_during_inference():
    import core.vision as vision

    worker = vision._LatestYoloWorker()
    model = _BlockingModel()
    worker.start()

    try:
        worker.submit(model, np.full((2, 2, 3), 1, dtype=np.uint8), 0.3)
        assert model.first_started.wait(timeout=1.0)

        worker.invalidate()
        model.release_first.set()

        assert _wait_until(lambda: len(model.calls) == 1)
        time.sleep(0.02)
        assert worker.latest() == (0, None)
    finally:
        model.release_first.set()
        worker.stop()


def test_camera_capture_keeps_driver_default_buffering_for_full_fps():
    import core.vision as vision

    class FakeCapture:
        def __init__(self):
            self.settings = []

        def set(self, key, value):
            self.settings.append((key, value))
            return True

    capture = FakeCapture()
    vision._configure_camera_capture(capture)

    configured_properties = {key for key, _ in capture.settings}
    assert vision.cv2.CAP_PROP_BUFFERSIZE not in configured_properties


def test_camera_loop_submits_yolo_without_synchronous_predict_call():
    import inspect
    import core.vision as vision

    source = inspect.getsource(vision._camera_loop)

    assert "_yolo_worker.submit(" in source
    assert "_predict_yolo_for_frame(" not in source
    assert source.index("if config.ENABLE_YOLO") < source.index("yolo_frame = cv2.resize")


def test_stream_encoding_is_skipped_without_client_or_photo(monkeypatch):
    import core.vision as vision

    monkeypatch.setattr(vision, "_stream_client_count", 0, raising=False)
    monkeypatch.setattr(vision.config, "take_photo_flag", False)
    assert vision._should_encode_stream_frame() is False

    monkeypatch.setattr(vision, "_stream_client_count", 1, raising=False)
    assert vision._should_encode_stream_frame() is True

    monkeypatch.setattr(vision, "_stream_client_count", 0, raising=False)
    monkeypatch.setattr(vision.config, "take_photo_flag", True)
    assert vision._should_encode_stream_frame() is True
