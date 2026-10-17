// HFDL layers (hfdl_proto.h)
#include "dect2/hfdl_proto.h"
#include "dect2/acars_proto.h"
#include "dect2/aero_pos.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstring>

namespace dect2 {

namespace {
const HfdlRate kRates[8] = {
    {0, 300, 1, true, false, 72, 17},  {1, 600, 1, false, false, 72, 17},  {2, 1200, 2, false, false, 72, 17},  {3, 1800, 3, false, false, 72, 17},
    {4, 300, 1, true, true, 168, 23},  {5, 600, 1, false, true, 168, 23},  {6, 1200, 2, false, true, 168, 23},  {7, 1800, 3, false, true, 168, 23},
};
const uint8_t kAOct[16] = {0x5B, 0xBC, 0x74, 0x57, 0x03, 0xD9, 0x89, 0x39, 0xF2, 0x08, 0xD5, 0x36, 0x94, 0x2C, 0x32, 0xFE};
const char* kMBase = "0111011011110100010110010111110001000000110011011000111001110101110000100110000010101011010010010100111100100011010100001111111";
const int kMShift[8] = {72, 82, 113, 123, 61, 103, 93, 9};

void addLine(std::string& s, const char* fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    s += b; s += '\n';
}
int parity(unsigned v) { return __builtin_popcount(v) & 1; }
} // namespace

const int8_t kHfdlT[kHfdlTLen] = {1, 1, 1, -1, 1, 1, -1, -1, 1, -1, 1, -1, -1, -1, -1};

const HfdlRate& hfdlRate(int m1) { return kRates[std::max(0, std::min(7, m1))]; }
int hfdlBurstSyms(int m1) { return kHfdlPreLen + hfdlRate(m1).blocks * kHfdlBlockLen; }
int hfdlCodedBits(int m1) { const HfdlRate& r = hfdlRate(m1); return r.blocks * kHfdlDataLen * r.bitsPerSym; }
int hfdlPayloadOctets(int m1) { const HfdlRate& r = hfdlRate(m1); const int info = hfdlCodedBits(m1) / (r.quarter ? 4 : 2); return (info - 6) / 8; }

int hfdlABit(int k) { return (kAOct[k >> 3] >> (7 - (k & 7))) & 1; }
int hfdlMBit(int shift, int k) { return kMBase[(kMShift[shift & 7] + k) % 127] - '0'; }

const std::vector<float>& hfdlPreambleRef(int m1) {
    static std::vector<float> ref[8];
    static bool init = false;
    if (!init) {
        for (int m = 0; m < 8; m++) {
            auto& r = ref[m];
            for (int a = 0; a < 2; a++) for (int k = 0; k < kHfdlALen; k++) r.push_back(hfdlABit(k) ? -1.f : 1.f);
            for (int k = 0; k < kHfdlM1Len; k++) r.push_back(hfdlMBit(m, k) ? -1.f : 1.f);
            for (int k = 0; k < kHfdlM2Len; k++) r.push_back(hfdlMBit(m, k) ? -1.f : 1.f);
            for (int t = 0; t < kHfdlTrain; t++) for (int k = 0; k < kHfdlTLen; k++) r.push_back(kHfdlT[k]);
        }
        init = true;
    }
    return ref[std::max(0, std::min(7, m1))];
}

void hfdlScramblerSigns(int n, std::vector<int8_t>& out) {
    // 15-bit m-sequence restarted every 120 data symbols: the new bit is the parity of bits 14 and 0, shifted in at bit 0
    // (liquid-dsp msequence(15, 0x4001, 0x4D4B) as dumphfdl uses it; bit 1 turns the symbol by 180 degrees)
    out.resize((size_t)n);
    unsigned st = 0x4D4B;
    for (int i = 0; i < n; i++) {
        if (i % 120 == 0) st = 0x4D4B;
        const unsigned b = (unsigned)parity(st & 0x4001);
        st = ((st << 1) | b) & 0x7FFF;
        out[(size_t)i] = b ? -1 : 1;
    }
}

const std::vector<int>& hfdlInterleave(int m1) {
    static std::vector<int> tab[8];
    m1 = std::max(0, std::min(7, m1));
    if (tab[m1].empty()) {
        // channel bits are written down the 40 rows, the column moving back by the shift after every bit (and on by one after
        // each pass over the rows); the decoder reads them with a row step of 9, the column moving on after each pass
        const int N = hfdlCodedBits(m1), cols = N / 40, sh = hfdlRate(m1).colShift;
        std::vector<int> cell((size_t)N);
        int row = 0, col = 0;
        for (int i = 0; i < N; i++) {
            cell[(size_t)(row * cols + col)] = i;
            if (++row == 40) { row = 0; col++; }
            col = ((col - sh) % cols + cols) % cols;
        }
        tab[m1].resize((size_t)N);
        row = 0; col = 0;
        for (int j = 0; j < N; j++) {
            tab[m1][(size_t)j] = cell[(size_t)(row * cols + col)];
            row = (row + 9) % 40;
            if (row == 0) col++;
        }
    }
    return tab[m1];
}

std::vector<uint8_t> hfdlConvEncode(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> o;
    o.reserve(bits.size() * 2);
    unsigned sr = 0;
    for (uint8_t b : bits) {
        sr = ((sr << 1) | (b & 1)) & 0x7F;
        o.push_back((uint8_t)parity(sr & 0x6D));   // 0x6D first, then 0x4F: dumphfdl's libfec (V27POLYA 0x6d, V27POLYB 0x4f), checked on air
        o.push_back((uint8_t)parity(sr & 0x4F));
    }
    return o;
}

std::vector<uint8_t> hfdlViterbi(const std::vector<float>& soft, int n) {
    // state = the last 6 input bits (newest in bit 0)
    static int out0[128], out1[128];
    static bool init = false;
    if (!init) { for (int r = 0; r < 128; r++) { out0[r] = parity((unsigned)r & 0x6D); out1[r] = parity((unsigned)r & 0x4F); } init = true; }
    std::vector<float> m(64, -1e30f), nm(64);
    m[0] = 0;
    std::vector<uint8_t> dec((size_t)n * 64);
    for (int t = 0; t < n; t++) {
        const float s0 = soft[(size_t)2 * t], s1 = soft[(size_t)2 * t + 1];
        for (int ns = 0; ns < 64; ns++) {
            const int b = ns & 1;
            float best = -1e30f; int bp = 0;
            for (int p = 0; p < 2; p++) {
                const int ps = (ns >> 1) | (p << 5);
                const int reg = ((ps << 1) | b) & 0x7F;
                const float bm = (out0[reg] ? -s0 : s0) + (out1[reg] ? -s1 : s1);
                const float v = m[(size_t)ps] + bm;
                if (v > best) { best = v; bp = p; }
            }
            nm[(size_t)ns] = best;
            dec[(size_t)t * 64 + ns] = (uint8_t)bp;
        }
        const float mx = *std::max_element(nm.begin(), nm.end());
        for (int s = 0; s < 64; s++) m[(size_t)s] = nm[(size_t)s] - mx;
    }
    int s = (int)(std::max_element(m.begin(), m.end()) - m.begin());
    std::vector<uint8_t> o((size_t)n);
    for (int t = n - 1; t >= 0; t--) {
        o[(size_t)t] = (uint8_t)(s & 1);
        s = (s >> 1) | (dec[(size_t)t * 64 + s] << 5);
    }
    return o;
}

namespace {
// v (first bit sent in bit 0) -> the phase index k of k * 360 / M degrees: the first bit is the most significant bit of the
// Gray label (liquid-dsp PSK4 / PSK8 as dumphfdl demodulates them: QPSK at 0 / 90 / 180 / 270 degrees, not 45)
int pskPhase(int bps, int v) {
    int g = 0;
    for (int q = 0; q < bps; q++) g |= ((v >> q) & 1) << (bps - 1 - q);
    int k = g; k ^= k >> 1; k ^= k >> 2;
    return k & ((1 << bps) - 1);
}
int pskLabel(int bps, int k) {
    const int g = k ^ (k >> 1);
    int v = 0;
    for (int q = 0; q < bps; q++) v |= ((g >> (bps - 1 - q)) & 1) << q;
    return v;
}
} // namespace

std::complex<float> hfdlPsk(int bps, int v) {
    if (bps == 1) return {(v & 1) ? -1.f : 1.f, 0.f};
    const int M = 1 << bps;
    const float ph = (float)(2 * M_PI * pskPhase(bps, v & (M - 1)) / M);
    return {std::cos(ph), std::sin(ph)};
}

int hfdlDecide(int bps, std::complex<float> y) {
    if (bps == 1) return y.real() < 0;
    const int M = 1 << bps;
    int k = (int)std::lround(std::arg(y) / (2 * M_PI / M));
    k = (k % M + M) % M;
    return pskLabel(bps, k);
}

void hfdlSoftBits(int bps, std::complex<float> y, float scale, float* out) {
    if (bps == 1) { out[0] = y.real() * scale; return; }
    const int M = 1 << bps;
    float d[8];
    for (int v = 0; v < M; v++) d[v] = std::norm(y - hfdlPsk(bps, v));
    for (int b = 0; b < bps; b++) {
        float m0 = 1e30f, m1 = 1e30f;
        for (int v = 0; v < M; v++) { if ((v >> b) & 1) m1 = std::min(m1, d[v]); else m0 = std::min(m0, d[v]); }
        out[b] = (m1 - m0) * scale * 0.5f;
    }
}

std::vector<std::complex<float>> hfdlBurst(int m1, const std::vector<uint8_t>& pdu) {
    const HfdlRate& r = hfdlRate(m1);
    const int N = hfdlCodedBits(m1);
    const int info = N / (r.quarter ? 4 : 2);
    std::vector<uint8_t> bits((size_t)info, 0);
    const int octets = std::min((int)pdu.size(), hfdlPayloadOctets(m1));
    for (int i = 0; i < octets; i++) for (int b = 0; b < 8; b++) bits[(size_t)(i * 8 + b)] = (pdu[(size_t)i] >> b) & 1;
    std::vector<uint8_t> c = hfdlConvEncode(bits);
    if (r.quarter) { std::vector<uint8_t> d; d.reserve(c.size() * 2); for (uint8_t x : c) { d.push_back(x); d.push_back(x); } c.swap(d); }
    const auto& pos = hfdlInterleave(m1);
    std::vector<uint8_t> chan((size_t)N);
    for (int j = 0; j < N; j++) chan[(size_t)pos[(size_t)j]] = c[(size_t)j];
    std::vector<std::complex<float>> s;
    s.reserve((size_t)(kHfdlPrekey + hfdlBurstSyms(m1)));
    for (int k = 0; k < kHfdlPrekey; k++) s.push_back({(float)kHfdlT[k % kHfdlTLen], 0.f});   // prekey: not used by the receiver
    for (float v : hfdlPreambleRef(m1)) s.push_back({v, 0.f});
    const int nd = r.blocks * kHfdlDataLen;
    std::vector<int8_t> scr;
    hfdlScramblerSigns(nd, scr);
    int di = 0;
    for (int b = 0; b < r.blocks; b++) {
        for (int k = 0; k < kHfdlDataLen; k++, di++) {
            int v = 0;
            for (int q = 0; q < r.bitsPerSym; q++) v |= chan[(size_t)(di * r.bitsPerSym + q)] << q;
            s.push_back(hfdlPsk(r.bitsPerSym, v) * (float)scr[(size_t)di]);
        }
        for (int k = 0; k < kHfdlTLen; k++) s.push_back({(float)kHfdlT[k], 0.f});
    }
    return s;
}

std::vector<uint8_t> hfdlDecodeSoft(int m1, const std::vector<float>& chan) {
    const HfdlRate& r = hfdlRate(m1);
    const int N = hfdlCodedBits(m1);
    const auto& pos = hfdlInterleave(m1);
    std::vector<float> d((size_t)N);
    for (int j = 0; j < N; j++) d[(size_t)j] = chan[(size_t)pos[(size_t)j]];
    if (r.quarter) { std::vector<float> h((size_t)N / 2); for (int j = 0; j < N / 2; j++) h[(size_t)j] = d[(size_t)2 * j] + d[(size_t)2 * j + 1]; d.swap(h); }
    const int info = (int)d.size() / 2;
    const std::vector<uint8_t> bits = hfdlViterbi(d, info);
    std::vector<uint8_t> o((size_t)hfdlPayloadOctets(m1), 0);
    for (size_t i = 0; i < o.size(); i++) for (int b = 0; b < 8; b++) o[i] |= (uint8_t)(bits[i * 8 + (size_t)b] << b);
    return o;
}

uint16_t hfdlFcs(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1); }
    return (uint16_t)~c;
}
bool hfdlFcsOk(const uint8_t* p, size_t n) { const uint16_t f = hfdlFcs(p, n); return p[n] == (f & 0xFF) && p[n + 1] == (f >> 8); }

