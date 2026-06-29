#import "../template.typ": *
#set-appendix(3, "Configuration Reference")

= Configuration Reference

The options in #cfg-key[koncepcja.cfg] (Chapter 4), by section. Any of these can
also be set for one run with #cmd[-O section.key=value].

== [system]

#table(
  columns: (auto, 1fr),
  stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Key*], [*Meaning*],
  [#cfg-key[model]], [0 = 464, 1 = 664, 2 = 6128, 3 = 6128+],
  [#cfg-key[ram_size]], [RAM in KB (up to 4096 with expansion)],
  [#cfg-key[speed]], [Clock speed in MHz],
)

== [video]

#table(
  columns: (auto, 1fr),
  stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Key*], [*Meaning*],
  [#cfg-key[scr_scale]], [Window scale factor],
  [#cfg-key[scr_style]], [Rendering style (0--11)],
  [#cfg-key[vsync]], [1 = VSYNC on (default), 0 = off],
)

== [sound]

#table(
  columns: (auto, 1fr),
  stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Key*], [*Meaning*],
  [#cfg-key[snd_enabled]], [Enable sound],
  [#cfg-key[snd_playback_rate]], [0 = 11025, 1 = 22050, 2 = 44100, 3 = 48000, 4 = 96000 Hz],
)

== [peripheral]

#table(
  columns: (auto, 1fr),
  stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Key*], [*Meaning*],
  [#cfg-key[m4_http_port]], [M4 Board web-server port (default 8080)],
  [#cfg-key[m4_bind_ip]], [M4 bind address],
  [#cfg-key[symbiface]], [Enable the Symbiface II board],
)

== [input]

#table(
  columns: (auto, 1fr),
  stroke: 0.4pt + rule-grey, inset: 4.5pt,
  [*Key*], [*Meaning*],
  [#cfg-key[amx_mouse]], [Enable the AMX mouse],
)
