# YOLO English Class Annotations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Render the five YOLO classes with English severity labels and five distinct colors while preserving the existing Chinese defect logs and frontend data.

**Architecture:** Add an explicit five-class annotation table in `config.py` and a pure resolver in `core/vision.py`. The draw path uses this resolver instead of the Chinese business severity profile, while logging continues to use `resolve_defect_profile()`.

**Tech Stack:** Python 3, NumPy, OpenCV, pytest, ONNX metadata

## Global Constraints

- Model class order is exactly `spall`, `crack`, `hole`, `root`, `sediment`.
- Labels use `<class> | <severity> | <confidence>` and contain ASCII only.
- Severities are `Mild`, `Moderate`, `Severe`, with `Pending` for unknown classes.
- Colors use OpenCV BGR values: spall green `(0, 255, 0)`, crack yellow `(0, 255, 255)`, hole red `(0, 0, 255)`, root purple `(255, 0, 255)`, sediment blue `(255, 0, 0)`.
- Existing Chinese log, CSV, advice, and frontend formats remain unchanged.
- Preserve unrelated uncommitted stream-statistics changes in `config.py`, `core/vision.py`, dashboard files, and `tests/test_stream_stats.py`.

---

### Task 1: Five-Class Annotation Resolution

**Files:**
- Modify: `config.py:61-132`
- Modify: `core/vision.py:57-77`
- Create: `tests/test_yolo_annotation_style.py`

**Interfaces:**
- Consumes: ONNX class ID and optional metadata name.
- Produces: `_resolve_yolo_annotation(class_id, model_name=None) -> dict` with `name`, `severity`, and `bgr`.

- [ ] **Step 1: Write the failing resolver tests**

```python
from core.severity import resolve_defect_profile
from core.vision import _resolve_yolo_annotation


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


def test_spall_model_name_maps_to_existing_chinese_business_profile():
    profile = resolve_defect_profile(0, "spall")
    assert (
        profile["type"],
        profile["severity"],
        profile["severity_code"],
    ) == ("管壁脱落", "轻度", 1)
```

- [ ] **Step 2: Run resolver tests and verify RED**

Run:

```bash
pytest -q tests/test_yolo_annotation_style.py
```

Expected: collection fails because `_resolve_yolo_annotation` does not exist.

- [ ] **Step 3: Add model fallback and annotation configuration**

In `config.py`, update the fallback mapping and add the exact `spall` business alias:

```python
DEFECT_NAME_MAP = {
    0: "管壁脱落",
    1: "裂缝",
    2: "破洞",
    3: "树根侵入",
    4: "泥沙淤积",
}

DEFECT_TYPE_ALIASES = {
    "spall": "管壁脱落",
    # keep all existing aliases
}
```

After `DEFECT_SEVERITY_PROFILES`, add:

```python
YOLO_CLASS_NAMES = {
    0: "spall",
    1: "crack",
    2: "hole",
    3: "root",
    4: "sediment",
}

YOLO_ANNOTATION_STYLES = {
    "spall": {"severity": "Mild", "bgr": (0, 255, 0)},
    "crack": {"severity": "Moderate", "bgr": (0, 255, 255)},
    "hole": {"severity": "Severe", "bgr": (0, 0, 255)},
    "root": {"severity": "Severe", "bgr": (255, 0, 255)},
    "sediment": {"severity": "Moderate", "bgr": (255, 0, 0)},
}
```

- [ ] **Step 4: Implement the pure resolver**

Add to `core/vision.py` before `_draw_scaled_yolo_result()`:

```python
def _resolve_yolo_annotation(class_id, model_name=None):
    normalized_name = (
        str(model_name).strip().lower()
        if model_name is not None
        else ""
    )
    if normalized_name not in config.YOLO_ANNOTATION_STYLES:
        normalized_name = config.YOLO_CLASS_NAMES.get(int(class_id), "")

    style = config.YOLO_ANNOTATION_STYLES.get(normalized_name)
    if style is not None:
        return {
            "name": normalized_name,
            "severity": style["severity"],
            "bgr": tuple(style["bgr"]),
        }

    fallback_name = (
        str(model_name)
        if model_name is not None
        else str(class_id)
    )
    return {
        "name": fallback_name,
        "severity": "Pending",
        "bgr": (128, 128, 128),
    }
```

- [ ] **Step 5: Run resolver and business-mapping tests**

Run:

```bash
pytest -q tests/test_yolo_annotation_style.py tests/test_defect_severity.py
```

Expected: all tests pass, including correct `spall` business resolution.

### Task 2: English Labels and Per-Class Drawing Colors

**Files:**
- Modify: `core/vision.py:77-107`
- Modify: `tests/test_yolo_annotation_style.py`

**Interfaces:**
- Consumes: `_resolve_yolo_annotation(class_id, model_name)`.
- Produces: ASCII label `<name> | <severity> | <confidence>` and per-class OpenCV rectangle/text color.

- [ ] **Step 1: Write the failing draw-path test**

Append to `tests/test_yolo_annotation_style.py`:

```python
import cv2
import numpy as np

from core.vision import _YoloBox, _YoloResult, _draw_scaled_yolo_result


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
```

- [ ] **Step 2: Run the draw test and verify RED**

Run:

```bash
pytest -q tests/test_yolo_annotation_style.py::test_draw_uses_english_label_and_blue_sediment_color
```

Expected: FAIL because the current draw path uses the Chinese business profile.

- [ ] **Step 3: Use annotation style in the draw path**

Replace the display-only profile use in `_draw_scaled_yolo_result()`:

```python
annotation = _resolve_yolo_annotation(cls_id, model_name)
label = (
    f"{annotation['name']} | "
    f"{annotation['severity']} | "
    f"{conf:.2f}"
)
box_color = annotation["bgr"]
```

Keep `resolve_defect_profile()` unchanged in the defect logging path.

- [ ] **Step 4: Run focused tests**

Run:

```bash
pytest -q tests/test_yolo_annotation_style.py tests/test_defect_severity.py tests/test_yolo_frame_interval.py
```

Expected: all focused tests pass.

- [ ] **Step 5: Run full verification**

Run:

```bash
pytest -q
```

Expected: full test suite passes with zero failures.

- [ ] **Step 6: Inspect only task-owned diffs**

Run:

```bash
git diff -- config.py core/vision.py tests/test_yolo_annotation_style.py
```

Expected: only five-class fallback/annotation configuration, resolver, draw-path changes, and new tests; unrelated stream-statistics edits remain intact.
