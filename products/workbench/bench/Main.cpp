// Circuit Bench -- a test bench for the Circuit Decimator processor.
//
// Loops a WAV (default: the sim/ riff, or drop a DI file on the window) or
// takes live input, runs it through the processor compiled in directly (no
// plugin loading), and shows a scope plus solver health. Presets are the
// processor's programs, plus any search results loaded with "Load search..."
// (search/runs/*/elites.json); "Power cycle" re-solves the circuit's DC state.
//
//   Circuit Bench [file.wav] [search/runs/<name>/elites.json] [--play] [--mute] [--program N] [--circuit N]
//
// Circuit menu: the fuzz workbench (knobs, schematic, presets, search results),
// or any netlist circuit from core/circuit/circuits/Catalog.h with every
// component as a knob, a node meter and (if it has a core) the B-H loop.
//
// --mute runs everything but sends silence to the device (headless demos/tests).
#include <juce_audio_utils/juce_audio_utils.h>

#include "NetControls.h"
#include "ui/GainReductionView.h"
#include "PluginProcessor.h"
#include "ui/CircuitView.h"

namespace {

const auto kBackground = juce::Colour(0xff1b1c1e);
const auto kTrace = juce::Colour(0xff7fd18b);
const auto kGrid = juce::Colour(0xff34363a);

// Rolling capture of the output, drawn triggered on a rising zero crossing.
class Scope : public juce::Component, private juce::Timer {
public:
    Scope() { startTimerHz(30); }

    // audio thread
    void push(const float* data, int n)
    {
        int w = write.load(std::memory_order_relaxed);
        for (int i = 0; i < n; ++i) buffer[(size_t) (w++ & mask)] = data[i];
        write.store(w, std::memory_order_release);
    }

    void paint(juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        g.fillAll(kBackground);
        g.setColour(kGrid);
        g.drawHorizontalLine((int) r.getCentreY(), r.getX(), r.getRight());
        g.drawRect(r);

        constexpr int shown = 1024;
        const int end = write.load(std::memory_order_acquire);
        // search back from (end - shown) for a rising zero crossing
        int start = end - shown;
        for (int i = start; i > end - size / 2; --i) {
            if (buffer[(size_t) ((i - 1) & mask)] < 0 && buffer[(size_t) (i & mask)] >= 0) { start = i; break; }
        }
        juce::Path p;
        for (int i = 0; i < shown; ++i) {
            const float y = juce::jlimit(-1.0f, 1.0f, buffer[(size_t) ((start + i) & mask)]);
            const float px = r.getX() + r.getWidth() * (float) i / (float) (shown - 1);
            const float py = r.getCentreY() - y * r.getHeight() * 0.48f;
            if (i == 0) p.startNewSubPath(px, py); else p.lineTo(px, py);
        }
        g.setColour(kTrace);
        g.strokePath(p, juce::PathStrokeType(1.2f));
    }

private:
    void timerCallback() override { repaint(); }

