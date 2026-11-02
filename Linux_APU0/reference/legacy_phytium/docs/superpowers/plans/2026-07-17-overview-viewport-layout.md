# System Overview Viewport Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the desktop system overview fit its core monitoring and complete control panel into one viewport at both 1920×1080 and 2560×1440, while showing PID tuning and defect logs only on their dedicated pages.

**Architecture:** Keep the existing single-page sections and `switchView()` navigation. Add an explicit `view-overview` state class to the content container, hide PID/log sections in overview mode, and use a desktop-only CSS grid whose middle camera/GIS row consumes the remaining viewport height.

**Tech Stack:** HTML, CSS, vanilla JavaScript, ECharts, pytest source-contract tests.

## Global Constraints

- Modify only `dashboard/index.html`, `dashboard/style.css`, `dashboard/app.js`, and frontend-focused tests.
- Do not modify Python application code, communication code, sensor code, motor code, or API routes.
- At desktop widths above 1024px with a fine pointer, overview must fit in one viewport at 1920×1080 and preserve the same hierarchy at 2560×1440.
- PID tuning and defect logs must remain available only through their existing sidebar child pages.
- Existing tablet, mobile, and landscape touch layouts must retain scrolling and stacking behavior.
- When a viewport cannot safely contain the overview, vertical scrolling must remain available.

---

### Task 1: Add failing overview layout contracts

**Files:**
- Create: `tests/test_overview_viewport_layout.py`
- Read: `dashboard/app.js`
- Read: `dashboard/style.css`

**Interfaces:**
- Consumes: Existing `switchView(viewMode)` function and `.focus-hidden` visibility class.
- Produces: Source-level contracts for the `view-overview` state, overview-only section visibility, initial overview activation, and desktop viewport grid.

- [ ] **Step 1: Write the failing tests**

```python
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read_dashboard_file(name):
    return (ROOT / "dashboard" / name).read_text(encoding="utf-8")


def test_overview_hides_pid_and_logs_but_keeps_dedicated_views():
    js = read_dashboard_file("app.js")
    overview_branch = js.split("if (viewMode === 'overview') {", 1)[1].split(
        "} else if (viewMode === 'camera') {", 1
    )[0]

    assert "pidSection.classList.add('focus-hidden')" in overview_branch
    assert "logsSection.classList.add('focus-hidden')" in overview_branch
    assert "} else if (viewMode === 'pid') {" in js
    assert "} else if (viewMode === 'logs') {" in js


def test_overview_sets_explicit_view_state_and_is_applied_on_startup():
    js = read_dashboard_file("app.js")

    assert "contentInner.classList.toggle('view-overview', viewMode === 'overview')" in js
    assert "window.switchView('overview');" in js


def test_desktop_overview_uses_remaining_viewport_height():
    css = read_dashboard_file("style.css")

    assert "@media (min-width: 1025px) and (hover: hover) and (pointer: fine)" in css
    assert ".content-inner.view-overview {" in css
    assert "grid-template-rows: auto auto minmax(320px, 1fr) auto;" in css
    assert "height: 100%;" in css
    assert "min-height: 760px;" in css
    assert ".content-inner.view-overview .middle-row {" in css
    assert ".content-inner.view-overview .middle-card {" in css
```

- [ ] **Step 2: Run the new tests and verify they fail**

Run:

```powershell
pytest tests/test_overview_viewport_layout.py -v
```

Expected: three failures because `view-overview`, initial `switchView('overview')`, overview PID/log hiding, and the desktop grid rules do not exist yet.

- [ ] **Step 3: Commit the failing tests**

```powershell
git add tests/test_overview_viewport_layout.py
git commit -m "test: define desktop overview layout"
```

### Task 2: Apply explicit overview visibility state

**Files:**
- Modify: `dashboard/app.js:927-1033`
- Test: `tests/test_overview_viewport_layout.py`
- Test: `tests/test_frontend_asset_split.py`

**Interfaces:**
- Consumes: `switchView(viewMode)`, `section-pid`, `section-logs`, `.focus-hidden`, `resizeWorkSurfaces()`, and `myPidChart`.
- Produces: The `view-overview` class on `.content-inner`; overview mode with PID/log sections hidden; initial overview activation after navigation setup.

- [ ] **Step 1: Add the content state and overview visibility rules**

Inside `switchView()`, obtain the content container before view-specific branches and toggle the overview class:

```javascript
var contentInner = document.querySelector('.content-inner');
if (contentInner) {
    contentInner.classList.toggle('view-overview', viewMode === 'overview');
}
```

Extend the overview branch:

