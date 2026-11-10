// Testes do motor do LimpaTudo (sem JUCE).
// Compilar (Linux/macOS), a partir da pasta do projeto:
//   mkdir -p /tmp/lt && for f in third_party/rnnoise/src/*.c; do
//     gcc -O2 -mavx2 -mfma -w -c $f -Ithird_party/rnnoise/include -Ithird_party/rnnoise/src -o /tmp/lt/$(basename $f).o; done
//   g++ -O2 -std=c++17 -mavx2 -mfma tests/test_core.cpp /tmp/lt/*.o -Ithird_party/rnnoise/include -ISource -o /tmp/lt/test_core
//   /tmp/lt/test_core

#include "Engine.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>

using namespace limpatudo;

static int failures = 0;

static void check (bool ok, const char* msg)
{
    std::printf ("  %s %s\n", ok ? "OK   " : "FALHA", msg);
    if (! ok) ++failures;
}

static double rmsOf (const std::vector<float>& v, size_t from, size_t to)
{
    double s = 0;
    for (size_t i = from; i < to; ++i) s += (double) v[i] * v[i];
    return std::sqrt (s / (double) (to - from + 1));
}

static double toDb (double x) { return 20.0 * std::log10 (x + 1e-20); }

// Processa um sinal mono ou estéreo com blocos de tamanho irregular, como um host de verdade.
// paramsAt(pos) devolve os parâmetros a usar no bloco que começa na amostra pos.
static std::vector<std::vector<float>> runFn (const std::vector<std::vector<float>>& in,
                                              const std::function<Params (size_t)>& paramsAt,
                                              double sr, float lowCut, int* latencyOut = nullptr)
{
    const int nch = (int) in.size();
    Engine e;
    e.prepare (sr, nch);
    if (latencyOut) *latencyOut = e.getLatencySamples();

    auto out = in;
    const int sizes[] = { 64, 100, 512, 37, 1024, 333, 2048 };
    size_t pos = 0; int si = 0;
    const size_t total = in[0].size();
    while (pos < total)
    {
        const int n = (int) std::min<size_t> ((size_t) sizes[si++ % 7], total - pos);
        float* ptrs[2] = { out[0].data() + pos, nch > 1 ? out[1].data() + pos : nullptr };
        e.process (ptrs, n, paramsAt (pos), lowCut);
        pos += (size_t) n;
    }
    return out;
}

static std::vector<std::vector<float>> run (const std::vector<std::vector<float>>& in, const Params& p,
                                            double sr, float lowCut, int* latencyOut = nullptr)
{
    return runFn (in, [&] (size_t) { return p; }, sr, lowCut, latencyOut);
}

static std::vector<std::vector<float>> mono (const std::vector<float>& v) { return { v }; }

static Params neutral (Mode m)
{
    Params p;
    p.mode = m; p.source = 1.0f; p.ambience = 0.0f; p.noise = 1.0f; p.listen = Listen::Normal;
    return p;
}

// erro (em dB relativo ao sinal) entre out[n + lat] e ref[n]
static double alignedErrorDb (const std::vector<float>& ref, const std::vector<float>& out, int lat, size_t from, size_t to)
{
    double e = 0, s = 0;
    for (size_t n = from; n < to && n + (size_t) lat < out.size(); ++n)
    {
        const double d = (double) out[n + (size_t) lat] - ref[n];
        e += d * d; s += (double) ref[n] * ref[n];
    }
    return 10.0 * std::log10 ((e + 1e-30) / (s + 1e-30));
}

static int findLag (const std::vector<float>& a, const std::vector<float>& b, int maxLag, double& bestCorr)
{
    int bestLag = 0;
    bestCorr = -1;
    const int start = (int) a.size() / 4, len = std::min<int> ((int) a.size() / 2, 48000);
    double ea = 0;
    for (int i = 0; i < len; ++i) ea += (double) a[(size_t) start + i] * a[(size_t) start + i];

    for (int lag = 0; lag <= maxLag; ++lag)
    {
        double c = 0, eb = 0;
        for (int i = 0; i < len; ++i)
        {
            const double x = a[(size_t) (start + i - lag)];
            const double y = b[(size_t) (start + i)];
            c += x * y; eb += y * y;
        }
        const double corr = c / std::sqrt (ea * eb + 1e-20);
        if (corr > bestCorr) { bestCorr = corr; bestLag = lag; }
    }
    return bestLag;
}

