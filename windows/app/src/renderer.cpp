#include "renderer.h"

#include <d3dcompiler.h>
#include <wincodec.h>

#include <algorithm>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace owcapp {

namespace {

const char* kShader = R"(
cbuffer Params : register(b0) {
  float4 r0;   // rgb.r = dot(r0.xyz, (Y - off.x, Cb - off.y, Cr - off.z))
  float4 r1;
  float4 r2;
  float4 off;
};
Texture2D texY : register(t0);
Texture2D texUV : register(t1);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
  VSOut o;
  float2 p = float2((id << 1) & 2, id & 2);
  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
  o.uv = p;
  return o;
}
float4 PSMain(VSOut i) : SV_Target {
  float y = texY.Sample(samp, i.uv).r;
  float2 c = texUV.Sample(samp, i.uv).rg;
  float3 v = float3(y, c.x, c.y) - off.xyz;
  float3 rgb = float3(dot(r0.xyz, v), dot(r1.xyz, v), dot(r2.xyz, v));
  return float4(saturate(rgb), 1);
}
)";

struct CBData {
  float r0[4], r1[4], r2[4], off[4];
};

CBData makeCb(const YuvParams& p) {
  const float kr = p.bt709 ? 0.2126f : 0.299f;
  const float kb = p.bt709 ? 0.0722f : 0.114f;
  const float kg = 1.0f - kr - kb;
  const float ys = p.full_range ? 1.0f : 255.0f / 219.0f;
  const float cs = p.full_range ? 1.0f : 255.0f / 224.0f;
  const float yoff = p.full_range ? 0.0f : 16.0f / 255.0f;
  CBData d{};
  // R = Y + 2(1-kr) Cr ; B = Y + 2(1-kb) Cb ; G = Y - (2kb(1-kb)/kg) Cb - (2kr(1-kr)/kg) Cr
  d.r0[0] = ys; d.r0[1] = 0;                          d.r0[2] = 2 * (1 - kr) * cs;
  d.r1[0] = ys; d.r1[1] = -(2 * kb * (1 - kb) / kg) * cs; d.r1[2] = -(2 * kr * (1 - kr) / kg) * cs;
  d.r2[0] = ys; d.r2[1] = 2 * (1 - kb) * cs;          d.r2[2] = 0;
  d.off[0] = yoff; d.off[1] = 128.0f / 255.0f; d.off[2] = 128.0f / 255.0f;
  return d;
}

}  // namespace

