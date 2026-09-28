// Host-UI keyboard chords — the Command key on macOS, Control elsewhere.
//
// The rule these establish: a Cmd/Ctrl chord belongs to the host UI, an
// unmodified key (F-keys included) belongs to the emulated CPC. The CPC has no
// Command key, so the chords never collide with software running inside it,
// unlike the F-keys, which many CPC programs use.
//
// One resolver for every consumer — the SDL event loop dispatches from it, the
// ImGui menus and the About box print its labels, and the native macOS menu
// gives the same items real key equivalents — so a chord cannot exist in one
// place and not the others.

#pragma once

#include <cstdint>
#include <string>

enum class HostChord : std::uint8_t {
  None,
  CommandPalette,  // Cmd+K
  OpenDisk,        // Cmd+O: Load Disk A...
  SaveSnapshot,    // Cmd+S: Save Snapshot...
};

#ifdef __APPLE__
constexpr bool kHostChordApple = true;
#else
constexpr bool kHostChordApple = false;
#endif

// `keycode` is the SDL keycode (letters arrive as lowercase ASCII; uppercase is
// accepted too). `ctrl`/`cmd` are the modifier states; which one counts is the
// platform's — `apple` is a parameter so tests can cover both.
HostChord host_chord_for(int keycode, bool ctrl, bool cmd,
                         bool apple = kHostChordApple);

// The label a menu shows for the chord: "Cmd+O" / "Ctrl+O". Empty for None.
std::string host_chord_label(HostChord chord, bool apple = kHostChordApple);

// The bare key the chord uses (for a native key equivalent): 'o', 's', 'k'.
// 0 for None.
char host_chord_key(HostChord chord);
