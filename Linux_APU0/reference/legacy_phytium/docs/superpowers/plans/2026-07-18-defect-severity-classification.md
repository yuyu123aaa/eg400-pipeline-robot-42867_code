# Defect Severity Classification Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add fixed light, medium, and severe ratings for five pipeline defect classes across inference overlays, live logs, CSV reports, and the dashboard.

**Architecture:** Keep all class aliases and fixed severity profiles in `config.py`, and expose one pure resolver from `core/severity.py`. The vision pipeline resolves each model detection once, then uses the returned profile for the overlay and all persisted/API fields; the browser only renders those fields.

**Tech Stack:** Python 3, Flask, OpenCV, ONNX Runtime, vanilla JavaScript, pytest

## Global Constraints

- Severity is determined only by defect type.
- The fixed mapping is: 管壁脱落=轻度, 裂缝=中度, 泥沙淤积=中度, 破洞=重度, 树根侵入=重度.
- Confidence continues to control recording through the existing `DEFECT_CONFIDENCE` threshold and does not affect severity.
- Unknown classes resolve to 待定 with `severity_code=0` and must not stop the camera thread.
- The current ONNX model has only `root`, `hole`, and `crack`; English and Chinese aliases must work so a later five-class ONNX replacement needs no severity code changes.
- Do not modify model files, confidence thresholds, or log cooldown behavior.

---

### Task 1: Fixed Severity Resolver

**Files:**
- Create: `core/severity.py`
- Modify: `config.py`
- Test: `tests/test_defect_severity.py`

**Interfaces:**
- Consumes: `config.DEFECT_TYPE_ALIASES` and `config.DEFECT_SEVERITY_PROFILES`
- Produces: `resolve_defect_profile(class_id: int, model_name: object = None) -> dict`

- [ ] **Step 1: Write the failing resolver tests**

```python
from core.severity import resolve_defect_profile


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
        assert (profile["type"], profile["severity"], profile["severity_code"]) == values


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
```

- [ ] **Step 2: Run the resolver tests and verify RED**

Run: `python -m pytest tests/test_defect_severity.py -v`

Expected: collection fails because `core.severity` does not exist.

- [ ] **Step 3: Add configuration and the minimal resolver**

Add five severity profiles and aliases for the current `root/hole/crack` labels, the future `spalling/sediment` labels, and their Chinese names. Implement `resolve_defect_profile()` so it normalizes the model label, falls back to `DEFECT_NAME_MAP[class_id]`, returns a copy of a configured profile, and returns the safe pending profile for an unknown value.

- [ ] **Step 4: Run the resolver tests and verify GREEN**

Run: `python -m pytest tests/test_defect_severity.py -v`

Expected: both tests pass.

- [ ] **Step 5: Commit**

```bash
git add config.py core/severity.py tests/test_defect_severity.py
git commit -m "feat: add fixed defect severity resolver"
```

### Task 2: Vision Overlay and Defect Record Integration

**Files:**
- Modify: `core/vision.py`
- Modify: `tests/test_defect_severity.py`

**Interfaces:**
- Consumes: `resolve_defect_profile(class_id, model_name)`
- Produces: severity-colored `_draw_scaled_yolo_result()` output and `_build_defect_log(...) -> dict`

- [ ] **Step 1: Write failing vision behavior tests**

```python
import numpy as np
from core.vision import _YoloBox, _YoloResult, _build_defect_log, _draw_scaled_yolo_result


def test_overlay_uses_severity_color():
    frame = np.zeros((100, 100, 3), dtype=np.uint8)
    result = _YoloResult(frame, [_YoloBox([10, 10, 30, 30], 1, 0.91)], {1: "hole"})
    rendered = _draw_scaled_yolo_result(frame, result, (100, 100))
    assert tuple(rendered[10, 10]) == (0, 0, 255)


def test_defect_log_contains_severity_confidence_and_advice():
    log = _build_defect_log(
        profile={"type": "破洞", "severity": "重度", "severity_code": 3, "advice": "建议立即检修"},
        confidence=0.876,
        mileage=2.345,
        timestamp="2026-07-18 10:30:00",
        image_url="/images/test.jpg",
    )
    assert log["confidence"] == 0.88
    assert log["severity"] == "重度"
    assert log["severity_code"] == 3
    assert log["advice"] == "建议立即检修"
```

