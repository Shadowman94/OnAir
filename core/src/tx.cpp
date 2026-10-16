// Transmit: the list of modes OnAir can send (see tx.h for the reasoning), their generators, and the TxSession that feeds a radio.
#include "dect2/tx.h"
#include "dect2/atsc_gen.h"
#include "dect2/device_claims.h"
#include "dect2/demo_ts.h"
#include "dect2/dtmb_gen.h"
#include "dect2/dvbc_rx.h"
#include "dect2/dvbs_gen.h"
#include "dect2/dvbt.h"
#include "dect2/dvbt_gen.h"
#include "dect2/exact_resampler.h"
#include "dect2/dsp_compat.h"
#include "dect2/fftutil.h"
#include "dect2/isdbt.h"
#include "dect2/hfdig_freedv.h"
#include "dect2/isdbt_gen.h"
#include "dect2/m17_codec2.h"
#include "dect2/mode_tuning.h"
#include "dect2/offset_tune.h"
#include "dect2/t2.h"
#include "dect2/t2gen.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace dect2 {

// ---------------------------------------------------------------- the list

namespace {
constexpr const char* kTrip = "tests/test_tx.cpp";
constexpr const char* kUnv = "offered from the mode generator; engine round-trip not run in test_tx (unverified)";
constexpr const char* kOpt = "the mode's SynthConfig (see the mode's *_gen.h)";

TxModeSpec mk(const char* id, const char* name, int std, const char* gen, const char* opt, const char* trip,
              int variant = 0, double nom = 0, bool fixed = false, double minr = -1) {
    TxModeSpec m;
    m.id = id; m.name = name; m.stdMode = std; m.variant = variant;
    const ModeTuning* mt = modeTuning(std);
    m.nominalRateHz = nom > 0 ? nom : (mt ? mt->sampleRate : 2e6);
    m.fixedRate = fixed;
    m.minRateHz = minr >= 0 ? minr : (mt ? mt->minSampleRate : 0);
    m.generator = gen; m.options = opt; m.roundTrip = trip;
    return m;
}

std::vector<TxModeSpec> makeTxTable() {
    // First eight: the original verified TV / ADS-B list. After that: every other generator-capable mode that is not on the deny-list.
    return {
        mk("dvbt2", "DVB-T2", 0, "T2Generator, real PLP with the test transport stream", "FFT size, guard interval, pilot pattern, channel width", kTrip, 1, 64e6 / 7, true, 0),
        mk("dvbt", "DVB-T", 0, "dvbt::Generator, demo programme", "mode 2K/8K, guard, modulation, code rate, channel width", kTrip, 2, 64e6 / 7, true, 0),
        mk("atsc", "ATSC 1.0 (8-VSB)", 3, "atsc::Generator, demo programme", "none", kTrip, 0, 8e6, false, 6.5e6),
        mk("dab", "DAB / DAB+", 4, "makeDabSynth, transmission mode I with real audio", "none", kTrip, 0, 2.048e6, false, 2e6),
        mk("isdbt", "ISDB-T", 6, "ISDB-T layers A and B with the demo programme", "channel width 6/7/8 MHz", kTrip, 0, 8e6, false, 6e6),
        mk("dvbs", "DVB-S / S2 / S2X", 8, "makeDvbsSynth (dvbs_tx.h chain), demo programme", "standard, modulation, code rate, roll-off, symbol rate ...", kTrip),
        mk("dtmb", "DTMB", 9, "makeDtmbSynth (dtmb_tx.h chain), demo programme", "header, modulation, code rate, interleaver, channel width", kTrip),
        mk("adsb", "ADS-B / Mode S", 13, "makeAdsbSynth, Mode S frames of simulated aircraft", "number of aircraft, message mix, message rate", kTrip),
        mk("atsc3", "ATSC 3.0", 5, "makeAtsc3Synth, in-memory test programme", kOpt, kUnv, 0, 8e6, false, 6.5e6),
        mk("fm", "FM broadcast", 7, "FmGenerator, stereo multiplex with RDS", "tone Hz, WAV, station name, radio text", kTrip, 0, 2e6, false, 500e3),
        mk("atv", "Analog TV", 10, "makeAtvSynth, test card and sound carrier", kOpt, kUnv),
        mk("dmr", "DMR", 11, "makeDmrSynth", kOpt, kTrip),
        mk("drm", "DRM", 12, "makeDrmSynth", kOpt, kUnv),
        mk("sonde", "Radiosonde", 15, "makeSondeSynth", kOpt, kUnv),
        mk("ais", "AIS", 16, "makeAisSynth", kOpt, kTrip),
        mk("marine", "Marine (NAVTEX / DSC / weather fax)", 17, "makeMarineSynth", kOpt, kUnv),
        mk("acars", "ACARS", 18, "makeAcarsSynth", kOpt, kUnv),
        mk("inmc", "Inmarsat-C", 19, "makeInmcSynth", kOpt, kUnv),
        mk("aero", "Inmarsat Aero", 20, "makeAeroSynth", kOpt, kUnv),
        mk("iridium", "Iridium", 21, "makeIridiumSynth", kOpt, kUnv),
        mk("mesh", "Mesh (LoRa)", 22, "makeMeshSynth", kOpt, kUnv),
        mk("hdr", "HD Radio", 23, "makeHdrSynth", kOpt, kUnv),
        mk("cdr", "CDR", 24, "makeCdrSynth", kOpt, kUnv),
        mk("pager", "Pagers (POCSAG / FLEX)", 25, "makePagerSynth, FM on a 25 kHz channel", "mix / POCSAG / FLEX, speed, polarity (pager_gen.h)", kTrip),
        mk("packet", "APRS / Packet", 26, "makePacketSynth", kOpt, kTrip),
        mk("rtty", "RTTY", 27, "makeRttyTxAudio / makeHfdigSynth", "text, baud, shift, USB/LSB", kTrip, 0),
        mk("sstv", "SSTV", 27, "makeSstvTx, PPM or test picture with callsign stamp", "image file, callsign, SSTV mode", kUnv, 1),
        mk("freedv", "FreeDV", 27, "makeFreedvTestAudio", kOpt, kUnv, 2),
        mk("ft8", "FT8", 27, "makeFtxTxAudio", "callsign, grid, message kind", kTrip, 3),
        mk("ft4", "FT4", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 4),
        mk("wspr", "WSPR", 27, "makeFtxTxAudio", "callsign, grid, power dBm", kUnv, 5),
        mk("ft2", "FT2", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 6),
        mk("hamdrm", "HamDRM", 27, "makeHamdrmTx, file bytes and callsign", "file, callsign", kUnv, 7),
        mk("jt65", "JT65", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 21),
        mk("jt9", "JT9", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 22),
        mk("q65", "Q65", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 23),
        mk("fst4", "FST4", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 24),
        mk("msk144", "MSK144", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 25),
        mk("olivia", "Olivia", 27, "makeKbdTx", "text, tones, bandwidth", kUnv, 26),
        mk("contestia", "Contestia", 27, "makeKbdTx", "text", kUnv, 27),
        mk("mfsk", "MFSK", 27, "makeKbdTx", "text", kUnv, 28),
        mk("thor", "THOR", 27, "makeKbdTx", "text", kUnv, 29),
        mk("dominoex", "DominoEX", 27, "makeKbdTx", "text", kUnv, 30),
        mk("hell", "Hellschreiber", 27, "makeKbdTx", "text", kUnv, 31),
        mk("airband", "Airband AM", 28, "makeAirbandSynth, AM voice channels", "tone Hz or WAV file", kUnv),
        mk("vdl2", "VDL Mode 2", 29, "makeVdl2Synth", kOpt, kUnv),
        mk("vdl4", "VDL Mode 4", 84, "makeVdl4Synth", kOpt, kUnv),
        mk("uat", "UAT 978", 85, "makeUatSynth", kOpt, kUnv),
        mk("wisun", "Wi-SUN", 86, "makeWisunSynth", kOpt, kUnv),
        mk("vorils", "VOR / ILS", 32, "makeVorSynth", kOpt, kUnv),
        mk("m17", "M17", 33, "makeM17Synth", kOpt, kUnv),
        mk("dstar", "D-STAR", 35, "makeDstarSynth", kOpt, kUnv),
        mk("cw", "CW", 36, "makeCwSynth", kOpt, kTrip),
        mk("hfdl", "HFDL", 37, "makeHfdlSynth", kOpt, kUnv),
        mk("psk", "PSK31", 38, "makePskSynth", kOpt, kUnv),
        mk("js8", "JS8Call", 39, "makeJs8Synth", kOpt, kUnv),
        mk("nxdn", "NXDN", 40, "makeNxdnSynth", kOpt, kUnv),
        mk("dvbc", "DVB-C", 42, "makeDvbcSynth", kOpt, kUnv),
        mk("uplink", "Mode S uplink", 45, "makeUplinkSynth", kOpt, kUnv),
        mk("orbcomm", "Orbcomm", 46, "makeOrbcommSynth", kOpt, kUnv),
        mk("dpmr", "dPMR", 47, "makeDpmrSynth", kOpt, kUnv),
        mk("tetra", "TETRA", 48, "makeTetraSynth", kOpt, kUnv),
        mk("mac", "MAC", 49, "makeMacSynth", kOpt, kUnv),
        mk("ale", "HF ALE", 51, "makeAleSynth", kOpt, kUnv),
        mk("p25", "P25", 52, "makeP25Synth", kOpt, kUnv),
        mk("qo100", "QO-100", 55, "makeQo100Synth", kOpt, kUnv),
        mk("meteor", "Meteor-M LRPT", 61, "makeMeteorSynth", kOpt, kUnv),
        mk("goes", "GOES HRIT", 62, "makeGoesSynth", kOpt, kUnv),
        mk("cubesat", "CubeSat telemetry", 63, "makeCubesatSynth", kOpt, kUnv),
        mk("globalstar", "Globalstar", 64, "makeGlobalstarSynth", kOpt, kUnv),
        mk("vdes", "VDES", 65, "makeVdesSynth", kOpt, kUnv),
        mk("hrpt", "HRPT / AHRPT", 66, "makeHrptSynth", kOpt, kUnv),
        mk("zigbee", "Zigbee / 802.15.4", 67, "makeZigbeeSynth", kOpt, kUnv),
        mk("flarm", "FLARM", 68, "makeFlarmSynth", kOpt, kUnv),
        mk("hfbeacon", "HF beacons", 69, "makeHfbeaconSynth", kOpt, kUnv),
        mk("dji", "DJI drones", 70, "makeDjiSynth", kOpt, kUnv),
        mk("loraraw", "LoRa (raw)", 73, "makeLoraRawSynth", kOpt, kUnv),
        mk("same", "NOAA Weather Radio SAME", 78, "makeSameSynth", kOpt, kUnv),
        mk("dect", "DECT cordless phones", 80, "makeDectSynth", kOpt, kUnv),
    };
}
}