// ---------------------------------------------------------------------------------------------
// Sinais de teste

// "Violão": cordas dedilhadas (Karplus-Strong) em padrão de arpejo.
static std::vector<float> makeGuitar (double sr, double seconds, double startAt = 0.2)
{
    const size_t n = (size_t) (sr * seconds);
    std::vector<float> out (n, 0.0f);
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> uni (-1.0f, 1.0f);
    const double notes[] = { 110.0, 164.8, 220.0, 277.2, 329.6, 220.0, 164.8, 246.9 };
    int ni = 0;
    for (double t = startAt; t < seconds - 0.8; t += 0.375, ++ni)
    {
        const double f = notes[ni % 8];
        const int period = (int) std::lround (sr / f);
        std::vector<float> line ((size_t) period);
        for (auto& v : line) v = uni (rng);
        size_t pos = (size_t) (t * sr), li = 0;
        for (size_t i = 0; i < (size_t) (sr * 1.6) && pos + i < n; ++i)
        {
            const size_t nx = (li + 1) % line.size();
            const float y = line[li];
            line[li] = 0.4985f * (line[li] + line[nx]);
            out[pos + i] += 0.25f * y;
            li = nx;
        }
    }
    return out;
}

// "Ventoinha": ruído passa-baixas + zumbido de 100 Hz.
static std::vector<float> makeFan (double sr, double seconds, float level)
{
    const size_t n = (size_t) (sr * seconds);
    std::vector<float> out (n);
    std::mt19937 rng (99);
    std::normal_distribution<float> gauss (0.0f, 1.0f);
    float lp = 0;
    for (size_t i = 0; i < n; ++i)
    {
        lp += 0.12f * (gauss (rng) - lp);
        out[i] = level * (2.2f * lp + 0.15f * (float) std::sin (2 * kPi * 100.0 * (double) i / sr));
    }
    return out;
}

// "Voz": harmônicos de 120 Hz com envelope silábico (pausas entre sílabas).
static void makeVoice (std::vector<float>& clean, std::vector<float>& noisy, double sr, double seconds, float noiseLevel)
{
    const int n = (int) (sr * seconds);
    clean.assign ((size_t) n, 0.0f);
    noisy.assign ((size_t) n, 0.0f);
    std::mt19937 rng (1234);
    std::normal_distribution<float> gauss (0.0f, 1.0f);
    for (int i = 0; i < n; ++i)
    {
        const double t = i / sr;
        const double env = std::max (0.0, std::sin (2 * kPi * 2.0 * t));
        const double f0 = 120.0 + 15.0 * std::sin (2 * kPi * 0.7 * t);
        double s = 0;
        for (int h = 1; h <= 25; ++h) s += std::sin (2 * kPi * f0 * h * t) / (double) h;
        clean[(size_t) i] = (float) (0.25 * env * s);
        noisy[(size_t) i] = clean[(size_t) i] + noiseLevel * gauss (rng);
    }
}

