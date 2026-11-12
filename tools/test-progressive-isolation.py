#!/usr/bin/env python3
"""Static guardrails that prevent accidentally sharing a system GIMP identity.

Run from any directory: python tools/test-progressive-isolation.py

This is not a build or runtime test. It checks fork-specific namespace choices
and the Windows launcher safety requirements.
"""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


class ForkIsolationTests(unittest.TestCase):
    def test_private_user_paths_are_forced_before_environment_init(self):
        app = source("app/main.c")
        for variable in ("GIMP3_DIRECTORY", "GIMP3_CACHEDIR", "GIMP3_TEMPDIR"):
            self.assertIn(f'g_setenv ("{variable}",', app)
        self.assertIn('g_setenv ("GIMP_PROGRESSIVE_FORK_ISOLATED", "1", TRUE);', app)
        self.assertIn("GIMP-Progressive-Fork", app)
        self.assertLess(app.index("if (! gimp_progressive_fork_isolate ())"),
                        app.index("gimp_env_init (FALSE);"))

    def test_fork_does_not_migrate_installed_gimp_settings(self):
        app = source("app/core/gimp-user-install.c")
        self.assertIn('"GIMP_PROGRESSIVE_FORK_ISOLATED"', app)
        self.assertLess(app.index('g_strcmp0 (g_getenv ("GIMP_PROGRESSIVE_FORK_ISOLATED")'),
                        app.index('user_install_detect_old (install, gimp_directory ());'))

    def test_windows_mutex_and_hidden_receiver_are_private(self):
        main = source("app/main.c")
        unique = source("app/gui/gui-unique.h")
        self.assertIn("gimp_progressive_fork_primary_instance_running", main)
        self.assertNotIn('L"Local\\\\gimp_primary_instance_running"', main)
        self.assertIn("GimpProgressiveForkUniqueHandler", unique)
        self.assertIn("GimpProgressiveForkProxy", unique)
        self.assertNotIn('L"GimpProxy"', unique)
        self.assertIn('L"gimp.ProgressiveFork"', main)

    def test_application_and_linux_dbus_identities_are_private(self):
        app = source("app/gui/gimpapp.c")
        service = source("app/gui/gimpdbusservice.h")
        manager = source("app/gui/gui-unique.c")
        self.assertIn('"org.gimp.ProgressiveFork"', app)
        self.assertIn('"org.gimp.ProgressiveFork.UI"', service)
        self.assertIn('"/org/gimp/ProgressiveFork/UI"', service)
        self.assertIn('"/org/gimp/ProgressiveFork"', manager)
        self.assertNotIn('#define GIMP_DBUS_SERVICE_NAME   "org.gimp.GIMP.UI"', service)

    def test_guarded_launcher_cannot_target_system_executable(self):
        launcher = source("tools/run-progressive-isolated.ps1")
        self.assertIn("intro-buildoptions.json", launcher)
        self.assertIn("Executable does not contain the fork isolation markers", launcher)
        self.assertIn("GIMP_PROGRESSIVE_FORK_ISOLATED", launcher)
        self.assertIn("--new-instance", launcher)
        for dangerous in ("New-ItemProperty", "Set-ItemProperty",
                          "Remove-ItemProperty", "meson install", "reg add"):
            self.assertNotIn(dangerous, launcher)


if __name__ == "__main__":
    unittest.main(verbosity=2)
