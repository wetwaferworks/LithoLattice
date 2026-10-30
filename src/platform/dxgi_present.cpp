// 2026-10-25 (box, DXGI present): see dxgi_present.h.
#include "dxgi_present.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cstring>
#include "litholattice/core/log.h"

namespace litholattice {
namespace {
template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
}  // namespace

struct cDxgiPresenter::Impl {
  HWND hwnd = nullptr;
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  IDXGISwapChain1* sc = nullptr;
  ID3D11Texture2D* upload = nullptr;  // dynamic, CPU-write
  int w = 0, h = 0;
  ~Impl() { rel(upload); rel(sc); rel(ctx); rel(dev); }

  bool resize(int nw, int nh) {
    rel(upload);
    if (!sc) {
      IDXGIDevice* dxdev = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* fac = nullptr;
      HRESULT hr = dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxdev));
      if (SUCCEEDED(hr)) hr = dxdev->GetAdapter(&ad);
      if (SUCCEEDED(hr)) hr = ad->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&fac));
      DXGI_SWAP_CHAIN_DESC1 d{};
      d.Width = UINT(nw); d.Height = UINT(nh);
      d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      d.SampleDesc.Count = 1;
      d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      d.BufferCount = 2;
      d.Scaling = DXGI_SCALING_STRETCH;  // frame is render-resolution; DWM stretches to the client
      d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
      d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
      if (SUCCEEDED(hr)) hr = fac->CreateSwapChainForHwnd(dev, hwnd, &d, nullptr, nullptr, &sc);
      if (fac && SUCCEEDED(hr)) fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
      rel(fac); rel(ad); rel(dxdev);
      if (FAILED(hr)) { rel(sc); w = h = 0; logError("dxgi present: swap chain creation failed (0x%08lx)", long(hr)); return false; }
    } else {
      const HRESULT hr = sc->ResizeBuffers(0, UINT(nw), UINT(nh), DXGI_FORMAT_UNKNOWN, 0);
      if (FAILED(hr)) { w = h = 0; logError("dxgi present: ResizeBuffers failed (0x%08lx)", long(hr)); return false; }
    }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(nw); td.Height = UINT(nh); td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &upload))) { upload = nullptr; w = h = 0; logError("dxgi present: upload texture failed"); return false; }
    w = nw; h = nh;
    return true;
  }
};

std::unique_ptr<cDxgiPresenter> cDxgiPresenter::create(void* hwnd) {
  auto p = std::unique_ptr<cDxgiPresenter>(new cDxgiPresenter());
  p->impl_ = std::make_unique<Impl>();
  p->impl_->hwnd = static_cast<HWND>(hwnd);
  const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2,
                                 D3D11_SDK_VERSION, &p->impl_->dev, nullptr, &p->impl_->ctx);
  if (FAILED(hr)) { logError("dxgi present: D3D11CreateDevice failed (0x%08lx); using GDI", long(hr)); return nullptr; }
  logInfo("software present: DXGI flip-discard (D3D11)");
  return p;
}
cDxgiPresenter::~cDxgiPresenter() = default;

bool cDxgiPresenter::present(const uint32_t* pixels, int w, int h, bool vsync) {
  Impl& m = *impl_;
  if (!pixels || w <= 0 || h <= 0) return false;
  if ((w != m.w || h != m.h || !m.sc) && !m.resize(w, h)) return false;
  D3D11_MAPPED_SUBRESOURCE ms{};
  if (FAILED(m.ctx->Map(m.upload, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return false;
  const size_t row = size_t(w) * 4;
  auto* dst = static_cast<uint8_t*>(ms.pData);
  const auto* src = reinterpret_cast<const uint8_t*>(pixels);
  if (ms.RowPitch == row) std::memcpy(dst, src, row * size_t(h));
  else for (int y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * ms.RowPitch, src + size_t(y) * row, row);
  m.ctx->Unmap(m.upload, 0);
  ID3D11Texture2D* back = nullptr;
  if (FAILED(m.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back)))) return false;
  m.ctx->CopyResource(back, m.upload);
  back->Release();
  const HRESULT hr = m.sc->Present(vsync ? 1 : 0, 0);
  return SUCCEEDED(hr) || hr == DXGI_STATUS_OCCLUDED;
}
}  // namespace litholattice
