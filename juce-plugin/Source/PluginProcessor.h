#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include <faust/dsp/llvm-dsp.h>
#include <faust/gui/MapUI.h>

// ---- Pre-allocated JUCE parameter slot for one FAUST control ------------------
// Registered at processor construction; label/range updated after each generation.
struct DynamicFaustParameter : public juce::AudioParameterFloat
{
    explicit DynamicFaustParameter(int index)
        : juce::AudioParameterFloat("faust_p" + juce::String(index),
                                    "P"        + juce::String(index + 1),
                                    0.0f, 1.0f, 0.5f)
        , index_(index)
    {}

    // Called on message thread after each successful generation.
    void configure(const juce::String& label, float min, float max, float defaultVal)
    {
        { juce::SpinLock::ScopedLockType sl(nameLock_); displayName_ = label; }
        faustMin_.store(min, std::memory_order_relaxed);
        faustMax_.store(max, std::memory_order_relaxed);
        const float norm = (std::abs(max - min) > 1e-6f)
                           ? juce::jlimit(0.f, 1.f, (defaultVal - min) / (max - min))
                           : 0.5f;
        setValueNotifyingHost(norm);
    }

    void deactivate()
    {
        { juce::SpinLock::ScopedLockType sl(nameLock_); displayName_ = {}; }
        faustMin_.store(0.f, std::memory_order_relaxed);
        faustMax_.store(1.f, std::memory_order_relaxed);
        setValueNotifyingHost(0.5f);
    }

    // Maps the current JUCE 0..1 value to FAUST parameter space — safe on audio thread.
    float getFaustValue() const noexcept
    {
        const float v = get();
        return faustMin_.load(std::memory_order_relaxed)
             + v * (faustMax_.load(std::memory_order_relaxed)
                  - faustMin_.load(std::memory_order_relaxed));
    }

    juce::String getName(int maxLen) const override
    {
        juce::SpinLock::ScopedLockType sl(nameLock_);
        return (displayName_.isEmpty() ? "P" + juce::String(index_ + 1) : displayName_)
                   .substring(0, maxLen);
    }

    juce::String getText(float normValue, int) const override
    {
        const float fv = faustMin_.load(std::memory_order_relaxed)
                       + normValue * (faustMax_.load(std::memory_order_relaxed)
                                    - faustMin_.load(std::memory_order_relaxed));
        return juce::String(fv, 2);
    }

    float getValueForText(const juce::String& text) const override
    {
        const float fv    = text.getFloatValue();
        const float range = faustMax_.load(std::memory_order_relaxed)
                          - faustMin_.load(std::memory_order_relaxed);
        if (std::abs(range) < 1e-6f) return 0.5f;
        return juce::jlimit(0.f, 1.f,
                            (fv - faustMin_.load(std::memory_order_relaxed)) / range);
    }

private:
    int                    index_;
    mutable juce::SpinLock nameLock_;
    juce::String           displayName_;
    std::atomic<float>     faustMin_ { 0.f };
    std::atomic<float>     faustMax_ { 1.f };
};

// ---- Captures FAUST parameter zone pointers via buildUserInterface() ----------
struct FaustZoneCapture : public UI
{
    struct Info { juce::String label; FAUSTFLOAT* zone; float min, max, init; };
    std::vector<Info> zones;

    void addHorizontalSlider(const char* l, FAUSTFLOAT* z,
                              FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT) override
    { zones.push_back({strip(l), z, min, max, init}); }

    void addVerticalSlider(const char* l, FAUSTFLOAT* z,
                            FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT s) override
    { addHorizontalSlider(l, z, init, min, max, s); }

    void addNumEntry(const char* l, FAUSTFLOAT* z,
                      FAUSTFLOAT init, FAUSTFLOAT min, FAUSTFLOAT max, FAUSTFLOAT s) override
    { addHorizontalSlider(l, z, init, min, max, s); }

    void openTabBox(const char*) override {}
    void openHorizontalBox(const char*) override {}
    void openVerticalBox(const char*) override {}
    void closeBox() override {}
    void addButton(const char*, FAUSTFLOAT*) override {}
    void addCheckButton(const char*, FAUSTFLOAT*) override {}
    void addHorizontalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
    void addVerticalBargraph(const char*, FAUSTFLOAT*, FAUSTFLOAT, FAUSTFLOAT) override {}
    void addSoundfile(const char*, const char*, Soundfile**) override {}

private:
    static juce::String strip(const char* label)
    {
        juce::String s(label);
        const int br = s.indexOf("[");
        return (br >= 0 ? s.substring(0, br) : s).trim();
    }
};

class AbletonAIPluginAudioProcessor : public juce::AudioProcessor
{
public:
    enum class Status { Idle, Generating, Done, Error };

    // -------------------------------------------------------------------------
    // ARCHITECTURE NOTE
    // The built-in C++ DSP engine (DspType enum below) is a temporary fallback
    // that approximates common effects so users hear something immediately.
    //
    // The REAL output is lastFaustCode — the complete FAUST program Claude
    // generated. When libfaust JIT is integrated, processBlock will compile
    // and run that code directly, making every generated effect truly custom.
    //
    // Integration point: replace the switch in processBlock with:
    //   if (faustDsp_ != nullptr) faustDsp_->compute(N, inputs, outputs);
    // -------------------------------------------------------------------------
    enum class DspType { Passthrough = 0, Saturation, Bitcrusher, Tremolo, Filter,
                         Delay, Chorus, Reverb };

    AbletonAIPluginAudioProcessor();
    ~AbletonAIPluginAudioProcessor() override;

