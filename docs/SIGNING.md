# SECoS — Code Signing & Trust Model

**Status:** design (implemented in M9). Locked decisions (2026-05-31):
Ed25519 + SHA-256; **every** executable must be signed; refuse-by-default with a
`-DDEV_ALLOW_UNSIGNED` build override; single project key for v0 (format
keyring-ready). Open-source software is **ported from source** and signed — not
run as Linux binaries.

---

## 1. Why — the signature is the root of trust

SecOS has three execution modes (`docs/DRIVER_SPACE.md`): **kernel** (ring 0),
**driver** (ring 3, capability-mediated hardware access), **user** (ring 3, no
hardware). A process declares its mode and the capabilities it wants in its
`.note.secos` **manifest**. That declaration is only trustworthy if it cannot be
forged — so **a valid signature is required for any ELF to run at all**, and the
signature covers the manifest. Consequences:

- No valid signature ⇒ the loader **refuses** to run the ELF (user *or* driver).
- A process is created as a **driver** only if it is validly signed **and** its
  manifest declares `MANIFEST_FLAG_DRIVER` with the requested capabilities — i.e.
  the signer vouches for those privileges. (Wired up fully in M11.)
- Signing is the **admission gate**: signing a program = the key holder (the
  integrator/distributor) vouching for it and admitting it into the trust domain.

This is a closed, signed-execution model (cf. iOS / locked-down enterprise OSes).

## 2. Cryptography

- **Digest:** SHA-256.
- **Signature:** Ed25519 (deterministic, compact 64-byte signatures, 32-byte
  keys; self-contained verify with no malloc — suitable for a freestanding
  kernel). The kernel embeds the **trusted public key** and only ever *verifies*;
  the **private key never ships in the kernel** (it lives on the build/signing
  host), so the on-device trust anchor cannot be used to forge.

## 3. What is signed (the signed digest)

```
digest = SHA-256( entire ELF file, with the 64-byte QSIG `sig` field set to 0 )
signature = Ed25519_sign(digest)        // signs the 32-byte digest
```

This is the single source of truth: host signer and kernel verifier perform the
**identical** computation — hash the whole file, treating the 64 signature bytes
as zero. Rationale:

- Covers **everything** security-relevant — ELF header (entry point), program
  headers (segment vaddrs/flags), the loaded segments, and the manifest — so any
  tampering invalidates the signature. (Simpler and stronger than hashing only
  PT_LOAD segments: there is nothing to keep in sync between the two sides except
  "where are the 64 sig bytes".)
- Only the `sig[64]` field is excluded (zeroed); the rest of the QSIG note
  (version, key_id) is covered.
- Implemented in `tools/secos_signlib.py` (host) and `mm/elf_sign.c` (kernel).

## 4. ELF carrier format (`.note.secos`)

Two `PT_NOTE` entries with name `"SECOS"`:

- **Manifest note**, type `QSEC` (`0x51534543`) — extended `elf_manifest_raw`:
  `version`, `flags`, `proc_type` (USER/DRIVER), `caps_mask`, `max_mem`,
  `entry_hint`. (Today's manifest has version/flags/max_mem/entry_hint; M9 adds
  `proc_type` + `caps_mask`.)
- **Signature note**, type `QSIG` (`0x51534947`) — `{ uint32 version; uint32
  key_id; uint8 sig[64]; }`. `key_id` selects the trusted key (0 = project key in
  v0; a keyring of `core`/`ports`/`driver` keys can be added later without a
  format change).

## 5. Verification flow (loader gate)

```
load ELF buffer
  -> parse QSEC manifest          (missing manifest => refuse, unless DEV_ALLOW_UNSIGNED)
  -> parse QSIG signature         (missing signature => refuse, unless DEV_ALLOW_UNSIGNED)
  -> recompute the signed digest  (§3)
  -> ed25519_verify(digest, sig, trusted_pubkey[key_id])
        fail  => refuse (do not map, do not run)
        ok    => proceed: map segments, enforce manifest (W^X, max_mem, ...),
                 set proc_type; DRIVER only if manifest says so
```

`-DDEV_ALLOW_UNSIGNED` (build-time) downgrades "refuse" to "warn + run" so the
system can bootstrap before the toolchain/keys exist. Off in any real build.

## 6. Toolchain (host, build-time)

- `tools/secos-keygen` — generate the Ed25519 keypair; the public key is emitted
  as a C array embedded in the kernel, the private key stays on the build host
  (never committed in real use).
- `tools/secos-sign <elf>` — compute the digest (§3), Ed25519-sign it, and patch
  the `QSIG` note's `sig` field in place (the ELF must already carry the note,
  linked in via the user crt0/manifest). Every user/driver ELF is signed.
- `tools/gen_signed_test.py` — builds a minimal signed ELF and emits
  `crypto/signed_test_elf.h` for the kernel's signature-verify self-test.

**DEV key (bootstrap):** `tools/secos-keygen --dev` derives the keypair from a
**fixed dev seed** (in `secos_signlib.py`) so the build can sign without secrets
while bootstrapping. The committed `crypto/secos_pubkey.h` is this DEV public
key. **Production replaces it** with a real random key whose private half is kept
offline (`secos-keygen` with no `--dev` prints a random seed to store securely).

## 7. Open-source software

"Importing open-source software" means: take the upstream **source**, recompile
it against the SecOS libc (custom syscall ABI underneath, POSIX-friendly API on
top), then **sign** the resulting SecOS-native ELF. SecOS does **not** run
unmodified Linux binaries (no Linux syscall layer or dynamic linker) — that would
import Linux's entire ABI/attack surface and break the security thesis. The
signature is the integrator's admission of vetted, recompiled software.
