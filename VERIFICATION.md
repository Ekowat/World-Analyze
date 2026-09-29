# Minecraft 1.26.52 compatibility update — World Analysis 1.5.2

## Target and scope

Target: supplied ARM64 `dot52values.so`, containing the version string
`1.26.52.03_RC2`.

SHA-256: `5f5baf8e9ded92432e4c2cc9ff58a9313245323e8ca53ce37c169cc720e757c4`.

This is a compiled, statically checked compatibility build. Minecraft and the
Levi menu were not run on an Android device during this update. Device testing
is still required; byte-pattern uniqueness alone does not prove the semantics
of a function or every object-layout field.

## Signature changes

- 64 original signatures still matched uniquely and were retained.
- 46 signatures were replaced with exact bytes from the supplied dot52 ELF.
- The resulting 110/110 patterns each match exactly once at the recorded RVA.
- `compatibility/dot52-bindings.json` records every pattern, RVA, and target hash.
- `scripts/verify_dot52.py` reproduces the byte-matching audit without external Python dependencies.

Changed function identities were recovered using pattern comparisons and, for
ambiguous cases, AArch64 disassembly, native call targets, and RTTI/vtable context.
In particular, navigation ticks use the relevant PathNavigation-family virtual
slot 3, the local-player getter uses ClientInstance slot 32, GameMode attack is
reached from SurvivalMode attack, and the item-damage accessor reads the Damage
NBT value. Exact new patterns deliberately target this binary; they are not a
promise of compatibility with another hotfix.

## Corrected layout bindings

| Binding | Previous | dot52 | Evidence |
|---|---:|---:|---|
| ClientInstance getGuiData virtual slot | 235 | 236 | Primary vtable at 0x12BC3300; accessor 0x9818A8C loads this+0x5C0. |
| BlockSource getDimensionId virtual slot | 18 | 19 | Slot 19 at 0xFB8F214 forwards to Dimension virtual slot 3; slot 18 is a region query. |
| ClientInstance level-renderer pointer | 0x190 | 0x1A0 | Accessor 0x9816524 and its neighboring renderer caller use this+0x1A0. |

Other field constants were retained. Selected render, actor, item and world
accessors were inspected; every retained SDK field was not independently
verified. Their full correctness must not be inferred from the signature count.

## Levi menu integration

The existing registry contains all 11 modules, all configured to show in the
menu. Registration now happens in the loader enable lifecycle before game-hook
readiness, and each entry explicitly uses the loader-provided mod owner ID.
A missing game hook therefore no longer suppresses the entire module list.

Game-dependent module initialization still waits for the native game hooks.
Enabled modules receive their enable callback after initialization, preserving
startup behavior for saved settings and early menu toggles.

The build uses Preloader headers at commit
`e0a61cbcc46b7fc84f977958f2bbeac47b6fe856`. Imported native API symbols were
checked against the Preloader library shipped in LeviLauncher Android v1.5.24.
This is an API/symbol check, not a visual test of the Android menu.

## Preserved module implementations and packaging

All 29 files under `src/modules/` are byte-for-byte identical to the uploaded ZIP.
`compatibility/module-source-hashes.json` records their original/current hashes.
The original font is retained. The icon is a transparent 512px white wireframe
globe, with its editable SVG in `resources/icon.svg`.

The ARM64 release was built with Android NDK r28c, API 28, C++20, shared libc++,
and 16 KiB ELF segment alignment. The library exports `PLGetModRegistration`.
The launcher supplies Preloader and libc++; neither is duplicated in the package.
The archive contains only the manifest, mod library, font, and icon.
The supplied Minecraft binary is not redistributed.
