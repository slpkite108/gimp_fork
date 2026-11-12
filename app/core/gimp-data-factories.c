/* GIMP - The GNU Image Manipulation Program
 * Copyright (C) 1995-2002 Spencer Kimball, Peter Mattis, and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gio/gio.h>
#include <gegl.h>

#include "libgimpbase/gimpbase.h"
#include "libgimpconfig/gimpconfig.h"

#include "core-types.h"

#include "config/gimprc.h"

#include "gimp.h"
#include "gimp-data-factories.h"
#include "gimp-gradients.h"
#include "gimp-memsize.h"
#include "gimp-palettes.h"
#include "gimp-utils.h"
#include "gimpcontainer.h"
#include "gimpbrush-load.h"
#include "gimpbrush.h"
#include "gimpbrushclipboard.h"
#include "gimpbrushgenerated-load.h"
#include "gimpbrushpipe-load.h"
#include "gimpcurve.h"
#include "gimpdataloaderfactory.h"
#include "gimpdynamics.h"
#include "gimpdynamics-load.h"
#include "gimpgradient-load.h"
#include "gimpgradient.h"
#include "gimpmybrush-load.h"
#include "gimpmybrush.h"
#include "gimppalette-load.h"
#include "gimppalette.h"
#include "gimppattern-load.h"
#include "gimppattern.h"
#include "gimppatternclipboard.h"
#include "gimptagcache.h"
#include "gimptoolpreset.h"
#include "gimptoolpreset-load.h"

#include "text/gimpfont.h"
#include "text/gimpfontfactory.h"

#include "gimp-intl.h"


/* Opt-in progressive startup. All operations run on the GIMP main context:
 * factory refresh and container notifications are not thread-safe.
 *
 * A user request may take an item out of the pending queue immediately.
 * An already executing synchronous factory refresh is not preemptible;
 * per-file cooperative scheduling is a separate follow-up optimization.
 */
#define GIMP_STARTUP_STATE_KEY "gimp-progressive-startup"

typedef enum
{
  GIMP_STARTUP_EMPTY,
  GIMP_STARTUP_IMAGE,
  GIMP_STARTUP_PROJECT
} GimpStartupMode;

typedef enum
{
  GIMP_STARTUP_MYPAINT,
  GIMP_STARTUP_PATTERNS,
  GIMP_STARTUP_PALETTES,
  GIMP_STARTUP_GRADIENTS,
  GIMP_STARTUP_TOOL_PRESETS,
  GIMP_STARTUP_N_RESOURCES
} GimpStartupResource;

typedef struct
{
  Gimp                *gimp;    /* weak; state is owned by Gimp */
  GimpStartupMode      mode;
  GimpDataFactory     *factories[GIMP_STARTUP_N_RESOURCES];
  gboolean             pending[GIMP_STARTUP_N_RESOURCES];
  gboolean             loading[GIMP_STARTUP_N_RESOURCES];
  guint                idle_id;
} GimpStartupState;


static GimpStartupState *
gimp_startup_get_state (Gimp *gimp)
{
  return g_object_get_data (G_OBJECT (gimp), GIMP_STARTUP_STATE_KEY);
}

static void
gimp_startup_state_free (gpointer data)
{
  GimpStartupState *state = data;

  if (state->idle_id)
    g_source_remove (state->idle_id);

  g_free (state);
}