const std::vector<TxModeSpec>& txModes() {
    static const std::vector<TxModeSpec> t = makeTxTable();
    return t;
}

const TxModeSpec* txModeById(const std::string& id) {
    for (const auto& m : txModes()) if (id == m.id) return &m;
    return nullptr;
}

std::unique_ptr<ModeSynth> makeTxPayloadSynth(const TxModeSpec& mode, const TxRequest& r, const SynthConfig& clean, double rateHz);

const TxModeSpec* txModeFor(int stdMode, int dvbVariant) {
    if (stdMode == 1) return txModeById("dvbt2");
    if (stdMode == 2) return txModeById("dvbt");
    for (const auto& m : txModes()) {
        if (m.stdMode != stdMode) continue;
        if (stdMode == 0 && m.variant != (dvbVariant == 2 ? 2 : 1)) continue;
        return &m;
    }
    return nullptr;
}

std::string txRefusalReason(int stdMode) {
    if (txModeFor(stdMode, 1)) return {};
    switch (stdMode) {
    case 14: return "GNSS is not transmitted: its generator is a satellite-signal simulator (spoofing)";
    case 34: case 41: case 59: case 71:
        return "cellular signals are not transmitted: the generators model a network cell for testing the decoder";
    case 53: return "Bluetooth is not transmitted: it would impersonate a Bluetooth advertiser";
    case 56: case 72:
        return "Wi-Fi is not transmitted: the generator models access-point beacons for the decoder";
    case 58: return "Drone Remote ID is not transmitted: it rides on Wi-Fi and Bluetooth advertisements";
    case 79: return "ANT+ is not transmitted: it would impersonate a sensor network";
    case 81: return "TETRAPOL is not transmitted: receive only, control-channel system information";
    case 30: case 76:
        return "this is a receive-only sensor harvester: there is nothing meaningful to transmit";
    case 43: case 50: case 54: case 74:
        return "this is an analysis mode: there is nothing to transmit";
    case 44: return "this is a spectrum analyser: there is nothing to transmit";
    case 57: return "this is a general listening mode: there is nothing specific to transmit";
    case 60: return "time signals are not transmitted: the generator imitates broadcast clocks (spoofing)";
    case 75: return "this is a signal workbench: it is for measurement, not a radio service";
    case 77: return "Multitone paging is not transmitted: the generator is not the real (unpublished) format, only a detector test";
    default: return "this mode has no transmit generator";
    }
}

