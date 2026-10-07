#pragma once

#include <cstdint>
#include <optional>

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

// One frame turn of the frame runner (the GUI's Z80 thread, or the -H main
// thread), and its half of the handshake with CpcPauseLease (beads-b0bj). A
// pauser stores g_emu_paused=true, then waits for g_z80_idle; the runner
// stores g_z80_idle=false, then re-reads g_emu_paused. All four accesses are
// seq_cst, so at least one side sees the other's store: either the runner sees
// the pause and stays idle (entered() is false: run nothing), or the pauser
// sees the runner busy and waits for the turn to end. Checking the pause
// before marking busy, as the Z80 thread used to, leaves a window where both
// go ahead.
//
// During an entered turn a lease taken on the runner's own thread (an autotype
// ~KONCPC_RESET~ handled in the frame epilogue) does not wait: the runner
// cannot race its own frame.
//
// idle_after: -H marks itself idle when the turn ends, since its main thread
// does other work between frames and may take a lease itself (an M4 HTTP
// reset). The GUI's Z80 thread stays busy until it next sees the pause, so a
// lease also excludes the video-ring publish that follows its frame.
class EmuFrameTurn {
 public:
  explicit EmuFrameTurn(bool idle_after);
  ~EmuFrameTurn();
  EmuFrameTurn(const EmuFrameTurn&) = delete;
  EmuFrameTurn& operator=(const EmuFrameTurn&) = delete;

  bool entered() const { return entered_; }

 private:
  bool idle_after_;
  bool entered_ = false;
};

// One turn of the -H main loop: a frame unless the machine is paused (nullopt:
// nothing ran, the caller sleeps). The turn holds g_z80_idle false, so a
// CpcPauseLease taken on another thread (IPC reset, snapshot, disk...) waits
// for it to end instead of mutating the machine under a running frame
// (beads-b0bj).
std::optional<EmuFrameResult> emu_headless_turn();

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