// ---------------------------------------------------------------- stations

const std::vector<HfdlStationInfo>& hfdlStations() {
    static const std::vector<HfdlStationInfo> t = {
        {1, "San Francisco", 37.6, -122.4, {21934, 17919, 13276, 11327, 10081, 8927, 6559, 5508}},
        {2, "Molokai", 21.2, -157.2, {21937, 17934, 13324, 13312, 10027, 8936, 8912, 5514}},
        {3, "Reykjavik", 64.1, -21.9, {17985, 15025, 11184, 8977, 6712, 5720, 3900}},
        {4, "Riverhead", 40.9, -72.6, {21931, 17919, 13276, 11387, 8912, 6661, 5652}},
        {5, "Auckland", -37.0, 174.8, {17916, 13351, 10084, 8921, 6535, 5583}},
        {6, "Hat Yai", 6.9, 100.4, {21949, 17928, 13270, 10066, 8825, 6535, 5655}},
        {7, "Shannon", 52.7, -8.9, {11384, 10081, 8942, 6532, 5547, 3455}},
        {8, "Johannesburg", -26.1, 28.2, {21949, 17922, 13321, 11321, 8834, 5529, 4681}},
        {9, "Barrow", 71.3, -156.8, {21937, 17916, 11354, 10093, 8936, 6646, 5544, 4687}},
        {10, "Muan", 34.9, 126.4, {21931, 17958, 13342, 11330, 8939, 6589, 5502}},
        {11, "Albrook", 9.0, -79.6, {17901, 13264, 10063, 8894, 6589, 5589}},
        {13, "Santa Cruz", -17.6, -63.1, {21997, 17916, 13315, 11318, 8957, 6628, 4660}},
        {14, "Krasnoyarsk", 56.2, 92.5, {21990, 17912, 13321, 10087, 8886, 6596, 5559}},
        {15, "Al Muharraq", 26.3, 50.6, {21982, 17967, 13354, 11348, 8885, 5544}},
        {16, "Agana", 13.5, 144.8, {21928, 17919, 13312, 11306, 8927, 6652}},
        {17, "Canarias", 27.9, -15.4, {21955, 17928, 13303, 11348, 8948, 6529, 5589}},
    };
    return t;
}
const HfdlStationInfo* hfdlStation(int id) { for (const auto& s : hfdlStations()) if (s.id == id) return &s; return nullptr; }
std::vector<int> hfdlMaskKhz(int gsId, uint32_t mask) {
    std::vector<int> o;
    const HfdlStationInfo* s = hfdlStation(gsId);
    for (int b = 0; b < 20; b++) if ((mask >> b) & 1) o.push_back(s && b < (int)s->khz.size() ? s->khz[(size_t)b] : -b - 1);   // -1 - bit: not in the table
    return o;
}
int hfdlStationOnKhz(int khz) { for (const auto& s : hfdlStations()) for (int k : s.khz) if (k == khz) return s.id; return 0; }

