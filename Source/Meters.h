#pragma once
#include <juce_dsp/juce_dsp.h>
#include <complex>
#include <vector>
#include <atomic>
#include <cstring>

// =====================================================================
//  Vega Productions — DSP building blocks
// =====================================================================
namespace vega
{
inline float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g)  { return 20.0f * std::log10 (std::max (g, 1.0e-9f)); }

// ---------------------------------------------------------------------
//  Biquad (RBJ cookbook, transposed direct form II, double precision)
// ---------------------------------------------------------------------
struct BiquadCoeffs
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    static BiquadCoeffs normalise (double b0, double b1, double b2, double a0, double a1, double a2)
    {
        return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0 };
    }
    static double clampF (double f, double fs) { return juce::jlimit (5.0, fs * 0.49, f); }

    static BiquadCoeffs peak (double fs, double f, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * clampF (f, fs) / fs;
        const double c = std::cos (w), al = std::sin (w) / (2.0 * q);
        return normalise (1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
    }
    static BiquadCoeffs lowShelf (double fs, double f, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * clampF (f, fs) / fs;
        const double c = std::cos (w), al = std::sin (w) / (2.0 * q), sA = 2.0 * std::sqrt (A) * al;
        return normalise (A * ((A + 1) - (A - 1) * c + sA), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sA),
                          (A + 1) + (A - 1) * c + sA, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sA);
    }
    static BiquadCoeffs highShelf (double fs, double f, double q, double gainDb)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w = juce::MathConstants<double>::twoPi * clampF (f, fs) / fs;
        const double c = std::cos (w), al = std::sin (w) / (2.0 * q), sA = 2.0 * std::sqrt (A) * al;
        return normalise (A * ((A + 1) + (A - 1) * c + sA), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sA),
                          (A + 1) - (A - 1) * c + sA, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sA);
    }
    static BiquadCoeffs highPass (double fs, double f, double q = 0.7071)
    {
        const double w = juce::MathConstants<double>::twoPi * clampF (f, fs) / fs;
        const double c = std::cos (w), al = std::sin (w) / (2.0 * q);
        return normalise ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
    }

    double magnitude (double f, double fs) const
    {
        const double w = juce::MathConstants<double>::twoPi * f / fs;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
        return std::abs ((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2));
    }
};

struct Biquad
{
    BiquadCoeffs c;
    double z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    inline float process (float xf)
    {
        const double x = xf, y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return (float) y;
    }
};

// ---------------------------------------------------------------------
//  Three‑band mastering EQ (low shelf / bell / air shelf)
struct TruePeakInterpolator
{
    static constexpr int half = 16, taps = 2 * half, phases = 7;  // 8x: t = 1/8 .. 7/8
    float coef[phases][taps];
    float hist[2][taps] {};
    int pos = 0;

    TruePeakInterpolator()
    {
        for (int p = 0; p < phases; ++p)
        {
            const double t = (p + 1) / 8.0;
            for (int k = 0; k < taps; ++k)
            {
                const double d = (k - half + 1) - t;   // tap offsets -half+1 .. half
                const double pi = juce::MathConstants<double>::pi;
                const double sinc = std::sin (pi * d) / (pi * d);
                const double w = 0.5 + 0.5 * std::cos (pi * d / (half + 0.5));
                coef[p][k] = (float) (sinc * w);
            }
        }
    }
    void reset() { std::memset (hist, 0, sizeof (hist)); pos = 0; }

    /** push one stereo sample; returns the inter‑sample peak (abs, both channels) in the interval
        between the samples that are `half` and `half-1` samples old. */
    inline float push (float l, float r)
    {
        hist[0][pos] = l; hist[1][pos] = r;
        pos = (pos + 1) % taps;              // pos now points at the oldest sample
        float pk = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int p = 0; p < phases; ++p)
            {
                float acc = 0.0f;
                for (int k = 0; k < taps; ++k) acc += hist[c][(pos + k) % taps] * coef[p][k];
                pk = std::max (pk, std::abs (acc));
            }
        return pk;
    }
    inline float sampleAt (int c, int age) const { return hist[c][(pos - 1 - age + 2 * taps) % taps]; }
};

