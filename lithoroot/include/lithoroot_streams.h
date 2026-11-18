/* LithoRoot streams core: ios_base, ios, streambuf, istream, ostream, iostream,
 * istreambuf_iterator. Subset per the S0d fallback: char only, no locale, no wide
 * streams, no iomanip, no cout/cerr/cin (no stream globals, so no static-init order
 * and no shared lock). Each stream object is single-thread, as in the standard.
 * Formatting: integers in decimal (or hex/oct via the hex/oct/dec manipulators),
 * floating point like printf %g with precision() (default 6). */
#ifndef LITHOROOT_STREAMS_H
#define LITHOROOT_STREAMS_H
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace std {

using streamsize = long long;
using streamoff = long long;
using streampos = long long;

class ios_base {
public:
  using openmode = unsigned;
  static constexpr openmode app = 1, ate = 2, binary = 4, in = 8, out = 16, trunc = 32;
  using iostate = unsigned;
  static constexpr iostate goodbit = 0, badbit = 1, eofbit = 2, failbit = 4;
  using seekdir = int;
  static constexpr seekdir beg = 0, cur = 1, end = 2;
  using fmtflags = unsigned;
  static constexpr fmtflags dec = 1, hex = 2, oct = 4, basefield = 7, fixed = 8, scientific = 16,
                            floatfield = 24, boolalpha = 32, skipws = 64;
  ios_base(const ios_base&) = delete;
  ios_base& operator=(const ios_base&) = delete;
  fmtflags flags() const noexcept { return fl_; }
  fmtflags flags(fmtflags f) noexcept { fmtflags o = fl_; fl_ = f; return o; }
  fmtflags setf(fmtflags f) noexcept { fmtflags o = fl_; fl_ |= f; return o; }
  fmtflags setf(fmtflags f, fmtflags mask) noexcept { fmtflags o = fl_; fl_ = (fl_ & ~mask) | (f & mask); return o; }
  void unsetf(fmtflags f) noexcept { fl_ &= ~f; }
  streamsize precision() const noexcept { return prec_; }
  streamsize precision(streamsize p) noexcept { streamsize o = prec_; prec_ = p; return o; }
  streamsize width() const noexcept { return 0; }
protected:
  ios_base() = default;
  fmtflags fl_ = dec | skipws;
  streamsize prec_ = 6;
};

class streambuf {
public:
  static constexpr int eof = -1;
  virtual ~streambuf() = default;
  int sgetc() { return peekc(); }
  int sbumpc() { int c = peekc(); if (c != eof) advance(); return c; }
  int snextc() { return sbumpc() == eof ? eof : sgetc(); }
  streamsize sgetn(char* s, streamsize n) { return xsgetn(s, n); }
  int sputc(char c) { return xsputn(&c, 1) == 1 ? static_cast<unsigned char>(c) : eof; }
  streamsize sputn(const char* s, streamsize n) { return xsputn(s, n); }
  streampos pubseekoff(streamoff off, ios_base::seekdir d, ios_base::openmode w = ios_base::in | ios_base::out) { return seekoff(off, d, w); }
  streampos pubseekpos(streampos p, ios_base::openmode w = ios_base::in | ios_base::out) { return seekoff(p, ios_base::beg, w); }
  int pubsync() { return sync(); }
protected:
  streambuf() = default;
  streambuf(const streambuf&) = default;
  streambuf& operator=(const streambuf&) = default;
  virtual int peekc() { return eof; }   // next char without consuming, or eof
  virtual void advance() {}             // consume the char peekc returned
  virtual streamsize xsgetn(char* s, streamsize n) {
    streamsize k = 0;
    for (; k < n; ++k) { int c = sbumpc(); if (c == eof) break; s[k] = static_cast<char>(c); }
    return k;
  }
  virtual streamsize xsputn(const char*, streamsize) { return 0; }
  virtual streampos seekoff(streamoff, ios_base::seekdir, ios_base::openmode) { return -1; }
  virtual int sync() { return 0; }
};

class ios : public ios_base {
public:
  explicit ios(streambuf* sb) : sb_(sb) { if (!sb) st_ = badbit; }
  bool good() const noexcept { return st_ == 0; }
  bool eof() const noexcept { return (st_ & eofbit) != 0; }
  bool fail() const noexcept { return (st_ & (failbit | badbit)) != 0; }
  bool bad() const noexcept { return (st_ & badbit) != 0; }
  explicit operator bool() const noexcept { return !fail(); }
  bool operator!() const noexcept { return fail(); }
  iostate rdstate() const noexcept { return st_; }
  void setstate(iostate s) noexcept { st_ |= s; }
  void clear(iostate s = goodbit) noexcept { st_ = sb_ ? s : (s | badbit); }
  streambuf* rdbuf() const noexcept { return sb_; }
protected:
  ios() = default;
  void init(streambuf* sb) noexcept { sb_ = sb; st_ = sb ? goodbit : badbit; }
  streambuf* sb_ = nullptr;
  iostate st_ = goodbit;
};

class ostream : virtual public ios {
public:
  explicit ostream(streambuf* sb) { init(sb); }
  ostream& write(const char* s, streamsize n) {
    if (!fail() && n > 0 && sb_->sputn(s, n) != n) setstate(badbit);
    return *this;
  }
  ostream& put(char c) { return write(&c, 1); }
  ostream& flush() { if (sb_ && sb_->pubsync() == -1) setstate(badbit); return *this; }
  streampos tellp() { return fail() ? -1 : sb_->pubseekoff(0, cur, out); }
  ostream& seekp(streampos p) { if (!fail() && sb_->pubseekpos(p, out) == -1) setstate(failbit); return *this; }
  ostream& seekp(streamoff o, seekdir d) { if (!fail() && sb_->pubseekoff(o, d, out) == -1) setstate(failbit); return *this; }

  ostream& operator<<(const char* s) { if (!s) { setstate(badbit); return *this; } return write(s, streamsize(__builtin_strlen(s))); }
  ostream& operator<<(const string& s) { return write(s.data(), streamsize(s.size())); }
  ostream& operator<<(string_view s) { return write(s.data(), streamsize(s.size())); }
  ostream& operator<<(char c) { return put(c); }
  ostream& operator<<(signed char c) { return put(char(c)); }
  ostream& operator<<(unsigned char c) { return put(char(c)); }
  ostream& operator<<(bool b) { return (fl_ & boolalpha) ? *this << (b ? "true" : "false") : put(b ? '1' : '0'); }
  ostream& operator<<(short v) { return sint(v); }
  ostream& operator<<(int v) { return sint(v); }
  ostream& operator<<(long v) { return sint(v); }
  ostream& operator<<(long long v) { return sint(v); }
  ostream& operator<<(unsigned short v) { return uint(v); }
  ostream& operator<<(unsigned v) { return uint(v); }
  ostream& operator<<(unsigned long v) { return uint(v); }
  ostream& operator<<(unsigned long long v) { return uint(v); }
  ostream& operator<<(float v) { return flt(double(v)); }
  ostream& operator<<(double v) { return flt(v); }
  ostream& operator<<(const void* p) {
    char b[24]; int n = snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(p)));
    return write(b, n);
  }
  ostream& operator<<(nullptr_t) { return *this << "nullptr"; }
  ostream& operator<<(streambuf* sb) {  // copy everything left in sb (the read-whole-file idiom)
    if (!sb) { setstate(badbit); return *this; }
    char buf[4096]; streamsize got, total = 0;
    while ((got = sb->sgetn(buf, sizeof buf)) > 0) { write(buf, got); total += got; if (fail()) break; }
    if (total == 0) setstate(failbit);
    return *this;
  }
  ostream& operator<<(ostream& (*m)(ostream&)) { return m(*this); }
  ostream& operator<<(ios_base& (*m)(ios_base&)) { m(*this); return *this; }
