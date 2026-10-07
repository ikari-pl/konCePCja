#pragma once

#include <cstdint>

// One emulated frame plus every per-frame duty that follows it (keyboard
// snapshot, stats, the machine frame, breakpoint/step handling, recorders,
// session, autotype, telnet, IPC frame step...). Both emulation loops call it:
// the GUI's Z80 thread and the -H main loop. Each duty exists once here; the
// loops keep only what differs — the GUI thread publishes the frame to the
// video ring and wakes the render thread, -H services its drains inline
// (beads-cv2.2).
enum class EmuFrameResult : std::uint8_t {
  kFrameComplete,  // a whole frame ran and its epilogue is done
  kStopped,        // a breakpoint/watchpoint or a finished step stopped the
                   // machine mid-frame
  kBreakAbsorbed,  // an autotype KONCPC_WAITBREAK break was consumed; the
                   // machine keeps running
};

// With speed limited (or autotype active) the board's own 50 Hz deadline
// paces the frame, in both loops: -H runs at real time like the GUI, and
// limit_speed=0 runs either one unpaced (beads-gnx3).
EmuFrameResult emu_run_frame();

// Auto frameskip never skips more frames than this in a row, so a machine that
// can never keep up (Faithful tier, a slow host) still shows a picture.
inline constexpr unsigned kMaxConsecutiveSkips = 5;

// Auto frameskip: skip rendering this frame when the previous one completed
// paced but late (subcycle_bridge_frame_was_late), up to kMaxConsecutiveSkips
// in a row. Decided only at frame boundaries (prev_complete), never mid-frame.
inline bool frameskip_should_skip(bool prev_complete, bool limit,
                                  unsigned frameskip, bool prev_late,
                                  unsigned consecutive_skips) {
  return prev_complete && limit && frameskip != 0 && prev_late &&
         consecutive_skips < kMaxConsecutiveSkips;
}

// The stop half of emu_run_frame: classify an EC_BREAKPOINT exit from
// z80.breakpoint_reached / watchpoint_reached / step_in. Exposed for tests.
EmuFrameResult emu_handle_stop();
