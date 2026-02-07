#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <thread>
#include <vector>

namespace
{
suno::Model modelFromIndex(int index)
{
    switch (index)
    {
    case 0: return suno::Model::V4;
    case 1: return suno::Model::V4_5;
    case 2: return suno::Model::V4_5PLUS;
    case 3: return suno::Model::V4_5ALL;
    case 4: return suno::Model::V5;
    default: return suno::Model::V4_5ALL;
    }
}
} // namespace

AceForgeSunoAudioProcessor::AceForgeSunoAudioProcessor()
    : AudioProcessor(BusesProperties()
#if !JucePlugin_IsMidiEffect
#if !JucePlugin_IsSynth
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
      )
{
    client_ = std::make_unique<suno::SunoClient>("");
    playbackBuffer_.resize(kPlaybackFifoFrames * 2, 0.0f);
    {
        juce::ScopedLock l(statusLock_);
        statusText_ = "Set API key and click Generate, or record and use Cover / Add Vocals.";
    }
}

AceForgeSunoAudioProcessor::~AceForgeSunoAudioProcessor()
{
    cancelPendingUpdate();
}

void AceForgeSunoAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);
    sampleRate_.store(sampleRate);
}

void AceForgeSunoAudioProcessor::releaseResources() {}

void AceForgeSunoAudioProcessor::setApiKey(const juce::String& key)
{
    apiKey_ = key.trim();
    if (client_)
        client_->setApiKey(apiKey_.toStdString());
    connected_.store(false);
    if (client_ && client_->hasApiKey() && client_->checkCredits())
        connected_.store(true);
}

void AceForgeSunoAudioProcessor::clearAllSegments()
{
    juce::ScopedLock l(segmentLock_);
    segments_.clear();
    currentSegmentBuffer_.clear();
    selectedSegmentIndex_.store(-1);
}

int AceForgeSunoAudioProcessor::getNumSegments() const
{
    juce::ScopedLock l(segmentLock_);
    return static_cast<int>(segments_.size());
}

AceForgeSunoAudioProcessor::RecordedSegment AceForgeSunoAudioProcessor::getSegment(int index) const
{
    juce::ScopedLock l(segmentLock_);
    if (index < 0 || index >= static_cast<int>(segments_.size()))
        return {};
    return segments_[static_cast<size_t>(index)];
}

double AceForgeSunoAudioProcessor::getSegmentDurationSeconds(int index) const
{
    auto seg = getSegment(index);
    const int numCh = 2;
    const int totalFrames = static_cast<int>(seg.buffer.size()) / numCh;
    if (totalFrames <= 0 || seg.sampleRate <= 0.0)
        return 0.0;
    int end = seg.trimEndSamples > 0 ? seg.trimEndSamples : totalFrames;
    int start = seg.trimStartSamples;
    int frames = std::max(0, end - start);
    return static_cast<double>(frames) / seg.sampleRate;
}

void AceForgeSunoAudioProcessor::setSegmentTrim(int index, int startSamples, int endSamples)
{
    juce::ScopedLock l(segmentLock_);
    if (index < 0 || index >= static_cast<int>(segments_.size()))
        return;
    auto& seg = segments_[static_cast<size_t>(index)];
    const int totalFrames = static_cast<int>(seg.buffer.size()) / 2;
    seg.trimStartSamples = juce::jlimit(0, totalFrames, startSamples);
    seg.trimEndSamples = (endSamples <= 0) ? 0 : juce::jlimit(0, totalFrames, endSamples);
}

void AceForgeSunoAudioProcessor::removeSegment(int index)
{
    juce::ScopedLock l(segmentLock_);
    if (index < 0 || index >= static_cast<int>(segments_.size()))
        return;
    segments_.erase(segments_.begin() + index);
    int sel = selectedSegmentIndex_.load();
    if (sel == index)
        selectedSegmentIndex_.store(-1);
    else if (sel > index)
        selectedSegmentIndex_.store(sel - 1);
}

bool AceForgeSunoAudioProcessor::hasRecordedAudio() const
{
    return hasSelectedSegment();
}

