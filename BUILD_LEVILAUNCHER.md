# Build World Analysis 1.5.2 for Minecraft 1.26.52

Requirements: Android NDK r28c, CMake 3.22+, Ninja, Python 3, Git.

```sh
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/android-ndk-r28c/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
  -DANDROID_STL=c++_shared -DCMAKE_BUILD_TYPE=Release
cmake --build build --target WorldAnalysis --parallel 2
```

Outputs: `build/libWorldAnalysis.so` and `build/WorldAnalysis.levipack`.
The package contains the ARM64 library, manifest, original font, and white
wireframe globe PNG icon. The editable SVG is included in the source.
Preloader and the shared C++ runtime are supplied by LeviLauncher and are not
bundled in the mod. CMake pins the Preloader API revision used for this build.

The included GitHub Actions workflow uses these same build settings.
The original xmake project is retained as an alternative; the delivered binary
was built with CMake.

To repeat the signature check against your own local copy of the supplied binary:

```sh
python3 scripts/verify_dot52.py /path/to/dot52values.so
```

The Minecraft binary is intentionally not included in either output archive.
