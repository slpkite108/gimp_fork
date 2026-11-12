# GIMP Progressive Fork: isolation from installed GIMP

**Scope:** developer builds from the `feature/progressive-startup` branch.
This fork is an experimental version of GIMP, not a replacement for a locally
installed copy. It should run side by side with the installed application.

## Isolation implemented in this branch

| Shared resource | Installed GIMP | Progressive Fork |
| --- | --- | --- |
| User settings | Existing default GIMP profile | `GIMP-Progressive-Fork/<GIMP_USER_VERSION>` below the user's config root |
| Cache | Existing GIMP cache | `GIMP-Progressive-Fork/<GIMP_USER_VERSION>` below the user's cache root |
| Temporary files | Existing GIMP temp directory | `GIMP-Progressive-Fork/<GIMP_USER_VERSION>` below the OS temp root |
| Windows process mutex | `Local\\gimp_primary_instance_running` | `Local\\gimp_progressive_fork_primary_instance_running` |
| Windows hidden IPC class/window | `GimpWin32UniqueHandler` / `GimpProxy` | `GimpProgressiveForkUniqueHandler` / `GimpProgressiveForkProxy` |
| Windows taskbar AppUserModelID | `gimp.GimpApplication` | `gimp.ProgressiveFork` |
| GApplication ID | Original GIMP application ID | `org.gimp.ProgressiveFork` |
| Linux D-Bus well-known name | `org.gimp.GIMP.UI` | `org.gimp.ProgressiveFork.UI` |
| Linux D-Bus object path | `/org/gimp/GIMP/UI` | `/org/gimp/ProgressiveFork/UI` |

The D-Bus *interface schema* stays `org.gimp.GIMP.UI`, as it must match the
code generated from `app/gui/dbus-service.xml`. The fork uses a different
bus name and object path, which prevents either application from contacting
the other's service.

The fork forces the private config/cache/temp paths at process startup,
**before** `gimp_env_init()`. It does not trust inherited `GIMP3_DIRECTORY`,
`GIMP3_CACHEDIR`, or `GIMP3_TEMPDIR` values. The first-run installer is
prevented from migrating an existing GIMP user profile; it creates fresh
defaults for the fork in the private location.

## Windows: safe testing from a build tree

Build this branch in a **separate Meson build directory** and do not copy its
binaries, DLLs or plug-ins into the installed GIMP directory.

Example PowerShell invocation (adjust the executable version to the actual
file created by the build):

```powershell
.\tools\run-progressive-isolated.ps1 -BuildDir .\build-progressive -Executable .\build-progressive\app\gimp-3.2.exe -Mode Cold

.\tools\run-progressive-isolated.ps1 -BuildDir .\build-progressive -Executable .\build-progressive\app\gimp-3.2.exe -Mode Image -FilePath C:\test\sample.png

.\tools\run-progressive-isolated.ps1 -BuildDir .\build-progressive -Executable .\build-progressive\app\gimp-3.2.exe -Mode Project -FilePath C:\test\sample.xcf
```

The launcher:
1. Requires a Meson build directory and an executable located inside it.
2. Verifies the EXE contains this fork's isolation markers (prevents accidentally launching the standard GIMP binary).
3. Uses `--new-instance` so this test does not send file-open messages to an existing process.
4. Enables or disables the progressive loader using `-Baseline`, without affecting the installed GIMP.
5. Does not write registry entries, install files, change desktop shortcuts, or change file associations.

To run the original eager path using the isolated fork binary:

```powershell
.\tools\run-progressive-isolated.ps1 -BuildDir .\build-progressive -Executable .\build-progressive\app\gimp-3.2.exe -Mode Cold -Baseline
```

The executable itself also enforces the private profile/IPC names even when
launched directly rather than through the PowerShell helper.

## Guardrail tests

```shell
python tools/test-progressive-isolation.py
```

This static test checks the private directory setup, timing (before GIMP env
initialization), user-profile migration prevention, Windows identifiers,
D-Bus identifiers and the guarded launcher. It is **not** a complete Windows
runtime or distribution-installation test.

## What is not yet isolated

- **Do not run `meson install` into a prefix already containing installed
  GIMP.** This branch still uses standard GIMP names for many installed
  binaries, DLLs, plug-ins, desktop files and data resources. Use a wholly
  separate install prefix when packaging the fork; only launching from a
  separate build directory is covered by the helper.
- Do not set Windows Open With / default-program associations to the fork
  during development. The launcher passes test file names explicitly and
  does not alter those associations.
- A macOS `.app` packaging identifier / platform file association is not
  changed by this work. Avoid installing the fork over an existing app bundle.
- Manual installs into the same installation prefix, shared external plug-in
  paths, or explicit changes to GIMP preferences can bypass the isolation.
  Test in disposable directories and with copies of project files.

This is a source-level isolation implementation. **A successfully compiled
Windows build and side-by-side runtime test have not yet been verified.**
