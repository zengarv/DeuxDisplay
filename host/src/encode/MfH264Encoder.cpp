#include "MfH264Encoder.h"

#include <codecapi.h>
#include <mferror.h>
#include <wrl/implements.h>

#include "../common/Clock.h"
#include "../common/Log.h"

using Microsoft::WRL::ComPtr;

namespace dd::encode
{

// Re-arms BeginGetEvent after each event and forwards counts to the encoder. Stops when the
// MFT shuts down (EndGetEvent fails), which also breaks the generator <-> callback ref cycle.
class EventPump
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IMFAsyncCallback>
{
  public:
    EventPump(MfH264Encoder* owner, IMFMediaEventGenerator* generator) : m_owner(owner), m_generator(generator) {}

    HRESULT Start() { return m_generator->BeginGetEvent(this, nullptr); }
    void Detach()
    {
        std::lock_guard lock(m_ownerMutex);
        m_owner = nullptr;
    }

    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }

    STDMETHODIMP Invoke(IMFAsyncResult* result) override
    {
        ComPtr<IMFMediaEvent> event;
        HRESULT hr = m_generator->EndGetEvent(result, &event);
        if (FAILED(hr))
        {
            return S_OK; // shut down
        }
        MediaEventType type = MEUnknown;
        HRESULT status = S_OK;
        event->GetType(&type);
        event->GetStatus(&status);
        {
            std::lock_guard lock(m_ownerMutex);
            if (!m_owner)
            {
                return S_OK;
            }
            m_owner->OnEvent(type, status);
        }
        m_generator->BeginGetEvent(this, nullptr);
        return S_OK;
    }

  private:
    std::mutex m_ownerMutex;
    MfH264Encoder* m_owner;
    ComPtr<IMFMediaEventGenerator> m_generator;
};

namespace
{

HRESULT SetUint(ICodecAPI* api, const GUID& key, ULONG value)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    return api->SetValue(&key, &v);
}

HRESULT SetBool(ICodecAPI* api, const GUID& key, bool value)
{
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return api->SetValue(&key, &v);
}

} // namespace

MfH264Encoder::MfH264Encoder() = default;

MfH264Encoder::~MfH264Encoder()
{
    Shutdown();
}

void MfH264Encoder::OnEvent(MediaEventType type, HRESULT status)
{
    std::lock_guard lock(m_mutex);
    if (FAILED(status))
    {
        m_error = status;
    }
    else if (type == METransformNeedInput)
    {
        ++m_needInput;
    }
    else if (type == METransformHaveOutput)
    {
        ++m_haveOutput;
    }
    m_cv.notify_all();
}

bool MfH264Encoder::WaitFor(int& counter, DWORD timeoutMs)
{
    std::unique_lock lock(m_mutex);
    const bool ready = m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                     [&] { return counter > 0 || FAILED(m_error); });
    if (!ready || FAILED(m_error))
    {
        return false;
    }
    --counter;
    return true;
}

HRESULT MfH264Encoder::Initialize(ID3D11Device* device, LUID adapterLuid, const EncoderSettings& settings)
{
    Shutdown();
    m_fps = settings.fps;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr))
    {
        return hr;
    }
    m_mfStarted = true;

    // The encoder shares the capture device, so it must tolerate use from MF worker threads.
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread))))
    {
        multithread->SetMultithreadProtected(TRUE);
    }

    MFT_REGISTER_TYPE_INFO input{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output{MFMediaType_Video, settings.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264};
    ComPtr<IMFAttributes> enumAttributes;
    MFCreateAttributes(&enumAttributes, 1);
    enumAttributes->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&adapterLuid), sizeof(adapterLuid));

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    hr = MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &input, &output,
                  enumAttributes.Get(), &activates, &count);
    if (FAILED(hr))
    {
        return hr;
    }
    if (count == 0)
    {
        CoTaskMemFree(activates);
        return MF_E_TOPO_CODEC_NOT_FOUND;
    }
    m_activate = activates[0];
    for (UINT32 i = 0; i < count; ++i)
    {
        activates[i]->Release();
    }
    CoTaskMemFree(activates);

    wchar_t* name = nullptr;
    UINT32 nameLength = 0;
    if (SUCCEEDED(m_activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &nameLength)))
    {
        m_name = name;
        CoTaskMemFree(name);
    }

    hr = m_activate->ActivateObject(IID_PPV_ARGS(&m_transform));
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<IMFAttributes> attributes;
    hr = m_transform->GetAttributes(&attributes);
    if (FAILED(hr))
    {
        return hr;
    }
    attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
    attributes->SetUINT32(MF_LOW_LATENCY, TRUE);

    UINT resetToken = 0;
    hr = MFCreateDXGIDeviceManager(&resetToken, &m_deviceManager);
    if (FAILED(hr))
    {
        return hr;
    }
    hr = m_deviceManager->ResetDevice(device, resetToken);
    if (FAILED(hr))
    {
        return hr;
    }
    hr = m_transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_deviceManager.Get()));
    if (FAILED(hr))
    {
        return hr;
    }

    hr = m_transform.As(&m_codecApi);
    if (FAILED(hr))
    {
        return hr;
    }
    ConfigureCodecApi(settings);

    hr = SetMediaTypes(settings);
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<IMFMediaEventGenerator> generator;
    hr = m_transform.As(&generator);
    if (FAILED(hr))
    {
        return hr;
    }
    m_pump = Microsoft::WRL::Make<EventPump>(this, generator.Get());
    hr = m_pump->Start();
    if (FAILED(hr))
    {
        return hr;
    }

    m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_started = true;
    m_forceKeyframe = true;
    return S_OK;
}

