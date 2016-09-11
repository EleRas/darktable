/*
    This file is part of darktable,
    copyright (c) 2015 tamas feher

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "common/debug.h"
#include "common/darktable.h"
#include "common/colorspaces.h"
#include "common/imageio_module.h"
#include "common/styles.h"
#include "common/collection.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/conf.h"
#include "control/signal.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "gui/accelerators.h"
#include "gui/presets.h"
#include <stdlib.h>
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include "dtgtk/button.h"
#include "bauhaus/bauhaus.h"

DT_MODULE(4)

#define timelapse_export_MAX_IMAGE_SIZE UINT16_MAX

typedef struct dt_lib_timelapse_export_t
{
  GtkWidget *reset_timelapse_button, *initialize_timelapse_button, *equalize_button;
} dt_lib_timelapse_export_t;

const char *name()
{
  return _("timelapse");
}

uint32_t views()
{
  return DT_VIEW_TIMELAPSE;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

void gui_reset(dt_lib_module_t *self)
{
}

static void reset_timelapse_button_clicked(GtkWidget *widget, dt_lib_timelapse_export_t *d)
{
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_TIMELAPSE_RESET);
}

static void initialize_timelapse_button_clicked(GtkWidget *widget, dt_lib_timelapse_export_t *d)
{
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_TIMELAPSE_INITIALIZE);
}

static void equalize_button_clicked(GtkWidget *widget, dt_lib_timelapse_export_t *d)
{
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_TIMELAPSE_EQUALIZE);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_timelapse_export_t *d = (dt_lib_timelapse_export_t *)malloc(sizeof(dt_lib_timelapse_export_t));
  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(5));

  self->widget = gtk_grid_new();
  GtkGrid *grid = GTK_GRID(self->widget);
  gtk_grid_set_row_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(5));
  gtk_grid_set_column_homogeneous(grid, TRUE);
  int line = 0;

  GtkWidget *reset_timelapse_button = gtk_button_new_with_label(_("reset timelase values"));
  d->reset_timelapse_button = reset_timelapse_button;
  g_object_set(G_OBJECT(reset_timelapse_button), "tooltip-text",
               _("reset timelase stuff, like keyframes and markers"), (char *)NULL);
  gtk_grid_attach(grid, reset_timelapse_button, 0, ++line, 3, 1);
  g_signal_connect(G_OBJECT(d->reset_timelapse_button), "button-release-event", G_CALLBACK(reset_timelapse_button_clicked), (gpointer)d);

  GtkWidget *initialize_timelapse_button = gtk_button_new_with_label(_("initialize all frames"));
  d->initialize_timelapse_button = initialize_timelapse_button;
  g_object_set(G_OBJECT(initialize_timelapse_button), "tooltip-text",
               _("calculate average brightness set keyframes"), (char *)NULL);
  gtk_grid_attach(grid, initialize_timelapse_button, 0, ++line, 2, 1);
  g_signal_connect(G_OBJECT(d->initialize_timelapse_button), "button-release-event", G_CALLBACK(initialize_timelapse_button_clicked), (gpointer)d);

  GtkWidget *equalize_button = gtk_button_new_with_label(_("equalize"));
  d->equalize_button = equalize_button;
  g_object_set(G_OBJECT(equalize_button), "tooltip-text",
               _("equalize changes in exposure, aperture and iso"), (char *)NULL);
  gtk_grid_attach(grid, equalize_button, 2, line, 1, 1);
  g_signal_connect(G_OBJECT(d->equalize_button), "button-release-event", G_CALLBACK(equalize_button_clicked), (gpointer)d);

  self->gui_reset(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