protected:
  ostream() = default;
private:
  ostream& uint(unsigned long long v) {
    char b[24]; const char* f = (fl_ & hex) ? "%llx" : (fl_ & oct) ? "%llo" : "%llu";
    return write(b, snprintf(b, sizeof b, f, v));
  }
  ostream& sint(long long v) {
    if (fl_ & (hex | oct)) return uint(static_cast<unsigned long long>(v));
    char b[24]; return write(b, snprintf(b, sizeof b, "%lld", v));
  }
  ostream& flt(double v) {
    char b[352]; const int p = int(prec_ < 0 ? 6 : prec_ > 60 ? 60 : prec_);
    const char* f = (fl_ & floatfield) == fixed ? "%.*f" : (fl_ & floatfield) == scientific ? "%.*e" : "%.*g";
    int n = snprintf(b, sizeof b, f, p, v);
    return write(b, n < 0 ? 0 : (n >= int(sizeof b) ? int(sizeof b) - 1 : n));
  }
};

// Rvalue insertion: std::ofstream(p) << text;
template <class Os, class T>
  requires (is_base_of_v<ostream, remove_reference_t<Os>> && !is_lvalue_reference_v<Os> &&
            requires(ostream& o, const T& x) { o << x; })
Os&& operator<<(Os&& os, const T& x) { static_cast<ostream&>(os) << x; return static_cast<Os&&>(os); }

