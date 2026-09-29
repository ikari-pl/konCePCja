// konCePCja — quit policy: when may an exit raise the "unsaved disk" dialog?
//
// Pure decisions, no SDL, so the rules are unit-testable. The host shell
// (kon_cpc_ja.cpp) feeds in the live state.
#pragma once

#include <cstring>

// A quit reaches cleanExit() either from a user gesture (window close, F10,
// the native Quit item) or programmatically (IPC `quit`, SIGTERM/SIGINT,
// -E/--exit-after, -B/--exit-on-break). Only a gesture may block on a
// native modal: nobody is there to answer one raised by a script, and a
// test harness that terminates the emulator with a dirty disk would hang
// on it — or, worse, pop it onto the maintainer's desktop.
//
// KONCPC_NO_DIALOGS=1 in the environment suppresses the dialog for every
// path. Harnesses set it so no test can ever raise a modal.
inline bool koncpc_quit_should_prompt(bool headless, bool ask_if_unsaved,
                                      bool drive_altered,
                                      bool dialogs_suppressed) {
  return !headless && !dialogs_suppressed && ask_if_unsaved && drive_altered;
}

// KONCPC_NO_DIALOGS: unset or "0" keeps dialogs; anything else suppresses.
inline bool koncpc_dialogs_suppressed_by_env(const char* value) {
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}
