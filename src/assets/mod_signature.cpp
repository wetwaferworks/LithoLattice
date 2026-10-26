// Engine-side mod signature verification. Uses Worker A's tested crypto
// (litholattice_crypto): canonicalModHash + Ed25519 verify.
#include "litholattice/assets/mod_signature.h"

#include <array>
#include <cstring>
#include <fstream>

#include "litholattice/crypto/ed25519.h"
#include "litholattice/crypto/sha256.h"
#include "litholattice/crypto/signing.h"

namespace litholattice::assets {

const char* signatureStatusName(SignatureStatus s) {
  switch (s) {
    case SignatureStatus::Verified: return "verified";
    case SignatureStatus::Unsigned: return "unsigned";
    case SignatureStatus::Invalid: return "invalid";
  }
  return "unknown";
}

namespace {

// The baked key, decoded from hex once at startup.
bool bakedKey(std::array<uint8_t, 32>& out) {
  return crypto::fromHex(kBakedPublicKeyHex, out.data(), out.size());
}

}  // namespace

/// Core verifier: checks signature.json against an explicit trusted public key.
/// `keyDesc` names the key for error messages ("baked-in key", "test key", ...).
static SignatureStatus verifyAgainstKey(const std::filesystem::path& modDir,
                                        const std::array<uint8_t, 32>& trustedPub,
                                        const char* keyDesc, std::string* err) {
  auto setErr = [&](const std::string& m) {
    if (err) *err = m;
  };

  // No signature.json → unsigned (not an error; sandboxed path handles it).
  // A filesystem ERROR here is fail-closed (Invalid, not Unsigned): a transient
  // I/O failure must not downgrade a signed mod to the sandboxed path.
  const std::filesystem::path sigPath = modDir / crypto::kSignatureFileName;
  std::error_code ec;
  const bool exists = std::filesystem::exists(sigPath, ec);
  if (ec) {
    setErr(std::string("cannot stat signature.json: ") + ec.message());
    return SignatureStatus::Invalid;
  }
  if (!exists) {
    return SignatureStatus::Unsigned;
  }

  // Parse signature.json.
  std::string parseErr;
  auto sig = crypto::readModSignature(modDir, &parseErr);
  if (!sig) {
    setErr(std::string("signature.json malformed: ") + parseErr);
    return SignatureStatus::Invalid;
  }

  // The signature must name JD's baked-in key. A valid signature under any
  // OTHER key is still Invalid — we only trust JD (first-party) and keys JD
  // signs for (second-party, via JD's signature on their mod).
  if (std::memcmp(sig->publicKey.data(), trustedPub.data(), 32) != 0) {
    setErr(std::string("signature.json names an untrusted public key (not ") + keyDesc + ")");
    return SignatureStatus::Invalid;
  }

  // Symlink refusal at verify time (P3-3, Appendix E M4): the canonical
  // hash excludes symlinked content that the resource loader would read
  // through in-root links, so any signed mod whose tree contains a
  // symlink is Invalid. Runs only when a signature exists — unsigned dev
  // mods (returned above) are unaffected.
  {
    std::string symErr;
    if (crypto::modTreeHasSymlink(modDir, &symErr)) {
      setErr("signed mod contains a symlink (refused at verify time)");
      return SignatureStatus::Invalid;
    }
  }

  // Recompute the canonical hash and verify the Ed25519 signature.
  std::string hashErr;
  auto hash = crypto::canonicalModHash(modDir, &hashErr);
  if (!hash) {
    setErr(std::string("cannot hash mod dir: ") + hashErr);
    return SignatureStatus::Invalid;
  }
  crypto::ed25519::Signature edSig;
  crypto::ed25519::PublicKey edPub;
  std::memcpy(edSig.data(), sig->signature.data(), 64);
  std::memcpy(edPub.data(), trustedPub.data(), 32);
  if (!crypto::ed25519::verify(edSig, hash->data(), hash->size(), edPub)) {
    setErr("Ed25519 signature verification failed (mod tampered or mis-signed)");
    return SignatureStatus::Invalid;
  }

  return SignatureStatus::Verified;
}

SignatureStatus verifyModSignatureAgainstKey(const std::filesystem::path& modDir,
                                             const std::array<uint8_t, 32>& trustedPub,
                                             std::string* err) {
  return verifyAgainstKey(modDir, trustedPub, "the expected key", err);
}

SignatureStatus verifyModSignature(const std::filesystem::path& modDir, std::string* err) {
  std::array<uint8_t, 32> jdPub;
  if (!bakedKey(jdPub)) {
    if (err) *err = "baked-in public key is malformed (not 64 hex chars) — build misconfigured";
    return SignatureStatus::Invalid;
  }
  return verifyAgainstKey(modDir, jdPub, "JD's baked-in key", err);
}

// ============================================================================
// BAKED-KEY REGISTRY (M3 — code review 2026-10-26, JD ruling 2)
// ============================================================================
// The guard used to recognize only the retired test key, so when the
// workstation dev key was baked in (2026-10-15) the release guard stopped
// tripping — a shipping build would have carried the dev key silently, and
// "Verified" would have meant "signed by a development workstation." The
// guard is now a registry: every key that has ever been baked in is known
// by role, and a baked key is a SHIPPING key only when it matches
// kShippingPublicKeyHex below — JD's real master key, installed by the
// documented procedure in mod_signature.h. Until JD installs it, that
// constant is empty, NO baked key is a shipping key, and release builds
// fail closed at boot. That is deliberate: the 2-minute key install is
// JD's hand on the crown jewels, not something a build can wave through.
// ============================================================================

namespace {

// JD's designated shipping public key (hex). EMPTY until JD installs his
// master key (see mod_signature.h). The private half never ships, never
// commits, and is not recorded here or anywhere in the tree.
constexpr char kShippingPublicKeyHex[] = "";

struct KnownBakedKey {
  const char* hex;
  const char* description;
};

// Every public key that has ever been baked into mod_signature.h, by role.
// Public keys only — public by design, safe to commit.
constexpr KnownBakedKey kKnownBakedKeys[] = {
    {"3bb44b55eabf04186da2ba95b8d93459eadc59d397815487ac45a27d8a9bb787",
     "retired test key (throwaway; its private seed was public in selftests)"},
    {"04c1766ef29e93b7d36fdf185f64ee958e9dded4c8c82f0dca03029e8a30011c",
     "workstation dev key 2026-10-15 (development signing only)"},
};

bool bakedKeyEqualsHex(const char* hex) {
  std::array<uint8_t, 32> jdPub, other{};
  if (!bakedKey(jdPub)) return false;
  if (!crypto::fromHex(hex, other.data(), other.size())) return false;
  return std::memcmp(jdPub.data(), other.data(), 32) == 0;
}

}  // namespace

/// True only when the baked-in key is JD's designated shipping key.
bool bakedKeyIsShippingKey() {
  if (kShippingPublicKeyHex[0] == '\0') return false;  // not yet installed
  return bakedKeyEqualsHex(kShippingPublicKeyHex);
}

/// Human-readable role of the baked-in key, for boot logs.
const char* bakedKeyDescription() {
  if (bakedKeyIsShippingKey()) return "JD shipping key";
  for (const KnownBakedKey& k : kKnownBakedKeys)
    if (bakedKeyEqualsHex(k.hex)) return k.description;
  return "unknown key (not in the baked-key registry)";
}

// Historical name, GENERALIZED semantics (M3): true when the baked-in key
// is anything other than JD's designated shipping key — the retired test
// key, the workstation dev key, or an unknown key. The boot guard in
// main.cpp calls this and fails closed in release builds, so no
// non-shipping key can reach players silently. (Before M3 this compared
// against the retired test key only, which the dev key slipped past.)
bool bakedKeyIsTestKey() { return !bakedKeyIsShippingKey(); }

}  // namespace litholattice::assets
