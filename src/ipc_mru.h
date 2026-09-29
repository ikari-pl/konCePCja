#pragma once

// konCePCja — Recent-list (MRU) entries from IPC loads (beads-00jf).
//
// A successful IPC `load` / `snapshot load` belongs on the Recent list just as
// a File-menu or drag-drop load does. CPC.mru_* is read by the menu every
// frame and pushing to it saves the config, so the IPC thread only stages the
// entry here and the main thread applies it from ipc_drain_input().

#include <string>
#include <vector>

#include "koncepcja.h"

// Which of t_CPC's Recent lists an entry goes on: &t_CPC::mru_disks, ...
using CpcMruList = std::vector<std::string> t_CPC::*;

// Any thread: queue path for `list`.
void ipc_mru_stage(CpcMruList list, const std::string& path);

// Main thread: move the queued entries onto CPC's lists. save_config=true
// also saves the config, as a File-menu load does; false updates the lists
// only (a headless run never rewrites the user's config). While the Options
// dialog is open nothing is applied: CPC then holds the dialog's uncommitted
// edits, which a save would persist and Cancel would roll the entry back
// with. The entries wait for the dialog to close.
void ipc_mru_apply_staged(bool save_config);