bool Renderer::init(HWND hwnd, int width, int height) {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  const D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                   D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got{};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, fls,
                                 static_cast<UINT>(std::size(fls)), D3D11_SDK_VERSION, &dev_, &got, &ctx_);
  if (FAILED(hr))
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, fls, static_cast<UINT>(std::size(fls)),
                           D3D11_SDK_VERSION, &dev_, &got, &ctx_);
  if (FAILED(hr)) return false;

  ComPtr<IDXGIDevice> dxgiDev;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(dev_.As(&dxgiDev)) || FAILED(dxgiDev->GetAdapter(&adapter)) ||
      FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))))
    return false;
  DXGI_ADAPTER_DESC ad{};
  if (SUCCEEDED(adapter->GetDesc(&ad))) adapter_name_ = ad.Description;

  ComPtr<IDXGIFactory5> f5;
  if (SUCCEEDED(factory.As(&f5))) {
    BOOL allow = FALSE;
    if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) && allow)
      tearing_ = true;
  }
  swap_flags_ = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = static_cast<UINT>(std::max(width, 8));
  sd.Height = static_cast<UINT>(std::max(height, 8));
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 4;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Scaling = DXGI_SCALING_STRETCH;
  sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  sd.Flags = swap_flags_;
  // NOTE: no IDXGISwapChain2::SetMaximumFrameLatency(1): it makes Present(0) block on DWM.
  if (FAILED(factory->CreateSwapChainForHwnd(dev_.Get(), hwnd, &sd, nullptr, nullptr, &swap_))) return false;
  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  bw_ = static_cast<int>(sd.Width);
  bh_ = static_cast<int>(sd.Height);
  if (!createTargets()) return false;

  // preview pipeline
  ComPtr<ID3DBlob> vsb, psb, err;
  if (FAILED(D3DCompile(kShader, std::strlen(kShader), "owc_yuv", nullptr, nullptr, "VSMain", "vs_4_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err)))
    return false;
  if (FAILED(D3DCompile(kShader, std::strlen(kShader), "owc_yuv", nullptr, nullptr, "PSMain", "ps_4_0",
                        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err)))
    return false;
  dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
  dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = sizeof(CBData);
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  dev_->CreateBuffer(&bd, nullptr, &cb_);
  D3D11_SAMPLER_DESC smd{};
  smd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  smd.AddressU = smd.AddressV = smd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  dev_->CreateSamplerState(&smd, &samp_);
  D3D11_RASTERIZER_DESC rd{};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  dev_->CreateRasterizerState(&rd, &rs_);
  D3D11_BLEND_DESC bld{};
  bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  dev_->CreateBlendState(&bld, &bs_);
  return vs_ && ps_ && cb_ && samp_ && rs_ && bs_;
}

void Renderer::shutdown() {
  if (ctx_) ctx_->ClearState();
  rtv_.Reset();
  rgb_rtv_.Reset();
  rgb_srv_.Reset(); srv_y_.Reset(); srv_uv_.Reset();
  tex_y_.Reset(); tex_uv_.Reset(); tex_rgb_.Reset();
  vs_.Reset(); ps_.Reset(); cb_.Reset(); samp_.Reset(); rs_.Reset(); bs_.Reset();
  swap_.Reset();
  ctx_.Reset();
  dev_.Reset();
}

bool Renderer::createTargets() {
  rtv_.Reset();
  ComPtr<ID3D11Texture2D> bb;
  if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
  return SUCCEEDED(dev_->CreateRenderTargetView(bb.Get(), nullptr, &rtv_));
}

void Renderer::resize(int width, int height) {
  if (!swap_ || width <= 0 || height <= 0) return;
  if (width == bw_ && height == bh_) return;
  ctx_->OMSetRenderTargets(0, nullptr, nullptr);
  rtv_.Reset();
  if (SUCCEEDED(swap_->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN,
                                     swap_flags_))) {
    bw_ = width;
    bh_ = height;
    createTargets();
  }
}

bool Renderer::createPreviewResources(uint32_t w, uint32_t h) {
  srv_y_.Reset(); srv_uv_.Reset(); rgb_srv_.Reset(); rgb_rtv_.Reset();
  tex_y_.Reset(); tex_uv_.Reset(); tex_rgb_.Reset();
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DYNAMIC;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &tex_y_))) return false;
  d.Width = w / 2; d.Height = h / 2;
  d.Format = DXGI_FORMAT_R8G8_UNORM;
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &tex_uv_))) return false;
  d.Width = w; d.Height = h;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  d.CPUAccessFlags = 0;
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &tex_rgb_))) return false;
  if (FAILED(dev_->CreateShaderResourceView(tex_y_.Get(), nullptr, &srv_y_))) return false;
  if (FAILED(dev_->CreateShaderResourceView(tex_uv_.Get(), nullptr, &srv_uv_))) return false;
  if (FAILED(dev_->CreateShaderResourceView(tex_rgb_.Get(), nullptr, &rgb_srv_))) return false;
  if (FAILED(dev_->CreateRenderTargetView(tex_rgb_.Get(), nullptr, &rgb_rtv_))) return false;
  pw_ = w;
  ph_ = h;
  return true;
}

