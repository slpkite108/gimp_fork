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

#pragma once


void      gimp_data_factories_init        (Gimp               *gimp);
void      gimp_data_factories_add_builtin (Gimp               *gimp);
void      gimp_data_factories_clear       (Gimp               *gimp);
void      gimp_data_factories_exit        (Gimp               *gimp);

gboolean  gimp_data_factories_wait        (Gimp               *gimp);

gint64    gimp_data_factories_get_memsize (Gimp   *gimp,
                                           gint64 *gui_size);
void      gimp_data_factories_data_clean  (Gimp   *gimp);

void      gimp_data_factories_load        (Gimp               *gimp,
                                           GimpInitStatusFunc  status_callback);
void      gimp_data_factories_save        (Gimp               *gimp);

/* Experimental, opt-in startup scheduling. All functions are called on the
 * GIMP main thread; deferred factory loading must not touch GTK off-thread.
 */
void      gimp_data_factories_startup_prepare
                                          (Gimp               *gimp,
                                           const gchar       **filenames,
                                           gboolean            enabled);
void      gimp_data_factories_startup_begin_background
                                          (Gimp               *gimp);
void      gimp_data_factories_startup_require_factory
                                          (Gimp               *gimp,
                                           GimpDataFactory    *factory);
void      gimp_data_factories_startup_require_container
                                          (Gimp               *gimp,
                                           GimpDataFactory    *factory);
void      gimp_data_factories_startup_require_all
                                          (Gimp               *gimp);
void      gimp_data_factories_startup_clear
                                          (Gimp               *gimp);

