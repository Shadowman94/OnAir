// Shared declarations of the OnAir window: the application state, small helpers and the panels that draw the interface.
// The panels live in widgets.cpp, app_state.cpp, toolbar.cpp, plots.cpp, analysis_tabs.cpp, tv.cpp, scan_outputs.cpp,
// antenna.cpp, dab_ui.cpp, wizard.cpp; main.cpp has the window loop.
#pragma once
// OnAir — digital TV receiver (DVB-T2, DVB-T, ATSC) for macOS. Phase 0 shell: sources, spectrum, waterfall, status, log.
#ifdef _WIN32
#include <direct.h>
#endif
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "plot.h"
#include "i18n.h"
#include "fm_saved.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>


#include "dect2/engine.h"
#include "dect2/modes.h"
#include "dect2/gpu_ldpc.h"
#include "dect2/t2rx.h"
#include "dect2/nettuner.h"
#include "dect2/netradio.h"
#include "dect2/hamlib_rig.h"
#include "dect2/rig_setup.h"
#include "airplay.h"
#include "dect2/timecompat.h"
#include "dect2/platform.h"
#include "dect2/scanner.h"
#include "dect2/updater.h"
#include "dect2/gain.h"
#include "dect2/gain_opt.h"
#include "dect2/tx.h"
#include "dect2/channel.h"
#include "dect2/dvbt.h"
#include "dect2/quality.h"
#include "dect2/direction.h"
#include "dect2/teletext.h"
#include "dect2/feeds.h"
#include "dect2/share.h"
#include "dect2/pskreporter.h"
#include "dect2/wsprnet.h"
#include "dect2/passes.h"
#include "dect2/maplayers.h"
#include "theme.h"
#include "gfx.h"
#include "icons.h"
#include "mascot.h"
#include "scale.h"
#include "platform.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>
#include <unistd.h>

using namespace dect2;

// ------------------------------------------------------------------ waterfall texture

struct Waterfall {
    static constexpr int W = 4096, H = 512;
    enum Map { Classic, Viridis, Inferno, Grey, Turbo, Cividis, NumMaps };   // Cividis: made to read the same with red-green colour blindness
    static const char* mapName(int m) {
        static const char* const n[NumMaps] = {"Classic", "Viridis", "Inferno", "Grey", "Turbo", "Cividis"};
        return n[m < 0 || m >= NumMaps ? 0 : m];
    }
    gfx::Image* img = nullptr;
    uint32_t lut[256] = {};
    int builtMap = -1;
    std::vector<uint32_t> row;      // scratch for the row being written (kept: no allocation per frame)
    std::vector<float> acc, floorScratch;   // the highest level per bin since the last row; the samples of the noise floor estimate
    bool accEmpty = true;
    int writeRow = 0; // newest row; older rows follow at writeRow+1 ... wrapping
    int filled = 0;
    double pushT = 0; // when the newest row came in (glfwGetTime)
    double rowDt = 1.0 / 30; // steady seconds per row, for the time labels and the scrolling
    double stamps[128] = {};
    int nStamps = 0;
    float minDb = -100, maxDb = -32;
    int map = -1;            // the colour map (Map); -1: the one that goes with the interface theme
    bool autoTrack = false;  // the intensity range follows the noise floor
    float rowsPerSec = 0;    // 0: one row per spectrum frame (about 30 a second); else this many rows a second, each the highest level of the frames in between
    bool paused = false;     // no new rows: the picture stays and can be scrolled back
    double nextRowT = 0;
    double trackT = 0;

    // average over the last 128 rows, and move only when that drifts by more than 2%: the labels stay put
    void stamp(double t) {
        const int N = 128;
        if (nStamps >= N) {
            const double dt = (t - stamps[nStamps % N]) / N;
            if (dt > 0 && std::fabs(dt - rowDt) > 0.02 * rowDt) rowDt = dt;
        } else if (nStamps >= 8) rowDt = (t - stamps[0]) / nStamps;   // starting up: whatever we have
        stamps[nStamps % N] = t;
        nStamps++;
        pushT = t;
    }
    bool slow() const { return rowsPerSec > 0.5f && rowsPerSec < 28.f; }   // rows come at a set, slower pace
    bool timeKnown() const { return nStamps >= 128 || slow(); }
    void setRate(float r) { rowsPerSec = r; nStamps = 0; nextRowT = 0; accEmpty = true; rowDt = slow() ? 1.0 / r : 1.0 / 30; }

    int effMap() const { return map >= 0 && map < NumMaps ? map : pal::wfMode() == 2 ? Turbo : pal::wfMode() == 1 ? Grey : Classic; }
    static int& curMap() { static int m = 0; return m; }   // the map in use, for the other waterfalls (modes with their own picture) that colour with jet()
    static uint32_t jet(float v) { return color(curMap(), v); }
    void palette() { builtMap = effMap(); curMap() = builtMap; for (int i = 0; i < 256; i++) lut[i] = color(builtMap, i / 255.0f); }   // after the map or the interface palette changed
    void init(gfx::Backend* gfx) {
        img = gfx->createImage(W, H, 0xFF000000u);
        row.assign(W, 0);
        palette();
    }
    static uint32_t stopsColor(const float (*s)[3], int n, float v) {   // n colour stops (0..255 per channel), blended linearly
        v *= (float)(n - 1);
        const int i = std::min(n - 2, (int)v);
        const float f = v - i;
        auto ch = [&](int k) { return (uint32_t)(s[i][k] + (s[i + 1][k] - s[i][k]) * f + 0.5f); };
        return 0xFF000000u | (ch(2) << 16) | (ch(1) << 8) | ch(0);
    }
    static uint32_t color(int map, float v) {
        v = std::min(1.f, std::max(0.f, v));
        if (map == Turbo) {   // Turbo (Google's colour map), with the noise floor fading to black
            const float t = 0.12f + 0.84f * v;
            auto poly = [&](float a0, float a1, float a2, float a3, float a4, float a5) { return std::min(1.f, std::max(0.f, a0 + t * (a1 + t * (a2 + t * (a3 + t * (a4 + t * a5)))))); };
            const float r = poly(0.13572138f, 4.61539260f, -42.66032258f, 132.13108234f, -152.94239396f, 59.28637943f);
            const float g = poly(0.09140261f, 2.19418839f, 4.84296658f, -14.18503333f, 4.27729857f, 2.82956604f);
            const float b = poly(0.10667330f, 12.64194608f, -60.58204836f, 110.36276771f, -89.90310912f, 27.34824973f);
            const float f = std::min(1.f, v / 0.22f), k = f * f * (3 - 2 * f);   // smoothstep: black at the bottom
            return 0xFF000000u | ((uint32_t)(b * k * 255 + 0.5f) << 16) | ((uint32_t)(g * k * 255 + 0.5f) << 8) | (uint32_t)(r * k * 255 + 0.5f);
        }
        static const float cool[5][3] = {{6, 10, 20}, {16, 38, 70}, {30, 100, 140}, {110, 190, 205}, {240, 250, 250}};   // Classic: dark blue, teal, near white
        static const float grey[5][3] = {{0, 0, 0}, {34, 34, 33}, {96, 95, 92}, {170, 168, 162}, {244, 242, 236}};
        static const float viridis[9][3] = {{68, 1, 84}, {71, 44, 122}, {59, 81, 139}, {44, 113, 142}, {33, 144, 141}, {39, 173, 129}, {92, 200, 99}, {170, 220, 50}, {253, 231, 37}};
        static const float inferno[9][3] = {{0, 0, 4}, {31, 12, 72}, {85, 15, 109}, {136, 34, 106}, {186, 54, 85}, {227, 89, 51}, {249, 140, 10}, {249, 201, 50}, {252, 255, 164}};
        static const float cividis[9][3] = {{0, 32, 77}, {26, 52, 107}, {66, 78, 108}, {97, 104, 111}, {124, 123, 120}, {154, 145, 119}, {187, 167, 108}, {221, 193, 88}, {255, 233, 69}};
        switch (map) {
        case Cividis: return stopsColor(cividis, 9, v);
        case Viridis: return stopsColor(viridis, 9, v);
        case Inferno: return stopsColor(inferno, 9, v);
        case Grey: return stopsColor(grey, 5, v);
        default: return stopsColor(cool, 5, v);
        }
    }

