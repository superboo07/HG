#include "runtime/ps2_audio.h"
#include "runtime/ps2_memory.h"
#include "ps2_host_backend.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <cstring>
#include <iostream>
#include <vector>

namespace
{
    struct SnddrvStreamState
    {
        std::mutex mutex;
        std::deque<int16_t> interleavedPcm;
        AudioStream stream{};
        uint64_t streamKey = 0u;
        uint32_t sampleRate = 0u;
        uint64_t submittedChunks = 0u;
        uint64_t callbackCallsSinceSubmission = 0u;
        uint64_t callbackFramesSinceSubmission = 0u;
        uint64_t underrunFramesSinceSubmission = 0u;
        std::chrono::steady_clock::time_point lastSubmissionTime{};
        uint64_t capturedFrames = 0u;
        std::FILE *captureFile = nullptr;
        uint64_t outputCapturedFrames = 0u;
        uint64_t outputCaptureFrameLimit = 0u;
        std::FILE *outputCaptureFile = nullptr;
        uint32_t resumeRampFramesRemaining = 0u;
        bool captureAttempted = false;
        bool outputCaptureAttempted = false;
        bool playing = false;
        bool valid = false;
    };

    std::atomic<SnddrvStreamState *> gPs2xSnddrvStreamState{nullptr};

    void ps2xSnddrvAudioCallback(void *buffer, unsigned int frames)
    {
        auto *output = static_cast<int16_t *>(buffer);
        const size_t sampleCount = static_cast<size_t>(frames) * 2u;
        SnddrvStreamState *state = gPs2xSnddrvStreamState.load(std::memory_order_acquire);
        if (!state)
        {
            std::fill_n(output, sampleCount, int16_t{0});
            return;
        }

        std::lock_guard<std::mutex> lock(state->mutex);
        size_t produced = 0u;
        while (produced < sampleCount && !state->interleavedPcm.empty())
        {
            output[produced++] = state->interleavedPcm.front();
            state->interleavedPcm.pop_front();
        }

        const size_t producedFrames = produced / 2u;
        if (state->resumeRampFramesRemaining != 0u)
        {
            constexpr uint32_t kResumeRampFrames = 64u;
            const size_t rampFrames = std::min<size_t>(
                producedFrames, state->resumeRampFramesRemaining);
            const uint32_t completed =
                kResumeRampFrames - state->resumeRampFramesRemaining;
            for (size_t frame = 0u; frame < rampFrames; ++frame)
            {
                const int32_t numerator = static_cast<int32_t>(completed + frame + 1u);
                output[frame * 2u] = static_cast<int16_t>(
                    static_cast<int32_t>(output[frame * 2u]) * numerator /
                    static_cast<int32_t>(kResumeRampFrames));
                output[frame * 2u + 1u] = static_cast<int16_t>(
                    static_cast<int32_t>(output[frame * 2u + 1u]) * numerator /
                    static_cast<int32_t>(kResumeRampFrames));
            }
            state->resumeRampFramesRemaining -= static_cast<uint32_t>(rampFrames);
        }

        if (produced < sampleCount && producedFrames != 0u)
        {
            // A short producer miss used to create an abrupt nonzero-to-zero
            // edge, followed by another hard edge when PCM resumed. Those
            // discontinuities are heard as crackles even though the submitted
            // PCM itself is clean. Apply a 64-frame (1.33 ms at 48 kHz)
            // de-click ramp at each edge while preserving the underrun's exact
            // duration and every real sample's ordering.
            constexpr size_t kUnderrunRampFrames = 64u;
            const size_t rampFrames = std::min(producedFrames, kUnderrunRampFrames);
            const size_t firstRampFrame = producedFrames - rampFrames;
            for (size_t frame = 0u; frame < rampFrames; ++frame)
            {
                const int32_t numerator = static_cast<int32_t>(rampFrames - frame - 1u);
                const size_t sample = (firstRampFrame + frame) * 2u;
                output[sample] = static_cast<int16_t>(
                    static_cast<int32_t>(output[sample]) * numerator /
                    static_cast<int32_t>(rampFrames));
                output[sample + 1u] = static_cast<int16_t>(
                    static_cast<int32_t>(output[sample + 1u]) * numerator /
                    static_cast<int32_t>(rampFrames));
            }
            state->resumeRampFramesRemaining = 64u;
        }
        std::fill(output + produced, output + sampleCount, int16_t{0});
        ++state->callbackCallsSinceSubmission;
        state->callbackFramesSinceSubmission += frames;
        state->underrunFramesSinceSubmission +=
            static_cast<uint64_t>((sampleCount - produced) / 2u);
        if (state->outputCaptureFile &&
            state->outputCapturedFrames < state->outputCaptureFrameLimit)
        {
            const size_t framesToWrite = static_cast<size_t>(std::min<uint64_t>(
                frames, state->outputCaptureFrameLimit - state->outputCapturedFrames));
            (void)std::fwrite(output, sizeof(int16_t) * 2u, framesToWrite,
                              state->outputCaptureFile);
            state->outputCapturedFrames += framesToWrite;
            if (state->outputCapturedFrames == state->outputCaptureFrameLimit)
            {
                std::fclose(state->outputCaptureFile);
                state->outputCaptureFile = nullptr;
            }
        }
    }