    static constexpr int size = 8192, mask = size - 1;
    std::array<float, size> buffer {};
    std::atomic<int> write { 0 };
};

class Bench : public juce::Component,
              public juce::AudioIODeviceCallback,
              public juce::FileDragAndDropTarget,
              private juce::Timer {
public:
    explicit Bench(const juce::String& commandLine)
    {
        afm.registerBasicFormats();
        readThread.startThread();

        for (auto* b : { &openButton, &playButton, &bypassButton, &resetButton, &audioButton, &searchButton })
            addAndMakeVisible(b);
        bypassButton.setClickingTogglesState(true);

        sourceBox.addItem("File (loop)", 1);
        sourceBox.addItem("Live input", 2);
        sourceBox.setSelectedId(1, juce::dontSendNotification);
        addAndMakeVisible(sourceBox);

        circuitBox.addItem("Fuzz workbench", 1);
        {
            const auto names = cd::catalog::names();
            for (size_t k = 0; k < names.size(); ++k) circuitBox.addItem(names[k], (int) k + 2);
        }
        circuitBox.setSelectedId(1, juce::dontSendNotification);
        circuitBox.onChange = [this] { selectCircuit(circuitBox.getSelectedId() - 1); };
        addAndMakeVisible(circuitBox);
        addChildComponent(meter);
        addChildComponent(loopView);
        addChildComponent(grView);
        controlsViewport.setViewedComponent(&netControls, false);
        controlsViewport.setScrollBarsShown(true, false);
        addChildComponent(controlsViewport);

        rebuildPresetBox();
        presetBox.setSelectedId(1, juce::dontSendNotification);
        presetBox.setTextWhenNothingSelected("Preset");
        addAndMakeVisible(presetBox);

        for (auto* l : { &fileLabel, &statsLabel }) {
            l->setFont(juce::FontOptions(13.0f));
            l->setColour(juce::Label::textColourId, juce::Colours::lightgrey);
            addAndMakeVisible(l);
        }
        addAndMakeVisible(scope);
        addAndMakeVisible(circuitView);

        editor = std::make_unique<juce::GenericAudioProcessorEditor>(processor);
        addAndMakeVisible(*editor);

        openButton.onClick = [this] { chooseFile(); };
        playButton.onClick = [this] {
            if (transport.isPlaying()) transport.stop(); else transport.start();
            updatePlayButton();
        };
        bypassButton.onClick = [this] { bypass = bypassButton.getToggleState(); };
        resetButton.onClick = [this] {
            if (mode == 0) processor.engine.requestCircuitReset();
            else net.requestWarmStart();
        };
        audioButton.onClick = [this] { showAudioSettings(); };
        sourceBox.onChange = [this] { liveInput = sourceBox.getSelectedId() == 2; };
        presetBox.onChange = [this] {
            const int id = presetBox.getSelectedId();
            if (id >= kSearchIdBase) {
                const auto& v = searchKnobs[(size_t) (id - kSearchIdBase)];
                processor.setKnobs(v.data(), true);  // elites carry a loudness-matched Output
            } else if (id > 0) {
                processor.setCurrentProgram(id - 1);
            }
        };
        searchButton.onClick = [this] { chooseSearch(); };

        const auto err = deviceManager.initialiseWithDefaultDevices(2, 2);
        if (err.isNotEmpty()) statsLabel.setText("audio: " + err, juce::dontSendNotification);
        deviceManager.addAudioCallback(this);

        // args: an audio file to loop and/or a search run's elites.json
        juce::File initial(CD_DEFAULT_INPUT), search;
        juce::StringArray args;
        args.addTokens(commandLine, true);
        bool play = false;
        int program = -1, circuit = 0;
        for (int i = 0; i < args.size(); ++i) {
            const auto a = args[i].unquoted();
            if (a == "--play") { play = true; continue; }
            if (a == "--mute") { muted = true; continue; }
            if (a == "--program" && i + 1 < args.size()) { program = args[++i].getIntValue(); continue; }
            if (a == "--circuit" && i + 1 < args.size()) { circuit = args[++i].getIntValue(); continue; }
            const auto f = juce::File::getCurrentWorkingDirectory().getChildFile(a);
            if (f.hasFileExtension("json")) search = f; else if (a.isNotEmpty()) initial = f;
        }
        if (program >= 0) processor.setCurrentProgram(program);
        if (initial.existsAsFile()) loadFile(initial, play);  // paused unless --play: no surprise noise
        else fileLabel.setText("no file loaded: Open... or drop a WAV here", juce::dontSendNotification);
        if (!search.existsAsFile()) search = newestSearch();
        if (search.existsAsFile()) loadSearch(search, false);

        startTimerHz(10);
        setSize(1380, 860);
        if (circuit > 0) circuitBox.setSelectedId(circuit + 1);   // triggers selectCircuit
    }

    ~Bench() override
    {
        deviceManager.removeAudioCallback(this);
        transport.setSource(nullptr);
        editor.reset();
    }

    void paint(juce::Graphics& g) override { g.fillAll(kBackground); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(8);
        auto row = r.removeFromTop(28);
        for (auto* b : { &openButton, &playButton, &bypassButton, &resetButton, &audioButton, &searchButton }) {
            b->setBounds(row.removeFromLeft(b == &searchButton ? 110 : 80));
            row.removeFromLeft(6);
        }
        sourceBox.setBounds(row.removeFromLeft(110));
        row.removeFromLeft(6);
        circuitBox.setBounds(row.removeFromLeft(230));
        row.removeFromLeft(6);
        presetBox.setBounds(row);
        r.removeFromTop(6);
        fileLabel.setBounds(r.removeFromTop(20));
        statsLabel.setBounds(r.removeFromTop(20));
        r.removeFromTop(4);
        auto side = r.removeFromRight(420);
        editor->setBounds(side);
        controlsViewport.setBounds(side);
        netControls.setSize(side.getWidth() - controlsViewport.getScrollBarThickness(), netControls.getHeight());
        r.removeFromRight(8);
        scope.setBounds(r.removeFromBottom(140));
        r.removeFromBottom(8);
        circuitView.setBounds(r);
        const int panes = 1 + (net.uiHasCore ? 1 : 0) + (net.uiHasGainReduction ? 1 : 0);
        const int w = (r.getWidth() - 8 * (panes - 1)) / panes;
        meter.setBounds(r.removeFromLeft(w));
        if (net.uiHasGainReduction) {
            r.removeFromLeft(8);
            grView.setBounds(r.removeFromLeft(w));
        }
        if (net.uiHasCore) {
            r.removeFromLeft(8);
            loopView.setBounds(r);
        }
    }

    // ---- audio ----------------------------------------------------------
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override
    {
        const double sr = device->getCurrentSampleRate();
        const int bs = device->getCurrentBufferSizeSamples();
        work.setSize(2, bs);
        transport.prepareToPlay(bs, sr);
        processor.setPlayConfigDetails(2, 2, sr, bs);
        processor.prepareToPlay(sr, bs);
        net.prepare(sr, bs);
    }

    void audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut,
                                          int n, const juce::AudioIODeviceCallbackContext&) override
    {
        work.setSize(2, n, false, false, true);
        if (liveInput) {
            for (int c = 0; c < 2; ++c) {
                if (numIn > 0 && in[juce::jmin(c, numIn - 1)] != nullptr)
                    work.copyFrom(c, 0, in[juce::jmin(c, numIn - 1)], n);
                else
                    work.clear(c, 0, n);
            }
        } else {
            juce::AudioSourceChannelInfo info(&work, 0, n);
            transport.getNextAudioBlock(info);
        }
        if (!bypass) {
            if (mode == 0) processor.processBlock(work, midi);
            else net.process(work, n);
        }
        scope.push(work.getReadPointer(0), n);
        for (int c = 0; c < numOut; ++c) {
            if (out[c] == nullptr) continue;
            if (muted) juce::FloatVectorOperations::clear(out[c], n);
            else juce::FloatVectorOperations::copy(out[c], work.getReadPointer(juce::jmin(c, 1)), n);
        }
    }

