#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

#include "../capture/DesktopDuplicator.h"

namespace dd::render
{

// Turns a captured BGRA desktop frame into an NV12 texture for the encoder, drawing the
// pointer on top (Desktop Duplication frames never contain it). All work stays on the GPU.
class FrameComposer
{
  public:
    // Desktop frames are width x height (scan-out orientation); the NV12 output (what gets
    // encoded) is outWidth x outHeight. `rotation` is the clockwise angle the client applies to
    // show frames upright (see Rotation.h); it's used to place the pointer. `fullRange`: NV12 in
    // 0-255 instead of 16-235 (BT.709 either way).
    HRESULT Initialize(ID3D11Device* device, UINT width, UINT height, UINT outWidth, UINT outHeight,
                       uint16_t rotation = 0, bool fullRange = false);

    // Returns an NV12 texture from a small ring; it stays valid until kRingSize more calls.
    HRESULT Compose(ID3D11Texture2D* desktop, const capture::PointerState& pointer, ID3D11Texture2D** nv12);

    // Re-converts the last composed image (e.g. to answer a keyframe request on a static desktop).
    HRESULT ConvertLast(ID3D11Texture2D** nv12);

    static constexpr size_t kRingSize = 3;

  private:
    HRESULT InitPointerPipeline();
    HRESULT UpdatePointerTexture(const capture::PointerState& pointer);
    void DrawPointer(const capture::PointerState& pointer);
    HRESULT InitVideoProcessor();

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    UINT m_width = 0;
    UINT m_height = 0;
    UINT m_outWidth = 0;
    UINT m_outHeight = 0;
    uint16_t m_rotation = 0;
    bool m_fullRange = false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_compose;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_composeRtv;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_rectBuffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blend;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizer;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_pointerTexture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_pointerSrv;
    UINT m_pointerWidth = 0;
    UINT m_pointerHeight = 0;
    uint64_t m_pointerVersion = 0;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_vpEnum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_vp;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_vpInput;
    std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, kRingSize> m_nv12;
    std::array<Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>, kRingSize> m_vpOutput;
    size_t m_next = 0;
};

} // namespace dd::render