// ---------------------------------------------------------------- builders

namespace {
int32_t coordRaw(double deg) { int32_t v = (int32_t)std::lround(deg * 524288.0 / 180.0); v = std::max(-524288, std::min(524287, v)); return v & 0xFFFFF; }
double coordDeg(uint32_t raw) { int32_t v = (int32_t)(raw & 0xFFFFF); if (v & 0x80000) v -= 0x100000; return v * 180.0 / 524288.0; }
void putPosTime(std::vector<uint8_t>& o, const std::string& flight, double lat, double lon, int utcSec) {
    for (int i = 0; i < 6; i++) o.push_back(i < (int)flight.size() ? (uint8_t)flight[(size_t)i] : (uint8_t)' ');
    const uint32_t la = (uint32_t)coordRaw(lat), lo = (uint32_t)coordRaw(lon);
    o.push_back((uint8_t)la); o.push_back((uint8_t)(la >> 8)); o.push_back((uint8_t)(((la >> 16) & 0xF) | ((lo & 0xF) << 4)));
    o.push_back((uint8_t)(lo >> 4)); o.push_back((uint8_t)(lo >> 12));
    const int t2 = std::max(0, utcSec) / 2;
    o.push_back((uint8_t)t2); o.push_back((uint8_t)(t2 >> 8));
}
} // namespace

