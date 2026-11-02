import cv2
import numpy as np

from core.severity import resolve_defect_profile
from core.vision import (
    _YoloBox,
    _YoloResult,
    _draw_scaled_yolo_result,
    _resolve_yolo_annotation,
)


def test_resolves_all_five_english_annotation_styles():
    expected = {
        0: ("spall", "Mild", (0, 255, 0)),
        1: ("crack", "Moderate", (0, 255, 255)),
        2: ("hole", "Severe", (0, 0, 255)),
        3: ("root", "Severe", (255, 0, 255)),
        4: ("sediment", "Moderate", (255, 0, 0)),
    }

    for class_id, (name, severity, color) in expected.items():
        annotation = _resolve_yolo_annotation(class_id, name)
        assert annotation == {
            "name": name,
            "severity": severity,
            "bgr": color,
        }


def test_annotation_colors_are_unique():
    colors = {
        _resolve_yolo_annotation(class_id, name)["bgr"]
        for class_id, name in enumerate(
            ["spall", "crack", "hole", "root", "sediment"]
        )
    }

    assert len(colors) == 5


def test_annotation_falls_back_to_class_id_when_name_is_missing():
    assert _resolve_yolo_annotation(4) == {
        "name": "sediment",
        "severity": "Moderate",
        "bgr": (255, 0, 0),
    }


def test_unknown_annotation_returns_safe_pending_style():
    assert _resolve_yolo_annotation(99, "new-defect") == {
        "name": "new-defect",
        "severity": "Pending",
        "bgr": (128, 128, 128),
    }


def test_unknown_name_does_not_get_replaced_by_known_class_id():
    assert _resolve_yolo_annotation(0, "new-defect") == {
        "name": "new-defect",
        "severity": "Pending",
        "bgr": (128, 128, 128),
    }


def test_unknown_non_ascii_name_uses_ascii_safe_class_id():
    assert _resolve_yolo_annotation(99, "新缺陷") == {
        "name": "class-99",
        "severity": "Pending",
        "bgr": (128, 128, 128),
    }


def test_spall_model_name_maps_to_existing_chinese_business_profile():
    profile = resolve_defect_profile(0, "spall")

    assert (
        profile["type"],
        profile["severity"],
        profile["severity_code"],
    ) == ("管壁脱落", "轻度", 1)


def test_draw_uses_english_label_and_blue_sediment_color(monkeypatch):
    frame = np.zeros((100, 100, 3), dtype=np.uint8)
    result = _YoloResult(
        frame,
        [_YoloBox([10, 10, 30, 30], 4, 0.876)],
        {4: "sediment"},
    )
    labels = []
    real_put_text = cv2.putText

    def capture_put_text(image, text, *args, **kwargs):
        labels.append(text)
        return real_put_text(image, text, *args, **kwargs)

    monkeypatch.setattr(cv2, "putText", capture_put_text)
    rendered = _draw_scaled_yolo_result(frame, result, (100, 100))

    assert labels == ["sediment | Moderate | 0.88"]
    assert labels[0].isascii()
    assert tuple(rendered[10, 10]) == (255, 0, 0)