// ---------------------------------------------------------------------------------------------
int main()
{
    const double sr = 48000.0;

    // sinal base para os testes de identidade: violão + ventoinha
    auto guitar = makeGuitar (sr, 6.0);
    auto fan = makeFan (sr, 6.0, 0.02f);
    std::vector<float> mix (guitar.size());
    for (size_t i = 0; i < mix.size(); ++i) mix[i] = guitar[i] + fan[i];

    std::printf ("[1] Nulo: Fonte 100%%, Ambiência 0, Ruído 100%% => saída idêntica à entrada\n");
    for (Mode m : { Mode::Voice, Mode::Guitar, Mode::Other })
    {
        int lat = 0;
        auto out = run (mono (mix), neutral (m), sr, 20.0f, &lat);
        double corr;
        const int measured = findLag (mix, out[0], 4000, corr);
        const double err = alignedErrorDb (mix, out[0], lat, 20000, mix.size() - lat - 2000);
        std::printf ("  modo %d: latência medida=%d reportada=%d erro=%.1f dB correlação=%.5f\n",
                     (int) m, measured, lat, err, corr);
        check (measured == lat, "latência medida = reportada");
        check (err < -80.0, "erro < -80 dB (identidade)");
    }

    std::printf ("[2] Soma dos 3 stems (Fonte + Ambiência + Ruído) = entrada\n");
    for (Mode m : { Mode::Voice, Mode::Guitar })
    {
        Params p = neutral (m);
        std::vector<float> sum (mix.size(), 0.0f);
        int lat = 0;
        for (Listen l : { Listen::Source, Listen::Ambience, Listen::Noise })
        {
            p.listen = l;
            auto o = run (mono (mix), p, sr, 20.0f, &lat);
            for (size_t i = 0; i < sum.size(); ++i) sum[i] += o[0][i];
        }
        const double err = alignedErrorDb (mix, sum, lat, 20000, mix.size() - lat - 2000);
        std::printf ("  modo %d: erro da soma = %.1f dB\n", (int) m, err);
        check (err < -70.0, "soma dos stems reconstrói a entrada (< -70 dB)");
    }

    std::printf ("[3] Estéreo: identidade e máscara compartilhada\n");
    {
        std::vector<float> right (mix.size());
        for (size_t i = 0; i < right.size(); ++i) right[i] = 0.7f * guitar[i] + fan[mix.size() - 1 - i];
        int lat = 0;
        auto out = run ({ mix, right }, neutral (Mode::Guitar), sr, 20.0f, &lat);
        const double eL = alignedErrorDb (mix, out[0], lat, 20000, mix.size() - lat - 2000);
        const double eR = alignedErrorDb (right, out[1], lat, 20000, mix.size() - lat - 2000);
        std::printf ("  erro L=%.1f dB  R=%.1f dB\n", eL, eR);
        check (eL < -80.0 && eR < -80.0, "estéreo idêntico à entrada");
    }

    std::printf ("[4] Violão: ruído de ventoinha removido, notas preservadas\n");
    {
        // 10 s: 1,6 s só de "room tone" (ventoinha) e depois o violão. Avalia entre 2,5 s e 6 s.
        auto g2 = makeGuitar (sr, 10.0, 1.6);
        auto runCase = [&] (float fanLevel, Mode m, bool learn, double& snrIn, double& snrOut, double& levelDb)
        {
            auto fn = makeFan (sr, 10.0, fanLevel);
            std::vector<float> mx (g2.size());
            for (size_t i = 0; i < mx.size(); ++i) mx[i] = g2[i] + fn[i];
            int lat = 0;
            auto out = runFn (mono (mx), [&] (size_t pos)
            {
                Params p; p.mode = m;
                p.learning = learn && pos > (size_t) (0.1 * sr) && pos < (size_t) (1.4 * sr);
                return p;
            }, sr, 20.0f, &lat);

            const size_t from = (size_t) (2.5 * sr), to = (size_t) (6.0 * sr);
            double e0 = 0, e1 = 0, s = 0, eo = 0;
            for (size_t n = from; n < to; ++n)
            {
                const double d0 = (double) mx[n] - g2[n], d1 = (double) out[0][n + (size_t) lat] - g2[n];
                e0 += d0 * d0; e1 += d1 * d1; s += (double) g2[n] * g2[n];
                eo += (double) out[0][n + (size_t) lat] * out[0][n + (size_t) lat];
            }
            snrIn = 10 * std::log10 (s / e0); snrOut = 10 * std::log10 (s / e1); levelDb = 10 * std::log10 (eo / s);
        };

        double i0, o0, l0;
        for (Mode m : { Mode::Guitar, Mode::Other })
        {
            runCase (0.02f, m, false, i0, o0, l0);
            std::printf ("  modo %d, ruído fraco, automático:   SNR %.1f -> %.1f dB (%+.1f)  nível %.1f dB\n", (int) m, i0, o0, o0 - i0, l0);
            check (o0 - i0 > 0.5, "automático: melhora o SNR (limite teórico neste caso ~ +3 dB)");
            check (std::abs (l0) < 2.0, "nível do violão preservado (±2 dB)");
        }

        runCase (0.02f, Mode::Guitar, true, i0, o0, l0);
        std::printf ("  ruído fraco, com \"Aprender ruído\":  SNR %.1f -> %.1f dB (%+.1f)  nível %.1f dB\n", i0, o0, o0 - i0, l0);
        check (o0 - i0 > 1.5, "aprendendo o ruído: melhora > 1,5 dB (limite teórico ~ +3 dB)");
        check (std::abs (l0) < 2.0, "nível preservado");

        runCase (0.12f, Mode::Guitar, true, i0, o0, l0);
        std::printf ("  ruído FORTE, com \"Aprender ruído\":  SNR %.1f -> %.1f dB (%+.1f)  nível %.1f dB\n", i0, o0, o0 - i0, l0);
        check (o0 - i0 > 5.0, "ruído forte + aprender: melhora > 5 dB");

        runCase (0.12f, Mode::Guitar, false, i0, o0, l0);
        std::printf ("  ruído FORTE, automático:            SNR %.1f -> %.1f dB (%+.1f)  nível %.1f dB\n", i0, o0, o0 - i0, l0);
        check (o0 - i0 > 2.0, "ruído forte, automático: melhora > 2 dB");
    }

    std::printf ("[5] Voz: ruído branco removido nas pausas\n");
    for (double rate : { 48000.0, 44100.0, 96000.0 })
    {
        std::vector<float> clean, noisy;
        makeVoice (clean, noisy, rate, 8.0, 0.02f);
        Params p;                                      // Voz, padrão
        int lat = 0;
        auto out = run (mono (noisy), p, rate, 20.0f, &lat);

        bool finite = true;
        for (float v : out[0]) if (! std::isfinite (v)) finite = false;

        double inQ = 0, outQ = 0;
        for (size_t i = (size_t) rate; i < noisy.size() - 4000; ++i)
        {
            const double t = (double) i / rate;
            if (std::sin (2 * kPi * 2.0 * t) < -0.3)
            {
                inQ += (double) noisy[i] * noisy[i];
                outQ += (double) out[0][i + (size_t) lat] * out[0][i + (size_t) lat];
            }
        }
        const double red = 10.0 * std::log10 ((inQ + 1e-20) / (outQ + 1e-20));
        std::printf ("  sr=%6.0f  redução nas pausas=%.1f dB  finito=%d\n", rate, red, (int) finite);
        check (finite, "saída finita");
        check (red > 10.0, "redução > 10 dB");

        // latência reportada x medida (identidade) a esta taxa
        auto id = run (mono (noisy), neutral (Mode::Voice), rate, 20.0f, &lat);
        double corr;
        const int measured = findLag (noisy, id[0], 12000, corr);
        std::printf ("          latência medida=%d reportada=%d\n", measured, lat);
        check (std::abs (measured - lat) <= 24, "latência medida ~ reportada (±24)");
    }

    std::printf ("[6] Ambiência: knob remove a cauda de reverb sem comer o som direto\n");
    {
        // sílabas curtas (80 ms, decaimento rápido) numa sala com RT60 ~0,6 s
        std::vector<float> src ((size_t) (sr * 12.0), 0.0f);
        for (int k = 0; k < 7; ++k)
        {
            const size_t on = (size_t) (k * 1.5 * sr + 6000);
            for (size_t i = 0; i < (size_t) (0.25 * sr) && on + i < src.size(); ++i)
            {
                const double t = (double) i / sr;
                double v = 0;
                for (int h = 1; h <= 12; ++h) v += std::sin (2 * kPi * 140.0 * h * t) / (double) h;
                src[on + i] = (float) (0.3 * v * std::exp (-t / 0.03));
            }
        }

        const size_t irLen = (size_t) (sr * 0.9);
        std::vector<float> ir (irLen, 0.0f);
        std::mt19937 rng (5);
        std::normal_distribution<float> gauss (0.0f, 1.0f);
        ir[0] = 1.0f;
        for (size_t i = (size_t) (0.012 * sr); i < irLen; ++i)
            ir[i] = 0.35f * gauss (rng) * (float) std::exp (-6.9 * (double) i / (0.6 * sr));

        std::vector<float> wetSig (src.size(), 0.0f);
        for (size_t i = 0; i < src.size(); ++i)
        {
            if (std::abs (src[i]) < 1e-5f) continue;
            for (size_t j = 0; j < irLen && i + j < wetSig.size(); ++j) wetSig[i + j] += src[i] * ir[j];
        }

        Params p0; p0.mode = Mode::Other; p0.ambience = 0.0f; p0.noise = 0.0f;
        Params p1 = p0; p1.ambience = 1.0f;
        int lat = 0;
        auto o0 = run (mono (wetSig), p0, sr, 20.0f, &lat);
        auto o1 = run (mono (wetSig), p1, sr, 20.0f, &lat);

        double e0 = 0, e1 = 0, d0 = 0, d1 = 0;
        for (int k = 1; k < 7; ++k)
        {
            const size_t on = (size_t) (k * 1.5 * sr + 6000);
            for (size_t i = on + (size_t) (0.30 * sr); i < on + (size_t) (0.80 * sr); ++i)     // só cauda
            { e0 += (double) o0[0][i + lat] * o0[0][i + lat]; e1 += (double) o1[0][i + lat] * o1[0][i + lat]; }
            for (size_t i = on; i < on + (size_t) (0.06 * sr); ++i)                            // som direto
            { d0 += (double) o0[0][i + lat] * o0[0][i + lat]; d1 += (double) o1[0][i + lat] * o1[0][i + lat]; }
        }
        const double tailDrop = 10.0 * std::log10 (e0 / e1), directDrop = 10.0 * std::log10 (d0 / d1);
        std::printf ("  cauda reduzida em %.1f dB; som direto reduzido em %.1f dB\n", tailDrop, directDrop);
        check (tailDrop > 3.0, "ambiência 100 % reduz a cauda > 3 dB");
        check (directDrop < tailDrop - 2.0, "o som direto é bem menos afetado que a cauda");
    }

    std::printf ("[7] Limite de redução: controla a profundidade do corte\n");
    {
        std::mt19937 rng (3);
        std::normal_distribution<float> gauss (0.0f, 0.05f);
        std::vector<float> noiseOnly ((size_t) (sr * 8.0));
        for (auto& v : noiseOnly) v = gauss (rng);

        double red[2] = { 0, 0 };
        const float limits[2] = { 6.0f, 40.0f };
        for (int i = 0; i < 2; ++i)
        {
            Params p; p.mode = Mode::Guitar; p.limitDb = limits[i];
            int lat = 0;
            auto o = run (mono (noiseOnly), p, sr, 20.0f, &lat);
            red[i] = toDb (rmsOf (noiseOnly, (size_t) (4 * sr), noiseOnly.size() - 5000))
                   - toDb (rmsOf (o[0], (size_t) (4 * sr) + (size_t) lat, noiseOnly.size() - 5000 + (size_t) lat));
            std::printf ("  limite %2.0f dB -> redução medida %.1f dB\n", limits[i], red[i]);
        }
        check (red[0] < 8.0, "limite 6 dB: redução não passa de ~6 dB");
        check (red[1] > 12.0, "limite 40 dB: ruído estacionário bem reduzido (> 12 dB)");
    }

    std::printf ("[8] Cauda e suavidade: mudam a máscara sem quebrar nada\n");
    {
        for (float tail : { 10.0f, 500.0f })
            for (float smooth : { 0.0f, 1.0f })
            {
                Params p; p.mode = Mode::Guitar; p.tailMs = tail; p.smooth = smooth;
                auto o = run (mono (mix), p, sr, 20.0f);
                bool finite = true;
                double peak = 0;
                for (float v : o[0]) { if (! std::isfinite (v)) finite = false; peak = std::max (peak, (double) std::abs (v)); }
                std::printf ("  cauda=%3.0f ms suavidade=%.0f  pico=%.3f\n", tail, smooth, peak);
                check (finite && peak < 1.5, "saída finita e sem estouro");
            }
    }

    std::printf (failures == 0 ? "\nTODOS OS TESTES PASSARAM\n" : "\n%d FALHA(S)\n", failures);
    return failures == 0 ? 0 : 1;
}