int txEngineStd(int family) {
    if (family >= 6) return family + 2;
    if (family == 1) return 3;
    if (family == 2) return 4;
    if (family == 3) return 5;
    if (family == 4) return 6;
    if (family == 5) return 7;
    return 0;
}

const TxModeSpec* txModeForList(int family, const char* subId, int dvbVariant) {
    if (subId && *subId) {
        if (const TxModeSpec* m = txModeById(subId)) return m;
        const std::string s = subId;
        if (s == "pocsag" || s == "flex") return txModeById("pager");
        if (s == "navtex" || s == "dsc" || s == "wefax") return txModeById("marine");
        if (s == "vor" || s == "ils") return txModeById("vorils");
        if (s == "lorawan" || s == "meshtastic" || s == "meshcore" || s == "loraaprs" || s == "meshcom") return txModeById("mesh");
    }
    if (family == 0) return txModeFor(0, dvbVariant == 2 ? 2 : 1);
    return txModeFor(txEngineStd(family), dvbVariant);
}

TxPayloadKind txPayloadKind(const std::string& id) {
    if (id == "dvbt2" || id == "dvbt" || id == "atsc" || id == "atsc3" || id == "isdbt" || id == "dvbs" || id == "dtmb" || id == "dvbc")
        return TxPayloadKind::TvFile;
    if (id == "atv") return TxPayloadKind::AnalogTv;
    if (id == "dab" || id == "drm" || id == "hdr" || id == "cdr") return TxPayloadKind::BroadcastAudio;
    if (id == "ft8" || id == "ft4" || id == "wspr" || id == "ft2" || id == "jt65" || id == "jt9" || id == "q65" || id == "fst4" || id == "msk144")
        return TxPayloadKind::Ftx;
    if (id == "rtty") return TxPayloadKind::Rtty;
    if (id == "olivia" || id == "contestia" || id == "mfsk" || id == "thor" || id == "dominoex" || id == "hell") return TxPayloadKind::Keyboard;
    if (id == "cw") return TxPayloadKind::Cw;
    if (id == "psk") return TxPayloadKind::Psk;
    if (id == "js8") return TxPayloadKind::Js8;
    if (id == "hamdrm") return TxPayloadKind::Hamdrm;
    if (id == "sstv") return TxPayloadKind::Sstv;
    if (id == "freedv") return TxPayloadKind::Freedv;
    if (id == "pager") return TxPayloadKind::Pager;
    if (id == "packet") return TxPayloadKind::Packet;
    if (id == "ais") return TxPayloadKind::Ais;
    if (id == "adsb") return TxPayloadKind::AdsB;
    if (id == "fm") return TxPayloadKind::VoiceFm;
    if (id == "airband") return TxPayloadKind::VoiceAir;
    if (id == "m17" || id == "dmr" || id == "p25" || id == "nxdn" || id == "dpmr" || id == "dstar" || id == "tetra") return TxPayloadKind::VoiceDigital;
    if (id == "qo100") return TxPayloadKind::Qo100;
    if (id == "marine") return TxPayloadKind::Marine;
    if (id == "acars" || id == "vdl2" || id == "hfdl") return TxPayloadKind::AcarsFamily;
    if (id == "sonde") return TxPayloadKind::Sonde;
    if (id == "same") return TxPayloadKind::Same;
    if (id == "ale") return TxPayloadKind::Ale;
    if (id == "vorils") return TxPayloadKind::VorIls;
    if (id == "loraraw") return TxPayloadKind::LoraRaw;
    if (id == "dect") return TxPayloadKind::Dect;
    if (id == "cubesat") return TxPayloadKind::CubeSat;
    if (id == "meteor" || id == "goes" || id == "hrpt") return TxPayloadKind::WeatherScene;
    if (id == "inmc" || id == "aero" || id == "iridium" || id == "orbcomm" || id == "globalstar") return TxPayloadKind::SatScene;
    if (id == "flarm") return TxPayloadKind::Flarm;
    if (id == "zigbee") return TxPayloadKind::Zigbee;
    if (id == "dji") return TxPayloadKind::Dji;
    if (id == "hfbeacon") return TxPayloadKind::HfBeacon;
    if (id == "vdes") return TxPayloadKind::Vdes;
    if (id == "uplink") return TxPayloadKind::Uplink;
    if (id == "mesh") return TxPayloadKind::Mesh;
    if (id == "mac") return TxPayloadKind::Mac;
    if (id == "vdl4" || id == "uat" || id == "wisun") return TxPayloadKind::SatScene;   // demo-traffic generators, no custom message
    return TxPayloadKind::SatScene;   // never a generic callsign dump; unknown ids are treated as a test scene
}

bool txCanEncodeVoice(const TxModeSpec& m) {
    const std::string id = m.id ? m.id : "";
    if (id == "m17") return m17Codec2Available();
    if (id == "freedv") return hfdigFreedvLibrary();
    return false;
}

std::string txVocoderNote(const TxModeSpec& m) {
    const std::string id = m.id ? m.id : "";
    if (id == "m17") {
        return m17Codec2Available()
            ? "Live voice is encoded with Codec 2 (libcodec2)."
            : "libcodec2 is not loaded: live encode is off. Destination callsigns still go on the air; voice bits are the known test pattern. A 1 kHz tone, silence or a WAV/MP3 can still be sent as analog NFM.";
    }
    if (id == "freedv") {
        return hfdigFreedvLibrary()
            ? "Live voice is encoded with Codec 2 (FreeDV)."
            : "libcodec2 is not loaded: live encode is off. A 1 kHz tone, silence or a WAV/MP3 can still be sent.";
    }
    if (id == "dmr" || id == "p25" || id == "nxdn" || id == "dpmr" || id == "dstar")
        return "This vocoder cannot encode: OnAir loads mbelib for decode only (AMBE/IMBE). Destination ID / talkgroup / colour code still go on the air; voice bits are the test pattern. A 1 kHz tone, silence or a WAV/MP3 can be sent as analog NFM instead.";
    if (id == "tetra")
        return "TETRA ACELP is not encoded here. Colour code and IDs are shown; the downlink test pattern is sent. A 1 kHz tone, silence or a WAV/MP3 can be sent as analog NFM instead.";
    return {};
}

bool txModeNeedsAudio(const TxModeSpec& m, const TxRequest& r) {
    if (r.carrierTune) return false;
    const std::string id = m.id ? m.id : "";
    if (r.analogVoice) return true;
    if (id == "qo100" && r.variant == 1) return false;
    if (id == "fm" || id == "airband" || id == "qo100") return true;
    if (id == "atv") return r.audioSrc == 0 || r.audioSrc == 1;
    if ((id == "m17" || id == "freedv") && txCanEncodeVoice(m)) return true;
    return false;
}

