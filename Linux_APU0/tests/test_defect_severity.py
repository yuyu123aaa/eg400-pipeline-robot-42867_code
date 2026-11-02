import csv

import numpy as np

from core.severity import resolve_defect_profile
from core.vision import (
    _YoloBox,
    _YoloResult,
    _append_defect_csv,
    _build_defect_log,
    _draw_scaled_yolo_result,
)


def test_resolves_all_five_fixed_severity_levels():
    expected = {
        "spalling": ("管壁脱落", "轻度", 1),
        "crack": ("裂缝", "中度", 2),
        "sediment": ("泥沙淤积", "中度", 2),
        "hole": ("破洞", "重度", 3),
        "root": ("树根侵入", "重度", 3),
    }

    for model_name, values in expected.items():
        profile = resolve_defect_profile(99, model_name)
        assert (
            profile["type"],
            profile["severity"],
            profile["severity_code"],
        ) == values


def test_resolves_chinese_model_labels():
    assert resolve_defect_profile(99, "管壁脱落")["severity"] == "轻度"
    assert resolve_defect_profile(99, "裂纹")["severity"] == "中度"
    assert resolve_defect_profile(99, "泥沙淤积")["severity"] == "中度"
    assert resolve_defect_profile(99, "破洞")["severity"] == "重度"
    assert resolve_defect_profile(99, "树根侵入")["severity"] == "重度"


def test_unknown_class_returns_safe_pending_profile():
    profile = resolve_defect_profile(99, "new-defect")

    assert profile == {
        "type": "new-defect",
        "severity": "待定",
        "severity_code": 0,
        "color": "gray",
        "bgr": (128, 128, 128),
        "advice": "建议人工复核",
    }


def test_overlay_uses_severity_color():
    frame = np.zeros((100, 100, 3), dtype=np.uint8)
    result = _YoloResult(
        frame,
        [_YoloBox([10, 10, 30, 30], 1, 0.91)],
        {1: "hole"},
    )

    rendered = _draw_scaled_yolo_result(frame, result, (100, 100))

    assert tuple(rendered[10, 10]) == (0, 0, 255)


def test_defect_log_contains_severity_confidence_and_advice():
    log = _build_defect_log(
        profile={
            "type": "破洞",
            "severity": "重度",
            "severity_code": 3,
            "advice": "建议立即检修",
        },
        confidence=0.876,
        mileage=2.345,
        timestamp="2026-07-18 10:30:00",
        image_url="/images/test.jpg",
    )

    assert log == {
        "type": "破洞",
        "severity": "重度",
        "severity_code": 3,
        "confidence": 0.88,
        "advice": "建议立即检修",
        "mileage": 2.35,
        "time": "2026-07-18 10:30:00",
        "image": "/images/test.jpg",
    }


def test_csv_contains_severity_fields(tmp_path):
    csv_path = tmp_path / "report.csv"
    log = {
        "time": "2026-07-18 10:30:00",
        "mileage": 2.35,
        "type": "破洞",
        "severity": "重度",
        "confidence": 0.88,
        "advice": "建议立即检修",
    }

    _append_defect_csv(str(csv_path), log, "defect.jpg")

    with csv_path.open(encoding="utf-8-sig", newline="") as handle:
        row = next(csv.DictReader(handle))
    assert row["严重等级"] == "重度"
    assert row["识别置信度"] == "0.88"
    assert row["处置建议"] == "建议立即检修"
