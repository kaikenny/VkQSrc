/*
Steam Audio (phonon.h) integration wrapper for vkQuake.
See steam_audio.h for the public API and the design rationale (fixed
internal frame size vs. the mixer's variable-length paint chunks).

Only compiled when USE_STEAMAUDIO is defined -- meson.build adds this
source and the define together, only after finding libphonon.
*/
#include "quakedef.h"
#include "steam_audio.h"

#ifdef USE_STEAMAUDIO

#include <phonon.h>

typedef struct
{
	IPLBinauralEffect effect;		 /* NULL until first used */
	IPLDirectEffect	  direct_effect; /* NULL until first used; applies occlusion */

	float in_buf[STEAMAUDIO_FRAMESIZE];
	int	  in_count; /* valid samples currently buffered in in_buf */

	float out_buf[STEAMAUDIO_FRAMESIZE * 2]; /* interleaved stereo, produced but not yet drained */
	int	  out_avail;						  /* sample-pairs available at the front of out_buf */
} sa_channel_t;

static IPLContext		 sa_context = NULL;
static IPLHRTF			 sa_hrtf = NULL;
static IPLAudioSettings sa_audiosettings;
static qboolean			 sa_initialized = false;

static sa_channel_t sa_channels[MAX_CHANNELS];

static IPLAudioBuffer sa_scratch_out;

/* Output of the direct (occlusion) effect for whichever channel is
   currently being processed -- feeds into the binaural effect below.
   Processing is fully serialized (single mixer thread, one frame
   processed at a time under snd_mutex -- see snd_dma.c/snd_mix.c), so
   a single shared scratch buffer is safe, same as sa_scratch_out. */
static float sa_scratch_direct[STEAMAUDIO_FRAMESIZE];

qboolean SteamAudio_Init (int samplerate)
{
	IPLContextSettings contextSettings;
	IPLHRTFSettings		hrtfSettings;
	IPLerror			err;

	if (sa_initialized)
		return true;

	memset (&contextSettings, 0, sizeof (contextSettings));
	contextSettings.version = STEAMAUDIO_VERSION;

	err = iplContextCreate (&contextSettings, &sa_context);
	if (err != IPL_STATUS_SUCCESS)
	{
		Con_Printf ("SteamAudio_Init: iplContextCreate failed (%d)\n", (int) err);
		sa_context = NULL;
		return false;
	}

	memset (&sa_audiosettings, 0, sizeof (sa_audiosettings));
	sa_audiosettings.samplingRate = samplerate;
	sa_audiosettings.frameSize = STEAMAUDIO_FRAMESIZE;

	memset (&hrtfSettings, 0, sizeof (hrtfSettings));
	hrtfSettings.type = IPL_HRTFTYPE_DEFAULT;
	hrtfSettings.volume = 1.0f;

	err = iplHRTFCreate (sa_context, &sa_audiosettings, &hrtfSettings, &sa_hrtf);
	if (err != IPL_STATUS_SUCCESS)
	{
		Con_Printf ("SteamAudio_Init: iplHRTFCreate failed (%d)\n", (int) err);
		iplContextRelease (&sa_context);
		sa_context = NULL;
		return false;
	}

	if (iplAudioBufferAllocate (sa_context, 2, STEAMAUDIO_FRAMESIZE, &sa_scratch_out) != IPL_STATUS_SUCCESS)
	{
		Con_Printf ("SteamAudio_Init: iplAudioBufferAllocate failed\n");
		iplHRTFRelease (&sa_hrtf);
		iplContextRelease (&sa_context);
		sa_hrtf = NULL;
		sa_context = NULL;
		return false;
	}

	memset (sa_channels, 0, sizeof (sa_channels));

	sa_initialized = true;
	Con_Printf ("SteamAudio: initialized (%d Hz, %d samples/frame)\n", samplerate, STEAMAUDIO_FRAMESIZE);
	return true;
}

void SteamAudio_Shutdown (void)
{
	int i;

	if (!sa_initialized)
		return;

	for (i = 0; i < MAX_CHANNELS; i++)
	{
		if (sa_channels[i].effect)
			iplBinauralEffectRelease (&sa_channels[i].effect);
		if (sa_channels[i].direct_effect)
			iplDirectEffectRelease (&sa_channels[i].direct_effect);
	}
	memset (sa_channels, 0, sizeof (sa_channels));

	iplAudioBufferFree (sa_context, &sa_scratch_out);
	if (sa_hrtf)
		iplHRTFRelease (&sa_hrtf);
	if (sa_context)
		iplContextRelease (&sa_context);

	sa_hrtf = NULL;
	sa_context = NULL;
	sa_initialized = false;
}

qboolean SteamAudio_Available (void)
{
	return sa_initialized;
}

void SteamAudio_ResetChannel (int channel_idx)
{
	sa_channel_t *c;

	if (!sa_initialized || channel_idx < 0 || channel_idx >= MAX_CHANNELS)
		return;

	c = &sa_channels[channel_idx];
	c->in_count = 0;
	c->out_avail = 0;
}

