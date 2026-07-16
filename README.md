# silhouette

A production-grade, cross-platform C++20 UI rendering library, built around an
immediate-mode **sink** architecture: UI code emits draw calls; backends —
CPU rasterizers now, Vulkan and Metal next — consume them in place and retain
only what they each need.

Currently early in a ground-up rebuild: the core geometry, the emit seam, and
a fast CPU renderer (with a texture-free synthetic mode for exact
pixel-equivalence testing) are in. See `DESIGN.md` for the architecture, the
render ladder, and the engineering discipline; `CLAUDE.md` for build steps.

## Build

Windows (MSBuild + vcpkg manifest mode):

```powershell
msbuild silhouette.sln /p:Configuration=Debug /p:Platform=x64 /m
bin\Debug\silhouette_tests\silhouette_tests.exe
```

## License

MIT — see `LICENSE`.
