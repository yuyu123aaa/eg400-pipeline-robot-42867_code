# YOLO Startup Preload Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load the single YOLO ONNX Session before background services and Flask start, with flushed stage logs and fail-fast behavior.

**Architecture:** Reuse the existing `PRELOAD_YOLO` switch and `init_yolo()` singleton. Make preload enabled by default, stop startup when preload returns false, and expose Session-construction boundaries through flushed logs.

**Tech Stack:** Python 3.9, pytest, ONNX Runtime CPU Execution Provider

## Global Constraints

- Create exactly one ONNX Runtime Session and reuse it.
- Do not enable ONNX graph optimization in this change.
- Do not start Flask or background services after a preload failure.
- Preserve a configuration switch that can disable preload for diagnostics.

---

### Task 1: Startup preload behavior

**Files:**
- Modify: `config.py`
- Modify: `main.py`
- Test: `tests/test_startup_yolo_loading.py`

**Interfaces:**
- Consumes: `config.PRELOAD_YOLO: bool`, `init_yolo() -> bool`
- Produces: `start_background_services() -> bool`

- [ ] **Step 1: Write failing tests**

Add tests asserting that preload is enabled by default, occurs before every background service, returns `True` on success, returns `False` without starting services on failure, and can still be disabled.

- [ ] **Step 2: Verify the tests fail**

Run: `python -m pytest tests/test_startup_yolo_loading.py -q`

Expected: failures because preload currently defaults to false and startup ignores the `init_yolo()` result.

- [ ] **Step 3: Implement the startup behavior**

Set `PRELOAD_YOLO = True`. Return `False` immediately when `init_yolo()` fails; otherwise start services in the existing order and return `True`. Make `run_app()` skip Flask when startup returns false.

- [ ] **Step 4: Verify tests pass**

Run: `python -m pytest tests/test_startup_yolo_loading.py -q`

Expected: all tests pass.

### Task 2: Flushed ONNX initialization stage logs

**Files:**
- Modify: `core/vision.py`
- Test: `tests/test_onnx_runtime_stability.py`

**Interfaces:**
- Consumes: `_OnnxYoloModel(model_path)` and `init_yolo()`
- Produces: flushed markers before Session construction, after Session construction, and after full model initialization

- [ ] **Step 1: Write failing tests**

Capture `print()` calls and assert that every startup marker uses `flush=True`, includes the model path before constructing the Session, and records Session success separately from full initialization success.

- [ ] **Step 2: Verify the tests fail**

Run: `python -m pytest tests/test_onnx_runtime_stability.py -q`

Expected: failure because the current logs are not flushed and do not bracket Session construction.

- [ ] **Step 3: Implement minimal stage logging**

Add flushed prints immediately before and after `ort.InferenceSession(...)`, and make `init_yolo()` start/success/failure prints flush immediately.

- [ ] **Step 4: Run focused and full tests**

Run: `python -m pytest tests/test_startup_yolo_loading.py tests/test_onnx_runtime_stability.py -q`

Then run: `python -m pytest -q`

Expected: all tests pass.
