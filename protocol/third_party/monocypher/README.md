# Monocypher (vendored)

Monocypher **4.0.3** (released 2026-06-15), `src/monocypher.c`, `src/monocypher.h`
and `LICENCE.md` copied **unmodified** from the official release tarball
`https://github.com/LoupVaillant/Monocypher/releases/download/4.0.3/monocypher-4.0.3.tar.gz`
(SHA-256 `8cc9bc341a66249016db9bd70e9142d8d0aef9945973744b1ac05dbc55d8ee66`).
Licence: CC0-1.0 or BSD-2-Clause (dual, see `LICENCE.md`).

Used by `protocol/src/crypto.cpp` for X25519, the Elligator 2 map,
ChaCha20-Poly1305 (IETF, via `crypto_aead_init_ietf` + `crypto_aead_write/read`
with a fresh context per record = plain RFC 8439) and `crypto_wipe`.
Build notes (protocol/CMakeLists.txt): warnings silenced for this file only;
always built with `-O3` on GCC/Clang; on 32-bit MSVC with `/d2SSAOptimizer-`
(MSVC 14.29 x86 `/O2` miscompiles X25519; `owc_crypto_tests` detects it).
To update: replace the three files from a newer release and run `owc_crypto_tests`.
