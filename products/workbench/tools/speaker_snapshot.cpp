// speaker_snapshot -- render the speaker cross-section view (core/ui/SpeakerView.h)
// for a cab with the given parameters, to review how the drawing follows the knobs.
//
//   speaker_snapshot out.png [--width W] [--height H] [--hz F] [param=value ...]
//   params: CabParam names in SI units, e.g. depth=0.04 ribs=8 taper=2.5 micoff=0.08 micang=30 (deg) vb=0.1
#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/SpeakerView.h"

#include <map>

using namespace cd::acoustic;

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 2) { std::fprintf(stderr, "usage: %s out.png [--width W] [--height H] [--hz F] [name=value ...]\n", argv[0]); return 2; }
    const std::map<std::string, int> names {
        { "E", kYoungs }, { "rho", kDensity }, { "h", kThickness }, { "taper", kTaper }, { "ribs", kRibs }, { "aniso", kAniso },
        { "eta", kLoss }, { "depth", kDepth }, { "curve", kCurve }, { "dcr", kDustCap }, { "dcm", kCapMass },
        { "sr", kSurroundR }, { "skr", kSurroundKr }, { "bl", kBl }, { "vb", kVb }, { "qa", kQa },
        { "micoff", kMicOffset }, { "micdist", kMicDistance }, { "micang", kMicAngle }, { "miccap", kMicCapsule }, { "micpat", kMicPattern } };
    int w = 900, h = 560;
    double hz = 2300;
    Cab cab;
    cab.prepare(48000);
    for (int i = 2; i < argc; ++i) {
        const juce::String a(argv[i]);
        if (a == "--width" && i + 1 < argc) { w = juce::String(argv[++i]).getIntValue(); continue; }
        if (a == "--height" && i + 1 < argc) { h = juce::String(argv[++i]).getIntValue(); continue; }
        if (a == "--hz" && i + 1 < argc) { hz = juce::String(argv[++i]).getDoubleValue(); continue; }
        const auto key = a.upToFirstOccurrenceOf("=", false, false).toStdString();
        double v = a.fromFirstOccurrenceOf("=", false, false).getDoubleValue();
        auto it = names.find(key);
        if (it == names.end()) { std::fprintf(stderr, "unknown param %s\n", key.c_str()); return 2; }
        if (key == "micang") v *= M_PI / 180;
        cab.set(it->second, v);
    }
    // let the worker rebuild with the new parameters
    const int before = cab.rebuilds.load();
    for (int k = 0; k < 400 && cab.rebuilds.load() == before && argc > 2; ++k) {
        for (int i = 0; i < 64; ++i) cab.process(0);
        juce::Thread::sleep(5);
    }
    cd::ui::SpeakerView view([&cab] { return &cab; });
    view.setSize(w, h);
    view.setCursor(hz);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);   // timer: pick up params + display
    auto img = view.createComponentSnapshot(view.getLocalBounds(), true, 1.0f);
    juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]);
    out.deleteFile();
    juce::PNGImageFormat png;
    auto os = out.createOutputStream();
    if (!os || !png.writeImageToStream(img, *os)) { std::fprintf(stderr, "can't write %s\n", argv[1]); return 1; }
    cab.release();
    return 0;
}
