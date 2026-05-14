#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

static constexpr const char* BACKEND_URL = "http://127.0.0.1:8765/generate";

//==============================================================================
AbletonAIPluginAudioProcessor::AbletonAIPluginAudioProcessor()
    : AudioProcessor(BusesProperties()
          .withInput ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    for (int i = 0; i < MAX_FAUST_PARAMS; ++i)
        addParameter(faustParams_[i] = new DynamicFaustParameter(i));
}

AbletonAIPluginAudioProcessor::~AbletonAIPluginAudioProcessor()
{
    if (generationThread  != nullptr) generationThread ->stopThread(3000);
    if (activationThread_ != nullptr) activationThread_->stopThread(3000);
}

const juce::String AbletonAIPluginAudioProcessor::getName() const { return JucePlugin_Name; }

//==============================================================================
void AbletonAIPluginAudioProcessor::prepareToPlay(double sr, int)
{
    sampleRate_     = sr;
    tremoloPhase_   = 0.f;
    chorusPhase_    = 0.f;
    filterState_[0] = filterState_[1] = 0.f;

    // Delay: 2 s max
    const int maxDelay = (int)(2.0 * sr) + 1;
    for (auto& dl : delayLine_) { dl.buf.assign(maxDelay, 0.f); dl.pos = 0; }

    // Chorus: 50 ms max (centre 15 ms ± 10 ms depth)
    const int maxChorus = (int)(0.05 * sr) + 2;
    for (auto& cl : chorusLine_) { cl.buf.assign(maxChorus, 0.f); cl.pos = 0; }

    // Reverb — Freeverb tunings scaled from 44100 Hz reference
    static const int COMB_SZ[4]    = { 1557, 1617, 1491, 1422 };
    static const int ALLPASS_SZ[2] = {  556,  441 };
    const double ratio = sr / 44100.0;
    for (int ch = 0; ch < 2; ++ch)
    {
        const int spread = ch * 23;
        for (int c = 0; c < 4; ++c)
        {
            const int sz = (int)((COMB_SZ[c] + spread) * ratio) + 1;
            combFilters_[ch][c].buf.assign(sz, 0.f);
            combFilters_[ch][c].pos  = 0;
            combFilters_[ch][c].last = 0.f;
        }
        for (int a = 0; a < 2; ++a)
        {
            const int sz = (int)((ALLPASS_SZ[a] + spread) * ratio) + 1;
            allpassFilters_[ch][a].buf.assign(sz, 0.f);
            allpassFilters_[ch][a].pos = 0;
        }
    }
}

void AbletonAIPluginAudioProcessor::releaseResources()
{
    deleteFaustDsp(activeFaustDsp_, activeFaustFactory_);
    activeFaustDsp_     = nullptr;
    activeFaustFactory_ = nullptr;
    delete activeFaustZones_;
    activeFaustZones_ = nullptr;

    // Discard any pending DSP that never got swapped in.
    dsp*              p = pendingFaustDsp_    .exchange(nullptr);
    llvm_dsp_factory* f = pendingFaustFactory_.exchange(nullptr);
    FaustZoneCapture* z = pendingFaustZones_  .exchange(nullptr);
    deleteFaustDsp(p, f);
    delete z;
}

void AbletonAIPluginAudioProcessor::swapFaustDsp()
{
    dsp*              newDsp     = pendingFaustDsp_    .exchange(nullptr, std::memory_order_acq_rel);
    llvm_dsp_factory* newFactory = pendingFaustFactory_.exchange(nullptr, std::memory_order_acq_rel);
    FaustZoneCapture* newZones   = pendingFaustZones_  .exchange(nullptr, std::memory_order_acq_rel);

    dsp*              oldDsp     = activeFaustDsp_;
    llvm_dsp_factory* oldFactory = activeFaustFactory_;
    FaustZoneCapture* oldZones   = activeFaustZones_;

    activeFaustDsp_     = newDsp;
    activeFaustFactory_ = newFactory;
    activeFaustZones_   = newZones;

    // Delete old instances and configure new params off the audio thread.
    juce::MessageManager::callAsync([this, oldDsp, oldFactory, oldZones, newZones] {
        deleteFaustDsp(oldDsp, oldFactory);
        delete oldZones;

        // Wire up DynamicFaustParameter slots so the host sees the new knobs.
        const int n = (newZones != nullptr) ? (int)newZones->zones.size() : 0;
        for (int i = 0; i < MAX_FAUST_PARAMS; ++i)
        {
            if (i < n)
            {
                const auto& z = newZones->zones[i];
                faustParams_[i]->configure(z.label, z.min, z.max, z.init);
            }
            else
            {
                faustParams_[i]->deactivate();
            }
        }
        updateHostDisplay(juce::AudioProcessorListener::ChangeDetails()
                              .withParameterInfoChanged(true));
    });
}

