# silhouette — build & conventions

C++20 cross-platform UI rendering library. Read `DESIGN.md` first — it is the
design contract (sink architecture, render ladder, equivalence relation,
coverage discipline).

## Build (Windows / MSBuild + vcpkg manifest mode)

```powershell
& "$(& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" `
    -latest -find MSBuild\**\Bin\MSBuild.exe)" silhouette.sln /p:Configuration=Debug /p:Platform=x64 /m
bin\Debug\silhouette_tests\silhouette_tests.exe      # the gate
```

- Configurations: **Debug** (iteration), **DebugAsan** (AddressSanitizer —
  run the suite under it before landing), **Release**. Each installs the
  vcpkg manifest into its own tree (`vcpkg_debug` / `vcpkg_debugasan` /
  `vcpkg_release`) so CRT/ASan variants never collide; DebugAsan builds
  dependencies ASan-instrumented via the in-repo overlay triplet
  (`triplets/x64-windows-static-md-asan.cmake`).
- vcpkg (manifest mode, `vcpkg.json`): gtest, unordered-dense. The MSBuild
  vcpkg integration resolves them on first build; base triplet
  `x64-windows-static-md`.
- Tests are plain GTest console binaries — Visual Studio's Test Explorer
  discovers them from the solution; `msbuild` + running the exe is the CLI
  path.
- `common.props` is the single source of build settings; every `.vcxproj`
  imports it and declares only its ConfigurationType, GUID, and sources.

## Conventions

- **C++20, namespace `ui`.** Headers included as `core/…`, `paint/…`,
  `render/…` from `src/`.
- **Warnings are errors** (`/W4 /WX`; external headers at W3). Fix the code,
  never relax the flag.
- **Every commit green; every changed line tested in the same commit.** The
  suite runs in milliseconds — there is no excuse. Bias to many small
  commits.
- **Geometry is `constexpr`** wherever it can be. Record what each constant
  was folded from, in a comment.
- **Determinism**: renderers produce byte-identical output for identical
  input — the pixel-match gate depends on it. No wall-clock, no randomness in
  render paths (synthetic values come from explicit deterministic hashes).
- **No file IO in library code** — runtime code takes bytes/pixels from the
  caller; only tools and tests touch the filesystem.
- **Single-threaded per UI context.** No internal locks.
- **General-purpose vocabulary only** — see "The vocabulary law" below; it
  is enforced by local hooks, not just requested.
- Commits are authored anonymously as `Claude <noreply@anthropic.com>` (the
  repo-local git config pins this).

## The vocabulary law (mandatory — and enforced)

This library is general-purpose and public-bound. Nothing in the repo may
name, quote, or hint at any consuming application or any proprietary
implementation its behavior was validated against — in code, comments,
tests, fixtures, docs, generated/emitted text, or commit messages.

- **State ported behavior as THIS library's own law**: what the rule is
  and why it is right on its own terms (determinism, the integer pixel
  grid, spec consistency). Never "matching X"; never a foreign class,
  method, file, or script citation.
- **Commit messages describe THE CHANGE from the library's point of
  view.** A consumer appears only as "a consumer" / "a converted module",
  and only when technically load-bearing. If a sentence reads from the
  wrong angle, rewrite the sentence — no euphemism word-swaps; a reader
  must never sense a hidden upstream or raise an eyebrow.
- **Example identifiers in tests and docs are invented and neutral**
  (`round | square`, `solid | liquid | gas`, `light | dark`) — never
  lifted from a consumer's schema fields, asset paths, style names,
  colour spellings, or scripts.
- **Design rulings carry no person's name.** State the rule; "by design"
  if an attribution is needed.
- **No trailers of any kind** on commits — no authorship trailers, no
  session links.
- **Enforced**: local `pre-commit` and `commit-msg` hooks reject
  violations. A hook failure is a bug in your text — fix the text;
  `--no-verify` is never the answer. If the hooks are missing (fresh
  clone), STOP and reinstall them before committing (the maintainer's
  workspace documents how).

## Layout

```
src/core/      Rect/Vec2/Color (constexpr value types)
src/paint/     sink.hpp — the Sink concept (emit + font surface, ids, flags)
src/render/    cheap_raster (fast CPU sink, real/synthetic texture modes),
               pixel_match (the equivalence harness)
tests/         GTest — geometry, raster pixels, equivalence
ext/slughorn/  submodule (quality text, later stage)
```

## Current state

Early rebuild. Landed: core geometry, the emit seam (the Sink concept —
called directly, no emit-side state), the cheap CPU renderer (both texture
modes, full font surface) with the pixel-match harness. Next per DESIGN.md's
render ladder: quality CPU renderer (slughorn + HarfBuzz text), then Vulkan,
then Metal.