static gboolean
gimp_startup_is_project (const gchar *filename)
{
  GFile   *file;
  gchar   *base;
  gchar   *lower;
  gboolean is_project;

  file = g_file_new_for_commandline_arg (filename);
  base = g_file_get_basename (file);
  lower = g_ascii_strdown (base ? base : "", -1);

  /* A known regular image is safe to classify as an image. An
   * unrecognized type takes the conservative project path, so that
   * project-specific resources are never silently skipped.
   */
  is_project = ! (g_str_has_suffix (lower, ".png")  ||
                  g_str_has_suffix (lower, ".jpg")  ||
                  g_str_has_suffix (lower, ".jpeg") ||
                  g_str_has_suffix (lower, ".gif")  ||
                  g_str_has_suffix (lower, ".bmp")  ||
                  g_str_has_suffix (lower, ".tif")  ||
                  g_str_has_suffix (lower, ".tiff") ||
                  g_str_has_suffix (lower, ".webp") ||
                  g_str_has_suffix (lower, ".avif") ||
                  g_str_has_suffix (lower, ".heic") ||
                  g_str_has_suffix (lower, ".heif") ||
                  g_str_has_suffix (lower, ".jxl")  ||
                  g_str_has_suffix (lower, ".ico"));

  g_free (lower);
  g_free (base);
  g_object_unref (file);

  return is_project;
}

void
gimp_data_factories_startup_prepare (Gimp          *gimp,
                                     const gchar  **filenames,
                                     gboolean       enabled)
{
  GimpStartupState *state;
  gint              i;

  g_return_if_fail (GIMP_IS_GIMP (gimp));

  /* Do not apply this experiment to batch, headless or --no-data runs.
   * The caller also opts in explicitly through an environment variable.
   */
  if (! enabled || gimp->no_interface || gimp->no_data)
    return;

  if (gimp_startup_get_state (gimp))
    return;

  state = g_new0 (GimpStartupState, 1);
  state->gimp = gimp;
  state->mode = GIMP_STARTUP_EMPTY;

  if (filenames && filenames[0])
    {
      state->mode = GIMP_STARTUP_IMAGE;

      for (i = 0; filenames[i]; i++)
        {
          if (gimp_startup_is_project (filenames[i]))
            {
              state->mode = GIMP_STARTUP_PROJECT;
              break;
            }
        }
    }

  state->factories[GIMP_STARTUP_MYPAINT]      = gimp->mybrush_factory;
  state->factories[GIMP_STARTUP_PATTERNS]     = gimp->pattern_factory;
  state->factories[GIMP_STARTUP_PALETTES]     = gimp->palette_factory;
  state->factories[GIMP_STARTUP_GRADIENTS]    = gimp->gradient_factory;
  state->factories[GIMP_STARTUP_TOOL_PRESETS] = gimp->tool_preset_factory;

  g_object_set_data_full (G_OBJECT (gimp), GIMP_STARTUP_STATE_KEY,
                          state, gimp_startup_state_free);

  if (g_strcmp0 (g_getenv ("GIMP_STARTUP_PROFILE"), "1") == 0)
    g_printerr ("GIMP_PROGRESSIVE_STARTUP,mode,%s\n",
                state->mode == GIMP_STARTUP_EMPTY ? "empty" :
                state->mode == GIMP_STARTUP_IMAGE ? "image" : "project");
}

static void
gimp_startup_load_resource (GimpStartupState    *state,
                            GimpStartupResource  resource,
                            const gchar         *reason)
{
  GimpDataFactory *factory;
  gint64           start;

  if (! state->pending[resource] || state->loading[resource])
    return;

  /* Mark no longer pending before refresh: factory data loading can emit
   * "add" and "notify" signals which recursively request its container.
   */
  state->pending[resource] = FALSE;
  state->loading[resource] = TRUE;
  factory = state->factories[resource];
  start = g_get_monotonic_time ();

  gimp_data_factory_data_refresh (factory,
                                  gimp_get_user_context (state->gimp));
  gimp_data_factory_data_clean (factory);

  state->loading[resource] = FALSE;

  if (g_strcmp0 (g_getenv ("GIMP_STARTUP_PROFILE"), "1") == 0)
    g_printerr ("GIMP_PROGRESSIVE_STARTUP,resource,%s,%s,%" G_GINT64_FORMAT "\n",
                gimp_object_get_name (GIMP_OBJECT (factory)),
                reason, g_get_monotonic_time () - start);
}

