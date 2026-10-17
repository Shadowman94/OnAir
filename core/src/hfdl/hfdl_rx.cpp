// HFDL receiver (hfdl_rx.h)
#include "dect2/hfdl_rx.h"
#include "dect2/results_io.h"
#include "dect2/hfdl_proto.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <mutex>

namespace dect2 {

namespace {

constexpr double kFs = 7200.0;       // 4 samples per symbol
constexpr int kSps = 4;
constexpr int kSearchSyms = 2 * kHfdlALen;
constexpr int kChunk = 8;
constexpr double kMaxCfo = 250.0;
constexpr int kCoarseHyp = 4;          // trial offsets of the preamble search: -4 .. 4 steps
constexpr double kCoarseStep = 100.0;   // Hz (covers +-400 Hz in 8-symbol chunks; burst() refines +-250 Hz more)
constexpr double kCoarseThr = 0.50;     // blind preamble correlation (noise alone averages 1/sqrt(8) = 0.35 per hypothesis, more as the best of 9)
constexpr double kCoarseThrNearDc = 0.52; // dial channel sits on the LO: need a higher bar (DC spur false locks)
constexpr double kNearDcHz = 2500.0;
constexpr int kL = 12;               // feed-forward taps each side, half a symbol apart
constexpr int kNb = 6;               // feedback taps
constexpr double kDefCentre = 8.9e6;
using cd = std::complex<double>;

std::vector<float> lowpass(int taps, double fc, double fs) {
    std::vector<float> h((size_t)taps);
    double s = 0;
    for (int i = 0; i < taps; i++) {
        const double m = i - (taps - 1) / 2.0;
        const double x = 2 * fc / fs * m;
        const double sinc = std::fabs(m) < 1e-9 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
        const double w = 0.42 - 0.5 * std::cos(2 * M_PI * i / (taps - 1)) + 0.08 * std::cos(4 * M_PI * i / (taps - 1));
        h[(size_t)i] = (float)(2 * fc / fs * sinc * w);
        s += h[(size_t)i];
    }
    for (auto& v : h) v = (float)(v / s);
    return h;
}

double rrc(double t, double a) {
    if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / M_PI;
    if (std::fabs(std::fabs(t) - 1 / (4 * a)) < 1e-9) return a / std::sqrt(2.0) * ((1 + 2 / M_PI) * std::sin(M_PI / (4 * a)) + (1 - 2 / M_PI) * std::cos(M_PI / (4 * a)));
    return (std::sin(M_PI * t * (1 - a)) + 4 * a * t * std::cos(M_PI * t * (1 + a))) / (M_PI * t * (1 - 16 * a * a * t * t));
}

cf32 cubicAt(const std::vector<cf32>& x, double pos) {
    const int i = (int)std::floor(pos);
    const float f = (float)(pos - i);
    auto at = [&](int k) { return k >= 0 && k < (int)x.size() ? x[(size_t)k] : cf32(0, 0); };
    const cf32 a = at(i - 1), b = at(i), c = at(i + 1), d = at(i + 2);
    const cf32 c0 = b, c1 = 0.5f * (c - a), c2 = a - 2.5f * b + 2.f * c - 0.5f * d, c3 = 0.5f * (d - a) + 1.5f * (b - c);
    return ((c3 * f + c2) * f + c1) * f + c0;
}

std::string hex6(uint32_t a) { char b[8]; snprintf(b, sizeof b, "%06X", a & 0xFFFFFF); return b; }

} // namespace

std::vector<double> hfdlChannelsNear(double c, double half) {
    std::vector<double> o;
    for (const auto& s : hfdlStations())
        for (int k : s.khz) { const double f = k * 1e3; if (std::fabs(f + kHfdlCarrierHz - c) <= half && std::find(o.begin(), o.end(), f) == o.end()) o.push_back(f); }
    std::sort(o.begin(), o.end());
    return o;
}

struct HfdlReceiver::Impl {
    struct Chan {
        double dial = 0; double offHz = 0; int gsId = 0; bool inBand = false;
        cd ph{1, 0}, rot{1, 0}; int phN = 0;
        int D = 1; cd i1{0, 0}, i2{0, 0}, c1{0, 0}, c2{0, 0}; int dcnt = 0;
        std::vector<cf32> fh; size_t fpos = 0; int M = 1, mcnt = 0;
        double r2 = 0, tNext = 0; int64_t n2 = 0; cf32 h2[4] = {};
        std::vector<cf32> mh; size_t mpos = 0;
        std::vector<cf32> x; int64_t xBase = 0;          // matched filter output at 7200 Hz, x[0] is sample xBase
        int64_t searchAt = 0, waitUntil = 0;
        double noisePow = 0; double blkPow = 0; int blkN = 0;
        HfdlChannelInfo info;
    };
    std::mutex mu;
    std::function<void(const std::string&)> logCb;
    double rate = 0, offset = 0, center = 0;
    std::vector<double> want;
    bool rebuild = true;
    std::vector<Chan> ch;
    std::vector<float> fir, mf;
    uint64_t nIn = 0;
    uint64_t seq = 0, serial = 0;
    HfdlTelemetry tel;
    double lastGood = -1e9, lastBurst = -1e9;
    std::map<uint32_t, uint32_t> acIcao;      // gs << 8 | ac id -> ICAO
    std::map<int, bool> logged;

