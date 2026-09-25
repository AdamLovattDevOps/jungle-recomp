/* synth.h — a small General MIDI synthesiser for the type 4 music tracks.
 *
 * The game sent its music to the Windows MIDI mapper, so what players heard
 * depended on their sound card: often an OPL3 FM chip, sometimes a wavetable.
 * This stands in for that device. It is a plain subtractive synth with one
 * patch per GM family and synthesised drums, mono at the engine's 22050 Hz.
 */
#ifndef SYNTH_H
#define SYNTH_H
#include "core.h"

#define SYN_VOICES 32

typedef struct {
    u8    on, ch, note, held, drum, wave;
    float phase, inc, bend_base, amp, env, lp, hp, sweep, noise_lp;
    int   stage;                     /* 0 attack, 1 decay, 2 sustain, 3 release */
    float att, dec, sus, rel, cutoff, gain;
    u32   age;
} SynVoice;

typedef struct {
    u8    prog, vol, expr, sustain;
    float bend;                      /* semitones */
} SynChannel;

typedef struct {
    int        rate;
    SynChannel ch[16];
    SynVoice   v[SYN_VOICES];
    u32        clock, noise;
    float      master;
} Synth;

void synth_init(Synth *s, int rate);
void synth_reset(Synth *s);                      /* midiOutReset: all notes off, controllers default */
void synth_msg(Synth *s, u32 msg);               /* one midiOutShortMsg dword: status, data1, data2 */
void synth_render(Synth *s, s16 *out, u32 n);    /* mixed into out, saturating */

#endif
