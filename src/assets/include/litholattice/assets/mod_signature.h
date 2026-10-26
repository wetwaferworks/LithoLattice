// Cortana 2026-10-15 0733: SIGNING new dev public key (fatal-exit check intact)
// Elfie 2026-10-26 (M3, JD ruling 2): the guard is generalized — it now fails
// closed on ANY baked key that is not JD's designated shipping key (retired
// test, workstation dev, or unknown). See the registry in mod_signature.cpp.
// Engine-side mod signature verification (Worker B).
//
// Trust model (JD 2026-10-12, locked): certificates answer WHO vouches for the
// mod; sandboxing answers WHAT it can do. Tiers:
//   - Verified: signature.json exists, names the baked-in JD public key, and
//     the Ed25519 signature over the canonical mod hash is valid. Full trust.
//   - Unsigned: no signature.json. Loads sandboxed, clearly labeled.
//   - Invalid: signature.json exists but is malformed, names a different key,
//     or the signature doesn't verify. REFUSED in all builds (fail closed).
//
// The baked-in public key below is currently the WORKSTATION DEV KEY
// (2026-10-15, JD's workstation — development signing only). It is NOT JD's
// real shipping key: JD must install his master key before beta (see the
// marker). Until he does, release builds fail closed at boot (M3 registry
// in mod_signature.cpp). The private key never ships.
#pragma once
#include <array>

#include <cstdint>
#include <filesystem>
#include <string>

#include "litholattice/assets/mod_manifest.h"  // SignatureStatus

namespace litholattice::assets {

/// Human-readable status for logs.
const char* signatureStatusName(SignatureStatus s);

// ============================================================================
// BAKED-IN PUBLIC KEY
// ============================================================================
// REPLACE BEFORE BETA — this is the workstation DEV key, NOT JD's real key.
//
// To install the real key (2-minute operation for JD):
//   1. On JD's OFFLINE machine:  ./litho_sign keygen --out jd_master
//      (jd_master.priv is the crown jewels — never ships, never commits.)
//   2. Copy jd_master.pub to this machine. Get the hex:
//        od -A n -t x1 jd_master.pub | tr -d ' \n'
//   3. Paste the 64 hex chars below, replacing the test key.
//   4. Rebuild. Verify with: ./litho_sign verify --key jd_master.pub --mod <signed mod>
//      then confirm the engine reports "Verified" at load.
//
// Until then, every mod signed with the test key verifies — which is fine for
// development and MUST NOT ship to players.
// ============================================================================
inline constexpr char kBakedPublicKeyHex[] =
    "04c1766ef29e93b7d36fdf185f64ee958e9dded4c8c82f0dca03029e8a30011c";  // LOCAL DEV KEY 2026-10-15-0733 (JD workstation). Retired test key 3bb44b55... is still rejected by bakedKeyIsTestKey().

/// Verify the mod in `modDir` against the baked-in public key.
/// Returns the SignatureStatus; on Invalid, *err describes why.
/// Never throws; works identically in debug and release builds.
SignatureStatus verifyModSignature(const std::filesystem::path& modDir, std::string* err = nullptr);

/// Verify the mod in `modDir` against an explicit trusted public key (for tests:
/// generate an ephemeral keypair per run via crypto::generateKeypair — never
/// hardcode a private seed in the tree).
SignatureStatus verifyModSignatureAgainstKey(const std::filesystem::path& modDir,
                                             const std::array<uint8_t, 32>& trustedPub,
                                             std::string* err = nullptr);

/// True when the baked-in key is JD's designated shipping key (installed by
/// the procedure above). False for every other key — and for now false
/// always, because the shipping key is not yet installed.
bool bakedKeyIsShippingKey();

/// Human-readable role of the baked-in key ("JD shipping key", the retired
/// test key, the workstation dev key, or "unknown key"), for boot logs.
const char* bakedKeyDescription();

/// True when the baked-in key is NOT JD's designated shipping key.
/// Historical name, generalized semantics (M3, 2026-10-26): before M3 this
/// recognized only the retired test key, and the workstation dev key
/// slipped past the release guard. Shipping builds must fail closed on
/// this (see main.cpp boot guard). Public-key comparison only.
///
/// GUARD SCOPE (P3-3, Appendix E L3 — read before relying on the guard):
/// the ONLY enforcement point today is the game's main() boot sequence
/// (game/LithoKraft/src/app/main.cpp): it calls bakedKeyIsTestKey() and
/// exit(1)s in release (NDEBUG) builds, warning loudly in debug builds.
/// Nothing in the engine library, the litho_sign tool, or any other entry
/// point performs this check. Any FUTURE executable that links
/// litholattice_assets and calls verifyModSignature() must call
/// bakedKeyIsTestKey() itself at startup, or it will silently trust a
/// non-shipping key. The check is deliberately a library FUNCTION, not a
/// library side effect, so tools and tests can verify dev-signed mods on
/// purpose.
bool bakedKeyIsTestKey();

}  // namespace litholattice::assets