    double nowSec() const { return rate > 0 ? (double)nIn / rate : 0; }
    // mu held: the clock starts again (configure), the kept lists move back with it
    void shiftTimes(double dt) {
        for (auto& m : tel.messages) m.timeSec += dt;
        for (auto& a : tel.aircraft) { a.lastHeardSec += dt; if (a.hasPos) a.posTime += dt; for (auto& p : a.track) p.time += dt; }
        for (auto& g : tel.stations) if (g.lastHeardSec != 0) g.lastHeardSec += dt;
        tel.nowSec += dt;
    }

    void setup() {
        rebuild = false;
        ch.clear();
        if (rate < 1) return;
        const double c = center > 0 ? center : kDefCentre;
        const double half = rate / 2 - 3000;
        std::vector<double> list = want.empty() ? hfdlChannelsNear(c, std::max(0.0, half * 0.95)) : want;
        const int D = std::max(1, (int)std::floor(rate / 28800));
        const double r1 = rate / D;
        const int M = std::max(1, (int)std::floor(r1 / kFs));
        int taps = (int)std::ceil(4.0 * r1 / 2000) | 1;
        taps = std::max(31, std::min(301, taps));
        fir = lowpass(taps, 1700, r1);
        mf.clear();
        double s = 0;
        for (int i = -6 * kSps; i <= 6 * kSps; i++) { mf.push_back((float)rrc((double)i / kSps, kHfdlRrcAlpha)); s += mf.back(); }
        for (auto& v : mf) v = (float)(v / s);
        for (double f : list) {
            Chan k;
            k.dial = f;
            const int khz = (int)std::lround(f / 1e3);
            k.gsId = std::fabs(f - khz * 1e3) < 1 ? hfdlStationOnKhz(khz) : 0;
            const double off = f + kHfdlCarrierHz - c + offset;
            k.offHz = off;
            k.inBand = std::fabs(off) < half;
            k.rot = std::polar(1.0, -2 * M_PI * off / rate);
            k.D = D; k.M = M;
            k.fh.assign((size_t)taps, cf32(0, 0));
            k.r2 = r1 / M;
            k.mh.assign(mf.size(), cf32(0, 0));
            k.info.freqHz = f; k.info.gsId = k.gsId; k.info.inBand = k.inBand;
            ch.push_back(std::move(k));
        }
        tel.inputRate = rate;
        tel.centerHz = center;
    }

