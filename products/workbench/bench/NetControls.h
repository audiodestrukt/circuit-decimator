// NetControls -- a scrolling panel of every component knob for the current
// netlist circuit, built from its Catalog control list, grouped by section.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "NetBench.h"

class NetControls : public juce::Component {
public:
    explicit NetControls(NetBench& b) : bench(b) {}

    void setControls(const std::vector<cd::catalog::Control>& ctl)
    {
        removeAllChildren();
        order.clear();
        rows.clear();
        headings.clear();
        const char* group = nullptr;
        for (size_t k = 0; k < ctl.size() && k < (size_t) NetBench::kMaxControls; ++k) {
            const auto& q = ctl[k];
            if (!group || std::strcmp(group, q.group) != 0) {
                group = q.group;
                auto& h = headings.emplace_back(std::make_unique<juce::Label>());
                h->setText(group, juce::dontSendNotification);
                h->setFont(juce::FontOptions(15.0f, juce::Font::bold));
                h->setColour(juce::Label::textColourId, juce::Colour(0xfff2b27a));
                addAndMakeVisible(*h);
                order.push_back({ h.get(), nullptr, nullptr });
            }
            auto row = std::make_unique<Row>();
            row->name.setText(q.knob.name, juce::dontSendNotification);
            row->name.setColour(juce::Label::textColourId, juce::Colour(0xffc9ccd1));
            juce::NormalisableRange<double> range(q.knob.min, q.knob.max);
            if (q.knob.centre > q.knob.min) range.setSkewForCentre(q.knob.centre);
            row->slider.setSliderStyle(juce::Slider::LinearHorizontal);
            row->slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 96, 20);
            row->slider.setNormalisableRange(range);
            row->slider.setNumDecimalPlacesToDisplay(q.knob.decimals);
            if (std::strlen(q.knob.unit) > 0) row->slider.setTextValueSuffix(juce::String(" ") + q.knob.unit);
            row->slider.setValue(q.knob.def, juce::dontSendNotification);
            row->slider.setDoubleClickReturnValue(true, q.knob.def);
            const size_t idx = k;
            row->slider.onValueChange = [this, idx, s = &row->slider] { bench.values[idx] = s->getValue(); };
            addAndMakeVisible(row->name);
            addAndMakeVisible(row->slider);
            order.push_back({ nullptr, &row->name, &row->slider });
            rows.push_back(std::move(row));
        }
        setSize(getWidth() > 0 ? getWidth() : 400, (int) order.size() * 28 + 8);
        resized();
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff232529)); }

    void resized() override
    {
        int y = 4;
        for (auto& o : order) {
            if (o.heading) {
                o.heading->setBounds(8, y + 4, getWidth() - 16, 22);
            } else {
                o.name->setBounds(8, y, 130, 24);
                o.slider->setBounds(140, y, getWidth() - 148, 24);
            }
            y += 28;
        }
    }

private:
    struct Row {
        juce::Label name;
        juce::Slider slider;
    };
    struct Item {
        juce::Label* heading;
        juce::Label* name;
        juce::Slider* slider;
    };
    NetBench& bench;
    std::vector<std::unique_ptr<Row>> rows;
    std::vector<std::unique_ptr<juce::Label>> headings;
    std::vector<Item> order;
};