std::vector<uint8_t> hfdlSpdu(const HfdlSquitterSpec& s) {
    std::vector<uint8_t> b(66, 0);
    b[0] = (uint8_t)(((s.version & 3) << 2) | 0x20 | ((s.changeNote & 3) << 6));
    b[1] = (uint8_t)((s.gsId & 0x7F) | (s.utc ? 0x80 : 0));
    b[2] = (uint8_t)(s.frameIndex & 0xFF); b[3] = (uint8_t)((s.frameIndex >> 8) & 0xF);
    b[52] = 0;
    b[53] = (uint8_t)(s.systable & 0xFF);
    b[54] = (uint8_t)(((s.systable >> 8) & 0xF) | ((s.mask & 0xF) << 4));
    b[55] = (uint8_t)(s.mask >> 4); b[56] = (uint8_t)(s.mask >> 12);
    const HfdlSquitterGs& a = s.other[0]; const HfdlSquitterGs& c = s.other[1];
    b[57] = (uint8_t)((a.id & 0x7F) | (a.utc ? 0x80 : 0));
    b[58] = (uint8_t)a.mask; b[59] = (uint8_t)(a.mask >> 8);
    b[60] = (uint8_t)(((a.mask >> 16) & 0xF) | ((c.id & 0xF) << 4));
    b[61] = (uint8_t)(((c.id >> 4) & 7) | (c.utc ? 8 : 0) | ((c.mask & 0xF) << 4));
    b[62] = (uint8_t)(c.mask >> 4); b[63] = (uint8_t)(c.mask >> 12);
    const uint16_t f = hfdlFcs(b.data(), 64);
    b[64] = (uint8_t)f; b[65] = (uint8_t)(f >> 8);
    return b;
}

