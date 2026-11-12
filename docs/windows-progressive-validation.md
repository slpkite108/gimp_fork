# Windows progressive-startup validation

Date: 2026-11-12. Tested code: `ef444a24bf1e3bbda1488bebda4a5b29778578a9`.

The Windows build and focused functional checks passed. This establishes a
working development build, not release readiness or a cold-start speedup.

**Measurement correction (2026-11-12):** the original 30 startup samples below
used an unfinished first-run profile. The automation quit GIMP while the welcome
dialog was still open, preventing startup metadata from being saved. Every run
therefore queried all 140 plug-ins again. These samples must not be used as
normal cached-startup measurements or to judge the progressive loader's benefit.
After closing the welcome dialog normally, two diagnostic restarts initialized
in 2.622 and 2.564 seconds. See the root-cause investigation below.

## Environment and build

- Windows 11 Education, build 26300, x86-64; Ryzen 5 7500F, 6 cores / 12 threads.
- GIMP 3.3.1; gimp-data `dba1b6d3d5e0703c5337c9ec32805ac017dc1270`.
- Private portable MSYS2 CLANG64 toolchain; Clang 23.1.5, Meson 1.12.1,
  Ninja 1.13.2, GEGL 0.4.72, GTK 3.24.52, GLib 2.90.1.
- Meson `debugoptimized`, optimization level 2, assertions enabled. This is
  a development configuration, not a tuned release package.
- Build directory `build-progressive`; private install prefix
  `build-progressive/prefix`. No files were installed over the existing GIMP.

With the CLANG64 dependencies and WinFlexBison on PATH, configuration used:

```powershell
meson setup build-progressive "--prefix=$((Get-Location).Path)\build-progressive\prefix" `
  '-Drelocatable-bundle=no' '-Dbuild-id=org.gimp.ProgressiveFork' `
  '-Dcheck-update=no' '-Dgi-docgen=disabled' '-Dheadless-tests=disabled' `
  '-Denable-default-bin=disabled'
meson compile -C build-progressive -j 6
meson install -C build-progressive --no-rebuild
```

The full build completed. The private prefix supplies data and plug-ins;
runtime still requires the toolchain DLLs and the appropriate PATH,
PKG_CONFIG_PATH, XDG_DATA_DIRS and GI_TYPELIB_PATH. This is not a standalone
installer or portable distribution.

## Fixes required to reach a running build

- Include the context declarations used by the incremental loader, and the
  GTK/widget declarations used by its test. Keep declarations before statements.
- Ignore `.rc`, `.o`, `.obj` and `.dummy` files, alongside `.pdb`, during Windows
  plug-in discovery. Previously the uninstalled test build tried to execute
  generated build artifacts and reported pipe/plug-in failures.
- Remove a duplicated block inside the PowerShell launcher's filename regex.
  The original script failed parsing before it could enforce its safeguards.
- Wait for GUI executable completion so `$LASTEXITCODE` is available, preserving
  the child exit status and restoring the caller's startup environment flags.
- Return zero explicitly after successful launcher tests so the fixture's
  expected failure code does not leak into the CI runner's exit status.

## Completed checks

| Check | Result |
|---|---|
| Full Windows compilation | Passed |
| `tools/test-progressive-isolation.py` | 5 checks passed |
| `tools/test-progressive-launcher.ps1` | Passed in PowerShell 7 and Windows PowerShell 5.1 |
| `core` suite | 4 cases passed |
| `gimpidtable` suite | 8 cases passed |
| `startup-loader` suite | 2 cases passed using the production palette loader |
| `xcf` suite | 4 cases passed |
| Empty GUI launch | Window and resource docks appeared; deferred work completed |
| PNG argument with spaces | 1591 x 1080 image opened as one layer; canvas inspected |
| XCF argument with spaces | 100 x 90 project opened with two layers |
| Installed GIMP 3.2.8 plus fork | Separate windows observed while the fork opened XCF with `--new-instance` |
| Existing user profiles | 137 files had identical paths, lengths and SHA-256 hashes before and after the coexistence checks |

The [GitHub Actions guardrail run](https://github.com/slpkite108/gimp_fork/actions/runs/43185752491)
passed both jobs after the test-runner exit-status correction (`6b36bc6`).
CI covers the static isolation checks and Windows launcher, not the full GIMP
build; the compilation and application suites above ran locally.

The installed binary used disposable config/cache/temp/data directories during
the coexistence test. Registry settings, shortcuts and file associations were
not changed. The PNG was a copy of the generated `gimp-data/images/gimp-splash.png`;
the XCF was a copy of `app/tests/files/gimp-2-6-file.xcf`. Both copies retained
their source hashes. This does not validate complex projects with custom fonts,
brushes or other external resources.

Reproduce the focused checks after preparing the build environment:

```powershell
python tools/test-progressive-isolation.py
.\tools\test-progressive-launcher.ps1
meson test -C build-progressive --no-rebuild startup-loader core gimpidtable xcf --print-errorlogs
.\tools\run-progressive-isolated.ps1 -BuildDir .\build-progressive `
  -Executable .\build-progressive\app\gimp-3.3.exe -Mode Cold
```

Use `-Mode Image -FilePath <copied.png>` or `-Mode Project -FilePath <copied.xcf>`
for file-opening checks. Add `-Baseline` to keep eager loading while preserving
the fork's private profile and `--new-instance` isolation.

