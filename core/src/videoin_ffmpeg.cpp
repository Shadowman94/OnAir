// Cameras / capture cards through FFmpeg libavdevice (v4l2, dshow). Used off macOS.
// On Windows the list is the DirectShow video-capture category, which is where virtual cameras
// (OBS Virtual Camera and the like) are registered. Media Foundation does not see those.
#include "videoin_impl.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <dshow.h>
#include <chrono>
#include <mutex>
#else
#include <dirent.h>
#endif

namespace dect2 {

namespace {
const char* inputFmt() {
#ifdef _WIN32
    return "dshow";
#else
    return "v4l2";
#endif
}

bool openCam(VideoIn::Impl* I, const std::string& url, std::string& err) {
    avdevice_register_all();
    const AVInputFormat* inf = av_find_input_format(inputFmt());
    if (!inf) { err = "this build has no capture-device support (libavdevice)"; return false; }
    // A real webcam takes 640x480 at 25. A virtual camera (OBS) often only offers its own size and refuses the request,
    // so the second try opens whatever the device is already producing.
    auto tryOpen = [&](bool sized) -> AVFormatContext* {
        AVDictionary* opt = nullptr;
        av_dict_set(&opt, "rtbufsize", "10485760", 0);
        if (sized) {
            av_dict_set(&opt, "framerate", "25", 0);
            av_dict_set(&opt, "video_size", "640x480", 0);
        }
        AVFormatContext* fmt = nullptr;
        const int r = avformat_open_input(&fmt, url.c_str(), inf, &opt);
        av_dict_free(&opt);
        return r >= 0 ? fmt : nullptr;
    };
    AVFormatContext* fmt = tryOpen(true);
    if (!fmt) fmt = tryOpen(false);
    if (!fmt) { err = "could not open this capture device"; return false; }
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); err = "this capture device sent no video"; return false; }
    const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (si < 0) { avformat_close_input(&fmt); err = "this capture device has no video"; return false; }
    AVStream* st = fmt->streams[si];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0 || avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx); avformat_close_input(&fmt); err = "could not decode this capture device"; return false;
    }
    I->fileTh = std::thread([=]() mutable {
        SwsContext* sws = nullptr;
        AVFrame* fr = av_frame_alloc();
        AVPacket* pk = av_packet_alloc();
        std::vector<uint8_t> rgb;
        while (!I->stop.load()) {
            if (av_read_frame(fmt, pk) < 0) break;
            if (pk->stream_index != si) { av_packet_unref(pk); continue; }
            if (avcodec_send_packet(ctx, pk) >= 0) {
                while (avcodec_receive_frame(ctx, fr) >= 0) {
                    if (!sws) sws = sws_getContext(fr->width, fr->height, (AVPixelFormat)fr->format, fr->width, fr->height, AV_PIX_FMT_RGB24, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                    rgb.resize((size_t)fr->width * (size_t)fr->height * 3);
                    uint8_t* dst[4] = {rgb.data(), nullptr, nullptr, nullptr};
                    int ls[4] = {fr->width * 3, 0, 0, 0};
                    if (sws) { sws_scale(sws, fr->data, fr->linesize, 0, fr->height, dst, ls); I->setRgb(fr->width, fr->height, rgb.data()); }
                }
            }
            av_packet_unref(pk);
        }
        if (sws) sws_freeContext(sws);
        av_frame_free(&fr); av_packet_free(&pk);
        avcodec_free_context(&ctx); avformat_close_input(&fmt);
    });
    return true;
}
}

bool videoInCameraStart(VideoIn::Impl* I, const std::string& deviceId, std::string& err) {
#ifdef _WIN32
    std::string url = deviceId.empty() ? "video=0" : (deviceId.find('=') == std::string::npos ? "video=" + deviceId : deviceId);
#else
    std::string url = deviceId.empty() ? "/dev/video0" : deviceId;
#endif
    return openCam(I, url, err);
}

void videoInCameraStop(VideoIn::Impl* I) {
    I->stop = true;
    if (I->fileTh.joinable()) I->fileTh.join();
    I->backend = nullptr;
}

#ifdef _WIN32
static std::string utf8FromWide(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// DirectShow video-capture devices, by the friendly name FFmpeg opens as video=<name>.
// That category includes virtual cameras. Binding the moniker would start the device, so only the name is read.
static bool enumDshow(std::vector<VideoInputDevice>& out) {
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(ci) && ci != RPC_E_CHANGED_MODE) return false;
    const bool uninit = ci == S_OK || ci == S_FALSE;
    ICreateDevEnum* devenum = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_ICreateDevEnum, (void**)&devenum);
    if (FAILED(hr) || !devenum) { if (uninit) CoUninitialize(); return false; }
    IEnumMoniker* en = nullptr;
    hr = devenum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &en, 0);
    devenum->Release();
    if (hr != S_OK || !en) { if (uninit) CoUninitialize(); return true; }
    IMoniker* mon = nullptr;
    while (en->Next(1, &mon, nullptr) == S_OK) {
        IPropertyBag* bag = nullptr;
        if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_IPropertyBag, (void**)&bag)) && bag) {
            VARIANT name;
            VariantInit(&name);
            if (SUCCEEDED(bag->Read(L"FriendlyName", &name, nullptr)) && name.vt == VT_BSTR) {
                const std::string n = utf8FromWide(name.bstrVal);
                bool seen = n.empty();
                for (const auto& d : out) if (d.id == n) seen = true;
                if (!seen) out.push_back({n, n});
            }
            VariantClear(&name);
            bag->Release();
        }
        mon->Release();
    }
    en->Release();
    if (uninit) CoUninitialize();
    return true;
}
#endif

std::vector<VideoInputDevice> videoInListCameras() {
#ifdef _WIN32
    static std::mutex mu;
    static std::vector<VideoInputDevice> cache;
    static std::chrono::steady_clock::time_point when{};
    {
        std::lock_guard<std::mutex> lk(mu);
        if (when.time_since_epoch().count() != 0 && std::chrono::steady_clock::now() - when < std::chrono::seconds(2)) return cache;
    }
    std::vector<VideoInputDevice> fresh;
    if (!enumDshow(fresh)) fresh.push_back({"", "Default capture device"});
    std::lock_guard<std::mutex> lk(mu);
    cache = std::move(fresh);
    when = std::chrono::steady_clock::now();
    return cache;
#else
    std::vector<VideoInputDevice> v;
    DIR* d = opendir("/dev");
    if (d) {
        while (dirent* e = readdir(d)) {
            if (std::strncmp(e->d_name, "video", 5) != 0) continue;
            std::string p = std::string("/dev/") + e->d_name;
            v.push_back({p, p});
        }
        closedir(d);
    }
    if (v.empty()) v.push_back({"", "Default capture device"});
    return v;
#endif
}

} // namespace dect2
