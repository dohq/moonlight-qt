#pragma once

#include <SDL.h>
#include <opus.h>

#include <atomic>
#include <vector>

// Captures the default input device, Opus-encodes it and hands each packet to
// moonlight-common-c, which encrypts and sends it to the host. It is only
// valid between LiStartConnection() and LiStopConnection().
class MicrophoneCapture
{
public:
    MicrophoneCapture() = default;
    ~MicrophoneCapture();

    bool start();
    void stop();

private:
    static void SDLCALL captureCallback(void* userdata, Uint8* stream, int len);
    void encodeAndSend(const Uint8* pcm, int len);
    void encodeFrame(const opus_int16* pcm);

    SDL_AudioDeviceID m_Device = 0;
    OpusEncoder* m_Encoder = nullptr;
    bool m_AudioSubsystemInited = false;
    std::atomic<uint32_t> m_SendFailures{0};
    // Only touched by the capture callback thread.
    std::vector<opus_int16> m_Pending;
};
