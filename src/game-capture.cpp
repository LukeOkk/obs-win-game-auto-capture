// game-capture.cpp — Windows.Graphics.Capture (video) + WASAPI process
// loopback (audio) feeding an OBS async source. See game-capture.h.
//
// Video path mirrors the macOS plugin's "GPU frame -> CPU staging -> BGRA ->
// obs_source_output_video" approach. Audio uses per-process WASAPI loopback so
// only the captured game's sound enters OBS (background chat/browser audio is
// excluded), matching the macOS behavior. Audio requires Windows 10 2004+ and
// a Windows SDK that ships <audioclientactivationparams.h>; if unavailable the
// capture degrades gracefully to video-only.

#include "game-capture.h"
#include "plugin-log.h"

#include <util/platform.h>  // os_gettime_ns

// --- C++/WinRT + Direct3D (video) ---
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <inspectable.h>

// --- WASAPI (audio) ---
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#if __has_include(<audioclientactivationparams.h>)
#  include <audioclientactivationparams.h>
#  define HAVE_PROCESS_LOOPBACK 1
#else
#  define HAVE_PROCESS_LOOPBACK 0
#endif

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace wmeta = winrt::Windows::Foundation::Metadata;
namespace wf = winrt::Windows::Foundation;  // wf::IInspectable is the *projected* type
using winrt::com_ptr;
// NB: do NOT `using` the projected IInspectable — it collides with the ABI
// ::IInspectable pulled in by <inspectable.h> (error C2874).

// --------------------------------------------------------------------------
// Small helpers
// --------------------------------------------------------------------------

static std::string narrow(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

template <typename T>
static com_ptr<T> dxgi_interface_from(wf::IInspectable const &object) {
    auto access = object.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<T> result;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<T>(), result.put_void()));
    return result;
}

// --------------------------------------------------------------------------
// WASAPI per-process loopback
// --------------------------------------------------------------------------

#if HAVE_PROCESS_LOOPBACK
// Completion handler for ActivateAudioInterfaceAsync.
//
// IMPORTANT: the handler MUST be agile / free-threaded-marshalable, otherwise
// ActivateAudioInterfaceAsync fails with E_ILLEGAL_METHOD_CALL (0x8000000E).
// We implement IAgileObject and aggregate the free-threaded marshaler so we
// answer IMarshal too — this is what WRL's FtmBase does for the same purpose.
class ActivateHandler : public IActivateAudioInterfaceCompletionHandler,
                        public IAgileObject {
public:
    explicit ActivateHandler(HANDLE done) : m_done(done) {
        CoCreateFreeThreadedMarshaler(
            static_cast<IActivateAudioInterfaceCompletionHandler *>(this), m_ftm.put());
    }

    HRESULT activate_result = E_FAIL;
    com_ptr<IAudioClient> client;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IAgileObject)) {
            *ppv = static_cast<IAgileObject *>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IMarshal) && m_ftm) {
            return m_ftm->QueryInterface(riid, ppv);
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation *op) override {
        HRESULT hrActivate = E_FAIL;
        com_ptr<IUnknown> unk;
        HRESULT hr = op->GetActivateResult(&hrActivate, unk.put());
        activate_result = SUCCEEDED(hr) ? hrActivate : hr;
        if (SUCCEEDED(activate_result) && unk) {
            try { client = unk.as<IAudioClient>(); } catch (...) { activate_result = E_NOINTERFACE; }
        }
        SetEvent(m_done);
        return S_OK;
    }

private:
    LONG m_ref = 1;
    HANDLE m_done;
    com_ptr<IUnknown> m_ftm;  // free-threaded marshaler (agility)
};
#endif  // HAVE_PROCESS_LOOPBACK

class WasapiLoopback {
public:
    ~WasapiLoopback() { stop(); }

    bool start(DWORD pid, obs_source_t *source) {
#if !HAVE_PROCESS_LOOPBACK
        (void)pid; (void)source;
        return false;
#else
        stop();
        m_source = source;

        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!done) return false;

        AUDIOCLIENT_ACTIVATION_PARAMS params{};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = pid;
        params.ProcessLoopbackParams.ProcessLoopbackMode =
            PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT pv{};
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(params);
        pv.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

        ActivateHandler *raw = new ActivateHandler(done);
        com_ptr<IActivateAudioInterfaceCompletionHandler> handler;
        handler.attach(raw);  // takes the initial ref (ctor set ref=1)