bool AceForgeSunoAudioProcessor::hasSelectedSegment() const
{
    int idx = selectedSegmentIndex_.load();
    if (idx < 0)
        return false;
    juce::ScopedLock l(segmentLock_);
    if (idx >= static_cast<int>(segments_.size()))
        return false;
    const auto& seg = segments_[static_cast<size_t>(idx)];
    const int totalFrames = static_cast<int>(seg.buffer.size()) / 2;
    if (totalFrames < 44100) // at least ~1 sec
        return false;
    int end = seg.trimEndSamples > 0 ? seg.trimEndSamples : totalFrames;
    return (end - seg.trimStartSamples) >= 44100;
}

std::vector<uint8_t> AceForgeSunoAudioProcessor::encodeSegmentAsWav(int segmentIndex) const
{
    RecordedSegment seg;
    {
        juce::ScopedLock l(segmentLock_);
        if (segmentIndex < 0 || segmentIndex >= static_cast<int>(segments_.size()))
            return {};
        seg = segments_[static_cast<size_t>(segmentIndex)];
    }
    const int numCh = 2;
    const int totalFrames = static_cast<int>(seg.buffer.size()) / numCh;
    if (totalFrames <= 0)
        return {};
    int start = seg.trimStartSamples;
    int end = seg.trimEndSamples > 0 ? seg.trimEndSamples : totalFrames;
    int numFrames = std::max(0, end - start);
    if (numFrames <= 0)
        return {};

    juce::AudioBuffer<float> buf(numCh, numFrames);
    for (int c = 0; c < numCh; ++c)
        for (int i = 0; i < numFrames; ++i)
            buf.setSample(c, i, seg.buffer[static_cast<size_t>(start + i) * 2u + static_cast<size_t>(c)]);

    juce::MemoryBlock block;
    juce::WavAudioFormat wavFormat;
    auto options = juce::AudioFormatWriterOptions{}
                      .withSampleRate(seg.sampleRate)
                      .withNumChannels(numCh)
                      .withBitsPerSample(24);
    std::unique_ptr<juce::OutputStream> mos = std::make_unique<juce::MemoryOutputStream>(block, true);
    if (auto writer = wavFormat.createWriterFor(mos, options))
    {
        writer->writeFromAudioSampleBuffer(buf, 0, numFrames);
        writer->flush();
        size_t sz = block.getSize();
        if (sz > 0 && block.getData() != nullptr)
            return std::vector<uint8_t>(static_cast<const uint8_t*>(block.getData()),
                                        static_cast<const uint8_t*>(block.getData()) + sz);
    }
    return {};
}

void AceForgeSunoAudioProcessor::startGenerate(const juce::String& prompt, const juce::String& style, const juce::String& title,
                                                bool customMode, bool instrumental, int modelIndex)
{
    State expected = state_.load();
    while (expected == State::Submitting || expected == State::Running)
        return;
    if (!state_.compare_exchange_strong(expected, State::Submitting))
        return;

    jobPrompt_ = prompt;
    jobStyle_ = style;
    jobTitle_ = title.isEmpty() ? "aceforge_suno" : title;
    jobCustomMode_ = customMode;
    jobInstrumental_ = instrumental;
    jobModelIndex_ = modelIndex;
    jobIsCover_ = false;
    jobIsAddVocals_ = false;
    triggerAsyncUpdate();
    std::thread t(&AceForgeSunoAudioProcessor::runGenerateThread, this);
    t.detach();
}

void AceForgeSunoAudioProcessor::startUploadCover(const juce::String& prompt, const juce::String& style, const juce::String& title,
                                                  bool customMode, bool instrumental, int modelIndex)
{
    int segIdx = selectedSegmentIndex_.load();
    if (!hasSelectedSegment() || segIdx < 0)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Select a recorded segment first (DAW Play then Stop to capture).";
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }
    State expected = state_.load();
    while (expected == State::Submitting || expected == State::Running)
        return;
    if (!state_.compare_exchange_strong(expected, State::Submitting))
        return;

    jobPrompt_ = prompt;
    jobStyle_ = style;
    jobTitle_ = title.isEmpty() ? "aceforge_suno_cover" : title;
    jobCustomMode_ = customMode;
    jobInstrumental_ = instrumental;
    jobModelIndex_ = modelIndex;
    jobIsCover_ = true;
    jobIsAddVocals_ = false;
    jobSegmentIndex_ = segIdx;
    triggerAsyncUpdate();
    std::thread t(&AceForgeSunoAudioProcessor::runUploadCoverThread, this);
    t.detach();
}