bool txClaimRadio(const DeviceInfo& d, std::string& err) {
    txReleaseRadio();
    if (d.txFullDuplex) return true;   // receiving and transmitting on the same radio is allowed: no claim
    int holder = 0;
    if (claimDevice(d, kTxClaimOwner, &holder)) return true;
    err = d.name + " is in use by " + deviceHolderText(holder) + ", and it cannot receive and transmit at the same time. Choose another radio for transmitting, or stop " +
          deviceHolderText(holder) + " first.";
    return false;
}
void txReleaseRadio() { releaseDevice(kTxClaimOwner); }

// ---------------------------------------------------------------- generators

namespace {

// a stream of samples built up block by block from a frame generator
class FrameGen : public TxGenerator {
public:
    FrameGen(double rate, std::function<void(std::vector<cf32>&)> next, float gain) : rate_(rate), next_(std::move(next)), gain_(gain) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        while (pending_.size() - pos_ < n) {
            frame_.clear();
            next_(frame_);
            pending_.insert(pending_.end(), frame_.begin(), frame_.end());
        }
        for (size_t i = 0; i < n; i++) out[i] = pending_[pos_ + i] * gain_;
        pos_ += n;
        if (pos_ > (1u << 20)) { pending_.erase(pending_.begin(), pending_.begin() + (long)pos_); pos_ = 0; }
    }
private:
    double rate_;
    std::function<void(std::vector<cf32>&)> next_;
    float gain_;
    std::vector<cf32> pending_, frame_;
    size_t pos_ = 0;
};

class AtscGen : public TxGenerator {
public:
    AtscGen(double rate, std::function<void(uint8_t*)> ts) : rate_(rate) {
        atsc::ChannelConfig cc;   // snrDb 99: no noise worth the name
        cc.snrDb = 200;
        g_ = std::make_unique<atsc::Generator>(ts ? std::move(ts) : demoTsSource(19.39e6, DemoTsIdentity()), cc, rate);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        tmp_.clear();
        g_->generate(n, tmp_);
        const float g = 0.22f / 6.7f;   // the 8-VSB signal has an rms of about 6.7 units
        for (size_t i = 0; i < n; i++) out[i] = tmp_[i] * g;
    }
private:
    double rate_;
    std::unique_ptr<atsc::Generator> g_;
    std::vector<cf32> tmp_;
};

// ModeSynth modes are already at the rate asked for
class SynthGen : public TxGenerator {
public:
    SynthGen(std::unique_ptr<ModeSynth> s, double rate) : s_(std::move(s)), rate_(rate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { s_->generate(out, n); }
private:
    std::unique_ptr<ModeSynth> s_;
    double rate_;
};

// a generator at its own rate, played at another one
class ResampledGen : public TxGenerator {
public:
    ResampledGen(std::unique_ptr<TxGenerator> in, double outRate) : in_(std::move(in)), out_(outRate) {}
    bool configure() { return rs_.configure(in_->sampleRate(), out_); }
    double sampleRate() const override { return out_; }
    void generate(cf32* out, size_t n) override {
        std::vector<cf32> in(1 << 14);
        while (pending_.size() - pos_ < n) {
            in_->generate(in.data(), in.size());
            rs_.process(in.data(), in.size(), pending_);
        }
        std::copy(pending_.begin() + (long)pos_, pending_.begin() + (long)(pos_ + n), out);
        pos_ += n;
        if (pos_ > (1u << 20)) { pending_.erase(pending_.begin(), pending_.begin() + (long)pos_); pos_ = 0; }
    }
private:
    std::unique_ptr<TxGenerator> in_;
    double out_;
    ExactResampler rs_;
    std::vector<cf32> pending_;
    size_t pos_ = 0;
};

class CarrierGate : public TxGenerator {
public:
    CarrierGate(std::unique_ptr<TxGenerator> inner, std::atomic<bool>* flag, double rate)
        : inner_(std::move(inner)), flag_(flag), rate_(rate) {}
    double sampleRate() const override { return inner_ ? inner_->sampleRate() : rate_; }
    void generate(cf32* out, size_t n) override {
        if (flag_ && flag_->load(std::memory_order_relaxed)) {
            for (size_t i = 0; i < n; i++) out[i] = cf32(1.f, 0.f);
            return;
        }
        if (inner_) inner_->generate(out, n);
        else for (size_t i = 0; i < n; i++) out[i] = cf32(1.f, 0.f);
    }
private:
    std::unique_ptr<TxGenerator> inner_;
    std::atomic<bool>* flag_;
    double rate_;
};

std::function<void(uint8_t*)> loopingTsFile(const std::string& path, int stride = 188, int syncOff = 0) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    auto hold = std::shared_ptr<FILE>(f, fclose);
    return [hold, stride, syncOff](uint8_t* pkt) {
        std::vector<uint8_t> raw((size_t)stride);
        for (;;) {
            if (fread(raw.data(), 1, (size_t)stride, hold.get()) == (size_t)stride) {
                if (raw[(size_t)syncOff] == 0x47) { memcpy(pkt, raw.data() + syncOff, 188); return; }
                fseek(hold.get(), -(stride - 1), SEEK_CUR);
                continue;
            }
            fseek(hold.get(), 0, SEEK_SET);
            if (fread(raw.data(), 1, (size_t)stride, hold.get()) != (size_t)stride) { memset(pkt, 0, 188); pkt[0] = 0x47; return; }
            if (raw[(size_t)syncOff] == 0x47) { memcpy(pkt, raw.data() + syncOff, 188); return; }
        }
    };
}

bool looksLikeMpegTs(const std::string& path, int& stride, int& syncOff) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t b[192 * 4] = {};
    const size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    auto hits = [&](int st, int off) {
        int ok = 0, need = 0;
        for (size_t i = (size_t)off; i + 188 <= n; i += (size_t)st) { need++; if (b[i] == 0x47) ok++; }
        return need >= 3 && ok == need;
    };
    if (hits(188, 0)) { stride = 188; syncOff = 0; return true; }
    if (hits(192, 4)) { stride = 192; syncOff = 4; return true; }
    return false;
}

} // namespace

