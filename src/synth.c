/* synth.c — see synth.h. */
#include <math.h>
#include <string.h>
#include "synth.h"

enum { W_SINE, W_TRI, W_SAW, W_SQUARE, W_PULSE, W_EPIANO, W_ORGAN };
enum { D_NONE, D_KICK, D_SNARE, D_HAT, D_CYMBAL, D_TOM, D_CLAP, D_BLOCK };

/* One patch per GM family (program / 8): waveform, envelope in seconds,
 * sustain level, low-pass cutoff in Hz and a loudness trim. */
typedef struct { u8 wave; float att, dec, sus, rel, cutoff, gain; } Patch;
static const Patch FAMILY[16] = {
    { W_EPIANO, 0.002f, 1.20f, 0.00f, 0.30f, 3000, 0.9f },   /* piano */
    { W_SINE,   0.001f, 0.45f, 0.00f, 0.20f, 6000, 1.0f },   /* chromatic percussion */
    { W_ORGAN,  0.010f, 0.10f, 0.90f, 0.08f, 4000, 0.6f },   /* organ */
    { W_SAW,    0.002f, 0.60f, 0.10f, 0.15f, 2200, 0.7f },   /* guitar */
    { W_TRI,    0.004f, 0.50f, 0.55f, 0.08f, 1200, 1.3f },   /* bass */
    { W_SAW,    0.080f, 0.30f, 0.80f, 0.30f, 2500, 0.5f },   /* strings */
    { W_SAW,    0.100f, 0.30f, 0.80f, 0.40f, 2200, 0.5f },   /* ensemble */
    { W_SAW,    0.030f, 0.20f, 0.75f, 0.10f, 2800, 0.5f },   /* brass */
    { W_SQUARE, 0.020f, 0.20f, 0.75f, 0.08f, 2400, 0.45f },  /* reed */
    { W_TRI,    0.030f, 0.20f, 0.80f, 0.10f, 5000, 0.8f },   /* pipe */
    { W_SQUARE, 0.005f, 0.20f, 0.70f, 0.08f, 3500, 0.4f },   /* synth lead */
    { W_SAW,    0.200f, 0.50f, 0.70f, 0.50f, 1500, 0.5f },   /* synth pad */
    { W_TRI,    0.050f, 0.50f, 0.60f, 0.40f, 3000, 0.5f },   /* synth effects */
    { W_PULSE,  0.002f, 0.50f, 0.00f, 0.15f, 3000, 0.6f },   /* ethnic: kalimba, banjo, sitar */
    { W_SINE,   0.001f, 0.12f, 0.00f, 0.05f, 8000, 1.0f },   /* percussive: woodblock, steel drums */
    { W_SAW,    0.010f, 0.30f, 0.30f, 0.20f, 2000, 0.3f },   /* sound effects */
};

void synth_reset(Synth *s)
{
    memset(s->v, 0, sizeof s->v);
    for (int c = 0; c < 16; c++) { s->ch[c].vol = 100; s->ch[c].expr = 127; s->ch[c].sustain = 0; s->ch[c].bend = 0; }
}

void synth_init(Synth *s, int rate)
{
    memset(s, 0, sizeof *s);
    s->rate = rate; s->noise = 0x12345678u; s->master = 0.16f;
    synth_reset(s);
}

static float note_hz(float n) { return 440.0f * powf(2.0f, (n - 69.0f) / 12.0f); }

static SynVoice *alloc_voice(Synth *s)
{
    SynVoice *best = NULL;
    for (int i = 0; i < SYN_VOICES; i++) if (!s->v[i].on) return &s->v[i];
    for (int i = 0; i < SYN_VOICES; i++)             /* steal the oldest releasing voice, else the oldest */
        if (s->v[i].stage == 3 && (!best || s->v[i].age < best->age)) best = &s->v[i];
    if (!best) for (int i = 0; i < SYN_VOICES; i++) if (!best || s->v[i].age < best->age) best = &s->v[i];
    return best;
}

