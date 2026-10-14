/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DirectSound HLE.
 *
 * The XDK DirectSound library programs the MCPX APU (voice processor + DSP)
 * directly. Instead of emulating that hardware, its public API is replaced:
 * buffers play straight out of game memory (as on the APU), streams take the
 * game's packets, PCM and Xbox ADPCM are decoded and resampled in a software
 * mixer, and the result goes to waveOut. XACT and Bink sit on top of this
 * API and run unchanged.
 *
 * Mix bins: every voice sends its channels to up to 8 of the APU's mix bins
 * (speakers, crosstalk/HRTF bins, effect sends), each with its own volume. In
 * a stereo voice's bin list the entries alternate left, right, left, ... The
 * speaker and crosstalk bins are folded down to stereo here (LFE is left
 * out, as in a stereo downmix); effect sends (reverb) are not rendered.
 *
 * Entry points come from the title's symbol table (XbSymbolDatabase names).
 * XDK 4134 titles (JSRF) use the older mix bin API, which is ignored. */
#include "hle.h"
#include <mmsystem.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MIX_RATE   48000
#define MIX_FRAMES 480          /* 10 ms blocks */
#define MIX_BUFS   4

#define DS_OK 0
#define DSERR_INVALIDCALL  0x88780032
#define DSERR_OUTOFMEMORY  0x8007000E
#define E_PENDING_X        0x8000000A
#define E_ABORT_X          0x80004004

#define DSBPLAY_LOOPING    1
#define DSBPLAY_FROMSTART  2
#define DSBSTATUS_PLAYING  1
#define DSBSTATUS_PAUSED   2
#define DSBSTATUS_LOOPING  4
#define DSBSTOPEX_ENVELOPE        1
#define DSBSTOPEX_RELEASEWAVEFORM 2
#define DSBCAPS_MIXIN      0x00002000
/* IDirectSoundStream::GetStatus (XMediaObject status in the low bits) */
#define DSSTREAMSTATUS_READY    0x00000001
#define DSSTREAMSTATUS_PLAYING  0x00010000
#define DSSTREAMSTATUS_PAUSED   0x00020000
#define DSSTREAMSTATUS_STARVED  0x00040000
#define DSSTREAMSTATUS_ENVELOPE 0x00080000
#define DSSTREAMFLUSHEX_ENVELOPE  4
#define DSSTREAMFLUSHEX_ENVELOPE2 2
#define WAVE_FORMAT_XBOX_ADPCM 0x0069
#define E_FAIL_X           0x80004005

/* Voice.paused: the two pause states of the library (mutually exclusive) */
#define PAUSE_APP   1          /* Pause(DSBPAUSE_PAUSE) */
#define PAUSE_SYNCH 2          /* waiting for IDirectSound_SynchPlayback */

/* mix bins */
enum { BIN_FL, BIN_FR, BIN_C, BIN_LFE, BIN_BL, BIN_BR, BIN_XTLK_FL, BIN_XTLK_FR, BIN_XTLK_BL, BIN_XTLK_BR, BIN_I3DL2 };

#pragma pack(push, 1)
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize; } XWAVEFORMATEX;
#pragma pack(pop)
typedef struct { DWORD dwMixBin; LONG lVolume; } MIXBINPAIR;
typedef struct { DWORD dwCount; const MIXBINPAIR *lpPairs; } XDSMIXBINS;
typedef struct { DWORD dwSize, dwFlags, dwBufferBytes; XWAVEFORMATEX *lpwfxFormat; const XDSMIXBINS *lpMixBins; DWORD dwInputMixBin; } XDSBUFFERDESC;
typedef struct { DWORD dwFlags, dwMaxAttachedPackets; XWAVEFORMATEX *lpwfxFormat; void *lpfnCallback; void *lpvContext; const XDSMIXBINS *lpMixBins; } XDSSTREAMDESC;
typedef struct { void *pvBuffer; DWORD dwMaxSize; DWORD *pdwCompletedSize; DWORD *pdwStatus; void *ctx; LONGLONG *prtTimestamp; } XMEDIAPACKET;
typedef struct { DWORD dwFlags, dwInputSize, dwOutputSize, dwMaxLookahead; } XMEDIAINFO;
typedef struct { DWORD dwMixBinCount; MIXBINPAIR pairs[8]; LONG lPitch, lVolume, lI3DL2DirectVolume, lI3DL2RoomVolume; } XDSVOICEPROPS;
typedef struct { DWORD dwOffset; HANDLE hEvent; } XDSBPOSITIONNOTIFY;

/* ---------------- voices ---------------- */
#define VOICE_MAGIC 0x45434F56
#define FADE_FRAMES (MIX_RATE / 20)    /* 50 ms envelope release (envelopes themselves are not emulated) */
#define MAX_PACKETS 64
enum { K_BUFFER = 1, K_STREAM };

typedef struct Packet { XMEDIAPACKET p; uint32_t done; } Packet;

typedef struct Voice {
    const void *vtbl;              /* streams: the XMediaObject vtable the game calls through */
    uint32_t magic;
    LONG refs;
    int kind;
    struct Voice *next;
    /* format */
    XWAVEFORMATEX fmt;
    int adpcm, ch;
    uint32_t frame_bytes;          /* PCM: bytes per frame; ADPCM: bytes per block (all channels) */
    /* parameters */
    LONG volume, headroom, pitch;  /* mB, mB, 4096 per octave (relative to 48 kHz) */
    DWORD freq;                    /* 0: from the pitch */
    int nbins; MIXBINPAIR bins[8];
    uint8_t d3[0x38]; int use3d, has3d;
    struct Voice *out;             /* SetOutputBuffer: a MIXIN (submix) buffer */
    int mixin; DWORD input_bin;
    float g[2][2];                 /* [source channel][L/R] linear gain */
    double step;                   /* source frames per output frame */
    /* playback */
    int playing;                   /* the voice is running (it may still be paused or starved) */
    int paused;                    /* PAUSE_APP / PAUSE_SYNCH */
    int looping, release;
    int fade;                      /* stopping with an envelope release: frames left of the fade-out */
    double pos;                    /* frames, from the start of the play region (buffers) or packet (streams) */
    /* buffer source */
    uint8_t *data; uint32_t bytes; int owned;
    uint32_t play_start, play_len, loop_start, loop_len;   /* bytes */
    XDSBPOSITIONNOTIFY notify[16]; int nnotify;
    /* stream source */
    Packet q[MAX_PACKETS]; int qhead, qn, qmax;
    void *callback, *cb_ctx;
    int discontinuity, starved;
    int noact;                     /* Pause(DSSTREAMPAUSE_PAUSENOACTIVATE): packets queue up, the voice waits for Pause(0) */
    int flush_on_release;          /* FlushEx(ENVELOPE): flush the packets once the release is over */
    int env_hold;                  /* ...and after that: reports DSSTREAMSTATUS_ENVELOPE and plays nothing until Flush() */
    /* ADPCM decode cache */
    const uint8_t *cache_src; int16_t cache[2][65];
} Voice;

static Voice *g_voices;
static CRITICAL_SECTION g_ds_lock;
static int g_ds_started, g_old_mixbins;
static struct { uint32_t magic; LONG refs; } g_ds = { 0x4E534453, 1 };
static uint8_t *g_effect_mem[64];
static LONG g_bin_headroom[32];
float g_master_volume = 1.0f;
int g_ds_trace;
static volatile LONG g_mix_peak;     /* for --ds-trace level reports */
#define DST(...) do { if (g_ds_trace) { static int _n; if (_n++ < 4000) xlog(__VA_ARGS__); } } while (0)

static Voice *V(void *p) { Voice *v = p; return (v && v->magic == VOICE_MAGIC) ? v : NULL; }
static float mb_to_gain(LONG mb) { if (mb <= -10000) return 0.0f; if (mb > 1200) mb = 1200; return powf(10.0f, mb / 2000.0f); }
static LONG clamp_mb(LONG v) { return v < -10000 ? -10000 : v > 0 ? 0 : v; }

/* ---------------- decoding ---------------- */
static const int ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
static const int ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767 };

