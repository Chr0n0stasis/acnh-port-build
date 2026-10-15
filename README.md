# Private ACNH Apple build orchestration

Experimental adaptation for iPhone 13 (A15), not a verified playable port.

Uses seedds/smbw-mobile at the pinned commit in `ci/build.sh`, plus reviewed
local overlays. Source license remains that of the upstream MIT project.
No game binaries, extracted shaders, ROMs, or decryption keys are committed.
Only necessary build inputs are attached to a release in this private repository.

Actions builds unsigned iOS binaries and a macOS host. RomFS stays on the local
machine; the thin IPA expects the game's complete RomFS in Documents/romfs.
Cloud compilation does not establish device compatibility or memory usage.