    //==============================================================================
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override;
    bool acceptsMidi()  const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int  getNumPrograms()    override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==============================================================================
    void startGeneration(const juce::String& prompt, const juce::String& apiKey);
    void compileCode(const juce::String& code);  // JIT-compile arbitrary FAUST code

    Status       getStatus()        const { return currentStatus.load(); }
    juce::String getStatusMessage();
    juce::String getLastDeviceName();
    juce::String getLastFaustCode();
    juce::String getLastPrompt();
    void         setLastPrompt(const juce::String&);

    // ---- Layer / version history -------------------------------------------
    struct LayerInfo {
        enum class Status { Compiling, Compiled, Fallback };
        juce::String name;
        juce::String faustCode;
        Status       status { Status::Compiling };
    };

    int       getLayerCount();
    LayerInfo getLayer(int index);
    int       getActiveLayerIndex();
    void      activateLayer(int index);   // re-JIT that layer's stored code

    std::function<void()> onStatusChanged; // always called on message thread

    //==============================================================================
    // DSP parameters — written from message thread, read lock-free from audio thread
    std::atomic<int>   dspType    { (int)DspType::Passthrough };

    // Saturation
    std::atomic<float> dspDrive   { 1.f };
    std::atomic<float> dspBlend   { 1.f };
    std::atomic<float> dspOutput  { 1.f };

    // Bitcrusher
    std::atomic<float> dspBits    { 16.f };

    // Tremolo / Chorus (shared rate + depth)
    std::atomic<float> dspRate    { 4.f };
    std::atomic<float> dspDepth   { 0.5f };

    // Filter
    std::atomic<float> dspCutoff  { 1000.f };
    std::atomic<int>   dspHighpass{ 0 };

    // Delay
    std::atomic<float> dspDelayTime { 300.f };  // ms
    std::atomic<float> dspFeedback  { 0.4f };

    // Reverb
    std::atomic<float> dspRoomSize  { 0.5f };

private:
    //==============================================================================
    struct GenerationThread : public juce::Thread
    {
        GenerationThread(AbletonAIPluginAudioProcessor& p,
                         juce::String pr, juce::String ak)
            : juce::Thread("AI Generation"),
              owner(p), prompt(std::move(pr)), apiKey(std::move(ak)) {}
        void run() override;
        AbletonAIPluginAudioProcessor& owner;
        juce::String prompt, apiKey;
    };

    //==============================================================================
    // DSP processing helpers (called on audio thread only)
    void processSaturation (juce::AudioBuffer<float>&);
    void processBitcrusher (juce::AudioBuffer<float>&);
    void processTremolo    (juce::AudioBuffer<float>&);
    void processFilter     (juce::AudioBuffer<float>&);
    void processDelay      (juce::AudioBuffer<float>&);
    void processChorus     (juce::AudioBuffer<float>&);
    void processReverb     (juce::AudioBuffer<float>&);

    // Per-sample state (audio thread only — no locking needed)
    double sampleRate_      { 44100.0 };
    float  tremoloPhase_    { 0.f };
    float  chorusPhase_     { 0.f };
    float  filterState_[2]  { 0.f, 0.f };

    struct DelayLine { std::vector<float> buf; int pos { 0 }; };
    DelayLine delayLine_[2];
    DelayLine chorusLine_[2];

    struct CombFilter    { std::vector<float> buf; int pos { 0 }; float last { 0.f }; };
    struct AllpassFilter { std::vector<float> buf; int pos { 0 }; };
    CombFilter    combFilters_[2][4];
    AllpassFilter allpassFilters_[2][2];

    //==============================================================================
    std::atomic<Status> currentStatus { Status::Idle };

    juce::CriticalSection stateLock;
    juce::String statusMessage;
    juce::String lastDeviceName;
    juce::String lastFaustCode;
    juce::String lastPrompt;

    std::unique_ptr<GenerationThread> generationThread;

    // ---- Layer activation (re-JIT a stored layer without an API call) ------
    struct ActivationThread : public juce::Thread
    {
        ActivationThread(AbletonAIPluginAudioProcessor& p, juce::String code, int idx)
            : juce::Thread("Layer Activation"), owner(p),
              faustCode(std::move(code)), layerIndex(idx) {}
        void run() override;
        AbletonAIPluginAudioProcessor& owner;
        juce::String faustCode;
        int          layerIndex;
    };
    std::unique_ptr<ActivationThread> activationThread_;

    // ---- Layer state (all under stateLock) ---------------------------------
    std::vector<LayerInfo> layers_;
    int                    activeLayerIndex_ { -1 };

    // ---- FAUST JIT ---------------------------------------------------------
    // Generation thread compiles new DSP and stores it here.
    // Audio thread picks it up at the start of the next processBlock.
    std::atomic<dsp*>              pendingFaustDsp_     { nullptr };
    std::atomic<llvm_dsp_factory*> pendingFaustFactory_ { nullptr };
    std::atomic<FaustZoneCapture*> pendingFaustZones_   { nullptr };

    // Owned by audio thread only — never touched elsewhere.
    dsp*              activeFaustDsp_     { nullptr };
    llvm_dsp_factory* activeFaustFactory_ { nullptr };
    FaustZoneCapture* activeFaustZones_   { nullptr };

    void swapFaustDsp();   // called from audio thread when pendingFaustDsp_ is set
    void deleteFaustDsp(dsp*, llvm_dsp_factory*);  // safe to call from any thread

    // ---- Dynamic FAUST parameters (host-visible, automation/LFO capable) ---
    static constexpr int MAX_FAUST_PARAMS = 8;
    DynamicFaustParameter* faustParams_[MAX_FAUST_PARAMS] {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AbletonAIPluginAudioProcessor)
};