        com_ptr<IActivateAudioInterfaceAsyncOperation> op;
        HRESULT hr = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
            &pv, handler.get(), op.put());
        if (FAILED(hr)) {
            CloseHandle(done);
            PLUGIN_LOG(LOG_INFO, "audio: ActivateAudioInterfaceAsync failed 0x%08lx", hr);
            return false;
        }

        WaitForSingleObject(done, 3000);
        CloseHandle(done);

        if (FAILED(raw->activate_result) || !raw->client) {
            PLUGIN_LOG(LOG_INFO, "audio: process loopback activation failed 0x%08lx",
                       raw->activate_result);
            return false;
        }
        m_client = raw->client;

        // Force a fixed 48 kHz stereo Float32 format; AUTOCONVERTPCM inserts a
        // resampler/format converter when the engine format differs.
        WAVEFORMATEX fmt{};
        fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = 48000;
        fmt.wBitsPerSample = 32;
        fmt.nBlockAlign = (WORD)(fmt.nChannels * fmt.wBitsPerSample / 8);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.cbSize = 0;

        const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK |
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        // Shared event-driven: try the engine period first (0), then fall back
        // to a 20 ms buffer (200000 * 100ns) if the driver rejects 0.
        hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &fmt, nullptr);
        if (FAILED(hr)) {
            hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &fmt, nullptr);
        }
        if (FAILED(hr)) {
            PLUGIN_LOG(LOG_INFO, "audio: IAudioClient::Initialize failed 0x%08lx", hr);
            m_client = nullptr;
            return false;
        }
        m_format = fmt;

        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_event || FAILED(m_client->SetEventHandle(m_event))) {
            m_client = nullptr;
            return false;
        }
        if (FAILED(m_client->GetService(__uuidof(IAudioCaptureClient), m_capture.put_void()))) {
            m_client = nullptr;
            return false;
        }
        if (FAILED(m_client->Start())) {
            m_client = nullptr;
            m_capture = nullptr;
            return false;
        }

        m_running = true;
        m_thread = std::thread(&WasapiLoopback::capture_loop, this);
        return true;
#endif
    }

    void stop() {
        m_running = false;
        if (m_event) SetEvent(m_event);
        if (m_thread.joinable()) m_thread.join();
        if (m_client) {
            m_client->Stop();
            m_client = nullptr;
        }
        m_capture = nullptr;
        if (m_event) {
            CloseHandle(m_event);
            m_event = nullptr;
        }
        m_source = nullptr;
    }

    bool running() const { return m_running.load(); }

private:
#if HAVE_PROCESS_LOOPBACK
    void capture_loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        // m_capture / m_format / m_silence are members (declared below).
        bool logged_first = false;
        while (m_running) {
            DWORD w = WaitForSingleObject(m_event, 200);
            if (!m_running) break;
            if (w != WAIT_OBJECT_0) continue;

            UINT32 packet = 0;
            while (SUCCEEDED(m_capture->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 pos = 0, qpc = 0;
                if (FAILED(m_capture->GetBuffer(&data, &frames, &flags, &pos, &qpc)))
                    break;

                if (frames > 0 && m_source) {
                    obs_source_audio a{};
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        m_silence.assign((size_t)frames * m_format.nChannels, 0.0f);
                        a.data[0] = reinterpret_cast<uint8_t *>(m_silence.data());
                    } else {
                        a.data[0] = data;
                    }
                    a.frames = frames;
                    a.speakers = (m_format.nChannels >= 2) ? SPEAKERS_STEREO : SPEAKERS_MONO;
                    a.format = AUDIO_FORMAT_FLOAT;  // interleaved float32
                    a.samples_per_sec = m_format.nSamplesPerSec;
                    a.timestamp = os_gettime_ns();
                    obs_source_output_audio(m_source, &a);

                    if (!logged_first) {
                        logged_first = true;
                        PLUGIN_LOG(LOG_INFO, "first audio frame: %u frames, %u ch @ %lu Hz",
                                   frames, m_format.nChannels, m_format.nSamplesPerSec);
                    }
                }
                m_capture->ReleaseBuffer(frames);
            }
        }
        CoUninitialize();
    }