/* Xbox ADPCM: per channel 36-byte blocks -> 65 samples (header sample + 64 nibbles),
 * channels interleaved in 4-byte groups after the per-channel headers. */
static void adpcm_decode_block(Voice *v, const uint8_t *src)
{
    int ch = v->ch;
    for (int c = 0; c < ch; c++) {
        int pred = (int16_t)(src[c * 4] | (src[c * 4 + 1] << 8));
        int idx = src[c * 4 + 2]; if (idx > 88) idx = 88;
        int16_t *out = v->cache[c];
        out[0] = (int16_t)pred;
        int n = 1;
        for (int g = 0; g < 8; g++) {
            const uint8_t *p = src + ch * 4 + (g * ch + c) * 4;
            for (int k = 0; k < 8; k++) {
                int nib = (p[k >> 1] >> ((k & 1) * 4)) & 0xF;
                int step = ima_step[idx], diff = step >> 3;
                if (nib & 1) diff += step >> 2;
                if (nib & 2) diff += step >> 1;
                if (nib & 4) diff += step;
                if (nib & 8) pred -= diff; else pred += diff;
                if (pred > 32767) pred = 32767;
                if (pred < -32768) pred = -32768;
                idx += ima_index[nib]; if (idx < 0) idx = 0; if (idx > 88) idx = 88;
                out[n++] = (int16_t)pred;
            }
        }
    }
    v->cache_src = src;
}

static void set_format(Voice *v, const XWAVEFORMATEX *f)
{
    if (f) v->fmt = *f;
    else { memset(&v->fmt, 0, sizeof v->fmt); v->fmt.wFormatTag = 1; v->fmt.nChannels = 2; v->fmt.nSamplesPerSec = 48000; v->fmt.wBitsPerSample = 16; v->fmt.nBlockAlign = 4; }
    v->adpcm = v->fmt.wFormatTag == WAVE_FORMAT_XBOX_ADPCM;
    v->ch = v->fmt.nChannels >= 2 ? 2 : 1;
    if (v->adpcm) v->frame_bytes = 36 * v->ch;
    else v->frame_bytes = v->fmt.nBlockAlign ? v->fmt.nBlockAlign : (uint32_t)v->ch * (v->fmt.wBitsPerSample / 8);
    if (!v->frame_bytes) v->frame_bytes = 1;
    v->cache_src = NULL;
}
static uint32_t bytes_to_frames(Voice *v, uint32_t by) { return v->adpcm ? by / v->frame_bytes * 65 : by / v->frame_bytes; }
static uint32_t frames_to_bytes(Voice *v, uint32_t fr) { return v->adpcm ? fr / 65 * v->frame_bytes : fr * v->frame_bytes; }

/* sample `frame` of a byte range: s[0] left (or mono), s[1] right */
static void fetch(Voice *v, const uint8_t *base, uint32_t frame, float s[2])
{
    if (v->adpcm) {
        const uint8_t *blk = base + frame / 65 * v->frame_bytes;
        if (blk != v->cache_src) adpcm_decode_block(v, blk);
        int i = frame % 65;
        s[0] = v->cache[0][i] * (1.0f / 32768);
        s[1] = v->ch > 1 ? v->cache[1][i] * (1.0f / 32768) : s[0];
        return;
    }
    const uint8_t *p = base + frame * v->frame_bytes;
    if (v->fmt.wBitsPerSample == 8) {
        s[0] = (p[0] - 128) * (1.0f / 128);
        s[1] = v->ch > 1 ? (p[1] - 128) * (1.0f / 128) : s[0];
    } else {
        s[0] = *(const int16_t *)p * (1.0f / 32768);
        s[1] = v->ch > 1 ? *(const int16_t *)(p + 2) * (1.0f / 32768) : s[0];
    }
}

/* ---------------- parameters -> gains ---------------- */
static void update_voice(Voice *v)
{
    /* resampling step: explicit frequency, else the pitch (0 = 48 kHz) */
    double hz = v->freq ? (double)v->freq : 48000.0 * pow(2.0, v->pitch / 4096.0);
    if (v->has3d && v->use3d && (*(DWORD *)v->d3 & 0x20)) hz *= pow(2.0, *(LONG *)(v->d3 + 0x24) / 4096.0);
    v->step = hz / MIX_RATE;
    if (v->step > 16) v->step = 16;

    /* 3D attenuation per bin group (DirectSound's own split) */
    LONG a_front = 0, a_center = 0, a_xf = 0, a_xb = 0, a_i3 = 0;
    if (v->has3d && v->use3d) {
        const LONG *d = (const LONG *)v->d3;
        LONG base = clamp_mb(d[1] + d[2]);
        a_front = base;
        a_center = clamp_mb(d[7] + d[6] + d[3] + base);
        a_xf = clamp_mb(d[7] + d[5] + d[3] + base);
        a_xb = clamp_mb(d[7] + d[4] + base);
        a_i3 = clamp_mb(d[8] + base);
    }
    memset(v->g, 0, sizeof v->g);
    int nb = v->nbins;
    MIXBINPAIR defb[2] = { { BIN_FL, 0 }, { BIN_FR, 0 } };
    const MIXBINPAIR *b = v->bins;
    if (!nb || g_old_mixbins) { b = defb; nb = 2; }
    for (int i = 0; i < nb; i++) {
        int c = v->ch > 1 && nb >= 2 ? (i & 1) : 0;
        DWORD bin = b[i].dwMixBin;
        LONG mb = v->volume + b[i].lVolume - (g_old_mixbins ? 0 : v->headroom) + (bin < 32 ? g_bin_headroom[bin] : 0);
        switch (bin) {
        case BIN_C: mb += a_center; break;
        case BIN_XTLK_FL: case BIN_XTLK_FR: mb += a_xf; break;
        case BIN_XTLK_BL: case BIN_XTLK_BR: mb += a_xb; break;
        case BIN_I3DL2: mb += a_i3; break;
        default: mb += a_front; break;
        }
        float gl = mb_to_gain(mb);
        switch (bin) {
        case BIN_FL: case BIN_XTLK_FL: v->g[c][0] += gl; break;
        case BIN_FR: case BIN_XTLK_FR: v->g[c][1] += gl; break;
        case BIN_BL: case BIN_XTLK_BL: v->g[c][0] += gl * 0.7071f; break;
        case BIN_BR: case BIN_XTLK_BR: v->g[c][1] += gl * 0.7071f; break;
        case BIN_C: v->g[c][0] += gl * 0.7071f; v->g[c][1] += gl * 0.7071f; break;
        default: break;                                   /* I3DL2 reverb and FX sends: not rendered */
        }
    }
    if (v->ch == 1) { v->g[1][0] = v->g[0][0]; v->g[1][1] = v->g[0][1]; }
    for (int c = 0; c < 2; c++) for (int k = 0; k < 2; k++) if (v->g[c][k] > 2.0f) v->g[c][k] = 2.0f;
}

/* A voice routed into a submix (MIXIN) buffer loses its own mix bins: it
 * feeds the submix at its own volume, and the submix voice's bins and volume
 * take it to the speakers. */
static void effective_gains(Voice *v, float g[2][2])
{
    Voice *o = v->out;
    if (!o) { memcpy(g, v->g, sizeof v->g); return; }
    float vg = mb_to_gain(v->volume - v->headroom);
    for (int depth = 0; o->out && depth < 4; depth++) { vg *= mb_to_gain(o->volume - o->headroom); o = o->out; }
    for (int c = 0; c < 2; c++) for (int k = 0; k < 2; k++) g[c][k] = vg * o->g[c][k];
}