    // ------------------------------------------------ front end
    void push(Chan& k, cf32 v) {
        const cd y = cd(v.real(), v.imag()) * k.ph;
        k.ph *= k.rot;
        if (++k.phN >= 4096) { k.ph /= std::abs(k.ph); k.phN = 0; }
        k.i1 += y; k.i2 += k.i1;
        if (++k.dcnt < k.D) return;
        k.dcnt = 0;
        const cd d1 = k.i2 - k.c1; k.c1 = k.i2;
        const cd d2 = d1 - k.c2; k.c2 = d1;
        const cf32 z = cf32(d2 / (double)(k.D * k.D));
        k.fh[k.fpos] = z;
        k.fpos = (k.fpos + 1) % k.fh.size();
        if (++k.mcnt < k.M) return;
        k.mcnt = 0;
        cf32 acc(0, 0);
        const size_t T = k.fh.size();
        for (size_t i = 0; i < T; i++) acc += k.fh[(k.fpos + i) % T] * fir[i];
        // cubic interpolation from r2 to 7200 Hz
        k.h2[0] = k.h2[1]; k.h2[1] = k.h2[2]; k.h2[2] = k.h2[3]; k.h2[3] = acc;
        k.n2++;
        // h2[1] .. h2[2] spans input samples n2-3 .. n2-2
        const double step = k.r2 / kFs;
        while (k.tNext <= (double)(k.n2 - 2)) {
            const double f = k.tNext - (double)(k.n2 - 3);
            if (f >= 0) {
                const float u = (float)f;
                const cf32 a = k.h2[0], b = k.h2[1], c = k.h2[2], d = k.h2[3];
                const cf32 c1 = 0.5f * (c - a), c2 = a - 2.5f * b + 2.f * c - 0.5f * d, c3 = 0.5f * (d - a) + 1.5f * (b - c);
                symSample(k, ((c3 * u + c2) * u + c1) * u + b);
            }
            k.tNext += step;
        }
    }
    void symSample(Chan& k, cf32 v) {
        k.mh[k.mpos] = v;
        k.mpos = (k.mpos + 1) % k.mh.size();
        cf32 acc(0, 0);
        const size_t T = k.mh.size();
        for (size_t i = 0; i < T; i++) acc += k.mh[(k.mpos + i) % T] * mf[i];
        k.x.push_back(acc);
        k.blkPow += std::norm(acc); k.blkN++;
        if (k.blkN >= 1800) {     // a quarter second: the noise floor follows the quiet blocks
            const double p = k.blkPow / k.blkN;
            if (k.noisePow <= 0) k.noisePow = p;
            else if (p < k.noisePow * 2) k.noisePow += 0.1 * (p - k.noisePow);
            else k.noisePow *= 1.01;
            k.blkPow = 0; k.blkN = 0;
            k.info.noiseDb = (float)(10 * std::log10(k.noisePow + 1e-20));
        }
    }

    // ------------------------------------------------ search
    cf32 X(const Chan& k, int64_t i) const { const int64_t j = i - k.xBase; return j >= 0 && j < (int64_t)k.x.size() ? k.x[(size_t)j] : cf32(0, 0); }

    // A1 + A2 against the reference turned by a few trial frequency offsets: 8-symbol chunks only stay coherent within about
    // +-60 Hz, and a receiver off by a few ppm (or a station reported with the wrong carrier) is 100-250 Hz away
    static const std::vector<std::vector<cf32>>& coarseRefs() {
        static std::vector<std::vector<cf32>> r;
        if (r.empty()) {
            const auto& ref = hfdlPreambleRef(0);
            for (int h = -kCoarseHyp; h <= kCoarseHyp; h++) {
                std::vector<cf32> v((size_t)kSearchSyms);
                for (int q = 0; q < kSearchSyms; q++) v[(size_t)q] = ref[(size_t)q] * std::polar(1.f, (float)(-2 * M_PI * h * kCoarseStep * q / kHfdlBaud));
                r.push_back(std::move(v));
            }
        }
        return r;
    }
    double coarse(const Chan& k, int64_t s) const {
        const int64_t i0 = s - k.xBase;   // whole samples: cubicAt() would only return x[i] here, at several times the cost
        if (i0 < 0 || i0 + (int64_t)(kSearchSyms - 1) * kSps >= (int64_t)k.x.size()) return 0;
        const cf32* p = &k.x[(size_t)i0];
        double sumA = 0;
        for (int q = 0; q < kSearchSyms; q++) sumA += std::abs(p[q * kSps]);
        if (sumA <= 1e-6 * kSearchSyms) return 0;
        double best = 0;
        for (const auto& ref : coarseRefs()) {
            double sumC = 0;
            for (int c = 0; c < kSearchSyms / kChunk + 1; c++) {
                cf32 acc(0, 0);
                const int k0 = c * kChunk, k1 = std::min(kSearchSyms, k0 + kChunk);
                for (int q = k0; q < k1; q++) acc += p[q * kSps] * ref[(size_t)q];
                sumC += std::abs(acc);
            }
            best = std::max(best, sumC);
        }
        return best / sumA;
    }