```javascript
if (viewMode === 'overview') {
    if (currently3D) toggleView();
    if (toggleBtn) toggleBtn.classList.remove('hidden');
    if (pidSection) pidSection.classList.add('focus-hidden');
    if (logsSection) logsSection.classList.add('focus-hidden');
    if (historySection) historySection.classList.add('focus-hidden');
```

Reuse the previously declared `contentInner` in the vertical-centering block instead of declaring it again.

- [ ] **Step 2: Refresh all work surfaces after a view change**

At the end of `switchView()`, preserve the existing GIS resize and add:

```javascript
resizeWorkSurfaces();
if (myPidChart && !myPidChart.isDisposed()) {
    myPidChart.resize();
}
```

- [ ] **Step 3: Apply the overview state on initial page load**

Immediately after the `window.switchView` function definition and before sidebar click listeners:

```javascript
window.switchView('overview');
```

- [ ] **Step 4: Run focused JavaScript source-contract tests**

Run:

```powershell
pytest tests/test_overview_viewport_layout.py tests/test_frontend_asset_split.py -v
```

Expected: the two JavaScript contract tests pass; the CSS layout test still fails.

- [ ] **Step 5: Commit the navigation state change**

```powershell
git add dashboard/app.js tests/test_overview_viewport_layout.py
git commit -m "fix: isolate overview-only dashboard content"
```

### Task 3: Make the desktop overview consume one viewport

**Files:**
- Modify: `dashboard/style.css`
- Test: `tests/test_overview_viewport_layout.py`
- Test: `tests/test_mobile_dashboard_layout.py`

**Interfaces:**
- Consumes: The `view-overview` class produced by Task 2 and existing `.middle-row`/`.middle-card` layout primitives.
- Produces: A desktop-only four-row overview grid with an elastic camera/GIS row and a 760px safe minimum that falls back to content-area scrolling.

- [ ] **Step 1: Add desktop-only viewport rules**

Add these rules before the existing landscape-touch media query:

```css
@media (min-width: 1025px) and (hover: hover) and (pointer: fine) {
    .main-content {
        height: 100vh;
        min-height: 0;
    }

    .content-area {
        min-height: 0;
    }

    .content-inner.view-overview {
        display: grid;
        grid-template-rows: auto auto minmax(320px, 1fr) auto;
        height: 100%;
        min-height: 760px;
    }

    .content-inner.view-overview .middle-row {
        min-height: 0;
        height: 100%;
    }

    .content-inner.view-overview .middle-card {
        min-height: 0;
        height: 100%;
    }
}
```

The content area already owns vertical scrolling. When the available browser viewport is shorter than the safe 760px content height, the content becomes taller than the viewport and scrolls instead of overlapping.

- [ ] **Step 2: Run overview and responsive layout tests**

Run:

```powershell
pytest tests/test_overview_viewport_layout.py tests/test_mobile_dashboard_layout.py -v
```

Expected: all tests pass, including existing tablet/mobile rules.

- [ ] **Step 3: Commit the desktop layout**

```powershell
git add dashboard/style.css tests/test_overview_viewport_layout.py
git commit -m "fix: fit desktop overview to viewport"
```

### Task 4: Verify the complete frontend change

**Files:**
- Verify: `dashboard/app.js`
- Verify: `dashboard/style.css`
- Verify: `dashboard/index.html`
- Verify: `tests/test_overview_viewport_layout.py`

**Interfaces:**
- Consumes: Completed overview visibility and viewport layout.
- Produces: Evidence that the frontend-only change preserves existing page contracts.

- [ ] **Step 1: Run the complete automated test suite**

Run:

```powershell
pytest -v
```

Expected: all existing and new tests pass.

- [ ] **Step 2: Confirm only allowed implementation files changed**

Run:

```powershell
git status --short
git diff -- dashboard/index.html dashboard/style.css dashboard/app.js tests/test_overview_viewport_layout.py
```

Expected: implementation changes are limited to `dashboard/app.js`, `dashboard/style.css`, and the frontend test file. Pre-existing `.gitignore`, `.claude`, and `.vscode` changes remain untouched.

- [ ] **Step 3: Perform viewport checks**

Start the existing application without changing backend code and inspect the overview at:

- 1920×1080, browser zoom 100%: all four overview rows and all ten control buttons are visible without vertical scrolling.
- 2560×1440, browser zoom 100%: the same four rows remain in the same order, while camera/GIS receives the extra height.
- PID sidebar page: PID controls and chart appear; defect logs are absent.
- Defect logs sidebar page: logs appear; PID controls are absent.
- Width 1024px or less: current stacked scrolling behavior remains.

- [ ] **Step 4: Record final verification**

Do not claim completion unless the automated suite passes and the implementation diff contains no backend, communication, sensor, or motor changes.