inline ostream& endl(ostream& o) { o.put('\n'); return o.flush(); }
inline ostream& ends(ostream& o) { return o.put('\0'); }
inline ostream& flush(ostream& o) { return o.flush(); }
inline ios_base& hex(ios_base& s) { s.setf(ios_base::hex, ios_base::basefield); return s; }
inline ios_base& dec(ios_base& s) { s.setf(ios_base::dec, ios_base::basefield); return s; }
inline ios_base& oct(ios_base& s) { s.setf(ios_base::oct, ios_base::basefield); return s; }
inline ios_base& fixed(ios_base& s) { s.setf(ios_base::fixed, ios_base::floatfield); return s; }
inline ios_base& scientific(ios_base& s) { s.setf(ios_base::scientific, ios_base::floatfield); return s; }
inline ios_base& defaultfloat(ios_base& s) { s.unsetf(ios_base::floatfield); return s; }
inline ios_base& boolalpha(ios_base& s) { s.setf(ios_base::boolalpha); return s; }
inline ios_base& noboolalpha(ios_base& s) { s.unsetf(ios_base::boolalpha); return s; }

class istream : virtual public ios {
public:
  explicit istream(streambuf* sb) { init(sb); }
  streamsize gcount() const noexcept { return gc_; }
  int peek() { gc_ = 0; if (fail()) return streambuf::eof; int c = sb_->sgetc(); if (c == streambuf::eof) setstate(eofbit); return c; }
  int get() {
    gc_ = 0; if (fail()) { setstate(failbit); return streambuf::eof; }
    int c = sb_->sbumpc(); if (c == streambuf::eof) setstate(eofbit | failbit); else gc_ = 1; return c;
  }
  istream& get(char& ch) { int c = get(); if (c != streambuf::eof) ch = char(c); return *this; }
  istream& read(char* s, streamsize n) {
    gc_ = 0; if (fail()) { setstate(failbit); return *this; }
    gc_ = n > 0 ? sb_->sgetn(s, n) : 0;
    if (gc_ < n) setstate(eofbit | failbit);
    return *this;
  }
  istream& ignore(streamsize n = 1, int delim = streambuf::eof) {
    gc_ = 0; if (fail()) return *this;
    while (n-- > 0) { int c = sb_->sbumpc(); if (c == streambuf::eof) { setstate(eofbit); break; } ++gc_; if (c == delim) break; }
    return *this;
  }
  istream& unget() { setstate(failbit); return *this; }  // not provided by the subset: fails safe
  streampos tellg() { return fail() ? -1 : sb_->pubseekoff(0, cur, in); }
  istream& seekg(streampos p) { clear(rdstate() & ~eofbit); if (!fail() && sb_->pubseekpos(p, in) == -1) setstate(failbit); return *this; }
  istream& seekg(streamoff o, seekdir d) { clear(rdstate() & ~eofbit); if (!fail() && sb_->pubseekoff(o, d, in) == -1) setstate(failbit); return *this; }
  istream& getline(char* s, streamsize n, char delim = '\n') {
    gc_ = 0; if (n <= 0) { setstate(failbit); return *this; }
    streamsize k = 0;
    while (!fail()) {
      int c = sb_->sgetc();
      if (c == streambuf::eof) { setstate(eofbit); break; }
      if (char(c) == delim) { sb_->sbumpc(); ++gc_; break; }
      if (k + 1 >= n) { setstate(failbit); break; }
      sb_->sbumpc(); s[k++] = char(c); ++gc_;
    }
    s[k] = '\0'; if (gc_ == 0) setstate(failbit);
    return *this;
  }

