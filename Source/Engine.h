#pragma once

// Motor do LimpaTudo (sem dependência do JUCE, para poder ser testado sozinho).
//
// Ideia central: o áudio de entrada X é dividido em 3 componentes que SOMAM exatamente X:
//     Fonte     = X * G * Gd          (o que você quer: voz, violão...)
//     Ambiência = X * G * (1 - Gd)    (a "sala": cauda de reverb da fonte)
//     Ruído     = X * (1 - G)         (todo o resto: ventoinha, pássaros, carros...)
// e a saída é   Fonte*f + Ambiência*(1-amb) + Ruído*ruido.
// Com f=1, amb=0 e ruido=1 a saída é idêntica à entrada (só atrasada pela latência),
// então os erros do estimador nunca "vazam" para o áudio sem você mexer nos knobs.
//
// G (máscara de ruído) vem de:
//   - modo Voz   : RNNoise (rede neural) -> máscara = |saída da rede| / |entrada|
//   - modo Violão / Outro : estimador espectral (mínimos estatísticos + Wiener "decision-directed")
// Depois a máscara passa pelos controles anti-glitch (Limite, Suavidade, Cauda).
//
// Fluxo: host -> (reamostra p/ 48 kHz) -> STFT 1024/256 -> máscara -> mistura -> ISTFT
//        -> corte de graves -> (reamostra de volta p/ a taxa do host)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include "rnnoise.h"
}