static void note_on(Synth *s, int ch, int note, int vel)
{
    SynVoice *v = alloc_voice(s);
    memset(v, 0, sizeof *v);
    v->on = 1; v->ch = (u8)ch; v->note = (u8)note; v->held = 1; v->age = ++s->clock;
    v->amp = vel / 127.0f;
    if (ch == 9) {                                   /* GM drum kit, key numbers per the GM percussion map */
        int d; float hz = 0, dec;
        switch (note) {
        case 35: case 36: d = D_KICK; hz = 150; dec = 0.30f; break;
        case 38: case 40: d = D_SNARE; hz = 190; dec = 0.18f; break;
        case 37: case 39: d = D_CLAP; dec = 0.12f; break;
        case 42: case 44: d = D_HAT; dec = 0.05f; break;
        case 46: d = D_HAT; dec = 0.30f; break;
        case 49: case 52: case 55: case 57: d = D_CYMBAL; dec = 1.20f; break;
        case 51: case 53: case 59: d = D_CYMBAL; dec = 0.60f; break;
        case 41: case 43: case 45: case 47: case 48: case 50:
            d = D_TOM; hz = 60.0f + (note - 41) * 14.0f; dec = 0.35f; break;
        case 75: case 76: case 77: case 56: d = D_BLOCK; hz = note == 56 ? 540 : 800 + (note - 75) * 150; dec = 0.06f; break;
        default: d = D_HAT; dec = 0.10f; break;
        }
        v->drum = (u8)d; v->dec = dec; v->env = 1; v->stage = 1;
        v->inc = hz / s->rate; v->sweep = hz;
        v->gain = d == D_KICK ? 1.6f : d == D_HAT ? 0.5f : d == D_CYMBAL ? 0.35f : 1.0f;
        return;
    }
    const Patch *p = &FAMILY[s->ch[ch].prog >> 3];
    v->wave = p->wave; v->att = p->att; v->dec = p->dec; v->sus = p->sus; v->rel = p->rel;
    v->cutoff = p->cutoff; v->gain = p->gain;
    v->bend_base = (float)note;
    v->inc = note_hz(note + s->ch[ch].bend) / s->rate;
}

static void note_off(Synth *s, int ch, int note)
{
    for (int i = 0; i < SYN_VOICES; i++) {
        SynVoice *v = &s->v[i];
        if (v->on && v->ch == ch && v->note == note && v->held) {
            v->held = 0;
            if (!v->drum && !s->ch[ch].sustain) v->stage = 3;
        }
    }
}

void synth_msg(Synth *s, u32 msg)
{
    int st = msg & 0xFF, d1 = (msg >> 8) & 0x7F, d2 = (msg >> 16) & 0x7F, ch = st & 15;
    switch (st & 0xF0) {
    case 0x80: note_off(s, ch, d1); break;
    case 0x90: if (d2) note_on(s, ch, d1, d2); else note_off(s, ch, d1); break;
    case 0xB0:
        if (d1 == 7) s->ch[ch].vol = (u8)d2;
        else if (d1 == 11) s->ch[ch].expr = (u8)d2;
        else if (d1 == 64) {
            s->ch[ch].sustain = d2 >= 64;
            if (!s->ch[ch].sustain)
                for (int i = 0; i < SYN_VOICES; i++)
                    if (s->v[i].on && s->v[i].ch == ch && !s->v[i].held && !s->v[i].drum) s->v[i].stage = 3;
        } else if (d1 == 121) { s->ch[ch].vol = 100; s->ch[ch].expr = 127; s->ch[ch].bend = 0; s->ch[ch].sustain = 0; }
        else if (d1 == 120 || d1 == 123)
            for (int i = 0; i < SYN_VOICES; i++) if (s->v[i].on && s->v[i].ch == ch) s->v[i].stage = 3;
        break;
    case 0xC0: s->ch[ch].prog = (u8)d1; break;
    case 0xE0:
        s->ch[ch].bend = (((d2 << 7) | d1) - 8192) / 8192.0f * 2.0f;
        for (int i = 0; i < SYN_VOICES; i++)
            if (s->v[i].on && s->v[i].ch == ch && !s->v[i].drum)
                s->v[i].inc = note_hz(s->v[i].bend_base + s->ch[ch].bend) / s->rate;
        break;
    }
}

