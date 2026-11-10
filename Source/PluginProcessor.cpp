#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    // IDs fixos: não mudar depois de lançar (projetos salvos dependem deles)
    constexpr const char* kSource   = "fonte";
    constexpr const char* kAmbience = "ambiencia";
    constexpr const char* kNoise    = "ruido";
    constexpr const char* kMode     = "modo";
    constexpr const char* kSmooth   = "suavidade";
    constexpr const char* kTail     = "cauda";
    constexpr const char* kLimit    = "limite";
    constexpr const char* kLowCut   = "corte";
    constexpr const char* kGain     = "ganho";
    constexpr const char* kListen   = "ouvir";
}

LimpaTudoProcessor::LimpaTudoProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Entrada", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Saida",   juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETROS", createLayout())
{
    sourceParam   = apvts.getRawParameterValue (kSource);
    ambienceParam = apvts.getRawParameterValue (kAmbience);
    noiseParam    = apvts.getRawParameterValue (kNoise);
    modeParam     = apvts.getRawParameterValue (kMode);
    smoothParam   = apvts.getRawParameterValue (kSmooth);
    tailParam     = apvts.getRawParameterValue (kTail);
    limitParam    = apvts.getRawParameterValue (kLimit);
    lowCutParam   = apvts.getRawParameterValue (kLowCut);
    gainParam     = apvts.getRawParameterValue (kGain);
    listenParam   = apvts.getRawParameterValue (kListen);
}

juce::AudioProcessorValueTreeState::ParameterLayout LimpaTudoProcessor::createLayout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> params;

    auto pct = [] (float v, int) { return String (roundToInt (v)) + " %"; };

    // --- os 3 knobs principais ---
    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { kAmbience, 1 }, String::fromUTF8 ("Ambiência (remover)"),
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (pct)));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { kSource, 1 }, "Fonte (voz / instrumento)",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (pct)));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { kNoise, 1 }, String::fromUTF8 ("Ruído (devolver)"),
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (pct)));

    // --- modo e escuta ---
    params.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { kMode, 1 }, "Modo",
        StringArray { "Voz", String::fromUTF8 ("Violão"), "Outro" }, 0));

    params.push_back (std::make_unique<AudioParameterChoice> (
        ParameterID { kListen, 1 }, "Ouvir (solo)",
        StringArray { "Normal", "So a fonte", String::fromUTF8 ("So a ambiência"), String::fromUTF8 ("So o ruído") }, 0));

    // --- ajuste fino dos glitches ---
    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { kSmooth, 1 }, "Suavidade",
        NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f,
        AudioParameterFloatAttributes().withStringFromValueFunction (pct)));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { kTail, 1 }, "Cauda",
        NormalisableRange<float> (10.0f, 600.0f, 1.0f, 0.5f), 120.0f,
        AudioParameterFloatAttributes().withLabel ("ms")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { kLimit, 1 }, String::fromUTF8 ("Limite de redução"),
        NormalisableRange<float> (3.0f, 60.0f, 0.5f), 30.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    // --- acabamento ---
    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { kLowCut, 1 }, "Corte de graves",
        NormalisableRange<float> (20.0f, 300.0f, 1.0f, 0.5f), 40.0f,
        AudioParameterFloatAttributes().withLabel ("Hz")));

    params.push_back (std::make_unique<AudioParameterFloat> (
        ParameterID { kGain, 1 }, String::fromUTF8 ("Ganho de saída"),
        NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));

    return { params.begin(), params.end() };
}

bool LimpaTudoProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void LimpaTudoProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, std::max (1, getTotalNumInputChannels()));

    gainSmoothed.reset (sampleRate, 0.02);
    gainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (gainParam->load()));

    monoScratch.assign ((size_t) std::max (samplesPerBlock, 1), 0.0f);

    // se o projeto trouxe um perfil de ruído salvo, entrega ao motor
    {
        const juce::SpinLock::ScopedLockType lock (profileLock);
        engine.setNoiseProfile (profileCopy, profileValid.load());
    }

    // informa ao host a latência, para ele compensar automaticamente
    setLatencySamples (engine.getLatencySamples());
}

void LimpaTudoProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numChannels = std::min (buffer.getNumChannels(), limpatudo::kMaxCh);

    // perfil carregado de um projeto salvo
    if (pendingLoad.load())
    {
        if (profileLock.tryEnter())
        {
            engine.setNoiseProfile (pendingProfile, pendingProfileValid);
            profileCopy = pendingProfile;
            profileValid.store (pendingProfileValid);
            pendingLoad.store (false);
            profileLock.exit();
        }
    }

    limpatudo::Params p;
    p.mode = (limpatudo::Mode) juce::jlimit (0, 2, (int) std::lround (modeParam->load()));
    p.source = sourceParam->load() * 0.01f;
    p.ambience = ambienceParam->load() * 0.01f;
    p.noise = noiseParam->load() * 0.01f;
    p.smooth = smoothParam->load() * 0.01f;
    p.tailMs = tailParam->load();
    p.limitDb = limitParam->load();
    p.listen = (limpatudo::Listen) juce::jlimit (0, 3, (int) std::lround (listenParam->load()));
    p.learning = learning.load();
    p.clearLearned = clearProfileRequest.exchange (false);

    // espectro de entrada (mono) para a interface
    for (int i = 0; i < numSamples; ++i)
    {
        float s = buffer.getReadPointer (0)[i];
        if (numChannels > 1) s = 0.5f * (s + buffer.getReadPointer (1)[i]);
        scopeIn.push (s);
    }

    float* channels[limpatudo::kMaxCh] { buffer.getWritePointer (0), numChannels > 1 ? buffer.getWritePointer (1) : nullptr };
    engine.process (channels, numSamples, p, lowCutParam->load());

    // ganho de saída com suavização (evita estalos ao mexer no controle)
    gainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (gainParam->load()));
    if (gainSmoothed.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = gainSmoothed.getNextValue();
            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch)[i] *= g;
        }
    }
    else
    {
        buffer.applyGain (gainSmoothed.getTargetValue());
    }

    for (int i = 0; i < numSamples; ++i)
    {
        float s = buffer.getReadPointer (0)[i];
        if (numChannels > 1) s = 0.5f * (s + buffer.getReadPointer (1)[i]);
        scopeOut.push (s);
    }

    // se o motor terminou de aprender um perfil novo, guarda uma cópia para salvar no projeto
    if (profileLock.tryEnter())
    {
        if (engine.takeNewProfileFlag())
        {
            engine.getNoiseProfile (profileCopy);
            profileValid.store (engine.hasNoiseProfile());
        }
        profileLock.exit();
    }
}

juce::AudioProcessorEditor* LimpaTudoProcessor::createEditor()
{
    return new LimpaTudoEditor (*this);
}

void LimpaTudoProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();

    {
        const juce::SpinLock::ScopedLockType lock (profileLock);
        if (profileValid.load())
        {
            juce::MemoryBlock mb (profileCopy.data(), sizeof (float) * profileCopy.size());
            state.setProperty ("perfilRuido", mb.toBase64Encoding(), nullptr);
        }
        else
        {
            state.removeProperty ("perfilRuido", nullptr);
        }
    }

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void LimpaTudoProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);

    std::array<float, limpatudo::kBins> prof {};
    bool valid = false;
    if (state.hasProperty ("perfilRuido"))
    {
        juce::MemoryBlock mb;
        if (mb.fromBase64Encoding (state["perfilRuido"].toString()) && mb.getSize() == sizeof (float) * prof.size())
        {
            std::memcpy (prof.data(), mb.getData(), mb.getSize());
            valid = true;
        }
        state.removeProperty ("perfilRuido", nullptr);
    }

    apvts.replaceState (state);

    {
        const juce::SpinLock::ScopedLockType lock (profileLock);
        pendingProfile = prof;
        pendingProfileValid = valid;
        pendingLoad.store (true);
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LimpaTudoProcessor();
}