static gboolean
gimp_startup_load_idle (gpointer data)
{
  GimpStartupState         *state = data;
  const GimpStartupResource empty_order[] = {
    GIMP_STARTUP_TOOL_PRESETS, GIMP_STARTUP_PALETTES,
    GIMP_STARTUP_GRADIENTS, GIMP_STARTUP_PATTERNS,
    GIMP_STARTUP_MYPAINT
  };
  const GimpStartupResource image_order[] = {
    GIMP_STARTUP_PATTERNS, GIMP_STARTUP_GRADIENTS,
    GIMP_STARTUP_PALETTES, GIMP_STARTUP_MYPAINT,
    GIMP_STARTUP_TOOL_PRESETS
  };
  const GimpStartupResource *order = (state->mode == GIMP_STARTUP_IMAGE ?
                                       image_order : empty_order);
  guint i;

  /* Execute at most one category per low-priority main-loop turn. */
  for (i = 0; i < G_N_ELEMENTS (empty_order); i++)
    {
      if (state->pending[order[i]])
        {
          gimp_startup_load_resource (state, order[i], "background");
          return G_SOURCE_CONTINUE;
        }
    }

  state->idle_id = 0;
  return G_SOURCE_REMOVE;
}

void
gimp_data_factories_startup_begin_background (Gimp *gimp)
{
  GimpStartupState *state;

  g_return_if_fail (GIMP_IS_GIMP (gimp));

  state = gimp_startup_get_state (gimp);
  if (! state || state->mode == GIMP_STARTUP_PROJECT || state->idle_id)
    return;

  state->idle_id = g_idle_add_full (G_PRIORITY_LOW,
                                    gimp_startup_load_idle,
                                    state, NULL);
}

void
gimp_data_factories_startup_require_factory (Gimp            *gimp,
                                             GimpDataFactory *factory)
{
  GimpStartupState *state;
  guint             i;

  g_return_if_fail (GIMP_IS_GIMP (gimp));

  state = gimp_startup_get_state (gimp);
  if (! state)
    return;

  for (i = 0; i < GIMP_STARTUP_N_RESOURCES; i++)
    {
      if (state->factories[i] == factory)
        {
          if (state->pending[i])
            gimp_startup_load_resource (state, i, "user-request");

          return;
        }
    }
}

void
gimp_data_factories_startup_require_all (Gimp *gimp)
{
  GimpStartupState *state;
  guint             i;

  g_return_if_fail (GIMP_IS_GIMP (gimp));

  state = gimp_startup_get_state (gimp);
  if (! state)
    return;

  if (state->idle_id)
    {
      g_source_remove (state->idle_id);
      state->idle_id = 0;
    }

  for (i = 0; i < GIMP_STARTUP_N_RESOURCES; i++)
    if (state->pending[i])
      gimp_startup_load_resource (state, i, "require-all");
}

void
gimp_data_factories_startup_clear (Gimp *gimp)
{
  g_return_if_fail (GIMP_IS_GIMP (gimp));

  g_object_set_data (G_OBJECT (gimp), GIMP_STARTUP_STATE_KEY, NULL);
}