#endif  // HAVE_PROCESS_LOOPBACK

    // Declared unconditionally so stop() compiles even without process-loopback
    // support (IAudioClient/IAudioCaptureClient live in <audioclient.h>, which
    // is always available; only the activation params header is gated).
    com_ptr<IAudioClient> m_client;
    com_ptr<IAudioCaptureClient> m_capture;
    WAVEFORMATEX m_format{};
    std::vector<float> m_silence;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    HANDLE m_event = nullptr;
    obs_source_t *m_source = nullptr;
};

// --------------------------------------------------------------------------
// GameCapture::Impl — WGC video + audio
// --------------------------------------------------------------------------

struct GameCapture::Impl {
    obs_source_t *source = nullptr;
    capture_settings settings;

    // D3D
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    wgd3d::IDirect3DDevice winrtDevice{nullptr};

    // WGC
    wgc::GraphicsCaptureItem item{nullptr};
    wgc::Direct3D11CaptureFramePool framePool{nullptr};
    wgc::GraphicsCaptureSession session{nullptr};
    winrt::event_token frameToken{};
    winrt::Windows::Graphics::SizeInt32 lastSize{};

    // CPU staging
    com_ptr<ID3D11Texture2D> staging;
    UINT stagingW = 0, stagingH = 0;
    std::mutex d3dMutex;

    HWND hwnd = nullptr;
    bool bound = false;
    DWORD targetPid = 0;

    WasapiLoopback audio;

    // Apply settings live. A change to capture_audio starts/stops the loopback
    // immediately (no rebind needed), so a source can be muted/unmuted on the
    // fly — handy when two sources capture the same game and you only want one
    // to carry audio (otherwise the game's audio sums twice in the mix).
    void update(const capture_settings &cs) {
        settings = cs;
        if (bound && targetPid) {
            if (cs.capture_audio && !audio.running()) {
                if (audio.start(targetPid, source))
                    PLUGIN_LOG(LOG_INFO, "audio loopback started (live)");
                else
                    PLUGIN_LOG(LOG_INFO, "audio loopback unavailable (live) — video only");
            } else if (!cs.capture_audio && audio.running()) {
                audio.stop();
                PLUGIN_LOG(LOG_INFO, "audio loopback stopped (capture_audio off)");
            }
        }
    }

