---
status: completed
priority: p1
issue_id: "001"
tags: [code-review, security, imgui-branch]
dependencies: []
---

# TZX Tape Block Scanner Buffer Over-read Risk

## Problem Statement

The `tape_scan_blocks()` function in `src/imgui_ui.cpp` (lines 253-285) parses TZX tape image blocks using raw pointer arithmetic with `reinterpret_cast` to read multi-byte values. The function does not validate that sufficient bytes remain before reading block size fields, which can cause out-of-bounds memory reads when processing malformed tape images.

**Why it matters:** Users commonly load tape images from untrusted sources (game archives, internet downloads). A maliciously crafted TZX file could trigger memory corruption or crash the application.

## Findings

**Location:** `src/imgui_ui.cpp`, lines 253-285

**Vulnerable code pattern:**
```cpp
while (p < end) {
  imgui_state.tape_block_offsets.push_back(p);
  switch (*p) {
    case 0x10: p += *reinterpret_cast<word*>(p+0x01+0x02) + 0x04 + 1; break;
    case 0x11: p += (*reinterpret_cast<dword*>(p+0x01+0x0f) & 0x00ffffff) + 0x12 + 1; break;
    // ... more cases with similar patterns
    default:   p += *reinterpret_cast<dword*>(p+0x01) + 4 + 1; break;
  }
}
```

**Issues identified:**
1. No bounds checking before `reinterpret_cast<word*>(p+0x01+0x02)` - if `p+3` or `p+4` exceeds `end`, this reads out-of-bounds memory
2. The default case reads a dword at `p+0x01` without checking if 5 bytes are available
3. Malformed TZX files with corrupt block lengths could cause `p` to point beyond the buffer
4. Unaligned memory access via `reinterpret_cast` may cause undefined behavior on some architectures

## Proposed Solutions

### Option A: Add Bounds Checks Before Each Read (Recommended)
**Pros:** Minimal code change, clear intent, safe
**Cons:** Verbose, each case needs its own check
**Effort:** Small
**Risk:** Low

```cpp
// Before each reinterpret_cast, validate remaining bytes:
if (p + 5 > end) break; // For word read at p+3
word blockLen = *reinterpret_cast<word*>(p+0x01+0x02);
if (p + blockLen + 0x04 + 1 > end) break; // Validate total block size
p += blockLen + 0x04 + 1;
```

### Option B: Use Safe Helper Function
**Pros:** Cleaner code, reusable
**Cons:** More code to add
**Effort:** Medium
**Risk:** Low

```cpp
template<typename T>
static bool safe_read(byte* p, byte* end, size_t offset, T& out) {
  if (p + offset + sizeof(T) > end) return false;
  memcpy(&out, p + offset, sizeof(T)); // Safe for unaligned
  return true;
}
```

### Option C: Move Block Parsing to tape.cpp
**Pros:** DRY - reuse existing tape subsystem code
**Cons:** Larger refactor, needs tape API changes
**Effort:** Large
**Risk:** Medium

## Recommended Action

Option A - Add bounds checks inline. This is the minimal safe fix.

## Technical Details

**Affected files:** `src/imgui_ui.cpp`
**Affected functions:** `tape_scan_blocks()`
**Lines:** 253-285

## Acceptance Criteria

- [x] All `reinterpret_cast` reads are preceded by bounds validation
- [x] Malformed TZX with truncated block does not crash
- [x] Malformed TZX with oversized block length does not read past buffer
- [x] Normal TZX files still load correctly

## Work Log

| Date | Action | Result |
|------|--------|--------|
| 2026-02-03 | Created from code review | Pending triage |
| 2026-02-03 | Added safe_read_word/dword helpers; all block types have bounds checks | Completed |

## Resources

- Security audit report from security-sentinel agent
- TZX format specification: https://worldofspectrum.net/TZXformat.html
