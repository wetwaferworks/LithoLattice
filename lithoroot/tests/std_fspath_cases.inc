/* path cases shared by the wasm check and the host differential run. */
static const char* const kPaths[] = {
  "", ".", "..", "/", "a",  /* "//" left out: libstdc++ keeps it as "//" (implementation-defined); LithoRoot folds it to "/" */
  "/a", "a/", "/a/", "a/b", "a//b", "/a/b/", "a/b/c.txt", "a/.hidden", "a/b.tar.gz",
  "a/./b/../c", "../a/../../b", "/../a", "a/b/..", "a/b/../", "./", "foo.", "/x/y/z/", "res/pack/../tex/grass.png",
  "saves/world 1/region/r.0.0.lkr", "a/..", "a/../..", "..."};
static const char* const kBases[] = {"", "a", "a/b", "/a", "/a/b/c", "a/c", "../x", "."};