static qboolean SA_EnsureEffect (sa_channel_t *c)
{
	IPLBinauralEffectSettings effectSettings;
	IPLDirectEffectSettings   directSettings;

	if (!c->effect)
	{
		memset (&effectSettings, 0, sizeof (effectSettings));
		effectSettings.hrtf = sa_hrtf;

		if (iplBinauralEffectCreate (sa_context, &sa_audiosettings, &effectSettings, &c->effect) != IPL_STATUS_SUCCESS)
		{
			Con_Printf ("SteamAudio: iplBinauralEffectCreate failed\n");
			c->effect = NULL;
			return false;
		}
	}

	if (!c->direct_effect)
	{
		memset (&directSettings, 0, sizeof (directSettings));
		directSettings.numChannels = 1; /* mono in, mono out -- runs before the binaural effect */

		if (iplDirectEffectCreate (sa_context, &sa_audiosettings, &directSettings, &c->direct_effect) != IPL_STATUS_SUCCESS)
		{
			Con_Printf ("SteamAudio: iplDirectEffectCreate failed\n");
			c->direct_effect = NULL;
			return false;
		}
	}

	return true;
}

static void SA_RunFrame (sa_channel_t *c, const vec3_t direction, float occlusion)
{
	IPLDirectEffectParams	 directParams;
	IPLBinauralEffectParams params;
	float				   *indata[1];
	float				   *directdata[1];
	IPLAudioBuffer			inbuf;
	IPLAudioBuffer			directbuf;

	indata[0] = c->in_buf;
	inbuf.numChannels = 1;
	inbuf.numSamples = STEAMAUDIO_FRAMESIZE;
	inbuf.data = indata;

	directdata[0] = sa_scratch_direct;
	directbuf.numChannels = 1;
	directbuf.numSamples = STEAMAUDIO_FRAMESIZE;
	directbuf.data = directdata;

	if (occlusion < 0.0f)
		occlusion = 0.0f;
	else if (occlusion > 1.0f)
		occlusion = 1.0f;

	memset (&directParams, 0, sizeof (directParams));
	directParams.flags = IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION;
	directParams.occlusion = occlusion;

	iplDirectEffectApply (c->direct_effect, &directParams, &inbuf, &directbuf);

	memset (&params, 0, sizeof (params));
	params.direction.x = direction[0];
	params.direction.y = direction[1];
	params.direction.z = direction[2];
	params.interpolation = IPL_HRTFINTERPOLATION_NEAREST;
	params.spatialBlend = 1.0f;
	params.hrtf = sa_hrtf;
	params.peakDelays = NULL;

	iplBinauralEffectApply (c->effect, &params, &directbuf, &sa_scratch_out);
	iplAudioBufferInterleave (sa_context, &sa_scratch_out, c->out_buf);

	c->out_avail = STEAMAUDIO_FRAMESIZE;
	c->in_count = 0;
}

void SteamAudio_ProcessChannel (int channel_idx, const vec3_t direction, float gain, float occlusion, const float *in_mono, int count,
								 int *out_left, int *out_right, int out_stride)
{
	sa_channel_t *c;
	int			  written = 0;
	int			  in_pos = 0;
	int			  guard; /* safety cap on loop iterations */

	if (!sa_initialized || channel_idx < 0 || channel_idx >= MAX_CHANNELS || count <= 0)
		return;

	c = &sa_channels[channel_idx];
	if (!SA_EnsureEffect (c))
		return;

	guard = (count / STEAMAUDIO_FRAMESIZE) + 4;

	while (guard-- > 0)
	{
		/* 1) drain whatever spatialized output is already sitting in out_buf */
		if (c->out_avail > 0 && written < count)
		{
			int n = c->out_avail;
			int remaining = count - written;
			int i;
			if (n > remaining)
				n = remaining;

			for (i = 0; i < n; i++)
			{
				out_left[(written + i) * out_stride] += (int) (c->out_buf[i * 2 + 0] * gain);
				out_right[(written + i) * out_stride] += (int) (c->out_buf[i * 2 + 1] * gain);
			}
			if (n < c->out_avail)
				memmove (c->out_buf, c->out_buf + n * 2, (c->out_avail - n) * 2 * sizeof (float));
			c->out_avail -= n;
			written += n;
		}

		if (in_pos < count)
		{
			int need = STEAMAUDIO_FRAMESIZE - c->in_count;
			int have = count - in_pos;
			int take = (need < have) ? need : have;
			if (take > 0)
			{
				memcpy (c->in_buf + c->in_count, in_mono + in_pos, take * sizeof (float));
				c->in_count += take;
				in_pos += take;
			}
		}

		if (c->in_count >= STEAMAUDIO_FRAMESIZE)
		{
			SA_RunFrame (c, direction, occlusion);
			continue;
		}

		/* nothing left to do this call */
		if (written >= count && in_pos >= count)
			break;
		if (c->out_avail == 0 && c->in_count < STEAMAUDIO_FRAMESIZE)
			break; /* stalled waiting for enough input to fill a frame */
	}
}

#endif /* USE_STEAMAUDIO */