  istream& operator>>(string& s) {
    if (!sentry()) return *this;
    s.clear();
    for (int c; (c = sb_->sgetc()) != streambuf::eof && !space(c); sb_->sbumpc()) s.push_back(char(c));
    if (sb_->sgetc() == streambuf::eof) setstate(eofbit);
    if (s.empty()) setstate(failbit);
    return *this;
  }
  istream& operator>>(char& ch) { if (sentry()) get(ch); return *this; }
  // [istream.extractors]: signed/unsigned char read one character, like char.
  istream& operator>>(signed char& c) { char ch = 0; *this >> ch; if (!fail()) c = static_cast<signed char>(ch); return *this; }
  istream& operator>>(unsigned char& c) { char ch = 0; *this >> ch; if (!fail()) c = static_cast<unsigned char>(ch); return *this; }
  istream& operator>>(short& v) { return num(v); }
  istream& operator>>(int& v) { return num(v); }
  istream& operator>>(long& v) { return num(v); }
  istream& operator>>(long long& v) { return num(v); }
  istream& operator>>(unsigned short& v) { return num(v); }
  istream& operator>>(unsigned& v) { return num(v); }
  istream& operator>>(unsigned long& v) { return num(v); }
  istream& operator>>(unsigned long long& v) { return num(v); }
  istream& operator>>(float& v) { return num(v); }
  istream& operator>>(double& v) { return num(v); }
  istream& operator>>(bool& v) { int i = 0; num(i); if (!fail()) { if (i == 0 || i == 1) v = i != 0; else setstate(failbit); } return *this; }
  istream& operator>>(istream& (*m)(istream&)) { return m(*this); }
  istream& operator>>(ios_base& (*m)(ios_base&)) { m(*this); return *this; }
protected:
  istream() = default;
  streamsize gc_ = 0;
private:
  static bool space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }
  bool sentry() {
    if (fail()) { setstate(failbit); return false; }
    if (fl_ & skipws) while (true) {
      int c = sb_->sgetc();
      if (c == streambuf::eof) { setstate(eofbit | failbit); return false; }
      if (!space(c)) break;
      sb_->sbumpc();
    }
    return true;
  }
  template <class T> istream& num(T& v) {
    if (!sentry()) return *this;
    char b[352]; size_t k = 0;
    for (int c; k + 1 < sizeof b && (c = sb_->sgetc()) != streambuf::eof && !space(c); sb_->sbumpc()) {
      const char ch = char(c);
      const bool okc = (ch >= '0' && ch <= '9') ||
          ((ch == '-' || ch == '+') && (k == 0 || b[k - 1] == 'e' || b[k - 1] == 'E')) ||
          ((fl_ & hex) && ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F') || ch == 'x' || ch == 'X')) ||
          (is_floating_point_v<T> && (ch == '.' || ch == 'e' || ch == 'E'));
      if (!okc) break;
      b[k++] = ch;
    }
    b[k] = '\0';
    if (sb_->sgetc() == streambuf::eof) setstate(eofbit);
    char* e = nullptr; errno = 0;
    if constexpr (is_floating_point_v<T>) {
      const double d = strtod(b, &e);
      if (k == 0 || *e) { v = 0; setstate(failbit); } else v = T(d);
    } else {
      const int base = (fl_ & hex) ? 16 : (fl_ & oct) ? 8 : 10;
      if constexpr (is_signed_v<T>) {
        const long long x = strtoll(b, &e, base);
        if (k == 0 || *e) { v = 0; setstate(failbit); }
        else if (errno == ERANGE || x < (long long)__lk_min<T>() || x > (long long)__lk_max<T>()) { v = x < 0 ? __lk_min<T>() : __lk_max<T>(); setstate(failbit); }
        else v = T(x);
      } else {
        const unsigned long long x = strtoull(b, &e, base);
        if (k == 0 || *e || b[0] == '-') { v = 0; setstate(failbit); }
        else if (errno == ERANGE || x > (unsigned long long)__lk_max<T>()) { v = __lk_max<T>(); setstate(failbit); }
        else v = T(x);
      }
    }
    return *this;
  }
  template <class T> static constexpr T __lk_max() { if constexpr (is_signed_v<T>) return T((1ULL << (sizeof(T) * 8 - 1)) - 1); else return T(~T(0)); }
  template <class T> static constexpr T __lk_min() { if constexpr (is_signed_v<T>) return T(-__lk_max<T>() - 1); else return T(0); }
};