std::vector<uint8_t> hfdlLpdu(uint8_t type, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> o = {type};
    o.insert(o.end(), body.begin(), body.end());
    const uint16_t f = hfdlFcs(o.data(), o.size());
    o.push_back((uint8_t)f); o.push_back((uint8_t)(f >> 8));
    return o;
}
std::vector<uint8_t> hfdlLogonRequest(uint32_t icao) { return hfdlLpdu(0x8F, {(uint8_t)(icao >> 16), (uint8_t)(icao >> 8), (uint8_t)icao}); }
std::vector<uint8_t> hfdlLogonConfirm(uint32_t icao, int acId) { return hfdlLpdu(0x9F, {(uint8_t)(icao >> 16), (uint8_t)(icao >> 8), (uint8_t)icao, (uint8_t)acId, 0, 0, 0}); }
std::vector<uint8_t> hfdlUnnumbered(const std::vector<uint8_t>& h) { return hfdlLpdu(0x0D, h); }

std::vector<uint8_t> hfdlPerfData(const std::string& flight, double lat, double lon, int utcSec, int gsId, int freqId) {
    std::vector<uint8_t> o = {0xFF, 0xD1};
    putPosTime(o, flight, lat, lon, utcSec);
    o.push_back(2); o.push_back(1); o.push_back((uint8_t)(gsId & 0x7F)); o.push_back((uint8_t)freqId);
    o.resize(47, 0);
    o[27] = 3; o[35] = 12;      // a few counters
    return o;
}
std::vector<uint8_t> hfdlFreqData(const std::string& flight, double lat, double lon, int utcSec, const std::vector<HfdlSquitterGs>& heard) {
    std::vector<uint8_t> o = {0xFF, 0xD5};
    putPosTime(o, flight, lat, lon, utcSec);
    for (size_t i = 0; i < heard.size() && i < 6; i++) {
        const uint32_t m = heard[i].mask;
        o.push_back((uint8_t)(heard[i].id & 0x7F));
        o.push_back((uint8_t)m); o.push_back((uint8_t)(m >> 8)); o.push_back((uint8_t)(((m >> 16) & 0xF) | ((m & 0xF) << 4)));
        o.push_back((uint8_t)(m >> 4)); o.push_back((uint8_t)(m >> 12));
    }
    return o;
}
std::vector<uint8_t> hfdlAcarsHfnpdu(const std::string& reg, const std::string& label, char blockId, const std::string& text, bool) {
    AcarsBlockSpec s;
    s.reg = reg;
    s.label[0] = label.size() > 0 ? label[0] : 'H';
    s.label[1] = label.size() > 1 ? label[1] : '1';
    s.blockId = blockId;
    s.text = text;
    s.hasText = !text.empty();
    std::vector<uint8_t> f = acarsBuildFrame(s, 0);    // SYN SYN SOH ... DEL
    std::vector<uint8_t> o = {0xFF, 0xFF};
    o.insert(o.end(), f.begin() + 2, f.end());
    return o;
}

std::vector<uint8_t> hfdlMpduDown(int gsId, int acId, const std::vector<std::vector<uint8_t>>& l) {
    std::vector<uint8_t> o = {(uint8_t)(0x03 | ((l.size() & 0xF) << 2)), (uint8_t)(gsId & 0x7F), (uint8_t)acId, 0, 0, 0};
    for (const auto& x : l) o.push_back((uint8_t)(x.size() - 1));
    const uint16_t f = hfdlFcs(o.data(), o.size());
    o.push_back((uint8_t)f); o.push_back((uint8_t)(f >> 8));
    for (const auto& x : l) o.insert(o.end(), x.begin(), x.end());
    return o;
}
std::vector<uint8_t> hfdlMpduUp(int gsId, int acId, const std::vector<std::vector<uint8_t>>& l) {
    std::vector<uint8_t> o = {0x01, (uint8_t)(gsId & 0x7F), (uint8_t)acId, (uint8_t)((l.size() & 0xF) << 4)};
    for (const auto& x : l) o.push_back((uint8_t)(x.size() - 1));
    const uint16_t f = hfdlFcs(o.data(), o.size());
    o.push_back((uint8_t)f); o.push_back((uint8_t)(f >> 8));
    for (const auto& x : l) o.insert(o.end(), x.begin(), x.end());
    return o;
}

// ---------------------------------------------------------------- parser

