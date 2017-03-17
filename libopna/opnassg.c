#include "opnassg.h"
/*
static const float voltable[32] = {
  0.0f,           0.0f,           0x1.ae89f9p-8f, 0x1.000000p-7f,
  0x1.306fe0p-7f, 0x1.6a09e6p-7f, 0x1.ae89f9p-7f, 0x1.000000p-6f,
  0x1.306fe0p-6f, 0x1.6a09e6p-6f, 0x1.ae89f9p-6f, 0x1.000000p-5f,
  0x1.306fe0p-5f, 0x1.6a09e6p-5f, 0x1.ae89f9p-5f, 0x1.000000p-4f,
  0x1.306fe0p-4f, 0x1.6a09e6p-4f, 0x1.ae89f9p-4f, 0x1.000000p-3f,
  0x1.306fe0p-3f, 0x1.6a09e6p-3f, 0x1.ae89f9p-3f, 0x1.000000p-2f,
  0x1.306fe0p-2f, 0x1.6a09e6p-2f, 0x1.ae89f9p-2f, 0x1.000000p-1f,
  0x1.306fe0p-1f, 0x1.6a09e6p-1f, 0x1.ae89f9p-1f, 0x1.000000p-0f
};
*/

// if (i < 2) voltable[i] = 0;
// else       voltable[i] = round((0x7fff / 3.0) * pow(2.0, (i - 31)/4.0));

static const int16_t voltable[32] = {
      0,     0,    72,    85,
    101,   121,   144,   171,
    203,   241,   287,   341,
    406,   483,   574,   683,
    812,   965,  1148,  1365,
   1624,  1931,  2296,  2731,
   3247,  3862,  4592,  5461,
   6494,  7723,  9185, 10922
};

#define SINCTABLEBIT 7
#define SINCTABLELEN (1<<SINCTABLEBIT)

// GNU Octave
// Fc = 7987200
// Ff = Fc/144
// Fs = Fc/32
// Fe = 20000
// O = (((Ff/2)-Fe)*2)/(Fs/2)
// B = 128 * O / 2
// FILTER=sinc(linspace(-127.5,127.5,256)*2/9/2).*rotdim(kaiser(256,B))
// FILTERI=round(FILTER(1:128).*32768)
static const int16_t sinctable[SINCTABLELEN] = {
      1,     0,    -1,    -2,    -3,    -5,    -6,    -6,
     -6,    -5,    -2,     2,     7,    11,    16,    19,
     20,    18,    13,     5,    -5,   -17,   -29,   -38,
    -44,   -45,   -40,   -29,   -11,    12,    36,    60,
     79,    90,    91,    80,    56,    21,   -22,   -68,
   -112,  -146,  -166,  -166,  -144,  -100,   -37,    39,
    119,   193,   251,   282,   280,   241,   166,    61,
    -64,  -195,  -315,  -406,  -455,  -450,  -385,  -264,
    -96,   101,   306,   491,   632,   705,   694,   593,
    405,   147,  -154,  -464,  -744,  -954, -1062, -1043,
   -889,  -607,  -220,   230,   692,  1108,  1421,  1580,
   1552,  1322,   902,   328,  -343, -1032, -1655, -2125,
  -2369, -2333, -1994, -1365,  -498,   523,  1585,  2557,
   3306,  3714,  3690,  3185,  2206,   815,  -868, -2673,
  -4391, -5798, -6670, -6809, -6067, -4359, -1681,  1886,
   6178, 10957, 15928, 20765, 25133, 28724, 31275, 32600,
};

void opna_ssg_reset(struct opna_ssg *ssg) {
  for (int i = 0; i < 3; i++) {
    ssg->ch[i].tone_counter = 0;
    ssg->ch[i].out = false;
  }
  for (int i = 0; i < 0x10; i++) {
    ssg->regs[i] = 0;
  }
  ssg->noise_counter = 0;
  ssg->lfsr = 0;
  ssg->env_counter = 0;
  ssg->env_level = 0;
  ssg->env_att = false;
  ssg->env_alt = false;
  ssg->env_hld = false;
  ssg->env_holding = false;
  ssg->mask = 0;
}

void opna_ssg_resampler_reset(struct opna_ssg_resampler *resampler) {
  for (int i = 0; i < SINCTABLELEN; i++) {
    resampler->buf[i] = 0;
  }
  resampler->index = 0;
}

void opna_ssg_writereg(struct opna_ssg *ssg, unsigned reg, unsigned val) {
  if (reg > 0xfu) return;
  val &= 0xff;
  ssg->regs[reg] = val;

  if (reg == 0xd) {
    ssg->env_att = ssg->regs[0xd] & 0x4;
    if (ssg->regs[0xd] & 0x8) {
      ssg->env_alt = ssg->regs[0xd] & 0x2;
      ssg->env_hld = ssg->regs[0xd] & 0x1;
    } else {
      ssg->env_alt = ssg->env_att;
      ssg->env_hld = true;
    }
    ssg->env_holding = false;
    ssg->env_level = 0;
    ssg->env_counter = 0;
  }
}

unsigned opna_ssg_readreg(const struct opna_ssg *ssg, unsigned reg) {
  if (reg > 0xfu) return 0xff;
  return ssg->regs[reg];
}

unsigned opna_ssg_tone_period(const struct opna_ssg *ssg, int ch) {
  if (ch < 0) return 0;
  if (ch >= 3) return 0;
  return ssg->regs[0+ch*2] | ((ssg->regs[1+ch*2] & 0xf) << 8);
}