HRESULT MfH264Encoder::ConfigureCodecApi(const EncoderSettings& settings)
{
    // Individual knobs are optional per vendor; log what didn't stick instead of failing.
    struct
    {
        const wchar_t* name;
        HRESULT hr;
    } results[] = {
        {L"LowLatencyMode", SetBool(m_codecApi.Get(), CODECAPI_AVLowLatencyMode, true)},
        {L"RateControlMode", SetUint(m_codecApi.Get(), CODECAPI_AVEncCommonRateControlMode,
                                     eAVEncCommonRateControlMode_CBR)},
        {L"MeanBitRate", SetUint(m_codecApi.Get(), CODECAPI_AVEncCommonMeanBitRate, settings.bitrateKbps * 1000)},
        {L"BPictureCount", SetUint(m_codecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0)},
        // One reference frame => SPS max_dec_frame_buffering 1. Decoders that size their output
        // queue from the DPB (MediaTek) otherwise hold frames back before displaying them.
        {L"MaxNumRefFrame", SetUint(m_codecApi.Get(), CODECAPI_AVEncVideoMaxNumRefFrame, 1)},
        // 0 = fastest preset; latency matters more than the last few percent of compression.
        {L"QualityVsSpeed", SetUint(m_codecApi.Get(), CODECAPI_AVEncCommonQualityVsSpeed, settings.qualityVsSpeed)},
        // Long GOP: TCP is lossless, so IDRs are only needed at start and on client request.
        {L"GOPSize", SetUint(m_codecApi.Get(), CODECAPI_AVEncMPVGOPSize, settings.fps * 60)},
    };
    for (const auto& r : results)
    {
        if (FAILED(r.hr))
        {
            Log(L"encoder: %s not supported (0x%08lX)", r.name, Hr(r.hr));
        }
    }
    return S_OK;
}

HRESULT MfH264Encoder::SetMediaTypes(const EncoderSettings& settings)
{
    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, settings.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, settings.bitrateKbps * 1000);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, settings.width, settings.height);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, settings.fps, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, settings.hevc ? static_cast<UINT32>(eAVEncH265VProfile_Main_420_8)
                                                      : static_cast<UINT32>(eAVEncH264VProfile_High));
    out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    HRESULT hr = m_transform->SetOutputType(0, out.Get(), 0);
    if (FAILED(hr))
    {
        Log(L"encoder: SetOutputType failed 0x%08lX", Hr(hr));
        return hr;
    }

    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, settings.width, settings.height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, settings.fps, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = m_transform->SetInputType(0, in.Get(), 0);
    if (FAILED(hr))
    {
        Log(L"encoder: SetInputType failed 0x%08lX", Hr(hr));
    }
    return hr;
}