static bool remuxToMpegTs(const std::string& in, const std::string& out, std::string& err) {
    AVFormatContext* ic = nullptr;
    if (avformat_open_input(&ic, in.c_str(), nullptr, nullptr) < 0) { err = "could not open the file"; return false; }
    if (avformat_find_stream_info(ic, nullptr) < 0) { avformat_close_input(&ic); err = "could not read the file"; return false; }
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "mpegts", out.c_str()) < 0 || !oc) {
        avformat_close_input(&ic); err = "MPEG-TS muxer is not in this FFmpeg build"; return false;
    }
    std::vector<int> map(ic->nb_streams, -1);
    int nOut = 0;
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream* is = ic->streams[i];
        const AVMediaType t = is->codecpar->codec_type;
        if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO) continue;
        AVStream* os = avformat_new_stream(oc, nullptr);
        if (!os || avcodec_parameters_copy(os->codecpar, is->codecpar) < 0) continue;
        os->codecpar->codec_tag = 0;
        map[i] = nOut++;
    }
    if (nOut == 0) {
        avformat_free_context(oc); avformat_close_input(&ic);
        err = "the file has no video or audio stream to remux"; return false;
    }
    if (!(oc->oformat->flags & AVFMT_NOFILE) && avio_open(&oc->pb, out.c_str(), AVIO_FLAG_WRITE) < 0) {
        avformat_free_context(oc); avformat_close_input(&ic);
        err = "could not write the remuxed transport stream"; return false;
    }
    if (avformat_write_header(oc, nullptr) < 0) {
        avio_closep(&oc->pb); avformat_free_context(oc); avformat_close_input(&ic);
        err = "could not write an MPEG-TS header (the codecs may need a full encode)"; return false;
    }
    AVPacket* pkt = av_packet_alloc();
    int rc = 0;
    while ((rc = av_read_frame(ic, pkt)) >= 0) {
        const int oi = pkt->stream_index >= 0 && pkt->stream_index < (int)map.size() ? map[(size_t)pkt->stream_index] : -1;
        if (oi < 0) { av_packet_unref(pkt); continue; }
        AVStream* is = ic->streams[pkt->stream_index];
        AVStream* os = oc->streams[oi];
        pkt->stream_index = oi;
        av_packet_rescale_ts(pkt, is->time_base, os->time_base);
        pkt->pos = -1;
        if (av_interleaved_write_frame(oc, pkt) < 0) { av_packet_unref(pkt); rc = -1; break; }
        av_packet_unref(pkt);
    }
    av_write_trailer(oc);
    av_packet_free(&pkt);
    avio_closep(&oc->pb);
    avformat_free_context(oc);
    avformat_close_input(&ic);
    if (rc < 0 && rc != AVERROR_EOF) { err = "remux stopped before the end of the file"; return false; }
    return true;
}

bool txOpenMpegTs(const std::string& path, std::function<void(uint8_t*)>& outTs, std::string& note) {
    outTs = {};
    if (path.empty()) { note = "empty: the built-in demo programme"; return false; }
    int stride = 188, syncOff = 0;
    if (looksLikeMpegTs(path, stride, syncOff)) {
        outTs = loopingTsFile(path, stride, syncOff);
        note = "this file is used as a transport stream";
        return (bool)outTs;
    }
    char tmp[512];
#ifdef _WIN32
    snprintf(tmp, sizeof tmp, "%s\\onair_tx_remux.ts", getenv("TEMP") ? getenv("TEMP") : ".");
#else
    snprintf(tmp, sizeof tmp, "/tmp/onair_tx_remux_%d.ts", (int)getpid());
#endif
    std::string err;
    if (!remuxToMpegTs(path, tmp, err)) { note = err; return false; }
    outTs = loopingTsFile(tmp, 188, 0);
    note = "remuxed to MPEG-TS for the transmitter";
    return (bool)outTs;
}

void txSpectrumDb(const cf32* x, size_t n, int nfft, std::vector<float>& db) {
    db.assign((size_t)nfft, -120.f);
    if (!x || n == 0 || nfft < 8) return;
    int log2n = 0;
    while ((1 << log2n) < nfft) log2n++;
    nfft = 1 << log2n;
    db.assign((size_t)nfft, -120.f);
    std::vector<float> re((size_t)nfft), im((size_t)nfft);
    const size_t use = std::min(n, (size_t)nfft);
    for (int i = 0; i < nfft; i++) {
        const float w = i < (int)use ? (float)(0.5 - 0.5 * std::cos(2 * 3.14159265358979323846 * i / (double)use)) : 0.f;
        const cf32 s = i < (int)use ? x[i] : cf32(0.f, 0.f);
        re[(size_t)i] = s.real() * w;
        im[(size_t)i] = s.imag() * w;
    }
    fftSplit(re.data(), im.data(), log2n, false);
    float peak = 1e-20f;
    for (int i = 0; i < nfft; i++) {
        const float p = re[(size_t)i] * re[(size_t)i] + im[(size_t)i] * im[(size_t)i];
        peak = std::max(peak, p);
        db[(size_t)i] = 10.f * std::log10(std::max(p, 1e-20f) / (float)nfft);
    }
    (void)peak;
}

static SynthConfig cleanSynth(const SynthConfig& c, int stdMode) {
    SynthConfig s = c;
    s.snrDb = 200;            // no noise, no carrier or clock offset, no echo: a transmitter makes the clean signal
    s.cfoHz = 0;
    s.sroPpm = 0; s.echoDb = 0;
    s.gainModel = false;
    s.pace = 1.0;
    s.mode = stdMode;
    return s;
}

