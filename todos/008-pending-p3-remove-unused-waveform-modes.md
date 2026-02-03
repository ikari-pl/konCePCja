---
status: pending
priority: p3
issue_id: "008"
tags: [code-review, simplicity, imgui-branch]
dependencies: []
---

# Remove Unused Tape Waveform Modes 1 and 2

## Problem Statement

The tape waveform display has 4 modes, but modes 1 ("Bits") and 2 ("Sync") are either redundant or overengineered.

**Why it matters:** YAGNI - these modes add complexity without clear user benefit.

## Findings

**Location:** `src/imgui_ui.cpp` lines 665-782

**Current modes:**
- Mode 0 "Pulse" - Shows raw tape level transitions
- Mode 1 "Bits" - Nearly identical to Pulse, just different buffer
- Mode 2 "Sync" - Complex edge-locked baud-rate visualization
- Mode 3 "Decoded" - Shows actual decoded bits

**Issues:**
- Mode 1 is redundant with Mode 0 (same visualization, different data source)
- Mode 2 is a neat engineering demo but unlikely to be used by real users
- ~70 lines of code for questionable benefit

## Proposed Solutions

### Option A: Remove Modes 1 and 2 (Recommended)
**Pros:** Simpler UI, less code to maintain
**Cons:** Loses some diagnostic capability
**Effort:** Small
**Risk:** Low

Keep only:
- "Pulse" (raw tape level)
- "Decoded" (actual bits)

### Option B: Make Advanced Modes Hidden/Debug-Only
**Pros:** Keeps functionality for power users
**Cons:** Still have the code
**Effort:** Small
**Risk:** Low

### Option C: Keep All Modes
**Pros:** No change needed
**Cons:** YAGNI violation persists
**Effort:** None
**Risk:** None

## Recommended Action

Option A - Remove modes 1 and 2. Simplify.

## Technical Details

**Affected files:** `src/imgui_ui.cpp`
**Lines:** 665-782
**Estimated LOC saved:** ~70 lines

## Acceptance Criteria

- [ ] Only "Pulse" and "Decoded" modes available
- [ ] Mode selector updated
- [ ] State variable range reduced

## Work Log

| Date | Action | Result |
|------|--------|--------|
| 2026-02-03 | Created from code review | Pending triage |

## Resources

- Simplicity review from code-simplicity-reviewer agent