void
gimp_data_factories_init (Gimp *gimp)
{
  g_return_if_fail (GIMP_IS_GIMP (gimp));

  gimp->brush_factory =
    gimp_data_loader_factory_new (gimp,
                                  GIMP_TYPE_BRUSH,
                                  "brush-path",
                                  "brush-path-writable",
                                  "brush-paths",
                                  gimp_brush_new,
                                  gimp_brush_get_standard);
  gimp_object_set_static_name (GIMP_OBJECT (gimp->brush_factory),
                               "brush factory");
  gimp_data_loader_factory_add_loader (gimp->brush_factory,
                                       "GIMP Brush",
                                       gimp_brush_load,
                                       GIMP_BRUSH_FILE_EXTENSION,
                                       TRUE);
  gimp_data_loader_factory_add_loader (gimp->brush_factory,
                                       "GIMP Brush Pixmap",
                                       gimp_brush_load,
                                       GIMP_BRUSH_PIXMAP_FILE_EXTENSION,
                                       FALSE);
  gimp_data_loader_factory_add_loader (gimp->brush_factory,
                                       "Photoshop ABR Brush",
                                       gimp_brush_load_abr,
                                       GIMP_BRUSH_PS_FILE_EXTENSION,
                                       FALSE);
  gimp_data_loader_factory_add_loader (gimp->brush_factory,
                                       "Paint Shop Pro JBR Brush",
                                       gimp_brush_load_abr,
                                       GIMP_BRUSH_PSP_FILE_EXTENSION,
                                       FALSE);
 gimp_data_loader_factory_add_loader (gimp->brush_factory,
                                       "GIMP Generated Brush",
                                       gimp_brush_generated_load,
                                       GIMP_BRUSH_GENERATED_FILE_EXTENSION,
                                       TRUE);
  gimp_data_loader_factory_add_loader (gimp->brush_factory,
                                       "GIMP Brush Pipe",
                                       gimp_brush_pipe_load,
                                       GIMP_BRUSH_PIPE_FILE_EXTENSION,
                                       TRUE);

  gimp->dynamics_factory =
    gimp_data_loader_factory_new (gimp,
                                  GIMP_TYPE_DYNAMICS,
                                  "dynamics-path",
                                  "dynamics-path-writable",
                                  "dynamics-paths",
                                  gimp_dynamics_new,
                                  gimp_dynamics_get_standard);
  gimp_object_set_static_name (GIMP_OBJECT (gimp->dynamics_factory),
                               "dynamics factory");
  gimp_data_loader_factory_add_loader (gimp->dynamics_factory,
                                       "GIMP Paint Dynamics",
                                       gimp_dynamics_load,
                                       GIMP_DYNAMICS_FILE_EXTENSION,
                                       TRUE);

  gimp->mybrush_factory =
    gimp_data_loader_factory_new (gimp,
                                  GIMP_TYPE_MYBRUSH,
                                  "mypaint-brush-path",
                                  "mypaint-brush-path-writable",
                                  "mypaint-brush-paths",
                                  NULL,
                                  NULL);
  gimp_object_set_static_name (GIMP_OBJECT (gimp->mybrush_factory),
                               "mypaint brush factory");
  gimp_data_loader_factory_add_loader (gimp->mybrush_factory,
                                       "MyPaint Brush",
                                       gimp_mybrush_load,
                                       GIMP_MYBRUSH_FILE_EXTENSION,
                                       FALSE);

  gimp->pattern_factory =
    gimp_data_loader_factory_new (gimp,
                                  GIMP_TYPE_PATTERN,
                                  "pattern-path",
                                  "pattern-path-writable",
                                  "pattern-paths",
                                  NULL,
                                  gimp_pattern_get_standard);
  gimp_object_set_static_name (GIMP_OBJECT (gimp->pattern_factory),
                               "pattern factory");
  gimp_data_loader_factory_add_loader (gimp->pattern_factory,
                                       "GIMP Pattern",
                                       gimp_pattern_load,
                                       GIMP_PATTERN_FILE_EXTENSION,
                                       TRUE);
  gimp_data_loader_factory_add_fallback (gimp->pattern_factory,
                                         "Pattern from GdkPixbuf",
                                         gimp_pattern_load_pixbuf);

  gimp->gradient_factory =
    gimp_data_loader_factory_new (gimp,
                                  GIMP_TYPE_GRADIENT,
                                  "gradient-path",
                                  "gradient-path-writable",
                                  "gradient-paths",
                                  gimp_gradient_new,
                                  gimp_gradient_get_standard);
  gimp_object_set_static_name (GIMP_OBJECT (gimp->gradient_factory),
                               "gradient factory");
  gimp_data_loader_factory_add_loader (gimp->gradient_factory,
                                       "GIMP Gradient",
                                       gimp_gradient_load,
                                       GIMP_GRADIENT_FILE_EXTENSION,
                                       TRUE);
  gimp_data_loader_factory_add_loader (gimp->gradient_factory,
                                       "SVG Gradient",
                                       gimp_gradient_load_svg,
                                       GIMP_GRADIENT_SVG_FILE_EXTENSION,
                                       FALSE);

  gimp->palette_factory =
    gimp_data_loader_factory_new (gimp,
                                  GIMP_TYPE_PALETTE,
                                  "palette-path",
                                  "palette-path-writable",
                                  "palette-paths",
                                  gimp_palette_new,
                                  gimp_palette_get_standard);
  gimp_object_set_static_name (GIMP_OBJECT (gimp->palette_factory),
                               "palette factory");
  gimp_data_loader_factory_add_loader (gimp->palette_factory,
                                       "GIMP Palette",
                                       gimp_palette_load,
                                       GIMP_PALETTE_FILE_EXTENSION,
                                       TRUE);

  gimp->font_factory =
    gimp_font_factory_new (gimp,
                           "font-path");
  gimp_object_set_static_name (GIMP_OBJECT (gimp->font_factory),
                               "font factory");
  gimp_font_class_set_font_factory (GIMP_FONT_FACTORY (gimp->font_factory));

  gimp->tool_preset_factory =
    gimp_data_loader_factory_new (gimp,
                                  GIMP_TYPE_TOOL_PRESET,
                                  "tool-preset-path",
                                  "tool-preset-path-writable",
                                  "tool-preset-paths",
                                  gimp_tool_preset_new,
                                  NULL);
  gimp_object_set_static_name (GIMP_OBJECT (gimp->tool_preset_factory),
                               "tool preset factory");
  gimp_data_loader_factory_add_loader (gimp->tool_preset_factory,
                                       "GIMP Tool Preset",
                                       gimp_tool_preset_load,
                                       GIMP_TOOL_PRESET_FILE_EXTENSION,
                                       TRUE);

  gimp->tag_cache = gimp_tag_cache_new ();
}