    std::vector<uint8_t> buildWavFromPcm(const int16_t *pcm, size_t sampleCount, uint32_t sampleRate)
    {
        const uint32_t dataSize = static_cast<uint32_t>(sampleCount * 2);
        const uint32_t fileSize = 36 + dataSize;
        std::vector<uint8_t> wav(8 + fileSize);

        uint8_t *p = wav.data();
        p[0] = 'R';
        p[1] = 'I';
        p[2] = 'F';
        p[3] = 'F';
        p[4] = static_cast<uint8_t>(fileSize);
        p[5] = static_cast<uint8_t>(fileSize >> 8);
        p[6] = static_cast<uint8_t>(fileSize >> 16);
        p[7] = static_cast<uint8_t>(fileSize >> 24);
        p[8] = 'W';
        p[9] = 'A';
        p[10] = 'V';
        p[11] = 'E';
        p[12] = 'f';
        p[13] = 'm';
        p[14] = 't';
        p[15] = ' ';
        p[16] = 16;
        p[17] = 0;
        p[18] = 0;
        p[19] = 0;
        p[20] = 1;
        p[21] = 0;
        p[22] = 1;
        p[23] = 0;
        p[24] = static_cast<uint8_t>(sampleRate);
        p[25] = static_cast<uint8_t>(sampleRate >> 8);
        p[26] = static_cast<uint8_t>(sampleRate >> 16);
        p[27] = static_cast<uint8_t>(sampleRate >> 24);
        const uint32_t byteRate = sampleRate * 2;
        p[28] = static_cast<uint8_t>(byteRate);
        p[29] = static_cast<uint8_t>(byteRate >> 8);
        p[30] = static_cast<uint8_t>(byteRate >> 16);
        p[31] = static_cast<uint8_t>(byteRate >> 24);
        p[32] = 2;
        p[33] = 0;
        p[34] = 16;
        p[35] = 0;
        p[36] = 'd';
        p[37] = 'a';
        p[38] = 't';
        p[39] = 'a';
        p[40] = static_cast<uint8_t>(dataSize);
        p[41] = static_cast<uint8_t>(dataSize >> 8);
        p[42] = static_cast<uint8_t>(dataSize >> 16);
        p[43] = static_cast<uint8_t>(dataSize >> 24);
        std::memcpy(p + 44, pcm, dataSize);
        return wav;
    }
}

namespace ps2_vag
{
    bool decode(const uint8_t *data, uint32_t sizeBytes,
                std::vector<int16_t> &outPcm, uint32_t &outSampleRate);
}

struct PS2AudioBackend::Impl
{
    struct TrackedSound
    {
        Sound snd;
        uint32_t sampleKey;
    };
    std::vector<TrackedSound> activeSounds;
    SnddrvStreamState snddrv;
};

PS2AudioBackend::PS2AudioBackend() : m_impl(std::make_unique<Impl>())
{
}

PS2AudioBackend::~PS2AudioBackend()
{
    if (m_impl)
        stopAll();
}

void PS2AudioBackend::onVagTransfer(const uint8_t *rdram, uint32_t srcAddr, uint32_t sizeBytes)
{
    if (!rdram || sizeBytes < 48)
        return;

    const uint32_t physAddr = srcAddr & PS2_RAM_MASK;
    if (physAddr + sizeBytes > PS2_RAM_SIZE)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(rdram + physAddr, sizeBytes, pcm, sampleRate))
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = std::move(sample);
    m_mostRecentSampleKey = physAddr;
}

