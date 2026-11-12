# GIMP Progressive Startup — experimental implementation

**Status (2026-11-12): code committed, Windows build and runtime behavior NOT verified.** The entire experiment is OFF by default. Keep the original startup behavior as the control.

## Goals

- Respect the startup intent: empty application, file-associated image, or file-associated project.
- Make interactive UI and the requested content usable before loading unrelated resource files.
- When a user action requires a pending resource, make it a hard dependency and load it before the feature uses that resource.
- Preserve existing projects, font semantics, plug-in registration, user presets, tags, and shutdown persistence.
- Prevent concurrent access to non-thread-safe GIMP/GTK objects from worker threads.

## Actual code paths

| Launch profile | Detection | Eager path | Deferred path |
|---|---|---|---|
| EMPTY | No file arguments | Core GIMP/GUI, brushes, dynamics, asynchronous font enumeration, plug-in registration | MyPaint brushes, patterns, palettes, gradients, tool presets |
| IMAGE | File names have recognized raster image extensions | Core GIMP/GUI, image-loading plug-ins, brushes, dynamics, asynchronous font enumeration, command-line image opening | Same five groups, with image-focused background priority |
| PROJECT | XCF, unknown formats, or any mixed launch containing an unknown/project file | Original eager resource loading, with font enumeration started first | None |

File classification is conservative and extension-based. A file with an unknown extension is assigned PROJECT, not IMAGE. This avoids skipping dependencies for project-like formats, but can classify uncommon raster formats as PROJECT. MIME/content-aware detection is **not yet implemented**. For multiple file arguments the most conservative classification wins. An existing GIMP instance receives files via the single-instance mechanism and is not a new process cold start.

### Resource ordering

- EMPTY auto-load: tool presets → palettes → gradients → patterns → MyPaint brushes.
- IMAGE auto-load: patterns → gradients → palettes → MyPaint brushes → tool presets.
- PROJECT: no deferred factories; start existing asynchronous font discovery early, then retain original eager factory ordering.

In the deferred path the initial `gimp_data_factory_data_init(..., TRUE)` creates standard fallback data and config/path notifications, with disk traversal postponed until `gimp_data_factory_data_refresh()`. Tag cache container subscriptions are registered before deferred objects are added, so tag additions should still be applied. This assumption requires regression tests.

### Demand escalation

- `gimp_data_factory_get_data()` promotes a pending factory before returning a named resource.
- Generic `gimp_data_factory_get_container()` **does not promote** a resource: internal GimpContext construction queries all factory containers and would otherwise defeat all deferral.
- `gimp_data_factory_view_new()` explicitly promotes a factory when an actual resource dock is requested.
- A change to the active **user context tool** promotes MyPaint brushes, gradients, or patterns where relevant; internal tool-options contexts do not count as user requests.
- Additional consumers can call `gimp_data_factories_startup_require_container()` once bootstrap completes to request a fully populated factory explicitly.
- `gimp_data_factories_wait()` requests *all* pending factories before waiting for existing async work, preserving project/fully-restored semantics.
- The main-loop idle queue processes **one resource category per iteration** at `G_PRIORITY_LOW`. A category demanded by a feature is removed from pending and synchronously refreshed.
- During normal shutdown, unfinished resources are completed before saving resources/tag metadata and before plug-in and module teardown. The idle source is removed before factory destruction.

**Important limitation:** loading is incremental at the *category* level, not at the individual file level. A synchronous factory refresh blocks the GTK main thread until that category finishes. An already-running category **cannot be preempted**, and UI events cannot be processed in its middle. There is no background worker thread for these synchronous factory refreshes yet. Per-file cooperative loading and a cancellable/priority-aware work queue are required before claiming full interactive preemption.

## Enable the experiment

Set `GIMP_PROGRESSIVE_STARTUP=1` only when running a compiled executable built from this branch. Leave unset to use the unchanged baseline.

### Windows PowerShell

```powershell
$env:GIMP_PROGRESSIVE_STARTUP = "1"
$env:GIMP_STARTUP_PROFILE = "1"
& "PATH_TO_YOUR_BUILT_GIMP.exe" --no-splash 2> startup-profile.log
Remove-Item Env:GIMP_PROGRESSIVE_STARTUP
Remove-Item Env:GIMP_STARTUP_PROFILE
```

