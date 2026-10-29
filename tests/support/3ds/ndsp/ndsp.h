#pragma once
#include <3ds.h>
struct ndspWaveBuf {
  s16 *data_pcm16 = nullptr;
  u32 nsamples = 0;
  bool looping = false;
  u8 status = 0;
};
inline constexpr u8 NDSP_WBUF_DONE = 0, NDSP_WBUF_QUEUED = 1;
inline constexpr int NDSP_FORMAT_MONO_PCM16 = 1, NDSP_FORMAT_STEREO_PCM16 = 2;
inline constexpr int NDSP_OUTPUT_STEREO = 0;
void ndspChnReset(int);
void ndspChnSetFormat(int, int);
void ndspChnSetRate(int, float);
void ndspChnSetMix(int, float *);
void ndspSetOutputMode(int);
void ndspSetMasterVol(float);
void ndspChnWaveBufAdd(int, ndspWaveBuf *);
void DSP_FlushDataCache(void *, u32);