/* ---------------- stream packets ---------------- */
typedef struct { void *fn, *sctx, *pctx; DWORD status; } StreamCb;
static void run_stream_callback(void *p)
{
    StreamCb *c = p;
    uint32_t a[3] = { (uint32_t)c->sctx, (uint32_t)c->pctx, c->status };
    xcall(c->fn, 3, a);
    free(c);
}
/* complete the head packet (lock held) */
static void packet_done(Voice *v, DWORD status)
{
    Packet *k = &v->q[v->qhead];
    if (k->p.pdwCompletedSize) *k->p.pdwCompletedSize = status == 0 ? k->p.dwMaxSize : k->done;
    if (k->p.pdwStatus) *k->p.pdwStatus = status;
    if (v->callback) {
        StreamCb *c = malloc(sizeof *c);
        c->fn = v->callback; c->sctx = v->cb_ctx; c->pctx = k->p.ctx; c->status = status;
        kernel_queue_work(run_stream_callback, c);
    } else if (k->p.ctx) SetEvent((HANDLE)k->p.ctx);
    v->qhead = (v->qhead + 1) % MAX_PACKETS; v->qn--;
    v->cache_src = NULL;
}
/* The stream model follows the library (CMcpxStream): Process() queues a
 * packet and starts the voice unless it is held back (PAUSENOACTIVATE, or the
 * envelope hold after FlushEx(ENVELOPE)); Flush() completes every packet as
 * flushed and stops. FlushEx(ENVELOPE) lets the voice play out its release,
 * then flushes and stays in the "envelope" state, where new packets are queued
 * but not played, until the owner calls Flush(). XACT relies on that
 * handshake to retire a streamed sound (all with the lock held). */
static void stream_stop(Voice *v)
{
    v->playing = 0; v->fade = 0; v->starved = 0; v->pos = 0; v->cache_src = NULL;
}
static void stream_flush(Voice *v)
{
    while (v->qn) packet_done(v, E_ABORT_X);
    stream_stop(v);
    v->discontinuity = 0; v->flush_on_release = 0; v->env_hold = 0;
}
static void stream_service(Voice *v)
{
    if (v->env_hold || v->noact || !v->qn) return;
    if (!v->playing) { v->playing = 1; v->pos = 0; }
    v->starved = 0;
}
static void stream_release_done(Voice *v)
{
    stream_stop(v);
    if (v->flush_on_release) {
        while (v->qn) packet_done(v, E_ABORT_X);
        v->flush_on_release = 0; v->discontinuity = 0; v->env_hold = 1;
    }
}

/* ---------------- mixer ---------------- */
static void notify_cross(Voice *v, uint32_t from, uint32_t to)
{
    for (int i = 0; i < v->nnotify; i++) {
        DWORD o = v->notify[i].dwOffset;
        if (o != 0xFFFFFFFF && ((from <= to && o >= from && o < to) || (from > to && (o >= from || o < to))))
            SetEvent(v->notify[i].hEvent);
    }
}
static void notify_stop(Voice *v)
{
    for (int i = 0; i < v->nnotify; i++) if (v->notify[i].dwOffset == 0xFFFFFFFF) SetEvent(v->notify[i].hEvent);
}

static void mix_buffer(Voice *v, float *out, int frames, float g[2][2])
{
    if (!v->data || !v->bytes) { v->playing = 0; return; }
    uint32_t rs = v->play_start < v->bytes ? v->play_start : 0;
    uint32_t rlen = v->play_len && rs + v->play_len <= v->bytes ? v->play_len : v->bytes - rs;
    const uint8_t *base = v->data + rs;
    uint32_t total = bytes_to_frames(v, rlen);
    if (!total) { v->playing = 0; return; }
    uint32_t ls = bytes_to_frames(v, v->loop_start), le = v->loop_len ? ls + bytes_to_frames(v, v->loop_len) : total;
    if (le > total) le = total;
    if (ls >= le) ls = 0;
    int loop = v->looping && !v->release;
    uint32_t pos0 = frames_to_bytes(v, (uint32_t)v->pos);
    for (int i = 0; i < frames; i++) {
        uint32_t f = (uint32_t)v->pos, end = loop ? le : total;
        if (f >= end) {
            if (loop) { v->pos = ls + (v->pos - end); while (v->pos >= le) v->pos -= le - ls; f = (uint32_t)v->pos; }
            else { v->playing = 0; v->pos = 0; notify_stop(v); break; }
        }
        float a[2], b[2], t = (float)(v->pos - f);
        fetch(v, base, f, a);
        uint32_t f2 = f + 1;
        if (f2 >= end) f2 = loop ? ls : f;
        fetch(v, base, f2, b);
        float l = a[0] + (b[0] - a[0]) * t, r = a[1] + (b[1] - a[1]) * t;
        if (v->fade) {
            float k = (float)v->fade / FADE_FRAMES;
            l *= k; r *= k;
            if (--v->fade == 0) { v->playing = 0; v->pos = 0; notify_stop(v); break; }
        }
        out[i * 2]     += l * g[0][0] + r * g[1][0];
        out[i * 2 + 1] += l * g[0][1] + r * g[1][1];
        v->pos += v->step;
    }
    if (v->nnotify) notify_cross(v, pos0, frames_to_bytes(v, (uint32_t)v->pos));
}

static void mix_stream(Voice *v, float *out, int frames, float g[2][2])
{
    for (int i = 0; i < frames; i++) {
        float kf = 1.0f;
        if (v->fade) {                          /* envelope release: runs whether or not there is data */
            kf = (float)v->fade / FADE_FRAMES;
            if (--v->fade == 0) { stream_release_done(v); return; }
        }
        while (v->qn) {                         /* retire finished (and empty) packets */
            Packet *k = &v->q[v->qhead];
            uint32_t total = bytes_to_frames(v, k->p.dwMaxSize);
            if ((uint32_t)v->pos < total) break;
            v->pos -= total; if (v->pos < 0) v->pos = 0;
            k->done = k->p.dwMaxSize;
            packet_done(v, 0);
        }
        if (!v->qn) {
            if (v->fade) continue;
            if (v->discontinuity) { stream_stop(v); v->discontinuity = 0; }
            else v->starved = 1;
            return;
        }
        Packet *k = &v->q[v->qhead];
        uint32_t total = bytes_to_frames(v, k->p.dwMaxSize);
        uint32_t f = (uint32_t)v->pos;
        const uint8_t *base = k->p.pvBuffer;
        float a[2], b[2], t = (float)(v->pos - f);
        fetch(v, base, f, a);
        if (f + 1 < total) fetch(v, base, f + 1, b); else { b[0] = a[0]; b[1] = a[1]; }
        float l = (a[0] + (b[0] - a[0]) * t) * kf, r = (a[1] + (b[1] - a[1]) * t) * kf;
        out[i * 2]     += l * g[0][0] + r * g[1][0];
        out[i * 2 + 1] += l * g[0][1] + r * g[1][1];
        v->pos += v->step;
        k->done = frames_to_bytes(v, (uint32_t)v->pos);
    }
}

static void mix(float *out, int frames)
{
    memset(out, 0, frames * 2 * sizeof(float));
    EnterCriticalSection(&g_ds_lock);
    for (Voice *v = g_voices; v; v = v->next) {
        if (!v->playing || v->paused || v->mixin) continue;
        float g[2][2];
        effective_gains(v, g);
        if (v->kind == K_BUFFER) mix_buffer(v, out, frames, g);
        else mix_stream(v, out, frames, g);
    }
    LeaveCriticalSection(&g_ds_lock);
}