void PS2AudioBackend::onVagTransferFromBuffer(const uint8_t *data, uint32_t sizeBytes, uint32_t keyAddr)
{
    if (!data || sizeBytes < 48)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(data, sizeBytes, pcm, sampleRate))
        return;

    const uint32_t physAddr = keyAddr & PS2_RAM_MASK;
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = sample;
    m_mostRecentSampleKey = physAddr;
    m_loadOrderSamples.push_back(std::move(sample));
    m_loadOrderSampleKeys.push_back(physAddr);
    constexpr size_t kMaxLoadOrderSamples = 32;
    if (m_loadOrderSamples.size() > kMaxLoadOrderSamples)
    {
        m_loadOrderSamples.erase(m_loadOrderSamples.begin());
        m_loadOrderSampleKeys.erase(m_loadOrderSampleKeys.begin());
    }
}

void PS2AudioBackend::onSnddrvPcm16Stereo(const uint8_t *firstChannel,
                                          const uint8_t *secondChannel,
                                          uint32_t bytesPerChannel,
                                          uint32_t sampleRate,
                                          uint64_t streamKey)
{
#if defined(PLATFORM_VITA)
    (void)firstChannel;
    (void)secondChannel;
    (void)bytesPerChannel;
    (void)sampleRate;
    (void)streamKey;
#else
    if (!m_audioReady || !firstChannel || !secondChannel || bytesPerChannel == 0u ||
        (bytesPerChannel & 1u) != 0u || sampleRate == 0u)
    {
        return;
    }

    SnddrvStreamState &stream = m_impl->snddrv;
    std::unique_lock<std::mutex> lock(stream.mutex);
    if (!stream.valid || stream.sampleRate != sampleRate)
    {
        if (stream.valid)
        {
            gPs2xSnddrvStreamState.store(nullptr, std::memory_order_release);
            const AudioStream previousStream = stream.stream;
            stream.valid = false;
            stream.playing = false;
            lock.unlock();
            StopAudioStream(previousStream);
            UnloadAudioStream(previousStream);
            lock.lock();
        }
        stream.stream = LoadAudioStream(sampleRate, 16u, 2u);
        stream.valid = IsAudioStreamValid(stream.stream);
        stream.sampleRate = sampleRate;
        stream.streamKey = 0u;
        stream.interleavedPcm.clear();
        stream.callbackCallsSinceSubmission = 0u;
        stream.callbackFramesSinceSubmission = 0u;
        stream.underrunFramesSinceSubmission = 0u;
        stream.lastSubmissionTime = {};
        stream.resumeRampFramesRemaining = 0u;
        if (!stream.valid)
            return;
        gPs2xSnddrvStreamState.store(&stream, std::memory_order_release);
        SetAudioStreamCallback(stream.stream, ps2xSnddrvAudioCallback);
        if (!stream.outputCaptureAttempted)
        {
            stream.outputCaptureAttempted = true;
            if (const char *capturePath = std::getenv("PS2X_SNDDRV_OUTPUT_CAPTURE"))
            {
                stream.outputCaptureFile = std::fopen(capturePath, "wb");
                uint64_t captureSeconds = 180u;
                if (const char *secondsText =
                        std::getenv("PS2X_SNDDRV_OUTPUT_CAPTURE_SECONDS"))
                {
                    const unsigned long long parsed = std::strtoull(secondsText, nullptr, 0);
                    if (parsed != 0u)
                        captureSeconds = std::min<uint64_t>(parsed, 600u);
                }
                stream.outputCaptureFrameLimit =
                    static_cast<uint64_t>(sampleRate) * captureSeconds;
                if (stream.outputCaptureFile)
                {
                    const auto unixMicroseconds =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
                    std::cerr << "[ps2-audio] output capture started unix_us="
                              << unixMicroseconds << " sample_rate=" << sampleRate
                              << " path=" << capturePath << '\n';
                }
            }
        }
    }

    if (stream.streamKey != streamKey)
    {
        stream.streamKey = streamKey;
        stream.interleavedPcm.clear();
    }

    const size_t frameCount = bytesPerChannel / sizeof(int16_t);
    std::vector<int16_t> firstPcm(frameCount);
    std::vector<int16_t> secondPcm(frameCount);
    for (size_t index = 0u; index < frameCount; ++index)
    {
        const size_t offset = index * 2u;
        firstPcm[index] = static_cast<int16_t>(
            static_cast<uint16_t>(firstChannel[offset]) |
            (static_cast<uint16_t>(firstChannel[offset + 1u]) << 8u));
        secondPcm[index] = static_cast<int16_t>(
            static_cast<uint16_t>(secondChannel[offset]) |
            (static_cast<uint16_t>(secondChannel[offset + 1u]) << 8u));
    }

    // Keep a bounded host queue. Eight seconds is enough to absorb scheduling
    // jitter while preventing a malformed producer from growing without limit.
    std::vector<int16_t> interleaved;
    interleaved.reserve(firstPcm.size() * 2u);
    int32_t peak = 0;
    for (size_t index = 0u; index < firstPcm.size(); ++index)
    {
        interleaved.push_back(firstPcm[index]);
        interleaved.push_back(secondPcm[index]);
        peak = std::max(peak, std::abs(static_cast<int32_t>(firstPcm[index])));
        peak = std::max(peak, std::abs(static_cast<int32_t>(secondPcm[index])));
    }

    if (!stream.captureAttempted)
    {
        stream.captureAttempted = true;
        if (const char *capturePath = std::getenv("PS2X_SNDDRV_PCM_CAPTURE"))
            stream.captureFile = std::fopen(capturePath, "wb");
    }
    constexpr uint64_t kCaptureSeconds = 30u;
    const uint64_t captureFrameLimit = static_cast<uint64_t>(sampleRate) * kCaptureSeconds;
    if (stream.captureFile && stream.capturedFrames < captureFrameLimit)
    {
        const size_t framesToWrite = static_cast<size_t>(std::min<uint64_t>(
            firstPcm.size(), captureFrameLimit - stream.capturedFrames));
        (void)std::fwrite(interleaved.data(), sizeof(int16_t) * 2u, framesToWrite,
                          stream.captureFile);
        stream.capturedFrames += framesToWrite;
        if (stream.capturedFrames == captureFrameLimit)
        {
            std::fclose(stream.captureFile);
            stream.captureFile = nullptr;
        }
    }

    ++stream.submittedChunks;
    if (std::getenv("PS2X_SNDDRV_TIMING_TRACE") != nullptr)
    {
        const auto now = std::chrono::steady_clock::now();
        const auto deltaMicroseconds = stream.lastSubmissionTime ==
                                               std::chrono::steady_clock::time_point{}
                                           ? 0
                                           : std::chrono::duration_cast<std::chrono::microseconds>(
                                                 now - stream.lastSubmissionTime)
                                                 .count();
        std::cerr << "[ps2-audio-timing] chunk=" << stream.submittedChunks
                  << " steady_us="
                  << std::chrono::duration_cast<std::chrono::microseconds>(
                         now.time_since_epoch())
                         .count()
                  << " delta_us=" << deltaMicroseconds
                  << " frames=" << frameCount
                  << " queue_before_frames=" << stream.interleavedPcm.size() / 2u
                  << " callbacks=" << stream.callbackCallsSinceSubmission
                  << " callback_frames=" << stream.callbackFramesSinceSubmission
                  << " underrun_frames=" << stream.underrunFramesSinceSubmission
                  << " stream_key=0x" << std::hex << stream.streamKey << std::dec
                  << '\n';
        stream.lastSubmissionTime = now;
        stream.callbackCallsSinceSubmission = 0u;
        stream.callbackFramesSinceSubmission = 0u;
        stream.underrunFramesSinceSubmission = 0u;
    }
    if (std::getenv("PS2X_SNDDRV_AUDIO_TRACE") &&
        (stream.submittedChunks <= 8u || (stream.submittedChunks % 100u) == 0u))
    {
        int32_t rawPcmPeak = 0;
        for (size_t offset = 0u; offset + 1u < bytesPerChannel; offset += 2u)
        {
            const int16_t sample = static_cast<int16_t>(
                static_cast<uint16_t>(firstChannel[offset]) |
                (static_cast<uint16_t>(firstChannel[offset + 1u]) << 8u));
            rawPcmPeak = std::max(rawPcmPeak, std::abs(static_cast<int32_t>(sample)));
        }
        const double queuedMilliseconds =
            static_cast<double>(stream.interleavedPcm.size()) * 500.0 /
            static_cast<double>(sampleRate);
        std::cerr << "[ps2-audio] snddrv chunk=" << stream.submittedChunks
                  << " pcm_bytes_per_channel=0x" << std::hex << bytesPerChannel
                  << std::dec << " frames=" << firstPcm.size()
                  << " peak=" << peak
                  << " raw_pcm_peak=" << rawPcmPeak
                  << " queued_ms=" << queuedMilliseconds << '\n';
    }

    const size_t maxQueuedSamples = static_cast<size_t>(sampleRate) * 2u * 8u;
    if (stream.interleavedPcm.size() + interleaved.size() > maxQueuedSamples)
    {
        stream.interleavedPcm.clear();
    }
    stream.interleavedPcm.insert(stream.interleavedPcm.end(),
                                 interleaved.begin(), interleaved.end());
    // Prime the callback with real PCM before starting the device. Raylib pulls
    // fixed 240-frame blocks and performs an initial device-fill burst. Waiting
    // for at least 50 ms gives that burst and one late block real samples while
    // adding only the time needed for the second already-scheduled refill.
    constexpr size_t kStartupPrebufferFrames = 2'400u;
    const bool startStream = !stream.playing &&
                             stream.interleavedPcm.size() / 2u >=
                                 kStartupPrebufferFrames;
    const AudioStream streamToStart = stream.stream;
    if (startStream)
        stream.playing = true;
    lock.unlock();
    if (startStream)
        PlayAudioStream(streamToStart);
#endif
}

