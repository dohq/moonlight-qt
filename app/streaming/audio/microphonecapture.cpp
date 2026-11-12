#include "microphonecapture.h"

#include <Limelight.h>

// Frame length that the host accepts (2.5-120 ms); 20 ms balances latency and overhead.
static const int kSampleRate = 48000;
static const int kFrameSamples = 960;
// Speech needs far less than the audio stream; this keeps packets well under the 1400 byte limit.
static const int kBitrate = 32000;
// SDL's PipeWire backend hands the callback only `samples` frames per graph cycle and drops the
// rest when that is smaller than PipeWire's quantum (1024 by default), losing ~6% of the audio at 960.
// Capture with a buffer larger than any common quantum and cut it into 20 ms frames ourselves.
static const int kCaptureBufferSamples = 2048;

MicrophoneCapture::~MicrophoneCapture()
{
    stop();
}

bool MicrophoneCapture::start()
{
    if (m_Device != 0) {
        return true;
    }

    if (!LiIsMicrophoneStreamActive()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Host did not negotiate microphone passthrough");
        return false;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Microphone: SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s", SDL_GetError());
        return false;
    }
    m_AudioSubsystemInited = true;

    int opusError;
    m_Encoder = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &opusError);
    if (m_Encoder == nullptr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Microphone: opus_encoder_create() failed: %d", opusError);
        stop();
        return false;
    }
    opus_encoder_ctl(m_Encoder, OPUS_SET_BITRATE(kBitrate));
    opus_encoder_ctl(m_Encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(m_Encoder, OPUS_SET_PACKET_LOSS_PERC(10));

    SDL_AudioSpec want = {};
    want.freq = kSampleRate;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = kCaptureBufferSamples;
    want.callback = captureCallback;
    want.userdata = this;

    SDL_AudioSpec have;
    // No allowed changes: SDL resamples/remixes so the callback always sees mono S16 at 48 kHz.
    m_Pending.clear();
    m_Pending.reserve(kCaptureBufferSamples + kFrameSamples);
    m_Device = SDL_OpenAudioDevice(nullptr, 1, &want, &have, 0);
    if (m_Device == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Microphone: SDL_OpenAudioDevice(capture) failed: %s", SDL_GetError());
        stop();
        return false;
    }

    SDL_PauseAudioDevice(m_Device, 0);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Microphone passthrough started");
    return true;
}

void MicrophoneCapture::stop()
{
    // Closing the device joins the callback thread, so the encoder can be freed afterwards.
    if (m_Device != 0) {
        SDL_CloseAudioDevice(m_Device);
        m_Device = 0;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Microphone passthrough stopped");
    }

    if (m_Encoder != nullptr) {
        opus_encoder_destroy(m_Encoder);
        m_Encoder = nullptr;
    }

    // Balanced against the init above so the audio renderer's own init/quit stays paired.
    if (m_AudioSubsystemInited) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        m_AudioSubsystemInited = false;
    }
}

void SDLCALL MicrophoneCapture::captureCallback(void* userdata, Uint8* stream, int len)
{
    static_cast<MicrophoneCapture*>(userdata)->encodeAndSend(stream, len);
}

void MicrophoneCapture::encodeAndSend(const Uint8* pcm, int len)
{
    const opus_int16* samples = reinterpret_cast<const opus_int16*>(pcm);
    m_Pending.insert(m_Pending.end(), samples, samples + len / (int)sizeof(opus_int16));

    size_t offset = 0;
    while (m_Pending.size() - offset >= (size_t)kFrameSamples) {
        encodeFrame(m_Pending.data() + offset);
        offset += kFrameSamples;
    }
    m_Pending.erase(m_Pending.begin(), m_Pending.begin() + offset);
}

void MicrophoneCapture::encodeFrame(const opus_int16* pcm)
{
    unsigned char packet[400];
    int encoded = opus_encode(m_Encoder, pcm, kFrameSamples, packet, sizeof(packet));
    if (encoded <= 0) {
        return;
    }

    // Send as captured: the host does not reorder, so batching would only add delay.
    if (LiSendMicrophoneOpusData(packet, encoded) < 0 && m_SendFailures++ == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Microphone: failed to send packet");
    }
}