namespace limpatudo
{

constexpr int kN = 1024;                 // tamanho da janela da STFT
constexpr int kHop = 256;                // passo (75 % de sobreposição)
constexpr int kBins = kN / 2 + 1;
constexpr int kFrame = 480;              // frame do RNNoise (10 ms a 48 kHz)
constexpr int kWetPrime = 480;
constexpr int kDryDelay = 1440;          // atraso do original para alinhar com a saída do RNNoise
constexpr int kLookahead = 2;            // quadros de antecipação da máscara (abre antes dos ataques)
constexpr int kLatency48 = kDryDelay + kN + kLookahead * kHop;   // latência total a 48 kHz (confirmada em tests/test_core.cpp)
constexpr int kChunk = 1024;
constexpr int kMaxExpand = 8;
constexpr int kScratch = kChunk * kMaxExpand + 64;
constexpr int kMaxCh = 2;
constexpr double kPi = 3.14159265358979323846;

// Ajustes internos (calibrados com os testes em tests/test_core.cpp).
namespace tuning
{
    constexpr float ddAlphaGuitar = 0.94f;   // memória da decisão "a priori" do Wiener (maior = menos chiado, ataques um pouco mais lentos)
    constexpr float ddAlphaOther  = 0.94f;
    constexpr float noiseBias     = 3.5f;    // compensa o viés dos mínimos estatísticos
    constexpr float kappaScale    = 1.6f;    // força do de-reverb
    constexpr float gammaRaw      = 1.0f;    // peso da potência bruta (1) x suavizada (0) na razão sinal/ruído
}

enum class Mode { Voice = 0, Guitar = 1, Other = 2 };
enum class Listen { Normal = 0, Source = 1, Ambience = 2, Noise = 3 };

struct Params
{
    Mode mode = Mode::Voice;
    float source = 1.0f;        // 0..1   volume da fonte
    float ambience = 0.0f;      // 0..1   quanto da ambiência é REMOVIDO (0 = não mexe)
    float noise = 0.0f;         // 0..1   quanto do ruído original é DEVOLVIDO (0 = nenhum)
    float smooth = 0.5f;        // 0..1   suavidade da máscara (menos "glitch" / chiado musical)
    float tailMs = 120.0f;      // ms     cauda (release) da máscara
    float limitDb = 30.0f;      // dB     redução máxima de ruído
    Listen listen = Listen::Normal;
    bool learning = false;      // true enquanto o botão "Aprender ruído" está ativo
    bool clearLearned = false;  // pulso: apaga o perfil de ruído aprendido
};

// ---------------------------------------------------------------------------------------------
class Biquad
{
public:
    void setHighPass (double sampleRate, double freq, double q = 0.70710678)
    {
        const double w0 = 2.0 * kPi * freq / sampleRate;
        const double cw = std::cos (w0), sw = std::sin (w0), alpha = sw / (2.0 * q);
        set ((1 + cw) / 2, -(1 + cw), (1 + cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    void setLowPass (double sampleRate, double freq, double q = 0.70710678)
    {
        const double w0 = 2.0 * kPi * freq / sampleRate;
        const double cw = std::cos (w0), sw = std::sin (w0), alpha = sw / (2.0 * q);
        set ((1 - cw) / 2, 1 - cw, (1 - cw) / 2, 1 + alpha, -2 * cw, 1 - alpha);
    }

    void setBypass() { set (1, 0, 0, 1, 0, 0); }
    void reset() { z1 = z2 = 0; }

    float process (float x)
    {
        const double in = x;
        const double out = b0 * in + z1;
        z1 = b1 * in - a1 * out + z2;
        z2 = b2 * in - a2 * out;
        return (float) out;
    }

private:
    void set (double nb0, double nb1, double nb2, double na0, double na1, double na2)
    {
        b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0;
        a1 = na1 / na0; a2 = na2 / na0;
    }

    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
};

// ---------------------------------------------------------------------------------------------
// Reamostrador em fluxo (interpolação cúbica Catmull-Rom). Latência de ~2 amostras.
class StreamResampler
{
public:
    void prepare (double inRate, double outRate)
    {
        step = inRate / outRate;
        buf.clear();
        buf.reserve ((size_t) kChunk + 64);
        buf.assign (4, 0.0f);
        pos = 1.0;
    }

    void reset()
    {
        buf.assign (4, 0.0f);
        pos = 1.0;
    }

    int process (const float* in, int n, float* out, int maxOut)
    {
        buf.insert (buf.end(), in, in + n);
        int count = 0;

        while ((int) pos + 2 < (int) buf.size() && count < maxOut)
        {
            const int i = (int) pos;
            const float t = (float) (pos - i);
            const float y0 = buf[(size_t) i - 1], y1 = buf[(size_t) i],
                        y2 = buf[(size_t) i + 1], y3 = buf[(size_t) i + 2];

            const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
            const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c = -0.5f * y0 + 0.5f * y2;
            out[count++] = ((a * t + b) * t + c) * t + y1;
            pos += step;
        }

        const int drop = (int) pos - 1;
        if (drop > 0)
        {
            buf.erase (buf.begin(), buf.begin() + drop);
            pos -= drop;
        }
        return count;
    }

private:
    double step = 1.0, pos = 1.0;
    std::vector<float> buf;
};

// ---------------------------------------------------------------------------------------------
// FFT complexa radix-2 (tamanho fixo kN), com tabelas pré-calculadas.
class Fft
{
public:
    Fft()
    {
        for (int i = 0; i < kN / 2; ++i)
        {
            cosT[(size_t) i] = (float) std::cos (2.0 * kPi * i / kN);
            sinT[(size_t) i] = (float) std::sin (2.0 * kPi * i / kN);
        }
        int bits = 0;
        while ((1 << bits) < kN) ++bits;
        for (int i = 0; i < kN; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
    }

    // sign = -1: direta, +1: inversa (sem normalização)
    void run (float* re, float* im, int sign) const
    {
        for (int i = 0; i < kN; ++i)
        {
            const int j = rev[(size_t) i];
            if (j > i) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
        }
        for (int len = 2; len <= kN; len <<= 1)
        {
            const int half = len >> 1, stride = kN / len;
            for (int i = 0; i < kN; i += len)
            {
                for (int k = 0; k < half; ++k)
                {
                    const float wr = cosT[(size_t) (k * stride)];
                    const float wi = sign * sinT[(size_t) (k * stride)];
                    const int a = i + k, b = a + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr;        im[a] += ti;
                }
            }
        }
    }

private:
    std::array<float, kN / 2> cosT {}, sinT {};
    std::array<int, kN> rev {};
};

// ---------------------------------------------------------------------------------------------
// Núcleo espectral a 48 kHz: recebe amostras (já atrasadas) e devolve a mistura Fonte/Ambiência/Ruído.
class Spectral
{
public:
    Spectral()
    {
        for (int i = 0; i < kN; ++i)
            win[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (2.0 * kPi * i / kN));
        reset();
    }

    void reset()
    {
        for (auto& c : hist) c.fill (0.0f);
        for (auto& c : acc) c.fill (0.0f);
        for (auto& c : emit) c.fill (0.0f);
        for (auto& c : pend) c.fill (0.0f);
        histW.fill (0.0f); pendW.fill (0.0f);
        idx = 0;

        P.fill (0.0f); curMin.fill (1e30f); warmFrames = 0;
        for (auto& s : subMins) s.fill (1e30f);
        subCount = 0; subPos = 0; first = true;
        shat2.fill (0.0f); gPrev.fill (1.0f); gdPrev.fill (1.0f); pavg.fill (0.0f);
        for (auto& h : pHist) h.fill (0.0f);
        pHistPos = 0;
        ringPos = 0; filled = 0;
        learnAcc.fill (0.0); learnFrames = 0; wasLearning = false;
        for (auto& pw : ringPow) pw.fill (0.0f);
        for (auto& g : ringG) g.fill (1.0f);
        std::memset (ringRe, 0, sizeof (ringRe)); std::memset (ringIm, 0, sizeof (ringIm));
    }

    void setParams (const Params& np) { p = np; }

    // Perfil de ruído aprendido (potência média por bin, mesma janela/escala da STFT interna)
    bool getHasLearned() const { return hasLearned; }
    bool takeNewProfileFlag() { const bool f = newProfile; newProfile = false; return f; }
    void getProfile (std::array<float, kBins>& dst) const { dst = learned; }
    void setProfile (const std::array<float, kBins>& src, bool valid) { learned = src; hasLearned = valid; }
    void setChannels (int n) { nch = std::min (std::max (n, 1), kMaxCh); }

    // xd: amostras dos canais já alinhadas; wet: saída do RNNoise alinhada (usada no modo Voz)
    void pushSample (const float* xd, float wet, float* out)
    {
        for (int c = 0; c < nch; ++c)
        {
            pend[(size_t) c][(size_t) idx] = xd[c];
            out[c] = emit[(size_t) c][(size_t) idx];
        }
        pendW[(size_t) idx] = wet;

        if (++idx == kHop)
        {
            idx = 0;
            processHop();
        }
    }

private:
    static constexpr int kSubLen = 40;     // quadros por sub-janela dos mínimos estatísticos (~210 ms)
    static constexpr int kSubCount = 24;   // sub-janelas (~5 s de memória)
    static constexpr int kWarmFrames = 8;  // quadros ignorados no começo (janela ainda parcialmente vazia)
    static constexpr int kRevD = 8;        // atraso (quadros) da estimativa de cauda de reverb (~43 ms)

    void processHop()
    {
        // 1) janelas deslizantes -> FFT
        for (int c = 0; c < nch; ++c)
        {
            auto& h = hist[(size_t) c];
            std::memmove (h.data(), h.data() + kHop, sizeof (float) * (size_t) (kN - kHop));
            std::memcpy (h.data() + kN - kHop, pend[(size_t) c].data(), sizeof (float) * kHop);
            for (int n = 0; n < kN; ++n) { re[c][n] = h[(size_t) n] * win[(size_t) n]; im[c][n] = 0.0f; }
            fft.run (re[c], im[c], -1);
        }

        // sinal "mid" (mesma máscara para todos os canais: mantém a imagem estéreo estável)
        for (int k = 0; k < kBins; ++k)
        {
            const float mr = nch == 2 ? 0.5f * (re[0][k] + re[1][k]) : re[0][k];
            const float mi = nch == 2 ? 0.5f * (im[0][k] + im[1][k]) : im[0][k];
            midPow[(size_t) k] = mr * mr + mi * mi;
        }

        // 2) máscara bruta de ruído
        if (p.mode == Mode::Voice)
        {
            std::memmove (histW.data(), histW.data() + kHop, sizeof (float) * (size_t) (kN - kHop));
            std::memcpy (histW.data() + kN - kHop, pendW.data(), sizeof (float) * kHop);
            for (int n = 0; n < kN; ++n) { wre[n] = histW[(size_t) n] * win[(size_t) n]; wim[n] = 0.0f; }
            fft.run (wre, wim, -1);
            for (int k = 0; k < kBins; ++k)
            {
                const float w2 = wre[k] * wre[k] + wim[k] * wim[k];
                gRaw[(size_t) k] = std::min (1.0f, std::sqrt (w2 / (midPow[(size_t) k] + 1e-20f)));
            }
        }
        else
        {
            estimateWiener();
        }

        // 2a) aprender ruído: média da potência enquanto o botão está ativo
        if (p.clearLearned) { hasLearned = false; learned.fill (0.0f); newProfile = true; }
        if (p.learning)
        {
            if (! wasLearning) { learnAcc.fill (0.0); learnFrames = 0; }
            for (int k = 0; k < kBins; ++k) learnAcc[(size_t) k] += (double) midPow[(size_t) k];
            ++learnFrames;
        }
        else if (wasLearning && learnFrames >= 20)      // pelo menos ~100 ms de áudio
        {
            for (int k = 0; k < kBins; ++k) learned[(size_t) k] = (float) (learnAcc[(size_t) k] / learnFrames);
            hasLearned = true; newProfile = true;
        }
        wasLearning = p.learning;

        // 2b) guarda o quadro e a máscara bruta. O quadro só é processado kLookahead quadros depois,
        //     então a máscara abre ANTES dos ataques (preserva o dedilhado / as consoantes).
        const size_t slotCur = (size_t) ringPos;
        for (int c = 0; c < nch; ++c)
        {
            std::memcpy (ringRe[c][slotCur], re[c], sizeof (float) * kN);
            std::memcpy (ringIm[c][slotCur], im[c], sizeof (float) * kN);
        }
        ringPow[slotCur] = midPow;
        ringG[slotCur] = gRaw;
        ringPos = (ringPos + 1) % kRing;

        if (filled < kLookahead) { ++filled; return; }       // enchendo a fila: ainda sem saída

        const size_t slotTgt = (size_t) ringPos;              // o mais antigo da fila
        for (int k = 0; k < kBins; ++k)
        {
            float m = 0.0f;
            for (int j = 0; j < kRing; ++j) m = std::max (m, ringG[(size_t) j][(size_t) k]);
            gRaw[(size_t) k] = m;
        }
        midPow = ringPow[slotTgt];

        // 3) controles anti-glitch (Limite, Suavidade, Cauda)
        shapeMask();

        // 4) de-reverb: separa Fonte e Ambiência dentro da parte "boa" do sinal
        estimateDereverb();

        // 5) ganho final por bin
        for (int k = 0; k < kBins; ++k)
        {
            const float g = gMask[(size_t) k], gd = gdMask[(size_t) k];
            const float sPart = g * gd, aPart = g * (1.0f - gd), nPart = 1.0f - g;
            float t;
            switch (p.listen)
            {
                case Listen::Source:   t = sPart; break;
                case Listen::Ambience: t = aPart; break;
                case Listen::Noise:    t = nPart; break;
                default:               t = sPart * p.source + aPart * (1.0f - p.ambience) + nPart * p.noise; break;
            }
            gain[(size_t) k] = t;
        }

        // 6) aplica no quadro antigo, volta ao tempo e soma (overlap-add; janelas sqrt-Hann com 75 % => soma 2)
        for (int c = 0; c < nch; ++c)
        {
            for (int n = 0; n < kN; ++n)
            {
                const float g = gain[(size_t) std::min (n, kN - n)];
                re[c][n] = ringRe[c][slotTgt][n] * g;
                im[c][n] = ringIm[c][slotTgt][n] * g;
            }
            fft.run (re[c], im[c], +1);
            auto& a = acc[(size_t) c];
            for (int n = 0; n < kN; ++n)
                a[(size_t) n] += re[c][n] * win[(size_t) n] * (0.5f / (float) kN);

            std::memcpy (emit[(size_t) c].data(), a.data(), sizeof (float) * kHop);
            std::memmove (a.data(), a.data() + kHop, sizeof (float) * (size_t) (kN - kHop));
            std::memset (a.data() + kN - kHop, 0, sizeof (float) * kHop);
        }
    }

    // Estimador para Violão / Outro: mínimos estatísticos (ou perfil aprendido) + Wiener "decision-directed".
    void estimateWiener()
    {
        // Ignora quadros de aquecimento (histórico ainda zerado) e silêncio digital: eles dariam um
        // "mínimo" falso de ~0 e o estimador acharia que não existe ruído.
        float total = 0.0f;
        for (int k = 0; k < kBins; ++k) total += midPow[(size_t) k];
        const bool usable = warmFrames >= kWarmFrames && total > 1e-10f * (float) kBins;
        if (warmFrames < kWarmFrames) ++warmFrames;

        if (usable)
        {
            const float aP = 0.85f;
            for (int k = 0; k < kBins; ++k)
            {
                const size_t i = (size_t) k;
                P[i] = first ? midPow[i] : aP * P[i] + (1.0f - aP) * midPow[i];
                curMin[i] = std::min (curMin[i], P[i]);
            }
            first = false;

            if (++subCount >= kSubLen)
            {
                subCount = 0;
                subMins[(size_t) subPos] = curMin;
                subPos = (subPos + 1) % kSubCount;
                curMin = P;
            }
        }

        const bool guitar = p.mode == Mode::Guitar;
        const float alpha = guitar ? tuning::ddAlphaGuitar : tuning::ddAlphaOther;
        const float bias = tuning::noiseBias;
        const float xiMin = 0.003f;

        for (int k = 0; k < kBins; ++k)
        {
            const size_t i = (size_t) k;
            float m = curMin[i];
            for (int s = 0; s < kSubCount; ++s) m = std::min (m, subMins[(size_t) s][i]);

            if (! hasLearned && m > 1e29f)          // ainda sem nenhuma medida de ruído: deixa passar
            {
                gRaw[i] = 1.0f;
                shat2[i] = midPow[i];
                continue;
            }

            const float noise = hasLearned ? std::max (learned[i], 1e-14f) : std::max (bias * m, 1e-14f);
            const float pEst = tuning::gammaRaw * midPow[i] + (1.0f - tuning::gammaRaw) * P[i];
            const float gamma = pEst / noise;
            float xi = alpha * shat2[i] / noise + (1.0f - alpha) * std::max (gamma - 1.0f, 0.0f);
            xi = std::max (xi, xiMin);
            const float g = xi / (1.0f + xi);
            shat2[i] = g * g * pEst;
            gRaw[i] = g;
        }
    }

    // Limite de redução -> suavização espectral (log) -> release temporal
    void shapeMask()
    {
        const float floorLin = std::pow (10.0f, -std::max (p.limitDb, 0.0f) / 20.0f);
        const float s = std::min (1.0f, std::max (0.0f, p.smooth));
        const int radius = 1 + (int) (s * 4.0f);          // 1..5 bins

        for (int k = 0; k < kBins; ++k)
            lg[(size_t) k] = std::log (std::max (gRaw[(size_t) k], floorLin));

        for (int k = 0; k < kBins; ++k)
        {
            float sum = 0.0f, wsum = 0.0f;
            for (int d = -radius; d <= radius; ++d)
            {
                const int j = std::min (std::max (k + d, 0), kBins - 1);
                const float w = (float) (radius + 1 - std::abs (d));
                sum += w * lg[(size_t) j]; wsum += w;
            }
            const float smoothed = sum / wsum;

            // Suaviza só aberturas NOVAS e isoladas (chiado musical). Parciais que já estavam abertos no quadro
            // anterior (harmônicos de uma nota sustentada) mantêm o ganho cheio: não "achata" o timbre.
            const float target = std::max (gRaw[(size_t) k], floorLin);
            const float persist = std::min (1.0f, gPrev[(size_t) k] / target);
            gTmp[(size_t) k] = std::exp (lg[(size_t) k] + s * (1.0f - persist) * (smoothed - lg[(size_t) k]));
        }

        const float tail = std::max (p.tailMs, 1.0f);
        const float rel = std::exp (-(float) kHop / (48.0f * tail));   // 48 amostras por ms
        const float atk = 0.5f * s;                                    // subida quase imediata (preserva ataques)

        for (int k = 0; k < kBins; ++k)
        {
            const size_t i = (size_t) k;
            float g = gTmp[i];
            const float prev = gPrev[i];
            g = g < prev ? rel * prev + (1.0f - rel) * g : atk * prev + (1.0f - atk) * g;
            g = std::min (1.0f, std::max (g, floorLin));
            gPrev[i] = g;
            gMask[i] = g;
        }
    }

    // De-reverb por subtração espectral da cauda tardia (modelo de decaimento exponencial).
    void estimateDereverb()
    {
        const float rt60 = p.mode == Mode::Guitar ? 0.6f : 0.5f;
        const float kappa = tuning::kappaScale * (p.mode == Mode::Guitar ? 0.8f : (p.mode == Mode::Voice ? 1.2f : 1.0f));
        const float aD = std::pow (10.0f, -6.0f * (float) (kRevD * kHop) / (48000.0f * rt60));

        pHistPos = (pHistPos + 1) % (kRevD + 1);
        const int oldPos = (pHistPos + 1) % (kRevD + 1);   // o mais antigo = kRevD quadros atrás

        const float relD = std::exp (-(float) kHop / (48.0f * 40.0f));   // release fixo de 40 ms

        for (int k = 0; k < kBins; ++k)
        {
            const size_t i = (size_t) k;
            const float g = gMask[i];
            const float ps = midPow[i] * g * g;
            pavg[i] = 0.5f * pavg[i] + 0.5f * ps;
            pHist[(size_t) pHistPos][i] = pavg[i];

            const float late = aD * pHist[(size_t) oldPos][i];
            float gd = 1.0f;
            if (pavg[i] > 1e-12f)
            {
                const float g2 = 1.0f - kappa * late / pavg[i];
                gd = std::sqrt (std::min (1.0f, std::max (g2, 0.0625f)));   // no máximo -12 dB
            }
            gd = gd < gdPrev[i] ? relD * gdPrev[i] + (1.0f - relD) * gd : gd;
            gdPrev[i] = gd;
            gdTmp[i] = gd;
        }

        for (int k = 0; k < kBins; ++k)
        {
            const float a = gdTmp[(size_t) std::max (k - 1, 0)];
            const float b = gdTmp[(size_t) k];
            const float c = gdTmp[(size_t) std::min (k + 1, kBins - 1)];
            gdMask[(size_t) k] = 0.25f * a + 0.5f * b + 0.25f * c;
        }
    }

    Params p;
    int nch = 1;
    Fft fft;
    std::array<float, kN> win {};

    std::array<std::array<float, kN>, kMaxCh> hist {}, acc {};
    std::array<std::array<float, kHop>, kMaxCh> emit {}, pend {};
    std::array<float, kN> histW {};
    std::array<float, kHop> pendW {};
    int idx = 0;

    float re[kMaxCh][kN] {}, im[kMaxCh][kN] {};
    float wre[kN] {}, wim[kN] {};

    std::array<float, kBins> midPow {}, gRaw {}, lg {}, gTmp {}, gMask {}, gdTmp {}, gdMask {}, gain {};
    std::array<float, kBins> P {}, curMin {}, shat2 {}, gPrev {}, gdPrev {}, pavg {};
    std::array<std::array<float, kBins>, kSubCount> subMins {};
    std::array<std::array<float, kBins>, kRevD + 1> pHist {};
    int subCount = 0, subPos = 0, pHistPos = 0, warmFrames = 0;
    bool first = true;

    static constexpr int kRing = kLookahead + 1;
    float ringRe[kMaxCh][kRing][kN] {}, ringIm[kMaxCh][kRing][kN] {};
    std::array<std::array<float, kBins>, kRing> ringPow {}, ringG {};
    int ringPos = 0, filled = 0;

    std::array<float, kBins> learned {};
    std::array<double, kBins> learnAcc {};
    int learnFrames = 0;
    bool hasLearned = false, wasLearning = false, newProfile = false;
};

// ---------------------------------------------------------------------------------------------
// Motor completo, na taxa do host. Processa 1 ou 2 canais (a máscara é compartilhada).
class Engine
{
public:
    Engine() = default;
    ~Engine() { destroy(); }
    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;

    void prepare (double newHostRate, int numChannels)
    {
        hostRate = newHostRate;
        nch = std::min (std::max (numChannels, 1), kMaxCh);
        resampling = std::abs (hostRate - 48000.0) > 0.5;

        destroy();
        state = rnnoise_create (nullptr);
        spectral.setChannels (nch);

        for (int c = 0; c < kMaxCh; ++c)
        {
            down[c].prepare (hostRate, 48000.0);
            up[c].prepare (48000.0, hostRate);
            aa1[c].setBypass(); aa2[c].setBypass();
            if (hostRate > 48000.5)
            {
                aa1[c].setLowPass (hostRate, 21000.0, 0.54119610);
                aa2[c].setLowPass (hostRate, 21000.0, 1.30656296);
            }
            lowCut[c].setBypass();
        }
        lastLowCut = -1.0f;
        reset();
    }

    void reset()
    {
        spectral.reset();
        fi = 0;
        frameIn.fill (0.0f);
        wetRing.fill (0.0f);
        wr = kWetPrime; rd = 0; wetCount = kWetPrime;
        for (auto& d : dryRing) d.fill (0.0f);
        dryPos = 0;
        for (int c = 0; c < kMaxCh; ++c)
        {
            down[c].reset(); up[c].reset();
            aa1[c].reset(); aa2[c].reset(); lowCut[c].reset();
            outFifo[c].assign (kOutFifoSize, 0.0f);
            ofRd[c] = ofWr[c] = ofCount[c] = 0;
            for (int i = 0; i < kOutPrime; ++i) pushOut (c, 0.0f);
        }
    }

    bool hasNoiseProfile() const { return spectral.getHasLearned(); }
    bool takeNewProfileFlag() { return spectral.takeNewProfileFlag(); }
    void getNoiseProfile (std::array<float, kBins>& dst) const { spectral.getProfile (dst); }
    void setNoiseProfile (const std::array<float, kBins>& src, bool valid) { spectral.setProfile (src, valid); }

    int getLatencySamples() const
    {
        if (! resampling) return kLatency48;
        return (int) std::lround (kLatency48 * hostRate / 48000.0) + kOutPrime + 4;
    }

    // Processa no lugar (in-place). channels[c] aponta para as amostras do canal c.
    void process (float* const* channels, int numSamples, const Params& params, float lowCutHz)
    {
        if (state == nullptr) return;
        spectral.setParams (params);
        updateLowCut (lowCutHz);

        int done = 0;
        while (done < numSamples)
        {
            const int n = std::min (kChunk, numSamples - done);
            float* ptrs[kMaxCh] {};
            for (int c = 0; c < nch; ++c) ptrs[c] = channels[c] + done;
            processChunk (ptrs, n);
            done += n;
        }
    }

private:
    static constexpr int kOutFifoSize = 1 << 15;
    static constexpr int kOutPrime = 16;
    static constexpr int kWetRingSize = 2048;

    void destroy()
    {
        if (state != nullptr) { rnnoise_destroy (state); state = nullptr; }
    }

    void updateLowCut (float hz)
    {
        if (std::abs (hz - lastLowCut) < 0.01f) return;
        lastLowCut = hz;
        for (int c = 0; c < kMaxCh; ++c)
        {
            if (hz <= 20.5f) lowCut[c].setBypass();
            else lowCut[c].setHighPass (48000.0, (double) hz);
        }
    }

    void pushOut (int c, float v)
    {
        if (ofCount[c] >= kOutFifoSize) return;
        outFifo[c][(size_t) ofWr[c]] = v;
        ofWr[c] = (ofWr[c] + 1) & (kOutFifoSize - 1);
        ++ofCount[c];
    }

    void processChunk (float* const* io, int n)
    {
        const float* in48[kMaxCh] {};
        int n48 = n;

        if (resampling)
        {
            for (int c = 0; c < nch; ++c)
            {
                if (hostRate > 48000.5)
                    for (int i = 0; i < n; ++i)
                        tmpIn[c][(size_t) i] = aa2[c].process (aa1[c].process (io[c][i]));
                else
                    std::memcpy (tmpIn[c].data(), io[c], sizeof (float) * (size_t) n);

                const int got = down[c].process (tmpIn[c].data(), n, buf48[c].data(), kScratch);
                if (c == 0) n48 = got;
                in48[c] = buf48[c].data();
            }
        }
        else
        {
            for (int c = 0; c < nch; ++c) in48[c] = io[c];
        }

        for (int k = 0; k < n48; ++k)
        {
            float x[kMaxCh] {}, xd[kMaxCh] {}, y[kMaxCh] {};
            for (int c = 0; c < nch; ++c) x[c] = in48[c][k];

            // original atrasado (alinhado com a saída do RNNoise)
            for (int c = 0; c < nch; ++c)
            {
                xd[c] = dryRing[(size_t) c][(size_t) dryPos];
                dryRing[(size_t) c][(size_t) dryPos] = x[c];
            }
            if (++dryPos >= kDryDelay) dryPos = 0;

            const float mid = nch == 2 ? 0.5f * (x[0] + x[1]) : x[0];
            frameIn[(size_t) fi++] = mid * 32768.0f;
            if (fi == kFrame)
            {
                rnnoise_process_frame (state, frameOut.data(), frameIn.data());
                for (int i = 0; i < kFrame; ++i)
                {
                    wetRing[(size_t) wr] = frameOut[(size_t) i] * (1.0f / 32768.0f);
                    wr = (wr + 1) & (kWetRingSize - 1);
                }
                wetCount += kFrame;
                fi = 0;
            }

            float wet = 0.0f;
            if (wetCount > 0)
            {
                wet = wetRing[(size_t) rd];
                rd = (rd + 1) & (kWetRingSize - 1);
                --wetCount;
            }

            spectral.pushSample (xd, wet, y);
            for (int c = 0; c < nch; ++c) mix48[c][(size_t) k] = lowCut[c].process (y[c]);
        }

        if (! resampling)
        {
            for (int c = 0; c < nch; ++c)
                std::memcpy (io[c], mix48[c].data(), sizeof (float) * (size_t) n);
            return;
        }

        for (int c = 0; c < nch; ++c)
        {
            const int m = up[c].process (mix48[c].data(), n48, tmpOut[c].data(), kScratch);
            for (int i = 0; i < m; ++i) pushOut (c, tmpOut[c][(size_t) i]);

            for (int i = 0; i < n; ++i)
            {
                if (ofCount[c] > 0)
                {
                    io[c][i] = outFifo[c][(size_t) ofRd[c]];
                    ofRd[c] = (ofRd[c] + 1) & (kOutFifoSize - 1);
                    --ofCount[c];
                }
                else
                {
                    io[c][i] = 0.0f;
                }
            }
        }
    }

    DenoiseState* state = nullptr;
    double hostRate = 48000.0;
    int nch = 1;
    bool resampling = false;

    Spectral spectral;
    StreamResampler down[kMaxCh], up[kMaxCh];
    Biquad aa1[kMaxCh], aa2[kMaxCh], lowCut[kMaxCh];
    float lastLowCut = -1.0f;

    std::array<float, kFrame> frameIn {}, frameOut {};
    int fi = 0;

    std::array<float, kWetRingSize> wetRing {};
    int wr = 0, rd = 0, wetCount = 0;

    std::array<std::array<float, kDryDelay>, kMaxCh> dryRing {};
    int dryPos = 0;

    std::array<std::array<float, kChunk>, kMaxCh> tmpIn {};
    std::array<std::array<float, kScratch>, kMaxCh> buf48 {}, mix48 {}, tmpOut {};

    std::vector<float> outFifo[kMaxCh];
    int ofRd[kMaxCh] {}, ofWr[kMaxCh] {}, ofCount[kMaxCh] {};
};

} // namespace limpatudo
