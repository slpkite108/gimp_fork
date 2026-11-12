/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 2026 GIMP Progressive Startup contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "config.h"

#include <gegl.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

#include "widgets/widgets-types.h"
#include "core/gimp.h"
#include "core/gimpcontainer.h"
#include "core/gimpcontext.h"
#include "core/gimpdatafactory.h"
#include "core/gimpdataloaderfactory.h"
#include "core/gimppalette.h"
#include "core/gimppalette-load.h"

#include "gimp-app-test-utils.h"
#include "tests.h"


/* A real palette file tests the existing loader callback, nested directory
 * traversal, content registration, cancellation and full refresh. This is
 * not a synthetic scheduler-only test: it exercises production code.
 */
typedef struct
{
  gchar *base_dir;
  gchar *nested_dir;
  gchar *palette_file;
  gchar *old_path;
  gchar *old_writable_path;
} PaletteFixture;

static void
palette_fixture_init (Gimp           *gimp,
                      PaletteFixture *fixture)
{
  GError *error = NULL;
  const gchar *contents =
    "GIMP Palette\n"
    "Name: Startup Cooperative Test\n"
    "Columns: 1\n"
    "#\n"
    "255 0 0 Red\n"
    "0 255 0 Green\n";

  fixture->base_dir = g_dir_make_tmp ("gimp-progressive-test-XXXXXX",
                                      &error);
  g_assert_no_error (error);
  g_assert_nonnull (fixture->base_dir);

  fixture->nested_dir = g_build_filename (fixture->base_dir, "nested", NULL);
  fixture->palette_file = g_build_filename (fixture->nested_dir,
                                             "cooperative.gpl", NULL);
  g_assert_cmpint (g_mkdir_with_parents (fixture->nested_dir, 0700), ==, 0);
  g_assert_true (g_file_set_contents (fixture->palette_file, contents,
                                     -1, &error));
  g_assert_no_error (error);

  g_object_get (gimp->config,
                "palette-path", &fixture->old_path,
                "palette-path-writable", &fixture->old_writable_path,
                NULL);
  g_object_set (gimp->config,
                "palette-path", fixture->base_dir,
                "palette-path-writable", fixture->base_dir,
                NULL);
}

static void
palette_fixture_clear (Gimp           *gimp,
                       PaletteFixture *fixture)
{
  g_object_set (gimp->config,
                "palette-path", fixture->old_path,
                "palette-path-writable", fixture->old_writable_path,
                NULL);

  g_assert_cmpint (g_remove (fixture->palette_file), ==, 0);
  g_assert_cmpint (g_rmdir (fixture->nested_dir), ==, 0);
  g_assert_cmpint (g_rmdir (fixture->base_dir), ==, 0);

  g_free (fixture->palette_file);
  g_free (fixture->nested_dir);
  g_free (fixture->base_dir);
  g_free (fixture->old_path);
  g_free (fixture->old_writable_path);
}

static GimpDataFactory *
palette_factory_new (Gimp *gimp)
{
  GimpDataFactory *factory =
    gimp_data_loader_factory_new (gimp,
                                  GIMP_TYPE_PALETTE,
                                  "palette-path",
                                  "palette-path-writable",
                                  "palette-paths",
                                  gimp_palette_new,
                                  gimp_palette_get_standard);

  gimp_data_loader_factory_add_loader (factory, "GIMP Palette",
                                       gimp_palette_load,
                                       GIMP_PALETTE_FILE_EXTENSION, TRUE);
  return factory;
}

static void
test_cooperative_equals_eager (gconstpointer user_data)
{
  Gimp            *gimp = (Gimp *) user_data;
  GimpContext     *context = gimp_get_user_context (gimp);
  PaletteFixture   fixture = { 0, };
  GimpDataFactory *incremental;
  GimpDataFactory *eager;
  guint            steps = 0;

  palette_fixture_init (gimp, &fixture);
  incremental = palette_factory_new (gimp);
  eager = palette_factory_new (gimp);

  gimp_data_factory_data_init (incremental, context, TRUE);
  gimp_data_factory_data_init (eager, context, FALSE);

  g_assert_null (gimp_data_factory_get_data (incremental,
                                              "Startup Cooperative Test",
                                              NULL, FALSE));

  gimp_data_loader_factory_incremental_begin (incremental);
  while (gimp_data_loader_factory_incremental_step (incremental, context))
    {
      steps++;
      g_assert_cmpuint (steps, <, 100000);
    }

  g_assert_cmpuint (steps, >, 0);
  g_assert_nonnull (gimp_data_factory_get_data (incremental,
                                                "Startup Cooperative Test",
                                                NULL, FALSE));
  g_assert_nonnull (gimp_data_factory_get_data (eager,
                                                "Startup Cooperative Test",
                                                NULL, FALSE));

  g_assert_cmpint (gimp_container_get_n_children
                     (gimp_data_factory_get_container (incremental)),
                   ==,
                   gimp_container_get_n_children
                     (gimp_data_factory_get_container (eager)));

  /* Completed and cancelled scans are safe to stop a second time. */
  gimp_data_loader_factory_incremental_cancel (incremental);
  g_assert_false (gimp_data_loader_factory_incremental_step (incremental,
                                                              context));

  g_object_unref (eager);
  g_object_unref (incremental);
  palette_fixture_clear (gimp, &fixture);
}

static void
test_cancel_and_full_refresh (gconstpointer user_data)
{
  Gimp            *gimp = (Gimp *) user_data;
  GimpContext     *context = gimp_get_user_context (gimp);
  PaletteFixture   fixture = { 0, };
  GimpDataFactory *factory;

  palette_fixture_init (gimp, &fixture);
  factory = palette_factory_new (gimp);
  gimp_data_factory_data_init (factory, context, TRUE);

  gimp_data_loader_factory_incremental_begin (factory);
  g_assert_true (gimp_data_loader_factory_incremental_step (factory, context));

  /* A changed resource path does a full refresh and replaces the partial
   * enumeration. The old enumerator must not survive this transition.
   */
  gimp_data_factory_data_refresh (factory, context);
  g_assert_false (gimp_data_loader_factory_incremental_step (factory,
                                                              context));
  g_assert_nonnull (gimp_data_factory_get_data (factory,
                                                "Startup Cooperative Test",
                                                NULL, FALSE));

  g_object_unref (factory);
  palette_fixture_clear (gimp, &fixture);
}

int
main (int    argc,
      char **argv)
{
  Gimp *gimp;
  gint  result;

  g_test_init (&argc, &argv, NULL);

  gimp_test_utils_set_gimp3_directory ("GIMP_TESTING_ABS_TOP_SRCDIR",
                                       "app/tests/gimpdir");
  gimp = gimp_init_for_testing ();

  g_test_add_data_func ("/startup-loader/cooperative-equals-eager",
                        gimp, test_cooperative_equals_eager);
  g_test_add_data_func ("/startup-loader/cancel-and-full-refresh",
                        gimp, test_cancel_and_full_refresh);

  result = g_test_run ();

  gimp_test_utils_set_gimp3_directory ("GIMP_TESTING_ABS_TOP_BUILDDIR",
                                       "app/tests/gimpdir-output");
  gimp_exit (gimp, TRUE);

  return result;
}