namespace
{
    constexpr uint32_t LIBSD_CMD_SET_VOICE = 0x8010u;
}

void PS2AudioBackend::onSoundCommand(uint32_t sid, uint32_t rpcNum,
                                     const uint8_t *sendBuf, uint32_t sendSize,
                                     uint8_t *recvBuf, uint32_t recvSize)
{
    if (sid != 0x80000701u)
        return;

    if ((rpcNum == LIBSD_CMD_SET_VOICE || (rpcNum & 0xFF00u) == 0x8100u) &&
        sendBuf && sendSize >= 20)
    {
        uint32_t sampleAddr = 0;
        uint32_t voiceIndex = 0xFFFFFFFFu;
        for (int vo = 4; vo >= 0 && voiceIndex == 0xFFFFFFFFu; vo -= 4)
        {
            if (vo < static_cast<int>(sendSize))
            {
                uint32_t v = 0;
                std::memcpy(&v, sendBuf + vo, sizeof(v));
                if (v < 24u)
                    voiceIndex = v;
            }
        }

        constexpr uint32_t kMinPlausibleAddr = 0x1000u;
        for (int off = 12; off <= 24 && sampleAddr == 0; off += 4)
        {
            if (sendSize >= static_cast<uint32_t>(off + 4))
            {
                uint32_t cand = 0;
                std::memcpy(&cand, sendBuf + off, sizeof(cand));
                if (cand >= kMinPlausibleAddr && (cand <= PS2_RAM_MASK || (cand & ~PS2_RAM_MASK) == 0))
                    sampleAddr = cand;
            }
        }
        if (sampleAddr == 0)
            sampleAddr = m_mostRecentSampleKey;

        float pitch = 1.0f;
        if (sendSize >= 12)
        {
            uint16_t pitchHalf = 0;
            std::memcpy(&pitchHalf, sendBuf + 8, sizeof(pitchHalf));
            if (pitchHalf != 0)
                pitch = 4096.0f / static_cast<float>(pitchHalf);
        }
        play(sampleAddr, pitch, 1.0f, voiceIndex);
    }
}

