#pragma once

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dd::encode
{

struct EncoderSettings
{
    UINT width = 0;
    UINT height = 0;
    UINT fps = 60;
    UINT bitrateKbps = 30000;
    bool hevc = false;       // H.265 instead of H.264
    UINT qualityVsSpeed = 0; // CODECAPI_AVEncCommonQualityVsSpeed: 0 fastest .. 100 best quality
};

struct EncoderTiming
{
    uint64_t waitInputUs = 0;  // waiting for METransformNeedInput
    uint64_t processingUs = 0; // ProcessInput -> output collected
};

struct EncodedFrame
{
    std::vector<uint8_t> data; // Annex-B access unit
    uint64_t timestampUs = 0;
    bool keyframe = false;
};

class EventPump;

// Hardware H.264/HEVC encoder through Media Foundation (NVENC / Quick Sync / AMF behind one API),
// configured for low latency: no B-frames, CBR, CODECAPI_AVLowLatencyMode.
class MfH264Encoder
{
  public:
    MfH264Encoder();
    ~MfH264Encoder();
    MfH264Encoder(const MfH264Encoder&) = delete;
    MfH264Encoder& operator=(const MfH264Encoder&) = delete;

    // `device` must have been created with D3D11_CREATE_DEVICE_VIDEO_SUPPORT.
    HRESULT Initialize(ID3D11Device* device, LUID adapterLuid, const EncoderSettings& settings);
    void Shutdown();

    void RequestKeyframe() { m_forceKeyframe = true; }

    // Submits one NV12 frame and collects whatever output is ready (normally exactly this frame).
    HRESULT Encode(ID3D11Texture2D* nv12, uint64_t timestampUs, std::vector<EncodedFrame>& out);

    const std::wstring& Name() const { return m_name; }
    // What it was initialized with. Bitrate and quality are fixed for an encoder's lifetime:
    // hardware MFTs may accept changes through ICodecAPI yet ignore them (Intel's does).
    const EncoderSettings& Settings() const { return m_settings; }
    const EncoderTiming& LastTiming() const { return m_timing; }

  private:
    friend class EventPump;
    void OnEvent(MediaEventType type, HRESULT status);
    bool WaitFor(int& counter, DWORD timeoutMs);
    HRESULT ConfigureCodecApi(const EncoderSettings& settings);
    HRESULT SetMediaTypes(const EncoderSettings& settings);
    HRESULT CollectOutput(std::vector<EncodedFrame>& out);

    Microsoft::WRL::ComPtr<IMFActivate> m_activate;
    Microsoft::WRL::ComPtr<IMFTransform> m_transform;
    Microsoft::WRL::ComPtr<ICodecAPI> m_codecApi;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_deviceManager;
    Microsoft::WRL::ComPtr<EventPump> m_pump;
    std::wstring m_name;
    EncoderSettings m_settings;
    UINT m_fps = 60;
    EncoderTiming m_timing;
    bool m_forceKeyframe = true;
    bool m_started = false;
    bool m_mfStarted = false;

    std::mutex m_mutex;
    std::condition_variable m_cv;
    int m_needInput = 0;
    int m_haveOutput = 0;
    HRESULT m_error = S_OK;
};

} // namespace dd::encode