- [ ] **Step 2: Run the two vision tests and verify RED**

Run: `python -m pytest tests/test_defect_severity.py -v`

Expected: the overlay assertion fails with the old green color and `_build_defect_log` is missing.

- [ ] **Step 3: Implement profile-based overlay and record creation**

Resolve each box from `result.names`, draw the label as `缺陷名 | 等级 | 置信度`, and use the configured BGR color. Add `_build_defect_log()` and replace the inline dictionary in `_camera_loop()` with it.

- [ ] **Step 4: Run the tests and verify GREEN**

Run: `python -m pytest tests/test_defect_severity.py -v`

Expected: all resolver and vision tests pass.

- [ ] **Step 5: Commit**

```bash
git add core/vision.py tests/test_defect_severity.py
git commit -m "feat: apply severity to vision detections"
```

### Task 3: CSV Severity Fields

**Files:**
- Modify: `core/vision.py`
- Modify: `tests/test_defect_severity.py`

**Interfaces:**
- Consumes: the unified defect log from `_build_defect_log()`
- Produces: `_append_defect_csv(csv_path: str, defect_log: dict, image_filename: str) -> None`

- [ ] **Step 1: Write a failing CSV test**

```python
import csv


def test_csv_contains_severity_fields(tmp_path):
    from core.vision import _append_defect_csv
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
```

- [ ] **Step 2: Run the CSV test and verify RED**

Run: `python -m pytest tests/test_defect_severity.py::test_csv_contains_severity_fields -v`

Expected: import fails because `_append_defect_csv` does not exist.

- [ ] **Step 3: Extract and use the CSV writer**

Implement `_append_defect_csv()` with headers `记录时间`, `发生里程(m)`, `病害类型`, `严重等级`, `识别置信度`, `处置建议`, and `关联图片文件`. Replace the inline CSV block in `_camera_loop()` with this helper.

- [ ] **Step 4: Run the severity tests and verify GREEN**

Run: `python -m pytest tests/test_defect_severity.py -v`

Expected: all tests pass.

- [ ] **Step 5: Commit**

```bash
git add core/vision.py tests/test_defect_severity.py
git commit -m "feat: include severity in inspection csv"
```

### Task 4: Dashboard Severity Badge

**Files:**
- Modify: `dashboard/app.js`
- Create: `tests/test_defect_severity_frontend.py`

**Interfaces:**
- Consumes: `severity`, `severity_code`, `confidence`, and `advice` from `/api/defect_logs`
- Produces: a color-coded severity badge plus confidence and advice text on each live defect card

- [ ] **Step 1: Write a failing static frontend test**

```python
from pathlib import Path


APP_JS = Path("dashboard/app.js").read_text(encoding="utf-8")


def test_live_defect_card_renders_severity_fields():
    for token in ("log.severity", "log.severity_code", "log.confidence", "log.advice"):
        assert token in APP_JS
    assert "severityBadgeClass" in APP_JS
```

- [ ] **Step 2: Run the frontend test and verify RED**

Run: `python -m pytest tests/test_defect_severity_frontend.py -v`

Expected: assertions fail because the current card only renders type, mileage, and time.

- [ ] **Step 3: Add safe badge rendering**

Inside `fetchDefectLogs()`, derive `severityCode` with a zero fallback, map `1/2/3` to yellow/orange/red Tailwind classes, and render `log.severity || '待定'`, formatted confidence, and `log.advice || '建议人工复核'`.

- [ ] **Step 4: Run frontend and full regression tests**

Run: `python -m pytest tests/test_defect_severity_frontend.py -v`

Expected: pass.

Run: `python -m pytest -q`

Expected: full suite passes.

- [ ] **Step 5: Commit**

```bash
git add dashboard/app.js tests/test_defect_severity_frontend.py
git commit -m "feat: show defect severity on dashboard"
```