    // One spectrum frame arrives: it becomes a row of the texture, now or (rows at a set pace) when the next row is due. True when a row was written.
    bool offer(const std::vector<float>& dbfs, double now) {
        if (paused || dbfs.empty()) return false;
        if (effMap() != builtMap) palette();
        const float* d = dbfs.data();
        if (slow()) {   // the highest level of every bin since the last row, so a short burst is not missed
            if (acc.size() != dbfs.size() || accEmpty) { acc = dbfs; accEmpty = false; }
            else for (size_t i = 0; i < dbfs.size(); i++) acc[i] = std::max(acc[i], dbfs[i]);
            if (now < nextRowT) return false;
            const double dt = 1.0 / rowsPerSec;
            nextRowT = now - nextRowT > dt ? now + dt : nextRowT + dt;
            accEmpty = true;
            d = acc.data();
        }
        writeFrom(d, dbfs.size());
        return true;
    }
    // the bins of the frame onto the 4096 columns (the highest of those that share a column), the levels onto the colour table: one row uploaded
    void writeFrom(const float* d, size_t n) {
        const float inv = 255.f / std::max(1.f, maxDb - minDb);
        uint32_t* r = row.data();
        for (int x = 0; x < W; x++) {
            const size_t a = (size_t)x * n / W, b = std::max(a + 1, (size_t)(x + 1) * n / W);
            float m = d[a];
            for (size_t k = a + 1; k < b; k++) m = std::max(m, d[k]);
            const float v = (m - minDb) * inv;
            r[x] = lut[v <= 0.f ? 0 : v >= 255.f ? 255 : (int)v];
        }
        writeRow = (writeRow + H - 1) % H;
        img->update(0, writeRow, W, 1, r);
        filled = std::min(H, filled + 1);
    }
    // the range from the spectrum: the noise floor is the 20th percentile, the top is the strongest signal (at most 45 dB above the floor).
    // instant: set it at once; else move a part of the way (called a few times a second, so the colours do not jump)
    void fitRange(const std::vector<float>& s, bool instant) {
        const size_t n = s.size();
        if (n < 64) return;
        const size_t step = std::max<size_t>(1, n / 1024);
        floorScratch.clear();
        float top = -300;
        for (size_t i = 0; i < n; i += step) { floorScratch.push_back(s[i]); top = std::max(top, s[i]); }
        const size_t k = floorScratch.size() / 5;
        std::nth_element(floorScratch.begin(), floorScratch.begin() + (long)k, floorScratch.end());
        const float fl = floorScratch[k];
        const float lo = std::floor(fl - 6.f);
        float hi = std::ceil(std::min(top + 2.f, fl + 45.f));
        if (hi < lo + 12.f) hi = lo + 12.f;
        if (instant) { minDb = lo; maxDb = hi; } else { minDb += (lo - minDb) * 0.25f; maxDb += (hi - maxDb) * 0.25f; }
    }
};

extern std::string gForceTab;

// ------------------------------------------------------------------ app state

struct BwChoice { const char* label; double mhz; double nativeMsps; };
inline const BwChoice kBw[] = {
    {"8 MHz", 8, 64.0 / 7}, {"7 MHz", 7, 8.0}, {"6 MHz", 6, 48.0 / 7}, {"5 MHz", 5, 40.0 / 7}, {"1.7 MHz", 1.7, 131.0 / 71},
};

extern gfx::Backend* gGfx;
extern GLFWwindow* gWindow;
extern int gWinX, gWinY, gWinW, gWinH;

// One subtitle service of the playing programme: a DVB subtitling stream (EN 300 743) or a teletext subtitle page (EN 300 706)
struct SubOpt {
    bool ttx = false;
    int pid = 0, comp = 0, anc = 0;   // DVB: composition / ancillary page id; teletext: comp = page number
    std::string lang, key, label;
    bool fallback = false;            // teletext without a subtitling entry: page 888, else 777
};

struct VideoTex {
    gfx::Video* tex = nullptr;
    double pts = 0;          // presentation time (seconds, as in the stream) of the newest picture: subtitles are timed against it
    // subtitle overlay (subs_ui.cpp): the DVB subtitle bitmap as a texture over the picture, and a hook that draws any overlay on it
    gfx::Video* sub = nullptr;
    int subW = 0, subH = 0;
    uint64_t subSeq = 0;
    bool subShow = false;
    void (*overlayFn)(void* ctx, ImDrawList* dl, ImVec2 p0, ImVec2 size) = nullptr;
    void* overlayCtx = nullptr;
    int w = 0, h = 0;
    double dar = 0;          // display aspect of the newest picture (0 = use w / h); it can change between programmes
    uint64_t seq = 0;
    bool deint = true;
    bool has() const { return tex != nullptr; }
    void update(Player& pl) {
        auto f = pl.videoFrame(seq);
        if (!f || f->w <= 0) return;
        dar = f->dar;
        pts = f->pts;
        const bool yuv = f->rgba.empty();
        if (!tex || w != f->w || h != f->h) {
            delete tex;
            tex = gGfx->createVideo(f->w, f->h);
            w = f->w; h = f->h;
        }
        if (!yuv) { tex->uploadRGBA(f->rgba.data()); return; }
        tex->uploadNV12(f->y.data(), f->uv.data(), f->bt709, f->fullRange, f->interlaced && deint);
    }
    void draw(ImVec2 box) {
        if (!tex) return;
        if (box.x < 1 || box.y < 1 || h <= 0) return;
        float ar = dar > 0.05 ? (float)dar : (float)w / h;
        ImVec2 sz = box;
        if (box.x / box.y > ar) sz.x = box.y * ar; else sz.y = box.x / ar;
        ImVec2 p = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(p.x + (box.x - sz.x) * 0.5f, p.y + (box.y - sz.y) * 0.5f));
        ImGui::Image(tex->texture(), sz);
        if (overlayFn) overlayFn(overlayCtx, ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), sz);
        ImGui::SetCursorPos(p);
    }
};

using SavedChannel = plat::Channel;


// parts of the window the tour points at
enum WizTarget { TgNone = 0, TgSwitch, TgToolbar, TgMain, TgRight, TgConst, TgCount };

constexpr int kNumFamilies = 85;   // App::family runs from 0 to kNumFamilies - 1
constexpr int kNumSubModes = 31;   // dect2::subModes(): the protocols of HF digital, mesh, pagers, marine and VOR / ILS listed as modes of their own
constexpr int kNumSlots = kNumFamilies + kNumSubModes;   // what keeps its own frequency and gains: each family, and each protocol entry

