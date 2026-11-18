// Host harness for lk_svg_parse.h (stage 1, the parser). Build:
//   g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
//       -I engine/src/assets/include engine/src/assets/fuzz/lk_svg_check.cpp -o lk_svg_check
// Modes: parse <files> | malformed | fuzz <iters> <files>
#include "litholattice/assets/lk_svg_parse.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
using namespace litholattice;
static std::string slurp(const char* p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }
static bool run(const std::string& s, const char** err, size_t* shapes = nullptr, size_t* pts = nullptr) {
  lksvg::Doc d; bool ok = lksvg::parse(reinterpret_cast<const uint8_t*>(s.data()), s.size(), d, err);
  if (shapes) *shapes = d.shapes.size();
  if (pts) { *pts = 0; for (auto& sh : d.shapes) *pts += sh.pts.size(); }
  if (!ok && !d.shapes.empty()) { std::printf("VIOLATION: partial output on failure\n"); std::exit(2); }
  return ok;
}
int main(int argc, char** argv) {
  if (argc < 2) return 1;
  std::string mode = argv[1];
  if (mode == "parse") {
    int bad = 0;
    for (int k = 2; k < argc; ++k) { const char* e = nullptr; size_t sh, pt; bool ok = run(slurp(argv[k]), &e, &sh, &pt);
      std::printf("%s %s shapes=%zu points=%zu%s%s\n", ok ? "OK  " : "FAIL", argv[k], sh, pt, ok ? "" : " : ", ok ? "" : e); bad += !ok; }
    return bad ? 1 : 0;
  }
  if (mode == "malformed") {
    const char* good = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\"><path d=\"M 1 1 L 9 1 L 5 9 Z\" fill=\"#ff0000\"/></svg>";
    const char* cases[] = {
      "", "<svg>", "<svg viewBox=\"0 0 10 10\">", "<svg viewBox=\"0 0 10 10\"></svg><svg viewBox=\"0 0 1 1\"></svg>",
      "<svg viewBox=\"0 0 -1 10\"></svg>", "<svg viewBox=\"0 0 10\"></svg>", "<svg></svg>",
      "<svg viewBox=\"0 0 10 10\"><g></g></svg>", "<svg viewBox=\"0 0 10 10\"><text>x</text></svg>",
      "<svg viewBox=\"0 0 10 10\"><path d=\"m 1 1 l 2 2\"/></svg>", "<svg viewBox=\"0 0 10 10\"><path d=\"M 1 1 C 1 2 3 4 5 6\"/></svg>",
      "<svg viewBox=\"0 0 10 10\"><path d=\"M 1 1 A 1 1 0 0 1 2 2\"/></svg>", "<svg viewBox=\"0 0 10 10\"><path d=\"L 1 1\"/></svg>",
      "<svg viewBox=\"0 0 10 10\"><path d=\"M 1\"/></svg>", "<svg viewBox=\"0 0 10 10\"><path d=\"Z\"/></svg>", "<svg viewBox=\"0 0 10 10\"><path/></svg>",
      "<svg viewBox=\"0 0 10 10\"><path d=\"M 1e999 1 L 2 2\"/></svg>", "<svg viewBox=\"0 0 10 10\"><path d=\"M 1 1 L 2 2\" fill=\"red\"/></svg>",
      "<svg viewBox=\"0 0 10 10\"><path d=\"M 1 1 L 2 2\" fill=\"#ggg\"/></svg>", "<svg viewBox=\"0 0 10 10\"><path d=\"M 1 1 L 2 2\" fill-opacity=\"2\"/></svg>",
      "<svg viewBox=\"0 0 10 10\"><path d=\"M 1 1 L 2 2\" transform=\"rotate(45)\"/></svg>", "<svg viewBox=\"0 0 10 10\"><path d=\"M 1 1 L 2 2\" style=\"fill:red\"/></svg>",
      "<svg viewBox=\"0 0 10 10\"><path d=\"M 1 1 L 2 2\" fill=#fff/></svg>", "<svg viewBox=\"0 0 10 10\"><path d=\"M 1 1 L 2 2/></svg>",
      "<svg viewBox=\"0 0 10 10\"><polygon points=\"1 1 2 2\"/></svg>", "<svg viewBox=\"0 0 10 10\"><polygon points=\"1 1 2 2 3\"/></svg>",
      "<svg viewBox=\"0 0 10 10\"><circle cx=\"1\" cy=\"1\"/></svg>", "<svg viewBox=\"0 0 10 10\"><circle cx=\"1\" cy=\"1\" r=\"-1\"/></svg>",
      "<svg viewBox=\"0 0 10 10\"><line x1=\"1\" y1=\"1\" x2=\"2\"/></svg>", "<svg viewBox=\"0 0 10 10\"><rect x=\"0\" y=\"0\" width=\"0\" height=\"1\"/></svg>",
      "<svg viewBox=\"0 0 10 10\"><path d=\"M 1 1 L 2 2\" stroke-width=\"5000\"/></svg>", "<svg viewBox=\"0 0 10 10\"><path d=\"M 1 1 L 2 2\" stroke-linejoin=\"arcs\"/></svg>",
      "<svg viewBox=\"0 0 10 10\"></path></svg>", "<path d=\"M 1 1 L 2 2\"/>", "<svg viewBox=\"0 0 10 10\"><svg viewBox=\"0 0 1 1\"></svg></svg>",
      "<!DOCTYPE svg><svg viewBox=\"0 0 10 10\"></svg>", "<svg viewBox=\"0 0 10 10\"><!-- c --></svg>",
      "<svg viewBox=\"0 0 10 10\"><path d=\"M 1 1 L 2 2\" fill=\"#fff\" fill=\"url(#g)\"/></svg>",
      "<svg viewBox=\"0 0 10 10\"><path d=\"M 0000000000000001 1 L 2 2\"/></svg>", "<svg viewBox=\"0 0 10 10\"><path d=\"M 1. 1 L .5e 2\"/></svg>",
    };
    int rejected = 0, accepted = 0;
    for (const char* c : cases) { const char* e = nullptr; if (run(c, &e)) { ++accepted; std::printf("ACCEPTED (bad): %s\n", c); } else ++rejected; }
    const char* e = nullptr; bool ctl = run(good, &e);
    std::string big(lksvg::kMaxInput + 1, ' '); bool bigOk = run(big, &e);
    std::string many = "<svg viewBox=\"0 0 1 1\">"; for (size_t k = 0; k <= lksvg::kMaxShapes; ++k) many += "<rect width=\"1\" height=\"1\"/>"; many += "</svg>"; bool manyOk = run(many, &e);
    std::printf("malformed: %d rejected, %d accepted; control %s; oversize %s; too-many-shapes %s (%s)\n", rejected, accepted, ctl ? "OK" : "FAIL", bigOk ? "ACCEPTED" : "rejected", manyOk ? "ACCEPTED" : "rejected", e);
    return (accepted || !ctl || bigOk || manyOk) ? 1 : 0;
  }
  if (mode == "fuzz") {
    long iters = std::atol(argv[2]); std::vector<std::string> corpus; for (int k = 3; k < argc; ++k) corpus.push_back(slurp(argv[k]));
    uint64_t x = 0x9E3779B97F4A7C15ull; auto rnd = [&] { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    const char dict[] = "<>/=\"' MLHVQZmlcCA#0123456789.-e,svgpathfillstroke";
    long ok = 0;
    for (long it = 0; it < iters; ++it) {
      std::string s = corpus[rnd() % corpus.size()]; int n = 1 + int(rnd() % 8);
      for (int m = 0; m < n && !s.empty(); ++m) { size_t p = rnd() % s.size();
        switch (rnd() % 5) { case 0: s[p] = char(rnd()); break; case 1: s[p] = dict[rnd() % (sizeof(dict) - 1)]; break;
          case 2: s.erase(p, 1 + rnd() % 8); break; case 3: s.insert(p, 1, dict[rnd() % (sizeof(dict) - 1)]); break; case 4: s.resize(p); break; } }
      const char* e = nullptr; ok += run(s, &e);
    }
    std::printf("fuzz: %ld iterations, %ld accepted, %ld rejected, 0 violations\n", iters, ok, iters - ok);
    return 0;
  }
  return 1;
}
