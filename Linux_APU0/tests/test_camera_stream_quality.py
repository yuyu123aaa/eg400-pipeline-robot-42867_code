import re
from pathlib import Path

import numpy as np


class FakeBox:
    def __init__(self, xyxy, cls_id=0, conf=0.9):
        self.xyxy = np.array([xyxy], dtype=np.float32)
        self.cls = np.array([cls_id], dtype=np.float32)
        self.conf = np.array([conf], dtype=np.float32)


def test_yolo_boxes_are_drawn_on_camera_resolution_frame():
    import core.vision as vision

    camera_frame = np.zeros((480, 640, 3), dtype=np.uint8)
    yolo_result = vision._YoloResult(
        np.zeros((192, 256, 3), dtype=np.uint8),
        [FakeBox([10, 20, 50, 80])],
        {0: "defect"},
    )

    annotated = vision._draw_scaled_yolo_result(
        camera_frame,
        yolo_result,
        source_size=(256, 192),
    )

    assert annotated.shape == (480, 640, 3)
    assert np.any(annotated != camera_frame)


def test_camera_panel_keeps_four_three_aspect_ratio():
    css = Path("dashboard/style.css").read_text(encoding="utf-8")
    camera_rule = re.search(
        r"#section-camera\s+\.middle-card-body\s*\{(?P<body>[^}]*)\}",
        css,
        flags=re.S,
    )

    assert camera_rule is not None
    body = camera_rule.group("body")
    assert "aspect-ratio: 4 / 3" in body
    assert "flex: 0 1 auto" in body
    assert "flex: 1;" not in body
