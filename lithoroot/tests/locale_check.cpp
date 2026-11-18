// LithoRoot <locale.h>/<clocale> (C locale only), <malloc.h>, and the signed/unsigned char
// extractors. Returns the number of failed checks (0 = pass).
#include <clocale>
#include <cstring>
#include <locale.h>
#include <malloc.h>
#include <sstream>
#include <string>
extern "C" int lk_locale_check() {
  int bad = 0;
  auto ck = [&](bool ok) { if (!ok) ++bad; };
  ck(std::strcmp(std::setlocale(LC_ALL, nullptr), "C") == 0);
  ck(std::setlocale(LC_NUMERIC, "C") != nullptr);
  ck(std::setlocale(LC_NUMERIC, "") != nullptr);
  ck(std::setlocale(LC_ALL, "POSIX") != nullptr);
  ck(std::setlocale(LC_ALL, "POSIXX") == nullptr);
  ck(std::setlocale(LC_ALL, "en_US.UTF-8") == nullptr);
  ck(std::setlocale(99, "C") == nullptr);
  const std::lconv* lc = std::localeconv();
  ck(lc && std::strcmp(lc->decimal_point, ".") == 0 && lc->thousands_sep[0] == '\0');
  locale_t l = newlocale(LC_NUMERIC_MASK, "C", nullptr);
  ck(l != nullptr);
  ck(newlocale(LC_NUMERIC_MASK, "de_DE", nullptr) == nullptr);
  ck(newlocale(1 << 20, "C", nullptr) == nullptr);
  locale_t old = uselocale(l);
  ck(old == LC_GLOBAL_LOCALE);
  uselocale(old);
  freelocale(l);
  void* p = malloc(32);
  ck(p != nullptr);
  free(p);
  std::istringstream in("  A7 z");
  unsigned char u = 0; signed char s = 0; unsigned char w = 0;
  in >> u >> s >> w;
  ck(!in.fail() && u == 'A' && s == '7' && w == 'z');
  unsigned char e = 'q';
  in >> e;
  ck(in.fail() && e == 'q');
  return bad;
}
