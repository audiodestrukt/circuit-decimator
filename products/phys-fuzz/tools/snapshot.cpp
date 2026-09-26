// physfuzz_snapshot -- render the Phys Fuzz editor to a PNG while audio plays
// through it, so the circuit view shows real signal (design review, store art).
//
//   physfuzz_snapshot out.png input.wav [--program N] [--width W] [--seconds S] [id=value ...]
#include <juce_audio_formats/juce_audio_formats.h>

#include "../PhysFuzzEditor.h"

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s out.png input.wav [--program N] [--width W] [--seconds S] [id=value ...]\n", argv[0]);
        return 2;
    }
    int program = -1, width = 960;
    double seconds = 1.5;
    juce::StringArray sets;
    for (int i = 3; i < argc; ++i) {
        juce::String a(argv[i]);
        if (a == "--program" && i + 1 < argc) program = juce::String(argv[++i]).getIntValue();
        else if (a == "--width" && i + 1 < argc) width = juce::String(argv[++i]).getIntValue();
        else if (a == "--seconds" && i + 1 < argc) seconds = juce::String(argv[++i]).getDoubleValue();
        else sets.add(a);
    }

    juce::AudioFormatManager afm;
    afm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(
        afm.createReaderFor(juce::File::getCurrentWorkingDirectory().getChildFile(argv[2])));
    if (!reader) { std::fprintf(stderr, "can't read %s\n", argv[2]); return 1; }
    const double sr = reader->sampleRate;
    const int len = (int) std::min<juce::int64>(reader->lengthInSamples, (juce::int64) (seconds * sr));
    juce::AudioBuffer<float> audio(2, len);
    reader->read(&audio, 0, len, 0, true, true);

    PhysFuzzProcessor proc;
    if (program >= 0) proc.setCurrentProgram(program);
    for (auto& s : sets) {
        auto* prm = proc.apvts.getParameter(s.upToFirstOccurrenceOf("=", false, false));
        if (!prm) { std::fprintf(stderr, "unknown param %s\n", s.toRawUTF8()); return 2; }
        prm->setValueNotifyingHost(prm->convertTo0to1(s.fromFirstOccurrenceOf("=", false, false).getFloatValue()));
    }
    constexpr int block = 256;
    proc.setPlayConfigDetails(2, 2, sr, block);
    proc.prepareToPlay(sr, block);

    std::unique_ptr<juce::AudioProcessorEditor> editor(proc.createEditor());
    editor->setSize(width, juce::roundToInt(width * 640.0 / 960.0));

    // play in roughly real time so the view's smoothing timers see it
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> io(2, block);
    for (int pos = 0; pos + block <= len; pos += block) {
        for (int c = 0; c < 2; ++c) io.copyFrom(c, 0, audio, c, pos, block);
        proc.processBlock(io, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil((int) (1000.0 * block / sr));
    }

    auto img = editor->createComponentSnapshot(editor->getLocalBounds(), true, 1.0f);
    juce::File out = juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]);
    out.deleteFile();
    juce::PNGImageFormat png;
    auto os = out.createOutputStream();
    if (!os || !png.writeImageToStream(img, *os)) { std::fprintf(stderr, "can't write %s\n", argv[1]); return 1; }
    editor.reset();
    return 0;
}