static DWORD WINAPI mixer_thread(LPVOID p)
{
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 2, MIX_RATE, MIX_RATE * 4, 4, 16, 0 };
    HWAVEOUT wo = NULL;
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        wo = NULL; xlog("dsound: no audio device; mixing silently");
    }
    static int16_t pcm[MIX_BUFS][MIX_FRAMES * 2];
    static float mixbuf[MIX_FRAMES * 2];
    WAVEHDR hdr[MIX_BUFS];
    memset(hdr, 0, sizeof hdr);
    for (int i = 0; i < MIX_BUFS; i++) {
        hdr[i].lpData = (LPSTR)pcm[i]; hdr[i].dwBufferLength = sizeof pcm[i];
        if (wo) { waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]); hdr[i].dwFlags |= WHDR_DONE; }
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    LARGE_INTEGER qf, t0, t; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&t0);
    uint64_t mixed = 0;
    for (;;) {
        int any = 0;
        for (int i = 0; i < MIX_BUFS; i++) {
            if (wo) { if (!(hdr[i].dwFlags & WHDR_DONE)) continue; }
            else {   /* no device: keep positions advancing in real time */
                QueryPerformanceCounter(&t);
                uint64_t want = (uint64_t)((t.QuadPart - t0.QuadPart) / (qf.QuadPart / 1000)) * (MIX_RATE / 1000);
                if (mixed + MIX_FRAMES > want) continue;
                mixed += MIX_FRAMES;
            }
            mix(mixbuf, MIX_FRAMES);
            float vol = g_master_volume;
            int peak = 0;
            for (int k = 0; k < MIX_FRAMES * 2; k++) {
                float x = mixbuf[k] * vol, ax = fabsf(x);
                if (ax > 0.9f) {        /* soft knee instead of hard clipping */
                    float y = 0.9f + 0.1f * tanhf((ax - 0.9f) * 10.0f);
                    x = x < 0 ? -y : y;
                }
                int s = (int)(x * 32767.0f);
                if (abs(s) > peak) peak = abs(s);
                pcm[i][k] = (int16_t)s;
            }
            if (peak > g_mix_peak) g_mix_peak = peak;
            if (wo) { hdr[i].dwFlags &= ~WHDR_DONE; waveOutWrite(wo, &hdr[i], sizeof hdr[i]); }
            any = 1;
        }
        if (!any) { if (wo) WaitForSingleObject(ev, 20); else Sleep(2); }
        if (g_ds_trace) {
            static DWORD last; DWORD now = GetTickCount();
            if (now - last > 2000) {
                last = now; int n = 0, np = 0;
                EnterCriticalSection(&g_ds_lock);
                for (Voice *v = g_voices; v; v = v->next) { n++; np += v->playing && !v->paused; }
                LeaveCriticalSection(&g_ds_lock);
                xlog("dsound: %d voices, %d playing, peak %.1f dBFS", n, np, g_mix_peak ? 20 * log10(g_mix_peak / 32767.0) : -99.0);
                g_mix_peak = 0;
            }
        }
    }
    return 0;
}

static void ds_start(void)
{
    if (g_ds_started) return;
    g_ds_started = 1;
    host_create_thread(0, mixer_thread, NULL, 0, NULL);
}

static Voice *voice_new(int kind, const XWAVEFORMATEX *fmt, const XDSMIXBINS *bins)
{
    Voice *v = calloc(1, sizeof *v);
    v->magic = VOICE_MAGIC; v->refs = 1; v->kind = kind;
    set_format(v, fmt);
    v->freq = 0; v->pitch = (LONG)lround(4096.0 * log2(v->fmt.nSamplesPerSec ? v->fmt.nSamplesPerSec / 48000.0 : 1.0));
    if (bins && bins->lpPairs && !g_old_mixbins) {
        v->nbins = bins->dwCount > 8 ? 8 : (int)bins->dwCount;
        memcpy(v->bins, bins->lpPairs, v->nbins * sizeof(MIXBINPAIR));
    }
    update_voice(v);
    EnterCriticalSection(&g_ds_lock);
    v->next = g_voices; g_voices = v;
    LeaveCriticalSection(&g_ds_lock);
    return v;
}
static ULONG voice_release(Voice *v)
{
    if (!v) return 0;
    EnterCriticalSection(&g_ds_lock);
    LONG r = --v->refs;
    if (r <= 0) {
        if (v->kind == K_STREAM) stream_flush(v);
        for (Voice **pp = &g_voices; *pp; pp = &(*pp)->next) if (*pp == v) { *pp = v->next; break; }
        for (Voice *o = g_voices; o; o = o->next) if (o->out == v) o->out = NULL;
    }
    LeaveCriticalSection(&g_ds_lock);
    if (r > 0) return r;
    if (v->owned) free(v->data);
    v->magic = 0; free(v);
    return 0;
}