struct App {
    Engine engine;
    int rxId = 1;                 // receiver number: RX 1 is the first (receivers.h)
    bool rxForcedMute = false, rxSavedMuted = false;   // only the receiver that plays gets sound: the others are muted
    NetTuner net{engine};      // network tuner (declared after the engine so that it stops first)
    bool netOn = false, netLan = false;
    int netPort = 8089;
    char netKey[32] = "";
    // first-run tour (wizard.inc)
    bool wizOpen = false, wizStart = false, wizAcked = false;
    bool galleryOpen = false;           // the picture gallery (View > Images)
    bool passesOpen = false;            // the satellite pass planner window (View > Passes)
    bool scannerOpen = false;           // the scanner window (View > Scanner, highly experimental)
    bool mapOpen = false;               // the map window (View > Map): positions and Maidenhead locators
    bool rotatorOpen = false;           // View > Antenna rotator (rotator_ui.cpp)
    int wizStep = 0;
    bool setupOpen = false;             // the setup wizard (language, radio, location, goals); Help > Setup wizard
    int setupStep = 0, setupRegion = 0;
    unsigned setupGoals = 0;            // bit per goal in wizard.cpp
    char setupLoc[48] = "";             // a Maidenhead locator or coordinates
    double wizStepT = 0;
    float wizX = -1;
    ImVec2 tgMin[8], tgMax[8];   // screen rectangles of the parts of the window the tour points at
    VideoTex video;
    float volume = 1.f;
    bool muted = false, subsOn = true;
    int playReq = -1;
    int dabStation = -1;      // --station: DAB sub-channel to play on start (testing)
    std::vector<DeviceInfo> devices; // [0]=synthetic, [1]=file, then sound cards, then radios
    // radios on the network the user added (prefs "netRadios"; listed by refreshDevices) and the "Add network radio" window
    std::vector<NetRadio> netRadios;
    std::vector<HamlibRig> hamlibRigs;   // prefs "hamlibRigs"; serial-CAT rigs listed with the other radios
    bool hamlibAddOpen = false;
    int hamlibAddModel = 3073;            // the model picked last (prefs "hamlibAddModel"); 3073 = IC-7300 until the user picks another
    int hamlibAddBaud = 0;                // 0 = the model's default
    char hamlibModelQuery[64] = "";       // the search box of the model picker
    std::vector<RigModelInfo> hamlibModels;   // Hamlib's model list, loaded the first time the window opens
    std::vector<SerialPortInfo> hamlibPorts;  // serial ports found when the window opened or Refresh was pressed
    bool hamlibWasOpen = false;
    uint64_t hamlibRigSeq = 0;            // the last rig-side change applied to the frequency and mode boxes
    bool hamlibLinkWas = true;            // the CAT link state last logged
    char hamlibAddPath[256] = "";
    char hamlibAddLabel[128] = "";
    char hamlibAddAudio[128] = "";         // AudioIn id (empty = CAT only, silence IQ)
    struct NetProbe { std::atomic<bool> done{false}; bool ok = false; std::string err; NetRadio radio; };
    std::shared_ptr<NetProbe> netAddProbe;   // a connection test running on a worker thread
    bool netAddOpen = false;
    int netAddType = NetRadio::RtlTcp;
    char netAddHost[128] = "";
    int netAddPort = 1234;
    char netAddRemote[128] = "";   // SoapyRemote: the server radio to ask for ("mirisdr", "driver=sdrplay,serial=1")
    std::string netAddErr;
    // a search for Hermes Lite 2 radios (a discovery broadcast) running on a worker thread, and what the last one found
    struct NetDiscovery { std::atomic<bool> done{false}; std::vector<FoundNetRadio> found; };
    std::shared_ptr<NetDiscovery> netDiscovery;
    std::vector<FoundNetRadio> netFound;
    bool netSearched = false;
    int devIdx = 0;
    TuneSettings tune;
    FileOptions file;
    int audioIqMode = 0;          // AudioIqMode: I/Q stereo, USB audio, LSB audio, mono real (sound-card source)
    int bwIdx = 0;
    int constView = 0;        // data constellation: 0 cells, 1 density, 2 clusters
    struct ConstStats {       // statistics of the decoded cells, collected over the last few seconds
        static constexpr int G = 96;
        uint64_t seq = 0; int mod = -1, plp = -1;
        std::vector<float> grid;                         // G x G density, row 0 at the top
        struct Pt { double n = 0, ei = 0, eq = 0, e2 = 0; };
        std::vector<Pt> pts;                             // per transmitted point: count, mean error, summed squared error
    } cst;
    bool atscMode = false;    // 6 MHz channel settings: families 1 (ATSC 1.0), 3 (ATSC 3.0) and 4 (ISDB-T)
    bool atsc3Mode = false;   // ATSC 3.0 (family 3)
    bool isdbtMode = false;   // ISDB-T (family 4)
    bool dabMode = false;     // DAB / DAB+ (family 2)
    bool fmMode = false;      // FM radio (family 5)
    bool newUi = true;        // the new interface (ui2.cpp) instead of the classic one (View > Classic interface)
    struct FamGain { int lna = 32, vga = 20; bool amp = true, known = false; } famGain[kNumSlots];   // the radio gains remembered for each mode
    double famFreq[kNumSlots] = {};  // the frequency each of the modes added after FM was last tuned to (0 = never)
    std::string modeGroupsOpen;   // the mode list groups left unfolded, by name, separated by |
    int uiVariant = 0;        // layout of the new interface: 0 sidebar (the default), 1 scope, 2 tiles, 3 faceplate, 4-6 scope children, 7 panel
    float uiTextScale = 1.f;  // View > Text size: 0.8 .. 2.0, on top of the display scale (Ctrl +, Ctrl -, Ctrl 0)
    float splitSide = 0, splitRight = 0, splitBottom = 0, splitDock = 0, splitBot2 = 0;   // dragged panel sizes in unscaled points (0 = automatic): sidebar, instruments, lower row; scope layout: instruments, lower row
    int uiTheme = 1;          // its palette: 0 terminal, 1 instrument, 2 mono
    std::string uiLang;       // the interface language chosen in View > Language ("de", "lt" ...), empty: the system's
    int dtmbBwMhz = 8;        // DTMB channel width: 8 MHz (China, Hong Kong), 6 MHz (Cuba)
    int isdbtBwMhz = 6;       // ISDB-T channel width: 6 MHz (Japan, the Americas), 7, 8 MHz (the Maldives); 0 = automatic
    bool atvFm = false;       // analog TV: FM video as analog FPV links send it (20 Msps), not the amplitude modulated broadcast signal
    int atvNbtv = 0;          // analog TV: 0 television, 1 NBTV / mechanical TV on AM, 2 the same on narrow FM (the audio is the picture)
    int atvNbtvLines = 32;    // NBTV / mechanical: lines (24 .. 240) and pictures a second; 32 at 12.5 is the NBTVA standard
    float atvNbtvFps = 12.5f;
    bool wideDecode = false;  // "Whole span": decode every carrier in the visible span, not only the tuned one (only for modes that opt in: modeSupportsWide)
    uint64_t wideFamMask = 0; // the switch per mode family (bit = family); wideDecode is the current mode's bit
    int wideSel = 0;          // the carrier the sound and the detail follow (0: the best locked one)
    bool mtvOn = false;       // "Multi-channel": decode two or three adjacent TV channels of a wide capture (DVB-T2 / DVB-T)
    int mtvMax = 0;           // receivers at once (0: 2 on a laptop, 3 on a desktop)
    int mtvGrid = 0;          // 0 find out, 1 the capture is centred on a channel, 2 between two channels
    std::vector<MultiService> mtvSvc;   // every service of every channel (refreshed with ts)
    bool atvWide = false;     // analog TV: system E chosen (14 MHz channel, 10 MHz of video): the radio runs at 20 Msps
    bool reduceCpuHidden = true;   // View > Reduce CPU when hidden: no drawing while the window is minimised or hidden (decoding goes on)
    bool discordOn = false, discordFreq = true;   // View > Show in Discord (opt-in Rich Presence, discord_rpc.cpp) and its "Show the frequency"
    bool discordStation = true, discordRt = false;   // broadcast FM: the RDS name (or the saved station name), and the RDS radio text
    bool mapSend = false, mapConsent = false;     // Server > Send data to map server (opt-in; consent once)
    bool mapDeclined = false;                     // the start-up question was answered "No, do not ask again" (the Server menu still works)
    std::string mapStationId;                     // client id for /ingest, generated once
    std::string mapServer;                        // empty: https://map.on-air.dev (a local worker URL for developers)
    bool feedsOpen = false;   // View > Data feeds: the window (feeds_ui.cpp)
    dect2::FeedConfig feeds;  // the data feeds out (all off by default), saved with the preferences
    dect2::ShareConfig share; // "Share my radio": the rtl_tcp server (off by default; feeds_ui.cpp), saved with the preferences
    dect2::PskReporterConfig pskr;   // PSK Reporter spots (off by default; feeds_ui.cpp), saved with the preferences
    dect2::WsprNetConfig wsprNet;    // WSPRnet spots (off by default; feeds_ui.cpp), saved with the preferences
    bool highContrast = false; // View > Theme > High contrast: black, white and yellow, thick outlines, dim text lifted to 7:1 (ui2.cpp, main.cpp); wins over lightUi
    bool lightUi = false;     // View > Light: every colour drawn with its lightness turned over (dark on white), hues kept (main.cpp)
    int family = 68;           // 68 DVB, 1 ATSC, 2 DAB, 3 ATSC 3.68, 4 ISDB-T, 5 FM, 6 DVB-S/S2, 7 DTMB, 8 analog TV, 9 DMR, 168 DRM, 11 ADS-B, 12 GNSS, 13 radiosonde, 14 AIS, 15 marine, 16 ACARS, 17 Inmarsat-C, 18 Inmarsat Aero, 19 Iridium, 268 mesh, 21 HD Radio, 22 CDR, 23 pagers, 24 APRS / packet, 25 HF digital, 26 airband, 27 VDL Mode 2, 28 ISM sensors, 368 VOR / ILS, 31 M17, 33 D-STAR, 34 CW, 35 HFDL, 36 PSK31, 37 JS8Call, 42 spectrum analyser, 38 NXDN, 468 DVB-C, 45 dPMR, 44 Orbcomm, 43 Mode S uplink, 46 TETRA, 49 HF ALE, 47 MAC, 48 radio astronomy, 568 P25, 51 Bluetooth LE, 52 ionospheric sounders, 53 QO-16868, 54 Wi-Fi networks, 55 general receiver, 61 CubeSat telemetry, 64 HRPT / AHRPT (6 and up: see ModeUi; engine standard code = family + 2)
    int sub = 0;              // 1 .. kNumSubModes: the protocol entry (dect2::subModes() index + 1) of HF digital, mesh, pagers, marine or VOR / ILS; 0 none
    std::deque<float> dabSnrH, dabFicH, dabCfoH, dabCirH;
    std::deque<float> dabBufH, dabRsH, dabAuH;   // of the selected service: they restart when another one is selected
    int dabHistSub = -2;
    // announcements (EN 300 401 8.1.6): switch to the sub-channel that carries one while it lasts
    bool dabAnnOn = true; unsigned dabAnnMask = 0x3F;   // types 0..5: alarm, road traffic, transport, warning, news, weather
    int dabUserSub = -1;                                // the sub-channel the user chose (what is played when there is no announcement)
    bool dabAnnActive = false, dabAnnSuspended = false; // switched to an announcement; the user went back to the service until it ends
    int dabAnnSub = -1, dabAnnType = -1;
    double dabAnnLastSwitch = -1e9;                     // time of the last switch to or from an announcement: at most one every 5 s
    bool dabPrefsLoaded = false;
    int dabRecFmt = 0; bool dabRecLog = true;           // recording: 0 WAV, 1 as broadcast
    std::string dabRecErr;
    // following: when the signal is lost, the service is looked for on the other frequencies the FIC lists for its ensemble (FIG 0/21) and for the
    // other ensembles that carry it (FIG 0/24)
    bool dabFollow = false;
    std::vector<std::pair<double, uint16_t>> dabAlts;   // MHz, ensemble id
    double dabAltStamp = 0, dabLostSince = 0, dabFollowT0 = 0;
    int dabFollowIdx = 0;
    uint32_t dabFollowSid = 0;
    bool dabFollowing = false;
    uint64_t dabRsLast = 0, dabAuLast = 0;
    std::deque<float> fmSnrH, fmPilotH, fmRdsH;
    bool fmMono = false;      // FM: force mono (a weak station is quieter in mono)
    int fmDeemph = 50;        // FM de-emphasis in microseconds: 50 (Europe, Middle East, most of the world) or 75 (Americas, South Korea)
    int fmChanKhz = 0;        // FM channel filter: 0 = automatic (the receiver's standard 220 kHz), else a manual width of 100 to 300 kHz
    struct DabScan {
        bool running = false; int idx = -1; double t0 = 0, lockT = 0, savedFreq = 218.64;
        struct Res { std::string name, label, stations; double mhz = 0; bool found = false; float snr = 0; };
        std::vector<Res> results;
    } dabScan;
    struct FmScan {
        bool running = false; int phase = 0; int idx = -1; double t0 = 0, savedFreq = 100.0; uint64_t seq0 = 0, rxSeq0 = 0;   // seq0: spectrum frame, rxSeq0: receiver report at the moment of tuning; phase 0 surveys the band, phase 1 checks each candidate
        double scanLoMhz = 87.0, scanHiMhz = 108.0; int surveyHops = 11;   // scan range (MHz), fixed for the run from preferences
        std::vector<double> cand;
        std::vector<std::string> psSeq; double psT = 0;   // the names the station sent on this channel, in order, and when the last new one came
        // rt: radio text; psText: the names of a station whose name changes (a scrolling name), in order; pi: programme identification
        struct Res { std::string name, pty; double mhz = 0; bool found = false, stereo = false, rds = false; float snr = 0; std::string rt, psText; int pi = 0; bool tp = false; };
        std::vector<Res> results;
    } fmScan;
    std::vector<FmSavedStation> fmSaved;   // stations the user saved by hand, kept in the preferences between sessions
    FmSavedEdit fmSavedEdit;               // rename box, search and delete confirmation for this receiver only
    bool bwAuto = true;       // the engine measures the channel width and switches by itself
    double freqMhz = 522.0;
    bool autoScroll = true;
    int tab = 0;
    SpectrumFrame spec;
    std::vector<float> smooth, peak;
    uint64_t lastSeq = 0;
    Waterfall wf;
    float yMin = -110, yMax = -30;
    float specZoom = 0.f;   // 0..1 frequency zoom for the vertical slider (0 = full bandwidth, 1 = ~1 kHz span)
    int tuningMode = 0;                 // 0 retune the radio LO on spectrum click, 1 decode offset only (sticky LO)
    bool peakHold = true;               // the max-hold line
    float holdDecay = 4.5f;             // dB a second the max-hold line falls (0: it stays until it is reset)
    float specAvgSec = 0.08f;           // time constant of the averaged trace in seconds (0: the raw frames)
    bool specFill = false, specGrid = true, specPeaks = false, specReadout = true, specDbm = false;   // fill under the line, grid, peak markers, level at the cursor, dBm instead of dBFS
    double specLastT = 0;               // when the last spectrum frame came in
    ImFont* mono = nullptr;
    ImFont* ui = nullptr;
    std::string hackrfErr;
    bool hackrfErrHidden = false;       // the user dismissed the listing error; a new listing shows it again
    std::string startErr, startErrKey;  // last failed start/retune of the radio (from the engine log); key = the log line it came from, so a dismissed one stays away
    std::vector<std::string> usbHints;  // why a plugged-in radio is not listed (Linux), refreshed with the radio list
    double diagCopiedAt = -10, diagPollAt = -10;
    bool diagWasRunning = false;
    bool recSigmf = true;               // write SigMF metadata beside IQ recordings (pref "recSigmf", default on)
    int recFormat = 0;                  // IQ recording format: 0 = 8-bit, 1 = float (pref "recFormat")
    int tvBufMb = 256;                  // TV live buffer for Pause (pref "tvBufMb")
    int tvRecMin = 0, tvRecAtH = 0, tvRecAtM = 0;   // TV recording timer: stop after N minutes (0 = off), at a clock time
    bool tvRecAtOn = false, tvRecWasActive = false;
    std::string tvRecDonePath, tvRecErr;   // the last finished TV recording; why one could not start
    bool recWasActive = false;          // a recording was running at the last frame (to log it once when it ends)
    std::string recDonePath;            // the last finished recording, for "Show recording"
    RxTelemetry rx;
    uint64_t rxSeq = 0;
    // Second mode (Engine::setSecondary): a second receiver on the same capture. Its screens are the mode's own, drawn with a.rx swapped
    // for its report (secondScope, modeui.cpp); its sound stays off unless asked for.
    struct SecondMode {
        int modeIdx = -1;     // the kModes row chosen, -1 = none
        double mhz = 0;       // its frequency
        bool audio = false;   // play its sound (off: muted whatever the first mode does)
        RxTelemetry rx;
        uint64_t seq = 0;
        bool drawing = false; // its screens are being drawn: tuneFreq() tunes it instead of the radio
    } second;
    TsSnapshot ts;
    BbStats bb;
    double tsT = 0;
    OutputConfig out;
    int selService = -1;     // service id used for outputs (-1 = whole multiplex)
    char udpHost[64] = "127.0.0.1";
    char filePath[512] = "";
    std::deque<float> hCfo, hSnr, hTiming;
    struct HistSample { float t, snr, mer, loss, cfo, sro, level, clip, quality, aux; };   // aux: FM, the share of RDS blocks received intact
    std::deque<HistSample> hist;      // 4 samples per second, last 15 minutes
    double histT = 0;
    uint64_t histOk = 0, histBad = 0;
    bool rxSeen = false;
    int computeMode = 2; // 0 CPU, 1 GPU, 2 auto
    int stdMode = 0;     // 0 auto, 1 DVB-T2, 2 DVB-T
    bool popOut = false, videoOnly = false;
    bool popTop = true;      // the pop-out video window stays above other windows
    int histWindow = 60;
    int ttxPage = 100;
    int ttxSub = -1;                // teletext subpage shown in the tab: -1 = the newest
    int ttxEntry = 0, ttxEntryVal = 0;   // digits typed so far for a page number
    bool ttxHold = false, ttxReveal = false;
    TtxPage ttxHeld;                // the page frozen by "hold"
    std::vector<SubOpt> subOpts;    // subtitle services of the playing programme (DVB subtitles, teletext subtitle pages)
    int subIdx = -1;                // the one chosen
    std::string subsPick;           // its key, remembered between runs
    int plpSel = -1; // -1 = automatic
    int subTv = 0, subRx = 0, subStream = 0; // sub-view of the TV / Receiver / Stream tabs
    std::map<int, std::vector<EpgEvent>> epg;
    double epgT = -10;
    int guideSid = -1, guideEvent = -1, guidePlaySid = -1; // guide follows playing service until user picks another in the list
    bool fakeEpg = false; // --fakeepg: made-up programmes, for checking the layout when the mux sends none
    char favName[64] = "";
    MultipathDetector mpd;
    QualityMeter quality;
    std::vector<SavedChannel> channels; // DVB-T2 muxes found by the scanner (remembered)
    bool scanWas = false;
    double scanHarvestT = 0;
    bool agcOn = false;
    std::string gainDev;   // the radio a.tune.gainDb was set for (board:serial); another radio starts at its own sensible gain
    double autoRateHz = 0;      // the mode's own radio rate ("Auto" of the sample-rate control), set by applyBandwidth()
    double chosenRateHz = 0;    // the rate chosen for this mode on this radio and applied by applyBandwidth() (0 = Auto)
    std::string biasCtx;        // the mode / radio the antenna power was last loaded for (biasContext)
    std::string rateCtx;        // mode / radio / channel width applyBandwidth() last ran for (the control refreshes Auto when it changes)
    std::string radioKey;  // the radio a.tune.radio (its own settings, DeviceInfo::settings) was loaded for (radioKeyOf); "" = none
    AutoGain agc;
    GainSweep sweep;
    GainOptSession gopt;   // "Optimise gain" (gain_opt_ui.cpp): a sweep of the radio's gain range for the best quality that does not overload
    bool sweepRetune = false;
    Scanner scanner;
    ScanConfig scanCfg;
    int scanPreset = 0;
    bool scanWasRunning = false;
    std::unique_ptr<Updater> upd;      // update check and installer
    bool updCheck = true, updAuto = true, updPre = false, updStarted = false;   // official updates on; betas off until asked
    double updLast = 0;                // when the last check was made (seconds since 1970)
    std::string updSkip;               // a version the user chose to skip
    DirectionFinder dir;
    int antKind = 0;          // 0 directional, 1 dipole / indoor, 2 omnidirectional
    bool dirAgcWas = false;
};