std::unique_ptr<TxGenerator> makeTxGenerator(const TxModeSpec& m, const TxRequest& r, double rateHz, std::string& err) {
    const SynthConfig s = cleanSynth(r.synth, m.stdMode);
    std::unique_ptr<TxGenerator> g;
    double native = 0;
    std::function<void(uint8_t*)> fileTs;
    std::string fileNote;
    const bool wantTs = m.stdMode == 0 || m.stdMode == 3 || m.stdMode == 6 || m.stdMode == 8 || m.stdMode == 9 || m.stdMode == 42;
    if (wantTs && !r.file.empty()) {
        if (!txOpenMpegTs(r.file, fileTs, fileNote)) {
            if (fileNote.empty()) fileNote = "could not use this file as a transport stream; the demo programme is sent";
        }
    } else if (wantTs) fileNote = r.services.empty() && r.name.empty() ? "empty: the built-in demo programme" : "built-in test card with your service names";
    const_cast<TxRequest&>(r).fileNote = fileNote;
    auto ident = [](const TxRequest& r) {
        DemoTsIdentity id;
        if (!r.provider.empty()) id.provider = r.provider;
        if (!r.network.empty()) id.network = r.network;
        id.serviceId = r.serviceId > 0 ? r.serviceId : 1;
        id.programmes = r.mux;
        id.services = r.services;
        if (id.services.empty() && !r.name.empty()) id.services.push_back(r.name);
        return id;
    };
    auto namedTs = [&](std::function<void(uint8_t*)> file, double br) {
        if (file) return file;
        return demoTsSource(br > 1e6 ? br : 20e6, ident(r));
    };
    if (m.stdMode == 0) {
        const double bw = r.bandwidthMhz > 0 ? r.bandwidthMhz : 8;
        native = nativeRateHz(bw);
        if (m.variant == 2) {
            dvbt::Params tp;
            tp.mode = s.dvbtMode; tp.guard = s.dvbtGuard; tp.mod = s.dvbtMod; tp.crHp = tp.crLp = s.dvbtRate;
            auto gen = std::make_shared<dvbt::Generator>(tp, namedTs(fileTs, dvbt::netBitrate(tp, native)));
            g = std::make_unique<FrameGen>(native, [gen](std::vector<cf32>& f) { gen->nextSymbol(f); }, 0.25f);
        } else {
            TxParams tp = s.tx;
            tp.payload = true;
            tp.tsSource = namedTs(fileTs, 24e6);
            auto gen = std::make_shared<T2Generator>(tp);
            g = std::make_unique<FrameGen>(native, [gen](std::vector<cf32>& f) { gen->nextFrame(f); }, 0.25f);
        }
    } else if (m.stdMode == 3) {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        if (rr < 6.5e6) { err = "ATSC needs at least 6.5 Msps"; return nullptr; }
        g = std::make_unique<AtscGen>(rr, namedTs(fileTs, 19.39e6));
        native = rr;
    } else if (m.stdMode == 8) {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        auto ms = makeDvbsSynth(s, rr, namedTs(fileTs, dvbsNetBitrate(dvbs::dvbsConfigFromSynth(s, rr).tx)));
        if (!ms) { char b[200]; snprintf(b, sizeof b, "the %s generator cannot make a signal at %.3f Msps with these options", m.name, rr / 1e6); err = b; return nullptr; }
        g = std::make_unique<SynthGen>(std::move(ms), rr);
        native = rr;
    } else if (m.stdMode == 9) {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        auto ms = makeDtmbSynth(s, rr, namedTs(fileTs, 20e6));
        if (!ms) { char b[200]; snprintf(b, sizeof b, "the %s generator cannot make a signal at %.3f Msps with these options", m.name, rr / 1e6); err = b; return nullptr; }
        g = std::make_unique<SynthGen>(std::move(ms), rr);
        native = rr;
    } else if (m.stdMode == 42) {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        auto ms = makeDvbcSynth(s, rr, namedTs(fileTs, 20e6));
        if (!ms) { char b[200]; snprintf(b, sizeof b, "the %s generator cannot make a signal at %.3f Msps with these options", m.name, rr / 1e6); err = b; return nullptr; }
        g = std::make_unique<SynthGen>(std::move(ms), rr);
        native = rr;
    } else if (m.stdMode == 55 && r.variant == 1) {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        std::function<void(uint8_t*)> ts;
        std::string note;
        if (!r.file.empty()) txOpenMpegTs(r.file, ts, note);
        const_cast<TxRequest&>(r).fileNote = note;
        auto ms = makeDvbsSynth(s, rr, namedTs(ts, dvbsNetBitrate(dvbs::dvbsConfigFromSynth(s, rr).tx)));
        if (!ms) ms = makeModeSynth(m.stdMode, s, rr);
        if (!ms) { err = "QO-100 DATV could not start"; return nullptr; }
        g = std::make_unique<SynthGen>(std::move(ms), rr);
        native = rr;
    } else if (m.stdMode == 6) {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        isdbt::Params p;
        p.mode = 3; p.guard = isdbt::kGi8; p.partial = true;
        p.layer[0].segments = 1; p.layer[0].mod = isdbt::kQpsk; p.layer[0].rate = isdbt::kR23; p.layer[0].ti = 2;
        p.layer[1].segments = 12; p.layer[1].mod = isdbt::k64Qam; p.layer[1].rate = isdbt::kR34; p.layer[1].ti = 1;
        p.bwMhz = s.isdbtBwMhz == 7 || s.isdbtBwMhz == 8 ? s.isdbtBwMhz : 6;
        auto src = isdbt::singleLayerSource(1, namedTs(fileTs, isdbt::layerBitrate(p, 1) * 0.9));
        auto gen = std::make_shared<isdbt::Generator>(p, src, 1);
        const double nativeIsdb = isdbt::sampleRateFor(p.bwMhz);
        g = std::make_unique<FrameGen>(nativeIsdb, [gen](std::vector<cf32>& f) { gen->nextFrame(f); }, 0.22f);
        native = nativeIsdb;
        if (rr > 0 && std::fabs(rr - native) > 0.5) {
            auto rg = std::make_unique<ResampledGen>(std::move(g), rr);
            if (!rg->configure()) { char b[160]; snprintf(b, sizeof b, "%s cannot be resampled from %.3f to %.3f Msps", m.name, native / 1e6, rr / 1e6); err = b; return nullptr; }
            return rg;
        }
        return g;
    } else {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        if (m.minRateHz > 0 && rr < m.minRateHz) { char b[160]; snprintf(b, sizeof b, "%s needs at least %.3g Msps; the radio runs %.3g Msps", m.name, m.minRateHz / 1e6, rr / 1e6); err = b; return nullptr; }
        auto ms = makeTxPayloadSynth(m, r, s, rr);
        if (!ms) ms = makeModeSynth(m.stdMode, s, rr);
        if (!ms) { char b[200]; snprintf(b, sizeof b, "the %s generator cannot make a signal at %.3f Msps with these options", m.name, rr / 1e6); err = b; return nullptr; }
        g = std::make_unique<SynthGen>(std::move(ms), rr);
        native = rr;
    }
    if (rateHz > 0 && std::fabs(rateHz - native) > 0.5) {
        auto rg = std::make_unique<ResampledGen>(std::move(g), rateHz);
        if (!rg->configure()) { char b[160]; snprintf(b, sizeof b, "%s cannot be resampled from %.3f to %.3f Msps", m.name, native / 1e6, rateHz / 1e6); err = b; return nullptr; }
        return rg;
    }
    return g;
}

double txRequestRate(const TxModeSpec& m, const TxRequest& r, const TxRateLimits& lim) {
    double rate;
    if (r.rateHz > 0) rate = r.rateHz;   // the operator's rate, not the mode's
    else {
        rate = m.stdMode == 0 ? nativeRateHz(r.bandwidthMhz > 0 ? r.bandwidthMhz : 8) : m.nominalRateHz;
        if (m.stdMode == 6) rate = std::max(rate, (r.synth.isdbtBwMhz >= 7 ? r.synth.isdbtBwMhz : 6) * 1e6 * 1.34);
    }
    if (lim.minHz > 0 && rate < lim.minHz) rate = lim.minHz;
    if (lim.maxHz > 0 && rate > lim.maxHz) rate = lim.maxHz;
    return rate;
}

// ---------------------------------------------------------------- the session

namespace {
constexpr size_t kBlock = 1 << 15;
std::string mhzText(double hz) { char b[48]; snprintf(b, sizeof b, "%.6f MHz", hz / 1e6); return b; }
}

