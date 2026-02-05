---
status: completed
priority: p2
issue_id: "004"
tags: [code-review, patterns, imgui-branch]
dependencies: []
---

# Memory Hex Dump Rendering Duplicated 3 Times

## Problem Statement

The hex dump rendering logic for displaying emulator RAM appears in three places with slight variations, violating DRY principle and making maintenance harder.

**Why it matters:** Bug fixes or format changes must be applied in 3 places. Easy to have them drift out of sync.

## Findings

**Location 1:** `devtools_format_mem_line()` (lines 1410-1433)
```cpp
static void devtools_format_mem_line(std::ostringstream& out, unsigned int base_addr,
                                     int bytes_per_line, int format)
{
  out << std::uppercase << std::setfill('0') << std::setw(4) << std::hex << base_addr << " : ";
  for (int j = 0; j < bytes_per_line; j++) {
    unsigned int addr = (base_addr + j) & 0xFFFF;
    out << std::setw(2) << static_cast<unsigned int>(pbRAM[addr]) << " ";
  }
  // Extended formats...
}
```

**Location 2:** `imgui_render_memory_tool()` filtering path (lines 1748-1753)

**Location 3:** `imgui_render_memory_tool()` fast path (lines 1762-1766)

All three produce nearly identical output with minor variations.

## Proposed Solutions

### Option A: Single format_memory_line() Function (Recommended)
**Pros:** Single point of truth, easy to maintain
**Cons:** Slight refactoring needed
**Effort:** Small
**Risk:** Low

Create one function that handles all cases:
```cpp
static void format_memory_line(char* out, size_t out_size, unsigned int base_addr,
                               int bytes_per_line, int format);
```

### Option B: Consolidate Memory Tool into DevTools
**Pros:** Eliminates entire duplicate feature
**Cons:** Larger change, removes standalone window
**Effort:** Medium
**Risk:** Medium

The Memory Tool is largely redundant with DevTools Memory tab.

## Recommended Action

Option A - Extract single function. Quick win.

## Technical Details

**Affected files:** `src/imgui_ui.cpp`
**Lines:** 1410-1433, 1748-1753, 1762-1766
**Estimated LOC saved:** ~45 lines

## Acceptance Criteria

- [x] Single function for memory line formatting
- [x] DevTools Memory tab uses shared function
- [x] Memory Tool uses shared function
- [x] All format options still work

## Work Log

| Date | Action | Result |
|------|--------|--------|
| 2026-02-03 | Created from code review | Pending triage |
| 2026-02-03 | Created format_memory_line() using snprintf; used by both DevTools and Memory Tool | Completed |

## Resources

- Pattern analysis from pattern-recognition-specialist agent