static bool opna_ssg_chan_env(const struct opna_ssg *ssg, int chan) {
  return ssg->regs[0x8+chan] & 0x10;
}
static int opna_ssg_tone_volume(const struct opna_ssg *ssg, int chan) {
  return ssg->regs[0x8+chan] & 0xf;
}

static bool opna_ssg_tone_out(const struct opna_ssg *ssg, int chan) {
  unsigned reg = ssg->regs[0x7] >> chan;
  return (ssg->ch[chan].out || (reg & 0x1)) && ((ssg->lfsr & 1) || (reg & 0x8));
}

static bool opna_ssg_tone_silent(const struct opna_ssg *ssg, int chan) {
  unsigned reg = ssg->regs[0x7] >> chan;
  return (reg & 0x1) && (reg & 0x8);
}

static int opna_ssg_noise_period(const struct opna_ssg *ssg) {
  return ssg->regs[0x6] & 0x1f;
}

static int opna_ssg_env_period(const struct opna_ssg *ssg) {
  return (ssg->regs[0xc] << 8) | ssg->regs[0xb];
}

static int opna_ssg_env_level(const struct opna_ssg *ssg) {
  return ssg->env_att ? ssg->env_level : 31-ssg->env_level;
}

int opna_ssg_channel_level(const struct opna_ssg *ssg, int ch) {
  return opna_ssg_chan_env(ssg, ch)
       ? opna_ssg_env_level(ssg)
       : (opna_ssg_tone_volume(ssg, ch) << 1) + 1;
}

void opna_ssg_generate_raw(struct opna_ssg *ssg, int16_t *buf, int samples) {
  for (int i = 0; i < samples; i++) {
    if (((++ssg->noise_counter) >> 1) >= opna_ssg_noise_period(ssg)) {
      ssg->noise_counter = 0;
      ssg->lfsr |= (!((ssg->lfsr & 1) ^ ((ssg->lfsr >> 3) & 1))) << 17;
      ssg->lfsr >>= 1;
    }
    if (!ssg->env_holding) {
      if (++ssg->env_counter >= opna_ssg_env_period(ssg)) {
        ssg->env_counter = 0;
        ssg->env_level++;
        if (ssg->env_level == 0x20) {
          ssg->env_level = 0;
          if (ssg->env_alt) {
            ssg->env_att = !ssg->env_att;
          }
          if (ssg->env_hld) {
            ssg->env_level = 0x1f;
            ssg->env_holding = true;
          }
        }
      }
    }

    int16_t out = 0;
    for (int ch = 0; ch < 3; ch++) {
      if (++ssg->ch[ch].tone_counter >= opna_ssg_tone_period(ssg, ch)) {
        ssg->ch[ch].tone_counter = 0;
        ssg->ch[ch].out = !ssg->ch[ch].out;
      }
      if (ssg->mask & (1<<ch)) continue;
#if 1
      // may output DC offset
      // YMF288 seems to disable output when 0 <= Tp < 8
      if (opna_ssg_tone_out(ssg, ch)) {
        int level = opna_ssg_chan_env(ssg, ch)
          ? opna_ssg_env_level(ssg)
          : (opna_ssg_tone_volume(ssg, ch) << 1) + 1;
        out += voltable[level];
      }
#else
      if (!opna_ssg_tone_silent(ssg, ch)) {
        int level = opna_ssg_channel_level(ssg, ch);
        out += (opna_ssg_tone_out(ssg, ch) ? voltable[level] : -voltable[level]) / 2;
      }
#endif
      
    }
    buf[i] = out / 2;
  }
}

#define BUFINDEX(n) ((((resampler->index)>>1)+n)&(SINCTABLELEN-1))

void opna_ssg_mix_55466(
  struct opna_ssg *ssg, struct opna_ssg_resampler *resampler,
  int16_t *buf, int samples) {
  for (int i = 0; i < samples; i++) {
    {
      int ssg_samples = ((resampler->index + 9)>>1) - ((resampler->index)>>1);
      int16_t ssgbuf[5];
      opna_ssg_generate_raw(ssg, ssgbuf, ssg_samples);
      for (int j = 0; j < ssg_samples; j++) {
        resampler->buf[BUFINDEX(j)] = ssgbuf[j];
      }
      resampler->index += 9;
    }
    int32_t sample = 0;
    for (int j = 0; j < SINCTABLELEN; j++) {
      unsigned sincindex = j*2;
      if (!(resampler->index&1)) sincindex++;
      bool sincsign = sincindex & (1<<(SINCTABLEBIT));
      unsigned sincmask = ((1<<(SINCTABLEBIT))-1);
      sincindex = (sincindex & sincmask) ^ (sincsign ? sincmask : 0);
      sample += (resampler->buf[BUFINDEX(j)] * sinctable[sincindex])>>2;
    }

    sample >>= 16;
    sample *= 13000;
    sample >>= 14;

    int32_t lo = buf[i*2+0];
    int32_t ro = buf[i*2+1];
    lo += sample;
    ro += sample;
    if (lo < INT16_MIN) lo = INT16_MIN;
    if (lo > INT16_MAX) lo = INT16_MAX;
    if (ro < INT16_MIN) ro = INT16_MIN;
    if (ro > INT16_MAX) ro = INT16_MAX;
    buf[i*2+0] = lo;
    buf[i*2+1] = ro;
  }
}
#undef BUFINDEX