HRESULT MfH264Encoder::Encode(ID3D11Texture2D* nv12, uint64_t timestampUs, std::vector<EncodedFrame>& out)
{
    out.clear();
    if (!m_started)
    {
        return E_UNEXPECTED;
    }

    // Collect anything left over from a previous call first, so output is never reordered.
    while (true)
    {
        {
            std::lock_guard lock(m_mutex);
            if (m_haveOutput == 0)
            {
                break;
            }
            --m_haveOutput;
        }
        HRESULT hr = CollectOutput(out);
        if (FAILED(hr))
        {
            return hr;
        }
    }

    const uint64_t waitStart = NowMicros();
    if (!WaitFor(m_needInput, 500))
    {
        return FAILED(m_error) ? m_error : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    const uint64_t inputReady = NowMicros();
    m_timing.waitInputUs = inputReady - waitStart;

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE, &buffer);
    if (FAILED(hr))
    {
        return hr;
    }
    ComPtr<IMF2DBuffer> buffer2d;
    DWORD length = 0;
    if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->GetContiguousLength(&length)))
    {
        buffer->SetCurrentLength(length);
    }

    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(static_cast<LONGLONG>(timestampUs) * 10);
    sample->SetSampleDuration(10'000'000 / m_fps);

    if (m_forceKeyframe)
    {
        SetUint(m_codecApi.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
        m_forceKeyframe = false;
    }

    hr = m_transform->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr))
    {
        return hr;
    }

    // Low-latency mode yields one output per input; wait briefly for it.
    if (WaitFor(m_haveOutput, 100))
    {
        hr = CollectOutput(out);
        m_timing.processingUs = NowMicros() - inputReady;
        return hr;
    }
    m_timing.processingUs = NowMicros() - inputReady;
    return FAILED(m_error) ? m_error : S_OK;
}

HRESULT MfH264Encoder::CollectOutput(std::vector<EncodedFrame>& out)
{
    MFT_OUTPUT_STREAM_INFO info{};
    m_transform->GetOutputStreamInfo(0, &info);

    ComPtr<IMFSample> provided;
    if (!(info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)))
    {
        ComPtr<IMFMediaBuffer> buffer;
        MFCreateMemoryBuffer(info.cbSize ? info.cbSize : 4 * 1024 * 1024, &buffer);
        MFCreateSample(&provided);
        provided->AddBuffer(buffer.Get());
    }

    MFT_OUTPUT_DATA_BUFFER data{};
    data.pSample = provided.Get();
    DWORD status = 0;
    HRESULT hr = m_transform->ProcessOutput(0, 1, &data, &status);
    if (data.pEvents)
    {
        data.pEvents->Release();
    }

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
    {
        ComPtr<IMFMediaType> type;
        if (SUCCEEDED(m_transform->GetOutputAvailableType(0, 0, &type)))
        {
            m_transform->SetOutputType(0, type.Get(), 0);
        }
        return S_OK;
    }
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
    {
        return S_OK;
    }
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<IMFSample> sample;
    sample.Attach(data.pSample);
    if (provided)
    {
        sample->AddRef(); // we still hold `provided`, and Attach took the caller's reference
    }

    ComPtr<IMFMediaBuffer> contiguous;
    hr = sample->ConvertToContiguousBuffer(&contiguous);
    if (FAILED(hr))
    {
        return hr;
    }
    BYTE* bytes = nullptr;
    DWORD size = 0;
    hr = contiguous->Lock(&bytes, nullptr, &size);
    if (FAILED(hr))
    {
        return hr;
    }

    EncodedFrame frame;
    frame.data.assign(bytes, bytes + size);
    contiguous->Unlock();

    LONGLONG time = 0;
    sample->GetSampleTime(&time);
    frame.timestampUs = static_cast<uint64_t>(time / 10);
    frame.keyframe = MFGetAttributeUINT32(sample.Get(), MFSampleExtension_CleanPoint, FALSE) != FALSE;
    out.push_back(std::move(frame));
    return S_OK;
}

void MfH264Encoder::Shutdown()
{
    if (m_pump)
    {
        m_pump->Detach();
    }
    if (m_transform)
    {
        if (m_started)
        {
            m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        ComPtr<IMFShutdown> shutdown;
        if (SUCCEEDED(m_transform.As(&shutdown)))
        {
            shutdown->Shutdown();
        }
    }
    if (m_activate)
    {
        m_activate->ShutdownObject();
    }
    m_pump.Reset();
    m_codecApi.Reset();
    m_transform.Reset();
    m_activate.Reset();
    m_deviceManager.Reset();
    m_started = false;
    m_needInput = 0;
    m_haveOutput = 0;
    m_error = S_OK;
    if (m_mfStarted)
    {
        MFShutdown();
        m_mfStarted = false;
    }
}

} // namespace dd::encode
