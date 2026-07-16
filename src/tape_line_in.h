/* tape_line_in.h — live tape input: microphone/line-in → the sub-cycle deck.
 * HOST LAYER by design (docs/hardware/tape-device.md §4): SDL3 recording +
 * the Schmitt stage (hysteresis mirroring the mainboard's conditioning
 * circuit) live here; the machine only ever sees post-Schmitt levels via
 * feed_line_levels(). Channel select defaults LEFT — never mix by default:
 * out-of-phase stereo dubs cancel under summing (tape-remote-cable.md). */
#ifndef KONCPC_TAPE_LINE_IN_H
#define KONCPC_TAPE_LINE_IN_H

namespace subcycle {
class Machine;
}

/* Open the default recording device (stereo s16). Returns false when no
 * recording device is available. channel: 0 = left, 1 = right, 2 = mix. */
bool tape_line_in_start(int channel);
void tape_line_in_stop();
bool tape_line_in_active();

/* Z80-thread, once per frame: drain captured samples through the Schmitt
 * stage into the machine's rate-clocked line queue. */
void tape_line_in_pump(subcycle::Machine& machine);

/* --- Tape OUTPUT to the jack (the deck-recording / motor-REMOTE legs) ---
 * SAFETY (docs/hardware/tape-remote-cable.md §7): opt-in only, level ramps
 * from silence over ~2 s after arming, and ANY audio device change instantly
 * disarms — the dangerous moment is swapping the cable for headphones.
 * data_channel: 0 = left (motor carrier right), 1 = right (carrier left).
 * source_rdata: true = re-record what the deck plays; false = the CPC's own
 * SAVE (wdata). The motor leg is a ~19 kHz carrier while the relay is closed
 * (headphone jacks are AC-coupled — a DC level would not pass). */
bool tape_line_out_arm(subcycle::Machine& machine, int data_channel,
                       bool source_rdata);
void tape_line_out_disarm(subcycle::Machine& machine);
bool tape_line_out_active();
void tape_line_out_pump(subcycle::Machine& machine);

/* Output level for the tape data monitor (the auto-armed "screech"), 0..1.
 * Applied live in the pump (scales both the data square and the motor carrier);
 * persisted as [sound] tape_data_volume (percent). */
void tape_line_out_set_volume(float level);
float tape_line_out_volume();

#endif /* KONCPC_TAPE_LINE_IN_H */
