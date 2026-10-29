# Review findings deferred to Archie (M5, M6)

The LithoRoot review at a311223 raised two engine findings that change
behavior or tuning, so they are recorded here and handed to Archie
(systems architecture) instead of being changed in the fix series.
Nothing in this commit changes behavior.

## M5. Software-renderer LOD default 32/64/96 now applies on desktop

`engine/src/render/software_renderer.cpp:290` (`g_ringEdge`). With rings
at 32 chunks or more, nearly everything at normal render distances
renders at full fidelity, so far-LOD is effectively off on desktop. That
is a likely contributor to the roughly 10 FPS in unoptimized Debug builds.

Suggested next step: A/B it with `wasm/harness/ab_lod.mjs` and decide on a
per-platform default.

## M6. Frame-aware streaming budget brings wall-clock time into residency

`game.cpp` `pumpStreaming` (the block after line 1899).

- Grants depend on an EMA of real frame time, so which columns are
  resident on a given tick varies between runs and machines. If any sim
  path reads residency (column present or absent), replay and fairness
  hashes can diverge. This is not confirmed as a cause of any hash
  failure.
- `static float emaMs`, `lastGrant` and `floorOn` persist across world
  loads and dimension changes, and a long load frame pins the grant at
  "starving" for many frames afterward.

Suggested next step: make sim behavior independent of residency, or use a
fixed grant in replay and handshake-verify modes, and reset the statics in
`loadActive()`.