inline istream& ws(istream& is) { while (true) { int c = is.peek(); if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') is.get(); else break; } if (is.eof()) is.clear(is.rdstate() & ~ios_base::failbit); return is; }

template <class Is, class T>
  requires (is_base_of_v<istream, remove_reference_t<Is>> && !is_lvalue_reference_v<Is> &&
            requires(istream& i, T& x) { i >> x; })
Is&& operator>>(Is&& is, T&& x) { static_cast<istream&>(is) >> x; return static_cast<Is&&>(is); }

inline istream& getline(istream& is, string& s, char delim) {
  s.clear();
  if (is.fail()) { is.setstate(ios_base::failbit); return is; }
  streambuf* sb = is.rdbuf(); size_t n = 0;
  while (true) {
    int c = sb->sbumpc();
    if (c == streambuf::eof) { is.setstate(ios_base::eofbit); break; }
    ++n;
    if (char(c) == delim) break;
    s.push_back(char(c));
  }
  if (n == 0) is.setstate(ios_base::failbit);
  return is;
}
inline istream& getline(istream& is, string& s) { return getline(is, s, '\n'); }
inline istream& getline(istream&& is, string& s, char delim) { return getline(is, s, delim); }
inline istream& getline(istream&& is, string& s) { return getline(is, s, '\n'); }

class iostream : public istream, public ostream {
public:
  explicit iostream(streambuf* sb) { init(sb); }
protected:
  iostream() = default;
};

template <class CharT> class istreambuf_iterator;
template <>
class istreambuf_iterator<char> {
public:
  using iterator_category = input_iterator_tag;
  using value_type = char;
  using difference_type = streamoff;
  using pointer = const char*;
  using reference = char;
  constexpr istreambuf_iterator() noexcept = default;
  istreambuf_iterator(istream& is) noexcept : sb_(is.rdbuf()) {}
  istreambuf_iterator(streambuf* sb) noexcept : sb_(sb) {}
  char operator*() const { return char(sb_->sgetc()); }
  istreambuf_iterator& operator++() { sb_->sbumpc(); return *this; }
  istreambuf_iterator operator++(int) { istreambuf_iterator t = *this; sb_->sbumpc(); return t; }
  bool equal(const istreambuf_iterator& o) const { return at_end() == o.at_end(); }
  friend bool operator==(const istreambuf_iterator& a, const istreambuf_iterator& b) { return a.equal(b); }
private:
  bool at_end() const { if (sb_ && sb_->sgetc() == streambuf::eof) sb_ = nullptr; return sb_ == nullptr; }
  mutable streambuf* sb_ = nullptr;
};

}  // namespace std
#endif
