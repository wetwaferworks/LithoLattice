// Web-only GL import declarations.
//
// On the web build, the entry points the GL loader resolves are plain
// imports provided by the page layer (wasm/full/lkboot.js) under
// lk_gl<Name> names — one per entry in the loader's X-macro list, with
// the macro's own signatures, so a resolved pointer and a direct call
// are the same function. The declarations carry the compiler's import
// annotation (import_module "env") so the linker keeps them as module
// imports; no link-time JS library declares them. Window::
// glProcAddress (window_web.cpp) builds its name -> address table from
// the same macro; nothing else in the engine calls these directly (all
// GL flows through the litholattice_gl* pointer table, exactly as
// natively).
//
// Two names in the list have no WebGL2 existence (TexBuffer,
// DrawElementsBaseVertex): the address table answers null for them and
// the loader's fixups install the in-house shims (gl_web_shims.cpp),
// which is the resolution those names have always had on web.
//
// Guarded to the web target like lk_web.h: a native translation unit
// that includes it gets an empty header.
#pragma once

#if defined(__wasm__)

#include "gl_loader.h"

extern "C" {
#define LITHOLATTICE_GL_IMPORT(ret, name, args) \
  __attribute__((import_module("env"), import_name("lk_gl" #name))) ret lk_gl##name args;
LITHOLATTICE_GL_FUNCS(LITHOLATTICE_GL_IMPORT)
#undef LITHOLATTICE_GL_IMPORT
}

#endif  // defined(__wasm__)