void PS2AudioBackend::play(uint32_t sampleAddr, float pitch, float volume, uint32_t voiceIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample *sampleToPlay = nullptr;
    uint32_t sampleKey = 0;

    auto it = m_sampleBank.find(sampleAddr & PS2_RAM_MASK);
    if (it != m_sampleBank.end())
    {
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    else if (voiceIndex != 0xFFFFFFFFu &&
             voiceIndex < m_loadOrderSamples.size() &&
             voiceIndex < m_loadOrderSampleKeys.size())
    {
        sampleToPlay = &m_loadOrderSamples[voiceIndex];
        sampleKey = m_loadOrderSampleKeys[voiceIndex];
    }
    else
    {
        it = m_sampleBank.find(m_mostRecentSampleKey);
        if (it == m_sampleBank.end())
            return;
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    if (!sampleToPlay || sampleToPlay->pcm.empty())
        return;

    const bool isBgm = (sampleToPlay->pcm.size() > static_cast<size_t>(sampleToPlay->sampleRate * 5));
    playDecodedSample(sampleKey, *sampleToPlay, pitch, volume, isBgm);
}

void PS2AudioBackend::pruneFinishedSounds()
{
#if defined(PLATFORM_VITA)
    return;
#else
    auto &sounds = m_impl->activeSounds;
    auto it = sounds.begin();
    while (it != sounds.end())
    {
        if (!IsSoundPlaying(it->snd))
        {
            UnloadSound(it->snd);
            it = sounds.erase(it);
        }
        else
        {
            ++it;
        }
    }
#endif
}

void PS2AudioBackend::playDecodedSample(uint32_t sampleKey, DecodedSample &sample, float pitch, float volume,
                                        bool isBgm)
{
#if defined(PLATFORM_VITA)
    (void)sampleKey;
    (void)sample;
    (void)pitch;
    (void)volume;
    (void)isBgm;
    return;
#else
    if (!m_audioReady || sample.pcm.empty())
        return;

    pruneFinishedSounds();

    for (const auto &t : m_impl->activeSounds)
    {
        if (t.sampleKey == sampleKey && IsSoundPlaying(t.snd))
            return;
    }

    auto &sounds = m_impl->activeSounds;
    if (isBgm)
    {
        for (auto it = sounds.begin(); it != sounds.end();)
        {
            if (IsSoundPlaying(it->snd))
            {
                StopSound(it->snd);
                UnloadSound(it->snd);
                it = sounds.erase(it);
            }
            else
                ++it;
        }
    }

    constexpr int kMaxConcurrentSounds = 4;
    while (static_cast<int>(sounds.size()) >= kMaxConcurrentSounds)
    {
        StopSound(sounds.front().snd);
        UnloadSound(sounds.front().snd);
        sounds.erase(sounds.begin());
    }

    std::vector<uint8_t> wav = buildWavFromPcm(sample.pcm.data(), sample.pcm.size(), sample.sampleRate);
    Wave wave = LoadWaveFromMemory(".wav", wav.data(), static_cast<int>(wav.size()));
    if (wave.frameCount <= 0)
        return;
    Sound snd = LoadSoundFromWave(wave);
    UnloadWave(wave);
    SetSoundPitch(snd, pitch);
    SetSoundVolume(snd, volume);
    m_impl->activeSounds.push_back({snd, sampleKey});
    PlaySound(snd);
#endif
}

void PS2AudioBackend::stop(uint32_t voiceId)
{
    (void)voiceId;
}

void PS2AudioBackend::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(PLATFORM_VITA)
    return;
#else
    for (auto &t : m_impl->activeSounds)
    {
        StopSound(t.snd);
        UnloadSound(t.snd);
    }
    m_impl->activeSounds.clear();
    SnddrvStreamState &stream = m_impl->snddrv;
    AudioStream streamToUnload{};
    bool unloadStream = false;
    gPs2xSnddrvStreamState.store(nullptr, std::memory_order_release);
    {
        std::lock_guard<std::mutex> streamLock(stream.mutex);
        if (stream.valid)
        {
            streamToUnload = stream.stream;
            unloadStream = true;
            stream.valid = false;
            stream.playing = false;
        }
        stream.interleavedPcm.clear();
        stream.streamKey = 0u;
        stream.sampleRate = 0u;
        stream.submittedChunks = 0u;
        stream.callbackCallsSinceSubmission = 0u;
        stream.callbackFramesSinceSubmission = 0u;
        stream.underrunFramesSinceSubmission = 0u;
        stream.lastSubmissionTime = {};
        stream.capturedFrames = 0u;
        stream.outputCapturedFrames = 0u;
        stream.outputCaptureFrameLimit = 0u;
        stream.resumeRampFramesRemaining = 0u;
        if (stream.captureFile)
        {
            std::fclose(stream.captureFile);
            stream.captureFile = nullptr;
        }
        stream.captureAttempted = false;
        if (stream.outputCaptureFile)
        {
            std::fclose(stream.outputCaptureFile);
            stream.outputCaptureFile = nullptr;
        }
        stream.outputCaptureAttempted = false;
    }
    if (unloadStream)
    {
        StopAudioStream(streamToUnload);
        UnloadAudioStream(streamToUnload);
    }
#endif
}