/* ---------------- IDirectSound ---------------- */
static HRESULT __stdcall hle_DirectSoundCreate(void *guid, void **out, void *unk)
{
    ds_start();
    *out = &g_ds;
    xlog("DirectSoundCreate [hle]");
    return DS_OK;
}
static ULONG __stdcall hle_IDirectSound_AddRef(void *ds) { return ++g_ds.refs; }
static ULONG __stdcall hle_IDirectSound_Release(void *ds) { return g_ds.refs > 1 ? --g_ds.refs : 1; }
static HRESULT __stdcall hle_IDirectSound_GetCaps(void *ds, DWORD *caps)
{ caps[0] = 256; caps[1] = 64; caps[2] = 2047; caps[3] = 0; return DS_OK; }
static HRESULT __stdcall hle_IDirectSound_GetSpeakerConfig(void *ds, DWORD *cfg) { *cfg = 0x00010000; /* stereo */ return DS_OK; }
static HRESULT __stdcall hle_IDirectSound_DownloadEffectsImage(void *ds, const void *img, DWORD size, void *loc, void **desc)
{
    xlog("IDirectSound_DownloadEffectsImage(%lu bytes) [hle]", size);
    if (desc) {
        /* DSEFFECTIMAGEDESC { DWORD dwEffectCount; DSEFFECTMAP aEffectMaps[]; } (8 dwords per map) */
        uint32_t *d = calloc(1, 4 + 32 * 32);
        d[0] = 32;
        for (int i = 0; i < 32; i++) {
            if (!g_effect_mem[i]) g_effect_mem[i] = calloc(1, 0x10000);
            uint32_t *m = d + 1 + i * 8;
            m[0] = (uint32_t)g_effect_mem[i]; m[1] = 0x1000;            /* code */
            m[2] = (uint32_t)g_effect_mem[i]; m[3] = 0x8000;            /* state */
            m[4] = (uint32_t)(g_effect_mem[i] + 0x8000); m[5] = 0x4000; /* Y memory */
            m[6] = (uint32_t)(g_effect_mem[i] + 0xC000); m[7] = 0x4000; /* scratch */
        }
        *desc = d;
    }
    return DS_OK;
}
static HRESULT __stdcall hle_IDirectSound_GetEffectData(void *ds, DWORD idx, DWORD off, void *data, DWORD n)
{
    if (idx >= 64) return DSERR_INVALIDCALL;
    if (!g_effect_mem[idx]) g_effect_mem[idx] = calloc(1, 0x10000);
    if (off + n <= 0x10000) memcpy(data, g_effect_mem[idx] + off, n); else memset(data, 0, n);
    return DS_OK;
}
static HRESULT __stdcall hle_IDirectSound_SetEffectData(void *ds, DWORD idx, DWORD off, const void *data, DWORD n, DWORD apply)
{
    if (idx >= 64) return DSERR_INVALIDCALL;
    if (!g_effect_mem[idx]) g_effect_mem[idx] = calloc(1, 0x10000);
    if (off + n <= 0x10000) memcpy(g_effect_mem[idx] + off, data, n);
    return DS_OK;
}
static HRESULT __stdcall hle_IDirectSound_SetMixBinHeadroom(void *ds, DWORD bin, DWORD headroom)
{
    DST("SetMixBinHeadroom(%lu, %lu)", bin, headroom);
    if (bin >= 32) return DSERR_INVALIDCALL;
    EnterCriticalSection(&g_ds_lock);
    g_bin_headroom[bin] = 0;   /* the DSP undoes the voices' headroom; nothing to do in a float mixer */
    for (Voice *v = g_voices; v; v = v->next) update_voice(v);
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_IDirectSound_GetOutputLevels(void *ds, DWORD *levels, BOOL reset)
{ if (levels) memset(levels, 0, 12 * 4); return DS_OK; }
static HRESULT __stdcall hle_IDirectSound_GetTime(void *ds, LONGLONG *t)
{ LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); *t = (LONGLONG)((double)c.QuadPart * 1e7 / f.QuadPart); return DS_OK; }
static HRESULT __stdcall hle_ds_noop1(void *ds) { return DS_OK; }
static HRESULT __stdcall hle_ds_noop2(void *ds, DWORD a) { return DS_OK; }
static HRESULT __stdcall hle_ds_noop3(void *ds, DWORD a, DWORD b) { return DS_OK; }
static HRESULT __stdcall hle_ds_noop4(void *ds, DWORD a, DWORD b, DWORD c) { return DS_OK; }
static HRESULT __stdcall hle_ds_noop5(void *ds, DWORD a, DWORD b, DWORD c, DWORD e) { return DS_OK; }
static HRESULT __stdcall hle_ds_noop8(void *ds, DWORD a, DWORD b, DWORD c, DWORD e, DWORD f, DWORD g, DWORD h) { return DS_OK; }
static void __stdcall hle_DirectSoundDoWork(void) {}
static HRESULT __stdcall hle_IDirectSound_SynchPlayback(void *ds)
{
    EnterCriticalSection(&g_ds_lock);
    for (Voice *v = g_voices; v; v = v->next) if (v->paused == PAUSE_SYNCH) v->paused = 0;
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static void __cdecl hle_ds_void(void) {}

static HRESULT __stdcall hle_DirectSoundCreateBuffer(XDSBUFFERDESC *desc, void **out)
{
    ds_start();
    Voice *v = voice_new(K_BUFFER, desc->lpwfxFormat, desc->dwSize >= 20 ? desc->lpMixBins : NULL);
    if (desc->dwFlags & DSBCAPS_MIXIN) { v->mixin = 1; v->input_bin = desc->dwSize >= 24 ? desc->dwInputMixBin : 0; }
    else if (desc->dwBufferBytes) { v->bytes = desc->dwBufferBytes; v->data = calloc(1, v->bytes); v->owned = 1; }
    *out = v;
    DST("CreateSoundBuffer -> %p fmt %u ch %u rate %lu bits %u align %u bytes %lu flags %lx bins %d", v, v->fmt.wFormatTag,
        v->fmt.nChannels, v->fmt.nSamplesPerSec, v->fmt.wBitsPerSample, v->fmt.nBlockAlign, desc->dwBufferBytes, desc->dwFlags, v->nbins);
    return DS_OK;
}
static HRESULT __stdcall hle_IDirectSound_CreateSoundBuffer(void *ds, XDSBUFFERDESC *desc, void **out, void *unk)
{ return hle_DirectSoundCreateBuffer(desc, out); }

/* ---------------- voice parameters (buffers and streams) ---------------- */
#define VOICE_OR_FAIL(p) Voice *v = V(p); if (!v) return DSERR_INVALIDCALL
static HRESULT __stdcall hle_V_SetVolume(void *p, LONG mb)
{ VOICE_OR_FAIL(p); EnterCriticalSection(&g_ds_lock); v->volume = mb; update_voice(v); LeaveCriticalSection(&g_ds_lock); return DS_OK; }
static HRESULT __stdcall hle_V_SetPitch(void *p, LONG pitch)
{ VOICE_OR_FAIL(p); EnterCriticalSection(&g_ds_lock); v->pitch = pitch; v->freq = 0; update_voice(v); LeaveCriticalSection(&g_ds_lock); return DS_OK; }
static HRESULT __stdcall hle_V_SetFrequency(void *p, DWORD hz)
{
    VOICE_OR_FAIL(p);
    EnterCriticalSection(&g_ds_lock);
    v->freq = hz; if (!hz) v->pitch = (LONG)lround(4096.0 * log2(v->fmt.nSamplesPerSec ? v->fmt.nSamplesPerSec / 48000.0 : 1.0));
    update_voice(v);
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_V_SetHeadroom(void *p, DWORD h)
{ VOICE_OR_FAIL(p); EnterCriticalSection(&g_ds_lock); v->headroom = (LONG)h; update_voice(v); LeaveCriticalSection(&g_ds_lock); return DS_OK; }
static HRESULT __stdcall hle_V_SetMixBins(void *p, const XDSMIXBINS *b)
{
    VOICE_OR_FAIL(p);
    if (g_old_mixbins) return DS_OK;            /* XDK 4134: a bit mask */
    EnterCriticalSection(&g_ds_lock);
    v->nbins = 0;
    if (b && b->lpPairs) { v->nbins = b->dwCount > 8 ? 8 : (int)b->dwCount; memcpy(v->bins, b->lpPairs, v->nbins * sizeof(MIXBINPAIR)); }
    update_voice(v);
    LeaveCriticalSection(&g_ds_lock);
    if (g_ds_trace && b && b->lpPairs) {
        char s[200]; int o = 0;
        for (DWORD i = 0; i < b->dwCount && i < 8; i++) o += snprintf(s + o, sizeof s - o, " %lu:%ld", b->lpPairs[i].dwMixBin, b->lpPairs[i].lVolume);
        DST("SetMixBins(%p)%s", p, s);
    }
    return DS_OK;
}
static HRESULT __stdcall hle_V_SetMixBinVolumes(void *p, const XDSMIXBINS *b)
{
    VOICE_OR_FAIL(p);
    if (g_old_mixbins || !b || !b->lpPairs) return DS_OK;
    EnterCriticalSection(&g_ds_lock);
    for (DWORD i = 0; i < b->dwCount; i++)
        for (int k = 0; k < v->nbins; k++) if (v->bins[k].dwMixBin == b->lpPairs[i].dwMixBin) v->bins[k].lVolume = b->lpPairs[i].lVolume;
    update_voice(v);
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_V_SetOutputBuffer(void *p, void *outbuf)
{
    VOICE_OR_FAIL(p);
    Voice *o = V(outbuf);
    EnterCriticalSection(&g_ds_lock);
    v->out = (o && o != v) ? o : NULL;
    LeaveCriticalSection(&g_ds_lock);
    DST("SetOutputBuffer(%p, %p)", p, outbuf);
    return DS_OK;
}
static HRESULT __stdcall hle_V_SetFormat(void *p, const XWAVEFORMATEX *f)
{
    VOICE_OR_FAIL(p);
    EnterCriticalSection(&g_ds_lock);
    set_format(v, f);
    if (!v->freq) v->pitch = (LONG)lround(4096.0 * log2(v->fmt.nSamplesPerSec ? v->fmt.nSamplesPerSec / 48000.0 : 1.0));
    update_voice(v);
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_V_GetVoiceProperties(void *p, XDSVOICEPROPS *vp)
{
    VOICE_OR_FAIL(p);
    memset(vp, 0, sizeof *vp);
    EnterCriticalSection(&g_ds_lock);
    vp->dwMixBinCount = v->nbins;
    memcpy(vp->pairs, v->bins, v->nbins * sizeof(MIXBINPAIR));
    vp->lPitch = v->pitch; vp->lVolume = v->volume - v->headroom;
    vp->lI3DL2DirectVolume = vp->lI3DL2RoomVolume = -10000;
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_V_Set3DVoiceData(void *p, const uint8_t *d)
{
    VOICE_OR_FAIL(p);
    if (!d) return DS_OK;
    DWORD m = *(const DWORD *)d;
    EnterCriticalSection(&g_ds_lock);
    *(DWORD *)v->d3 = m;
    static const struct { DWORD bit; int off, n; } f[] = {
        { 1, 4, 4 }, { 2, 8, 4 }, { 4, 0xC, 8 }, { 8, 0x14, 8 }, { 0x10, 0x1C, 8 }, { 0x20, 0x24, 4 }, { 0x40, 0x28, 8 }, { 0x80, 0x30, 8 } };
    for (int i = 0; i < 8; i++) if (m & f[i].bit) memcpy(v->d3 + f[i].off, d + f[i].off, f[i].n);
    v->has3d = 1;
    update_voice(v);
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_V_Use3DVoiceData(void *p, BOOL use)
{ VOICE_OR_FAIL(p); EnterCriticalSection(&g_ds_lock); v->use3d = use != 0; update_voice(v); LeaveCriticalSection(&g_ds_lock); return DS_OK; }
static HRESULT __stdcall hle_V_noop2(void *p, DWORD a) { return DS_OK; }
static HRESULT __stdcall hle_V_noop3(void *p, DWORD a, DWORD b) { return DS_OK; }
static HRESULT __stdcall hle_V_noop4(void *p, DWORD a, DWORD b, DWORD c) { return DS_OK; }
static HRESULT __stdcall hle_V_noop5(void *p, DWORD a, DWORD b, DWORD c, DWORD d) { return DS_OK; }

/* ---------------- IDirectSoundBuffer ---------------- */
static ULONG __stdcall hle_Buf_AddRef(void *p) { Voice *v = V(p); return v ? (ULONG)InterlockedIncrement(&v->refs) : 0; }
static ULONG __stdcall hle_Buf_Release(void *p) { return voice_release(V(p)); }
static HRESULT __stdcall hle_Buf_SetBufferData(void *p, void *data, DWORD bytes)
{
    VOICE_OR_FAIL(p);
    DST("SetBufferData(%p, %p, %lu)", p, data, bytes);
    EnterCriticalSection(&g_ds_lock);
    if (v->owned) { free(v->data); v->owned = 0; }
    v->data = data; v->bytes = data ? bytes : 0; v->pos = 0; v->cache_src = NULL;
    v->play_start = v->play_len = 0;
    if (v->loop_start + v->loop_len > v->bytes) v->loop_start = v->loop_len = 0;
    if (!v->data) v->playing = 0;
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_SetPlayRegion(void *p, DWORD start, DWORD len)
{
    VOICE_OR_FAIL(p);
    EnterCriticalSection(&g_ds_lock);
    v->play_start = start; v->play_len = len;
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_SetLoopRegion(void *p, DWORD start, DWORD len)
{
    VOICE_OR_FAIL(p);
    EnterCriticalSection(&g_ds_lock);
    v->loop_start = start; v->loop_len = len;
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_Lock(void *p, DWORD off, DWORD n, void **p1, DWORD *n1, void **p2, DWORD *n2, DWORD flags)
{
    Voice *v = V(p); if (!v || !v->data) { DST("Lock(%p) INVALID", p); return DSERR_INVALIDCALL; }
    if (flags & 2) { off = 0; n = v->bytes; }      /* DSBLOCK_ENTIREBUFFER */
    if (off >= v->bytes) off = 0;
    if (n > v->bytes) n = v->bytes;
    *p1 = v->data + off;
    if (off + n <= v->bytes) { *n1 = n; if (p2) *p2 = NULL; if (n2) *n2 = 0; }
    else { *n1 = v->bytes - off; if (p2) *p2 = v->data; if (n2) *n2 = n - *n1; }
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_Unlock(void *p, void *a, DWORD b_, void *c, DWORD d) { return DS_OK; }
static HRESULT __stdcall hle_Buf_Play(void *p, DWORD r1, DWORD r2, DWORD flags)
{
    VOICE_OR_FAIL(p);
    DST("Play(%p, flags %lx) bytes %u", p, flags, v->bytes);
    EnterCriticalSection(&g_ds_lock);
    if (flags & DSBPLAY_FROMSTART) v->pos = 0;
    if (!v->playing) v->paused = 0;            /* a stopped voice has no pause state; Play() leaves an existing one alone */
    if ((flags & 4) && !v->paused) v->paused = PAUSE_SYNCH;   /* DSBPLAY_SYNCHPLAYBACK: wait for SynchPlayback */
    v->looping = (flags & DSBPLAY_LOOPING) != 0; v->playing = 1; v->release = 0; v->fade = 0;
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_PlayEx(void *p, DWORD t_lo, DWORD t_hi, DWORD flags) { return hle_Buf_Play(p, 0, 0, flags); }
static HRESULT __stdcall hle_Buf_Stop(void *p)
{
    VOICE_OR_FAIL(p);
    DST("Stop(%p)", p);
    EnterCriticalSection(&g_ds_lock);
    if (v->playing) notify_stop(v);
    v->playing = 0;
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_StopEx(void *p, DWORD t_lo, DWORD t_hi, DWORD flags)
{
    VOICE_OR_FAIL(p);
    DST("StopEx(%p, %lx)", p, flags);
    if (flags & DSBSTOPEX_ENVELOPE) {               /* envelope release: a short fade-out */
        EnterCriticalSection(&g_ds_lock);
        if (v->playing && !v->paused) { if (!v->fade) v->fade = FADE_FRAMES; }
        else v->playing = 0;
        LeaveCriticalSection(&g_ds_lock);
        return DS_OK;
    }
    if (flags & DSBSTOPEX_RELEASEWAVEFORM) {        /* leave the loop, play to the end */
        EnterCriticalSection(&g_ds_lock); v->release = 1; LeaveCriticalSection(&g_ds_lock);
        return DS_OK;
    }
    return hle_Buf_Stop(p);
}
static HRESULT __stdcall hle_Buf_Pause(void *p, DWORD pause)
{
    /* DSBPAUSE_RESUME 0, _PAUSE 1, _SYNCHPLAYBACK 2 (paused until SynchPlayback);
     * like the library, it only affects a playing voice */
    VOICE_OR_FAIL(p);
    if (pause > 2) return E_FAIL_X;
    EnterCriticalSection(&g_ds_lock);
    if (v->playing) v->paused = pause == 1 ? PAUSE_APP : pause == 2 ? PAUSE_SYNCH : 0;
    LeaveCriticalSection(&g_ds_lock);
    DST("Pause(%p, %lu)", p, pause);
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_PauseEx(void *p, DWORD t_lo, DWORD t_hi, DWORD pause) { return hle_Buf_Pause(p, pause); }
static HRESULT __stdcall hle_Buf_GetStatus(void *p, DWORD *st)
{
    /* as the library reports it: a paused voice is PAUSED, not PLAYING|PAUSED */
    Voice *v = V(p);
    DWORD s = 0;
    if (v && v->playing) s = (v->paused ? DSBSTATUS_PAUSED : DSBSTATUS_PLAYING) | (v->looping && !v->release ? DSBSTATUS_LOOPING : 0);
    *st = s;
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_GetCurrentPosition(void *p, DWORD *play, DWORD *write)
{
    VOICE_OR_FAIL(p);
    EnterCriticalSection(&g_ds_lock);
    uint32_t by = frames_to_bytes(v, (uint32_t)v->pos);
    uint32_t span = v->play_len ? v->play_len : v->bytes;
    if (play) *play = by;
    if (write) { uint32_t w = by + frames_to_bytes(v, 1024); *write = span ? w % span : 0; }
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_SetCurrentPosition(void *p, DWORD pos)
{
    VOICE_OR_FAIL(p);
    EnterCriticalSection(&g_ds_lock);
    v->pos = bytes_to_frames(v, pos);
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_SetNotificationPositions(void *p, DWORD n, const XDSBPOSITIONNOTIFY *pn)
{
    VOICE_OR_FAIL(p);
    EnterCriticalSection(&g_ds_lock);
    v->nnotify = n > 16 ? 16 : (int)n;
    if (pn) memcpy(v->notify, pn, v->nnotify * sizeof *pn); else v->nnotify = 0;
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}

/* ---------------- IDirectSoundStream (XMediaObject) ---------------- */
static ULONG __stdcall hle_Str_AddRef(void *p) { Voice *v = V(p); return v ? (ULONG)InterlockedIncrement(&v->refs) : 0; }
static ULONG __stdcall hle_Str_Release(void *p) { return voice_release(V(p)); }
static HRESULT __stdcall hle_Str_GetInfo(void *p, XMEDIAINFO *mi)
{
    VOICE_OR_FAIL(p);
    mi->dwFlags = 0x1 | 0x2 | 0x4;      /* FIXED_SAMPLE_SIZE | FIXED_PACKET_ALIGNMENT | INPUT_ASYNC */
    mi->dwInputSize = v->frame_bytes;
    mi->dwOutputSize = 0;
    mi->dwMaxLookahead = v->adpcm ? 36 * 64 * v->ch : 0x1000 * v->frame_bytes;
    return DS_OK;
}
static HRESULT __stdcall hle_Str_GetStatus(void *p, DWORD *st)
{
    VOICE_OR_FAIL(p);
    EnterCriticalSection(&g_ds_lock);
    DWORD s = 0;
    if (v->qn < v->qmax) s |= DSSTREAMSTATUS_READY;
    if (v->env_hold) s |= DSSTREAMSTATUS_ENVELOPE;
    if (v->playing) {                   /* a starved voice is paused by the hardware */
        int starved = v->starved && !v->qn;
        if (v->paused || starved) s |= DSSTREAMSTATUS_PAUSED | (starved ? DSSTREAMSTATUS_STARVED : 0);
        else s |= DSSTREAMSTATUS_PLAYING;
    }
    LeaveCriticalSection(&g_ds_lock);
    *st = s;
    return DS_OK;
}
static HRESULT __stdcall hle_Str_Process(void *p, const XMEDIAPACKET *in, const XMEDIAPACKET *out)
{
    VOICE_OR_FAIL(p);
    if (!in) return DSERR_INVALIDCALL;
    EnterCriticalSection(&g_ds_lock);
    if (v->qn >= MAX_PACKETS) { LeaveCriticalSection(&g_ds_lock); xlog("dsound: stream %p packet queue full", p); return DSERR_INVALIDCALL; }
    Packet *k = &v->q[(v->qhead + v->qn) % MAX_PACKETS];
    k->p = *in; k->done = 0;
    v->qn++;
    if (in->pdwStatus) *in->pdwStatus = E_PENDING_X;
    if (in->pdwCompletedSize) *in->pdwCompletedSize = 0;
    if (!v->env_hold) v->discontinuity = 0;
    stream_service(v);
    LeaveCriticalSection(&g_ds_lock);
    DST("Stream_Process(%p, %p, %lu bytes) queued %d%s [%p]", p, in->pvBuffer, in->dwMaxSize, v->qn,
        v->env_hold ? " (held)" : v->noact ? " (no activate)" : "", __builtin_return_address(0));
    return DS_OK;
}
static HRESULT __stdcall hle_Str_Discontinuity(void *p)
{
    /* stop once the queued packets have played; with none queued, stop now */
    VOICE_OR_FAIL(p);
    DST("Stream_Discontinuity(%p)%s", p, v->qn ? "" : " (empty: stop)");
    EnterCriticalSection(&g_ds_lock);
    if (v->qn) v->discontinuity = 1;
    else if (v->playing) stream_flush(v);
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_Str_Flush(void *p)
{ VOICE_OR_FAIL(p); DST("Stream_Flush(%p)", p); EnterCriticalSection(&g_ds_lock); stream_flush(v); LeaveCriticalSection(&g_ds_lock); return DS_OK; }
static HRESULT __stdcall hle_Str_FlushEx(void *p, DWORD t_lo, DWORD t_hi, DWORD flags)
{
    /* DSSTREAMFLUSHEX_ASYNC 1 (no difference here), _ENVELOPE 4: release, then
     * flush and hold (see stream_release_done); like the library, nothing
     * happens unless the voice is running. A timestamp is not honoured. */
    VOICE_OR_FAIL(p);
    if (!flags) return hle_Str_Flush(p);
    EnterCriticalSection(&g_ds_lock);
    DST("Stream_FlushEx(%p, %lx)%s", p, flags, v->playing ? "" : " (not running)");
    if (v->playing) {
        if (flags & (DSSTREAMFLUSHEX_ENVELOPE | DSSTREAMFLUSHEX_ENVELOPE2)) {
            v->flush_on_release = (flags & DSSTREAMFLUSHEX_ENVELOPE) != 0;
            if (!v->fade) v->fade = v->qn ? FADE_FRAMES : 1;   /* (a paused voice holds its release, as on the APU) */
        } else stream_stop(v);
        if (!(flags & DSSTREAMFLUSHEX_ENVELOPE)) stream_flush(v);
    }
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_Str_Pause(void *p, DWORD pause)
{
    /* DSSTREAMPAUSE_RESUME 0, _PAUSE 1, _SYNCHPLAYBACK 2, _PAUSENOACTIVATE 3.
     * XACT primes the next streamed sound with 3 (packets queue up, the voice
     * does not start) and starts it later with 0. */
    VOICE_OR_FAIL(p);
    if (pause > 3) return E_FAIL_X;
    EnterCriticalSection(&g_ds_lock);
    switch (pause) {
    case 0: v->paused = 0; if (v->noact) { v->noact = 0; stream_service(v); } break;
    case 1: v->paused = PAUSE_APP; break;
    case 2: v->paused = PAUSE_SYNCH; break;
    case 3: if (v->playing) v->paused = 0; else v->noact = 1; break;
    }
    LeaveCriticalSection(&g_ds_lock);
    DST("Stream_Pause(%p, %lu) [%p]", p, pause, __builtin_return_address(0));
    return DS_OK;
}
static HRESULT __stdcall hle_Str_PauseEx(void *p, DWORD t_lo, DWORD t_hi, DWORD pause) { return hle_Str_Pause(p, pause); }

static const void *const g_stream_vtbl[13] = {
    hle_Str_AddRef, hle_Str_Release, hle_Str_GetInfo, hle_Str_GetStatus, hle_Str_Process, hle_Str_Discontinuity, hle_Str_Flush,
    hle_V_SetFormat, hle_V_SetFrequency, hle_V_SetVolume, hle_V_SetPitch, hle_V_noop2, hle_V_noop2,
};

static HRESULT __stdcall hle_DirectSoundCreateStream(XDSSTREAMDESC *desc, void **out)
{
    ds_start();
    Voice *v = voice_new(K_STREAM, desc->lpwfxFormat, desc->lpMixBins);
    v->vtbl = g_stream_vtbl;
    v->qmax = desc->dwMaxAttachedPackets ? (desc->dwMaxAttachedPackets > MAX_PACKETS ? MAX_PACKETS : (int)desc->dwMaxAttachedPackets) : 8;
    v->callback = desc->lpfnCallback; v->cb_ctx = desc->lpvContext;
    *out = v;
    DST("CreateSoundStream -> %p fmt %u ch %u rate %lu bits %u packets %lu flags %lx callback %p bins %d", v, v->fmt.wFormatTag,
        v->fmt.nChannels, v->fmt.nSamplesPerSec, v->fmt.wBitsPerSample, desc->dwMaxAttachedPackets, desc->dwFlags, desc->lpfnCallback, v->nbins);
    return DS_OK;
}
static HRESULT __stdcall hle_IDirectSound_CreateSoundStream(void *ds, XDSSTREAMDESC *desc, void **out, void *unk)
{ return hle_DirectSoundCreateStream(desc, out); }

void dsound_install(void)
{
    InitializeCriticalSection(&g_ds_lock);
    /* XDK 4134 (JSRF) has mix bin masks instead of DSMIXBINS structures */
    g_old_mixbins = !sym("DSOUND__IDirectSoundBuffer_SetMixBinVolumes_8") && !sym("DSOUND__IDirectSoundStream_SetMixBinVolumes_8");
    struct { const char *name; void *fn; } t[] = {
        { "DirectSoundCreate", hle_DirectSoundCreate },
        { "DirectSoundCreateBuffer", hle_DirectSoundCreateBuffer },
        { "DirectSoundCreateStream", hle_DirectSoundCreateStream },
        { "DirectSoundDoWork", hle_DirectSoundDoWork },
        { "DirectSoundUseFullHRTF", hle_ds_void },
        { "DirectSoundUseLightHRTF", hle_ds_void },
        { "IDirectSound_AddRef", hle_IDirectSound_AddRef },
        { "IDirectSound_Release", hle_IDirectSound_Release },
        { "IDirectSound_GetCaps", hle_IDirectSound_GetCaps },
        { "IDirectSound_GetSpeakerConfig", hle_IDirectSound_GetSpeakerConfig },
        { "IDirectSound_DownloadEffectsImage", hle_IDirectSound_DownloadEffectsImage },
        { "IDirectSound_GetEffectData", hle_IDirectSound_GetEffectData },
        { "IDirectSound_SetEffectData", hle_IDirectSound_SetEffectData },
        { "IDirectSound_CommitEffectData", hle_ds_noop1 },
        { "IDirectSound_CreateSoundBuffer", hle_IDirectSound_CreateSoundBuffer },
        { "IDirectSound_CreateSoundStream", hle_IDirectSound_CreateSoundStream },
        { "IDirectSound_CommitDeferredSettings", hle_ds_noop1 },
        { "IDirectSound_SetMixBinHeadroom", hle_IDirectSound_SetMixBinHeadroom },
        { "IDirectSound_EnableHeadphones", hle_ds_noop2 },
        { "IDirectSound_GetOutputLevels", hle_IDirectSound_GetOutputLevels },
        { "IDirectSound_GetTime", hle_IDirectSound_GetTime },
        { "IDirectSound_SynchPlayback", hle_IDirectSound_SynchPlayback },
        { "IDirectSound_SetCooperativeLevel", hle_ds_noop3 },
        { "IDirectSound_SetAllParameters", hle_ds_noop3 },
        { "IDirectSound_SetDistanceFactor", hle_ds_noop3 },
        { "IDirectSound_SetDopplerFactor", hle_ds_noop3 },
        { "IDirectSound_SetOrientation", hle_ds_noop8 },
        { "IDirectSound_SetPosition", hle_ds_noop5 },
        { "IDirectSound_SetRolloffFactor", hle_ds_noop3 },
        { "IDirectSound_SetVelocity", hle_ds_noop5 },
        { "IDirectSound_SetI3DL2Listener", hle_ds_noop3 },
        /* buffers */
        { "IDirectSoundBuffer_AddRef", hle_Buf_AddRef },
        { "IDirectSoundBuffer_Release", hle_Buf_Release },
        { "IDirectSoundBuffer_SetBufferData", hle_Buf_SetBufferData },
        { "IDirectSoundBuffer_SetPlayRegion", hle_Buf_SetPlayRegion },
        { "IDirectSoundBuffer_SetLoopRegion", hle_Buf_SetLoopRegion },
        { "IDirectSoundBuffer_Lock", hle_Buf_Lock },
        { "IDirectSoundBuffer_Unlock", hle_Buf_Unlock },
        { "IDirectSoundBuffer_Play", hle_Buf_Play },
        { "IDirectSoundBuffer_PlayEx", hle_Buf_PlayEx },
        { "IDirectSoundBuffer_Stop", hle_Buf_Stop },
        { "IDirectSoundBuffer_StopEx", hle_Buf_StopEx },
        { "IDirectSoundBuffer_Pause", hle_Buf_Pause },
        { "IDirectSoundBuffer_PauseEx", hle_Buf_PauseEx },
        { "IDirectSoundBuffer_GetStatus", hle_Buf_GetStatus },
        { "IDirectSoundBuffer_GetCurrentPosition", hle_Buf_GetCurrentPosition },
        { "IDirectSoundBuffer_SetCurrentPosition", hle_Buf_SetCurrentPosition },
        { "IDirectSoundBuffer_SetNotificationPositions", hle_Buf_SetNotificationPositions },
        { "IDirectSoundBuffer_SetFrequency", hle_V_SetFrequency },
        { "IDirectSoundBuffer_SetVolume", hle_V_SetVolume },
        { "IDirectSoundBuffer_SetPitch", hle_V_SetPitch },
        { "IDirectSoundBuffer_SetHeadroom", hle_V_SetHeadroom },
        { "IDirectSoundBuffer_SetMixBins", hle_V_SetMixBins },
        { "IDirectSoundBuffer_SetMixBinVolumes_8", hle_V_SetMixBinVolumes },
        { "IDirectSoundBuffer_SetMixBinVolumes", hle_V_noop3 },
        { "IDirectSoundBuffer_SetOutputBuffer", hle_V_SetOutputBuffer },
        { "IDirectSoundBuffer_SetFormat", hle_V_SetFormat },
        { "IDirectSoundBuffer_GetVoiceProperties", hle_V_GetVoiceProperties },
        { "IDirectSoundBuffer_Set3DVoiceData", hle_V_Set3DVoiceData },
        { "IDirectSoundBuffer_Use3DVoiceData", hle_V_Use3DVoiceData },
        { "IDirectSoundBuffer_SetLFO", hle_V_noop2 },
        { "IDirectSoundBuffer_SetEG", hle_V_noop2 },
        { "IDirectSoundBuffer_SetFilter", hle_V_noop2 },
        { "IDirectSoundBuffer_SetAllParameters", hle_V_noop3 },
        { "IDirectSoundBuffer_SetConeAngles", hle_V_noop4 },
        { "IDirectSoundBuffer_SetConeOrientation", hle_V_noop5 },
        { "IDirectSoundBuffer_SetConeOutsideVolume", hle_V_noop3 },
        { "IDirectSoundBuffer_SetMaxDistance", hle_V_noop3 },
        { "IDirectSoundBuffer_SetMinDistance", hle_V_noop3 },
        { "IDirectSoundBuffer_SetMode", hle_V_noop3 },
        { "IDirectSoundBuffer_SetPosition", hle_V_noop5 },
        { "IDirectSoundBuffer_SetVelocity", hle_V_noop5 },
        { "IDirectSoundBuffer_SetDistanceFactor", hle_V_noop3 },
        { "IDirectSoundBuffer_SetDopplerFactor", hle_V_noop3 },
        { "IDirectSoundBuffer_SetRolloffFactor", hle_V_noop3 },
        { "IDirectSoundBuffer_SetRolloffCurve", hle_V_noop4 },
        { "IDirectSoundBuffer_SetI3DL2Source", hle_V_noop3 },
        /* streams (non-virtual methods; the virtual ones are in g_stream_vtbl) */
        { "IDirectSoundStream_SetFormat", hle_V_SetFormat },
        { "IDirectSoundStream_SetFrequency", hle_V_SetFrequency },
        { "IDirectSoundStream_SetVolume", hle_V_SetVolume },
        { "IDirectSoundStream_SetPitch", hle_V_SetPitch },
        { "IDirectSoundStream_SetHeadroom", hle_V_SetHeadroom },
        { "IDirectSoundStream_SetMixBins", hle_V_SetMixBins },
        { "IDirectSoundStream_SetMixBinVolumes_8", hle_V_SetMixBinVolumes },
        { "IDirectSoundStream_SetOutputBuffer", hle_V_SetOutputBuffer },
        { "IDirectSoundStream_GetVoiceProperties", hle_V_GetVoiceProperties },
        { "IDirectSoundStream_Set3DVoiceData", hle_V_Set3DVoiceData },
        { "IDirectSoundStream_Use3DVoiceData", hle_V_Use3DVoiceData },
        { "IDirectSoundStream_Pause", hle_Str_Pause },
        { "IDirectSoundStream_PauseEx", hle_Str_PauseEx },
        { "IDirectSoundStream_FlushEx", hle_Str_FlushEx },
        { "IDirectSoundStream_SetLFO", hle_V_noop2 },
        { "IDirectSoundStream_SetEG", hle_V_noop2 },
        { "IDirectSoundStream_SetFilter", hle_V_noop2 },
        { "IDirectSoundStream_SetAllParameters", hle_V_noop3 },
        { "IDirectSoundStream_SetConeAngles", hle_V_noop4 },
        { "IDirectSoundStream_SetConeOrientation", hle_V_noop5 },
        { "IDirectSoundStream_SetConeOutsideVolume", hle_V_noop3 },
        { "IDirectSoundStream_SetMaxDistance", hle_V_noop3 },
        { "IDirectSoundStream_SetMinDistance", hle_V_noop3 },
        { "IDirectSoundStream_SetMode", hle_V_noop3 },
        { "IDirectSoundStream_SetPosition", hle_V_noop5 },
        { "IDirectSoundStream_SetVelocity", hle_V_noop5 },
        { "IDirectSoundStream_SetDistanceFactor", hle_V_noop3 },
        { "IDirectSoundStream_SetDopplerFactor", hle_V_noop3 },
        { "IDirectSoundStream_SetRolloffFactor", hle_V_noop3 },
        { "IDirectSoundStream_SetRolloffCurve", hle_V_noop4 },
        { "IDirectSoundStream_SetI3DL2Source", hle_V_noop3 },
    };
    int n = 0;
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        char name[96]; snprintf(name, sizeof name, "DSOUND__%s", t[i].name);
        n += hle_patch_sym(name, t[i].fn, 0);
    }
    xlog("dsound: %d DirectSound entry points replaced%s", n, g_old_mixbins ? " (XDK 4134 mix bin masks ignored)" : "");
}
