---
status: completed
priority: p3
issue_id: "009"
tags: [code-review, simplicity, imgui-branch]
dependencies: []
---

# Remove Placeholder Character Set Tab

## Problem Statement

The DevTools "Char" tab is a placeholder that does nothing. Empty tabs confuse users.

**Why it matters:** UI should not show features that don't exist.

## Findings

**Location:** `src/imgui_ui.cpp` lines 1576-1583

```cpp
if (ImGui::BeginTabItem("Char")) {
  // Placeholder for character set visualization
  ImGui::Text("Character set visualization coming soon...");
  ImGui::EndTabItem();
}
```

## Proposed Solutions

### Option A: Remove the Tab (Recommended)
**Pros:** No placeholder UI
**Cons:** Loses the "coming soon" marker
**Effort:** Trivial
**Risk:** None

### Option B: Implement Character Set Display
**Pros:** Useful feature
**Cons:** Scope creep
**Effort:** Medium
**Risk:** Low

## Recommended Action

Option A - Remove until implemented.

## Technical Details

**Affected files:** `src/imgui_ui.cpp`
**Lines:** 1576-1583

## Acceptance Criteria

- [x] No "Char" tab in DevTools
- [x] Other tabs unaffected

## Work Log

| Date | Action | Result |
|------|--------|--------|
| 2026-02-03 | Created from code review | Pending triage |
| 2026-02-03 | Removed devtools_tab_char function and tab item | Completed |

## Resources

- Simplicity review from code-simplicity-reviewer agent
