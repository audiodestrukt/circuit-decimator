// bench_cpu -- the Circuit Bench audio callback's CPU load for a netlist circuit,
// measured the way the Bench's "CPU" figure is (callback time / buffer duration),
// at several buffer sizes, idle and while a knob is being dragged.
//
//   bench_cpu [circuit index] [--rate Hz]
#include "../bench/NetBench.h"

#include <chrono>
#include <cstdio>

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    int index = 5;
    double rate = 48000;
    for (int i = 1; i < argc; ++i) {
        const juce::String a(argv[i]);
        if (a == "--rate" && i + 1 < argc) rate = juce::String(argv[++i]).getDoubleValue();
        else index = a.getIntValue();
    }
    std::printf("circuit %d (%s) at %.0f Hz\n", index, cd::catalog::names()[(size_t) index].c_str(), rate);
    for (int block : { 64, 128, 256, 512 }) {
        for (int drag = 0; drag < 2; ++drag) {
            NetBench net;
            net.prepare(rate, block);
            net.select(index);
            juce::AudioBuffer<float> buf(2, block);
            auto run = [&](int blocks) { for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < block; ++i) { const float x = 0.3f * std::sin(0.05f * (float) (b * block + i)); buf.setSample(0, i, x); buf.setSample(1, i, x); }
                net.process(buf, block); } };
            run(20);   // picks up the circuit
            const int blocks = (int) (2.0 * rate / block);
            const auto& ctl = net.uiControls;
            int micIdx = -1;
            for (size_t k = 0; k < ctl.size(); ++k) if (std::string(ctl[k].knob.id) == "micoff") micIdx = (int) k;
            int eIdx = -1;
            for (size_t k = 0; k < ctl.size(); ++k) if (std::string(ctl[k].knob.id) == "E") eIdx = (int) k;
            double worst = 0, total = 0;
            for (int b = 0; b < blocks; ++b) {
                if (drag && b % 4 == 0) {   // a knob drag: a new value every few callbacks
                    if (micIdx >= 0) net.values[(size_t) micIdx] = 2.0 + 6.0 * std::sin(0.01 * b);
                    if (eIdx >= 0) net.values[(size_t) eIdx] = 4.0 + 0.5 * std::sin(0.013 * b);
                }
                const auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i < block; ++i) buf.setSample(0, i, 0.3f * std::sin(0.05f * (float) (b * block + i)));
                net.process(buf, block);
                const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                total += s;
                worst = std::max(worst, s);
                net.collect();
            }
            const double dur = block / rate;
            std::printf("  buffer %4d %s: average %5.1f %%  worst callback %6.1f %% of its buffer\n", block,
                        drag ? "knobs moving" : "idle        ", 100 * total / (blocks * dur), 100 * worst / dur);
        }
    }
}
