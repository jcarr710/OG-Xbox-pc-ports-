/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DirectSound HLE.
 *
 * The XDK DirectSound library programs the MCPX APU (voice processor + DSP)
 * directly. Instead of emulating that hardware, the public API is replaced:
 * buffers play straight out of game memory (as on the APU), PCM and Xbox
 * ADPCM are decoded in a software mixer, and the result goes to waveOut.
 * Entry points were located with XbSymbolDatabase (XDK 4134). */
#include "hle.h"
#include <mmsystem.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define MIX_RATE   48000
#define MIX_FRAMES 480          /* 10 ms blocks */
#define MIX_BUFS   4

#define DS_OK 0
#define DSERR_INVALIDCALL 0x88780032
#define DSBPLAY_LOOPING 1
#define DSBSTATUS_PLAYING 1
#define DSBSTATUS_LOOPING 4
#define WAVE_FORMAT_XBOX_ADPCM 0x0069

#pragma pack(push, 1)
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize; } XWAVEFORMATEX;
#pragma pack(pop)
typedef struct { DWORD dwSize, dwFlags, dwBufferBytes; XWAVEFORMATEX *lpwfxFormat; void *lpMixBins; DWORD dwInputMixBin; } XDSBUFFERDESC;

typedef struct DSBuf {
    uint32_t magic;
    LONG     refs;
    XWAVEFORMATEX fmt;
    uint8_t *data; uint32_t bytes; int owned;
    uint32_t loop_start, loop_len;     /* bytes */
    int      playing, looping;
    double   pos;                      /* in source frames */
    double   freq;                     /* Hz */
    float    gain;                     /* linear */
    LONG     volume_mb;
    struct DSBuf *next;
    /* ADPCM decode cache */
    int32_t  cache_block; int16_t cache[2][65];
} DSBuf;

#define DSBUF_MAGIC 0x46554253
static DSBuf *g_bufs;
static CRITICAL_SECTION g_ds_lock;
static int g_ds_started;
static struct { uint32_t magic; LONG refs; } g_ds = { 0x4E534453, 1 };
static uint8_t *g_effect_mem[64];
float g_master_volume = 1.0f;
int g_ds_trace;
#define DST(...) do { if (g_ds_trace) { static int _n; if (_n++ < 3000) xlog(__VA_ARGS__); } } while (0)

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
static void adpcm_decode_block(DSBuf *b, int32_t block)
{
    int ch = b->fmt.nChannels > 1 ? 2 : 1;
    const uint8_t *src = b->data + (uint32_t)block * 36 * ch;
    for (int c = 0; c < ch; c++) {
        int pred = (int16_t)(src[c * 4] | (src[c * 4 + 1] << 8));
        int idx = src[c * 4 + 2]; if (idx > 88) idx = 88;
        int16_t *out = b->cache[c];
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
                if (pred > 32767) pred = 32767; if (pred < -32768) pred = -32768;
                idx += ima_index[nib]; if (idx < 0) idx = 0; if (idx > 88) idx = 88;
                out[n++] = (int16_t)pred;
            }
        }
    }
    b->cache_block = block;
}

static uint32_t frame_bytes(DSBuf *b) { return b->fmt.nBlockAlign ? b->fmt.nBlockAlign : 1; }
static int is_adpcm(DSBuf *b) { return b->fmt.wFormatTag == WAVE_FORMAT_XBOX_ADPCM; }
static uint32_t total_frames(DSBuf *b)
{
    if (is_adpcm(b)) return (b->bytes / (36 * (b->fmt.nChannels > 1 ? 2 : 1))) * 65;
    return b->bytes / frame_bytes(b);
}
static uint32_t bytes_to_frames(DSBuf *b, uint32_t by)
{
    if (is_adpcm(b)) return (by / (36 * (b->fmt.nChannels > 1 ? 2 : 1))) * 65;
    return by / frame_bytes(b);
}
static uint32_t frames_to_bytes(DSBuf *b, uint32_t fr)
{
    if (is_adpcm(b)) return (fr / 65) * 36 * (b->fmt.nChannels > 1 ? 2 : 1);
    return fr * frame_bytes(b);
}