void TxSession::note(const std::string& s) {
    std::function<void(const std::string&)> cb;
    std::string line;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const time_t now = time(nullptr);
        char b[32];
        strftime(b, sizeof b, "%H:%M:%S ", localtime(&now));
        line = std::string(b) + s;
        log_.push_back(line);
        if (log_.size() > 500) log_.erase(log_.begin(), log_.begin() + 100);
        cb = cb_;
    }
    if (cb) cb(line);
}

void TxSession::fail(const std::string& why) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (failed_) return;
        error_ = why;
        failed_ = true;
    }
    note("TRANSMISSION STOPPED: " + why);
}

bool TxSession::start(std::unique_ptr<TxSink> sink, const TxRequest& r, const TxRateLimits& lim, std::string& err) {
    stop();
    if (!sink) { err = "this radio has no transmit path in OnAir"; return false; }
    const TxModeSpec* mode = txModeById(r.modeId);
    if (!mode) { err = "this mode cannot be transmitted by OnAir"; return false; }
    sink_ = std::move(sink);
    req_ = r;
    carrierTune_ = r.carrierTune;
    audio_.reset();
    video_.reset();
    stop_ = false; genDone_ = false; failed_ = false; finished_ = false; sent_ = 0; under_ = 0; error_.clear();
    q_.clear();
    {
        std::lock_guard<std::mutex> lk(mu_);
        tap_.clear(); tapPos_ = 0; tapHave_ = 0;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        log_.clear();
    }
    TxTune t = r.tune;
    t.sampleRate = txRequestRate(*mode, r, lim);
    if (!sink_->open(t, err)) { note("could not start: " + err); sink_.reset(); return false; }
    rate_ = sink_->sampleRate() > 0 ? sink_->sampleRate() : t.sampleRate;
    if (req_.tune.signalHz != 0 && rate_ > 0 && std::fabs(req_.tune.signalHz) >= rate_ * 0.45) {
        char b[280];
        snprintf(b, sizeof b, "the signal offset of %.3f MHz does not fit at %.3f Msps: keep it inside %.3f MHz of the centre, or the edges of the signal alias",
                 req_.tune.signalHz / 1e6, rate_ / 1e6, rate_ * 0.45 / 1e6);
        err = b;
        note("could not start: " + err);
        sink_->close();
        sink_.reset();
        return false;
    }
    if (txModeNeedsAudio(*mode, req_) && !req_.carrierTune) {
        audio_ = std::make_unique<AudioIn>();
        AudioIn::Kind k = AudioIn::Tone;
        if (req_.audioSrc == 0) k = AudioIn::Mic;
        else if (req_.audioSrc == 1) k = AudioIn::File;
        else if (req_.audioSrc == 3) k = AudioIn::Silence;
        std::string aerr;
        const std::string arg = k == AudioIn::Mic ? req_.audioDevice : req_.file;
        if (!audio_->start(k, 48000, arg, req_.toneHz > 0 ? req_.toneHz : 1000, aerr)) {
            if (k == AudioIn::Mic) { err = aerr; note("could not start: " + err); sink_->close(); sink_.reset(); audio_.reset(); return false; }
            audio_->start(AudioIn::Tone, 48000, {}, req_.toneHz > 0 ? req_.toneHz : 1000, aerr);
        }
        audio_->setMuted(req_.audioMute);
        audio_->setGain(req_.micGain > 0 ? req_.micGain : 1.f);
        req_.pullAudio = [this](float* st, int n) { return audio_ ? audio_->read(st, n) : 0; };
    }
    if (!req_.carrierTune && mode->stdMode == 10 && req_.videoSrc > 0) {
        video_ = std::make_unique<VideoIn>();
        std::string verr;
        const VideoIn::Kind vk = req_.videoSrc == 2 ? VideoIn::Camera : VideoIn::File;
        const std::string varg = vk == VideoIn::Camera ? req_.videoDevice : req_.file;
        if (!video_->start(vk, varg, verr)) {
            if (vk == VideoIn::Camera) { err = verr; note("could not start: " + err); sink_->close(); sink_.reset(); audio_.reset(); video_.reset(); return false; }
            video_.reset();
            note("video file not used: " + verr);
        } else {
            req_.pullVideo = [this](int w, int h, float* y, float* u, float* v) { return video_ && video_->copyYuv(w, h, y, u, v); };
        }
    }
    gen_ = makeTxGenerator(*mode, req_, rate_, err);
    if (!gen_ && req_.carrierTune) { err.clear(); gen_.reset(); }
    if (!gen_ && !req_.carrierTune) {
        note("could not start: " + err);
        sink_->close(); sink_.reset();
        if (audio_) { audio_->stop(); audio_.reset(); }
        if (video_) { video_->stop(); video_.reset(); }
        return false;
    }
    gen_ = std::make_unique<CarrierGate>(std::move(gen_), &carrierTune_, rate_);
    maxQ_ = std::max<size_t>(8, (size_t)(rate_ * 0.25 / kBlock));
    running_ = true;
    started_ = true;
    char b[400];
    snprintf(b, sizeof b, "TRANSMITTING %s on %s, %.3f Msps, gain %.1f dB%s, %s", mode->name, mhzText(r.tune.centerHz).c_str(), rate_ / 1e6, r.tune.gainDb, r.tune.ampOn ? " + amplifier" : "",
             r.loop ? "until stopped" : (std::to_string(r.onceSeconds).substr(0, std::to_string(r.onceSeconds).find('.') + 2) + " s, one pass").c_str());
    note(b);
    if (req_.carrierTune) note("CARRIER TUNE (unmodulated, for adjusting the transmission — not a message)");
    if (req_.tune.signalHz != 0) {
        char sh[240];
        snprintf(sh, sizeof sh, "the modulated signal is %+.3f kHz from the radio centre, so it goes out at %.6f MHz; the radio's own spike stays at the centre",
                 req_.tune.signalHz / 1e3, (txRadioHz(r.tune) + req_.tune.signalHz) / 1e6);
        note(sh);
    }
    gen_t_ = std::thread([this] { generatorLoop(); });
    send_t_ = std::thread([this] { senderLoop(); });
    return true;
}

void TxSession::generatorLoop() {
    // OffsetMixer subtracts its argument. A positive signalHz moves the carrier up from DC, off the radio's own spike.
    OffsetMixer shift;
    shift.set(-req_.tune.signalHz, rate_);
    const uint64_t limit = req_.loop ? ~0ull : (uint64_t)(std::max(0.1, req_.onceSeconds) * rate_);
    uint64_t made = 0;
    while (!stop_ && made < limit) {
        const size_t n = (size_t)std::min<uint64_t>(kBlock, limit - made);
        std::vector<cf32> blk(n);
        gen_->generate(blk.data(), n);
        shift.mix(blk.data(), n);
        tapWrite(blk.data(), n);
        made += n;
        std::unique_lock<std::mutex> lk(mu_);
        cvIn_.wait(lk, [&] { return stop_ || q_.size() < maxQ_; });
        if (stop_) break;
        q_.push_back(std::move(blk));
        cvOut_.notify_one();
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        genDone_ = true;
    }
    cvOut_.notify_all();
}