static float noise(Synth *s)
{
    s->noise ^= s->noise << 13; s->noise ^= s->noise >> 17; s->noise ^= s->noise << 5;
    return (float)(int)s->noise / 2147483648.0f;
}

static float osc(int wave, float ph)
{
    switch (wave) {
    case W_SINE:   return sinf(ph * 6.2831853f);
    case W_TRI:    return ph < 0.5f ? 4 * ph - 1 : 3 - 4 * ph;
    case W_SAW:    return 2 * ph - 1;
    case W_SQUARE: return ph < 0.5f ? 0.7f : -0.7f;
    case W_PULSE:  return ph < 0.25f ? 0.7f : -0.3f;
    case W_EPIANO: return 0.8f * sinf(ph * 6.2831853f) + 0.25f * sinf(ph * 12.566371f);
    case W_ORGAN:  return 0.5f * sinf(ph * 6.2831853f) + 0.3f * sinf(ph * 12.566371f) + 0.2f * sinf(ph * 25.132741f);
    }
    return 0;
}

void synth_render(Synth *s, s16 *out, u32 n)
{
    float dt = 1.0f / s->rate;
    for (int i = 0; i < SYN_VOICES; i++) {
        SynVoice *v = &s->v[i];
        if (!v->on) continue;
        SynChannel *c = &s->ch[v->ch];
        float lvl = v->amp * v->gain * (c->vol / 127.0f) * (c->expr / 127.0f) * s->master * 32767.0f;
        float a = v->drum ? 0 : 1.0f - expf(-6.2831853f * v->cutoff * dt);
        for (u32 k = 0; k < n; k++) {
            float x;
            if (v->drum) {                           /* one-shot: exponential decay from 1 */
                v->env *= expf(-dt * 6.9f / v->dec);
                if (v->env < 0.001f) { v->on = 0; break; }
                float nz = noise(s);
                v->noise_lp += 0.25f * (nz - v->noise_lp);
                float hp = nz - v->noise_lp;         /* crude high-pass for hats and cymbals */
                switch (v->drum) {
                case D_KICK: case D_TOM:
                    v->sweep += ((v->drum == D_KICK ? 45.0f : v->sweep * 0.97f) - v->sweep) * 0.0006f;
                    v->phase += v->sweep * dt; if (v->phase >= 1) v->phase -= 1;
                    x = sinf(v->phase * 6.2831853f); break;
                case D_SNARE:
                    v->phase += v->inc; if (v->phase >= 1) v->phase -= 1;
                    x = 0.45f * sinf(v->phase * 6.2831853f) + 0.7f * nz; break;
                case D_CLAP: x = nz; break;
                case D_BLOCK:
                    v->phase += v->inc; if (v->phase >= 1) v->phase -= 1;
                    x = sinf(v->phase * 6.2831853f); break;
                default: x = hp; break;
                }
                x *= v->env;
            } else {
                switch (v->stage) {
                case 0: v->env += dt / (v->att > 0 ? v->att : dt); if (v->env >= 1) { v->env = 1; v->stage = 1; } break;
                case 1: v->env -= (1.0f - v->sus) * dt / v->dec;
                        if (v->env <= v->sus) { v->env = v->sus; v->stage = 2; }
                        if (v->env <= 0.0005f) { v->on = 0; } break;
                case 2: break;
                case 3: v->env -= dt / v->rel; if (v->env <= 0) v->on = 0; break;
                }
                if (!v->on) break;
                v->phase += v->inc; if (v->phase >= 1) v->phase -= 1;
                v->lp += a * (osc(v->wave, v->phase) - v->lp);
                x = v->lp * v->env;
            }
            int m = out[k] + (int)(x * lvl);
            out[k] = (s16)(m > 32767 ? 32767 : m < -32768 ? -32768 : m);
        }
    }
}
