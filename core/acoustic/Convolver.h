// Convolver.h -- realtime convolution with the cab's physics-built IR, and the
// FFT it runs on. Plain C++, no JUCE.
//
//   Fft          radix-2 complex FFT with precomputed twiddles (no allocation after construction)
//   IRSpectra    an IR cut into partitions and transformed -- built OFF the audio thread
//   Convolver    uniformly partitioned overlap-save (block B, FFT 2B), one IR at a time,
//                with a crossfade when a new IR arrives; no allocation or locking in process()
//
// Latency: one block (B samples).
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>

namespace cd::acoustic {

using cplxf = std::complex<double>;

class Fft {
public:
    explicit Fft(size_t n = 2) { resize(n); }
    void resize(size_t n)
    {
        N = n;
        rev.assign(n, 0);
        for (size_t i = 0, j = 0; i < n; ++i) {
            rev[i] = j;
            size_t bit = n >> 1;
            for (; bit && (j & bit); bit >>= 1) j ^= bit;
            j |= bit;
        }
        tw.resize(n / 2);
        for (size_t k = 0; k < n / 2; ++k) tw[k] = std::polar(1.0, -2 * M_PI * (double) k / (double) n);
    }
    size_t size() const { return N; }
    void forward(cplxf* a) const { run(a, false); }
    void inverse(cplxf* a) const
    {
        run(a, true);
        const double s = 1.0 / (double) N;
        for (size_t i = 0; i < N; ++i) a[i] *= s;
    }

private:
    size_t N = 0;
    std::vector<size_t> rev;
    std::vector<cplxf> tw;
    void run(cplxf* a, bool inv) const
    {
        for (size_t i = 0; i < N; ++i)
            if (i < rev[i]) std::swap(a[i], a[rev[i]]);
        for (size_t len = 2; len <= N; len <<= 1) {
            const size_t half = len / 2, step = N / len;
            for (size_t i = 0; i < N; i += len)
                for (size_t k = 0; k < half; ++k) {
                    const cplxf w = inv ? std::conj(tw[k * step]) : tw[k * step];
                    const cplxf u = a[i + k], v = a[i + k + half] * w;
                    a[i + k] = u + v;
                    a[i + k + half] = u - v;
                }
        }
    }
};

// An IR as partition spectra for a Convolver with block size B.
struct IRSpectra {
    size_t block = 0;
    std::vector<std::vector<cplxf>> parts;   // P partitions x (B + 1) bins

    static std::unique_ptr<IRSpectra> make(const std::vector<double>& ir, size_t block)
    {
        auto s = std::make_unique<IRSpectra>();
        s->block = block;
        const size_t P = std::max<size_t>(1, (ir.size() + block - 1) / block);
        Fft fft(2 * block);
        std::vector<cplxf> buf(2 * block);
        for (size_t p = 0; p < P; ++p) {
            std::fill(buf.begin(), buf.end(), cplxf(0));
            for (size_t i = 0; i < block && p * block + i < ir.size(); ++i) buf[i] = ir[p * block + i];
            fft.forward(buf.data());
            s->parts.emplace_back(buf.begin(), buf.begin() + (std::ptrdiff_t) block + 1);
        }
        return s;
    }
};

class Convolver {
public:
    // maxParts: the longest IR (in partitions) it will be handed
    void prepare(size_t blockSize, size_t maxParts, size_t fadeBlocks = 8)
    {
        B = blockSize;
        maxP = maxParts;
        fade = std::max<size_t>(1, fadeBlocks);
        fft.resize(2 * B);
        in.assign(2 * B, 0.0);
        spec.assign(2 * B, cplxf(0));
        work.assign(2 * B, cplxf(0));
        fdl.assign(maxP, std::vector<cplxf>(B + 1, cplxf(0)));
        outCur.assign(B, 0.0);
        outOld.assign(B, 0.0);
        inBlock.assign(B, 0.0);
        outBlock.assign(B, 0.0);
        pos = 0;
        head = 0;
        fadeLeft = 0;
    }
    void reset()
    {
        std::fill(in.begin(), in.end(), 0.0);
        for (auto& v : fdl) std::fill(v.begin(), v.end(), cplxf(0));
        std::fill(outBlock.begin(), outBlock.end(), 0.0);
        pos = 0;
    }

    // Hand over a new IR (audio thread) when canAccept(): it crossfades in over
    // `fade` blocks. The IR it replaces comes back from retired() once the fade
    // is done; the caller frees it off the audio thread.
    bool canAccept() const { return old == nullptr; }
    void setIR(IRSpectra* ir)
    {
        old = cur;
        cur = ir;
        fadeLeft = old ? fade : 0;
    }
    IRSpectra* retired()
    {
        if (!old || fadeLeft > 0) return nullptr;
        IRSpectra* r = old;
        old = nullptr;
        return r;
    }
    IRSpectra* current() const { return cur; }
    // give up every IR it holds (not on the audio thread while running); the caller frees them
    void takeAll(std::vector<IRSpectra*>& out)
    {
        if (cur) out.push_back(cur);
        if (old) out.push_back(old);
        cur = old = nullptr;
        fadeLeft = 0;
    }

    // one sample in, one sample out (latency B)
    double process(double x)
    {
        inBlock[pos] = x;
        const double y = outBlock[pos];
        if (++pos == B) {
            pos = 0;
            runBlock();
        }
        return y;
    }

private:
    size_t B = 64, maxP = 1, fade = 8, pos = 0, head = 0, fadeLeft = 0;
    Fft fft;
    std::vector<double> in, outCur, outOld, inBlock, outBlock;
    std::vector<cplxf> spec, work;
    std::vector<std::vector<cplxf>> fdl;   // frequency-domain delay line of input blocks
    IRSpectra* cur = nullptr;
    IRSpectra* old = nullptr;

    void convolve(const IRSpectra* ir, std::vector<double>& out)
    {
        if (!ir) { std::fill(out.begin(), out.end(), 0.0); return; }
        const size_t P = std::min(ir->parts.size(), maxP);
        std::fill(work.begin(), work.end(), cplxf(0));
        for (size_t p = 0; p < P; ++p) {
            const auto& X = fdl[(head + maxP - p) % maxP];
            const auto& H = ir->parts[p];
            for (size_t k = 0; k <= B; ++k) work[k] += X[k] * H[k];
        }
        for (size_t k = 1; k < B; ++k) work[2 * B - k] = std::conj(work[k]);
        fft.inverse(work.data());
        for (size_t i = 0; i < B; ++i) out[i] = work[B + i].real();   // overlap-save: the second half
    }

    void runBlock()
    {
        // input: previous block + this block
        std::copy(in.begin() + (std::ptrdiff_t) B, in.end(), in.begin());
        std::copy(inBlock.begin(), inBlock.end(), in.begin() + (std::ptrdiff_t) B);
        for (size_t i = 0; i < 2 * B; ++i) spec[i] = in[i];
        fft.forward(spec.data());
        head = (head + 1) % maxP;
        std::copy(spec.begin(), spec.begin() + (std::ptrdiff_t) B + 1, fdl[head].begin());
        convolve(cur, outCur);
        if (old && fadeLeft > 0) {
            convolve(old, outOld);
            for (size_t i = 0; i < B; ++i) {
                const double g = ((double) (fade - fadeLeft) * B + (double) i) / (double) (fade * B);   // 0 -> 1
                outBlock[i] = g * outCur[i] + (1 - g) * outOld[i];
            }
            --fadeLeft;
        } else {
            std::copy(outCur.begin(), outCur.end(), outBlock.begin());
        }
    }
};

} // namespace cd::acoustic