bool Renderer::uploadNv12(const uint8_t* y, const uint8_t* uv, uint32_t w, uint32_t h, const YuvParams& p) {
  if (!dev_ || w < 2 || h < 2) return false;
  w &= ~1u;
  h &= ~1u;
  if (w != pw_ || h != ph_ || !tex_y_) {
    if (!createPreviewResources(w, h)) { pw_ = ph_ = 0; return false; }
  }
  D3D11_MAPPED_SUBRESOURCE m{};
  if (SUCCEEDED(ctx_->Map(tex_y_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    for (uint32_t r = 0; r < h; ++r)
      std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(r) * m.RowPitch, y + static_cast<size_t>(r) * w, w);
    ctx_->Unmap(tex_y_.Get(), 0);
  }
  if (SUCCEEDED(ctx_->Map(tex_uv_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    for (uint32_t r = 0; r < h / 2; ++r)
      std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(r) * m.RowPitch, uv + static_cast<size_t>(r) * w, w);
    ctx_->Unmap(tex_uv_.Get(), 0);
  }
  CBData cb = makeCb(p);
  ctx_->UpdateSubresource(cb_.Get(), 0, nullptr, &cb, 0, 0);

  ID3D11RenderTargetView* rt = rgb_rtv_.Get();
  ctx_->OMSetRenderTargets(1, &rt, nullptr);
  D3D11_VIEWPORT vp{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
  ctx_->RSSetViewports(1, &vp);
  ctx_->RSSetState(rs_.Get());
  const float bf[4] = {0, 0, 0, 0};
  ctx_->OMSetBlendState(bs_.Get(), bf, 0xffffffff);
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx_->IASetInputLayout(nullptr);
  ctx_->VSSetShader(vs_.Get(), nullptr, 0);
  ctx_->PSSetShader(ps_.Get(), nullptr, 0);
  ID3D11Buffer* b = cb_.Get();
  ctx_->PSSetConstantBuffers(0, 1, &b);
  ID3D11ShaderResourceView* srvs[2] = {srv_y_.Get(), srv_uv_.Get()};
  ctx_->PSSetShaderResources(0, 2, srvs);
  ID3D11SamplerState* ss = samp_.Get();
  ctx_->PSSetSamplers(0, 1, &ss);
  ctx_->Draw(3, 0);
  ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
  ctx_->PSSetShaderResources(0, 2, none);
  return true;
}

bool Renderer::readPreview(std::vector<uint8_t>& rgba) const {
  if (!tex_rgb_) return false;
  D3D11_TEXTURE2D_DESC d{};
  tex_rgb_->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> st;
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &st))) return false;
  ctx_->CopyResource(st.Get(), tex_rgb_.Get());
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;
  rgba.resize(static_cast<size_t>(d.Width) * d.Height * 4);
  for (UINT r = 0; r < d.Height; ++r)
    std::memcpy(rgba.data() + static_cast<size_t>(r) * d.Width * 4, static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(r) * m.RowPitch,
                static_cast<size_t>(d.Width) * 4);
  ctx_->Unmap(st.Get(), 0);
  return true;
}

void Renderer::beginRender(const float clear[4]) {
  ID3D11RenderTargetView* rt = rtv_.Get();
  ctx_->OMSetRenderTargets(1, &rt, nullptr);
  ctx_->ClearRenderTargetView(rt, clear);
}

bool Renderer::present() {
  if (!swap_) return false;
  if (!shot_path_.empty()) {
    shot_ok_ = saveBackBuffer(shot_path_);
    shot_path_.clear();
  }
  HRESULT hr = swap_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) return false;
  return true;
}

bool Renderer::saveBackBuffer(const std::wstring& path) {
  ComPtr<ID3D11Texture2D> bb;
  if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
  D3D11_TEXTURE2D_DESC d{};
  bb->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> st;
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &st))) return false;
  ctx_->CopyResource(st.Get(), bb.Get());
  D3D11_MAPPED_SUBRESOURCE m{};
  if (FAILED(ctx_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return false;

  bool ok = false;
  ComPtr<IWICImagingFactory> wic;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (SUCCEEDED(wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
        SUCCEEDED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&frame, &props)) && SUCCEEDED(frame->Initialize(props.Get())) &&
        SUCCEEDED(frame->SetSize(d.Width, d.Height))) {
      WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
      frame->SetPixelFormat(&fmt);
      if (fmt == GUID_WICPixelFormat32bppBGRA) {
        // Back buffer is RGBA: swap to BGRA and force opaque alpha (the swap chain ignores it).
        std::vector<uint8_t> px(static_cast<size_t>(d.Width) * d.Height * 4);
        for (UINT r = 0; r < d.Height; ++r) {
          const uint8_t* src = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(r) * m.RowPitch;
          uint8_t* dst = px.data() + static_cast<size_t>(r) * d.Width * 4;
          for (UINT x = 0; x < d.Width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = 255;
          }
        }
        if (SUCCEEDED(frame->WritePixels(d.Height, d.Width * 4, static_cast<UINT>(px.size()), px.data())) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit()))
          ok = true;
      }
    }
  }
  ctx_->Unmap(st.Get(), 0);
  return ok;
}

}  // namespace owcapp
