#include "PluginEditor.h"

using namespace ui;

// ============================================================================================
LimpaTudoLook::LimpaTudoLook()
{
    setColour (juce::Slider::textBoxTextColourId, ink);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textColourId, ink);
}

juce::Font LimpaTudoLook::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::Font (juce::FontOptions ((float) juce::jmin (15, buttonHeight * 6 / 10), juce::Font::bold));
}

void LimpaTudoLook::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                      float startAngle, float endAngle, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (4.0f);
    const float diameter = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto centre = bounds.getCentre();
    const float r = diameter * 0.5f;
    const float knobR = r * 0.72f;
    const float angle = startAngle + pos * (endAngle - startAngle);

    // marcas ao redor (a cada 10 %)
    for (int i = 0; i <= 10; ++i)
    {
        const float a = startAngle + (float) i / 10.0f * (endAngle - startAngle);
        const bool major = (i % 5) == 0;
        const float r0 = r * (major ? 0.86f : 0.90f), r1 = r * 0.98f;
        g.setColour (i <= (int) std::round (pos * 10.0f) ? orange : inkSoft.withAlpha (0.55f));
        g.drawLine (centre.x + std::sin (a) * r0, centre.y - std::cos (a) * r0,
                    centre.x + std::sin (a) * r1, centre.y - std::cos (a) * r1, major ? 2.2f : 1.4f);
    }

    // sombra e corpo do knob
    g.setColour (juce::Colours::black.withAlpha (0.18f));
    g.fillEllipse (centre.x - knobR + 1.5f, centre.y - knobR + 3.5f, knobR * 2.0f, knobR * 2.0f);

    juce::ColourGradient body (orange.brighter (0.18f), centre.x - knobR * 0.5f, centre.y - knobR * 0.8f,
                               orangeDk, centre.x + knobR * 0.6f, centre.y + knobR, false);
    g.setGradientFill (body);
    g.fillEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f);

    g.setColour (orangeDk.darker (0.3f));
    g.drawEllipse (centre.x - knobR, centre.y - knobR, knobR * 2.0f, knobR * 2.0f, 1.5f);

    // topo levemente mais claro
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.fillEllipse (centre.x - knobR * 0.78f, centre.y - knobR * 0.84f, knobR * 1.56f, knobR * 1.2f);

    // ponteiro
    juce::Path p;
    const float len = knobR * 0.62f;
    p.addRoundedRectangle (-2.2f, -knobR * 0.92f, 4.4f, len, 2.0f);
    g.setColour (paper);
    g.fillPath (p, juce::AffineTransform::rotation (angle).translated (centre.x, centre.y));

    juce::ignoreUnused (s);
}

void LimpaTudoLook::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = b.getToggleState();

    juce::Colour fill = on ? orange : paperDark;
    if (over) fill = fill.brighter (0.06f);
    if (down) fill = fill.darker (0.10f);

    g.setColour (fill);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (on ? orangeDk : inkSoft.withAlpha (0.45f));
    g.drawRoundedRectangle (r, 6.0f, 1.2f);
}

void LimpaTudoLook::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool)
{
    g.setFont (getTextButtonFont (b, b.getHeight()));
    g.setColour (b.getToggleState() ? juce::Colours::white : ink);
    g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (4, 0), juce::Justification::centred, 1);
}

// ============================================================================================
SpectrumView::SpectrumView (LimpaTudoProcessor& p) : proc (p)
{
    inDb.fill (-100.0f);
    outDb.fill (-100.0f);
    startTimerHz (30);
}

void SpectrumView::analyse (const ScopeBuffer& sb, std::array<float, 1024>& dst)
{
    sb.copyLatest (frame.data(), kFftSize);
    std::copy (frame.begin(), frame.end(), work.begin());
    std::fill (work.begin() + kFftSize, work.end(), 0.0f);
    window.multiplyWithWindowingTable (work.data(), (size_t) kFftSize);
    fft.performFrequencyOnlyForwardTransform (work.data());

    for (size_t i = 0; i < dst.size(); ++i)
    {
        const float mag = work[i] / (float) (kFftSize / 4);
        const float db = juce::Decibels::gainToDecibels (mag, -100.0f);
        dst[i] = db > dst[i] ? db : dst[i] * 0.80f + db * 0.20f;      // sobe rápido, desce devagar
    }
}

