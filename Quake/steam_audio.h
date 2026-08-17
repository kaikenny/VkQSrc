/*
Steam Audio (phonon.h) integration wrapper for vkQuake.

vkQuake's software mixer (snd_mix.c) paints channels in variable-length
chunks (SND_PaintChannelFrom8/16 are called with whatever `count` fits
between loop points and paint-buffer boundaries), but Steam Audio's
binaural effect wants fixed-size frames. So this wrapper owns a small
per-channel frame accumulator, keyed by the channel's index in
snd_channels[], and lets callers feed it arbitrary-sized chunks of
mono audio; it buffers internally and emits spatialized stereo output
as full frames become available.

Practical effect of this design:
  - There's up to STEAMAUDIO_FRAMESIZE-1 samples of latency (~5.8ms at
    44100Hz) between input and spatialized output.
  - The last partial frame of a sound that stops abruptly is dropped
    rather than flushed. For sound effects this is inaudible; it's a
    known simplification, not a crash risk.

When built without USE_STEAMAUDIO (steam_audio.c is not even compiled
in that case, see meson.build), every function below becomes a cheap
no-op inline, so call sites never need to be wrapped in #ifdef.
*/
#ifndef __STEAM_AUDIO_H
#define __STEAM_AUDIO_H

#include "quakedef.h"

/* Internal processing frame size (samples). Not the mixer's chunk size --
   see the file comment above for why these are decoupled. */
#define STEAMAUDIO_FRAMESIZE 256

#ifdef USE_STEAMAUDIO

/* Creates the Steam Audio context + default HRTF at the given output
   sample rate (pass shm->speed). Returns false on failure; caller
   should continue without spatialization in that case. */
qboolean SteamAudio_Init (int samplerate);

/* Releases HRTF, context, and all per-channel effects/buffers.
   Safe to call if never initialized. */
void SteamAudio_Shutdown (void);

/* True if Init succeeded and Steam Audio is usable this session. */
qboolean SteamAudio_Available (void);

/* Call whenever a channel slot starts playing a brand new sound
   (S_StartSound / S_StaticSound), so leftover buffered audio from
   whatever previously occupied that slot doesn't bleed into the new
   sound. channel_idx is the index into snd_channels[]. Cheap/safe to
   call even if Steam Audio isn't available. */
void SteamAudio_ResetChannel (int channel_idx);

/*
Spatializes `count` mono samples for the given channel slot and ADDS
the result into out_left/out_right (existing contents are preserved,
not overwritten -- matches how SND_PaintChannelFrom8/16 accumulate
into the paint buffer).

direction   - direction from listener to source, listener-relative
              (already rotated into listener space by the caller --
              see the note in snd_mix.c's SND_PaintChannelFromSteamAudio
              about axis convention). Need not be normalized.
gain        - linear multiplier applied to Steam Audio's [-1,1]-ish
              output before adding into out_left/out_right. Callers
              should scale this to match the fixed-point range the
              rest of the mixer uses (see snd_mix.c).
occlusion   - [0,1], 1 = fully visible, 0 = fully occluded. Applied via
              Steam Audio's IPLDirectEffect *before* the binaural
              effect. Callers currently derive this from a single BSP
              hull-trace fraction (see snd_mix.c), which is a soft,
              non-physical approximation -- not sphere-sampled partial
              occlusion -- but avoids a hard on/off pop at wall edges.
in_mono     - `count` samples, mono, normalized to roughly [-1,1].
count       - number of samples this call; does NOT need to be a
              multiple of STEAMAUDIO_FRAMESIZE.
out_left/out_right/out_stride - out_left[i*out_stride] and
              out_right[i*out_stride] receive sample i's contribution,
              for i in [0, count). Because of internal frame
              buffering, some calls may add nothing (silence) while a
              frame is still filling; a later call will flush the
              backlog. Pass e.g. (int*)paintbuffer + paintbufferstart*2
              and that+1, out_stride=2, to target vkQuake's
              portable_samplepair_t paint buffer directly.
*/
void SteamAudio_ProcessChannel (int channel_idx, const vec3_t direction, float gain, float occlusion, const float *in_mono, int count,
								 int *out_left, int *out_right, int out_stride);

qboolean SND_ShouldSpatialize (channel_t *ch, sfxcache_t *sc);

#else /* !USE_STEAMAUDIO */

static inline qboolean SteamAudio_Init (int samplerate)
{
	(void) samplerate;
	return false;
}

static inline void SteamAudio_Shutdown (void) {}

static inline qboolean SteamAudio_Available (void)
{
	return false;
}

static inline void SteamAudio_ResetChannel (int channel_idx)
{
	(void) channel_idx;
}

static inline void SteamAudio_ProcessChannel (int channel_idx, const vec3_t direction, float gain, float occlusion, const float *in_mono,
											   int count, int *out_left, int *out_right, int out_stride)
{
	(void) channel_idx;
	(void) direction;
	(void) gain;
	(void) occlusion;
	(void) in_mono;
	(void) count;
	(void) out_left;
	(void) out_right;
	(void) out_stride;
}

#endif /* USE_STEAMAUDIO */

#endif /* __STEAM_AUDIO_H */