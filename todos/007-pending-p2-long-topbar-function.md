---
status: pending
priority: p2
issue_id: "007"
tags: [code-review, patterns, architecture, imgui-branch]
dependencies: []
---

# Long Function: imgui_render_topbar() is 528 Lines

## Problem Statement

The `imgui_render_topbar()` function spans 528 lines (298-826), handling multiple unrelated responsibilities. This makes the code hard to navigate, test, and maintain.

**Why it matters:** Functions over 100 lines are a code smell. This function handles 7 distinct UI sections.

## Findings

**Location:** `src/imgui_ui.cpp` lines 298-826

**Responsibilities currently bundled:**
1. Top bar window setup and menu button
2. Drive A LED with click handling and eject popup
3. Drive B LED with click handling and eject popup
4. Tape transport UI (label, filename, play/stop/prev/next/eject)
5. Tape block counter
6. Waveform oscilloscope (4 different rendering modes)
7. FPS display

Each of these could be a separate function.

## Proposed Solutions

### Option A: Extract Helper Functions (Recommended)
**Pros:** Clearer structure, easier to test, single responsibility
**Cons:** More functions to navigate
**Effort:** Medium
**Risk:** Low

```cpp
static void render_topbar_menu_button();
static void render_drive_led(int drive, bool active, t_drive& drv, const std::string& file);
static void render_tape_transport();
static void render_tape_waveform();
```

### Option B: Use Lambdas Within Function
**Pros:** Keeps code together, no new functions
**Cons:** Still a long function, just better organized
**Effort:** Small
**Risk:** Low

### Option C: Split into Separate Files
**Pros:** Better build times, clear separation
**Cons:** More files to manage
**Effort:** Large
**Risk:** Medium

## Recommended Action

Option A - Extract helper functions. Good balance of clarity and effort.

## Technical Details

**Affected files:** `src/imgui_ui.cpp`
**Lines:** 298-826
**Estimated new functions:** 4-5

## Acceptance Criteria

- [ ] imgui_render_topbar() under 100 lines
- [ ] Each extracted function has single responsibility
- [ ] No change in visual appearance or behavior

## Work Log

| Date | Action | Result |
|------|--------|--------|
| 2026-02-03 | Created from code review | Pending triage |

## Resources

- Pattern analysis from pattern-recognition-specialist agent
- Architecture review from architecture-strategist agent
