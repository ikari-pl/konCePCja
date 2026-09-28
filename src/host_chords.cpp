#include "host_chords.h"

namespace {
int lower(int keycode) {
  return (keycode >= 'A' && keycode <= 'Z') ? keycode + ('a' - 'A') : keycode;
}
}  // namespace

HostChord host_chord_for(int keycode, bool ctrl, bool cmd, bool apple) {
  bool const modifier = apple ? cmd : ctrl;
  if (!modifier) return HostChord::None;
  switch (lower(keycode)) {
    case 'k':
      return HostChord::CommandPalette;
    case 'o':
      return HostChord::OpenDisk;
    case 's':
      return HostChord::SaveSnapshot;
    default:
      return HostChord::None;
  }
}

char host_chord_key(HostChord chord) {
  switch (chord) {
    case HostChord::CommandPalette:
      return 'k';
    case HostChord::OpenDisk:
      return 'o';
    case HostChord::SaveSnapshot:
      return 's';
    case HostChord::None:
      break;
  }
  return 0;
}

std::string host_chord_label(HostChord chord, bool apple) {
  char const key = host_chord_key(chord);
  if (key == 0) return "";
  std::string label = apple ? "Cmd+" : "Ctrl+";
  label += static_cast<char>(key - ('a' - 'A'));
  return label;
}
