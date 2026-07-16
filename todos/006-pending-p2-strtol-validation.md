---
status: completed
priority: p2
issue_id: "006"
tags: [code-review, security, imgui-branch]
dependencies: ["005"]
---

# Integer Overflow in strtol Without Full Validation

## Problem Statement

The code uses `strtol()` to parse hexadecimal addresses from user input but stores results in unsigned or narrower types without checking for overflow or error conditions.

**Why it matters:** While the 8-character input buffer limits practical risk, proper error handling is a security best practice and prevents unexpected behavior.

## Findings

**Locations:** Lines 1399, 1448-1449, 1463, 1667-1668, 1682, 1705

**Example vulnerable code:**
```cpp
unsigned int addr = strtol(imgui_state.devtools_poke_addr, nullptr, 16);
int val = strtol(imgui_state.devtools_poke_val, nullptr, 16);
if (addr < 65536 && val >= 0 && val <= 255) {
  pbRAM[addr] = static_cast<byte>(val);
}
```

**Issues:**
1. `strtol` returns `long`, stored in `unsigned int` - potential overflow
2. `nullptr` second argument means parse errors cannot be detected
3. `errno` not checked for overflow conditions

## Proposed Solutions

### Option A: Full strtol Error Checking (Recommended)
**Pros:** Proper error handling, detects all edge cases
**Cons:** More verbose
**Effort:** Small
**Risk:** Low

```cpp
char* end;
errno = 0;
long parsed = strtol(imgui_state.devtools_poke_addr, &end, 16);
if (errno != 0 || *end != '\0' || parsed < 0 || parsed > 0xFFFF) {
  // Invalid input - could show error or just ignore
  return;
}
unsigned int addr = static_cast<unsigned int>(parsed);
```

### Option B: Use strtoul for Unsigned Values
**Pros:** Semantically correct for addresses
**Cons:** Still needs error checking
**Effort:** Small
**Risk:** Low

## Recommended Action

Option A - Add proper error checking. Do this when consolidating poke UI (issue 005).

## Technical Details

**Affected files:** `src/imgui_ui.cpp`
**Lines:** 1399, 1448-1449, 1463, 1667-1668, 1682, 1705

## Acceptance Criteria

- [x] All strtol calls check endptr for parse errors
- [ ] errno checked for overflow (not needed - max_val check is sufficient)
- [x] Invalid input handled gracefully (no crash, no unexpected behavior)

## Work Log

| Date | Action | Result |
|------|--------|--------|
| 2026-02-03 | Created from code review | Pending triage |
| 2026-02-03 | Created parse_hex() helper with endptr and max_val checking; replaced all strtol calls | Completed |

## Resources

- Security audit from security-sentinel agent