void AbletonAIPluginAudioProcessor::deleteFaustDsp(dsp* d, llvm_dsp_factory* f)
{
    delete d;
    if (f) deleteDSPFactory(f);
}

//==============================================================================
void AbletonAIPluginAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                                  juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Swap in newly compiled FAUST DSP if the generation thread has one ready.
    if (pendingFaustDsp_.load(std::memory_order_acquire) != nullptr)
        swapFaustDsp();

    if (activeFaustDsp_ != nullptr)
    {
        // Push JUCE parameter values into the FAUST DSP zone pointers.
        if (activeFaustZones_ != nullptr)
        {
            const int n = (int)activeFaustZones_->zones.size();
            for (int i = 0; i < n && i < MAX_FAUST_PARAMS; ++i)
                *activeFaustZones_->zones[i].zone = faustParams_[i]->getFaustValue();
        }

        // Run the FAUST-generated effect.
        const int numCh  = buffer.getNumChannels();
        const int numSmp = buffer.getNumSamples();
        std::vector<float*> io(numCh);
        for (int c = 0; c < numCh; ++c) io[c] = buffer.getWritePointer(c);
        activeFaustDsp_->compute(numSmp, io.data(), io.data());
    }
    else
    {
        // Fallback C++ engine while no FAUST DSP is loaded.
        switch ((DspType)dspType.load())
        {
            case DspType::Saturation:  processSaturation(buffer);  break;
            case DspType::Bitcrusher:  processBitcrusher(buffer);  break;
            case DspType::Tremolo:     processTremolo(buffer);     break;
            case DspType::Filter:      processFilter(buffer);      break;
            case DspType::Delay:       processDelay(buffer);       break;
            case DspType::Chorus:      processChorus(buffer);      break;
            case DspType::Reverb:      processReverb(buffer);      break;
            default: break;
        }
    }
}

//==============================================================================
void AbletonAIPluginAudioProcessor::processSaturation(juce::AudioBuffer<float>& buf)
{
    const float drive  = dspDrive.load();
    const float blend  = dspBlend.load();
    const float output = dspOutput.load();

    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        float* data = buf.getWritePointer(ch);
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            const float x   = data[i];
            const float wet = std::tanh(x * drive);
            data[i] = (blend * wet + (1.f - blend) * x) * output;
        }
    }
}

void AbletonAIPluginAudioProcessor::processBitcrusher(juce::AudioBuffer<float>& buf)
{
    const float blend  = dspBlend.load();
    const float levels = std::pow(2.f, std::max(1.f, dspBits.load()));

    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        float* data = buf.getWritePointer(ch);
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            const float x   = data[i];
            const float wet = std::floor(x * levels) / levels;
            data[i] = blend * wet + (1.f - blend) * x;
        }
    }
}

void AbletonAIPluginAudioProcessor::processTremolo(juce::AudioBuffer<float>& buf)
{
    const float rate  = dspRate.load();
    const float depth = dspDepth.load();
    const float inc   = (float)(rate / sampleRate_);

    for (int i = 0; i < buf.getNumSamples(); ++i)
    {
        const float lfo = 0.5f * (1.f + std::sin(juce::MathConstants<float>::twoPi * tremoloPhase_));
        const float gain = 1.f - depth + depth * lfo;
        tremoloPhase_ += inc;
        if (tremoloPhase_ >= 1.f) tremoloPhase_ -= 1.f;

        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            buf.getWritePointer(ch)[i] *= gain;
    }
}

