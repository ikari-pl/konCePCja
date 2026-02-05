---
status: completed
priority: p3
issue_id: "010"
tags: [code-review, performance, imgui-branch]
dependencies: []
---

# Waveform Rendering Could Use AddPolyline

## Problem Statement

The waveform rendering uses multiple `AddLine()` calls in a loop (up to 128 iterations), which creates many draw commands.

**Why it matters:** Batching into a single `AddPolyline()` would reduce draw call overhead.

## Findings

**Location:** `src/imgui_ui.cpp` lines 665-782

**Current approach:**
```cpp
for (int i = 1; i < N; i++) {
  // Multiple AddLine calls per iteration for step waveform
  dl->AddLine(ImVec2(prevX, prevY), ImVec2(curX, prevY), wave_color, 1.0f);
  dl->AddLine(ImVec2(curX, prevY), ImVec2(curX, curY), wave_color, 1.0f);
}
```

**Estimated draw commands:** 2 * 128 = 256 per frame when visible

## Proposed Solutions

### Option A: Use AddPolyline (Recommended)
**Pros:** Single draw call, GPU-friendly
**Cons:** Need to pre-compute point array
**Effort:** Small
**Risk:** Low

```cpp
ImVec2 points[N * 2]; // For step waveform
int pointCount = 0;
for (int i = 0; i < N; i++) {
  points[pointCount++] = ImVec2(x, y1);
  points[pointCount++] = ImVec2(x, y2);
}
dl->AddPolyline(points, pointCount, wave_color, 0, 1.0f);
```

### Option B: Keep Current Approach
**Pros:** Simpler code
**Cons:** More draw calls
**Effort:** None
**Risk:** None

## Recommended Action

Option A - Use AddPolyline for cleaner rendering.

## Technical Details

**Affected files:** `src/imgui_ui.cpp`
**Lines:** 665-782

## Acceptance Criteria

- [x] Waveform rendered with AddPolyline
- [x] Visual appearance unchanged
- [x] Fewer ImGui draw commands

## Work Log

| Date | Action | Result |
|------|--------|--------|
| 2026-02-03 | Created from code review | Pending triage |
| 2026-02-03 | Refactored Pulse mode to use AddPolyline with pre-built point array | Completed |

## Resources

- Performance analysis from performance-oracle agent
