// IMBE through the user-installed mbelib (see p25_imbe.h); only the two functions of mbelib.h (1.3) that are needed.
#include "dect2/p25_imbe.h"
#include "../native_common.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {
struct Api {
    native::DynLib lib;
    bool ok = false;
    // mbe_parms is kept in a larger buffer of our own, so its layout is never needed
    void (DECT2_CALL* init)(void* cur, void* prev, void* prevEnh) = nullptr;
    void (DECT2_CALL* frame)(short* aout, int* errs, int* errs2, char* errStr, char fr[8][23], char d[88], void* cur, void* prev, void* prevEnh, int uvq) = nullptr;
};

const Api* scan() {
    Api* r = new Api;   // kept for the life of the program: decoders hold the one they started with
    std::vector<std::string> names;
    if (const char* e = getenv("DECT2_MBELIB_PATH")) if (*e) names.push_back(e);
    if (names.empty()) names = native::codecLibNames(DECT2_MBE_STEM, DECT2_MBE_SO, DECT2_MBE_WIN);
    if (r->lib.open(names)) r->ok = r->lib.get(r->init, "mbe_initMbeParms") && r->lib.get(r->frame, "mbe_processImbe7200x4400Frame");
    return r;
}

std::mutex gMu;
std::atomic<const Api*> gApi{nullptr};

const Api& api() {
    if (const Api* a = gApi.load(std::memory_order_acquire)) return *a;
    std::lock_guard<std::mutex> lk(gMu);
    if (!gApi.load()) gApi.store(scan(), std::memory_order_release);
    return *gApi.load();
}
} // namespace

bool p25ImbeAvailable(std::string* path) {
    const Api& a = api();
    if (a.ok && path) *path = a.lib.path;
    return a.ok;
}

bool p25ImbeRescan() {
    if (api().ok) return true;
    std::lock_guard<std::mutex> lk(gMu);
    gApi.store(scan(), std::memory_order_release);
    return gApi.load()->ok;
}

struct P25Imbe::Impl {
    const Api* a = nullptr;
    std::vector<double> cur, prev, enh;
};

P25Imbe::P25Imbe() : p_(std::make_unique<Impl>()) {
    const Api& a = api();
    if (!a.ok) return;
    p_->a = &a;
    p_->cur.assign(2048, 0.0); p_->prev.assign(2048, 0.0); p_->enh.assign(2048, 0.0);   // 16 kB each
    a.init(p_->cur.data(), p_->prev.data(), p_->enh.data());
}
P25Imbe::~P25Imbe() = default;
bool P25Imbe::ok() const { return p_->a != nullptr; }

void P25Imbe::decode(const char frIn[8][23], int16_t pcm[160]) {
    if (!p_->a) { memset(pcm, 0, 160 * sizeof(int16_t)); return; }
    char fr[8][23], d[88] = {}, err[64] = {};
    memcpy(fr, frIn, sizeof fr);
    int e1 = 0, e2 = 0;
    short out[160] = {};
    p_->a->frame(out, &e1, &e2, err, fr, d, p_->cur.data(), p_->prev.data(), p_->enh.data(), 3);
    for (int i = 0; i < 160; i++) pcm[i] = out[i];
}

} // namespace dect2
