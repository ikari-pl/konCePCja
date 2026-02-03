---
status: pending
priority: p2
issue_id: "005"
tags: [code-review, patterns, imgui-branch]
dependencies: []
---

# Poke Memory Input UI Duplicated Twice

## Problem Statement

The poke address/value input UI with parsing and validation appears identically in both DevTools Memory tab and the standalone Memory Tool.

**Why it matters:** 32 lines of identical code. Changes must be synchronized in two places.

## Findings

**DevTools Memory Tab** (lines 1438-1454):
```cpp
ImGui::SetNextItemWidth(50);
ImGui::InputText("Addr##dtpoke", imgui_state.devtools_poke_addr, ...);
ImGui::SameLine();
ImGui::SetNextItemWidth(40);
ImGui::InputText("Val##dtpoke", imgui_state.devtools_poke_val, ...);
ImGui::SameLine();
if (ImGui::Button("Poke##dt")) {
  if (imgui_state.devtools_poke_addr[0] && imgui_state.devtools_poke_val[0]) {
    unsigned int addr = strtol(imgui_state.devtools_poke_addr, nullptr, 16);
    int val = strtol(imgui_state.devtools_poke_val, nullptr, 16);
    if (addr < 65536 && val >= 0 && val <= 255) {
      pbRAM[addr] = static_cast<byte>(val);
    }
  }
}
```

**Memory Tool** (lines 1657-1673): Nearly identical code with different buffer names.

## Proposed Solutions

### Option A: Extract ui_poke_input() Helper (Recommended)
**Pros:** Eliminates duplication, single validation logic
**Cons:** Minor refactor
**Effort:** Small
**Risk:** Low

```cpp
static void ui_poke_input(char* addr_buf, size_t addr_size,
                          char* val_buf, size_t val_size,
                          const char* suffix) {
  // ... shared implementation
}
```

### Option B: Share State Buffers
**Pros:** Even less code
**Cons:** Both tools share same input state
**Effort:** Small
**Risk:** Low

Use same `mem_poke_addr`/`mem_poke_val` for both DevTools and Memory Tool.

## Recommended Action

Option A - Extract helper function. Cleaner separation.

## Technical Details

**Affected files:** `src/imgui_ui.cpp`
**Lines:** 1438-1454, 1657-1673
**Estimated LOC saved:** ~16 lines

## Acceptance Criteria

- [ ] Single function for poke input UI
- [ ] Both DevTools and Memory Tool use shared function
- [ ] Validation logic in one place

## Work Log

| Date | Action | Result |
|------|--------|--------|
| 2026-02-03 | Created from code review | Pending triage |

## Resources

- Pattern analysis from pattern-recognition-specialist agent