Test each startup profile independently, with all existing GIMP processes closed:

```powershell
# EMPTY
& "PATH_TO_YOUR_BUILT_GIMP.exe" --no-splash

# IMAGE
& "PATH_TO_YOUR_BUILT_GIMP.exe" --no-splash "C:\\test\\example.png"

# PROJECT
& "PATH_TO_YOUR_BUILT_GIMP.exe" --no-splash "C:\\test\\example.xcf"
```

These are manual examples, not completed test runs. Test with non-critical files in an isolated GIMP user profile. Do not overwrite actual project files before regression tests pass.

## Performance profiling

Set `GIMP_STARTUP_PROFILE=1`. The branch emits CSV-like records on stderr:

```text
GIMP_STARTUP_PROFILE,app_run_enter,1,1
GIMP_STARTUP_PROFILE,core_initialized,500000,220000
GIMP_RESTORE_PROFILE,data_factories,900000,850000
GIMP_PROGRESSIVE_STARTUP,mode,image
GIMP_PROGRESSIVE_STARTUP,resource,pattern factory,background,50000
GIMP_PROGRESSIVE_STARTUP,resource,palette factory,user-request,30000
```

All numbers in this block are **illustrative**. Startup markers report elapsed microseconds since `app_run()` and a delta since the previous marker. Restore markers report elapsed microseconds since `gimp_restore()` and their own delta. Deferred resource markers report the duration in microseconds for one factory refresh. Do not subtract timestamps across these series.

`app_run_enter` is not process creation (DLL loading is excluded). `initialized` is not first-frame rendering or verified interactivity. Add OS-level process tracing and a GUI first-paint/input-responsiveness probe before benchmarking whole cold start.

## Required regression matrix — not executed

| Scenario | Required behavior |
|---|---|
| Empty launch without env flag | Byte-for-byte existing eager path behavior in common code |
| Empty launch with flag | Default UI works; deferred categories eventually populate |
| PNG/JPEG via association | Open image successfully before optional background factories finish |
| XCF with text and custom brushes/gradients/patterns | Correct restored rendering, resource references and fonts |
| Unknown extension, mixed PNG+XCF | Conservative PROJECT path |
| Open XCF from UI while deferred | Fully materialize dependencies before file parsing |
| Open corresponding resource dock or select gradient/MyPaint/pattern tool before auto-load | User request promotes factory once, no duplicate registration |
| Feature request during a different factory load | Eventual promotion when main-loop control returns (not mid-call preemption) |
| Quit immediately while resources pending | All resource state and tags safely written; no dangling idle source |
| Change resource path during deferred load | Correct refresh semantics, no duplicates |
| Plug-ins, extensions, recovered session, multiple image files | No behavioral regression |
| User passes --no-data, --no-interface, --batch, --quit | Legacy initialization path |
| Cold / warm launches | Compare p50/p95 startup, first canvas display, first input latency, background completion, RAM and disk I/O |

## Known risks and next iteration

1. Some plug-in and UI consumers may cache factory containers before resources are populated. Container access alone is not promoted, because internal context creation references every category. Audit genuine consumers and add explicit demand hooks where required; validate change notifications for dockables and extension-provided resources.
2. The deferred category may still be a large, blocking directory traversal. Break directory enumeration and parsing into cooperatively scheduled units (or split I/O and main-thread object creation) to allow true action priority changes.
3. Direct `get_container()` calls after initialization may cause eager promotion even when merely querying metadata; trace these accesses to reduce unnecessary blocking.
4. Project detection should eventually use MIME sniffing / GIMP file handler information, while retaining a safe fallback for unknown formats.
5. A true user-visible readiness barrier should be tied to first paint and responsiveness, not the `gimp->initialized` assignment.
6. Restore may perform costly plug-in discovery before the first window; this remains untouched.

Do not claim a measured speedup or successful Windows build until separately verified. Baseline and experimental results should come from the **same source revision**, build configuration, machine and GIMP profile, differing only in the environment flag.