namespace {

std::string khzList(const std::vector<int>& k) {
    std::string s;
    for (int v : k) { char b[16]; if (v > 0) snprintf(b, sizeof b, "%s%d", s.empty() ? "" : " ", v); else snprintf(b, sizeof b, "%sbit%d", s.empty() ? "" : " ", -v - 1); s += b; }
    return s.empty() ? "none" : s;
}
std::string gsName(int id) { const HfdlStationInfo* s = hfdlStation(id); char b[48]; snprintf(b, sizeof b, "%d %s", id, s ? s->name : "(not in the table)"); return b; }

void readAcars(const uint8_t* p, size_t n, HfdlMessage& m) {
    m.hfnpdu = "ACARS";
    m.acars = true;
    if (n && p[0] == kAcarsSoh) { p++; n--; }
    size_t end = n;
    if (end && p[end - 1] == 0x7F) end--;
    if (end < 13 + 2) { m.summary = "ACARS: cut short"; addLine(m.detail, "ACARS: %zu octets, too short", n); return; }
    const size_t bl = end - 2;
    m.acarsCrcOk = acarsCrc(p, bl + 2) == 0;
    std::vector<uint8_t> t7(bl);
    for (size_t i = 0; i < bl; i++) t7[i] = p[i] & 0x7F;
    AcarsMessage a;
    if (!acarsParseBlock(t7.data(), (int)bl, a)) { m.summary = "ACARS: not a valid block"; addLine(m.detail, "ACARS: the block layout is not valid"); return; }
    m.reg = a.reg; m.flight = a.flightId; m.label = a.label; m.labelText = a.labelText; m.msgNum = a.msgNum; m.acarsText = a.text; m.blockId = a.blockId;
    std::string s = "ACARS " + a.label;
    if (!a.labelText.empty()) s += " (" + a.labelText + ")";
    if (!a.flightId.empty()) s += " " + a.flightId;
    std::string one;
    for (char c : a.text) one += (c == '\n' || c == '\r') ? ' ' : c;
    if (!one.empty()) s += ": " + (one.size() > 80 ? one.substr(0, 80) + "..." : one);
    m.summary = s;
    addLine(m.detail, "ACARS: mode %c, registration %s, label %s%s%s, block id %c%s", a.mode ? a.mode : '-', a.reg.empty() ? "-" : a.reg.c_str(), a.label.c_str(),
            a.labelText.empty() ? "" : " ", a.labelText.c_str(), a.blockId ? a.blockId : '-', m.acarsCrcOk ? "" : ", check sequence FAILED");
    if (!a.msgNum.empty() || !a.flightId.empty()) addLine(m.detail, "  message %s, flight %s", a.msgNum.empty() ? "-" : a.msgNum.c_str(), a.flightId.empty() ? "-" : a.flightId.c_str());
    if (!a.decoded.empty()) addLine(m.detail, "  reading: %s", a.decoded.c_str());
    AeroPosition pos;
    std::string adsc;
    if (aeroPositionFromMessage(a.label, a.text, m.uplink, pos, &adsc) && pos.valid) {
        m.hasPos = true; m.lat = pos.lat; m.lon = pos.lon; m.hasAlt = pos.hasAlt; m.altFt = pos.altFt; m.posKind = pos.kind;
        if (!pos.flightId.empty() && m.flight.empty()) m.flight = pos.flightId;
    }
    if (!adsc.empty()) { addLine(m.detail, "ADS-C (FANS-1/A):"); m.detail += adsc; if (m.detail.back() != '\n') m.detail += '\n'; }
    if (m.hasPos) addLine(m.detail, "  position %.4f %.4f%s (%s)", m.lat, m.lon, m.hasAlt ? (" " + std::to_string(m.altFt) + " ft").c_str() : "", m.posKind.c_str());
}

void readPosTime(const uint8_t* h, HfdlMessage& m, const char* what) {
    std::string fl;
    for (int i = 2; i < 8; i++) if (h[i] >= 0x20 && h[i] < 0x7F) fl += (char)h[i];
    while (!fl.empty() && fl.back() == ' ') fl.pop_back();
    while (!fl.empty() && fl.front() == ' ') fl.erase(0, 1);
    const uint32_t la = h[8] | (h[9] << 8) | ((h[10] & 0xF) << 16);
    const uint32_t lo = ((h[10] & 0xF0) >> 4) | (h[11] << 4) | (h[12] << 12);
    const int t = (h[13] | (h[14] << 8)) * 2;
    m.flight = fl;
    m.lat = coordDeg(la); m.lon = coordDeg(lo);
    m.hasPos = std::fabs(m.lat) <= 90 && std::fabs(m.lon) <= 180 && !(la == 0 && lo == 0);
    m.posKind = what;
    m.utcSec = t;
    addLine(m.detail, "%s: flight %s, position %.4f %.4f, time %02d:%02d:%02d UTC", what, fl.empty() ? "-" : fl.c_str(), m.lat, m.lon, t / 3600 % 24, t % 3600 / 60, t % 60);
}

void readHfnpdu(const uint8_t* h, size_t n, HfdlMessage& m) {
    if (n < 2 || h[0] != 0xFF) { m.summary = m.kind + " (" + std::to_string(n) + " octets, not an HFNPDU)"; addLine(m.detail, "  %zu octets of user data", n); return; }
    char b[200];
    switch (h[1]) {
    case 0xFF: readAcars(h + 2, n - 2, m); break;
    case 0xD1:
        m.hfnpdu = "Performance data";
        if (n < 47) { m.summary = "Performance data (short)"; break; }
        readPosTime(h, m, "Performance data");
        addLine(m.detail, "  version %d, flight leg %d, ground station %s, frequency id %d", h[15], h[16], gsName(h[17] & 0x7F).c_str(), h[18]);
        addLine(m.detail, "  MPDUs received 1800/1200/600/300: %d %d %d %d, with errors %d %d %d %d; SPDUs received %d, missed %d", h[27], h[28], h[29], h[30], h[31], h[32], h[33], h[34],
                h[35] | (h[36] << 8), h[37]);
        snprintf(b, sizeof b, "Performance data %s  %.3f %.3f", m.flight.c_str(), m.lat, m.lon);
        m.summary = b;
        break;
    case 0xD5: {
        m.hfnpdu = "Frequency data";
        if (n < 15) { m.summary = "Frequency data (short)"; break; }
        readPosTime(h, m, "Frequency data");
        int cnt = 0;
        for (size_t p = 15; p + 6 <= n && cnt < 6; p += 6, cnt++) {
            const int gs = h[p] & 0x7F;
            const uint32_t heard = h[p + 1] | (h[p + 2] << 8) | ((h[p + 3] & 0xF) << 16);
            const uint32_t tuned = ((h[p + 3] & 0xF0) >> 4) | (h[p + 4] << 4) | (h[p + 5] << 12);
            addLine(m.detail, "  station %s: heard on %s kHz, listening on %s kHz", gsName(gs).c_str(), khzList(hfdlMaskKhz(gs, heard)).c_str(), khzList(hfdlMaskKhz(gs, tuned)).c_str());
        }
        snprintf(b, sizeof b, "Frequency data %s  %.3f %.3f, %d station%s heard", m.flight.c_str(), m.lat, m.lon, cnt, cnt == 1 ? "" : "s");
        m.summary = b;
        break;
    }
    case 0xD0: m.hfnpdu = "System table"; if (n >= 5) { snprintf(b, sizeof b, "System table part %d of %d, version %d", h[2] & 0xF, (h[2] >> 4) + 1, (h[3] >> 4) | (h[4] << 4)); m.summary = b; addLine(m.detail, "  %s", b); } break;
    case 0xD2: m.hfnpdu = "System table request"; m.summary = "System table request"; break;
    case 0xDE: m.hfnpdu = "Delayed echo"; m.summary = "Delayed echo"; break;
    default: m.hfnpdu = "HFNPDU"; snprintf(b, sizeof b, "HFNPDU type 0x%02X (%zu octets)", h[1], n); m.summary = b; break;
    }
}

void readLpdu(const uint8_t* p, size_t n, HfdlMessage& m) {
    const size_t len = n - 2;     // the FCS was checked
    const uint8_t t = p[0];
    auto icao = [&](size_t at) { return len >= at + 3 ? (uint32_t)((p[at] << 16) | (p[at + 1] << 8) | p[at + 2]) : 0u; };
    char b[160];
    switch (t) {
    case 0x0D: case 0x1D:
        m.kind = t == 0x0D ? "Unnumbered data" : "Unnumbered acked data";
        addLine(m.detail, "LPDU: %s, %zu octets", m.kind.c_str(), len - 1);
        readHfnpdu(p + 1, len - 1, m);
        break;
    case 0x8F: case 0xBF: case 0x4F:
        m.kind = t == 0x8F ? "Logon request" : t == 0xBF ? "Logon request (DLS)" : "Logon resume";
        m.icao = icao(1);
        snprintf(b, sizeof b, "%s from %06X", m.kind.c_str(), m.icao); m.summary = b; addLine(m.detail, "LPDU: %s", b);
        break;
    case 0x9F: case 0x5F:
        m.kind = t == 0x9F ? "Logon confirm" : "Logon resume confirm";
        m.icao = icao(1);
        if (len >= 5) m.acId = p[4];
        snprintf(b, sizeof b, "%s %06X, aircraft id %d", m.kind.c_str(), m.icao, m.acId); m.summary = b; addLine(m.detail, "LPDU: %s", b);
        break;
    case 0x2F: case 0x3F:
        m.kind = t == 0x2F ? "Logon denied" : "Logoff request";
        m.icao = icao(1);
        snprintf(b, sizeof b, "%s %06X, reason %d", m.kind.c_str(), m.icao, len >= 5 ? p[4] : -1); m.summary = b; addLine(m.detail, "LPDU: %s", b);
        break;
    default:
        m.kind = "LPDU";
        snprintf(b, sizeof b, "LPDU type 0x%02X (%zu octets)", t, len); m.summary = b; addLine(m.detail, "%s", b);
        break;
    }
}

} // namespace

