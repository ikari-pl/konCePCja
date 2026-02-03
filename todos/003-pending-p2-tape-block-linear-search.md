---
status: pending
priority: p2
issue_id: "003"
tags: [code-review, performance, imgui-branch]
dependencies: []
---

# O(n) Linear Search Every Frame in Tape Block Update

## Problem Statement

The tape block index update performs a linear search through all tape block offsets every frame, even when the tape position hasn't changed.

**Why it matters:** Large TZX files with hundreds of blocks will cause noticeable per-frame overhead. This is unnecessary work when the tape isn't advancing.

## Findings

**Location:** `src/imgui_ui.cpp` lines 483-495

```cpp
// Update current block index from pbTapeBlock pointer
if (tape_loaded && !imgui_state.tape_block_offsets.empty()) {
  for (int i = 0; i < (int)imgui_state.tape_block_offsets.size(); i++) {
    if (imgui_state.tape_block_offsets[i] == pbTapeBlock) {
      imgui_state.tape_current_block = i;
      break;
    }
    // pbTapeBlock may be past last known offset (between blocks)
    if (imgui_state.tape_block_offsets[i] > pbTapeBlock) {
      imgui_state.tape_current_block = i > 0 ? i - 1 : 0;
      break;
    }
  }
}
```

**Impact:** O(n) where n = number of tape blocks. Typical tapes have 20-100+ blocks, executed 50 times per second.

## Proposed Solutions

### Option A: Skip Search When Pointer Unchanged (Recommended)
**Pros:** Minimal change, eliminates 99% of searches
**Cons:** Adds one pointer comparison per frame
**Effort:** Small
**Risk:** Low

```cpp
static byte* last_pbTapeBlock = nullptr;
if (pbTapeBlock != last_pbTapeBlock) {
  last_pbTapeBlock = pbTapeBlock;
  // ... existing search logic
}
```

### Option B: Use Binary Search
**Pros:** O(log n) instead of O(n)
**Cons:** More complex, still runs every frame
**Effort:** Small
**Risk:** Low

```cpp
auto it = std::lower_bound(offsets.begin(), offsets.end(), pbTapeBlock);
```

### Option C: Maintain Index in Tape Subsystem
**Pros:** Index always correct, no search needed
**Cons:** Requires modifying tape.cpp
**Effort:** Medium
**Risk:** Medium

## Recommended Action

Option A - Skip when unchanged. Simple and effective.

## Technical Details

**Affected files:** `src/imgui_ui.cpp`
**Lines:** 483-495

## Acceptance Criteria

- [ ] No linear search when tape position hasn't changed
- [ ] Block index still updates correctly during playback
- [ ] Block navigation (prev/next buttons) still works

## Work Log

| Date | Action | Result |
|------|--------|--------|
| 2026-02-03 | Created from code review | Pending triage |

## Resources

- Performance analysis from performance-oracle agent
