// FuzzEngine -- the realtime audio path every product shares: mono sum ->
// pickup -> circuit solver at 4x oversampling -> DC block -> output, with
// per-parameter smoothing, solver health stats and node telemetry for the
// circuit view. Products own their parameters and hand the engine a full
// knob vector (core/circuit/Knobs.h order, in knob units) each block.
//
// Two circuits share that knob surface: "London '66" (the Fuzz Face circuit,
// hand-built DK solver) and "Tokyo '68" (the Shin-Ei FY-2 circuit, a netlist on
// the general engine); setModel() picks one.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "circuit/Knobs.h"
#include "circuit/circuits/ShinEi.h"

namespace cd {

class FuzzEngine {
public:
    enum class Model { FuzzFace = 0, ShinEi = 1 };
    static constexpr int kNumModels = 2;
    // player-facing names: where and when each circuit came from (no trademarks)
    static const char* modelName(Model m) { return m == Model::ShinEi ? "Tokyo '68" : "London '66"; }

    FuzzEngine();

    void prepare(double sampleRate, int maxBlockSize, const double* knobs);
    void reset();
    int latencySamples() const;

    // Mono circuit: inputs are summed, the result is copied to every output.
    void process(juce::AudioBuffer<float>&, int numInputs, int numOutputs, const double* knobs);

    // "Power cycle": re-solve the DC operating point on the next block.
    void requestCircuitReset() { resetRequested = true; }

    // Which circuit the knobs drive. The audio thread switches at the next
    // block and warm-starts the new circuit (settled DC point, no thump).
    void setModel(Model m) { requestedModel = (int) m; }
    Model model() const { return (Model) requestedModel.load(); }

    // ---- telemetry (written by the audio thread, read by UIs) ----
    static constexpr int kNodes = std::max((int) FuzzFace::N, (int) ShinEi::N);
    std::array<std::atomic<float>, kNodes> nodeMean {}, nodeMin {}, nodeMax {};   // indexed by the active model's Node
    std::array<std::atomic<float>, kNumKnobs> knobsInUse {};  // what the circuit is set to
    std::atomic<int> modelInUse { 0 };                        // Model the telemetry comes from
    std::atomic<float> newtonAverage { 0 };
    std::atomic<int> newtonMax { 0 };
    std::atomic<long> solverFailures { 0 };

private:
    void processChunk(juce::AudioBuffer<float>&, int start, int n, int numInputs, int numOutputs);
    void setTargets(const double* knobs);
    void snapSmoothers();
    void restart(int model);

    static constexpr int osFactorLog2 = 2;     // 4x: fuzz output is square-ish
    static constexpr int controlInterval = 16; // oversampled samples per param update

    int maxBlock = 512;
    std::atomic<bool> resetRequested { false };
    std::atomic<int> requestedModel { 0 };
    int activeModel = 0;
    FuzzFaceDK circuit;   // realtime DK solver (FuzzFace.h is its reference)
    FuzzFaceParams target;
    ShinEi shinei;        // FY-2 netlist on the general engine
    ShinEiParams seTarget;
    juce::dsp::Oversampling<float> oversampling { 1, osFactorLog2,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true };
    juce::AudioBuffer<float> mono;
    juce::dsp::IIR::Filter<float> dcBlock;

    // one smoother per solver parameter, stepped at the oversampled rate;
    // resistances/caps/gains that span decades are smoothed in the log domain
    template <typename P>
    struct Smoothed {
        double P::* field;
        bool logDomain;
        juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear> value;
        double current() const { return logDomain ? std::exp(value.getCurrentValue()) : value.getCurrentValue(); }
        void target(const P& p)
        {
            const double v = p.*field;
            value.setTargetValue(logDomain ? std::log(v) : v);
        }
    };
    std::vector<Smoothed<FuzzFaceParams>> smoothers;
    std::vector<Smoothed<ShinEiParams>> seSmoothers;
    juce::SmoothedValue<float> inputGain, outputGain;
};

} // namespace cd