    void audioDeviceStopped() override
    {
        transport.releaseResources();
        processor.releaseResources();
    }

    // ---- files ----------------------------------------------------------
    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        return files.size() == 1 && afm.findFormatForFileExtension(juce::File(files[0]).getFileExtension()) != nullptr;
    }

    void filesDropped(const juce::StringArray& files, int, int) override { loadFile(juce::File(files[0])); }

private:
    void loadFile(const juce::File& f, bool play = true)
    {
        auto* reader = afm.createReaderFor(f);
        if (reader == nullptr) {
            fileLabel.setText("can't read " + f.getFileName(), juce::dontSendNotification);
            return;
        }
        const double fileRate = reader->sampleRate;
        auto source = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
        source->setLooping(true);
        transport.stop();
        transport.setSource(nullptr);
        readerSource = std::move(source);
        transport.setSource(readerSource.get(), 32768, &readThread, fileRate);
        if (play) transport.start();
        sourceBox.setSelectedId(1);
        fileLabel.setText(f.getFullPathName(), juce::dontSendNotification);
        updatePlayButton();
    }

    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser>("Input audio", juce::File(CD_DEFAULT_INPUT).getParentDirectory(),
                                                      afm.getWildcardForAllFormats());
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser& fc) {
                                 if (fc.getResult().existsAsFile()) loadFile(fc.getResult());
                             });
    }

    // Adds a search run's elites (search/explore.py output) to the preset menu.
    void chooseSearch()
    {
        chooser = std::make_unique<juce::FileChooser>("Search results", juce::File(CD_SEARCH_DIR), "elites.json");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [this](const juce::FileChooser& fc) {
                                 if (fc.getResult().existsAsFile()) loadSearch(fc.getResult());
                             });
    }

    // The most recent search/runs/*/elites.json, so found sounds are in the
    // preset menu without having to load them.
    static juce::File newestSearch()
    {
        juce::File best;
        for (const auto& entry : juce::RangedDirectoryIterator(juce::File(CD_SEARCH_DIR), true, "elites.json"))
            if (best == juce::File() || entry.getModificationTime() > best.getLastModificationTime())
                best = entry.getFile();
        return best;
    }

    void loadSearch(const juce::File& f, bool showMenu = true)
    {
        const auto doc = juce::JSON::parse(f);
        const auto* elites = doc["elites"].getArray();
        if (elites == nullptr) {
            fileLabel.setText("not a search result: " + f.getFullPathName(), juce::dontSendNotification);
            return;
        }
        searchKnobs.clear();
        rebuildPresetBox();
        presetBox.addSectionHeading("Found by search: " + f.getParentDirectory().getFileName());
        for (const auto& e : *elites) {
            std::array<double, cd::kNumKnobs> v {};
            for (int i = 0; i < cd::kNumKnobs; ++i) {
                const auto k = e["knobs"][cd::kKnobs[i].id];
                v[(size_t) i] = k.isVoid() ? cd::kKnobs[i].def : (double) k;
            }
            presetBox.addItem(e["name"].toString(), kSearchIdBase + (int) searchKnobs.size());
            searchKnobs.push_back(v);
        }
        searchLabel = juce::String(elites->size()) + " found presets from search/"
                      + f.getParentDirectory().getParentDirectory().getFileName() + "/"
                      + f.getParentDirectory().getFileName();
        if (showMenu) presetBox.showPopup();
    }

    // 0 = the fuzz workbench; k > 0 = netlist circuit k-1 from the catalog
    void selectCircuit(int m)
    {
        if (m > 0) {
            net.select(m - 1);
            netControls.setControls(net.uiControls);
            meter.setProbes(net.uiProbes);
        }
        mode = m;
        const bool fuzz = m == 0;
        circuitView.setVisible(fuzz);
        editor->setVisible(fuzz);
        presetBox.setEnabled(fuzz);
        searchButton.setEnabled(fuzz);
        meter.setVisible(!fuzz);
        loopView.setVisible(!fuzz && net.uiHasCore);
        grView.setVisible(!fuzz && net.uiHasGainReduction);
        grView.clear();
        controlsViewport.setVisible(!fuzz);
        resized();
    }

    void rebuildPresetBox()
    {
        presetBox.clear(juce::dontSendNotification);
        presetBox.addSectionHeading("Hand-made");
        for (int i = 0; i < processor.getNumPrograms(); ++i)
            presetBox.addItem(processor.getProgramName(i), i + 1);
    }

    void showAudioSettings()
    {
        auto* selector = new juce::AudioDeviceSelectorComponent(deviceManager, 0, 2, 0, 2, false, false, true, false);
        selector->setSize(500, 400);
        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned(selector);
        o.dialogTitle = "Audio settings";
        o.dialogBackgroundColour = kBackground;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    void updatePlayButton() { playButton.setButtonText(transport.isPlaying() ? "Stop" : "Play"); }

    void timerCallback() override
    {
        net.collect();
        const float newton = mode == 0 ? processor.engine.newtonAverage.load() : net.newtonAverage.load();
        const int newtonMax = mode == 0 ? processor.engine.newtonMax.load() : 0;
        const long fails = mode == 0 ? processor.engine.solverFailures.load() : net.failures.load();
        statsLabel.setText((mode == 0 && searchLabel.isNotEmpty() ? searchLabel + "   |   " : juce::String())
                               + juce::String::formatted("CPU %.1f%%   Newton avg %.2f%s   solver failures %ld",
                                                         deviceManager.getCpuUsage() * 100.0, newton,
                                                         mode == 0 ? juce::String::formatted(" / max %d", newtonMax).toRawUTF8() : "",
                                                         fails),
                           juce::dontSendNotification);
        // follow program changes made elsewhere (not while a search result is selected)
        const int sel = presetBox.getSelectedId();
        if (sel < kSearchIdBase && sel != processor.getCurrentProgram() + 1)
            presetBox.setSelectedId(processor.getCurrentProgram() + 1, juce::dontSendNotification);
    }

    CircuitDecimatorProcessor processor;
    juce::AudioDeviceManager deviceManager;
    juce::AudioFormatManager afm;
    juce::TimeSliceThread readThread { "file reader" };
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;
    juce::AudioTransportSource transport;
    juce::AudioBuffer<float> work;
    juce::MidiBuffer midi;
    std::atomic<bool> liveInput { false }, bypass { false }, muted { false };

    juce::TextButton openButton { "Open..." }, playButton { "Play" }, bypassButton { "Bypass" },
        resetButton { "Power cycle" }, audioButton { "Audio..." }, searchButton { "Load search..." };
    static constexpr int kSearchIdBase = 1000;
    std::vector<std::array<double, cd::kNumKnobs>> searchKnobs;
    juce::String searchLabel;
    juce::ComboBox sourceBox, presetBox;
    juce::Label fileLabel, statsLabel;
    Scope scope;
    CircuitView circuitView { processor.engine };
    NetBench net;
    std::atomic<int> mode { 0 };
    juce::ComboBox circuitBox;
    cd::ui::NodeMeterView meter { net.probes };
    cd::ui::BHLoopView loopView { net.trace };
    cd::ui::GainReductionView grView { net.gainReduction };
    NetControls netControls { net };
    juce::Viewport controlsViewport;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<juce::FileChooser> chooser;
};

class BenchApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Circuit Bench"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String& commandLine) override { window = std::make_unique<Window>(commandLine); }
    void shutdown() override { window.reset(); }

private:
    struct Window : juce::DocumentWindow {
        explicit Window(const juce::String& commandLine)
            : DocumentWindow("Circuit Bench", kBackground, allButtons)
        {
            setUsingNativeTitleBar(true);
            setContentOwned(new Bench(commandLine), true);
            setResizable(true, false);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }
        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };
    std::unique_ptr<Window> window;
};

} // namespace

START_JUCE_APPLICATION(BenchApp)