void
gimp_data_factories_add_builtin (Gimp *gimp)
{
  GimpData *data;

  g_return_if_fail (GIMP_IS_GIMP (gimp));

  /*  add the builtin FG -> BG etc. gradients  */
  gimp_gradients_init (gimp);

  /*  add the color history palette  */
  gimp_palettes_init (gimp);

  /*  add the clipboard brushes  */
  data = gimp_brush_clipboard_new (gimp, FALSE);
  gimp_data_make_internal (data, "gimp-brush-clipboard-image");
  gimp_container_add (gimp_data_factory_get_container (gimp->brush_factory),
                      GIMP_OBJECT (data));
  g_object_unref (data);

  data = gimp_brush_clipboard_new (gimp, TRUE);
  gimp_data_make_internal (data, "gimp-brush-clipboard-mask");
  gimp_container_add (gimp_data_factory_get_container (gimp->brush_factory),
                      GIMP_OBJECT (data));
  g_object_unref (data);

  /*  add the clipboard pattern  */
  data = gimp_pattern_clipboard_new (gimp);
  gimp_data_make_internal (data, "gimp-pattern-clipboard-image");
  gimp_container_add (gimp_data_factory_get_container (gimp->pattern_factory),
                      GIMP_OBJECT (data));
  g_object_unref (data);
}

