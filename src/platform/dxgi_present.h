// 2026-10-25 (box, DXGI present): minimal D3D11 + DXGI flip-model present for the software
// renderer's BGRA frame (default since 2026-10-25; LL_SW_PRESENT=gdi forces GDI StretchDIBits, which is also the automatic fallback).
#pragma once
#include <cstdint>
#include <memory>
namespace litholattice {
class cDxgiPresenter {
 public:
  /// nullptr if D3D11/DXGI is unavailable (caller falls back to GDI). hwnd is an HWND.
  static std::unique_ptr<cDxgiPresenter> create(void* hwnd);
  ~cDxgiPresenter();
  /// Uploads w x h 0xAARRGGBB top-row-first pixels and presents (sync interval 1 or 0);
  /// DWM stretches to the client area. False on failure (caller drops to GDI).
  bool present(const uint32_t* pixels, int w, int h, bool vsync);
  struct Impl;
 private:
  std::unique_ptr<Impl> impl_;
};
}  // namespace litholattice
