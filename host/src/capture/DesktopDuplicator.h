#pragma once

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

namespace dd::capture
{

struct PointerState
{
    bool visible = false;
    POINT position{};             // top-left of the shape, in output coordinates
    uint64_t shapeVersion = 0;    // bumps whenever `shape` changes
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo{};
    std::vector<uint8_t> shape;
};

struct Frame
{
    ID3D11Texture2D* desktop = nullptr; // valid until ReleaseFrame()
    int64_t presentQpc = 0;             // 0 if only the pointer changed
    bool desktopUpdated = false;
    bool pointerUpdated = false;
};

class DesktopDuplicator
{
  public:
    HRESULT Initialize(ID3D11Device* device, IDXGIOutput1* output);

    // S_OK with a frame; DXGI_ERROR_WAIT_TIMEOUT if nothing changed;
    // DXGI_ERROR_ACCESS_LOST (mode change, lock screen, UAC...) means re-Initialize.
    HRESULT AcquireFrame(UINT timeoutMs, Frame& frame);
    void ReleaseFrame();

    const PointerState& Pointer() const { return m_pointer; }
    DXGI_OUTDUPL_DESC Desc() const { return m_desc; }

  private:
    HRESULT UpdatePointer(const DXGI_OUTDUPL_FRAME_INFO& info);

    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_acquired;
    DXGI_OUTDUPL_DESC m_desc{};
    PointerState m_pointer;
    bool m_holdingFrame = false;
};

} // namespace dd::capture
