# IMU Software Zeroing, Sensor Timeout, and 10 Hz Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make browser-relative attitude wrap correctly, make recalibration capture the current attitude as a software baseline, extend IMU/sonar freshness to one second, and run automatic centering at 10 Hz.

**Architecture:** The browser owns the display-only attitude baseline and applies one shared `wrapAngle180` function to Roll, Pitch, and Yaw. Bare-metal safety continues to use raw IMU values, while timeout and controller-period constants are changed together so scheduling and `dt` remain consistent.

**Tech Stack:** Browser JavaScript, Python/pytest regression checks, C11 bare-metal modules, GCC host tests.

## Global Constraints

- Relative Roll, Pitch, and Yaw must remain in `[-180°, 180°]`.
- IMU and sonar freshness timeouts must both be 1 second using the existing 10 ms tick.
- Automatic centering must run at 10 Hz with `dt = 0.1 s`.
- Recalibration is software zeroing only; it must not trigger WIT accelerometer or magnetometer hardware calibration.
- Existing sensor validity ranges, front-stop threshold, PID values, and thruster mixing remain unchanged.

---

### Task 1: Browser attitude wrapping and click-time software zero

**Files:**
- Modify: `dashboard/app.js`
- Create: `tests/test_imu_relative_angle_zeroing.py`

**Interfaces:**
- Consumes: `/api/data` fields `roll`, `pitch`, and `yaw`.
- Produces: `wrapAngle180(angle) -> number`, `latestAngles`, and click-time `baseAngles`.

- [ ] **Step 1: Add failing frontend regression checks**

Create `tests/test_imu_relative_angle_zeroing.py` with checks that extract and execute the production `wrapAngle180` body using Node, require wrapped Roll/Pitch/Yaw differences, require `latestAngles` to be updated from `/api/data`, and require `reCalibrate()` to copy `latestAngles` into `baseAngles`.

- [ ] **Step 2: Run the focused test and verify RED**

Run:

```powershell
python -m pytest -q tests/test_imu_relative_angle_zeroing.py
```

Expected: failures because `wrapAngle180` and `latestAngles` do not exist and recalibration currently clears the baseline.

- [ ] **Step 3: Implement browser wrapping and software zero**

In `dashboard/app.js`:

```javascript
function wrapAngle180(angle) {
    return ((angle + 180) % 360 + 360) % 360 - 180;
}
```

Add:

```javascript
let latestAngles = { roll: null, pitch: null, yaw: null };
```

When finite values arrive, update `latestAngles`. Initialize `baseAngles` from `latestAngles` only when no baseline exists. Compute all relative angles using `wrapAngle180(current - base)`.

In `reCalibrate()`, copy finite `latestAngles` into a new `baseAngles` object before clearing browser GIS state and sending `CMD:CALIBRATE`.

- [ ] **Step 4: Run the focused frontend test and verify GREEN**

Run:

```powershell
python -m pytest -q tests/test_imu_relative_angle_zeroing.py
```

Expected: all tests pass, including `179 → -179 == 2` and `-179 → 179 == -2`.

---

### Task 2: One-second sensor freshness

**Files:**
- Modify: `../openamp_for_linux/tests/test_navigation_safety.c`
- Modify: `../openamp_for_linux/inc/navigation_safety.h`

**Interfaces:**
- Produces: `NAV_IMU_TIMEOUT_TICKS == 100U`, `NAV_SONAR_TIMEOUT_TICKS == 100U`.

- [ ] **Step 1: Add failing timeout assertions**

At the start of `main()` in `test_navigation_safety.c`, add:

```c
assert(NAV_IMU_TIMEOUT_TICKS == 100U);
assert(NAV_SONAR_TIMEOUT_TICKS == 100U);
```

- [ ] **Step 2: Run the safety host test and verify RED**

Run:

```powershell
python -m pytest -q tests/test_navigation_safety_host.py
```

Expected: failure because the current values are 20 and 30 ticks.

- [ ] **Step 3: Change both timeout constants**

In `inc/navigation_safety.h`, set:

```c
#define NAV_IMU_TIMEOUT_TICKS   100U
#define NAV_SONAR_TIMEOUT_TICKS 100U
```

- [ ] **Step 4: Run the safety host test and verify GREEN**

Run:

```powershell
python -m pytest -q tests/test_navigation_safety_host.py
```

Expected: pass with all existing boundary behavior preserved.

---

### Task 3: Ten-hertz automatic centering

**Files:**
- Modify: `../openamp_for_linux/tests/test_pipe_center_control.c`
- Modify: `../openamp_for_linux/tests/test_manual_control_integration.py`
- Modify: `../openamp_for_linux/inc/pipe_center_control.h`
- Modify: `../openamp_for_linux/src/slaver_00_example.c`

**Interfaces:**
- Produces: `PIPE_CENTER_CONTROL_PERIOD_TICKS == 10U`, `PIPE_CENTER_CONTROL_DT_S == 0.1f`, and `ACK: 10HZ CENTER CONTROL STARTED`.

- [ ] **Step 1: Add failing rate assertions**

Add to `test_pipe_center_control.c`:

```c
assert(PIPE_CENTER_CONTROL_PERIOD_TICKS == 10U);
assert_close(PIPE_CENTER_CONTROL_DT_S, 0.1f);
```

Update the integration regression to require `ACK: 10HZ CENTER CONTROL STARTED` and to describe 10 Hz control.

- [ ] **Step 2: Run focused tests and verify RED**

Run:

```powershell
python -m pytest -q tests/test_pipe_center_control_host.py tests/test_manual_control_integration.py
```

Expected: failure because the production period is still 5 ticks, `dt` is `0.05f`, and the ACK still says 20 Hz.

- [ ] **Step 3: Implement the 10 Hz constants and ACK**

In `inc/pipe_center_control.h`:

```c
#define PIPE_CENTER_CONTROL_PERIOD_TICKS 10U
#define PIPE_CENTER_CONTROL_DT_S         0.1f
```

In `src/slaver_00_example.c`, change the successful motor-start acknowledgment to:

```c
"ACK: 10HZ CENTER CONTROL STARTED"
```

- [ ] **Step 4: Run focused tests and verify GREEN**

Run:

```powershell
python -m pytest -q tests/test_pipe_center_control_host.py tests/test_manual_control_integration.py
```

Expected: all focused tests pass.

---

### Task 4: Full verification

**Files:**
- Verify only; no new production files.

- [ ] **Step 1: Run PhytiumLinux regression tests**

```powershell
python -m pytest -q
```

Expected: all tests pass.

- [ ] **Step 2: Run openamp focused and full tests**

```powershell
python -m pytest -q tests/test_navigation_safety_host.py tests/test_pipe_center_control_host.py tests/test_manual_control_integration.py
python -m pytest -q
```

Expected: all tests pass with no failures.

- [ ] **Step 3: Inspect the final diff**

Confirm the diff contains only the approved browser angle, software-zero, one-second timeout, 10 Hz scheduling, ACK, and regression-test changes.
