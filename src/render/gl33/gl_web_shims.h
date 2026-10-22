// Web-only GL loader fixups (Emscripten). Declared for gl_loader.cpp; the
// implementation TU is empty on every non-Emscripten build.
#pragma once

/// After a vcLoadGL pass that reported missing entry points, install logged
/// no-op shims for the entry points that cannot exist in WebGL2 (GLES 3.0)
/// and report whether the full table is now non-null. WebGL2 has no buffer
/// textures (glTexBuffer is GLES 3.1) and no core base-vertex draw
/// (glDrawElementsBaseVertex is GLES 3.1 / an extension ANGLE does not
/// expose under SwiftShader). Call-site safety:
///  - glTexBuffer is only called from the PBR material-table upload, which
///    is gated by supportsPbrPath(); the PBR program uses samplerBuffer and
///    cannot compile as GLSL ES 3.00, so the path is off on web regardless.
///  - glDrawElementsBaseVertex is only called on the GL arena path; the web
///    GL page runs --gl-arena off (per-mesh VAOs + glDrawElements). The shim
///    exists so an arena-on web run degrades loudly instead of calling a
///    null pointer.
/// Native builds never call this: the call site in gl_loader.cpp is inside
/// #if defined(__EMSCRIPTEN__), so native preprocessing is unchanged.
bool litholattice_webGlLoadFixups();