void AceForgeSunoAudioProcessor::startAddVocals(const juce::String& prompt, const juce::String& style, const juce::String& title)
{
    int segIdx = selectedSegmentIndex_.load();
    if (!hasSelectedSegment() || segIdx < 0)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Select a recorded segment first (DAW Play then Stop to capture instrumental).";
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }
    State expected = state_.load();
    while (expected == State::Submitting || expected == State::Running)
        return;
    if (!state_.compare_exchange_strong(expected, State::Submitting))
        return;

    jobPrompt_ = prompt;
    jobStyle_ = style;
    jobTitle_ = title.isEmpty() ? "aceforge_suno_vocals" : title;
    jobIsCover_ = false;
    jobIsAddVocals_ = true;
    jobSegmentIndex_ = segIdx;
    triggerAsyncUpdate();
    std::thread t(&AceForgeSunoAudioProcessor::runAddVocalsThread, this);
    t.detach();
}

void AceForgeSunoAudioProcessor::runGenerateThread()
{
    if (!client_ || !client_->hasApiKey())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "No API key";
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }
    if (!client_->checkCredits())
    {
        connected_.store(false);
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "API key invalid or no credits: " + juce::String(client_->lastError());
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }
    connected_.store(true);

    suno::GenerateParams p;
    p.prompt = jobPrompt_.toStdString();
    p.style = jobStyle_.toStdString();
    p.title = jobTitle_.toStdString();
    p.customMode = jobCustomMode_;
    p.instrumental = jobInstrumental_;
    p.model = modelFromIndex(jobModelIndex_);

    std::string taskId = client_->startGenerate(p);
    if (taskId.empty())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = juce::String(client_->lastError());
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }

    state_.store(State::Running);
    {
        juce::ScopedLock l(statusLock_);
        statusText_ = "Generating…";
    }
    triggerAsyncUpdate();

    while (true)
    {
        suno::TaskStatus st = client_->getTaskStatus(taskId);
        juce::String statusStr(st.status.c_str());
        if (statusStr.equalsIgnoreCase("SUCCESS") || statusStr.equalsIgnoreCase("success"))
        {
            if (st.audioUrls.empty())
            {
                state_.store(State::Failed);
                juce::ScopedLock l(statusLock_);
                lastError_ = "No audio URL in result";
                statusText_ = lastError_;
                triggerAsyncUpdate();
                return;
            }
            std::vector<uint8_t> wavBytes = client_->fetchAudio(st.audioUrls[0]);
            if (wavBytes.empty())
            {
                state_.store(State::Failed);
                juce::ScopedLock l(statusLock_);
                lastError_ = juce::String(client_->lastError());
                triggerAsyncUpdate();
                return;
            }
            {
                juce::ScopedLock l(pendingWavLock_);
                pendingWavBytes_ = std::move(wavBytes);
                pendingPrompt_ = jobPrompt_;
            }
            triggerAsyncUpdate();
            return;
        }
        if (statusStr.isNotEmpty() && (statusStr.contains("fail") || statusStr.contains("error")))
        {
            state_.store(State::Failed);
            juce::ScopedLock l(statusLock_);
            lastError_ = juce::String(st.errorMessage.empty() ? st.status.c_str() : st.errorMessage.c_str());
            statusText_ = lastError_;
            triggerAsyncUpdate();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
}

void AceForgeSunoAudioProcessor::runUploadCoverThread()
{
    if (!client_ || !client_->hasApiKey())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "No API key";
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }
    if (!client_->checkCredits())
    {
        connected_.store(false);
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "API key invalid or no credits";
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }
    connected_.store(true);

    std::vector<uint8_t> wavBytes = encodeSegmentAsWav(jobSegmentIndex_);
    if (wavBytes.empty())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Failed to encode selected segment as WAV";
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }

    state_.store(State::Running);
    { juce::ScopedLock l(statusLock_); statusText_ = "Uploading…"; }
    triggerAsyncUpdate();

    std::string uploadUrl = client_->uploadAudio(wavBytes, "recorded.wav");
    if (uploadUrl.empty())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = juce::String(client_->lastError());
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }

    suno::GenerateParams p;
    p.prompt = jobPrompt_.toStdString();
    p.style = jobStyle_.toStdString();
    p.title = jobTitle_.toStdString();
    p.customMode = jobCustomMode_;
    p.instrumental = jobInstrumental_;
    p.model = modelFromIndex(jobModelIndex_);

    std::string taskId = client_->startUploadCover(uploadUrl, p);
    if (taskId.empty())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = juce::String(client_->lastError());
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }

    { juce::ScopedLock l(statusLock_); statusText_ = "Generating cover…"; }
    triggerAsyncUpdate();

    while (true)
    {
        suno::TaskStatus st = client_->getTaskStatus(taskId);
        juce::String statusStr(st.status.c_str());
        if (statusStr.equalsIgnoreCase("SUCCESS") || statusStr.equalsIgnoreCase("success"))
        {
            if (st.audioUrls.empty())
            {
                state_.store(State::Failed);
                juce::ScopedLock l(statusLock_);
                lastError_ = "No audio URL in result";
                statusText_ = lastError_;
                triggerAsyncUpdate();
                return;
            }
            std::vector<uint8_t> audioBytes = client_->fetchAudio(st.audioUrls[0]);
            if (audioBytes.empty())
            {
                state_.store(State::Failed);
                juce::ScopedLock l(statusLock_);
                lastError_ = juce::String(client_->lastError());
                triggerAsyncUpdate();
                return;
            }
            {
                juce::ScopedLock l(pendingWavLock_);
                pendingWavBytes_ = std::move(audioBytes);
                pendingPrompt_ = jobPrompt_;
            }
            triggerAsyncUpdate();
            return;
        }
        if (statusStr.isNotEmpty() && (statusStr.contains("fail") || statusStr.contains("error")))
        {
            state_.store(State::Failed);
            juce::ScopedLock l(statusLock_);
            lastError_ = juce::String(st.errorMessage.empty() ? st.status.c_str() : st.errorMessage.c_str());
            statusText_ = lastError_;
            triggerAsyncUpdate();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
}

void AceForgeSunoAudioProcessor::runAddVocalsThread()
{
    if (!client_ || !client_->hasApiKey())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "No API key";
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }
    if (!client_->checkCredits())
    {
        connected_.store(false);
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "API key invalid or no credits";
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }
    connected_.store(true);

    std::vector<uint8_t> wavBytes = encodeSegmentAsWav(jobSegmentIndex_);
    if (wavBytes.empty())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Failed to encode selected segment as WAV";
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }

    state_.store(State::Running);
    { juce::ScopedLock l(statusLock_); statusText_ = "Uploading…"; }
    triggerAsyncUpdate();

    std::string uploadUrl = client_->uploadAudio(wavBytes, "instrumental.wav");
    if (uploadUrl.empty())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = juce::String(client_->lastError());
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }

    suno::AddVocalsParams p;
    p.uploadUrl = uploadUrl;
    p.prompt = jobPrompt_.toStdString();
    p.style = jobStyle_.toStdString();
    p.title = jobTitle_.toStdString();
    p.model = modelFromIndex(jobModelIndex_);

    std::string taskId = client_->startAddVocals(p);
    if (taskId.empty())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = juce::String(client_->lastError());
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }

    { juce::ScopedLock l(statusLock_); statusText_ = "Adding vocals…"; }
    triggerAsyncUpdate();

    while (true)
    {
        suno::TaskStatus st = client_->getTaskStatus(taskId);
        juce::String statusStr(st.status.c_str());
        if (statusStr.equalsIgnoreCase("SUCCESS") || statusStr.equalsIgnoreCase("success"))
        {
            if (st.audioUrls.empty())
            {
                state_.store(State::Failed);
                juce::ScopedLock l(statusLock_);
                lastError_ = "No audio URL in result";
                statusText_ = lastError_;
                triggerAsyncUpdate();
                return;
            }
            std::vector<uint8_t> audioBytes = client_->fetchAudio(st.audioUrls[0]);
            if (audioBytes.empty())
            {
                state_.store(State::Failed);
                juce::ScopedLock l(statusLock_);
                lastError_ = juce::String(client_->lastError());
                triggerAsyncUpdate();
                return;
            }
            {
                juce::ScopedLock l(pendingWavLock_);
                pendingWavBytes_ = std::move(audioBytes);
                pendingPrompt_ = jobPrompt_;
            }
            triggerAsyncUpdate();
            return;
        }
        if (statusStr.isNotEmpty() && (statusStr.contains("fail") || statusStr.contains("error")))
        {
            state_.store(State::Failed);
            juce::ScopedLock l(statusLock_);
            lastError_ = juce::String(st.errorMessage.empty() ? st.status.c_str() : st.errorMessage.c_str());
            statusText_ = lastError_;
            triggerAsyncUpdate();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
}

void AceForgeSunoAudioProcessor::startTestApi()
{
    State expected = state_.load();
    while (expected == State::Submitting || expected == State::Running)
        return;
    if (!state_.compare_exchange_strong(expected, State::Submitting))
        return;
    triggerAsyncUpdate();
    std::thread t(&AceForgeSunoAudioProcessor::runTestApiThread, this);
    t.detach();
}

void AceForgeSunoAudioProcessor::runTestApiThread()
{
    if (!client_ || !client_->hasApiKey())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "No API key";
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }
    if (!client_->checkCredits())
    {
        connected_.store(false);
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "API key invalid or no credits: " + juce::String(client_->lastError());
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }
    connected_.store(true);

    state_.store(State::Running);
    { juce::ScopedLock l(statusLock_); statusText_ = "Testing API (minimal generate)…"; }
    triggerAsyncUpdate();

    suno::GenerateParams p;
    p.prompt = "test";
    p.style = "instrumental";
    p.title = "api_test";
    p.customMode = false;
    p.instrumental = true;
    p.model = suno::Model::V4_5ALL;

    std::string taskId = client_->startGenerate(p);
    if (taskId.empty())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = juce::String(client_->lastError());
        statusText_ = lastError_;
        triggerAsyncUpdate();
        return;
    }

    while (true)
    {
        suno::TaskStatus st = client_->getTaskStatus(taskId);
        juce::String statusStr(st.status.c_str());
        if (statusStr.equalsIgnoreCase("SUCCESS") || statusStr.equalsIgnoreCase("success"))
        {
            if (st.audioUrls.empty())
            {
                state_.store(State::Failed);
                juce::ScopedLock l(statusLock_);
                lastError_ = "No audio URL in test result";
                statusText_ = lastError_;
                triggerAsyncUpdate();
                return;
            }
            std::vector<uint8_t> wavBytes = client_->fetchAudio(st.audioUrls[0]);
            if (wavBytes.empty())
            {
                state_.store(State::Failed);
                juce::ScopedLock l(statusLock_);
                lastError_ = juce::String(client_->lastError());
                triggerAsyncUpdate();
                return;
            }
            {
                juce::ScopedLock l(pendingWavLock_);
                pendingWavBytes_ = std::move(wavBytes);
                pendingPrompt_ = "API test";
                pendingIsTest_.store(true);
            }
            triggerAsyncUpdate();
            return;
        }
        if (statusStr.isNotEmpty() && (statusStr.contains("fail") || statusStr.contains("error")))
        {
            state_.store(State::Failed);
            juce::ScopedLock l(statusLock_);
            lastError_ = juce::String(st.errorMessage.empty() ? st.status.c_str() : st.errorMessage.c_str());
            statusText_ = lastError_;
            triggerAsyncUpdate();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
}

void AceForgeSunoAudioProcessor::pushSamplesToPlayback(const float* interleaved, int numFrames,
                                                       int sourceChannels, double sourceSampleRate)
{
    if (numFrames <= 0 || interleaved == nullptr)
        return;
    const double hostRate = sampleRate_.load(std::memory_order_relaxed);
    const double ratio = sourceSampleRate > 0.0 ? hostRate / sourceSampleRate : 1.0;
    const int outFrames = static_cast<int>(std::round(static_cast<double>(numFrames) * ratio));
    if (outFrames <= 0 || outFrames > kPlaybackFifoFrames)
        return;

    const int writeIdx = nextWriteIndex_.load(std::memory_order_relaxed);
    std::vector<float>& outBuf = pendingPlaybackBuffer_[writeIdx];
    outBuf.resize(static_cast<size_t>(outFrames) * 2u);
    float* out = outBuf.data();
    for (int i = 0; i < outFrames; ++i)
    {
        const double srcIdx = ratio > 0.0 ? (double)i / ratio : (double)i;
        const int i0 = std::min(std::max(0, static_cast<int>(srcIdx)), numFrames - 1);
        const int i1 = std::min(i0 + 1, numFrames - 1);
        const float t = static_cast<float>(srcIdx - std::floor(srcIdx));
        float l = 0.0f, r = 0.0f;
        if (sourceChannels >= 1)
        {
            l = interleaved[i0 * sourceChannels] * (1.0f - t) + interleaved[i1 * sourceChannels] * t;
            r = sourceChannels >= 2
                    ? interleaved[i0 * sourceChannels + 1] * (1.0f - t) + interleaved[i1 * sourceChannels + 1] * t
                    : l;
        }
        out[i * 2] = l;
        out[i * 2 + 1] = r;
    }
    pendingPlaybackFrames_.store(outFrames, std::memory_order_release);
    pendingPlaybackBufferIndex_.store(writeIdx, std::memory_order_release);
    pendingPlaybackReady_.store(true, std::memory_order_release);
    nextWriteIndex_.store(1 - writeIdx, std::memory_order_release);
}

void AceForgeSunoAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();

    // Host BPM and transport from playhead when available
    bool isPlaying = false;
    if (auto* playhead = getPlayHead())
    {
        juce::Optional<juce::AudioPlayHead::PositionInfo> pos = playhead->getPosition();
        if (pos)
        {
            if (pos->getBpm().hasValue())
                hostBpm_.store(*pos->getBpm());
            isPlaying = pos->getIsPlaying();
        }
    }

    // Transport-driven recording: play = start segment, stop = save segment
    {
        juce::ScopedLock l(segmentLock_);
        if (isPlaying && !wasPlaying_)
        {
            currentSegmentBuffer_.clear();
            transportRecording_.store(true);
        }
        else if (!isPlaying && wasPlaying_)
        {
            const int minSamples = 2 * 44100; // ~1 sec stereo at 44.1k
            if (static_cast<int>(currentSegmentBuffer_.size()) >= minSamples)
            {
                RecordedSegment seg;
                seg.buffer = currentSegmentBuffer_;
                seg.sampleRate = sampleRate_.load();
                seg.trimEndSamples = 0; // full length
                segments_.push_back(std::move(seg));
                selectedSegmentIndex_.store(static_cast<int>(segments_.size()) - 1);
            }
            currentSegmentBuffer_.clear();
            transportRecording_.store(false);
        }
        wasPlaying_ = isPlaying;

        if (isPlaying && numCh >= 2)
        {
            const size_t start = currentSegmentBuffer_.size();
            currentSegmentBuffer_.resize(start + static_cast<size_t>(numSamples) * 2u);
            for (int i = 0; i < numSamples; ++i)
            {
                currentSegmentBuffer_[start + static_cast<size_t>(i) * 2u] = buffer.getSample(0, i);
                currentSegmentBuffer_[start + static_cast<size_t>(i) * 2u + 1u] = buffer.getSample(1, i);
            }
        }
    }

    // Playback: same double-buffer / fifo pattern as AceForge
    if (pendingPlaybackReady_.exchange(false, std::memory_order_acq_rel))
    {
        const int N = pendingPlaybackFrames_.load(std::memory_order_acquire);
        const int bufIdx = pendingPlaybackBufferIndex_.load(std::memory_order_acquire);
        const std::vector<float>& srcBuf = pendingPlaybackBuffer_[bufIdx];
        if (N > 0 && N <= kPlaybackFifoFrames && srcBuf.size() >= static_cast<size_t>(N) * 2u)
        {
            playbackFifo_.reset();
            int start1, block1, start2, block2;
            playbackFifo_.prepareToWrite(N, start1, block1, start2, block2);
            const float* src = srcBuf.data();
            auto copyBlock = [&](int fifoStart, int count, int srcOffset)
            {
                for (int i = 0; i < count && (fifoStart + i) * 2 + 1 < static_cast<int>(playbackBuffer_.size()); ++i)
                {
                    const int s = (srcOffset + i) * 2;
                    playbackBuffer_[static_cast<size_t>(fifoStart + i) * 2u] = src[s];
                    playbackBuffer_[static_cast<size_t>(fifoStart + i) * 2u + 1u] = src[s + 1];
                }
            };
            copyBlock(start1, block1, 0);
            copyBlock(start2, block2, block1);
            playbackFifo_.finishedWrite(block1 + block2);
        }
    }

    int start1, block1, start2, block2;
    playbackFifo_.prepareToRead(numSamples, start1, block1, start2, block2);
    auto readFrames = [&](int fifoStart, int count, int bufferOffset)
    {
        for (int i = 0; i < count; ++i)
        {
            const size_t base = static_cast<size_t>(fifoStart + i) * 2u;
            if (base + 1 < playbackBuffer_.size() && bufferOffset + i < numSamples)
            {
                buffer.setSample(0, bufferOffset + i, playbackBuffer_[base]);
                buffer.setSample(1, bufferOffset + i, playbackBuffer_[base + 1]);
            }
        }
    };
    readFrames(start1, block1, 0);
    readFrames(start2, block2, block1);
    playbackFifo_.finishedRead(block1 + block2);

    const int readCount = block1 + block2;
    for (int i = readCount; i < numSamples; ++i)
    {
        buffer.setSample(0, i, 0.0f);
        buffer.setSample(1, i, 0.0f);
    }
}

