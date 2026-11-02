# Dashboard Data Polling Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make dashboard data polling configurable, non-overlapping, slower while the browser tab is hidden, and avoid repeated text writes when values are unchanged.

**Architecture:** Replace the fixed `setInterval(fetchData, 100)` with a single self-scheduling timeout. A polling coordinator owns the foreground/background intervals and in-flight state; `fetchData()` returns its promise so the coordinator schedules the next request only after completion.

**Tech Stack:** Browser JavaScript, pytest static frontend regression tests.

## Global Constraints

- Foreground polling defaults to `200` milliseconds.
- Hidden-tab polling defaults to `1000` milliseconds.
- Do not modify video streaming, PID/GIS-specific throttling, Three.js rendering, or YOLO behavior.
- Preserve the user's existing uncommitted Three.js changes in `dashboard/app.js`.
- Do not create a commit because the working tree already contains user-owned changes.

---

### Task 1: Configurable non-overlapping polling

**Files:**
- Modify: `dashboard/app.js`
- Create: `tests/test_dashboard_data_polling.py`

**Interfaces:**
- Consumes: `fetchData() -> Promise`
- Produces: `DATA_POLL_INTERVAL_MS`, `BACKGROUND_DATA_POLL_INTERVAL_MS`, `scheduleNextDataPoll(delayMs)`, `runDataPoll()`, `setTextIfChanged(element, value)`

- [x] **Step 1: Write failing regression tests**

Add static assertions that require both interval constants, forbid `setInterval(fetchData, ...)`, require in-flight protection and visibility-aware scheduling, and require guarded text updates.

- [x] **Step 2: Run tests to verify RED**

Run: `python -m pytest tests/test_dashboard_data_polling.py -v`

Expected: failures because the configurable self-scheduling poller and guarded text helper do not exist.

- [x] **Step 3: Implement the polling coordinator**

Add foreground/background constants, one timeout ID, and an in-flight flag. Schedule the next poll in `finally` after `await fetchData()`, and immediately refresh when the page returns to the foreground.

- [x] **Step 4: Guard repeated DOM text writes**

Add `setTextIfChanged(element, value)` and use it for values refreshed by `/api/data`. Cache the status badge mode and avoid rewriting its styles and text when the mode has not changed.

- [x] **Step 5: Run targeted and full tests**

Run:

```powershell
python -m pytest tests/test_dashboard_data_polling.py -v
python -m pytest -q
```

Expected: all polling tests pass and the existing suite reports no regressions.