bool hfdlParsePdu(const uint8_t* p, size_t n, std::vector<HfdlMessage>& out) {
    if (n < 4) return false;
    if (!(p[0] & 1)) {    // SPDU
        if (n < 66 || !hfdlFcsOk(p, 64)) return false;
        HfdlMessage m;
        m.squitter = true; m.uplink = true; m.kind = "SPDU";
        m.gsId = p[1] & 0x7F;
        m.utcSync = p[1] & 0x80;
        m.frameIndex = p[2] | ((p[3] & 0xF) << 8);
        static const char* notes[4] = {"none", "channel down", "upcoming frequency change", "ground station down"};
        const uint32_t mask0 = (p[54] >> 4) | (p[55] << 4) | (p[56] << 12);
        m.activeKhz = hfdlMaskKhz(m.gsId, mask0);
        const int t = m.frameIndex * 32;
        addLine(m.detail, "SPDU (squitter) from ground station %s, %s", gsName(m.gsId).c_str(), m.utcSync ? "UTC synchronised" : "not UTC synchronised");
        addLine(m.detail, "  version %d, change note: %s, RLS %s, ISO 8208 %s", (p[0] >> 2) & 3, notes[(p[0] >> 6) & 3], (p[0] & 2) ? "in use" : "not in use", (p[0] & 0x20) ? "supported" : "not supported");
        addLine(m.detail, "  TDMA frame %d (%02d:%02d:%02d UTC at 32 s per frame), offset %d, minimum priority %d, system table version %d", m.frameIndex, t / 3600 % 24, t % 3600 / 60, t % 60, p[3] >> 4,
                p[52] & 0xF, p[53] | ((p[54] & 0xF) << 8));
        addLine(m.detail, "  frequencies in use: %s kHz", khzList(m.activeKhz).c_str());
        const int id1 = p[57] & 0x7F, id2 = (p[60] >> 4) | ((p[61] & 7) << 4);
        const uint32_t m1 = p[58] | (p[59] << 8) | ((p[60] & 0xF) << 16), m2 = (p[61] >> 4) | (p[62] << 4) | (p[63] << 12);
        if (id1) addLine(m.detail, "  station %s%s: %s kHz", gsName(id1).c_str(), (p[57] & 0x80) ? " (UTC)" : "", khzList(hfdlMaskKhz(id1, m1)).c_str());
        if (id2) addLine(m.detail, "  station %s%s: %s kHz", gsName(id2).c_str(), (p[61] & 8) ? " (UTC)" : "", khzList(hfdlMaskKhz(id2, m2)).c_str());
        char b[200];
        const HfdlStationInfo* s = hfdlStation(m.gsId);
        snprintf(b, sizeof b, "Squitter %s, frame %d, %zu frequencies in use", s ? s->name : ("GS " + std::to_string(m.gsId)).c_str(), m.frameIndex, m.activeKhz.size());
        m.summary = b;
        out.push_back(std::move(m));
        return true;
    }
    const bool down = p[0] & 2;
    struct Part { int acId; size_t len; };
    std::vector<Part> parts;
    size_t hdr = 0;
    const int gs = p[1] & 0x7F;
    if (down) {
        const int cnt = (p[0] >> 2) & 0xF;
        hdr = 6 + (size_t)cnt;
        if (n < hdr + 2) return false;
        for (int i = 0; i < cnt; i++) parts.push_back({p[2], (size_t)p[6 + i] + 1});
    } else {
        const int nac = ((p[0] & 0x70) >> 4) + 1;
        size_t at = 2;
        for (int a = 0; a < nac; a++) {
            if (n < at + 2) return false;
            const int ac = p[at], cnt = (p[at + 1] >> 4) & 0xF;
            if (n < at + 2 + (size_t)cnt) return false;
            for (int i = 0; i < cnt; i++) parts.push_back({ac, (size_t)p[at + 2 + i] + 1});
            at += 2 + (size_t)cnt;
        }
        hdr = at;
    }
    if (n < hdr + 2 || !hfdlFcsOk(p, hdr)) return false;
    size_t at = hdr + 2;
    int good = 0;
    for (const Part& q : parts) {
        if (at + q.len > n) break;
        HfdlMessage m;
        m.uplink = !down; m.gsId = gs; m.acId = q.acId;
        addLine(m.detail, "MPDU %s, ground station %s, aircraft id %d", down ? "downlink" : "uplink", gsName(gs).c_str(), q.acId);
        if (q.len >= 3 && hfdlFcsOk(p + at, q.len - 2)) { readLpdu(p + at, q.len, m); good++; }
        else { m.kind = "LPDU"; m.summary = "LPDU with a bad check sequence"; addLine(m.detail, "LPDU: check sequence FAILED"); }
        out.push_back(std::move(m));
        at += q.len;
    }
    if (parts.empty()) {
        HfdlMessage m;
        m.uplink = !down; m.gsId = gs; m.acId = down ? p[2] : -1; m.kind = "MPDU"; m.summary = "MPDU without LPDUs";
        out.push_back(std::move(m));
    }
    return true;
    (void)good;
}

} // namespace dect2
