import importlib

import numpy as np


def test_linux_default_model_uses_onnx(monkeypatch):
    monkeypatch.setattr("platform.system", lambda: "Linux")

    import config

    config = importlib.reload(config)

    assert config.MODEL_PATH.endswith("best.onnx")


class _FakeOpenCvNet:
    def __init__(self, events, output=None):
        self.events = events
        self.output = (
            np.zeros((1, 9, 1008), dtype=np.float32)
            if output is None
            else output
        )

    def setPreferableBackend(self, backend):
        self.events.append(("backend", backend))

    def setPreferableTarget(self, target):
        self.events.append(("target", target))

    def setInput(self, blob):
        self.events.append(("input", blob))

    def forward(self):
        self.events.append(("forward",))
        return self.output


def test_onnx_model_loads_with_opencv_cpu_and_warms_up(monkeypatch, tmp_path):
    import core.vision as vision

    model_path = tmp_path / "model.onnx"
    model_path.write_bytes(b"placeholder")
    events = []
    fake_net = _FakeOpenCvNet(events)

    def fake_read_net(path):
        events.append(("load", path))
        return fake_net

    monkeypatch.setattr(vision.cv2.dnn, "readNetFromONNX", fake_read_net)

    model = vision._OnnxYoloModel(str(model_path))

    assert events[0] == ("load", str(model_path))
    assert events[1] == ("backend", vision.cv2.dnn.DNN_BACKEND_OPENCV)
    assert events[2] == ("target", vision.cv2.dnn.DNN_TARGET_CPU)
    assert events[3][0] == "input"
    assert events[3][1].shape == (1, 3, 192, 256)
    assert events[3][1].dtype == np.float32
    assert np.count_nonzero(events[3][1]) == 0
    assert events[4] == ("forward",)
    assert model.names == dict(vision.config.YOLO_CLASS_NAMES)


def test_onnx_model_predict_uses_opencv_blob_and_existing_postprocess(monkeypatch):
    import core.vision as vision

    events = []
    output = np.zeros((1, 9, 1008), dtype=np.float32)
    fake_net = _FakeOpenCvNet(events, output=output)
    fake_blob = np.full((1, 3, 192, 256), 0.25, dtype=np.float32)
    blob_calls = []
    postprocess_calls = []

    model = object.__new__(vision._OnnxYoloModel)
    model.net = fake_net
    model.input_w = 256
    model.input_h = 192
    model.names = dict(vision.config.YOLO_CLASS_NAMES)

    def fake_blob_from_image(image, scalefactor, size, mean, swapRB, crop, ddepth):
        blob_calls.append(
            {
                "image": image,
                "scalefactor": scalefactor,
                "size": size,
                "mean": mean,
                "swapRB": swapRB,
                "crop": crop,
                "ddepth": ddepth,
            }
        )
        return fake_blob

    def fake_postprocess(actual_output, width, height, conf):
        postprocess_calls.append((actual_output, width, height, conf))
        return ["box"]

    monkeypatch.setattr(vision.cv2.dnn, "blobFromImage", fake_blob_from_image)
    monkeypatch.setattr(model, "_postprocess", fake_postprocess)

    source = np.zeros((480, 640, 3), dtype=np.uint8)
    result = model.predict(source=source, conf=0.35)[0]

    assert blob_calls[0]["image"].shape == (192, 256, 3)
    assert blob_calls[0]["scalefactor"] == 1.0 / 255.0
    assert blob_calls[0]["size"] == (256, 192)
    assert blob_calls[0]["mean"] == (0, 0, 0)
    assert blob_calls[0]["swapRB"] is True
    assert blob_calls[0]["crop"] is False
    assert blob_calls[0]["ddepth"] == vision.cv2.CV_32F
    assert events == [("input", fake_blob), ("forward",)]
    assert postprocess_calls == [(output, 256, 192, 0.35)]
    assert result._frame.shape == (192, 256, 3)
    assert result.boxes == ["box"]
    assert result.names == dict(vision.config.YOLO_CLASS_NAMES)


def test_init_yolo_uses_direct_onnx_backend_for_onnx_models(monkeypatch, tmp_path):
    import core.vision as vision

    model_path = tmp_path / "model.onnx"
    model_path.write_bytes(b"placeholder")
    created = []

    class FakeOnnxModel:
        names = {0: "root", 1: "hole", 2: "crack"}

        def __init__(self, path):
            created.append(path)

    monkeypatch.setattr(vision.config, "MODEL_PATH", str(model_path))
    monkeypatch.setattr(vision, "_model", None)
    monkeypatch.setattr(vision, "_model_names", {})
    monkeypatch.setattr(vision, "_OnnxYoloModel", FakeOnnxModel)

    assert vision.init_yolo() is True
    assert created == [str(model_path)]
    assert vision._model_names == FakeOnnxModel.names


def test_init_yolo_flushes_start_and_completion_logs(monkeypatch, tmp_path):
    import core.vision as vision

    model_path = tmp_path / "model.onnx"
    model_path.write_bytes(b"placeholder")
    printed = []

    class FakeOnnxModel:
        names = {0: "spall"}

        def __init__(self, path):
            assert path == str(model_path)

    def record_print(*args, **kwargs):
        printed.append((" ".join(str(arg) for arg in args), kwargs))

    monkeypatch.setattr(vision.config, "MODEL_PATH", str(model_path))
    monkeypatch.setattr(vision, "_model", None)
    monkeypatch.setattr(vision, "_model_names", {})
    monkeypatch.setattr(vision, "_OnnxYoloModel", FakeOnnxModel)
    monkeypatch.setattr(vision, "print", record_print, raising=False)

    assert vision.init_yolo() is True
    assert printed[0] == (
        f"[YOLO] Loading model: {model_path}",
        {"flush": True},
    )
    assert printed[-1] == (
        "[YOLO] Model initialization completed.",
        {"flush": True},
    )
