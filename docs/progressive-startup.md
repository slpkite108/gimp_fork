# GIMP Progressive Startup — Phase 0: measurement

This branch experiments with faster cold startup while preserving GIMP behavior. **No resource loading has been deferred yet.** Its first change is opt-in, monotonic startup profiling to establish a baseline before touching initialization dependencies.

## Source changes

- `app/app.c`: optional application startup milestones.
- `app/core/gimp.c`: optional breakdown of resource restoration.
- Enable only with `GIMP_STARTUP_PROFILE=1`. With the variable unset, the logging statements do not execute.
- Output goes to stderr, in CSV-like records, not to persistent files.

Example output (illustrative values only):

```text
GIMP_STARTUP_PROFILE,app_run_enter,1,1
GIMP_STARTUP_PROFILE,gimp_new_completed,210000,209999
GIMP_RESTORE_PROFILE,parasites,5000,5000
GIMP_RESTORE_PROFILE,data_factories,920000,915000
GIMP_STARTUP_PROFILE,resources_restored,1540000,1040000
```

The numeric fields are **elapsed microseconds** and **delta microseconds**. `GIMP_STARTUP_PROFILE` is relative to entering `app_run()`; `GIMP_RESTORE_PROFILE` is relative to entering `gimp_restore()`. These are separate clocks; do not directly compare their elapsed timestamps. The delta includes time since the previous marker in the same series.

## Windows / PowerShell

Build the modified GIMP executable using the official GIMP Windows build instructions, and test that executable rather than an existing system installation. In PowerShell:

```powershell
$env:GIMP_STARTUP_PROFILE = "1"
& "PATH_TO_YOUR_BUILT_GIMP.exe" --no-splash 2> startup-profile.log
Remove-Item Env:GIMP_STARTUP_PROFILE
```

Replace the executable path with the actual compiled path. Close all existing GIMP processes before each startup test: single-instance activation can bypass the full initialization path. The environment flag must be set in the process being measured.

For fair comparisons, retain the same commit, build options, GIMP profile, fonts, plug-ins, and OS state. Perform several runs and report median and tail durations. Compare process cold launch and reboot / OS-cache-cold launch separately.

## Important measurement limitations

- `app_run_enter` is **not process creation time**; dynamic loading and earlier initialization are excluded. Use an external process tracer for whole-process startup.
- `initialized` is **not proof of first drawable frame or UI responsiveness**. For time-to-first-frame and time-to-interactive, instrument the GUI event loop and test external input response separately.
- The parent `resources_restored` interval includes more work than the nested `gimp_restore()` timing. Be careful when subtracting timings.
- Some loaders are already asynchronous, so a completed function call does not necessarily mean all resource work is complete.
- No speedup, memory reduction, or successful Windows build has been established yet.

## Planned optimization sequence

1. Verify Windows build and inspect baseline startup traces.
2. Build a resource / UI dependency map from `gimp_initialize()`, `gimp_restore()`, and the GUI restore callbacks.
3. Identify critical-path resources, cache-only metadata, and optional assets that can be loaded on first use.
4. Introduce one opt-in deferred-loading candidate at a time with explicit readiness / failure / cleanup states.
5. Verify feature compatibility, image opening, plug-ins, sessions, and cold / warm launches. Compare against an unchanged baseline build.

Keep upstream changes easy to merge. Do not implement eager-work skips without a verified consumer readiness path.