void SpectrumView::timerCallback()
{
    analyse (proc.scopeIn, inDb);
    analyse (proc.scopeOut, outDb);
    repaint();
}

void SpectrumView::paint (juce::Graphics& g)
{
    const auto area = getLocalBounds().toFloat();
    g.setColour (screen);
    g.fillRoundedRectangle (area, 8.0f);

    const auto plot = area.reduced (10.0f, 8.0f).withTrimmedBottom (16.0f);
    const double sr = std::max (proc.getSampleRate(), 8000.0);

    auto xForFreq = [&] (double f)
    {
        const double lo = std::log10 (30.0), hi = std::log10 (std::min (20000.0, sr * 0.5));
        return plot.getX() + (float) ((std::log10 (std::max (f, 30.0)) - lo) / (hi - lo)) * plot.getWidth();
    };
    auto yForDb = [&] (float db)
    {
        const float t = juce::jlimit (0.0f, 1.0f, (db + 100.0f) / 90.0f);       // -100 .. -10 dB
        return plot.getBottom() - t * plot.getHeight();
    };

    // grade e legenda de frequência
    g.setFont (juce::FontOptions (11.0f));
    for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
    {
        const float x = xForFreq (f);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "k" : juce::String ((int) f),
                    (int) x - 16, (int) plot.getBottom() + 2, 32, 14, juce::Justification::centred);
    }

    auto makePath = [&] (const std::array<float, 1024>& d, bool closed)
    {
        juce::Path p;
        bool started = false;
        for (size_t i = 2; i < d.size(); ++i)
        {
            const double f = (double) i * sr / (double) kFftSize;
            if (f < 30.0 || f > std::min (20000.0, sr * 0.5)) continue;
            const float x = xForFreq (f), y = yForDb (d[i]);
            if (! started) { p.startNewSubPath (x, closed ? plot.getBottom() : y); if (closed) p.lineTo (x, y); started = true; }
            else p.lineTo (x, y);
        }
        if (started && closed) { p.lineTo (plot.getRight(), plot.getBottom()); p.closeSubPath(); }
        return p;
    };

    g.setColour (orange.withAlpha (0.55f));
    g.fillPath (makePath (outDb, true));
    g.setColour (orange);
    g.strokePath (makePath (outDb, false), juce::PathStrokeType (1.2f));

    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.strokePath (makePath (inDb, false), juce::PathStrokeType (1.2f));

    g.setFont (juce::FontOptions (11.0f));
    g.setColour (juce::Colours::white.withAlpha (0.55f));
    g.drawText (juce::String::fromUTF8 ("linha branca: entrada   |   laranja: saída"),
                getLocalBounds().removeFromTop (20).reduced (12, 0), juce::Justification::centredRight);
}

