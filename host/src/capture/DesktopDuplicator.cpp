#include "DesktopDuplicator.h"

using Microsoft::WRL::ComPtr;

namespace dd::capture
{

HRESULT DesktopDuplicator::Initialize(ID3D11Device* device, IDXGIOutput1* output)
{
    ReleaseFrame();
    m_duplication.Reset();
    m_pointer = {};

    // DuplicateOutput1 lets us ask for BGRA explicitly (and is required for some HDR paths).
    ComPtr<IDXGIOutput5> output5;
    HRESULT hr = E_NOINTERFACE;
    if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output5))))
    {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(device, 0, ARRAYSIZE(formats), formats, &m_duplication);
    }
    if (FAILED(hr))
    {
        hr = output->DuplicateOutput(device, &m_duplication);
    }
    if (FAILED(hr))
    {
        return hr;
    }
    m_duplication->GetDesc(&m_desc);
    return S_OK;
}

HRESULT DesktopDuplicator::AcquireFrame(UINT timeoutMs, Frame& frame)
{
    ReleaseFrame();
    frame = {};

    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource> resource;
    HRESULT hr = m_duplication->AcquireNextFrame(timeoutMs, &info, &resource);
    if (FAILED(hr))
    {
        return hr;
    }
    m_holdingFrame = true;

    hr = UpdatePointer(info);
    if (FAILED(hr))
    {
        return hr;
    }

    frame.desktopUpdated = info.LastPresentTime.QuadPart != 0 && info.AccumulatedFrames > 0;
    frame.pointerUpdated = info.LastMouseUpdateTime.QuadPart != 0;
    frame.presentQpc = info.LastPresentTime.QuadPart;

    hr = resource.As(&m_acquired);
    if (FAILED(hr))
    {
        return hr;
    }
    frame.desktop = m_acquired.Get();
    return S_OK;
}

void DesktopDuplicator::ReleaseFrame()
{
    m_acquired.Reset();
    if (m_holdingFrame && m_duplication)
    {
        m_duplication->ReleaseFrame();
    }
    m_holdingFrame = false;
}

HRESULT DesktopDuplicator::UpdatePointer(const DXGI_OUTDUPL_FRAME_INFO& info)
{
    if (info.LastMouseUpdateTime.QuadPart != 0)
    {
        m_pointer.visible = info.PointerPosition.Visible != FALSE;
        m_pointer.position = info.PointerPosition.Position;
    }

    if (info.PointerShapeBufferSize == 0)
    {
        return S_OK;
    }

    m_pointer.shape.resize(info.PointerShapeBufferSize);
    UINT required = 0;
    HRESULT hr = m_duplication->GetFramePointerShape(static_cast<UINT>(m_pointer.shape.size()), m_pointer.shape.data(),
                                                     &required, &m_pointer.shapeInfo);
    if (FAILED(hr))
    {
        m_pointer.shape.clear();
        return hr;
    }
    ++m_pointer.shapeVersion;
    return S_OK;
}

} // namespace dd::capture