void AbletonAIPluginAudioProcessor::processFilter(juce::AudioBuffer<float>& buf)
{
    // One-pole IIR: coefficient derived from cutoff frequency
    const float cutoff  = dspCutoff.load();
    const bool  hipass  = dspHighpass.load() != 0;
    const float omega   = std::exp(-juce::MathConstants<float>::twoPi
                                   * (float)(cutoff / sampleRate_));
    const float a       = 1.f - omega; // lowpass coefficient

    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        float* data = buf.getWritePointer(ch);
        float  s    = filterState_[ch];
        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            s = a * data[i] + omega * s;         // one-pole lowpass
            data[i] = hipass ? (data[i] - s) : s;
        }
        filterState_[ch] = s;
    }
}

//==============================================================================
void AbletonAIPluginAudioProcessor::processDelay(juce::AudioBuffer<float>& buf)
{
    const float timeMs   = dspDelayTime.load();
    const float feedback = juce::jlimit(0.f, 0.95f, dspFeedback.load());
    const float blend    = dspBlend.load();
    const int   numCh    = std::min(buf.getNumChannels(), 2);

    for (int ch = 0; ch < numCh; ++ch)
    {
        auto& dl = delayLine_[ch];
        const int bufSize      = (int)dl.buf.size();
        const int delaySamples = juce::jlimit(1, bufSize - 1,
                                    (int)(timeMs * 0.001f * (float)sampleRate_));
        float* data = buf.getWritePointer(ch);

        for (int i = 0; i < buf.getNumSamples(); ++i)
        {
            const float dry     = data[i];
            const int   readPos = (dl.pos - delaySamples + bufSize) % bufSize;
            const float wet     = dl.buf[readPos];
            dl.buf[dl.pos]      = dry + wet * feedback;
            dl.pos              = (dl.pos + 1) % bufSize;
            data[i]             = dry * (1.f - blend) + wet * blend;
        }
    }
}

void AbletonAIPluginAudioProcessor::processChorus(juce::AudioBuffer<float>& buf)
{
    const float rate  = dspRate.load();
    const float depth = dspDepth.load();
    const float blend = dspBlend.load();
    const float inc   = (float)(rate / sampleRate_);

    const float centerSmp = 0.015f * (float)sampleRate_;          // 15 ms centre
    const float depthSmp  = depth * 0.010f * (float)sampleRate_;  // 0–10 ms swing

    const int numCh  = std::min(buf.getNumChannels(), 2);
    const int numSmp = buf.getNumSamples();

    for (int i = 0; i < numSmp; ++i)
    {
        const float lfo = std::sin(juce::MathConstants<float>::twoPi * chorusPhase_);
        chorusPhase_ += inc;
        if (chorusPhase_ >= 1.f) chorusPhase_ -= 1.f;

        const float delaySmp = centerSmp + lfo * depthSmp;
        const int   d0       = (int)delaySmp;
        const float frac     = delaySmp - (float)d0;

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto& cl = chorusLine_[ch];
            const int   sz  = (int)cl.buf.size();
            const float dry = buf.getReadPointer(ch)[i];

            cl.buf[cl.pos] = dry;

            const int   r0  = (cl.pos - d0 + sz) % sz;
            const int   r1  = (cl.pos - d0 - 1 + sz) % sz;
            const float wet = cl.buf[r0] * (1.f - frac) + cl.buf[r1] * frac;

            cl.pos = (cl.pos + 1) % sz;
            buf.getWritePointer(ch)[i] = dry * (1.f - blend) + wet * blend;
        }
    }
}