// ============================================================================================
LimpaTudoEditor::LimpaTudoEditor (LimpaTudoProcessor& p)
    : AudioProcessorEditor (&p), proc (p), spectrum (p)
{
    setLookAndFeel (&look);
    setSize (760, 690);

    addAndMakeVisible (spectrum);

    setupKnob (ambience, "ambiencia", juce::String::fromUTF8 ("AMBIÊNCIA"), true, true);
    setupKnob (source,   "fonte",     "FONTE",                              true, true);
    setupKnob (noise,    "ruido",     juce::String::fromUTF8 ("RUÍDO"),      true, true);

    setupKnob (smooth, "suavidade", "SUAVIDADE", false, false);
    setupKnob (tail,   "cauda",     "CAUDA",     false, false);
    setupKnob (limit,  "limite",    "LIMITE",    false, false);
    setupKnob (lowCut, "corte",     "GRAVES",    false, false);
    setupKnob (gain,   "ganho",     "GANHO",     false, false);

    // S de cada knob = "ouvir só isso" (exclusivo)
    ambience.solo.onClick = [this] { setChoice ("ouvir", getChoice ("ouvir") == 2 ? 0 : 2); };
    source.solo.onClick   = [this] { setChoice ("ouvir", getChoice ("ouvir") == 1 ? 0 : 1); };
    noise.solo.onClick    = [this] { setChoice ("ouvir", getChoice ("ouvir") == 3 ? 0 : 3); };

    for (auto* b : { &modeVoice, &modeGuitar, &modeOther })
    {
        b->setClickingTogglesState (false);
        addAndMakeVisible (*b);
    }
    modeVoice.onClick  = [this] { setChoice ("modo", 0); };
    modeGuitar.onClick = [this] { setChoice ("modo", 1); };
    modeOther.onClick  = [this] { setChoice ("modo", 2); };

    learnBtn.onClick = [this]
    {
        proc.learning.store (! proc.learning.load());
        refreshLearnUi();
    };
    clearBtn.onClick = [this]
    {
        proc.learning.store (false);
        proc.clearProfileRequest.store (true);
        proc.profileValid.store (false);
        refreshLearnUi();
    };
    addAndMakeVisible (learnBtn);
    addAndMakeVisible (clearBtn);

    profileInfo.setJustificationType (juce::Justification::centredLeft);
    profileInfo.setColour (juce::Label::textColourId, inkSoft);
    profileInfo.setFont (juce::FontOptions (13.0f));
    addAndMakeVisible (profileInfo);

    refreshLearnUi();
    startTimerHz (10);
}

LimpaTudoEditor::~LimpaTudoEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void LimpaTudoEditor::setupKnob (KnobBlock& k, const char* paramId, const juce::String& title, bool big, bool withSolo)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
    k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, big ? 90 : 70, 20);
    k.slider.setColour (juce::Slider::textBoxTextColourId, ink);
    k.slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    k.slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    k.slider.setDoubleClickReturnValue (true, proc.apvts.getParameter (paramId)->convertFrom0to1 (
                                                  proc.apvts.getParameter (paramId)->getDefaultValue()));
    addAndMakeVisible (k.slider);

    k.title.setText (title, juce::dontSendNotification);
    k.title.setJustificationType (juce::Justification::centred);
    k.title.setFont (juce::FontOptions (big ? 16.0f : 12.5f, juce::Font::bold));
    addAndMakeVisible (k.title);

    if (withSolo)
    {
        k.solo.setClickingTogglesState (false);
        k.solo.setTooltip (juce::String::fromUTF8 ("Ouvir só esta parte (clique de novo para voltar)"));
        addAndMakeVisible (k.solo);
    }

    k.att = std::make_unique<SliderAtt> (proc.apvts, paramId, k.slider);
}

int LimpaTudoEditor::getChoice (const char* id) const
{
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)))
        return p->getIndex();
    return 0;
}

void LimpaTudoEditor::setChoice (const char* id, int index)
{
    if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (id)))
    {
        p->beginChangeGesture();
        *p = index;
        p->endChangeGesture();
    }
}

void LimpaTudoEditor::refreshLearnUi()
{
    const bool learning = proc.learning.load();
    const bool valid = proc.profileValid.load();

    learnBtn.setToggleState (learning, juce::dontSendNotification);
    learnBtn.setButtonText (learning ? juce::String::fromUTF8 ("Parar e salvar ruído")
                                     : juce::String::fromUTF8 ("Aprender ruído"));
    clearBtn.setEnabled (valid || learning);

    if (learning)
        profileInfo.setText (juce::String::fromUTF8 ("Aprendendo… toque o áudio SÓ com o ruído (sem tocar/falar), depois pare."), juce::dontSendNotification);
    else if (valid)
        profileInfo.setText (juce::String::fromUTF8 ("Perfil de ruído salvo no projeto (usado no lugar da detecção automática)."), juce::dontSendNotification);
    else
        profileInfo.setText (juce::String::fromUTF8 ("Detecção automática do ruído. Para limpar melhor, aprenda 1–2 s só de ruído."), juce::dontSendNotification);
}