    // y_k = ref_k * x(t0 + 4k), interpolated
    void refProducts(const Chan& k, double t0, int n, std::vector<cf32>& y) const {
        const auto& ref = hfdlPreambleRef(0);
        y.resize((size_t)n);
        for (int q = 0; q < n; q++) {
            const double pos = t0 + q * kSps - (double)k.xBase;
            y[(size_t)q] = cubicAt(k.x, pos) * ref[(size_t)q];
        }
    }
    static cf32 corrAt(const std::vector<cf32>& y, double f, int k0, int k1) {
        const cf32 r = std::polar(1.f, (float)(-2 * M_PI * f / kHfdlBaud));
        cf32 ph = std::polar(1.f, (float)(-2 * M_PI * f * k0 / kHfdlBaud)), acc(0, 0);
        for (int q = k0; q < k1; q++) { acc += y[(size_t)q] * ph; ph *= r; }
        return acc;
    }

    void process(Chan& k) {
        const int64_t end = k.xBase + (int64_t)k.x.size();
        const int64_t span = (int64_t)kSearchSyms * kSps + 16;
        const double thr = std::fabs(k.offHz) < kNearDcHz ? kCoarseThrNearDc : kCoarseThr;
        if (k.searchAt < k.xBase) k.searchAt = k.xBase;
        if (end < k.waitUntil) return;
        while (k.searchAt + span + (int64_t)(kHfdlPreLen + 8) * kSps < end) {
            const int64_t s = k.searchAt;
            // no look-ahead past the prekey: a false preamble there moves searchAt beyond the real one
            const double m = coarse(k, s);
            if (m < thr) { k.searchAt += 2; continue; }
            int64_t best = s;
            double bm = m;
            for (int64_t q = s - 2; q <= s + 10; q++) {
                if (q < k.xBase) continue;
                const double v = coarse(k, q);
                if (v > bm) { bm = v; best = q; }
            }
            const int r = burst(k, best, end);
            if (r < 0) return;
            if (k.searchAt <= s) k.searchAt = s + kSps * 4;   // never spin on a false preamble
        }
        trim(k);
    }

    void trim(Chan& k) {
        const int64_t keep = k.searchAt - 64;
        if (keep - k.xBase > 40000) { k.x.erase(k.x.begin(), k.x.begin() + (keep - k.xBase)); k.xBase = keep; }
    }

