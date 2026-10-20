/*
 * audio_xaudio2.cpp - sound output with XAudio2 (Windows 10+), 44.1 kHz
 * 16-bit stereo.  XAudio2 is loaded at run time; without it the game is
 * silent.
 *
 * Two ways: audio_stream_start() runs a thread that keeps a few frames
 * queued, mixing each when the device needs it (real time, the default);
 * or the game hands over one frame per vertical blank (audio_submit()):
 * a few frames are kept queued; if the queue runs dry, the next frames
 * start it again, and if it grows (the PC's 60 Hz is a little faster
 * than the sound clock) a frame is dropped.
 */
#include <windows.h>
#include <xaudio2.h>
#include <string.h>
#include "platform/audio.h"

#define SAMPLE_RATE             44100
#define CHANNELS                2
#define RING_BUFFERS            16
#define MAX_FRAME_SAMPLES       2048
#define QUEUE_MAX               6       /* buffers; more: drop a frame */
#define STREAM_QUEUE            3       /* real time: frames kept queued (50 ms) */
#define STREAM_WAIT_MS          20      /* (woken up when a buffer has been played) */

typedef HRESULT(WINAPI *XAudio2CreateProc)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);

static HMODULE s_dll;
static IXAudio2 *s_xaudio;
static IXAudio2MasteringVoice *s_master;
static IXAudio2SourceVoice *s_source;
static int16_t s_ring[RING_BUFFERS][MAX_FRAME_SAMPLES * CHANNELS];
static int s_next;

static HANDLE s_stream_thread, s_buffer_played;
static volatile LONG s_streaming;
static AudioFill s_fill;
static int s_stream_frames;

/* wakes the stream thread when a buffer has been played */
struct VoiceCallback : IXAudio2VoiceCallback {
    void STDMETHODCALLTYPE OnBufferEnd(void *) override
    {
        if (s_buffer_played) {
            SetEvent(s_buffer_played);
        }
    }
    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void *) override {}
    void STDMETHODCALLTYPE OnLoopEnd(void *) override {}
    void STDMETHODCALLTYPE OnVoiceError(void *, HRESULT) override {}
};
static VoiceCallback s_callback;

int audio_init(void)
{
    if (s_source) {
        return 1;
    }
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    s_dll = LoadLibraryW(L"xaudio2_9.dll");
    if (!s_dll) {
        return 0;
    }
    XAudio2CreateProc create = (XAudio2CreateProc) (void *) GetProcAddress(s_dll, "XAudio2Create");
    if (!create || FAILED(create(&s_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR))) {
        return 0;
    }
    if (FAILED(s_xaudio->CreateMasteringVoice(&s_master, CHANNELS, SAMPLE_RATE, 0, NULL, NULL,
                                              AudioCategory_GameEffects))) {
        return 0;
    }
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = CHANNELS;
    format.nSamplesPerSec = SAMPLE_RATE;
    format.wBitsPerSample = 16;
    format.nBlockAlign = CHANNELS * 2;
    format.nAvgBytesPerSec = SAMPLE_RATE * format.nBlockAlign;
    if (FAILED(s_xaudio->CreateSourceVoice(&s_source, &format, 0, XAUDIO2_DEFAULT_FREQ_RATIO,
                                           &s_callback, NULL, NULL))) {
        return 0;
    }
    s_source->Start(0, XAUDIO2_COMMIT_NOW);
    return 1;
}

void audio_shutdown(void)
{
    audio_stream_stop();
    if (s_source) {
        s_source->Stop(0, XAUDIO2_COMMIT_NOW);
        s_source->DestroyVoice();
        s_source = NULL;
    }
    if (s_master) {
        s_master->DestroyVoice();
        s_master = NULL;
    }
    if (s_xaudio) {
        s_xaudio->Release();
        s_xaudio = NULL;
    }
}

static void queue_buffer(const int16_t *samples, int frames);

void audio_submit(const int16_t *samples, int frames)
{
    if (!s_source || frames <= 0 || frames > MAX_FRAME_SAMPLES) {
        return;
    }
    XAUDIO2_VOICE_STATE state;
    s_source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued >= QUEUE_MAX) {
        return;                         /* ahead of the sound clock: drop */
    }
    queue_buffer(samples, frames);
}

static void queue_buffer(const int16_t *samples, int frames)
{
    int16_t *buffer = s_ring[s_next];
    s_next = (s_next + 1) % RING_BUFFERS;
    if (samples) {
        memcpy(buffer, samples, (size_t) frames * CHANNELS * sizeof(int16_t));
    } else {
        s_fill(buffer, frames);
    }
    XAUDIO2_BUFFER b = {};
    b.AudioBytes = (UINT32) (frames * CHANNELS * sizeof(int16_t));
    b.pAudioData = (const BYTE *) buffer;
    s_source->SubmitSourceBuffer(&b, NULL);
}

static DWORD WINAPI stream_thread(void *)
{
    while (s_streaming) {
        XAUDIO2_VOICE_STATE state;
        s_source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        for (UINT32 queued = state.BuffersQueued; queued < STREAM_QUEUE && s_streaming; queued++) {
            queue_buffer(NULL, s_stream_frames);
        }
        WaitForSingleObject(s_buffer_played, STREAM_WAIT_MS);
    }
    return 0;
}

int audio_stream_start(AudioFill fill, int frames)
{
    if (!s_source || !fill || frames <= 0 || frames > MAX_FRAME_SAMPLES) {
        return 0;
    }
    audio_stream_stop();
    s_fill = fill;
    s_stream_frames = frames;
    if (!s_buffer_played) {
        s_buffer_played = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
    s_streaming = 1;
    s_stream_thread = CreateThread(NULL, 0, stream_thread, NULL, 0, NULL);
    if (!s_stream_thread) {
        s_streaming = 0;
        return 0;
    }
    SetThreadPriority(s_stream_thread, THREAD_PRIORITY_HIGHEST);
    return 1;
}

void audio_stream_stop(void)
{
    if (!s_stream_thread) {
        return;
    }
    s_streaming = 0;
    SetEvent(s_buffer_played);
    WaitForSingleObject(s_stream_thread, INFINITE);
    CloseHandle(s_stream_thread);
    s_stream_thread = NULL;
}

void audio_stop(void)
{
    if (s_source) {
        s_source->FlushSourceBuffers();
    }
}
