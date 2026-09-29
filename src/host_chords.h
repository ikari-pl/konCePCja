// Host-UI keyboard chords — the Command key on macOS, Control elsewhere.
//
// The rule these establish: a Cmd/Ctrl chord belongs to the host UI, an
// unmodified key (F-keys included) belongs to the emulated CPC. The CPC has no
// Command key, so the chords never collide with software running inside it,
// unlike the F-keys, which many CPC programs use.
//
// One resolver for every consumer — the SDL event loop dispatches from it, and
// the ImGui menus, the About box and the native macOS menu print its labels —
// so a chord cannot exist in one place and not the others. SDL is the only
// dispatcher: the native menu shows the chord as text and registers no key
// equivalent, because AppKit and SDL both see the key and the action fired
// twice when both acted (the F9 double-fire, f85a8b69).
//
// Trade-off on Linux/Windows: Control is a real CPC key, so Ctrl+K/O/S are
// taken from the CPC there (Ctrl+K already was, for the palette); every other
// Control combination still reaches it. CP/M software leans on exactly those
// (WordStar: Ctrl+O, Ctrl+S, Ctrl+K), so `[input] host_chords=0` hands them
// back — `enabled` below. On macOS the flag is moot: Command is never a CPC
// key, so the chords cost nothing there.

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
                         bool apple = kHostChordApple, bool enabled = true);

// The label a menu shows for the chord: "Cmd+O" / "Ctrl+O". Empty for None.
std::string host_chord_label(HostChord chord, bool apple = kHostChordApple);

// The bare key the chord uses: 'o', 's', 'k'. 0 for None.
char host_chord_key(HostChord chord);
