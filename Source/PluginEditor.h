#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include "PluginProcessor.h"

namespace ui
{
    // Paleta: corpo off-white, knobs laranja.
    const juce::Colour paper     { 0xffece7dd };
    const juce::Colour paperDark { 0xffdcd5c8 };
    const juce::Colour ink       { 0xff2b2926 };
    const juce::Colour inkSoft   { 0xff7a746a };
    const juce::Colour orange    { 0xfff26a1b };
    const juce::Colour orangeDk  { 0xffc4500e };
    const juce::Colour screen    { 0xff1d1b19 };
}

class LimpaTudoLook : public juce::LookAndFeel_V4
{
public:
    LimpaTudoLook();

    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool over, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
};

// Tela escura com o espectro: linha branca = entrada, área laranja = saída.
class SpectrumView : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumView (LimpaTudoProcessor&);
    ~SpectrumView() override { stopTimer(); }
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    void analyse (const ScopeBuffer&, std::array<float, 1024>& dst);

    static constexpr int kOrder = 11;          // 2048 pontos
    static constexpr int kFftSize = 1 << kOrder;

    LimpaTudoProcessor& proc;
    juce::dsp::FFT fft { kOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) kFftSize, juce::dsp::WindowingFunction<float>::hann };
    std::array<float, 1024> inDb {}, outDb {};
    std::array<float, kFftSize * 2> work {};
    std::array<float, kFftSize> frame {};
};

class LimpaTudoEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit LimpaTudoEditor (LimpaTudoProcessor&);
    ~LimpaTudoEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;

    struct KnobBlock
    {
        juce::Slider slider;
        juce::Label title;
        juce::TextButton solo { "S" };
        std::unique_ptr<SliderAtt> att;
    };

    void timerCallback() override;
    void setChoice (const char* id, int index);
    int getChoice (const char* id) const;
    void setupKnob (KnobBlock&, const char* paramId, const juce::String& title, bool big, bool withSolo);
    void refreshLearnUi();

    LimpaTudoProcessor& proc;
    LimpaTudoLook look;
    SpectrumView spectrum;

    KnobBlock ambience, source, noise;                     // os 3 knobs principais
    KnobBlock smooth, tail, limit, lowCut, gain;           // ajuste fino
    juce::TextButton modeVoice { "Voz" }, modeGuitar { juce::String::fromUTF8 ("Violão") }, modeOther { "Outro" };
    juce::TextButton learnBtn { juce::String::fromUTF8 ("Aprender ruído") };
    juce::TextButton clearBtn { "Limpar" };
    juce::Label profileInfo;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimpaTudoEditor)
};