static void fetch(DSBuf *b, uint32_t frame, float *l, float *r)
{
    if (is_adpcm(b)) {
        int32_t blk = frame / 65;
        if (blk != b->cache_block) adpcm_decode_block(b, blk);
        int i = frame % 65;
        *l = b->cache[0][i] / 32768.0f;
        *r = (b->fmt.nChannels > 1 ? b->cache[1][i] : b->cache[0][i]) / 32768.0f;
        return;
    }
    const uint8_t *p = b->data + frame * frame_bytes(b);
    if (b->fmt.wBitsPerSample == 8) {
        *l = (p[0] - 128) / 128.0f;
        *r = b->fmt.nChannels > 1 ? (p[1] - 128) / 128.0f : *l;
    } else {
        *l = *(const int16_t *)p / 32768.0f;
        *r = b->fmt.nChannels > 1 ? *(const int16_t *)(p + 2) / 32768.0f : *l;
    }
}

/* ---------------- mixer ---------------- */
static void mix(float *out, int frames)
{
    memset(out, 0, frames * 2 * sizeof(float));
    EnterCriticalSection(&g_ds_lock);
    for (DSBuf *b = g_bufs; b; b = b->next) {
        if (!b->playing || !b->data || !b->bytes || !b->fmt.nSamplesPerSec) continue;
        uint32_t total = total_frames(b);
        if (!total) continue;
        uint32_t ls = bytes_to_frames(b, b->loop_start);
        uint32_t le = b->loop_len ? ls + bytes_to_frames(b, b->loop_len) : total;
        if (le > total) le = total;
        double step = b->freq / MIX_RATE;
        for (int i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)b->pos;
            uint32_t end = b->looping ? le : total;
            if (f >= end) {
                if (b->looping && le > ls) { b->pos = ls + (b->pos - end); f = (uint32_t)b->pos; if (f >= le) f = ls; }
                else { b->playing = 0; b->pos = 0; break; }
            }
            float l, r; fetch(b, f, &l, &r);
            out[i * 2] += l * b->gain;
            out[i * 2 + 1] += r * b->gain;
            b->pos += step;
        }
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
        if (wo) {
            int any = 0;
            for (int i = 0; i < MIX_BUFS; i++) if (hdr[i].dwFlags & WHDR_DONE) {
                mix(mixbuf, MIX_FRAMES);
                for (int k = 0; k < MIX_FRAMES * 2; k++) {
                    float v = mixbuf[k] * g_master_volume * 32767.0f;
                    pcm[i][k] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
                }
                hdr[i].dwFlags &= ~WHDR_DONE;
                waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
                any = 1;
            }
            if (!any) WaitForSingleObject(ev, 20);
        } else {
            /* no device: keep positions advancing in real time */
            QueryPerformanceCounter(&t);
            uint64_t want = (uint64_t)((double)(t.QuadPart - t0.QuadPart) / qf.QuadPart * MIX_RATE);
            while (mixed + MIX_FRAMES <= want) { mix(mixbuf, MIX_FRAMES); mixed += MIX_FRAMES; }
            Sleep(5);
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

static DSBuf *B(void *p) { DSBuf *b = p; return (b && b->magic == DSBUF_MAGIC) ? b : NULL; }
static float mb_to_gain(LONG mb) { if (mb <= -10000) return 0.0f; if (mb > 0) mb = 0; return powf(10.0f, mb / 2000.0f); }

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
static HRESULT __stdcall hle_IDirectSound_GetSpeakerConfig(void *ds, DWORD *cfg) { *cfg = 0; return DS_OK; }
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
static HRESULT __stdcall hle_ds_noop1(void *ds) { return DS_OK; }
static HRESULT __stdcall hle_ds_noop3(void *ds, DWORD a, DWORD b) { return DS_OK; }
static HRESULT __stdcall hle_ds_noop5(void *ds, DWORD a, DWORD b, DWORD c, DWORD e) { return DS_OK; }
static HRESULT __stdcall hle_ds_noop8(void *ds, DWORD a, DWORD b, DWORD c, DWORD e, DWORD f, DWORD g, DWORD h) { return DS_OK; }
static void __stdcall hle_DirectSoundDoWork(void) {}
static void __cdecl hle_ds_void(void) {}

static HRESULT __stdcall hle_IDirectSound_CreateSoundBuffer(void *ds, XDSBUFFERDESC *desc, void **out, void *unk)
{
    DSBuf *b = calloc(1, sizeof *b);
    b->magic = DSBUF_MAGIC; b->refs = 1; b->gain = 1.0f; b->cache_block = -1;
    if (desc->lpwfxFormat) b->fmt = *desc->lpwfxFormat;
    else { b->fmt.wFormatTag = 1; b->fmt.nChannels = 2; b->fmt.nSamplesPerSec = 48000; b->fmt.wBitsPerSample = 16; b->fmt.nBlockAlign = 4; }
    b->freq = b->fmt.nSamplesPerSec;
    if (desc->dwBufferBytes) { b->bytes = desc->dwBufferBytes; b->data = calloc(1, b->bytes); b->owned = 1; }
    EnterCriticalSection(&g_ds_lock);
    b->next = g_bufs; g_bufs = b;
    LeaveCriticalSection(&g_ds_lock);
    *out = b;
    DST("CreateSoundBuffer -> %p fmt %u ch %u rate %lu bits %u align %u bytes %lu flags %lx", b, b->fmt.wFormatTag, b->fmt.nChannels,
        b->fmt.nSamplesPerSec, b->fmt.wBitsPerSample, b->fmt.nBlockAlign, desc->dwBufferBytes, desc->dwFlags);
    return DS_OK;
}

/* ---------------- IDirectSoundBuffer ---------------- */
static ULONG __stdcall hle_Buf_AddRef(void *p) { DSBuf *b = B(p); return b ? ++b->refs : 0; }
static ULONG __stdcall hle_Buf_Release(void *p)
{
    DSBuf *b = B(p);
    if (!b) return 0;
    if (--b->refs > 0) return b->refs;
    EnterCriticalSection(&g_ds_lock);
    for (DSBuf **pp = &g_bufs; *pp; pp = &(*pp)->next) if (*pp == b) { *pp = b->next; break; }
    LeaveCriticalSection(&g_ds_lock);
    if (b->owned) free(b->data);
    b->magic = 0; free(b);
    return 0;
}
static HRESULT __stdcall hle_Buf_SetBufferData(void *p, void *data, DWORD bytes)
{
    DSBuf *b = B(p); if (!b) return DSERR_INVALIDCALL;
    DST("SetBufferData(%p, %p, %lu)", p, data, bytes);
    EnterCriticalSection(&g_ds_lock);
    if (b->owned) { free(b->data); b->owned = 0; }
    b->data = data; b->bytes = bytes; b->pos = 0; b->cache_block = -1;
    if (b->loop_start + b->loop_len > bytes) b->loop_start = b->loop_len = 0;
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_Lock(void *p, DWORD off, DWORD n, void **p1, DWORD *n1, void **p2, DWORD *n2, DWORD flags)
{
    DSBuf *b = B(p); if (!b || !b->data) { DST("Lock(%p) INVALID", p); return DSERR_INVALIDCALL; }
    DST("Lock(%p, off %lu, n %lu, flags %lx) bytes %u pos %.0f", p, off, n, flags, b->bytes, b->pos);
    if (flags & 2) { off = 0; n = b->bytes; }      /* DSBLOCK_ENTIREBUFFER */
    if (off >= b->bytes) off = 0;
    if (n > b->bytes) n = b->bytes;
    *p1 = b->data + off;
    if (off + n <= b->bytes) { *n1 = n; if (p2) *p2 = NULL; if (n2) *n2 = 0; }
    else { *n1 = b->bytes - off; if (p2) *p2 = b->data; if (n2) *n2 = n - *n1; }
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_Unlock(void *p, void *a, DWORD b_, void *c, DWORD d) { return DS_OK; }
static HRESULT __stdcall hle_Buf_Play(void *p, DWORD r1, DWORD r2, DWORD flags)
{
    DSBuf *b = B(p); if (!b) return DSERR_INVALIDCALL;
    DST("Play(%p, flags %lx)", p, flags);
    EnterCriticalSection(&g_ds_lock);
    b->looping = (flags & DSBPLAY_LOOPING) != 0; b->playing = 1;
    LeaveCriticalSection(&g_ds_lock);
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_Stop(void *p) { DSBuf *b = B(p); DST("Stop(%p)", p); if (b) b->playing = 0; return DS_OK; }
static HRESULT __stdcall hle_Buf_GetStatus(void *p, DWORD *st)
{ DSBuf *b = B(p); *st = b && b->playing ? (DSBSTATUS_PLAYING | (b->looping ? DSBSTATUS_LOOPING : 0)) : 0; DST("GetStatus(%p) -> %lx", p, *st); return DS_OK; }
static HRESULT __stdcall hle_Buf_GetCurrentPosition(void *p, DWORD *play, DWORD *write)
{
    DSBuf *b = B(p); if (!b) return DSERR_INVALIDCALL;
    uint32_t by = frames_to_bytes(b, (uint32_t)b->pos);
    DST("GetCurrentPosition(%p) -> %u (playing %d)", p, by, b->playing);
    if (play) *play = by;
    if (write) { uint32_t w = by + frames_to_bytes(b, 1024); *write = b->bytes ? w % b->bytes : 0; }
    return DS_OK;
}
static HRESULT __stdcall hle_Buf_SetCurrentPosition(void *p, DWORD pos)
{ DSBuf *b = B(p); DST("SetCurrentPosition(%p, %lu)", p, pos); if (b) { EnterCriticalSection(&g_ds_lock); b->pos = bytes_to_frames(b, pos); LeaveCriticalSection(&g_ds_lock); } return DS_OK; }
static HRESULT __stdcall hle_Buf_SetLoopRegion(void *p, DWORD start, DWORD len)
{ DSBuf *b = B(p); if (b) { b->loop_start = start; b->loop_len = len; } return DS_OK; }
static HRESULT __stdcall hle_Buf_SetFrequency(void *p, DWORD hz)
{ DSBuf *b = B(p); if (b) b->freq = hz ? hz : b->fmt.nSamplesPerSec; return DS_OK; }
static HRESULT __stdcall hle_Buf_SetVolume(void *p, LONG mb)
{ DSBuf *b = B(p); if (b) { b->volume_mb = mb; b->gain = mb_to_gain(mb); } return DS_OK; }
static HRESULT __stdcall hle_Buf_noop2(void *p, DWORD a) { return DS_OK; }
static HRESULT __stdcall hle_Buf_noop3(void *p, DWORD a, DWORD b) { return DS_OK; }
static HRESULT __stdcall hle_Buf_noop4(void *p, DWORD a, DWORD b, DWORD c) { return DS_OK; }
static HRESULT __stdcall hle_Buf_noop5(void *p, DWORD a, DWORD b, DWORD c, DWORD d) { return DS_OK; }

int hle_patch(uint32_t va, void *fn, const char *name, const char *expect, int nexpect);

void dsound_install(void)
{
    InitializeCriticalSection(&g_ds_lock);
    struct { uint32_t va; void *fn; const char *name; } t[] = {
        { 0x001a0d9c, hle_DirectSoundCreate, "DirectSoundCreate" },
        { 0x0019f260, hle_DirectSoundDoWork, "DirectSoundDoWork" },
        { 0x0019e523, hle_ds_void, "DirectSound HRTF setup" },
        { 0x0019e4c6, hle_IDirectSound_AddRef, "IDirectSound_AddRef" },
        { 0x0019e4dc, hle_IDirectSound_Release, "IDirectSound_Release" },
        { 0x0019f024, hle_IDirectSound_GetCaps, "IDirectSound_GetCaps" },
        { 0x0019f040, hle_IDirectSound_GetSpeakerConfig, "IDirectSound_GetSpeakerConfig" },
        { 0x0019f05c, hle_IDirectSound_DownloadEffectsImage, "IDirectSound_DownloadEffectsImage" },
        { 0x0019f083, hle_IDirectSound_GetEffectData, "IDirectSound_GetEffectData" },
        { 0x0019f0aa, hle_IDirectSound_SetEffectData, "IDirectSound_SetEffectData" },
        { 0x001a0be2, hle_IDirectSound_CreateSoundBuffer, "IDirectSound_CreateSoundBuffer" },
        { 0x0019fe28, hle_ds_noop1, "IDirectSound_CommitDeferredSettings" },
        { 0x001a081e, hle_ds_noop3, "IDirectSound_SetAllParameters" },
        { 0x001a083e, hle_ds_noop3, "IDirectSound_SetDistanceFactor" },
        { 0x001a0862, hle_ds_noop3, "IDirectSound_SetDopplerFactor" },
        { 0x001a0886, hle_ds_noop8, "IDirectSound_SetOrientation" },
        { 0x001a08d0, hle_ds_noop5, "IDirectSound_SetPosition" },
        { 0x001a0905, hle_ds_noop3, "IDirectSound_SetRolloffFactor" },
        { 0x001a0929, hle_ds_noop5, "IDirectSound_SetVelocity" },
        { 0x001a095e, hle_ds_noop3, "IDirectSound_SetI3DL2Listener" },
        { 0x0019e4f7, hle_Buf_AddRef, "IDirectSoundBuffer_AddRef" },
        { 0x0019e50d, hle_Buf_Release, "IDirectSoundBuffer_Release" },
        { 0x001a097e, hle_Buf_SetBufferData, "IDirectSoundBuffer_SetBufferData" },
        { 0x0019f230, hle_Buf_Lock, "IDirectSoundBuffer_Lock" },
        { 0x0019e4f2, hle_Buf_Unlock, "IDirectSoundBuffer_Unlock" },
        { 0x0019f17c, hle_Buf_Play, "IDirectSoundBuffer_Play" },
        { 0x0019f1a0, hle_Buf_Stop, "IDirectSoundBuffer_Stop" },
        { 0x0019f1d8, hle_Buf_GetStatus, "IDirectSoundBuffer_GetStatus" },
        { 0x0019f1f4, hle_Buf_GetCurrentPosition, "IDirectSoundBuffer_GetCurrentPosition" },
        { 0x0019f214, hle_Buf_SetCurrentPosition, "IDirectSoundBuffer_SetCurrentPosition" },
        { 0x0019f1b8, hle_Buf_SetLoopRegion, "IDirectSoundBuffer_SetLoopRegion" },
        { 0x0019fe40, hle_Buf_SetFrequency, "IDirectSoundBuffer_SetFrequency" },
        { 0x0019f0d4, hle_Buf_SetVolume, "IDirectSoundBuffer_SetVolume" },
        { 0x0019f0f0, hle_Buf_noop2, "IDirectSoundBuffer_SetLFO" },
        { 0x0019f10c, hle_Buf_noop2, "IDirectSoundBuffer_SetEG" },
        { 0x0019f128, hle_Buf_noop2, "IDirectSoundBuffer_SetFilter" },
        { 0x0019f144, hle_Buf_noop2, "IDirectSoundBuffer_SetHeadroom" },
        { 0x0019f160, hle_Buf_noop2, "IDirectSoundBuffer_SetMixBins" },
        { 0x0019fe5c, hle_Buf_noop3, "IDirectSoundBuffer_SetAllParameters" },
        { 0x0019fe7c, hle_Buf_noop4, "IDirectSoundBuffer_SetConeAngles" },
        { 0x0019fea0, hle_Buf_noop5, "IDirectSoundBuffer_SetConeOrientation" },
        { 0x0019fed5, hle_Buf_noop3, "IDirectSoundBuffer_SetConeOutsideVolume" },
        { 0x0019fef5, hle_Buf_noop3, "IDirectSoundBuffer_SetMaxDistance" },
        { 0x0019ff19, hle_Buf_noop3, "IDirectSoundBuffer_SetMinDistance" },
        { 0x0019ff3d, hle_Buf_noop3, "IDirectSoundBuffer_SetMode" },
        { 0x0019ff5d, hle_Buf_noop5, "IDirectSoundBuffer_SetPosition" },
        { 0x0019ff92, hle_Buf_noop5, "IDirectSoundBuffer_SetVelocity" },
        { 0x0019ffc7, hle_Buf_noop3, "IDirectSoundBuffer_SetDistanceFactor" },
        { 0x0019ffeb, hle_Buf_noop3, "IDirectSoundBuffer_SetDopplerFactor" },
        { 0x001a000f, hle_Buf_noop3, "IDirectSoundBuffer_SetRolloffFactor" },
        { 0x001a0033, hle_Buf_noop3, "IDirectSoundBuffer_SetI3DL2Source" },
    };
    int n = 0;
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) n += hle_patch(t[i].va, t[i].fn, t[i].name, NULL, 0);
    xlog("dsound: %d DirectSound entry points replaced", n);
}
