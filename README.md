# Private ACNH Apple build orchestration

Experimental adaptation for iPhone 13 (A15), not a verified playable port.

Uses seedds/smbw-mobile at the pinned commit in `ci/build.sh`, plus reviewed
local overlays. Source license remains that of the upstream MIT project.
No game binaries, extracted shaders, ROMs, or decryption keys are committed.
Only necessary build inputs are attached to a release in this private repository.

Actions builds unsigned iOS binaries by default. RomFS stays on the local
machine; the thin IPA expects the game's complete RomFS in Documents/romfs.
Cloud compilation does not establish device compatibility or memory usage.

The default device iteration skips the macOS host, regression tests, extra syntax
checks, and ARM64 structure verification. Enable `full_checks` for the host and
regression suite; enable `verify_rewrite` separately for structural verification.
The default shader build compiles only iOS Metal libraries (`DMSH_MAC=0`),
including fragment variants and geometry lowering. Shared MSL and reflection
metadata are retained for constant-buffer checks. `full_checks` also enables
macOS Metal compilation. Platform-specific cache keys keep these modes separate.
The actual iOS compile/link and shader completeness checks still run.

Compiled shaders are restored automatically by translation-chain ID, game-input
checksum, and shader variant list. Host-only fixes do not invalidate them. No run
ID is required. A new chain or missing cache compiles once and saves only that
chain's output, excluding the large per-shader CAS and old chains. SPIRV-Cross has
a separate versioned cache. The IPA uploads before the shader cache is saved.
On the first use of the new cache, matching legacy output is restored and checked
against a successful same-input, same-chain build before reuse. Cold compilation
prints progress every 30 seconds, including translated shaders and built libraries.

Device captures in the separate `captured-shaders-20261016.zip` input asset add
48 byte-identified shaders before the chain is computed. No captured shader bytes
are committed. A changed chain restores the old content-addressed compiler cache
so unchanged shaders can be reused individually. Captured VS/PS interface padding
uses conservative relationships; actual pairings and rendering need device testing.