## Superseded startup measurements

These results are **invalid as a normal cached-startup comparison**. These
runs measure the existing internal `app_run()` to `initialized` interval. They
exclude process creation/DLL loading, do not measure first paint or input
latency, and do not establish a production-release speedup.

One unreported eager warm-up preceded five alternating eager/progressive pairs
for each mode: empty, PNG, then XCF (30 recorded runs). The same binary,
configuration, machine, input and disposable profile were reused; only
`GIMP_PROGRESSIVE_STARTUP` differed. Caches were not flushed or controlled.
The launcher's `Cold` mode name means no input file; it does not make the OS
caches cold. The fixtures are small smoke-test inputs, not representative large
photos or complex projects. CPU utilization was 9% before measurement; other
desktop applications remained running. Alternation reduces but does not remove
that noise.

Every run required the `initialized` marker, an observed application window,
the expected image/project metadata for file modes, and a normal zero exit.
Each window was closed through the UI. The welcome dialog was still present
when quitting. No critical errors, GIMP error messages or plug-in pipe failures
appeared in these logs; the stale-context shutdown warning remained on both
sides. The source fixtures were not saved or edited.

[Raw samples](windows-startup-samples.csv) contain all 30 successful runs.
Times below are seconds, median [minimum, maximum], five runs per cell.

| Mode | Eager baseline | Progressive flag |
|---|---|---|
| Empty | 20.789 [20.624, 20.898] | 20.480 [20.342, 20.541] |
| PNG | 21.070 [20.973, 21.160] | 20.896 [20.852, 20.913] |
| XCF | 20.559 [20.507, 20.822] | 20.594 [20.491, 20.676] |

The values are retained as an audit record of the flawed test setup. No
eager-versus-progressive performance conclusion should be drawn from them.

The dominant timed interval was `restore_signal`: group medians were
19.727–19.986 seconds, approximately 94–98% of the corresponding initialization
medians. This aggregate includes plug-in restoration, babl conversion setup and
GUI restoration callbacks. The follow-up below identifies repeated plug-in
queries as the dominant cause in this test setup.

## Root cause: incomplete first-run state

Timestamped `--verbose` output exposed the following sequence:

1. `welcome_dialog_create()` disables configuration autosaving while its dialog
   is open (`app/dialogs/welcome-dialog.c`).
2. Startup then sets `config-version` and `prev-language` in memory
   (`app/app.c`). Closing the whole application while the welcome dialog is
   still open did not write these fields to the test profile's `gimprc`.
3. At the next launch, missing `prev-language` sets `gimp->query_all = TRUE`.
   `gimp_plug_in_manager_query_new()` consequently queries every plug-in,
   despite the presence of a valid `pluginrc` cache.
4. Each query starts a plug-in process, including Python interpreters. The
   unacknowledged-profile diagnostic spent about 18.73 seconds between the
   first query and plug-in initialization. This explains most of the previous
   approximately 20-second restore interval.

Clicking the welcome dialog's **Close** button invokes
`welcome_dialog_response()`, which calls `gimp_rc_save()`. The test profile then
contained `prev-language` and `config-version`; the next two restarts queried
only three RAW bridge plug-ins instead of all 140.

| Diagnostic run | Plug-ins queried | Internal initialization |
|---|---:|---:|
| Unacknowledged first-run profile, repeated launch | 140 | 21.126 s |
| After closing the welcome dialog normally | 3 | 2.622 s |
| Another restart using that saved profile | 3 | 2.564 s |

All three runs used the same binary and progressive flag. The difference is
profile initialization, **not an improvement made to the progressive algorithm**.
These are instrumented diagnostic runs, not a five-pair performance benchmark.
The first post-install trace also queried 140 plug-ins and took 34.006 seconds;
it is not used as the steady-state baseline because installation had refreshed
the plug-in files. The original application's saved profile was not modified.

Both diagnostic restarts returned zero but logged a
`gimp_device_info_save_tool: GIMP_IS_DEVICE_INFO (info)` critical assertion
during shutdown. Its cause is unresolved and separate from the startup timing;
these runs do not establish clean shutdown or release readiness. The normal
launcher's separate fork profile already contained the required startup metadata.

The earlier test procedure failed to complete first-run setup. A valid new
comparison must close the welcome dialog, verify both metadata fields, confirm
cache reuse, then alternate eager/progressive runs in fresh result files. Do
not reuse or overwrite the superseded samples. First-paint, input latency and
true OS cold start still require separate measurement.

## Remaining limits and warnings

- The full matrix in [progressive-startup.md](progressive-startup.md) remains
  incomplete, including complex project fidelity, every resource-demand path,
  path changes during deferred work, and broad plug-in coverage.
- The three application test suites that create a GIMP instance each emitted a
  nonfatal writable-resource-folder/search-path warning during teardown. All
  18 assertions passed and no plug-in launch errors remained. These warnings
  have not been resolved.
- GUI logs contain development warnings about unsupported fonts, stale contexts
  at exit, and missing cached XML cleanup for opened images. They occur on the
  baseline too; normal test exits returned zero. Their underlying causes remain
  unverified and should not be described as fixed by this work.
- Default IPC routing without `--new-instance` was not exercised. The runtime
  coexistence result covers the guarded launcher, not that additional path.
- No installer, file-association integration, macOS packaging, or Linux runtime
  coexistence test was performed.
