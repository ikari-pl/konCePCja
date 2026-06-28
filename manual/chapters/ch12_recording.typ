#import "../template.typ": *
#set-chapter("Recording")

= Recording

#intro[
  konCePCja can capture what happens on the emulated machine in several formats:
  video, animated images, chiptune audio, and complete replayable sessions. This
  chapter covers each.
]

== AVI video recording

#idx("AVI recording")Record the CPC display and sound together to an `.avi` video
file --- useful for capturing gameplay or a demo with synchronised audio.

== GIF and frame dumps

#idx("GIF recording")For short clips and screenshots, konCePCja can save an
animated GIF (LZW-compressed) of the display, or a sequence of numbered PNG
frames. These are ideal for documentation and for sharing a moment of a game
without a full video file.

== YM audio recording

#idx("YM recording")Rather than recording sampled audio, the YM recorder captures
the actual register writes to the PSG sound chip and saves them as a `.ym`
chiptune file. The result is a compact, authentic record of the music that can be
replayed in YM players, exactly as the CPC produced it.

== Session recording and playback

#idx("session recording")A session recording captures the machine's input and
state over time so that an entire run can be replayed deterministically --- the
same inputs producing the same result every time. This is valuable for
demonstrations, for bug reports (a recording reproduces the problem exactly), and
as the basis for automated regression testing alongside the IPC interface
(Chapter 9).