void LimpaTudoEditor::timerCallback()
{
    const int mode = getChoice ("modo"), listen = getChoice ("ouvir");
    modeVoice.setToggleState (mode == 0, juce::dontSendNotification);
    modeGuitar.setToggleState (mode == 1, juce::dontSendNotification);
    modeOther.setToggleState (mode == 2, juce::dontSendNotification);

    source.solo.setToggleState (listen == 1, juce::dontSendNotification);
    ambience.solo.setToggleState (listen == 2, juce::dontSendNotification);
    noise.solo.setToggleState (listen == 3, juce::dontSendNotification);

    refreshLearnUi();
}

void LimpaTudoEditor::paint (juce::Graphics& g)
{
    g.fillAll (paper);

    // faixa superior
    auto top = getLocalBounds().removeFromTop (58);
    g.setColour (ink);
    g.setFont (juce::FontOptions (28.0f, juce::Font::bold));
    g.drawText ("LimpaTudo", top.reduced (18, 0), juce::Justification::centredLeft);

    // painéis
    g.setColour (paperDark);
    g.fillRoundedRectangle (juce::Rectangle<float> (12.0f, 296.0f, (float) getWidth() - 24.0f, 214.0f), 10.0f);
    g.fillRoundedRectangle (juce::Rectangle<float> (12.0f, 520.0f, (float) getWidth() - 24.0f, 108.0f), 10.0f);
}

void LimpaTudoEditor::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (58);

    // modo (Voz / Violão / Outro) à direita do título
    auto modeArea = top.reduced (14, 12).removeFromRight (270);
    const int mw = modeArea.getWidth() / 3;
    modeVoice.setBounds (modeArea.removeFromLeft (mw).reduced (3, 0));
    modeGuitar.setBounds (modeArea.removeFromLeft (mw).reduced (3, 0));
    modeOther.setBounds (modeArea.reduced (3, 0));

    spectrum.setBounds (12, 62, getWidth() - 24, 226);

    // 3 knobs principais
    auto row = juce::Rectangle<int> (12, 296, getWidth() - 24, 214);
    const int colW = row.getWidth() / 3;
    KnobBlock* mains[] = { &ambience, &source, &noise };
    for (int i = 0; i < 3; ++i)
    {
        auto col = row.withX (row.getX() + i * colW).withWidth (colW);
        auto head = col.removeFromTop (30);
        mains[i]->title.setBounds (head.reduced (colW / 4, 4));
        mains[i]->solo.setBounds (head.getX() + 14, head.getY() + 5, 30, 24);
        mains[i]->slider.setBounds (col.reduced (22, 2));
    }

    // ajuste fino (5 knobs pequenos)
    auto fine = juce::Rectangle<int> (12, 520, getWidth() - 24, 108);
    const int fw = fine.getWidth() / 5;
    KnobBlock* fines[] = { &smooth, &tail, &limit, &lowCut, &gain };
    for (int i = 0; i < 5; ++i)
    {
        auto col = fine.withX (fine.getX() + i * fw).withWidth (fw);
        fines[i]->title.setBounds (col.removeFromTop (20));
        fines[i]->slider.setBounds (col.reduced (10, 0));
    }

    // linha do "aprender ruído" (rodapé)
    auto learnRow = juce::Rectangle<int> (12, 636, getWidth() - 24, 40);
    learnBtn.setBounds (learnRow.removeFromLeft (190).reduced (0, 4));
    learnRow.removeFromLeft (6);
    clearBtn.setBounds (learnRow.removeFromLeft (80).reduced (0, 4));
    learnRow.removeFromLeft (10);
    profileInfo.setBounds (learnRow);
}