void AceForgeSunoAudioProcessor::handleAsyncUpdate()
{
    std::vector<uint8_t> wavBytes;
    juce::String promptForLibrary;
    bool isTest = false;
    {
        juce::ScopedLock l(pendingWavLock_);
        if (pendingWavBytes_.empty())
            return;
        wavBytes = std::move(pendingWavBytes_);
        pendingWavBytes_.clear();
        promptForLibrary = pendingPrompt_;
        isTest = pendingIsTest_.exchange(false);
    }

    juce::AudioFormatManager fm;
    fm.registerFormat(new juce::WavAudioFormat(), true);
    std::unique_ptr<juce::MemoryInputStream> mis(new juce::MemoryInputStream(wavBytes.data(), wavBytes.size(), false));
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(std::move(mis)));
    if (!reader)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Failed to decode WAV";
        statusText_ = lastError_;
        return;
    }
    const double fileSampleRate = reader->sampleRate;
    const int numCh = static_cast<int>(reader->numChannels);
    const int numSamples = static_cast<int>(reader->lengthInSamples);
    if (numSamples <= 0 || numCh <= 0)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Invalid WAV";
        statusText_ = lastError_;
        return;
    }
    juce::AudioBuffer<float> fileBuffer(numCh, numSamples);
    if (!reader->read(&fileBuffer, 0, numSamples, 0, true, true))
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Failed to read WAV samples";
        statusText_ = lastError_;
        return;
    }
    std::vector<float> interleaved(static_cast<size_t>(numSamples) * 2u);
    for (int i = 0; i < numSamples; ++i)
    {
        interleaved[static_cast<size_t>(i) * 2u] = numCh > 0 ? fileBuffer.getSample(0, i) : 0.0f;
        interleaved[static_cast<size_t>(i) * 2u + 1u] = numCh > 1 ? fileBuffer.getSample(1, i) : interleaved[static_cast<size_t>(i) * 2u];
    }
    pushSamplesToPlayback(interleaved.data(), numSamples, 2, fileSampleRate);
    state_.store(State::Succeeded);
    {
        juce::ScopedLock l(statusLock_);
        statusText_ = isTest ? "API test passed - audio received and playing." : "Generated - playing.";
    }

    if (isTest)
        return; // don't save test audio to library

    juce::File libDir = getLibraryDirectory();
    juce::String baseName = "suno_" + juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
    juce::File wavFile = libDir.getChildFile(baseName + ".wav");
    std::unique_ptr<juce::OutputStream> outStream = wavFile.createOutputStream();
    if (outStream != nullptr)
    {
        juce::WavAudioFormat wavFormat;
        auto options = juce::AudioFormatWriterOptions{}
                          .withSampleRate(fileSampleRate)
                          .withNumChannels(numCh)
                          .withBitsPerSample(24);
        if (auto writer = wavFormat.createWriterFor(outStream, options))
        {
            writer->writeFromAudioSampleBuffer(fileBuffer, 0, numSamples);
            writer->flush();
        }
    }
}