void AbletonAIPluginAudioProcessor::processReverb(juce::AudioBuffer<float>& buf)
{
    const float roomSize = dspRoomSize.load();
    const float blend    = dspBlend.load();
    const float feedback = 0.7f + roomSize * 0.28f;  // 0.70 – 0.98
    const float damp     = 0.4f;
    const float gain     = 0.015f;

    const int numCh  = std::min(buf.getNumChannels(), 2);
    const int numSmp = buf.getNumSamples();

    for (int ch = 0; ch < numCh; ++ch)
    {
        float* data = buf.getWritePointer(ch);
        for (int i = 0; i < numSmp; ++i)
        {
            const float dry   = data[i];
            const float input = dry * gain;
            float output      = 0.f;

            // 4 parallel comb filters (Freeverb style)
            for (int c = 0; c < 4; ++c)
            {
                auto& cf      = combFilters_[ch][c];
                const int sz  = (int)cf.buf.size();
                const float y = cf.buf[cf.pos];
                cf.last       = y * (1.f - damp) + cf.last * damp;
                cf.buf[cf.pos] = input + cf.last * feedback;
                cf.pos        = (cf.pos + 1) % sz;
                output        += y;
            }

            // 2 series allpass filters
            for (int a = 0; a < 2; ++a)
            {
                auto& ap         = allpassFilters_[ch][a];
                const int sz     = (int)ap.buf.size();
                const float bufO = ap.buf[ap.pos];
                const float apO  = -output + bufO;
                ap.buf[ap.pos]   = output + bufO * 0.5f;
                ap.pos           = (ap.pos + 1) % sz;
                output           = apO;
            }

            data[i] = dry * (1.f - blend) + output * 3.f * blend;
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* AbletonAIPluginAudioProcessor::createEditor()
{
    return new AbletonAIPluginAudioProcessorEditor(*this);
}

//==============================================================================
void AbletonAIPluginAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ScopedLock lock(stateLock);
    juce::MemoryOutputStream stream(destData, false);
    stream.writeString(lastFaustCode);
    stream.writeString(lastDeviceName);
    stream.writeString(lastPrompt);
    stream.writeInt(dspType.load());
    stream.writeFloat(dspDrive.load());
    stream.writeFloat(dspBlend.load());
    stream.writeFloat(dspOutput.load());
    stream.writeFloat(dspBits.load());
    stream.writeFloat(dspRate.load());
    stream.writeFloat(dspDepth.load());
    stream.writeFloat(dspCutoff.load());
    stream.writeInt(dspHighpass.load());
    stream.writeFloat(dspDelayTime.load());
    stream.writeFloat(dspFeedback.load());
    stream.writeFloat(dspRoomSize.load());

    // Layer history
    stream.writeInt(activeLayerIndex_);
    stream.writeInt((int)layers_.size());
    for (const auto& layer : layers_)
    {
        stream.writeString(layer.name);
        stream.writeString(layer.faustCode);
        stream.writeInt((int)layer.status);
    }
}

void AbletonAIPluginAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    juce::MemoryInputStream stream(data, (size_t)sizeInBytes, false);
    {
        juce::ScopedLock lock(stateLock);
        lastFaustCode  = stream.readString();
        lastDeviceName = stream.readString();
        if (!stream.isExhausted()) lastPrompt = stream.readString();
    }
    dspType    .store(stream.readInt());
    dspDrive   .store(stream.readFloat());
    dspBlend   .store(stream.readFloat());
    dspOutput  .store(stream.readFloat());
    dspBits    .store(stream.readFloat());
    dspRate    .store(stream.readFloat());
    dspDepth   .store(stream.readFloat());
    dspCutoff  .store(stream.readFloat());
    dspHighpass.store(stream.readInt());
    if (!stream.isExhausted()) dspDelayTime.store(stream.readFloat());
    if (!stream.isExhausted()) dspFeedback .store(stream.readFloat());
    if (!stream.isExhausted()) dspRoomSize .store(stream.readFloat());

    // Layer history
    if (!stream.isExhausted())
    {
        juce::ScopedLock lock(stateLock);
        activeLayerIndex_ = stream.readInt();
        const int count   = stream.readInt();
        layers_.clear();
        for (int i = 0; i < count && !stream.isExhausted(); ++i)
        {
            LayerInfo layer;
            layer.name      = stream.readString();
            layer.faustCode = stream.readString();
            layer.status    = (LayerInfo::Status)stream.readInt();
            layers_.push_back(layer);
        }
    }
}

//==============================================================================
void AbletonAIPluginAudioProcessor::startGeneration(const juce::String& prompt,
                                                     const juce::String& apiKey)
{
    if (currentStatus.load() == Status::Generating)
        return;

    if (generationThread  != nullptr) generationThread ->stopThread(1000);
    if (activationThread_ != nullptr) activationThread_->stopThread(1000);

    {
        juce::ScopedLock lock(stateLock);
        statusMessage  = "Generating\xe2\x80\xa6";
        lastDeviceName = {};
        // Don't clear lastFaustCode — the current layer's code stays visible
        // while the new one generates.

        LayerInfo provisional;
        provisional.name        = "Layer " + juce::String(layers_.size() + 1);
        provisional.faustCode   = {};
        provisional.status      = LayerInfo::Status::Compiling;
        layers_.push_back(provisional);
        activeLayerIndex_ = (int)layers_.size() - 1;
    }
    currentStatus.store(Status::Generating);
    if (onStatusChanged) onStatusChanged();

    generationThread = std::make_unique<GenerationThread>(*this, prompt, apiKey);
    generationThread->startThread();
}

//==============================================================================
void AbletonAIPluginAudioProcessor::compileCode(const juce::String& code)
{
    if (currentStatus.load() == Status::Generating) return;
    if (generationThread  != nullptr) generationThread ->stopThread(1000);
    if (activationThread_ != nullptr) activationThread_->stopThread(1000);

    int targetIndex;
    {
        juce::ScopedLock lock(stateLock);
        if (activeLayerIndex_ >= 0 && activeLayerIndex_ < (int)layers_.size())
        {
            targetIndex = activeLayerIndex_;
            layers_[targetIndex].faustCode = code;
            layers_[targetIndex].status    = LayerInfo::Status::Compiling;
        }
        else
        {
            LayerInfo layer;
            layer.name      = "Custom";
            layer.faustCode = code;
            layer.status    = LayerInfo::Status::Compiling;
            layers_.push_back(layer);
            targetIndex       = (int)layers_.size() - 1;
            activeLayerIndex_ = targetIndex;
        }
        lastFaustCode = code;
    }

    currentStatus.store(Status::Generating);
    if (onStatusChanged) onStatusChanged();

    activationThread_ = std::make_unique<ActivationThread>(*this, code, targetIndex);
    activationThread_->startThread();
}

//==============================================================================
int AbletonAIPluginAudioProcessor::getLayerCount()
{
    juce::ScopedLock lock(stateLock);
    return (int)layers_.size();
}

AbletonAIPluginAudioProcessor::LayerInfo AbletonAIPluginAudioProcessor::getLayer(int index)
{
    juce::ScopedLock lock(stateLock);
    if (index < 0 || index >= (int)layers_.size()) return {};
    return layers_[index];
}

int AbletonAIPluginAudioProcessor::getActiveLayerIndex()
{
    juce::ScopedLock lock(stateLock);
    return activeLayerIndex_;
}

void AbletonAIPluginAudioProcessor::activateLayer(int index)
{
    juce::String codeToLoad;
    {
        juce::ScopedLock lock(stateLock);
        if (index < 0 || index >= (int)layers_.size()) return;
        if (index == activeLayerIndex_) return;
        if (layers_[index].faustCode.isEmpty()) return;

        codeToLoad = layers_[index].faustCode;
        activeLayerIndex_ = index;
        lastFaustCode = codeToLoad;
        lastDeviceName = layers_[index].name;
        statusMessage = "Compiling\xe2\x80\xa6";
    }

    if (generationThread  != nullptr) generationThread ->stopThread(1000);
    if (activationThread_ != nullptr) activationThread_->stopThread(1000);

    currentStatus.store(Status::Generating);
    if (onStatusChanged) onStatusChanged();

    activationThread_ = std::make_unique<ActivationThread>(*this, codeToLoad, index);
    activationThread_->startThread();
}

void AbletonAIPluginAudioProcessor::ActivationThread::run()
{
    std::string code = faustCode.toStdString();
    std::string errMsg;
    const char* argv[] = { "-I", FAUST_LIBRARIES_PATH };

    llvm_dsp_factory* factory = createDSPFactoryFromString(
        "effect", code, 2, argv, "", errMsg, -1);

    LayerInfo::Status newLayerStatus = LayerInfo::Status::Compiled;

    if (factory != nullptr)
    {
        dsp* instance = factory->createDSPInstance();
        instance->init((int)owner.sampleRate_);

        auto* capture = new FaustZoneCapture();
        instance->buildUserInterface(capture);

        dsp*              oldPending = owner.pendingFaustDsp_
                                           .exchange(instance, std::memory_order_acq_rel);
        llvm_dsp_factory* oldFactory = owner.pendingFaustFactory_
                                           .exchange(factory, std::memory_order_acq_rel);
        FaustZoneCapture* oldZones   = owner.pendingFaustZones_
                                           .exchange(capture, std::memory_order_acq_rel);
        owner.deleteFaustDsp(oldPending, oldFactory);
        delete oldZones;
    }
    else
    {
        newLayerStatus = LayerInfo::Status::Fallback;
    }

    {
        juce::ScopedLock lock(owner.stateLock);
        if (layerIndex < (int)owner.layers_.size())
            owner.layers_[layerIndex].status = newLayerStatus;
    }
    owner.currentStatus.store(Status::Done);

    auto* ownerPtr = &owner;
    juce::MessageManager::callAsync([ownerPtr] {
        if (ownerPtr->onStatusChanged) ownerPtr->onStatusChanged();
    });
}

juce::String AbletonAIPluginAudioProcessor::getStatusMessage()
{
    juce::ScopedLock lock(stateLock);
    return statusMessage;
}

juce::String AbletonAIPluginAudioProcessor::getLastDeviceName()
{
    juce::ScopedLock lock(stateLock);
    return lastDeviceName;
}

juce::String AbletonAIPluginAudioProcessor::getLastFaustCode()
{
    juce::ScopedLock lock(stateLock);
    return lastFaustCode;
}

juce::String AbletonAIPluginAudioProcessor::getLastPrompt()
{
    juce::ScopedLock lock(stateLock);
    return lastPrompt;
}

void AbletonAIPluginAudioProcessor::setLastPrompt(const juce::String& p)
{
    juce::ScopedLock lock(stateLock);
    lastPrompt = p;
}

//==============================================================================
void AbletonAIPluginAudioProcessor::GenerationThread::run()
{
    juce::String body;
    body << "{\"prompt\":"  << juce::JSON::toString(juce::var(prompt))
         << ",\"api_key\":" << juce::JSON::toString(juce::var(apiKey))
         << ",\"target\":\"vst\"}";

    juce::URL url(BACKEND_URL);
    url = url.withPOSTData(body);

    auto stream = url.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inPostData)
            .withExtraHeaders("Content-Type: application/json\r\nAccept: application/json\r\n")
            .withConnectionTimeoutMs(90000));

    Status       newStatus;
    juce::String newMessage, newDeviceName, newFaustCode;
    bool         jitSucceeded = false;

    if (stream == nullptr)
    {
        newStatus  = Status::Error;
        newMessage = "Cannot reach backend \xe2\x80\x94 run: bash scripts/start_backend.sh";
    }
    else
    {
        auto parsed = juce::JSON::parse(stream->readEntireStreamAsString());

        if (parsed["status"].toString() == "ok")
        {
            newStatus     = Status::Done;
            newDeviceName = parsed["device_name"].toString();
            newFaustCode  = parsed["faust_code"].toString();
            newMessage    = newDeviceName;

            // Update the provisional layer's name now that we have it.
            {
                juce::ScopedLock lock(owner.stateLock);
                if (owner.activeLayerIndex_ >= 0 &&
                    owner.activeLayerIndex_ < (int)owner.layers_.size())
                {
                    owner.layers_[owner.activeLayerIndex_].name     = newDeviceName;
                    owner.layers_[owner.activeLayerIndex_].faustCode = newFaustCode;
                }
            }
            auto* ownerPtr = &owner;
            juce::MessageManager::callAsync([ownerPtr] {
                if (ownerPtr->onStatusChanged) ownerPtr->onStatusChanged();
            });

            // --- JIT-compile the FAUST code -----------------------------------
            {
                std::string faustCode = newFaustCode.toStdString();
                std::string errMsg;

                const char* argv[] = { "-I", FAUST_LIBRARIES_PATH };
                llvm_dsp_factory* factory = createDSPFactoryFromString(
                    "effect", faustCode, 2, argv, "", errMsg, -1);

                if (factory != nullptr)
                {
                    jitSucceeded = true;
                    dsp* instance = factory->createDSPInstance();
                    instance->init((int)owner.sampleRate_);

                    // Capture all hslider/vslider/numentry zone pointers.
                    auto* capture = new FaustZoneCapture();
                    instance->buildUserInterface(capture);

                    // Hand off to audio thread atomically.
                    // If there was already a pending DSP (shouldn't happen
                    // normally), clean it up here on the generation thread.
                    dsp*              oldPending = owner.pendingFaustDsp_
                                                       .exchange(instance, std::memory_order_acq_rel);
                    llvm_dsp_factory* oldFactory = owner.pendingFaustFactory_
                                                       .exchange(factory, std::memory_order_acq_rel);
                    FaustZoneCapture* oldZones   = owner.pendingFaustZones_
                                                       .exchange(capture, std::memory_order_acq_rel);
                    owner.deleteFaustDsp(oldPending, oldFactory);
                    delete oldZones;
                }
                else
                {
                    // JIT failed — fall back to dsp_params C++ engine.
                    newMessage = newDeviceName + " (FAUST err: "
                                 + juce::String(errMsg) + ")";
                }
            }

            // --- Fallback C++ engine params (used if JIT failed) -------------
            auto params = parsed["dsp_params"];
            if (params.isObject())
            {
                auto type = params["type"].toString();

                if (type == "saturation")
                {
                    owner.dspType  .store((int)DspType::Saturation);
                    owner.dspDrive .store((float)params.getProperty("drive",  10.0));
                    owner.dspBlend .store((float)params.getProperty("blend",   1.0));
                    owner.dspOutput.store((float)params.getProperty("output",  0.8));
                }
                else if (type == "bitcrusher")
                {
                    owner.dspType .store((int)DspType::Bitcrusher);
                    owner.dspBits .store((float)params.getProperty("bits",  8.0));
                    owner.dspBlend.store((float)params.getProperty("blend", 1.0));
                }
                else if (type == "tremolo")
                {
                    owner.dspType .store((int)DspType::Tremolo);
                    owner.dspRate .store((float)params.getProperty("rate",  4.0));
                    owner.dspDepth.store((float)params.getProperty("depth", 0.5));
                }
                else if (type == "filter")
                {
                    owner.dspType    .store((int)DspType::Filter);
                    owner.dspCutoff  .store((float)params.getProperty("cutoff",   1000.0));
                    owner.dspHighpass.store(params.getProperty("highpass", false) ? 1 : 0);
                }
                else if (type == "delay")
                {
                    owner.dspType    .store((int)DspType::Delay);
                    owner.dspDelayTime.store((float)params.getProperty("time",     300.0));
                    owner.dspFeedback .store((float)params.getProperty("feedback", 0.4));
                    owner.dspBlend    .store((float)params.getProperty("blend",    0.5));
                }
                else if (type == "chorus")
                {
                    owner.dspType .store((int)DspType::Chorus);
                    owner.dspRate .store((float)params.getProperty("rate",  0.5));
                    owner.dspDepth.store((float)params.getProperty("depth", 0.7));
                    owner.dspBlend.store((float)params.getProperty("blend", 0.5));
                }
                else if (type == "reverb")
                {
                    owner.dspType    .store((int)DspType::Reverb);
                    owner.dspRoomSize.store((float)params.getProperty("room_size", 0.5));
                    owner.dspBlend   .store((float)params.getProperty("blend",     0.5));
                }
                else
                {
                    owner.dspType.store((int)DspType::Passthrough);
                }
            }
        }
        else
        {
            newStatus  = Status::Error;
            newMessage = "Error: " + parsed["message"].toString();
        }
    }

    {
        juce::ScopedLock lock(owner.stateLock);
        owner.statusMessage  = newMessage;
        owner.lastDeviceName = newDeviceName;
        owner.lastFaustCode  = newFaustCode;

        // Finalise the active layer's status.
        if (owner.activeLayerIndex_ >= 0 &&
            owner.activeLayerIndex_ < (int)owner.layers_.size())
        {
            auto& layer = owner.layers_[owner.activeLayerIndex_];
            if (newStatus == Status::Done)
                layer.status = jitSucceeded ? LayerInfo::Status::Compiled
                                            : LayerInfo::Status::Fallback;
        }
    }
    owner.currentStatus.store(newStatus);

    auto* ownerPtr = &owner;
    juce::MessageManager::callAsync([ownerPtr] {
        if (ownerPtr->onStatusChanged)
            ownerPtr->onStatusChanged();
    });
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AbletonAIPluginAudioProcessor();
}
