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

The digest is computed **deterministically** over content that fully determines
what executes:

```
SHA-256(
   for each PT_LOAD segment, in ascending p_vaddr order:
       p_vaddr (8) || p_memsz (8) || p_flags (4) || segment_file_bytes
   || manifest_canonical_bytes        // version, flags, proc_type, caps, limits, entry
)
```

Notes:
- Covers the **loaded image** (what runs) and the **manifest** (mode/caps/limits),
  so tampering with either invalidates the signature.
- The signature note itself is **excluded** from the digest.
- Field order/endianness are fixed (little-endian) so host signer and kernel
  verifier agree byte-for-byte.

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
- `tools/secos-sign <elf>` — compute the digest (§3), Ed25519-sign it, append/patch
  the `QSIG` note. Every user/driver ELF is signed as a build step.

## 7. Open-source software

"Importing open-source software" means: take the upstream **source**, recompile
it against the SecOS libc (custom syscall ABI underneath, POSIX-friendly API on
top), then **sign** the resulting SecOS-native ELF. SecOS does **not** run
unmodified Linux binaries (no Linux syscall layer or dynamic linker) — that would
import Linux's entire ABI/attack surface and break the security thesis. The
signature is the integrator's admission of vetted, recompiled software.