void TxSession::senderLoop() {
    const bool rt = sink_->realtime();
    {   // fill the queue before the first sample goes out, so that the start is not an underrun
        std::unique_lock<std::mutex> lk(mu_);
        cvOut_.wait_for(lk, std::chrono::seconds(5), [&] { return stop_ || genDone_ || q_.size() >= std::min<size_t>(maxQ_ / 2 + 1, 4); });
    }
    uint64_t lastReported = 0;
    auto lastReport = std::chrono::steady_clock::now();
    while (!stop_) {
        std::vector<cf32> blk;
        bool underrun = false;
        {
            std::unique_lock<std::mutex> lk(mu_);
            for (;;) {
                if (stop_) break;
                if (!q_.empty()) { blk = std::move(q_.front()); q_.pop_front(); cvIn_.notify_one(); break; }
                if (genDone_) break;
                if (rt) { underrun = true; break; }   // the radio runs on: what it gets when nothing is ready is silence, counted
                cvOut_.wait_for(lk, std::chrono::milliseconds(5));
            }
        }
        if (stop_) break;
        if (blk.empty() && !underrun) break;   // the generator is done and the queue is empty: one pass finished
        if (underrun) { blk.assign(kBlock / 8, cf32(0.f, 0.f)); under_++; }
        std::string e;
        if (!sink_->write(blk.data(), blk.size(), e)) { fail(e); break; }
        if (!underrun) sent_ += blk.size();
        const auto now = std::chrono::steady_clock::now();
        if (under_ != lastReported && now - lastReport > std::chrono::seconds(2)) {
            note("the generator fell behind: " + std::to_string(under_.load() - lastReported) + " gap(s) of silence sent");
            lastReported = under_;
            lastReport = now;
        }
    }
    if (!stop_) {   // finished or failed on its own: the transmitter goes off now
        sink_->close();
        if (!failed_) {
            char b[160];
            snprintf(b, sizeof b, "transmission finished: %.1f s sent", (double)sent_ / rate_);
            note(b);
            std::lock_guard<std::mutex> lk(mu_);
            finished_ = true;
        }
        running_ = false;
    }
}

void TxSession::stop() {
    if (!started_) { sink_.reset(); return; }
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cvIn_.notify_all();
    cvOut_.notify_all();
    if (gen_t_.joinable()) gen_t_.join();
    if (send_t_.joinable()) send_t_.join();
    const bool wasRunning = running_.exchange(false);
    if (sink_) sink_->close();     // no sample goes out after this
    if (wasRunning && !failed_) {
        char b[160];
        snprintf(b, sizeof b, "transmission stopped: %.1f s sent, %llu gap(s) of silence", rate_ > 0 ? (double)sent_ / rate_ : 0.0, (unsigned long long)under_.load());
        note(b);
    }
    sink_.reset();
    gen_.reset();
    if (audio_) { audio_->stop(); audio_.reset(); }
    if (video_) { video_->stop(); video_.reset(); }
    carrierTune_ = false;
    started_ = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        tap_.clear(); tapPos_ = 0; tapHave_ = 0;
    }
}

void TxSession::setSpectrumTap(bool on) {
    const int want = on ? 1 : 2;
    const int prev = tapMode_.exchange(want, std::memory_order_acq_rel);
    if (on && prev != 1) {
        tapArm_.store(false, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(mu_);
        tap_.clear();
        tapPos_ = 0;
        tapHave_ = 0;
    }
}

void TxSession::armSpectrumTap() {
    tapMode_.store(1, std::memory_order_release);
    tapArm_.store(true, std::memory_order_release);
}

void TxSession::tapWrite(const cf32* x, size_t n) {
    if (!x || !n || rate_ <= 0) return;
    const int mode = tapMode_.load(std::memory_order_acquire);
    if (mode == 2) return;
    if (mode == 1) {
        if (!tapArm_.exchange(false, std::memory_order_acq_rel)) return;
        constexpr size_t kKeep = 2048;   // one spectrum FFT; the rest of the block is not copied
        const size_t use = std::min(n, kKeep);
        std::lock_guard<std::mutex> lk(mu_);
        if (tap_.size() != use) tap_.resize(use);
        std::memcpy(tap_.data(), x + (n - use), use * sizeof(cf32));
        tapPos_ = 0;
        tapHave_ = use;
        return;
    }
    const size_t cap = (size_t)std::max<double>(n, std::min(rate_ * 0.5, 1e6));
    std::lock_guard<std::mutex> lk(mu_);
    if (tap_.size() != cap) { tap_.assign(cap, cf32(0.f, 0.f)); tapPos_ = 0; tapHave_ = 0; }
    for (size_t i = 0; i < n; i++) {
        tap_[tapPos_] = x[i];
        tapPos_ = (tapPos_ + 1) % tap_.size();
        if (tapHave_ < tap_.size()) tapHave_++;
    }
}

void TxSession::copyTap(std::vector<cf32>& out, double& rateHz) const {
    std::lock_guard<std::mutex> lk(mu_);
    rateHz = rate_;
    if (!tapHave_ || tap_.empty()) { out.clear(); return; }
    out.resize(tapHave_);
    const size_t cap = tap_.size();
    size_t start = tapHave_ < cap ? 0 : tapPos_;
    for (size_t i = 0; i < tapHave_; i++) out[i] = tap_[(start + i) % cap];
}

void TxSession::setCarrierTune(bool on) {
    const bool was = carrierTune_.exchange(on);
    if (on && !was) note("CARRIER TUNE (unmodulated, for adjusting the transmission — not a message)");
    else if (!on && was) note("carrier tune off: payload resumed");
}

TxSession::Stats TxSession::stats() const {
    Stats s;
    s.samples = sent_;
    s.underruns = under_;
    s.rateHz = rate_;
    s.seconds = rate_ > 0 ? (double)sent_ / rate_ : 0;
    std::lock_guard<std::mutex> lk(mu_);
    s.failed = failed_;
    s.finished = finished_;
    s.error = error_;
    s.carrierTune = carrierTune_.load();
    s.audioPeak = audio_ ? audio_->peak() : 0.f;
    s.fileNote = req_.fileNote;
    return s;
}

std::vector<std::string> TxSession::log() const {
    std::lock_guard<std::mutex> lk(mu_);
    return log_;
}

} // namespace dect2
