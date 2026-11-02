import numpy as np


class FakeBox:
    def __init__(self):
        self.xyxy = np.array([[1, 2, 10, 12]], dtype=np.float32)
        self.cls = np.array([0], dtype=np.float32)
        self.conf = np.array([0.9], dtype=np.float32)


class FakeModel:
    names = {0: "root"}

    def __init__(self):
        self.calls = 0

    def predict(self, source, conf=0.3, verbose=False):
        self.calls += 1
        from core.vision import _YoloResult

        return [_YoloResult(source, [FakeBox()], self.names)]


def test_yolo_interval_one_runs_detection_every_frame(monkeypatch):
    import core.vision as vision

    model = FakeModel()
    frame = np.zeros((192, 256, 3), dtype=np.uint8)
    monkeypatch.setattr(vision.config, "YOLO_DETECT_EVERY_N_FRAMES", 1, raising=False)

    state = vision._YoloFrameIntervalState()
    for _ in range(3):
        result, is_fresh = vision._predict_yolo_for_frame(model, frame, state)
        assert is_fresh is True
        assert len(result.boxes) == 1

    assert model.calls == 3


def test_yolo_interval_reuses_last_result_between_detection_frames(monkeypatch):
    import core.vision as vision

    model = FakeModel()
    frame = np.zeros((192, 256, 3), dtype=np.uint8)
    monkeypatch.setattr(vision.config, "YOLO_DETECT_EVERY_N_FRAMES", 3, raising=False)

    state = vision._YoloFrameIntervalState()
    fresh_flags = []
    for _ in range(5):
        result, is_fresh = vision._predict_yolo_for_frame(model, frame, state)
        fresh_flags.append(is_fresh)
        assert len(result.boxes) == 1

    assert fresh_flags == [True, False, False, True, False]
    assert model.calls == 2


def test_yolo_interval_without_cached_result_runs_detection(monkeypatch):
    import core.vision as vision

    model = FakeModel()
    frame = np.zeros((192, 256, 3), dtype=np.uint8)
    monkeypatch.setattr(vision.config, "YOLO_DETECT_EVERY_N_FRAMES", 4, raising=False)

    state = vision._YoloFrameIntervalState()
    state.frames_since_detection = 1

    result, is_fresh = vision._predict_yolo_for_frame(model, frame, state)

    assert is_fresh is True
    assert len(result.boxes) == 1
    assert model.calls == 1
