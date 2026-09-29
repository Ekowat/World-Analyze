# World Analysis

World Analysis is an open-source native mod for Minecraft Bedrock on Android, built for LeviLauncher.

It adds tools for inspecting the world, nearby mobs, blocks, updates, containers, and other in-game information.

## Features

- Block and mob analysis
- Container / inventory viewer
- World and system notifications
- Block, redstone, crop, fluid, and spawn update viewer
- Technical world effects
- Analysis console and HUD controls
- Mob counter and mob pathfinding
- World scanning
- Ocean depth detection

## Requirements

- Android
- `arm64-v8a`
- LeviLauncher
- Minecraft Bedrock **1.26.52** for the current build

## Build

Requirements:

- Android NDK r28c
- xmake
- CMake
- Python 3

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

The build produces:

- `libWorldAnalysis.so`
- `WorldAnalysis.levipack`

The project also includes CMake configuration for Android builds.

## Compatibility

Minecraft signatures and offsets are version-specific. The current source targets Minecraft Bedrock **1.26.52**. See `VERIFICATION.md` for the validation scope used for this release.

## Credits

World Analysis is a modified derivative work based on **[BedrockTools](https://github.com/QYCottage/BedrockTools)** by **[RadiantByte](https://github.com/RadiantByte)**.

BedrockTools is licensed under the **GNU General Public License v3.0 (GPL-3.0)**. Its license and attribution are preserved in this repository. See [`NOTICE.md`](NOTICE.md) and [`LICENSE`](LICENSE).

World Analysis is not affiliated with, endorsed by, or sponsored by BedrockTools, RadiantByte, or QYCottage.

## License

The World Analysis source is released under **GNU GPL-3.0-only**. See [`LICENSE`](LICENSE).

Third-party components retain their own licenses. See [`NOTICE.md`](NOTICE.md) for the attribution and licensing notes relevant to this repository.