juce::String AceForgeSunoAudioProcessor::getStatusText() const
{
    juce::ScopedLock l(statusLock_);
    return statusText_;
}

juce::String AceForgeSunoAudioProcessor::getLastError() const
{
    juce::ScopedLock l(statusLock_);
    return lastError_;
}

juce::File AceForgeSunoAudioProcessor::getLibraryDirectory() const
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                         .getChildFile("AceForgeSuno")
                         .getChildFile("Generations");
    if (!dir.exists())
        dir.createDirectory();
    return dir;
}

std::vector<AceForgeSunoAudioProcessor::LibraryEntry> AceForgeSunoAudioProcessor::getLibraryEntries() const
{
    juce::Array<juce::File> wavs;
    getLibraryDirectory().findChildFiles(wavs, juce::File::findFiles, false, "*.wav");
    std::vector<LibraryEntry> entries;
    for (const juce::File& f : wavs)
        entries.push_back({ f, f.getFileName().upToFirstOccurrenceOf(".", false, false), f.getLastModificationTime() });
    std::sort(entries.begin(), entries.end(),
              [](const LibraryEntry& a, const LibraryEntry& b) { return a.time > b.time; });
    return entries;
}

// --- Boilerplate ---
juce::AudioProcessorEditor* AceForgeSunoAudioProcessor::createEditor() { return new AceForgeSunoAudioProcessorEditor(*this); }
bool AceForgeSunoAudioProcessor::hasEditor() const { return true; }
const juce::String AceForgeSunoAudioProcessor::getName() const { return "AceForge-Suno"; }
bool AceForgeSunoAudioProcessor::acceptsMidi() const { return false; }
bool AceForgeSunoAudioProcessor::producesMidi() const { return false; }
bool AceForgeSunoAudioProcessor::isMidiEffect() const { return false; }
double AceForgeSunoAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int AceForgeSunoAudioProcessor::getNumPrograms() { return 1; }
int AceForgeSunoAudioProcessor::getCurrentProgram() { return 0; }
void AceForgeSunoAudioProcessor::setCurrentProgram(int index) { juce::ignoreUnused(index); }
const juce::String AceForgeSunoAudioProcessor::getProgramName(int index) { juce::ignoreUnused(index); return "Default"; }
void AceForgeSunoAudioProcessor::changeProgramName(int index, const juce::String& newName) { juce::ignoreUnused(index, newName); }
bool AceForgeSunoAudioProcessor::isBusesLayoutSupported(const juce::AudioProcessor::BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    return true;
}

void AceForgeSunoAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream stream(destData, true);
    stream.writeString(apiKey_);
}

void AceForgeSunoAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;
    juce::MemoryInputStream stream(data, static_cast<size_t>(sizeInBytes), false);
    apiKey_ = stream.readString();
    if (client_)
        client_->setApiKey(apiKey_.toStdString());
    if (client_ && client_->hasApiKey() && client_->checkCredits())
        connected_.store(true);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AceForgeSunoAudioProcessor();
}