void
gimp_data_factories_clear (Gimp *gimp)
{
  g_return_if_fail (GIMP_IS_GIMP (gimp));

  if (gimp->brush_factory)
    gimp_data_factory_data_free (gimp->brush_factory);

  if (gimp->dynamics_factory)
    gimp_data_factory_data_free (gimp->dynamics_factory);

  if (gimp->mybrush_factory)
    gimp_data_factory_data_free (gimp->mybrush_factory);

  if (gimp->pattern_factory)
    gimp_data_factory_data_free (gimp->pattern_factory);

  if (gimp->gradient_factory)
    gimp_data_factory_data_free (gimp->gradient_factory);

  if (gimp->palette_factory)
    gimp_data_factory_data_free (gimp->palette_factory);

  if (gimp->font_factory)
    gimp_data_factory_data_free (gimp->font_factory);

  if (gimp->tool_preset_factory)
    gimp_data_factory_data_free (gimp->tool_preset_factory);
}

void
gimp_data_factories_exit (Gimp *gimp)
{
  g_return_if_fail (GIMP_IS_GIMP (gimp));

  /* Cancel the idle source before the queued factories can be destroyed. */
  gimp_data_factories_startup_clear (gimp);

  g_clear_object (&gimp->brush_factory);
  g_clear_object (&gimp->dynamics_factory);
  g_clear_object (&gimp->mybrush_factory);
  g_clear_object (&gimp->pattern_factory);
  g_clear_object (&gimp->gradient_factory);
  g_clear_object (&gimp->palette_factory);
  g_clear_object (&gimp->font_factory);
  g_clear_object (&gimp->tool_preset_factory);
  g_clear_object (&gimp->tag_cache);
}

gboolean
gimp_data_factories_wait (Gimp *gimp)
{
  GList    *data_types;
  GList    *excluded;
  gboolean  loaded = TRUE;

  /* XCF and other callers of this API require all resource data, not
   * merely an empty asynchronous queue. Upgrade any pending work now.
   */
  gimp_data_factories_startup_require_all (gimp);

  /* Curves are the only data type without a factory. */
  excluded = g_list_prepend (NULL, GIMPTYPE_TO_POINTER (GIMP_TYPE_CURVE));

  data_types = gimp_get_type_children (GIMP_TYPE_DATA, NULL, excluded);
  g_list_free (excluded);

  for (GList *iter = data_types; iter; iter = iter->next)
    {
      GimpDataFactory *factory;

      factory = gimp_get_data_factory (gimp, GIMPPOINTER_TO_TYPE (iter->data));

      if (factory)
        {
          GimpAsyncSet *set;

          set = gimp_data_factory_get_async_set (factory);
          g_object_get (set, "empty", &loaded, NULL);
          if (! loaded)
            {
              gimp_data_factory_data_wait (factory);
              loaded = TRUE;
            }
        }
    }

  g_list_free (data_types);

  return loaded;
}

gint64
gimp_data_factories_get_memsize (Gimp   *gimp,
                                 gint64 *gui_size)
{
  gint64 memsize = 0;

  g_return_val_if_fail (GIMP_IS_GIMP (gimp), 0);

  memsize += gimp_object_get_memsize (GIMP_OBJECT (gimp->named_buffers),
                                      gui_size);
  memsize += gimp_object_get_memsize (GIMP_OBJECT (gimp->brush_factory),
                                      gui_size);
  memsize += gimp_object_get_memsize (GIMP_OBJECT (gimp->dynamics_factory),
                                      gui_size);
  memsize += gimp_object_get_memsize (GIMP_OBJECT (gimp->mybrush_factory),
                                      gui_size);
  memsize += gimp_object_get_memsize (GIMP_OBJECT (gimp->pattern_factory),
                                      gui_size);
  memsize += gimp_object_get_memsize (GIMP_OBJECT (gimp->gradient_factory),
                                      gui_size);
  memsize += gimp_object_get_memsize (GIMP_OBJECT (gimp->palette_factory),
                                      gui_size);
  memsize += gimp_object_get_memsize (GIMP_OBJECT (gimp->font_factory),
                                      gui_size);
  memsize += gimp_object_get_memsize (GIMP_OBJECT (gimp->tool_preset_factory),
                                      gui_size);

  memsize += gimp_object_get_memsize (GIMP_OBJECT (gimp->tag_cache),
                                      gui_size);

  return memsize;
}