    bool ensure_device() {
        if (device) return true;
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        D3D_FEATURE_LEVEL fl{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                       nullptr, 0, D3D11_SDK_VERSION,
                                       device.put(), &fl, context.put());
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                   nullptr, 0, D3D11_SDK_VERSION,
                                   device.put(), &fl, context.put());
        }
        if (FAILED(hr)) {
            PLUGIN_LOG(LOG_WARNING, "D3D11CreateDevice failed: 0x%08lx", hr);
            return false;
        }
        try {
            auto dxgiDevice = device.as<IDXGIDevice>();
            // NB: the interop factory takes the *ABI* ::IInspectable, not the
            // projected winrt::...::IInspectable.
            com_ptr<::IInspectable> insp;
            winrt::check_hresult(
                CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), insp.put()));
            winrtDevice = insp.as<wgd3d::IDirect3DDevice>();
        } catch (winrt::hresult_error const &e) {
            PLUGIN_LOG(LOG_WARNING, "wrap D3D device failed: 0x%08x", (unsigned)e.code());
            return false;
        }
        return true;
    }

    bool ensure_staging(UINT w, UINT h) {
        if (staging && stagingW == w && stagingH == h) return true;
        staging = nullptr;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        com_ptr<ID3D11Texture2D> t;
        if (FAILED(device->CreateTexture2D(&d, nullptr, t.put()))) return false;
        staging = t;
        stagingW = w;
        stagingH = h;
        return true;
    }

    void on_frame_arrived(wgc::Direct3D11CaptureFramePool const &sender, wf::IInspectable const &) {
        auto frame = sender.TryGetNextFrame();
        if (!frame) return;
        auto contentSize = frame.ContentSize();

        {
            std::lock_guard<std::mutex> lk(d3dMutex);
            if (!context) return;
            com_ptr<ID3D11Texture2D> tex;
            try {
                tex = dxgi_interface_from<ID3D11Texture2D>(frame.Surface());
            } catch (...) {
                return;
            }
            if (!tex) return;

            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);
            if (!ensure_staging(desc.Width, desc.Height)) return;

            context->CopyResource(staging.get(), tex.get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) {
                obs_source_frame f{};
                f.width = desc.Width;
                f.height = desc.Height;
                f.format = VIDEO_FORMAT_BGRA;
                f.full_range = true;
                f.linesize[0] = m.RowPitch;
                f.data[0] = reinterpret_cast<uint8_t *>(m.pData);
                f.timestamp = os_gettime_ns();
                obs_source_output_video(source, &f);
                context->Unmap(staging.get(), 0);
            }
        }

        // Resize the frame pool to follow the target window for next frames.
        if (contentSize.Width != lastSize.Width || contentSize.Height != lastSize.Height) {
            std::lock_guard<std::mutex> lk(d3dMutex);
            lastSize = contentSize;
            try {
                if (framePool)
                    framePool.Recreate(winrtDevice,
                                       wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                       2, contentSize);
            } catch (...) {
            }
        }
    }

    bool bind(HWND h, DWORD pid, const std::wstring &exe_name) {
        teardown();

        if (!wgc::GraphicsCaptureSession::IsSupported()) {
            PLUGIN_LOG(LOG_WARNING, "Windows.Graphics.Capture not supported on this OS");
            return false;
        }
        if (!ensure_device()) return false;

        try {
            auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem>()
                               .as<::IGraphicsCaptureItemInterop>();
            wgc::GraphicsCaptureItem it{nullptr};
            winrt::check_hresult(interop->CreateForWindow(
                h, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(it)));
            item = it;
            lastSize = item.Size();

            framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
                winrtDevice, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, lastSize);
            frameToken = framePool.FrameArrived({this, &Impl::on_frame_arrived});
            session = framePool.CreateCaptureSession(item);

            try {
                if (wmeta::ApiInformation::IsPropertyPresent(
                        L"Windows.Graphics.Capture.GraphicsCaptureSession",
                        L"IsCursorCaptureEnabled"))
                    session.IsCursorCaptureEnabled(settings.capture_cursor);
            } catch (...) {
            }
            try {
                if (wmeta::ApiInformation::IsPropertyPresent(
                        L"Windows.Graphics.Capture.GraphicsCaptureSession",
                        L"IsBorderRequired"))
                    session.IsBorderRequired(false);  // hide yellow border on Win11
            } catch (...) {
            }

            session.StartCapture();
        } catch (winrt::hresult_error const &e) {
            PLUGIN_LOG(LOG_WARNING, "WGC bind failed: 0x%08x", (unsigned)e.code());
            teardown();
            return false;
        }

        hwnd = h;
        bound = true;
        targetPid = pid;
        PLUGIN_LOG(LOG_INFO, "capturing window (pid=%lu, exe=%s)", pid,
                   narrow(exe_name).c_str());

        if (settings.capture_audio) {
            if (audio.start(pid, source))
                PLUGIN_LOG(LOG_INFO, "audio loopback started");
            else
                PLUGIN_LOG(LOG_INFO, "audio loopback unavailable — video only");
        }
        return true;
    }

    void teardown() {
        audio.stop();
        {
            std::lock_guard<std::mutex> lk(d3dMutex);
            if (framePool && frameToken.value) {
                try { framePool.FrameArrived(frameToken); } catch (...) {}
                frameToken = {};
            }
            if (session) {
                try { session.Close(); } catch (...) {}
                session = nullptr;
            }
            if (framePool) {
                try { framePool.Close(); } catch (...) {}
                framePool = nullptr;
            }
            item = nullptr;
            staging = nullptr;
            stagingW = stagingH = 0;
        }
        hwnd = nullptr;
        bound = false;
        targetPid = 0;
    }
};

// --------------------------------------------------------------------------
// GameCapture wrapper
// --------------------------------------------------------------------------

GameCapture::GameCapture(obs_source_t *source) : p_(std::make_unique<Impl>()) {
    p_->source = source;
}

GameCapture::~GameCapture() {
    if (p_) p_->teardown();
}

void GameCapture::update(const capture_settings &cs) { p_->update(cs); }

bool GameCapture::bind(HWND hwnd, DWORD pid, const std::wstring &exe_name) {
    return p_->bind(hwnd, pid, exe_name);
}

void GameCapture::teardown() { p_->teardown(); }

bool GameCapture::is_bound() const { return p_->bound; }

HWND GameCapture::current_hwnd() const { return p_->hwnd; }