// ------------------------------------------------------------------ UI pieces


// ------------------------------------------------------------------ small widgets (pills, tags, gauges)





// label : value readout in the status area (label dim, value in the mono font)











































// ------------------------------------------------------------------ the six top-level views










// ------------------------------------------------------------------ antenna direction finder












// ------------------------------------------------------------------ main

// ------------------------------------------------------------------ the panels and helpers (defined in the .cpp files)
// widgets.cpp
bool tabItem(const char* name, Ic icon);
// the interface language: menu items (View > Language) or a small button with a list (the classic interface); both save the choice
void languageMenuItems(App& a);
void languageButton(App& a);
void setUiLanguage(App& a, const std::string& code);   // empty: the system's language
extern bool gTightTabs;   // the tabs of mainTabs() get less padding (a narrow pane)
void toggleFullscreen();
void handleFullscreenHotkeys();          // F11 and Alt+Enter; ignored while a text field has the keyboard
bool adsbFullMap(App& a, ImVec2 disp);           // adsb_ui.cpp: the ADS-B map or radar over the whole window when it is popped out
void receiverLocation(double& lat, double& lon);  // adsb_ui.cpp: the one receiver position (the ADS-B antenna position)
bool receiverLocationSet();                        // the user has set it (else it is a placeholder)
void receiverPositionFields(App& a);               // its fields (latitude, longitude, altitude) and the Locate button
double receiverAltitudeM();                         // its height above sea level, metres (0 until set)
void setReceiverLocation(App& a, double lat, double lon);   // move it (saved), e.g. View > Map's "Use as my location"
void syncAdsbReference(App& a);                              // push antenna position into the ADS-B tracker while that mode is running
struct MapSatRef { std::string name; int norad = 0; const dect2::Sgp4* sgp = nullptr; };
void passesMapSatellites(std::vector<MapSatRef>& out);   // passes_ui.cpp: the listed satellites with a TLE (for the live map)
struct MapCellTower { double lat = 0, lon = 0; int rangeM = 0; std::string label; unsigned colour = 0; };
void cellMapTowers(const dect2::CellTelemetry& t, std::vector<MapCellTower>& out);   // cell_ui.cpp: the towers placed (OpenCelliD / CSV)
void mapWindow(App& a);                             // map_ui.cpp: View > Map (a.mapOpen): coordinates, Maidenhead locators, distance and bearing
void mapSendServerMenu(App& a);                     // map_send.cpp: Server menu (opt-in public map)
void mapSendButton(App& a);                         // classic interface: the same choice as a button
void mapSendTick(App& a);                           // POST current decoded positions when opted in
bool mapPauseForTransmit(App& a);                   // turn the map connection off before the radio is keyed; true if it was on
void mapTransmitAbort(App& a);                      // the radio did not start: put the connection back if it was on
void mapTransmitEnded();                            // the radio is quiet: the Server control works again; the next send skips what is already on screen
void aeroToolsTick(App& a);                         // aero_ui.cpp: SNR-sound tone while Inmarsat Aero is the mode
void mapSendShutdown();                             // join the ingest thread before exit
bool mapFamilySends(int family);                    // this mode can put decoded positions on the public map
std::string mapSendStatusLine();                    // short "connected / N sent" line for the mode header (empty when off)
std::string mapSendDiagnosticsBlock(const App& a);  // map.on-air.dev ingest state + recent log for Copy diagnostics
void passesDevTle(const std::string& path);       // dev (--tle): TLEs from a file, as if pasted
std::vector<std::pair<std::string, double>> passesGlobalstarDoppler(double unixSec, double freqHz);   // passes_ui.cpp: Globalstar satellites above the horizon and their Doppler (TLEs from the planner)
void passesWindow(App& a);                         // passes_ui.cpp: the satellite pass planner window (a.passesOpen) and its tracking, every frame
void scannerWindow(App& a);                        // scanner_ui.cpp: the scanner window (a.scannerOpen) and its scan loop, every frame (HIGHLY EXPERIMENTAL)
void scannerButton(App& a);                        // a button that opens it (the classic interface's Scan tab)
GLFWmonitor* monitorFor(int x, int y, int w, int h);   // widgets.cpp: the monitor holding most of the rectangle
// The monitors, kept from the last change GLFW reported (glfwSetMonitorCallback) instead of asked for on each use: a monitor that goes away
// (display asleep, cable out) is dropped from the list before anything asks for its size or position again.
void watchMonitors();                                  // after ImGui_ImplGlfw_Init (it installs a monitor callback of its own; this one chains it)
const std::vector<GLFWmonitor*>& monitors();
GLFWmonitor* primaryMonitor();                         // nullptr when there is none (all asleep)
extern bool gWayland;   // main.cpp: GLFW runs on Wayland (window positions cannot be read or set there)
// popout.cpp: the video pop-out as a window of its own
extern bool gNoOsWindows;            // --hidden: no extra OS windows (the pop-out stays a panel inside the main window)
void popOutFrame(App& a);            // inside the ImGui frame: opens/closes the window and builds its picture
void popOutPresent();                // after the main frame (or instead of it while the main window is minimised): draws the window
bool popOutShown();                  // the pop-out window is open and not minimised
void popOutShutdown();               // before the graphics back end shuts down
bool pillButton(const char* label, bool selected, float padX = 11);
int subNav(const char* id, int& cur, std::initializer_list<const char*> names);
float tagAt(ImDrawList* dl, ImVec2 pos, const char* text, ImU32 bg, ImU32 fg = IM_COL32(225, 232, 240, 255));
void gaugePill(float width, float frac, ImU32 fill, const char* text);
void lamp(const char* label, int state /*0 grey 1 green 2 amber 3 red*/, int icon = -1);
// Lost samples (Engine::sampleLoss()) for the "dropped" read-outs: "0", or where they went (callback gaps, ring overflow) and, when the
// radio is under about 97% of its set rate, that rate. Red while that is happening or a loss is recent, amber after; the tooltip says
// which problem it is (the radio's deliveries, the input ring, or receiver load).
std::string lossText(const SampleLoss& l);
ImVec4 lossColour(const SampleLoss& l, ImVec4 normal = ImVec4(0.93f, 0.95f, 0.97f, 1));
void lossTooltip(const SampleLoss& l);
// The "?" button next to the sound of a mode whose voice needs a codec OnAir does not ship (codec_help.cpp, docs/CODECS.md): found or not,
// what it is, how to install it here, where to put it, a link to the full page and "Look again"
enum class CodecHelp { Codec2, DmrAmbe, Hdc, Dra, AeroAmbe, IridiumAmbe, DstarAmbe, NxdnAmbe, DpmrAmbe, P25Imbe, Avs, DraTv, Ac4 };
void codecHelpButton(CodecHelp c);
// A new file path in the "audio-streams" folder of the user data folder, without extension: "<stem>_<date>_<time>" (the stem cleaned for a file name)
std::string streamSavePath(const std::string& stem);
// "voice: needs mbelib (libmbe.dll, ...)": the exact file names tried, for the status line of the mbelib modes
const char* codecNeedMbe();
// "Listen" for a decoder mode that is silent by default (HF digital, PSK, JS8Call): the switch, remembered under prefKey, then the shared
// mute and volume, in a flow row (widgets.cpp). True when the switch changed.
bool listenControls(App& a, bool& on, const char* prefKey, const char* id, const char* tip);
int hfDemodLoad(const char* prefKey);                                         // the remembered HfDemod choice (default Auto)
bool hfDemodControl(App& a, int& choice, const char* prefKey, const char* id);   // the ham front end's demodulator combo; saves on change
// rows that wrap in a narrow window: flowNext() between the groups of a row (a label and its control, a lamp, a read-out) instead of
// ImGui::SameLine(); the next group stays on the line if it fits (as wide as in the last frame). flowBreak() ends the line, like NewLine().
bool flowNext(float spacing = -1);   // true: on the same line
void flowBreak();
void flowEnd();   // after the last group of a row that ends without flowNext(): its width is measured too
bool sameLineIf(float w, float spacing = -1);   // SameLine() when something w wide still fits after the last item; true if it did
std::string ellipsize(std::string s, float w, float size = 0);   // s cut short with "..." to fit w (at this font size, default the current one)
std::string fitCaption(const std::string& s, float w);           // a caption that fits w: without its "(...)" part, else cut short
void captionFit(float w, const char* fmt, ...);
void kvColumn(float col);   // after a key: SameLine() at the value column col, further left in a narrow pane (never over the key); wrap the value                  // TextDisabled() of such a caption, the whole of it on hover (above a plot w wide)
struct StatusPanel { ImVec2 p; ImGuiID id; StatusPanel(); ~StatusPanel(); };   // the tinted panel behind a status bar, as tall as its (wrapped) lines
void scatter(const char* id, const std::vector<cf32>& pts, ImVec2 size, double lim, ImVec4 col);
void historyPlot(const char* id, const char* ylabel, const std::deque<float>& h, ImVec2 size);
std::string fmtLocal(int64_t utc, const char* f);
const char* genreName(int g);
const char* fmtKbps(char* b, size_t n, double k);
void qualityBar(App& a, float width);
// app_state.cpp
void setFamily(App& a, int f);                 // a family with protocol entries opens on its first one
void setMode(App& a, int f, int sub);          // a mode-list entry: family and protocol entry (0 none, or the family's first)
const dect2::SubMode* curSub(const App& a);    // the protocol entry selected, nullptr for a mode without them
int subIndexOf(const char* id);                // 1 .. kNumSubModes, 0 when not one
int modeSlot(const App& a);                    // index of famFreq / famGain for the current entry
int engineStd(const App& a);
void refreshDevices(App& a);
void netRadioWindow(App& a);   // the "Add network radio" window (toolbar.cpp)
void hamlibRigWindow(App& a);  // the "Add Hamlib rig" window (toolbar.cpp)
void applyBandwidth(App& a);
// The sample rate chosen per mode and per radio (prefs "rate.<mode id>.<radioKeyOf>", in Hz; absent or 0 = Auto, the mode's own rate)
std::string rateModeId(const App& a);
std::string rateContext(const App& a);
double savedSampleRate(const App& a);
void saveSampleRate(App& a, double hz);
double modeMinSampleRate(const App& a);
void sampleRateControl(App& a);
std::string biasContext(const App& a);   // the prefs key of the antenna power for this mode on this radio ("" = none)
void loadBiasTee(App& a);
void saveBiasTee(const App& a);   // toolbar.cpp: the "Sample rate" control under the frequency
// The radio's own settings (DeviceInfo::settings, TuneSettings::radio), saved per radio: the key a radio is saved under, loading the
// selected radio's values into a.tune.radio when another radio was chosen, saving them after a change
std::string radioKeyOf(const DeviceInfo& d);
void syncRadioSettings(App& a);
void saveRadioSettings(const App& a);
std::string openFileDialog();
std::string saveFileDialog(const char* name);
void loadPrefs(App& a);
void loadFeedPrefs(App& a);         // feeds_ui.cpp: the data feed settings, and starts the ones that are on
void saveFeedPrefs(const App& a);
void gainOptButtons(App& a, bool running);   // gain_opt_ui.cpp: "Optimise gain" / "Cancel" and "Undo gain" on the gain row
void gainOptTick(App& a);                      // every frame (gainControl): runs the sweep
bool gainOptStatus(App& a);                    // the progress / result line; true when it drew something
void remoteApiLoadPrefs(App& a);    // remote_api_app.cpp: the remote-control API (HTTP + WebSocket), off by default: its settings, started when on
void remoteApiSavePrefs(const App& a);
void remoteApiTick(App& a);          // every frame: runs the commands the API queued for the main thread, publishes the telemetry
void remoteApiSection(App& a);       // the settings panel (in the Data feeds window)
void shareTick(App& a);             // feeds_ui.cpp: carries out the frequency / gain commands of the clients of "Share my radio" (when allowed)
void feedsWindow(App& a);          // feeds_ui.cpp: the "Data feeds" window (View > Data feeds)
void rotatorWindow(App& a);        // rotator_ui.cpp: GS-232 rotator connect / status
void rotatorTick(App& a);          // rotator_ui.cpp: signal-peak azimuth calibration sweep
void rotatorLoadPrefs();           // rotator_ui.cpp: prefs and optional auto-connect
void rotatorSavePrefs();
void rotatorPointBearing(double bearingDeg, double elDeg);
// The transmitter (tx_ui.cpp): independent of the receivers, with its own radio. The state is not an App's; transmitting is never saved.
struct ModeDef;
bool& txViewFlag();                // the RX / TX switch: true = TX view (never saved: the program opens in RX view)
void txViewSwitch(App& a);         // the slider at the top left of both interfaces
void txPanel(App& a);              // leftover: the compact TX bar + notice (classic/new shells draw spectrum and the right list themselves)
void txBar(App& a);                // frequency, radio, gain, Start / Stop / Carrier tune
void txSpectrum(App& a);           // live spectrum of the IQ going to the radio
void txRight(App& a);              // this mode's payload (or why it cannot transmit)
void txSelectMode(App& a, int family, const char* subId);
bool txModeHighlighted(const ModeDef& m);
void txTick(App& a);               // every frame: gives the radio back when a transmission ended; the on-screen TX indicator
void txShutdown();                 // stops a running transmission (the radio goes quiet before it returns); the program's end
bool txActive();
void savePrefs(const App& a);
int adcBitsFor(const DeviceInfo& d, double rateHz);
void ingestSpectrum(App& a);
void ingestRx(App& a);
std::vector<double> xs(const App& a);
float dbmOffset(const App& a);                 // dB to add to a level in dBFS to get dBm, set for the selected radio (0 until it is)
void setDbmOffset(const App& a, float db);
void wfAutoButton(App& a);                     // plots.cpp: "Auto", on while the waterfall range follows the noise floor
void wfDisplayPopup(App& a);                   // plots.cpp: colour map, intensity range, speed and pause of the waterfall
void specDisplayPopup(App& a);                 // plots.cpp: the spectrum's averaging, max hold, markers, grid, fill and unit
float& specWaterfallSplit();                   // plots.cpp: spectrum share of the combined plot (default 0.5)
void resetSpectrum(App& a);                    // plots.cpp: default dB ranges, waterfall auto, 50/50 split (#142)
// Whole-span decoding (modeui.cpp). A mode opts in with registerWideMode(family); the switch is drawn by the spectrum (plots.cpp) for those only.
bool modeSupportsWide(int family);
void registerWideMode(int family);
bool wideActive(const App& a);                          // the switch is on for this mode and the engine runs it
void wideSync(App& a);                                  // once a frame: pushes the switch, the selected carrier and the mute to the engine
void wideCarrierCell(const App& a, int carrierId, double relHz);   // a table cell with the carrier's frequency (MHz); "-" for a single channel
void wideCarriersTable(App& a, float h);                // the carriers table: click selects (sound, detail), double click tunes
double radioCenterMhz(const App& a);   // where the radio is tuned: the frequency plus the mode's tuneOffsetHz (a file source: the frequency as typed)
void applyOutputs(App& a);
std::string channelLabel(const SavedChannel& c);
void harvestScan(App& a);
// The result lists the screens keep themselves (results_ui.cpp; the receivers' own are Engine::setResultsDir): load at start, save every
// 30 s (now: at once, at exit), each into <data folder>/results/app-<name>.bin
void appResultsLoad(App& a);
void appResultsSave(App& a, bool now = false);
// The Clear of a mode's decoded lists, at the top of its list (results_ui.cpp): true when pressed
bool resultsClearButton(const char* id);
void scanDbTick(App& a);   // scan_db_ui.cpp: asks to share a finished scan
void tuneToChannel(App& a, const SavedChannel& c);
void followBandwidth(App& a);
void setDvbBandwidth(App& a, int idx);   // toolbar.cpp: the DVB-T2 / DVB-T width kBw[idx] (manual), or idx < 0 for Automatic; a running receiver follows at once
void setIsdbtBandwidth(App& a, int mhz);  // isdbt_ui.cpp: 6, 7, 8 or 0 (automatic); a running receiver starts again with it
const char* isdbtWidthLabel(int mhz);      // isdbt_ui.cpp: "6 MHz", "8 MHz (Maldives)", "Automatic"
void setDtmbBandwidth(App& a, int mhz);  // dtmb_ui.cpp: 8 or 6 MHz; a running receiver starts again with it
// toolbar.cpp
void toolbar(App& a);
void sourceOptions(App& a);
bool sourceOptionsOpen(const App& a);   // synthetic, a file not playing, or a sound card
void statusBar(App& a);
void gainControl(App& a);
void standardSwitch(App& a);
extern float gSwitchWidth;
// The receiver modes (toolbar.cpp). Adding a mode is one row there.
// subId: one protocol of a shared receiver (dect2::subModes()), a mode of its own with its own settings, decoder and views
struct ModeDef { int family; const char* name; int group; ImU32 col; const char* blurb; const char* tip; const char* sub; ImVec4 accent; const char* subId = nullptr; const char* keys = nullptr; };
extern const ModeDef kModes[];
extern const int kNumModes;
constexpr int kNumGroups = 14;   // the headings of the mode lists (toolbar.cpp); UI only, never saved by number
extern const char* const kGroupNames[kNumGroups];
extern const ImU32 kGroupCols[kNumGroups];
// The screens of a mode added after FM (family 6 and up). Each mode has one in its app/<mode>_ui.cpp and modeui.cpp lists them; the shell calls whichever
// entry is set where it would draw the DVB version. A null entry means: nothing of this kind for the mode (or the default noted).
struct ModeMeter { const char* label; const char* fmt; double v, lo, hi; int level; };   // one bar of the meter bank; level 0 neutral, 1 good, 2 marginal, 3 bad
struct ModeUi {
    const char* sideTitle = "SERVICES";                 // title of the list on the right: SERVICES, STATIONS, AIRCRAFT, CALLS ...
    const char* tabName = nullptr;                      // the mode's own main tab, drawn instead of the TV tab (null: the TV tab, for the TV modes)
    Ic tabIcon = Ic::Tv;
    void (*tab)(App&) = nullptr;
    void (*receiver)(App&) = nullptr;                   // the Receiver tab: what the receiver is doing (null: no tab)
    bool stream = false;                                // show the Stream tab (transport stream, outputs): the modes that make a transport stream
    void (*list)(App&) = nullptr;                       // the list on the right (null: the service cards of the TV modes)
    void (*panels)(App&) = nullptr;                     // the analysis row at the bottom, where the TV modes have the constellations
    void (*status)(App&) = nullptr;                     // the status bar
    void (*summary)(const App&, std::string& line1, std::string& line2) = nullptr;   // the two lines in the top bar
    void (*tuner)(App&, bool& retune) = nullptr;        // the mode's own tuning controls (symbol rate, standard, ...); set retune to apply
    void (*decoder)(App&, bool& retune) = nullptr;      // its decoder options
    void (*synth)(App&, bool& changed) = nullptr;       // options of the built-in test signal (SynthConfig::mode, modeOpt, modeVal); set changed to restart it
    void (*scan)(App&) = nullptr;                       // the Scan tab (null: a note)
    void (*tick)(App&) = nullptr;                       // every frame while the mode is selected: push volume and mute to the receiver, run scans, collect histories
    void (*meters)(const App&, std::vector<ModeMeter>& out) = nullptr;   // the meter bank of the Scope: Meters layout
};
bool modeSupportsWideHf(int family);                    // modeui.cpp: the HF families that decode the visible span inside the receiver (CW, PSK, HF digital)
bool modeSupportsWide(int family);                      // modeui.cpp: the families that can decode the whole visible span (CW, PSK, HF digital)
const ModeUi* modeUi(int family);                       // modeui.cpp: nullptr for the original families (0 to 5)
const char* listTitle(const App& a, bool upper);        // "STATIONS", "SERVICES" or the mode's own, for the list on the right
// Second mode (modeui.cpp): run kModes[modeIdx] at mhz beside the first mode (-1: off); false and the engine's note when refused
bool setSecondMode(App& a, int modeIdx, double mhz);
void secondModeTick(App& a);                 // every frame: its report, its tick (sound pushed muted unless asked for)
void secondModeTabs(App& a);                 // in mainTabs(): the tab "Second: <mode>" with the mode's own main screen
void secondModeControl(App& a, bool vertical);   // the tuner panel: the button and its pop-up (pick a mode and its frequency)
void selectMode(App& a, int family, const char* subId = nullptr);
bool modeSelected(const App& a, const ModeDef& m);    // the row of the current mode
const char* modeSearchBox();                          // a search box at the top of a mode drop-down (toolbar.cpp); returns the query
bool modeMatches(const ModeDef& m, const char* query); // the search box: name, subtitle, description and keywords
void startReceiver(App& a);     // what the Start button does
enum TbPart { TbSource = 1, TbFreq = 2, TbTuner = 4, TbGain = 8, TbDecoder = 16, TbAll = 31 };
void toolbarParts(App& a, int mask, bool vertical);   // pieces of the top bar, for the sidebar of the new interface
void receiverGlance(App& a, float w, float h);       // analysis_tabs.cpp: a few key/value lines about the receiver
void tuneFreq(App& a, double mhz);                     // set the frequency now (resets the FM receiver)
void tuneDecode(App& a, double absMhz);                // sticky LO: decode absMhz without moving the radio centre
void spectrumTune(App& a, double absMhz);              // spectrum / waterfall click (respects tuningMode)
void uplinkNoteAdsb(const dect2::AdsbTelemetry& t);   // uplink_ui.cpp: callsigns heard by the ADS-B mode in this session
double meshSampleRate(App& a);                  // mesh_ui.cpp: the radio rate the selected LoRa bandwidth needs (2 Msps; 4 above 500 kHz)
void dvbsStatusRows(App& a, const DvbsTelemetry& t, bool on, bool run);   // dvbs_ui.cpp: the lock lamps and readouts of a report (QO-100's decoder panel)
void dvbsSetSymbolRate(App& a, double msym);   // dvbs_ui.cpp: the DVB-S/S2 symbol rate, set from elsewhere (the spectrum explorer)
// wb_ui.cpp: the Signal workbench. A file dropped on the window opens in it (the mode is selected); the dev flags open and drive it for --shot runs
void wbQueueDrop(const char* path);
void wbDropTick(App& a);
void wbDevOpen(const std::string& path, double rate, int tab);
void wbDevSelect(double t0Sec, double t1Sec, double f0Hz, double f1Hz);
void wbDevDemod(int kind, double baud, bool runMeasure, bool runDemod);
void mainTabs(App& a);          // the tab bar with the tabs that fit the current mode (main.cpp)
void drawShell2(App& a, ImVec2 disp);   // ui2.cpp: the new interface
void applyUiTheme(App& a);              // ui2.cpp: classic or new colours and shapes
// plots.cpp
// The width of the channel the mode's receiver works with: the light band on the spectrum, its edges on the waterfall. DVB-T2 / DVB-T: the
// measured or chosen raster width; FM: the channel filter; DTMB: 8 or 6 MHz; DVB-S/S2: symbol rate x (1 + roll-off) once it is known; the
// others: the width their standard (and receiver) fixes. settable: the user can choose it (channelWidthPopup), hasAuto: and leave it automatic.
struct ChanWidth { double mhz = 0; bool settable = false, hasAuto = false, manual = false; };
ChanWidth channelWidth(const App& a);
std::string widthText(double mhz);   // "7 MHz", "1.536 MHz", "12.5 kHz"
void channelWidthPopup(App& a);      // automatic or manual, where the mode allows it; otherwise what fixes the width
void spectrumWaterfall(App& a, float& frac, bool drag);   // the spectrum on top of the waterfall, one panel
void specFreqViewUpdate(const App& a);                                     // plots.cpp: keep linked frequency pan/zoom in sync with the radio
void specFreqViewGet(double& loMhz, double& hiMhz);
void specFreqViewWheel(App& a, float mouseNorm, float wheel, double& loMhz, double& hiMhz);
bool specDbRangeKeys(App& a);                                              // Ctrl+arrows dB range (3D view has no spectrum plot hover)
float specPlotBwBtnW();                                                    // plots.cpp: layout of corner controls (same frame)
float specPlotWideBtnW();
void channelWidthButton(App& a, ImVec2 plotMin, ImVec2 plotSize, const char* yLabel);
void wideSwitch(App& a, ImVec2 plotMin, ImVec2 plotSize);
void multiSwitch(App& a, ImVec2 plotMin, ImVec2 plotSize);
float specCornerPopup(App& a, const char* label, const char* popId, ImVec2 plotMin, ImVec2 plotSize, float rightUsed, float reserveLeft, float dropY, bool on, const char* tip, void (*body)(App&));
float bandPlanStripHeight();         // plots.cpp: one text line plus padding; follows the font and resize
void drawBandPlanStrip(ImDrawList* dl, float x0, float x1, float y, float h, double loMhz, double hiMhz);   // saAllocations in view
void specxDrawBookmarks(ImDrawList* dl, double loHz, double hiHz, float x0, float x1, float y0, float y1);   // specx_ui.cpp: named point / range marks
// specx_ident.cpp: "Identify this signal" (dect2/sig_ident.h). The region is [offHz -+ widthHz / 2] from the samples' centre, which the radio
// has at sampleCentreRadioHz (rfOfCentreHz: its RF, for the bands); identifyAt: the signal under a click on the spectrum (MHz)
void identifyStart(App& a, double sampleCentreRadioHz, double offHz, double widthHz, double rfOfCentreHz);
void identifyAt(App& a, double clickMhz);
void identifyWindow(App& a);   // every frame: the window while it is open
// general_ui.cpp: the general receiver's channel marker on the spectrum (plotId 0) and the waterfall (1): a click puts the channel
// there (inside the band, without retuning the radio), its filter edges can be dragged
void generalPlotMouse(App& a, int plotId);
void generalChannelBand(const App& a, bool fill);
bool generalDragging(int plotId);
double generalChannelWidthMhz(const App& a);
void histogramPlot(App& a, ImVec2 size);
void constDensityPlot(App& a, ImVec2 sz);
void constClusterPlot(App& a, ImVec2 sz);
// analysis_tabs.cpp
void syncTab(App& a);
void historyTab(App& a);
void frameMapTab(App& a);
void signallingTab(App& a);
void atscPanels(App& a);
void atsc3Status(App& a);
bool atsc3Quality(App& a, QualityReport& q);   // the signal-quality bar for ATSC 3.0 (share of decoded blocks); false when there is nothing to show
void atsc3ReceiverTab(App& a);
void isdbtStatus(App& a);
void isdbtReceiverTab(App& a);
void updateTick(App& a);
void updateButton(App& a);
void supportButton();
void clearAllButton(App& a);   // top bar, next to the version: empties every decoded list, after an "are you sure"
bool updateOnExit(App& a);
void constellationsTab(App& a);
void channelTab(App& a);
void impulseTab(App& a);
void snrTab(App& a);
void fecTab(App& a);
void tsTab(App& a);
void logPanel(App& a);
void historyLogTab(App& a);
// diag_ui.cpp: radio problems shown under the radio picker, and the diagnostics text
struct RadioMsg { int kind; std::string text; };   // kind: 0 radio listing error, 1 failed start/retune, 2 frequency outside the radio's range, 3 USB hint
std::string radioRangeWarning(const DeviceInfo& d, double freqMhz, const std::map<std::string, std::string>* radio = nullptr);   // empty when the frequency is fine or the range is unknown
namespace fixit { struct Entry; }
void fixitUi(App& a, const fixit::Entry* e, const char* idKey);   // diag_ui.cpp: the "How to fix" button + popup of a fixit.h entry (nothing for nullptr)
std::vector<RadioMsg> radioMessages(const App& a);                    // what is shown right now (dismissed ones left out)
struct DiagInput {
    std::string version, os, cpu, gpu;
    std::vector<DeviceInfo> radios;
    std::vector<std::string> messages, log;   // log: the whole engine log, filtered inside
    std::string mapServer;                    // map ingest connection log (map_send.cpp)
};
std::string buildDiagnostics(const DiagInput& in);   // plain text, no ImGui
std::string diagnosticsText(App& a);
void radioMessagesUi(App& a, bool vertical);
// radio_ui.cpp: the "Radio settings" of the selected radio (only the ones it has): a collapsible section in a side panel (vertical), a
// button with a popup in a one-line bar. A change reaches a running radio at once (a retune, or a restart for one that takes effect at open)
void radioSettingsUi(App& a, bool vertical);
// subs_ui.cpp: DVB subtitles and teletext subtitle pages over the picture, the language choice, the teletext page drawing
void subsUpdate(App& a);                  // every frame: the subtitle services of the playing programme, decoder PIDs, the overlay bitmap
void subsControls(App& a);                // on/off checkbox and language choice (the TV panel)
void subsKey(App& a, int what);           // 1 = on/off, 2 = next language (the pop-out window's keys)
void drawTtxGrid(ImDrawList* dl, ImFont* f, ImVec2 o, float cw, float ch, const TtxGrid& g, bool overlay, bool reveal);
// tv.cpp
void teletextTab(App& a);
const EpgEvent* epgCurrent(const App& a, int sid, const EpgEvent** next = nullptr);
void guideTab(App& a);
void playerTab(App& a);
void rightPanel(App& a);
void tvTab(App& a);
void channelDashboard(App& a);
// scan_outputs.cpp
void scanTab(App& a);
void outputsTab(App& a);
// antenna.cpp
void feedDirection(App& a);
void compassRose(App& a, ImVec2 size);
void antennaTab(App& a);
// main.cpp
ImU32 ttxColour(int c, float alpha = 1.f);
int64_t utcNowOf(const App& a);
void overviewTab(App& a);
void receiverTab(App& a);
void streamTab(App& a);
void routeTab(App& a, const std::string& name);
ImU32 scoreColour(double sc);
void drawUI(App& a, ImVec2 disp, bool draw = true);   // draw = false: only the work that must go on for a receiver whose tab is not shown
// dab_ui.cpp
const char* dabChannelName(double mhz);
bool dabChannelCombo(App& a);
void dabStatus(App& a);
void dabHistory(App& a);
void dabPanels(App& a);
std::string dabSubText(const DabEnsemble& e, int sub);
void dabSelectStation(App& a, int sub, bool play);
void dabStations(App& a);
void dabRadioTab(App& a);
void dabEnsembleTab(App& a);
void dabTuneMhz(App& a, double mhz);   // dab_ui.cpp: tunes the running receiver to a DAB frequency
bool dabLogoImage(App& a, const DabEnsemble& ens, uint32_t sid, float h);   // dab_ui.cpp: the station logo (SPI / slide show) of a service
void dabSlidePanel(App& a, const DabEnsemble& ens, float maxH);   // dab_ui.cpp: the latest slide show picture with its name
void dabAnalyzerTab(App& a);       // dab_analyzer_ui.cpp: the ensemble as a tree with details and the MSC allocation
void dabJournalineTab(App& a);     // dab_ui.cpp: Journaline pages (ETSI TS 102 979)
bool dabHasJournaline(const DabEnsemble& ens);
void dabFicTab(App& a);            // dab_fic_ui.cpp: the FIC, FIG by FIG, as a tree
void dabTransmittersTab(App& a);   // dab_tii_ui.cpp: the TII transmitters, on a map with a transmitter list
void dabScanTab(App& a);
void dabScanStep(App& a);
void fmScanTab(App& a);
void fmScanStep(App& a);
// fm_ui.cpp
bool fmFrequencyCombo(App& a);
void fmStatus(App& a);
void fmHistory(App& a);
void fmPanels(App& a);
void fmRadioTab(App& a);
void fmRadioPanel(App& a);
void fmTune(App& a, double mhz);
// wizard.cpp
void wizEnter(App& a, int step);
void wizAction(App& a, int step);
void wizFinish(App& a);
void wizard(App& a, ImVec2 disp);
void setupWizardOpen(App& a);                        // wizard.cpp: (re)start the setup wizard
void setupWizard(App& a, ImVec2 disp);               // wizard.cpp: the window, each frame
#include "dect2/device_claims.h"
#include "receivers.h"