// Look‑ahead brick‑wall limiter with true‑peak (inter‑sample) detection
class LoudnessMeter
{
public:
    void prepare (double sampleRate)
    {
        fs = sampleRate;
        // K‑weighting stage 1 (high shelf) and stage 2 (RLB high‑pass), from BS.1770
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            shelfC = { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                       2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / fs);
            const double a0 = 1.0 + K / Q + K * K;
            hpC = { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        }
        for (auto& f : stage1) f.c = shelfC;
        for (auto& f : stage2) f.c = hpC;
        blockLen = std::max (1, (int) std::round (0.1 * fs));
        gating.assign (36000 * 2, 0.0f);  // two hours of 100 ms‑hop gating blocks
        hardReset();
    }

    void requestReset() { resetRequested.store (true); }

    void process (const float* L, const float* R, int n)
    {
        if (resetRequested.exchange (false)) hardReset();
        for (int i = 0; i < n; ++i)
        {
            const double l = stage2[0].process (stage1[0].process (L[i]));
            const double r = stage2[1].process (stage1[1].process (R[i]));
            acc += l * l + r * r;
            if (++accCount >= blockLen)
            {
                ring[(size_t) ringPos] = acc / blockLen;
                ringPos = (ringPos + 1) % 30;
                ringFilled = std::min (30, ringFilled + 1);
                acc = 0; accCount = 0;

                const double m = meanOfLast (4), st = meanOfLast (30);
                momentary.store (toLufs (m));
                shortTerm.store (toLufs (st));
                if (ringFilled >= 4)
                {
                    const int idx = numGating.load (std::memory_order_relaxed);
                    if (idx < (int) gating.size())
                    {
                        gating[(size_t) idx] = (float) m;
                        numGating.store (idx + 1, std::memory_order_release);
                    }
                }
            }
        }
    }

    /** Call from the message thread — gated integrated loudness. */
    float computeIntegrated() const
    {
        const int n = numGating.load (std::memory_order_acquire);
        double sum = 0; int cnt = 0;
        const double absGate = fromLufs (-70.0);
        for (int i = 0; i < n; ++i) if (gating[(size_t) i] > absGate) { sum += gating[(size_t) i]; ++cnt; }
        if (cnt == 0) return -100.0f;
        const double relGate = fromLufs (toLufs (sum / cnt) - 10.0);
        sum = 0; cnt = 0;
        for (int i = 0; i < n; ++i)
            if (gating[(size_t) i] > absGate && gating[(size_t) i] > relGate) { sum += gating[(size_t) i]; ++cnt; }
        return cnt ? toLufs (sum / cnt) : -100.0f;
    }

    std::atomic<float> momentary { -100.0f }, shortTerm { -100.0f };

private:
    static float toLufs (double p)   { return p > 1.0e-12 ? (float) (-0.691 + 10.0 * std::log10 (p)) : -100.0f; }
    static double fromLufs (double l) { return std::pow (10.0, (l + 0.691) / 10.0); }

    double meanOfLast (int k) const
    {
        k = std::min (k, ringFilled);
        if (k == 0) return 0;
        double s = 0;
        for (int i = 1; i <= k; ++i) s += ring[(size_t) ((ringPos - i + 30) % 30)];
        return s / k;
    }
    void hardReset()
    {
        for (auto& f : stage1) f.reset();
        for (auto& f : stage2) f.reset();
        acc = 0; accCount = 0; ringPos = 0; ringFilled = 0; ring.fill (0);
        numGating.store (0);
        momentary.store (-100.0f); shortTerm.store (-100.0f);
    }

    double fs = 48000;
    BiquadCoeffs shelfC, hpC;
    std::array<Biquad, 2> stage1, stage2;
    int blockLen = 4800, accCount = 0, ringPos = 0, ringFilled = 0;
    double acc = 0;
    std::array<double, 30> ring {};
    std::vector<float> gating;
    std::atomic<int> numGating { 0 };
    std::atomic<bool> resetRequested { false };
};

// ---------------------------------------------------------------------
//  Lock‑free single‑producer / single‑consumer stereo sample FIFO
// ---------------------------------------------------------------------
} // namespace vega