void
gimp_data_factories_data_clean (Gimp *gimp)
{
  g_return_if_fail (GIMP_IS_GIMP (gimp));

  gimp_data_factory_data_clean (gimp->brush_factory);
  gimp_data_factory_data_clean (gimp->dynamics_factory);
  gimp_data_factory_data_clean (gimp->mybrush_factory);
  gimp_data_factory_data_clean (gimp->pattern_factory);
  gimp_data_factory_data_clean (gimp->gradient_factory);
  gimp_data_factory_data_clean (gimp->palette_factory);
  gimp_data_factory_data_clean (gimp->font_factory);
  gimp_data_factory_data_clean (gimp->tool_preset_factory);
}

static void
gimp_data_factories_load_progressive (Gimp               *gimp,
                                      GimpInitStatusFunc  status_callback,
                                      GimpStartupState   *state)
{
  GimpContext *context = gimp_get_user_context (gimp);

  /* Brush and paint dynamics are retained on the critical path because
   * the initial drawing context and tool options depend on them.
   */
  status_callback (NULL, _("Brushes"), 0.1);
  gimp_data_factory_data_init (gimp->brush_factory, context, gimp->no_data);

  status_callback (NULL, _("Dynamics"), 0.15);
  gimp_data_factory_data_init (gimp->dynamics_factory, context, gimp->no_data);

  /* Font loading uses GimpAsync already; launch it early without blocking
   * the first window, and keep its standard wait mechanism intact.
   */
  status_callback (NULL, _("Fonts"), 0.2);
  gimp_data_factory_data_init (gimp->font_factory, context, gimp->no_fonts);

  /* Register factory paths and standard data, but defer disk traversal.
   * A later request can synchronously promote any pending resource.
   */
  status_callback (NULL, _("MyPaint Brushes"), 0.3);
  gimp_data_factory_data_init (gimp->mybrush_factory, context, TRUE);
  state->pending[GIMP_STARTUP_MYPAINT] = TRUE;

  status_callback (NULL, _("Patterns"), 0.4);
  gimp_data_factory_data_init (gimp->pattern_factory, context, TRUE);
  state->pending[GIMP_STARTUP_PATTERNS] = TRUE;

  status_callback (NULL, _("Palettes"), 0.5);
  gimp_data_factory_data_init (gimp->palette_factory, context, TRUE);
  state->pending[GIMP_STARTUP_PALETTES] = TRUE;

  status_callback (NULL, _("Gradients"), 0.6);
  gimp_data_factory_data_init (gimp->gradient_factory, context, TRUE);
  state->pending[GIMP_STARTUP_GRADIENTS] = TRUE;

  /* Color history uses in-memory palettes and must be restored eagerly. */
  status_callback (NULL, _("Color History"), 0.65);
  gimp_palettes_load (gimp);

  status_callback (NULL, _("Tool Presets"), 0.7);
  gimp_data_factory_data_init (gimp->tool_preset_factory, context, TRUE);
  state->pending[GIMP_STARTUP_TOOL_PRESETS] = TRUE;

  /* The tag cache hooks "add": deferred resource objects receive their
   * existing tags once they are inserted into these containers.
   */
  status_callback (NULL, _("Updating tag cache"), 0.75);
  gimp_tag_cache_load (gimp->tag_cache);
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->brush_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->dynamics_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->mybrush_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->pattern_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->gradient_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->palette_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->font_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->tool_preset_factory));
}

