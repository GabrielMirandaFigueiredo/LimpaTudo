#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Engine.h"

// FIFO simples (sem travas) só para alimentar o analisador de espectro da interface.
class ScopeBuffer
{
public:
    static constexpr int kSize = 8192;

    void push (float v) noexcept
    {
        const int w = writeIdx.load (std::memory_order_relaxed);
        data[(size_t) (w & (kSize - 1))] = v;
        writeIdx.store (w + 1, std::memory_order_release);
    }

    // copia as últimas n amostras (n <= kSize)
    void copyLatest (float* dst, int n) const noexcept
    {
        const int w = writeIdx.load (std::memory_order_acquire);
        for (int i = 0; i < n; ++i)
            dst[i] = data[(size_t) ((w - n + i) & (kSize - 1))];
    }

private:
    std::array<float, kSize> data {};
    std::atomic<int> writeIdx { 0 };
};

class LimpaTudoProcessor : public juce::AudioProcessor
{
public:
    LimpaTudoProcessor();
    ~LimpaTudoProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "LimpaTudo"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- usado pela interface ---
    juce::AudioProcessorValueTreeState apvts;
    ScopeBuffer scopeIn, scopeOut;

    // "Aprender ruído": 3 estados na interface (sem perfil -> aprendendo -> perfil salvo)
    std::atomic<bool> learning { false };
    std::atomic<bool> clearProfileRequest { false };
    std::atomic<bool> profileValid { false };

    // 20 pontos da ordem de leitura dos parâmetros
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

private:
    limpatudo::Engine engine;
    juce::SmoothedValue<float> gainSmoothed;

    std::atomic<float>* sourceParam = nullptr;
    std::atomic<float>* ambienceParam = nullptr;
    std::atomic<float>* noiseParam = nullptr;
    std::atomic<float>* modeParam = nullptr;
    std::atomic<float>* smoothParam = nullptr;
    std::atomic<float>* tailParam = nullptr;
    std::atomic<float>* limitParam = nullptr;
    std::atomic<float>* lowCutParam = nullptr;
    std::atomic<float>* gainParam = nullptr;
    std::atomic<float>* listenParam = nullptr;

    // perfil de ruído aprendido: cópia protegida para salvar/carregar o projeto
    juce::SpinLock profileLock;
    std::array<float, limpatudo::kBins> profileCopy {};
    std::array<float, limpatudo::kBins> pendingProfile {};
    bool pendingProfileValid = false;
    std::atomic<bool> pendingLoad { false };

    std::vector<float> monoScratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimpaTudoProcessor)
};
