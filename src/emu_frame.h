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

// bridge_paces: let the board's own 50 Hz deadline pace the frame when speed
// is limited. The GUI's Z80 thread passes true; -H passes false.
EmuFrameResult emu_run_frame(bool bridge_paces);

// The stop half of emu_run_frame: classify an EC_BREAKPOINT exit from
// z80.breakpoint_reached / watchpoint_reached / step_in. Exposed for tests.
EmuFrameResult emu_handle_stop();