    // returns -1 when more samples are needed, else 0 (searchAt moved on)
    int burst(Chan& k, int64_t s0, int64_t end) {
        std::vector<cf32> y;
        // coherent frequency search on A1 + A2
        refProducts(k, (double)s0, kSearchSyms, y);
        double bf = 0, bv = -1;
        std::vector<double> mags;
        for (double f = -kMaxCfo; f <= kMaxCfo + 1e-9; f += 1.5) { const double v = std::abs(corrAt(y, f, 0, kSearchSyms)); mags.push_back(v); if (v > bv) { bv = v; bf = f; } }
        {
            const int i = (int)std::lround((bf + kMaxCfo) / 1.5);
            if (i > 0 && i + 1 < (int)mags.size()) { const double a = mags[(size_t)i - 1], b = mags[(size_t)i], c = mags[(size_t)i + 1]; const double den = a - 2 * b + c; if (den < 0) bf += 1.5 * 0.5 * (a - c) / den; }
        }
        // timing
        double tv[5];
        for (int d = -2; d <= 2; d++) { refProducts(k, (double)(s0 + d), kSearchSyms, y); tv[d + 2] = std::abs(corrAt(y, bf, 0, kSearchSyms)); }
        int bd = 2; for (int d = 0; d < 5; d++) if (tv[d] > tv[bd]) bd = d;
        double tau = bd - 2;
        if (bd > 0 && bd < 4) { const double den = tv[bd - 1] - 2 * tv[bd] + tv[bd + 1]; if (den < 0) tau += 0.5 * (tv[bd - 1] - tv[bd + 1]) / den; }
        const double t0 = (double)s0 + tau;
        refProducts(k, t0, kSearchSyms, y);
        const cf32 C1 = corrAt(y, bf, 0, kHfdlALen), C2 = corrAt(y, bf, kHfdlALen, kSearchSyms);
        const double df = std::arg(C2 * std::conj(C1)) * kHfdlBaud / (2 * M_PI * kHfdlALen);
        if (std::fabs(df) < 6) bf += df;
        const cf32 Call = corrAt(y, bf, 0, kSearchSyms);
        const cf32 h = Call / (float)kSearchSyms;
        { double se = 0; for (const auto& v : y) se += std::norm(v); if (std::norm(Call) < 0.3 * kSearchSyms * se || std::norm(h) < 1e-12) { k.searchAt = std::max(k.searchAt, s0) + 2; return 0; } }   // not A1 + A2 coherently
        // M1
        const cf32 rot = std::polar(1.f, (float)(-2 * M_PI * bf / kHfdlBaud));
        auto sym = [&](int i) { return cubicAt(k.x, t0 + i * kSps - (double)k.xBase) * std::polar(1.f, (float)(-2 * M_PI * bf * i / kHfdlBaud)); };
        (void)rot;
        int m1 = -1; double bs = -1e30;
        std::vector<cf32> ms((size_t)kHfdlM1Len);
        for (int j = 0; j < kHfdlM1Len; j++) ms[(size_t)j] = sym(kSearchSyms + j) * std::conj(h);
        for (int sc = 0; sc < 8; sc++) {
            double sum = 0;
            for (int j = 0; j < kHfdlM1Len; j++) sum += (hfdlMBit(sc, j) ? -1 : 1) * ms[(size_t)j].real();
            if (sum > bs) { bs = sum; m1 = sc; }
        }
        k.info.bursts++;
        lastBurst = nowSec();
        double me = 0; for (const auto& v : ms) me += std::norm(v);
        const double score = bs / std::sqrt(kHfdlM1Len * me + 1e-30);
        if (score < 0.3) { k.searchAt = std::max(k.searchAt + 2, s0 + kSearchSyms * kSps); k.info.bursts--; return 0; }   // not a preamble after all
        const int nsym = hfdlBurstSyms(m1);
        if ((int64_t)std::ceil(t0) + (int64_t)(nsym + kL + 8) * kSps >= end) { k.info.bursts--; k.waitUntil = (int64_t)std::ceil(t0) + (int64_t)(nsym + kL + 8) * kSps + 1; return -1; }
        // half-symbol samples, derotated
        const int nu = 2 * nsym + 4 * kL + 4;
        std::vector<cf32> u((size_t)nu);
        const int uoff = 2 * kL + 2;      // u[uoff + 2i] is symbol i
        for (int m = 0; m < nu; m++) {
            const double si = (m - uoff) / 2.0;
            u[(size_t)m] = cubicAt(k.x, t0 + si * kSps - (double)k.xBase) * std::polar(1.f, (float)(-2 * M_PI * bf * si / kHfdlBaud));
        }
        double pw = 0;
        for (int i = 0; i < kHfdlPreLen; i++) pw += std::norm(u[(size_t)(uoff + 2 * i)]);
        pw /= kHfdlPreLen;
        // reference / known flags
        const HfdlRate& R = hfdlRate(m1);
        const auto& pre = hfdlPreambleRef(m1);
        std::vector<int8_t> scr;
        hfdlScramblerSigns(R.blocks * kHfdlDataLen, scr);
        // equaliser
        const int Nf = 2 * kL + 1;
        std::vector<cf32> w((size_t)Nf, cf32(0, 0)), b((size_t)kNb, cf32(0, 0)), dp((size_t)kNb, cf32(0, 0));
        w[(size_t)kL] = std::conj(h) / (std::norm(h) + 1e-30f);
        float theta = 0;
        double errKnown = 0; int nKnown = 0;
        std::vector<float> soft((size_t)hfdlCodedBits(m1));
        auto run = [&](int i, bool known, cf32 ref, float mu, int di) {
            const cf32* v = &u[(size_t)(uoff + 2 * i - kL)];
            cf32 yv(0, 0);
            float nrm = 1e-9f;
            for (int q = 0; q < Nf; q++) { yv += w[(size_t)q] * v[q]; nrm += std::norm(v[q]); }
            for (int q = 0; q < kNb; q++) { yv += b[(size_t)q] * dp[(size_t)q]; nrm += std::norm(dp[(size_t)q]); }
            const cf32 rr = std::polar(1.f, -theta);
            const cf32 z = yv * rr;
            cf32 d;
            if (known) d = ref;
            else {
                const float sg = (float)scr[(size_t)di];
                const cf32 q = z * sg;
                d = hfdlPsk(R.bitsPerSym, hfdlDecide(R.bitsPerSym, q)) * sg;
            }
            const cf32 e = d - z;
            if (known) { errKnown += std::norm(e); nKnown++; }
            const cf32 E = e * std::conj(rr);
            const float g = mu / nrm;
            for (int q = 0; q < Nf; q++) w[(size_t)q] += g * E * std::conj(v[q]);
            for (int q = 0; q < kNb; q++) b[(size_t)q] += g * E * std::conj(dp[(size_t)q]);
            theta += 0.03f * (z * std::conj(d)).imag() / (std::norm(d) + 1e-9f);
            for (int q = kNb - 1; q > 0; q--) dp[(size_t)q] = dp[(size_t)q - 1];
            dp[0] = d;
            return z;
        };
        for (int pass = 0; pass < 2; pass++) {
            std::fill(dp.begin(), dp.end(), cf32(0, 0));
            theta = 0;
            errKnown = 0; nKnown = 0;
            for (int i = 0; i < kHfdlPreLen; i++) run(i, true, cf32(pre[(size_t)i], 0), pass == 0 ? 0.25f : 0.1f, 0);
        }
        const double preErr = errKnown / std::max(1, nKnown);
        errKnown = 0; nKnown = 0;
        const float muD = 0.15f, muP = 0.5f;      // normalised LMS steps on decisions and on the probes
        std::vector<cf32> dataSym((size_t)(R.blocks * kHfdlDataLen));
        std::vector<double> probeErr((size_t)R.blocks + 1);
        probeErr[0] = preErr;
        int i = kHfdlPreLen, di = 0;
        for (int blk = 0; blk < R.blocks; blk++) {
            for (int q = 0; q < kHfdlDataLen; q++, i++, di++) dataSym[(size_t)di] = run(i, false, cf32(), muD, di) * (float)scr[(size_t)di];
            const double e0 = errKnown; const int n0 = nKnown;
            for (int q = 0; q < kHfdlTLen; q++, i++) run(i, true, cf32((float)kHfdlT[q], 0), muP, 0);
            probeErr[(size_t)blk + 1] = (errKnown - e0) / std::max(1, nKnown - n0);
        }
        const double sig2 = std::max(1e-4, errKnown / std::max(1, nKnown));
        // each block weighted by the error on the probes beside it (a fade makes its bits less sure)
        for (size_t s = 0; s < dataSym.size(); s++) {
            const size_t blk = s / kHfdlDataLen;
            const double e = std::max(1e-4, 0.5 * (probeErr[blk] + probeErr[blk + 1]));
            hfdlSoftBits(R.bitsPerSym, dataSym[s], (float)(1.0 / e), &soft[s * (size_t)R.bitsPerSym]);
        }
        const std::vector<uint8_t> pdu = hfdlDecodeSoft(m1, soft);
        std::vector<HfdlMessage> msgs;
        const bool ok = hfdlParsePdu(pdu.data(), pdu.size(), msgs);
        const double tStart = (double)(t0) / kFs - kHfdlPrekey / kHfdlBaud;
        const float snr = (float)(-10 * std::log10(sig2));
        const float lvl = (float)(10 * std::log10(pw + 1e-20));
        k.searchAt = (int64_t)t0 + (int64_t)(nsym - 40) * kSps;
        std::lock_guard<std::mutex> lk(mu);
        k.info.levelDb = lvl; k.info.snrDb = snr; k.info.lastCfoHz = (float)bf;
        if (!ok) { k.info.bad++; tel.blocksBad++; k.searchAt = s0 + kSearchSyms * kSps; return 0; }
        k.info.good++; k.info.lastHeardSec = nowSec(); k.info.lastBps = R.bps;
        tel.blocksOk++; tel.byRate[m1 & 3]++;
        tel.cfoHz = bf; tel.snrDb = snr; tel.dataValid = true;
        lastGood = nowSec();
        for (auto& m : msgs) {
            m.serial = ++serial; m.timeSec = tStart; m.wallTime = (int64_t)time(nullptr); m.freqHz = k.dial; m.levelDb = lvl; m.snrDb = snr; m.cfoHz = (float)bf;
            m.bps = R.bps; m.slots = R.twoSlots ? 2 : 1;
            note(k, m);
            tel.messages.insert(tel.messages.begin(), m);
        }
        if (tel.messages.size() > kHfdlMsgMax) tel.messages.resize(kHfdlMsgMax);
        return 0;
    }

