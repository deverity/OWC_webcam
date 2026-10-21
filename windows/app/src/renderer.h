// D3D11 renderer: flip-model swap chain (tearing when supported), NV12 -> RGB
// preview texture (YUV conversion in a pixel shader) and window screenshots.
#pragma once
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace owcapp {

struct YuvParams {
  bool bt709 = true;
  bool full_range = false;
};

// Small RGBA8 texture for ImGui::Image / ImDrawList::AddImage (scopes, LUT
// thumbnails). Recreated when the size changes. UI thread only (immediate context).
class ImageTexture {
 public:
  // rgba: w*h*4 bytes, tightly packed, straight alpha. Returns false on error.
  bool update(ID3D11Device* dev, ID3D11DeviceContext* ctx, const uint8_t* rgba, uint32_t w, uint32_t h);
  void reset();
  ID3D11ShaderResourceView* srv() const { return srv_.Get(); }
  bool valid() const { return srv_ != nullptr; }
  uint32_t width() const { return w_; }
  uint32_t height() const { return h_; }

 private:
  Microsoft::WRL::ComPtr<ID3D11Texture2D> tex_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_;
  uint32_t w_ = 0, h_ = 0;
};

class Renderer {
 public:
  bool init(HWND hwnd, int width, int height);
  void shutdown();
  void resize(int width, int height);

  ID3D11Device* device() const { return dev_.Get(); }
  ID3D11DeviceContext* context() const { return ctx_.Get(); }
  bool tearing() const { return tearing_; }
  const std::wstring& adapterName() const { return adapter_name_; }

  // Uploads a tightly packed NV12 frame (Y: w*h, UV: w*h/2 interleaved) and
  // converts it to the RGB preview texture. Returns false on error.
  bool uploadNv12(const uint8_t* y, const uint8_t* uv, uint32_t w, uint32_t h, const YuvParams& p);
  ID3D11ShaderResourceView* previewSrv() const { return rgb_srv_.Get(); }
  uint32_t previewWidth() const { return pw_; }
  uint32_t previewHeight() const { return ph_; }

  // Copies the converted RGB preview back to the CPU (RGBA8, tightly packed). For tests.
  bool readPreview(std::vector<uint8_t>& rgba) const;

  // Binds the back buffer and clears it.
  void beginRender(const float clear[4]);
  // Queues a PNG capture of the back buffer taken just before the next present().
  void requestScreenshot(const std::wstring& pngPath) { shot_path_ = pngPath; }
  bool lastScreenshotOk() const { return shot_ok_; }
  bool screenshotPending() const { return !shot_path_.empty(); }
  // Presents; handles the pending screenshot first. Returns false if the
  // device was lost.
  bool present();

 private:
  bool createTargets();
  bool createPreviewResources(uint32_t w, uint32_t h);
  bool saveBackBuffer(const std::wstring& path);

  Microsoft::WRL::ComPtr<ID3D11Device> dev_;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx_;
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv_;
  bool tearing_ = false;
  UINT swap_flags_ = 0;
  int bw_ = 0, bh_ = 0;
  std::wstring adapter_name_;

  // preview
  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
  Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
  Microsoft::WRL::ComPtr<ID3D11Buffer> cb_;
  Microsoft::WRL::ComPtr<ID3D11SamplerState> samp_;
  Microsoft::WRL::ComPtr<ID3D11RasterizerState> rs_;
  Microsoft::WRL::ComPtr<ID3D11BlendState> bs_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> tex_y_, tex_uv_, tex_rgb_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv_y_, srv_uv_, rgb_srv_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rgb_rtv_;
  uint32_t pw_ = 0, ph_ = 0;

  std::wstring shot_path_;
  bool shot_ok_ = false;
};

}  // namespace owcapp
