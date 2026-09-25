// plugin_render -- load the built VST3 headlessly and run a WAV through it.
//
//   plugin_render <plugin.vst3> <in.wav> <out.wav> [--block N] [--prepare N]
//                 [--program N] [id=value ...] [--ramp id:from:to ...]
//
// Values are in the parameter's own units ("battery=6", "temperature=80").
// --ramp automates a parameter linearly across the whole file, which is how a
// DAW would drive it. --prepare smaller than --block exercises the plugin's
// oversized-host-block path.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#include <chrono>
#include <cstdio>

using namespace juce;

// VST3 exposes numeric parameter IDs, so match on the display name,
// ignoring case and spaces ("junctionleak" finds "Junction Leak").
static AudioProcessorParameter* findParam(AudioPluginInstance& p, const String& id)
{
    auto key = [](const String& s) { return s.removeCharacters(" ").toLowerCase(); };
    for (auto* prm : p.getParameters())
        if (key(prm->getName(64)) == key(id)) return prm;
    std::fprintf(stderr, "unknown param '%s'; have:", id.toRawUTF8());
    for (auto* prm : p.getParameters()) std::fprintf(stderr, " [%s]", prm->getName(64).toRawUTF8());
    std::fprintf(stderr, "\n");
    return nullptr;
}

int main(int argc, char** argv)
{
    ScopedJuceInitialiser_GUI init;
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s plugin.vst3 in.wav out.wav [--block N] [--prepare N] [id=v] [--ramp id:a:b]\n", argv[0]);
        return 2;
    }
    int block = 256, prepare = -1, program = -1;
    StringArray sets, ramps;
    for (int i = 4; i < argc; ++i) {
        String a(argv[i]);
        if (a == "--block" && i + 1 < argc) block = String(argv[++i]).getIntValue();
        else if (a == "--prepare" && i + 1 < argc) prepare = String(argv[++i]).getIntValue();
        else if (a == "--ramp" && i + 1 < argc) ramps.add(argv[++i]);
        else if (a == "--program" && i + 1 < argc) program = String(argv[++i]).getIntValue();
        else sets.add(a);
    }
    if (prepare < 0) prepare = block;

    AudioFormatManager afm;
    afm.registerBasicFormats();
    std::unique_ptr<AudioFormatReader> reader(afm.createReaderFor(File::getCurrentWorkingDirectory().getChildFile(argv[2])));
    if (!reader) { std::fprintf(stderr, "can't read %s\n", argv[2]); return 1; }
    const double sr = reader->sampleRate;
    const int len = (int) reader->lengthInSamples;
    AudioBuffer<float> audio(2, len);
    reader->read(&audio, 0, len, 0, true, true);

    AudioPluginFormatManager pfm;
    addHeadlessDefaultFormatsToManager(pfm);
    OwnedArray<PluginDescription> types;
    for (auto* f : pfm.getFormats())
        f->findAllTypesForFile(types, File::getCurrentWorkingDirectory().getChildFile(argv[1]).getFullPathName());
    if (types.isEmpty()) { std::fprintf(stderr, "no plugin in %s\n", argv[1]); return 1; }
    String err;
    auto plugin = pfm.createPluginInstance(*types[0], sr, prepare, err);
    if (!plugin) { std::fprintf(stderr, "load failed: %s\n", err.toRawUTF8()); return 1; }

    if (program >= 0) {
        plugin->setCurrentProgram(program);
        std::printf("program %d: %s\n", program, plugin->getProgramName(program).toRawUTF8());
    }
    for (auto& s : sets) {
        auto* prm = findParam(*plugin, s.upToFirstOccurrenceOf("=", false, false));
        if (!prm) { std::fprintf(stderr, "unknown param %s\n", s.toRawUTF8()); return 2; }
        prm->setValueNotifyingHost(prm->getValueForText(s.fromFirstOccurrenceOf("=", false, false)));
    }
    struct Ramp { AudioProcessorParameter* p; float a, b; };
    std::vector<Ramp> rs;
    for (auto& r : ramps) {
        auto parts = StringArray::fromTokens(r, ":", "");
        auto* prm = parts.size() == 3 ? findParam(*plugin, parts[0]) : nullptr;
        if (!prm) { std::fprintf(stderr, "bad ramp %s\n", r.toRawUTF8()); return 2; }
        rs.push_back({ prm, prm->getValueForText(parts[1]), prm->getValueForText(parts[2]) });
    }

    plugin->setPlayConfigDetails(2, 2, sr, prepare);
    plugin->prepareToPlay(sr, prepare);
    std::printf("%s: latency %d samples, %d params\n", plugin->getName().toRawUTF8(),
                plugin->getLatencySamples(), plugin->getParameters().size());

    MidiBuffer midi;
    AudioBuffer<float> io(2, block);
    const auto t0 = std::chrono::steady_clock::now();
    for (int pos = 0; pos < len; pos += block) {
        const int n = jmin(block, len - pos);
        for (auto& r : rs) r.p->setValueNotifyingHost(r.a + (r.b - r.a) * (float) pos / (float) len);
        io.setSize(2, n, false, false, true);
        for (int c = 0; c < 2; ++c) io.copyFrom(c, 0, audio, c, pos, n);
        plugin->processBlock(io, midi);
        for (int c = 0; c < 2; ++c) audio.copyFrom(c, pos, io, c, 0, n);
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    plugin->releaseResources();

    float peak = audio.getMagnitude(0, len);
    bool finite = true;
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < len; ++i) finite &= std::isfinite(audio.getSample(c, i));
    std::printf("%.2f s audio in %.3f s (%.1fx realtime), peak %.1f dBFS, finite=%s\n",
                len / sr, wall, len / sr / wall, Decibels::gainToDecibels(peak), finite ? "yes" : "NO");

    File out = File::getCurrentWorkingDirectory().getChildFile(argv[3]);
    out.deleteFile();
    WavAudioFormat wav;
    std::unique_ptr<OutputStream> os = out.createOutputStream();
    auto writer = wav.createWriterFor(os, AudioFormatWriterOptions {}.withSampleRate(sr).withNumChannels(2).withBitsPerSample(24));
    if (!writer) { std::fprintf(stderr, "can't write %s\n", argv[3]); return 1; }
    writer->writeFromAudioSampleBuffer(audio, 0, len);
    return finite ? 0 : 3;
}