    HfdlGroundStation& station(int id) {
        for (auto& g : tel.stations) if (g.id == id) return g;
        HfdlGroundStation g;
        g.id = id;
        if (const HfdlStationInfo* s = hfdlStation(id)) { g.name = s->name; g.hasPos = true; g.lat = s->lat; g.lon = s->lon; }
        tel.stations.push_back(g);
        std::sort(tel.stations.begin(), tel.stations.end(), [](const HfdlGroundStation& a, const HfdlGroundStation& b) { return a.id < b.id; });
        for (auto& x : tel.stations) if (x.id == id) return x;
        return tel.stations.front();
    }

    void note(Chan& k, HfdlMessage& m) {
        const double now = nowSec();
        if (m.squitter) {
            tel.squitters++;
            HfdlGroundStation& g = station(m.gsId);
            g.squitters++; g.freqHz = k.dial; g.lastHeardSec = now; g.levelDb = m.levelDb; g.utcSync = m.utcSync; g.frameIndex = m.frameIndex; g.activeKhz = m.activeKhz; g.heardDirect = true;
            if (!logged[m.gsId]) {
                logged[m.gsId] = true;
                if (logCb) { char b[160]; snprintf(b, sizeof b, "HFDL: ground station %d %s heard on %.3f MHz", m.gsId, g.name.c_str(), k.dial / 1e6); logCb(b); }
            }
            return;
        }
        tel.lpdus++;
        if (m.gsId) { HfdlGroundStation& g = station(m.gsId); g.messages++; if (m.uplink) { g.heardDirect = true; g.lastHeardSec = now; g.freqHz = k.dial; } }
        const uint32_t slot = (uint32_t)(m.gsId << 8) | (uint32_t)(m.acId & 0xFF);
        if (m.kind == "Logon confirm" || m.kind == "Logon resume confirm") { if (m.icao && m.acId >= 0) acIcao[((uint32_t)m.gsId << 8) | (uint32_t)m.acId] = m.icao; }
        if (!m.icao && m.acId > 0) { auto it = acIcao.find(slot); if (it != acIcao.end()) m.icao = it->second; }
        if (m.acars) tel.acarsMsgs++;
        if (!m.icao && m.acId <= 0 && m.reg.empty()) return;
        const uint32_t key = m.icao ? m.icao : 0x1000000u | slot;
        HfdlAircraft* a = nullptr;
        for (auto& x : tel.aircraft) if (x.key == key) { a = &x; break; }
        if (!a && m.icao) for (auto& x : tel.aircraft) if (x.key == (0x1000000u | slot)) { x.key = key; a = &x; break; }
        if (!a) { tel.aircraft.push_back(HfdlAircraft()); a = &tel.aircraft.back(); a->key = key; }
        a->icao = m.icao ? m.icao : a->icao;
        if (m.acId > 0) a->acId = m.acId;
        a->gsId = m.gsId;
        if (!m.reg.empty()) a->reg = m.reg;
        if (!m.flight.empty()) a->flight = m.flight;
        a->messages++; a->freqHz = k.dial; a->lastHeardSec = now;
        if (!m.uplink) a->levelDb = m.levelDb;
        if (m.kind == "Logon confirm") a->loggedOn = true;
        if (m.kind == "Logoff request" || m.kind == "Logon denied") a->loggedOn = false;
        if (m.hasPos) {
            tel.positionsTotal++;
            a->hasPos = true; a->lat = m.lat; a->lon = m.lon; a->posKind = m.posKind; a->posTime = now;
            if (m.hasAlt) { a->hasAlt = true; a->altFt = m.altFt; }
            a->track.push_back({m.lat, m.lon, now});
            if (a->track.size() > 24) a->track.erase(a->track.begin());
        }
        std::stable_sort(tel.aircraft.begin(), tel.aircraft.end(), [](const HfdlAircraft& x, const HfdlAircraft& y) { return x.lastHeardSec > y.lastHeardSec; });
        if (tel.aircraft.size() > 200) tel.aircraft.resize(200);
        (void)hex6;
    }
};

HfdlReceiver::HfdlReceiver() : p_(std::make_unique<Impl>()) {}
HfdlReceiver::~HfdlReceiver() = default;

void HfdlReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->rebuild = true; p_->nIn = 0; p_->shiftTimes(-p_->tel.nowSec); }
void HfdlReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->offset != hz) { p_->offset = hz; p_->rebuild = true; } }
void HfdlReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (p_->center != hz) { p_->center = hz; p_->rebuild = true; } }
bool HfdlReceiver::ready() const { return p_->rate >= hfdlTuning().minSampleRate - 1; }
void HfdlReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->logCb = std::move(cb); }
void HfdlReceiver::setChannels(const std::vector<double>& hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->want) { p_->want = hz; p_->rebuild = true; } }

void HfdlReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    HfdlTelemetry t;                     // the messages, aircraft and ground stations stay (clearResults); the clock runs on
    t.centerHz = p_->tel.centerHz; t.nowSec = p_->tel.nowSec;
    t.messages = std::move(p_->tel.messages); t.aircraft = std::move(p_->tel.aircraft); t.stations = std::move(p_->tel.stations);
    p_->tel = std::move(t);
    p_->lastGood = p_->lastBurst = -1e9;
    p_->rebuild = true;
    p_->seq++;
}

void HfdlReceiver::feed(const cf32* x, size_t n) {
    Impl& I = *p_;
    { std::lock_guard<std::mutex> lk(I.mu); if (I.rebuild) I.setup(); }
    if (I.rate < 1 || I.ch.empty()) { I.nIn += n; return; }
    for (auto& k : I.ch) {
        if (!k.inBand) continue;
        for (size_t i = 0; i < n; i++) {
            cf32 v = x[i];
            if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
            I.push(k, v);
        }
        I.process(k);
    }
    I.nIn += n;
    std::lock_guard<std::mutex> lk(I.mu);
    const double now = I.nowSec();
    I.tel.nowSec = now;
    I.tel.channels.clear();
    for (auto& k : I.ch) { k.info.active = k.info.lastHeardSec > 0 && now - k.info.lastHeardSec < 40; I.tel.channels.push_back(k.info); }
    I.tel.state = now - I.lastGood < 120 ? 2 : now - I.lastBurst < 60 ? 1 : 0;
    I.seq++;
}

void HfdlReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->tel.messages.clear(); p_->tel.aircraft.clear(); p_->tel.stations.clear(); p_->acIcao.clear();
    p_->seq++;
}
std::string HfdlReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return results::pack(p_->tel.nowSec, p_->serial, p_->tel.messages, p_->tel.aircraft, p_->tel.stations, p_->acIcao);
}
bool HfdlReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    uint64_t serial = 0;
    HfdlTelemetry l;
    std::map<uint32_t, uint32_t> acIcao;
    if (!results::unpack(blob, t0, serial, l.messages, l.aircraft, l.stations, acIcao)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    const double now = p_->tel.nowSec;
    p_->tel.messages = std::move(l.messages); p_->tel.aircraft = std::move(l.aircraft); p_->tel.stations = std::move(l.stations);
    p_->acIcao = std::move(acIcao);
    p_->serial = std::max(p_->serial, serial);
    p_->tel.nowSec = t0;
    p_->shiftTimes(now - t0);
    p_->seq++;
    return true;
}

bool HfdlReceiver::telemetry(HfdlTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->seq == lastSeq) return false;
    out = p_->tel;
    out.seq = p_->seq;
    return true;
}

ModeTuning hfdlTuning() {
    ModeTuning t;
    t.stdMode = 37; t.id = "hfdl"; t.name = "HFDL";
    t.minMhz = 2; t.maxMhz = 30; t.defMhz = 8.9;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.4;
    t.minSampleRate = 24000;
    t.tuneOffsetHz = 0;          // the channels sit beside the centre; the DC spike falls between them
    return t;
}

} // namespace dect2
