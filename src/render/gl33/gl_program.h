// Private to the gl33 backend: a linked GLSL program (RAII, move-only).
#pragma once
#include <optional>
#include <string>
#include "gl_loader.h"

namespace litholattice::gl33 {

class cGlProgram {
 public:
  cGlProgram() = default;
  ~cGlProgram();
  cGlProgram(cGlProgram&& o) noexcept : id_(o.id_) { o.id_ = 0; }
  cGlProgram& operator=(cGlProgram&& o) noexcept;
  cGlProgram(const cGlProgram&) = delete;
  cGlProgram& operator=(const cGlProgram&) = delete;

  /// Compiles and links; nullopt with the driver's log in `log` on failure.
  static std::optional<cGlProgram> build(const std::string& vertex, const std::string& fragment, std::string* log);

  GLuint id() const { return id_; }
  GLint uniform(const char* name) const { return glGetUniformLocation(id_, name); }
  void use() const { glUseProgram(id_); }

 private:
  explicit cGlProgram(GLuint id) : id_(id) {}
  GLuint id_ = 0;
};

}  // namespace litholattice::gl33
