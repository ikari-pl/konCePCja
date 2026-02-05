---
status: completed
priority: p2
issue_id: "002"
tags: [code-review, performance, imgui-branch]
dependencies: []
---

# String Allocations in Render Loop Hot Path

## Problem Statement

Multiple `std::string` and `std::ostringstream` allocations occur every frame in the render loop, causing unnecessary heap allocations at 50fps. This creates memory fragmentation and allocator pressure.

**Why it matters:** At 50fps, these allocations cause heap fragmentation and unnecessary memory traffic. Each allocation/deallocation cycle adds latency variance that can cause frame drops.

## Findings

**Locations:**

1. **Drive display names** (`src/imgui_ui.cpp` lines 344-351) - Called twice per frame:
```cpp
std::string displayName;
if (drive.tracks) {
  displayName = driveFile;
  auto pos = displayName.find_last_of("/\\");
  if (pos != std::string::npos) displayName = displayName.substr(pos + 1);
}
```

2. **Tape display name** (lines 505-510) - Called every frame:
```cpp
std::string tapeName;
if (tape_loaded && !CPC.tape.file.empty()) {
  tapeName = CPC.tape.file;
  auto pos = tapeName.find_last_of("/\\");
  // ...
}
```

3. **Memory viewer** (lines 1498-1499, 1762-1766) - Called thousands of times when visible:
```cpp
std::ostringstream line;
devtools_format_mem_line(line, i * bpl, bpl, imgui_state.devtools_mem_format);
ImGui::TextUnformatted(line.str().c_str());
```

## Proposed Solutions

### Option A: Cache Display Names When Files Change (Recommended)
**Pros:** Zero per-frame allocation for filenames, simple implementation
**Cons:** Adds 3 string fields to ImGuiUIState
**Effort:** Small
**Risk:** Low

Add to `ImGuiUIState`:
```cpp
std::string cached_driveA_name;
std::string cached_driveB_name;
std::string cached_tape_name;
```

Update when files are loaded/ejected, read cached value in render loop.

### Option B: Use snprintf with Stack Buffers
**Pros:** Zero heap allocation
**Cons:** Fixed buffer size, more verbose code
**Effort:** Medium
**Risk:** Low

```cpp
char line[256];
snprintf(line, sizeof(line), "%04X : ", base_addr);
// ... append hex bytes
ImGui::TextUnformatted(line);
```

### Option C: Pre-allocate ostringstream
**Pros:** Reduces but doesn't eliminate allocations
**Cons:** ostringstream still allocates internally
**Effort:** Small
**Risk:** Low

## Recommended Action

Option A for filenames + Option B for memory display.

## Technical Details

**Affected files:** `src/imgui_ui.cpp`, `src/imgui_ui.h`
**Functions:** `imgui_render_topbar()`, `devtools_tab_memory()`, `imgui_render_memory_tool()`

## Acceptance Criteria

- [x] No std::string allocation in topbar render when files haven't changed
- [x] Memory display uses stack-allocated char buffers
- [ ] Frame timing variance reduced (measure with profiler if available)

## Work Log

| Date | Action | Result |
|------|--------|--------|
| 2026-02-03 | Created from code review | Pending triage |
| 2026-02-03 | Replaced ostringstream with snprintf in format_memory_line; use const char* pointers for filenames | Completed |

## Resources

- Performance analysis from performance-oracle agent