void
gimp_data_factories_load (Gimp               *gimp,
                          GimpInitStatusFunc  status_callback)
{
  GimpStartupState *state;

  g_return_if_fail (GIMP_IS_GIMP (gimp));

  state = gimp_startup_get_state (gimp);

  if (state && state->mode != GIMP_STARTUP_PROJECT)
    {
      gimp_data_factories_load_progressive (gimp, status_callback, state);
      return;
    }

  /* For project files, start asynchronous font enumeration first while
   * the remaining factories initialize. All project resources stay eager.
   */
  if (state && state->mode == GIMP_STARTUP_PROJECT)
    {
      status_callback (NULL, _("Fonts"), 0.05);
      gimp_data_factory_data_init (gimp->font_factory, gimp->user_context,
                                   gimp->no_fonts);
    }

  /*  initialize the list of gimp brushes    */
  status_callback (NULL, _("Brushes"), 0.1);
  gimp_data_factory_data_init (gimp->brush_factory, gimp->user_context,
                               gimp->no_data);

  /*  initialize the list of gimp dynamics   */
  status_callback (NULL, _("Dynamics"), 0.15);
  gimp_data_factory_data_init (gimp->dynamics_factory, gimp->user_context,
                               gimp->no_data);

  /*  initialize the list of mypaint brushes    */
  status_callback (NULL, _("MyPaint Brushes"), 0.2);
  gimp_data_factory_data_init (gimp->mybrush_factory, gimp->user_context,
                               gimp->no_data);

  /*  initialize the list of gimp patterns   */
  status_callback (NULL, _("Patterns"), 0.3);
  gimp_data_factory_data_init (gimp->pattern_factory, gimp->user_context,
                               gimp->no_data);

  /*  initialize the list of gimp palettes   */
  status_callback (NULL, _("Palettes"), 0.4);
  gimp_data_factory_data_init (gimp->palette_factory, gimp->user_context,
                               gimp->no_data);

  /*  initialize the list of gimp gradients  */
  status_callback (NULL, _("Gradients"), 0.5);
  gimp_data_factory_data_init (gimp->gradient_factory, gimp->user_context,
                               gimp->no_data);

  /*  initialize the color history   */
  status_callback (NULL, _("Color History"), 0.55);
  gimp_palettes_load (gimp);

  /*  initialize the list of gimp fonts (if not already started)  */
  if (! state || state->mode != GIMP_STARTUP_PROJECT)
    {
      status_callback (NULL, _("Fonts"), 0.6);
      gimp_data_factory_data_init (gimp->font_factory, gimp->user_context,
                                   gimp->no_fonts);
    }

  /*  initialize the list of gimp tool presets if we have a GUI  */
  if (! gimp->no_interface)
    {
      status_callback (NULL, _("Tool Presets"), 0.7);
      gimp_data_factory_data_init (gimp->tool_preset_factory, gimp->user_context,
                                   gimp->no_data);
    }

  /* update tag cache */
  status_callback (NULL, _("Updating tag cache"), 0.75);
  gimp_tag_cache_load (gimp->tag_cache);
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->brush_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->dynamics_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->mybrush_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->pattern_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->gradient_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->palette_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->font_factory));
  gimp_tag_cache_add_container (gimp->tag_cache,
                                gimp_data_factory_get_container (gimp->tool_preset_factory));
}

void
gimp_data_factories_save (Gimp *gimp)
{
  g_return_if_fail (GIMP_IS_GIMP (gimp));

  gimp_tag_cache_save (gimp->tag_cache);

  gimp_data_factory_data_save (gimp->brush_factory);
  gimp_data_factory_data_save (gimp->dynamics_factory);
  gimp_data_factory_data_save (gimp->mybrush_factory);
  gimp_data_factory_data_save (gimp->pattern_factory);
  gimp_data_factory_data_save (gimp->gradient_factory);
  gimp_data_factory_data_save (gimp->palette_factory);
  gimp_data_factory_data_save (gimp->font_factory);
  gimp_data_factory_data_save (gimp->tool_preset_factory);

  gimp_palettes_save (gimp);
}
