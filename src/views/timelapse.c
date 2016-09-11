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
#include "views/view.h"
#include "bauhaus/bauhaus.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/colorspaces.h"
#include "common/collection.h"
#include "common/debug.h"
#include "common/dtpthread.h"
#include "common/image_cache.h"
#include "common/history.h"
#include "common/metadata.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/blend.h"
#include "develop/masks.h"
#include "develop/lightroom.h"
#include "dtgtk/button.h"
#include "dtgtk/icon.h"
#include "dtgtk/expander.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "gui/draw.h"

#include <stdbool.h>
#include <gdk/gdkkeysyms.h>
#include <stdint.h>
#include <math.h>

DT_MODULE(1)

#define ICON_SIZE DT_PIXEL_APPLY_DPI(20)
#define MARGIN 20
#define BAR_HEIGHT 18

//#define dt_vvLRDT_BLEND_VERSION() 7
#define LRDT_EXPOSURE_VERSION 2
#define LRDT_BLEND_VERSION 4
#define PI 3.14159265358979323846

typedef struct dt_iop_exposure_params_t
{
  float black, exposure, gain;
} dt_iop_exposure_params_t;

enum
{
  COL_KEYFRAME,
  COL_FRAME,
  COL_FILENAME,
  COL_CORRECTION,
  COL_BRIGHTNESS,
  COL_EXPOSURE,
  COL_APERTURE,
  COL_ISO,
  COL_FOCAL_LENGTH
};

typedef struct dt_timelapse_t
{
  int table_scroll;
  int table_length;
  int table_highlight;
  int table_select;
  int mouse_y;
  bool mouse_moved;
  int preview_height;
  int preview_width;
  bool preview_height_change;
  GtkWidget *treeview;
  int32_t collection_count;
  int missed_frames;
  double progress_fraction;
  guint progress_total;

  // double buffer
  uint32_t *buf1, *buf2;
  uint32_t *front, *back;

  // output profile before we overwrote it:
  int old_profile_type;

  // state machine stuff for image transitions:
  dt_pthread_mutex_t lock;

  bool keyframing;

  // some magic to hide the mosue pointer
  guint mouse_timeout;
  guint redraw_timeout;
  dt_progress_t *progress;

  GtkListStore *framelist_store;
  GtkWidget *framelist_tree;
  GtkCellRenderer *framelist_renderer;
  GtkTreeViewColumn *framelist_column;
  GtkTreeIter framelist_iter;

  float *exposure_curve;
  float *aperture_curve;
  float *average_curve;
  float *correction_curve;
  float *iso_curve;
  bool exposure_curve_visible;
  bool aperture_curve_visible;
  bool average_curve_visible;
  bool correction_curve_visible;
  bool iso_curve_visible;

  int32_t full_preview_id;
  double full_preview_zoom;

  bool full_preview_step_forward;
  bool full_preview_step_backward;
  double full_preview_mouse_x;

  dt_view_image_over_t image_over;
  dt_image_t image_storage;
  int image_storage_image_num;
} dt_timelapse_t;

/*typedef struct dt_timelapse_format_t
{
  int max_width, max_height;
  int width, height;
  char style[128];
  gboolean style_append;
  dt_timelapse_t *d;
} dt_timelapse_format_t;*/

typedef struct lr2dt
{
  float lr, dt;
} lr2dt_t;

static float get_interpolate(lr2dt_t lr2dt_table[], float value)
{
  int k = 0;

  while(lr2dt_table[k + 1].lr < value) k++;

  return lr2dt_table[k].dt
         + ((value - lr2dt_table[k].lr) / (lr2dt_table[k + 1].lr - lr2dt_table[k].lr))
           * (lr2dt_table[k + 1].dt - lr2dt_table[k].dt);
}

static float lr2dt_blacks(float value)
{
  lr2dt_t lr2dt_blacks_table[]
      = { { -100, 0.020 }, { -50, 0.005 }, { 0, 0 }, { 50, -0.005 }, { 100, -0.010 } };

  return get_interpolate(lr2dt_blacks_table, value);
}

static float lr2dt_exposure(float value)
{
  lr2dt_t lr2dt_exposure_table[] = { { -5, -4.5 }, { 0, 0 }, { 5, 4.5 } };

  return get_interpolate(lr2dt_exposure_table, value);
}

static void _capture_mipmaps_updated_signal_callback(gpointer instance, gpointer user_data)
{
  dt_control_queue_redraw_center();
}

static gboolean _redraw(gpointer data)
{
  _capture_mipmaps_updated_signal_callback(data, NULL);
  return FALSE;
}

const char *name(dt_view_t *self)
{
  return _("timelapse");
}



uint32_t view(dt_view_t *self)
{
  return DT_VIEW_TIMELAPSE;
}



void add_to_history(int imgid, char *operation, dt_iop_params_t *params, int params_size, char *imported,
                        size_t imported_len, int version, int *import_count, int multi_priority, char *multi_name)
{
  int32_t num_free = 1;
  sqlite3_stmt *stmt;
  dt_develop_blend_params_t blend_params = { 0 };
  multi_priority = 0;

  //fprintf(stderr, "--------------------------------------------------- IMGID %d\n", imgid);

  // search for priority num
  bool op_found = FALSE;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT num, multi_priority FROM history WHERE imgid = ?1 AND operation=?2 ORDER BY multi_priority DESC", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    num_free = sqlite3_column_int(stmt, 0);
    multi_priority = sqlite3_column_int(stmt, 1) + 1;
    //fprintf(stderr, "NEXT PRIORITY num > -1 ... num: %d WITH multi_priority: %d\n", num_free, multi_priority);
  }
  
  sqlite3_finalize(stmt);
  //multi_priority++;

  // search for applied operations
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT num, multi_priority FROM history WHERE imgid = ?1 AND operation=?2 AND multi_name=?3", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, operation, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 3, multi_name, -1, SQLITE_TRANSIENT);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    num_free = sqlite3_column_int(stmt, 0);
    multi_priority = sqlite3_column_int(stmt, 1);
    //fprintf(stderr, "OP ENTRY FOUND WHEN num > -1 ... num: %d WITH multi_priority: %d\n", num_free, multi_priority);
    op_found = TRUE;
  }
  
  sqlite3_finalize(stmt);


  /* update timelapse value */
  if(op_found != op_found)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(
        dt_database_get(darktable.db),
        "UPDATE history SET op_params = ?1, blendop_version = ?2 WHERE imgid = ?3 AND operation=?4 AND num = ?5", -1,
        &stmt, NULL);
    //fprintf(stderr, "UPDATE history SET blendop_params = aaa WHERE imgid = %d AND operation=%s AND num = %d\n", imgid, operation, num_free);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 1, params, params_size, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, LRDT_EXPOSURE_VERSION);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, imgid);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, operation, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 5, num_free);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  /* insert timelapse value at position num = 1 */
  else
  {
    /* get next position num=1 */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT num FROM history WHERE imgid = ?1 ORDER BY num DESC", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
    {
      num_free = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);

    num_free++;


    // add new entry at the begin of history
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT INTO history (imgid, num, module, operation, op_params, enabled, "
                                "blendop_params, blendop_version, multi_priority, multi_name) "
                                "VALUES (?1, ?2, ?3, ?4, ?5, 1, ?6, ?7, ?8, ?9)",
                                //"VALUES (?1, ?2, ?3, ?4, ?5, 1, null, ?7, ?8, ?9)",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, num_free);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, version);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 4, operation, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 5, params, params_size, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 6, &blend_params, sizeof(dt_develop_blend_params_t), SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 7, LRDT_EXPOSURE_VERSION);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 8, multi_priority);
    //const char *mn = multi_name;
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 9, (char *)multi_name, -1, SQLITE_TRANSIENT);
    //fprintf(stderr, "INSERT INTO history num: %d ... operation: %s ... LRDT_EXPOSURE_VERSION: '%d'\n", num_free, (char *)operation, LRDT_EXPOSURE_VERSION);

    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  // always make the whole stack active
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE images SET history_end = (SELECT MAX(num) + 1 FROM history WHERE imgid = ?1) WHERE id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}



float get_image_module_exposure(dt_view_t *self, dt_image_t *img)
{
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;

  float exposure_value = 0.0f;
  int multi_priority_max = -1;
  char *multi_name_max = " T";

  //dt_image_cache_read_release(darktable.image_cache, img);
  //fprintf(stderr, "EDIT IMAGE id: %d %d ... exposure_correction: %f\n", img->id, img->id, img->exposure_correction);

  dt_iop_exposure_params_t pe;
  memset(&pe, 0, sizeof(pe));

  pe.black = lr2dt_blacks((float)0);
  lr2dt_exposure(img->exposure_correction);

  pe.exposure = img->exposure_correction;
  char imported[256] = { 0 };
  int n_import = 0;

  add_to_history(img->id, "exposure", (dt_iop_params_t *)&pe, sizeof(dt_iop_exposure_params_t), imported,
                 sizeof(imported), LRDT_EXPOSURE_VERSION, &n_import, multi_priority_max, multi_name_max);
  //fprintf(stderr, "add_to_history ... num: %d ... multi_priority: %d ... multi_name: %s\n", 
  //        img->id, multi_priority_max, multi_name_max);

  // signal history changed
  dt_dev_reload_history_items(darktable.develop);
  dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));

  d->image_storage.id = img->id;
  //dev->image_storage.id

  /* if current image in develop reload history */
  if(dt_dev_is_current_image(darktable.develop, img->id))
  {
    dt_dev_reload_history_items(darktable.develop);
    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
  }

  /* update xmp file */
  dt_image_synch_xmp(img->id);
  //dt_mipmap_cache_remove(darktable.mipmap_cache, img->id);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE);

  // update xmp file
  //dt_image_synch_xmp(img->id);

  return exposure_value;
}



double _interpolate_cubic(double y1, double y2, double y3, double y4, double mu)
{
  double a0,a1,a2,a3,mu2;

  mu2 = mu*mu;
  a0 = y4 - y3 - y1 + y2;
  a1 = y1 - y2 - a0;
  a2 = y3 - y1;
  a3 = y2;

  a0 = -0.5*y1 + 1.5*y2 - 1.5*y3 + 0.5*y4;
  a1 = y1 - 2.5*y2 + 2*y3 - 0.5*y4;
  a2 = -0.5*y1 + 0.5*y3;
  a3 = y2;

  return(a0*mu*mu2+a1*mu2+a2*mu+a3);
}



double _interpolate_linear(double y1, double y2, double mu)
{
  double res = (y1 * (1 - mu) + y2 * mu);
  fprintf(stderr, "result: %f\n", res);
  double mu2;

  mu2 = (1-cos(mu*PI))/2;
  return(y1*(1-mu2)+y2*mu2);
  return res;
}



void _draw_line(cairo_t *cr, double line_width, double red, double green, double blue, double x1, double x2, double y1, double y2, double x3, double y3, int interpolate)
{
  double y0 = 0;
      //if(interpolate)
  //fprintf(stderr, "interpolate: %d ON\n", interpolate);

  for(int i = x1; i <= x2; i++)
  {
    double new_y2 = y2;
    double y2_mu = (1 / (x2 - x1)) * (i - x1);

    if(interpolate)
      fprintf(stderr, "interpolate: --- \n");

    if(interpolate)
      new_y2 = _interpolate_linear(y1, y2, y2_mu);

    if(interpolate)
      new_y2 = _interpolate_cubic(y0, y1, y2, y3, y2_mu);

    if(interpolate)
      fprintf(stderr, "_interpolate_linear x1: %f  y1: %f    x2: %f  y1: %f\n", x1, y1, x2, new_y2);

    cairo_set_line_width(cr, line_width);
    cairo_set_source_rgb(cr, red, green, blue);

    if(interpolate)
    {
      cairo_move_to(cr, i, y1);
      cairo_line_to(cr, i + 1, new_y2);
    }
    else
    {
      cairo_move_to(cr, x1, y1);
      cairo_line_to(cr, x2, new_y2);
    }

    cairo_stroke(cr);

    //y0 = y1;
    //y1 = new_y2;

    if(!interpolate)
      break;

    //i += (x2 - x1) / 3;
  }

  return;
}



void draw_diagram(float *data, int points, int type, cairo_t *cr, int32_t width, int32_t height, double line_width, double red, double green, double blue, int interpolate)
{
  int cur_x, cur_y, last_x, last_y, next_x, next_y, start_y = 0;
  float x1 = 0, x2 = 0, x3 = 0, x4 = 0, y1 = 0, y2 = 0, y3 = 0, y4 = 0;
  float min = data[0], max = data[0];//, diff;
  //int32_t area_width = points;
  //float *new_curve;

  /*if(interpolate) {
    area_width = width;
    points = width;
    new_curve = (float*)malloc(sizeof(float) * points);

    for(int i = 0; i < points; i++)
    {
      new_curve[i] = img->average_brightness;
    }
  }*/

  for(int i = 0; i < points; i++)
  {
    if(data[i] < min)
      min = data[i];

    if(data[i] > max)
      max = data[i];
  }

  width -= (2 * MARGIN);
  height -= (2 * MARGIN);
  height /= 3;
  start_y = MARGIN + (height * 2);

  last_x = MARGIN;
  last_y = start_y - (((data[0] - min) / (max - min)) * height);

  if(min == max)
    last_y = start_y;

  for(int i = 0; i < points; i++)
  {
    cur_x = MARGIN + (int)(((float)(width) / ((float)points - 1)) * (float)(i));
    cur_y = start_y - (int)(((float)(data[i] - min) / (float)(max - min)) * (float)height);
    next_x = MARGIN + (int)(((float)(width) / ((float)points - 1)) * (float)(i + 1));
    next_y = start_y - (int)(((float)(data[i + 1] - min) / (float)(max - min)) * (float)height);
    //cur_y = (int)_interpolate_linear((double)cur_y, (double)last_y, cur_x);

    //if(data[i] != 0.0f) 
    //{

      if(interpolate > 0)
      {
        x1 = MARGIN + (int)(((float)(width) / ((float)points - 1)) * (float)(i));
        y1 = start_y - (int)(((float)(data[i] - min) / (float)(max - min)) * (float)height);

        if(i + 1 < points - 1)
        {
          x2 = MARGIN + (int)(((float)(width) / ((float)points - 1)) * (float)(i + 1));
          y2 = start_y - (int)(((float)(data[i + 1] - min) / (float)(max - min)) * (float)height);
        }

        if(i + 2 < points - 1)
        {
          x3 = MARGIN + (int)(((float)(width) / ((float)points - 1)) * (float)(i + 2));
          y3 = start_y - (int)(((float)(data[i + 2] - min) / (float)(max - min)) * (float)height);
        }
        
        if(i + 3 < points - 1)
        {
          x4 = MARGIN + (int)(((float)(width) / ((float)points - 1)) * (float)(i + 3));
          y4 = start_y - (int)(((float)(data[i + 3] - min) / (float)(max - min)) * (float)height);
        }

        x1 = x1;
        x2 = x2;
        x3 = x3;
        x4 = x4;

        for(int k = x2; k < x3; k++)
        {
          double new_y2 = y2;
          double y2_mu = (1 / (x3 - x2)) * (k - x2);

          new_y2 = _interpolate_cubic(y1, y2, y3, y4, y2_mu);

          //fprintf(stderr, "interpolate: %d\n", interpolate);
          _draw_line(cr, line_width, red, green, blue, k, k + 1, y2, new_y2, 0, 0, FALSE);
        }
      }
      else
        _draw_line(cr, line_width, red, green, blue, last_x, cur_x, last_y, cur_y, next_x, next_y, FALSE);

      last_x = cur_x;
      last_y = cur_y;
    //}
  }

  return;
}

/*float get_img_average_brightness(uint32_t imgid, int32_t width, int32_t height, int32_t area_width, int32_t area_height)
{
  dt_mipmap_buffer_t buf;
  dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, width, height);
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, mip, DT_MIPMAP_BEST_EFFORT, 'r');

  uint8_t *rgbbuf = NULL;
  float result = -1.0f;
  if(buf.buf)
  {
    rgbbuf = (uint8_t *)calloc(buf.width * buf.height * 4, sizeof(uint8_t));

    if(rgbbuf)
    {
      result = 0.0f;

      for(int i = 0; i < buf.height; i++)
      {
        uint8_t *in = buf.buf + i * buf.width * 4;

        for(int j = 0; j < buf.width; j++, in += 4)
        {
          result += (0.2126 * in[0]) + (0.7152 * in[1]) + (0.0722 * in[2]);
        }
      }
    }

    result /= buf.height * buf.width;
  }

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
  return result;
}*/



float get_average_brightness(dt_view_t *self, uint32_t imgid, int32_t width, int32_t height, int32_t area_width, int32_t area_height)
{
  /* ??? */
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;

  /* ??? */
  dt_pthread_mutex_lock(&d->lock);

  dt_mipmap_buffer_t buf;
  dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, width, height);
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, mip, DT_MIPMAP_BEST_EFFORT, 'r');

  uint8_t *rgbbuf = NULL;
  float result = -1.0f;
  if(buf.buf)
  {
    rgbbuf = (uint8_t *)calloc(buf.width * buf.height * 4, sizeof(uint8_t));

    if(rgbbuf)
    {
      result = 0.0f;

      for(int i = 0; i < buf.height; i++)
      {
        uint8_t *in = buf.buf + i * buf.width * 4;

        for(int j = 0; j < buf.width; j++, in += 4)
        {
          result += (0.2126 * in[0]) + (0.7152 * in[1]) + (0.0722 * in[2]);
        }
      }
    }

    result /= buf.height * buf.width;
  }

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  /* ??? */
  dt_pthread_mutex_unlock(&d->lock);
  dt_control_queue_redraw();
  return result;
}



static void write_frames(dt_view_t *self)
{
  fprintf(stderr, "write_frames\n");
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;
  //dt_pthread_mutex_lock(&d->lock);
  d->collection_count = dt_collection_get_selected_count(darktable.collection);

  GList *all_images = dt_collection_get_selected(darktable.collection, d->collection_count);
  while(all_images)
  {
    int imgid = GPOINTER_TO_INT(all_images->data);
    dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
    //fprintf(stderr, "WRITE FRAME: %s ... num: %d ... exposure_correction: %f\n", img->filename, img->id, img->exposure_correction);
    dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);
    all_images = g_list_delete_link(all_images, all_images);
  }

  //dt_pthread_mutex_unlock(&d->lock);
  return;
}



static void reset_frames(dt_view_t *self)
{
  //fprintf(stderr, "reset_frames\n");
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;
  //dt_pthread_mutex_lock(&d->lock);
  d->collection_count = dt_collection_get_selected_count(darktable.collection);

  GList *all_images = dt_collection_get_selected(darktable.collection, d->collection_count);
  while(all_images)
  {
    int imgid = GPOINTER_TO_INT(all_images->data);
    dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    img->average_brightness = -1;
    img->timelapse_keyframe = 0;
    dt_image_cache_read_release(darktable.image_cache, img);
    all_images = g_list_delete_link(all_images, all_images);
  }

  //dt_pthread_mutex_unlock(&d->lock);
  d->redraw_timeout = g_timeout_add(500, _redraw, NULL);
  write_frames(self);
  return;
}



static void _reset_frames(gpointer instance, gpointer user_data) {
  //fprintf(stderr, "_reset_frames\n");
  dt_view_t *self = (dt_view_t *)user_data;
  //dt_timelapse_t *d = (dt_timelapse_t *)self->data;

  /* TO-DO */
  //d->progress = dt_control_progress_create(darktable.control, TRUE, _("calculate average brightness"));
  //dt_control_progress_make_cancellable(darktable.control, d->progress, _read_frames_cancel, NULL);
  reset_frames(self);
}


static void read_frames(dt_view_t *self, gboolean equalize)
{
  fprintf(stderr, "read_frames\n");
  dt_control_queue_redraw_center();
  double last_LW = 0;
  dt_image_t *last_img = NULL;
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;
  d->keyframing = TRUE;

  d->collection_count = dt_collection_get_selected_count(darktable.collection);

  GList *all_images = dt_collection_get_selected(darktable.collection, d->collection_count);
  GList *imageid_list = NULL;
  int row = 1;
  bool image_changed;
  int first_run = -1;

  /* first step - get average brightness, set keyframes on changes */
  while(all_images)
  {
    int imgid = GPOINTER_TO_INT(all_images->data);
    dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    img->exposure_correction = 0;
    img->timelapse_keyframe = 0;
    image_changed = FALSE;
    //fprintf(stderr, "IMG: %d ... average_brightness %Lf\n", imgid, img->average_brightness);

    if(img && img->average_brightness <= 0)
    {
      img->average_brightness = get_average_brightness(self, img->id, 
                                                       d->preview_width, d->preview_height,
                                                       d->preview_width, d->preview_height);
    }


    /* calc EV values*/
    double LWk = log2(sqrt((double)img->exif_aperture));
    double LWt = log2(1/(double)img->exif_exposure);
    double LW = LWk + LWt;

    /* set img as keyframe if exposure, aperture or iso was changed */
    if(last_img != NULL
       && (last_img->exif_exposure != img->exif_exposure
           || last_img->exif_aperture != img->exif_aperture
           || last_img->exif_iso != img->exif_iso))
    {
      img->exposure_correction = 0;
      img->timelapse_keyframe = 1;
      last_img->timelapse_keyframe = 1;

      image_changed = TRUE;
    }

    /* ramping exposure of images for last change */
    if(image_changed || row == d->collection_count || g_list_length(all_images) == 1)
    {  
      fprintf(stderr, "IMG: %s ... last_LW: %f ... LW: %f\n", img->filename, last_LW, LW);

      /* calc EV change*/
      if(last_LW > 0 && last_LW > LW && equalize)
      {
        last_img->exposure_correction = last_LW - LW;
        //fprintf(stderr, "IMG: %s ... exp: %f ... apert: %f ... iso: %f ... LW-change: %f\n", img->filename, img->exif_exposure, img->exif_aperture, img->exif_iso, last_LW - LW);
      }
      else if(last_LW > 0 && last_LW < LW && equalize)
      {
        img->exposure_correction = LW - last_LW;
        //fprintf(stderr, "IMG: %s ... exp: %f ... apert: %f ... iso: %f ... LW-change: %f\n", img->filename, img->exif_exposure, img->exif_aperture, img->exif_iso, LW - last_LW);
      }

      int ramping_row = 0;
      int ramping_frame_count = g_list_length(imageid_list);

      // Declare and initialize two arrays to hold the coordinates of the initial data points
      int N = (int)ramping_frame_count;
      double x[N], y[N];
   
      // Generate the points
      double xx = PI, step = 4 * PI / (N - 1);
      for(int i = 0; i < N; ++i, xx += step) {
          x[i] = xx;
          y[i] = sin(2 * xx) / xx;
          fprintf(stderr, "interpolate x %f ... y %f\n", x[i], y[i]);
      }

      if(first_run < 0)
        first_run = g_list_length(imageid_list);

      while(imageid_list && equalize)
      {

        //if(changeimgid != last_img->id || g_list_length(all_images) == 1)
        //{

          if(first_run > 0)
            ramping_row = first_run;

          int changeimgid = GPOINTER_TO_INT(imageid_list->data);
          double ramp_value = (double)last_img->exposure_correction / (double)ramping_frame_count * (double)ramping_row;
          fprintf(stderr, "change_images get: %d ... %f\n", changeimgid, ramp_value);
          dt_image_t *changeimg = dt_image_cache_get(darktable.image_cache, changeimgid, 'r');
          changeimg->exposure_correction = ramp_value;
          get_image_module_exposure(self, changeimg);
          dt_image_cache_read_release(darktable.image_cache, changeimg);

          //dt_image_cache_write_release(darktable.image_cache, changeimg, DT_IMAGE_CACHE_SAFE);
          //if(first_run == 0)
          ramping_row++;
        //}

        imageid_list = g_list_next(imageid_list);

        dt_control_queue_redraw_center();
      }

      if(first_run > 0)
        first_run = 0;

      /*if(g_list_length(all_images) == 0)
      {
        d->keyframing = FALSE;
      }*/
    }

    if(d->keyframing)
    {
      last_LW = LW;
      last_img = img;
      /* store */
      //dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_SAFE);
      //change_images = g_list_append(change_images, GINT_TO_POINTER(imgid));
      imageid_list = g_list_append(imageid_list, GINT_TO_POINTER(imgid));
      fprintf(stderr, "change_images %d add: %d\n", row, imgid);
      all_images = g_list_delete_link(all_images, all_images);
      fprintf(stderr, "g_list_length(all_images) %d ... row %d .. count %d\n", g_list_length(all_images), row, d->collection_count);
      row++;

      /*if(g_list_length(all_images) == 0)
        goto changed_images;*/

      if(g_list_length(all_images) == 0)
      {
        d->keyframing = FALSE;
      }
    }

    dt_image_cache_read_release(darktable.image_cache, img);
    //dt_control_queue_redraw_center();
  }

  g_list_free(all_images);
  g_list_free(imageid_list);
  

  write_frames(self);
  d->progress_fraction += 1.0 / d->progress_total;
  d->progress_fraction = 0.5;

  /* TO-DO */
  //dt_control_progress_set_progress(darktable.control, d->progress, d->progress_fraction);
  d->redraw_timeout = g_timeout_add(500, _redraw, NULL);
  return;
}

static void _read_frames(gpointer instance, gpointer user_data) {
  fprintf(stderr, "_read_frames\n");
  dt_view_t *self = (dt_view_t *)user_data;

  //dt_timelapse_t *d = (dt_timelapse_t *)self->data;
  //d->reset_average_brightness_values = TRUE;

  /* TO-DO */
  //d->progress = dt_control_progress_create(darktable.control, TRUE, _("calculate average brightness"));
  //dt_control_progress_make_cancellable(darktable.control, d->progress, _read_frames_cancel, NULL);
  read_frames(self, FALSE);
}

static void _equalize_frames(gpointer instance, gpointer user_data) {
  fprintf(stderr, "_read_frames\n");
  dt_view_t *self = (dt_view_t *)user_data;
  read_frames(self, TRUE);
}



int timelapse_image_expose(dt_view_t *self, uint32_t imgid, cairo_t *cr, 
                           int32_t width, int32_t height, int32_t area_width, int32_t area_height)
{
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;

  dt_mipmap_buffer_t buf;
  dt_mipmap_size_t mip = dt_mipmap_cache_get_matching_size(darktable.mipmap_cache, width, height);
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, mip, DT_MIPMAP_BEST_EFFORT, 'r');

  float scale = 1.0;
  cairo_surface_t *surface = NULL;
  uint8_t *rgbbuf = NULL;
  if(buf.buf)
  {
    rgbbuf = (uint8_t *)calloc(buf.width * buf.height * 4, sizeof(uint8_t));

    if(rgbbuf)
    {
      gboolean do_copy = TRUE;

      if(dt_conf_get_bool("cache_color_managed"))
      {
        pthread_rwlock_rdlock(&darktable.color_profiles->xprofile_lock);

        // we only color manage when a thumbnail is sRGB or AdobeRGB. everything else just gets dumped to the screen
        if(buf.color_space == DT_COLORSPACE_SRGB &&
           darktable.color_profiles->transform_srgb_to_display)
        {
          cmsDoTransform(darktable.color_profiles->transform_srgb_to_display,
                         buf.buf, rgbbuf, buf.width * buf.height);
          do_copy = FALSE;
        }
        else if(buf.color_space == DT_COLORSPACE_ADOBERGB
                && darktable.color_profiles->transform_adobe_rgb_to_display)
        {
          cmsDoTransform(darktable.color_profiles->transform_adobe_rgb_to_display,
                         buf.buf, rgbbuf, buf.width * buf.height);
          do_copy = FALSE;
        }
        else if(buf.color_space == DT_COLORSPACE_NONE)
        {
          //fprintf(stderr, "oops, there seems to be a code path not setting the color space of thumbnails!\n");
        }

        pthread_rwlock_unlock(&darktable.color_profiles->xprofile_lock);
      }

      if(do_copy)
      {
        for(int i = 0; i < buf.height; i++)
        {
          uint8_t *in = buf.buf + i * buf.width * 4;
          uint8_t *out = rgbbuf + i * buf.width * 4;

          for(int j = 0; j < buf.width; j++, in += 4, out += 4)
          {
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
          }
        }
      }

      const int32_t stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, buf.width);
      surface = cairo_image_surface_create_for_data(rgbbuf, CAIRO_FORMAT_RGB24, buf.width, buf.height, stride);
    }

    scale = fminf((width-2) / (float)buf.width, (height-2) / (float)buf.height);
  }

  // draw centered and fitted:
  cairo_save(cr);
  cairo_translate(cr, MARGIN + (width / 2.0), MARGIN + height / 2.0);
  cairo_scale(cr, scale * d->full_preview_zoom, scale * d->full_preview_zoom);

  if(buf.buf && surface)
  {
    cairo_translate(cr, -0.5 * buf.width, -0.5 * buf.height);
    cairo_set_source_surface(cr, surface, 0, 0);

    if((buf.width <= 8 && buf.height <= 8) || fabsf(scale - 1.0f) < 0.01f)
      cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);

    cairo_rectangle(cr, 0, 0, buf.width, buf.height);
    cairo_fill(cr);
    cairo_surface_destroy(surface);
    free(rgbbuf);
  }

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  cairo_save(cr);
  cairo_restore(cr);

  // kill all paths, in case img was not loaded yet, or is blocked:
  cairo_new_path(cr);
  cairo_restore(cr);

  //dt_pthread_mutex_unlock(mutex);

  if(buf.size != mip && buf.width != 8 && buf.height != 8) 
    return 1;

  return 0;
}



/* to-do using native gtktree */
void init_framelist(dt_view_t *self) 
{
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;
  d->framelist_store = gtk_list_store_new(9,
      G_TYPE_BOOLEAN, /* keyframe */
      G_TYPE_INT, /* framenumber */
      G_TYPE_STRING, /* filename */
      G_TYPE_STRING, /* correction */
      G_TYPE_STRING, /* brightness */
      G_TYPE_STRING, /* exposure */
      G_TYPE_STRING, /* aperture */
      G_TYPE_STRING, /* iso */
      G_TYPE_STRING /* focal length */
  );

  gtk_tree_view_set_model(GTK_TREE_VIEW(d->framelist_tree), GTK_TREE_MODEL(d->framelist_store));
  gtk_list_store_append(d->framelist_store, &d->framelist_iter);
}



/* to-do using native gtktree */
void draw_framelist(dt_view_t *self, dt_image_t *img, int frame, int last) 
{
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;
  d->framelist_tree = gtk_tree_view_new();

  gtk_list_store_append(d->framelist_store, &d->framelist_iter);

  char exposure_correction[256];
  if(img->exposure_correction > -1)
    snprintf(exposure_correction, sizeof(exposure_correction), "%.2g", img->exposure_correction);
  else
    snprintf(exposure_correction, sizeof(exposure_correction), "---");

  char average_brightness[256];
  if(img->average_brightness > -1)
    snprintf(average_brightness, sizeof(average_brightness), "%.6Lg", img->average_brightness);
  else
    snprintf(average_brightness, sizeof(average_brightness), "---");

  char exif_exposure[256];
  if(img->exif_exposure <= 0.5)
    snprintf(exif_exposure, sizeof(exif_exposure), "1/%.2g", 1 / img->exif_exposure);
  else if(img->exif_exposure <= 1)
    snprintf(exif_exposure, sizeof(exif_exposure), "%.1g''", img->exif_exposure);
  else
    snprintf(exif_exposure, sizeof(exif_exposure), "%.2g''", img->exif_exposure);

  char exif_aperture[256];
  snprintf(exif_aperture, sizeof(exif_aperture), "%.2g", img->exif_aperture);

  char exif_iso[256];
  snprintf(exif_iso, sizeof(exif_iso), "%.0f", img->exif_iso);

  char exif_focal_length[256];
  snprintf(exif_focal_length, sizeof(exif_focal_length), "%.0f mm", img->exif_focal_length);

  gtk_list_store_set(d->framelist_store, &d->framelist_iter, 
                     COL_KEYFRAME, img->timelapse_keyframe, 
                     COL_FRAME, frame, 
                     COL_FILENAME, img->filename, 
                     COL_CORRECTION, exposure_correction, 
                     COL_BRIGHTNESS, average_brightness, 
                     COL_EXPOSURE, exif_exposure, 
                     COL_APERTURE, exif_aperture, 
                     COL_ISO, exif_iso, 
                     COL_FOCAL_LENGTH, exif_focal_length, 
                     last);
}



void init(dt_view_t *self)
{
  self->data = calloc(1, sizeof(dt_timelapse_t));
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;

  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  GdkScreen *screen = gtk_widget_get_screen(window);
  if(!screen) screen = gdk_screen_get_default();

  int monitor = gdk_screen_get_monitor_at_window(screen, gtk_widget_get_window(window));

  GdkRectangle rect;
  gdk_screen_get_monitor_geometry(screen, monitor, &rect);

  //fprintf(stderr, "Width: %d, Height: %d", rect.width, rect.height);

  d->preview_height = rect.height / 2.5;
  d->preview_width = rect.width;
  d->table_select = 1;
  d->full_preview_zoom = 1;
  d->preview_height_change = FALSE;
}



void cleanup(dt_view_t *self)
{
  free(self->data);
}



int try_enter(dt_view_t *self)
{
  /* verify that there are images to display */
  if(dt_collection_get_count(darktable.collection) > 1)
  {
    return 0;
  }
  else
  {
    dt_control_log(_("there are no images in this collection"));
    return 1;
  }
}



void gui_init(dt_lib_module_t *self)
{
  fprintf(stderr, "gui_init\n");
}



void enter(dt_view_t *self)
{
  //darktable.gui->reset = 1;
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;

  // use display profile:
  d->old_profile_type = dt_conf_get_int("plugins/lighttable/export/icctype");
  dt_conf_set_int("plugins/lighttable/export/icctype", DT_COLORSPACE_DISPLAY);

  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_LEFT, TRUE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_RIGHT, FALSE, FALSE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_TOP, TRUE, TRUE);
  dt_ui_panel_show(darktable.gui->ui, DT_UI_PANEL_BOTTOM, TRUE, FALSE);

  // also hide arrows
  dt_ui_border_show(darktable.gui->ui, TRUE);
  gtk_widget_hide(darktable.gui->widgets.right_border);
  gtk_widget_hide(darktable.gui->widgets.bottom_border);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_TIMELAPSE_RESET,
                            G_CALLBACK(_reset_frames), (gpointer)self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_TIMELAPSE_INITIALIZE,
                            G_CALLBACK(_read_frames), (gpointer)self);

  dt_control_signal_connect(darktable.signals, DT_SIGNAL_TIMELAPSE_EQUALIZE,
                            G_CALLBACK(_equalize_frames), (gpointer)self);

  //d->redraw_timeout = g_timeout_add(500, _redraw, NULL);
  //darktable.gui->reset = 0;

  //check_preview(self);



  //GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  //gtk_widget_set_vexpand(scroll,TRUE);
  /*d->framelist_tree = gtk_tree_view_new();

  // Adding the outer container
  //gtk_container_add(GTK_CONTAINER(dt_ui_main_window(darktable.gui->ui)), scroll);

  //GtkTreeIter iter;
  // remove empty last row
  //gtk_list_store_remove (model, &iter);

  // Set up the cell renderers
  d->framelist_renderer = gtk_cell_renderer_toggle_new();
  //mycell = gtk.CellRendererText();
  ////g_signal_connect (renderer, "edited", G_CALLBACK(cell_edited), model);
  //g_signal_connect (renderer, "toggled", G_CALLBACK (cell_toggled), model);
  //g_object_set_data(G_OBJECT(renderer), "column", GINT_TO_POINTER (I_N_COLUMNS));
  GtkTreeViewColumn *column;
  column = gtk_tree_view_column_new_with_attributes(_("keyframe"), d->framelist_renderer, "active", COL_KEYFRAME, NULL);
  //column.add_attribute(renderer, 'cell-background', "red")
  //column.set_property('foreground','black')
  //column.add_attribute(mycell, 'foreground', "red") 
  //mycell.set_property('foreground-set', TRUE) 
  //g_object_set(G_OBJECT(renderer), "weight", PANGO_WEIGHT_BOLD, "weight-set", TRUE, NULL);
  //g_object_set(G_OBJECT(renderer), "foreground", "Red", "foreground-set", TRUE, NULL);

  gtk_tree_view_column_set_sizing(column , GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(column , 50);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->framelist_tree), column);

  d->framelist_renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("frame"), d->framelist_renderer, "text", COL_FRAME, "foreground", "Red", "foreground-set", TRUE, NULL);
  gtk_tree_view_column_set_sizing(column , GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(column , 50);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->framelist_tree), column);

  d->framelist_renderer = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes(_("filename"), d->framelist_renderer, "text", COL_FILENAME, NULL);
  gtk_tree_view_column_set_sizing(column , GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(column , 200);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->framelist_tree), column);

  d->framelist_renderer = gtk_cell_renderer_text_new();
  g_object_set(d->framelist_renderer, "editable", TRUE, NULL);
  //g_signal_connect (renderer, "edited", G_CALLBACK(cell_edited), model);
  //g_object_set_data (G_OBJECT (renderer), "column", GINT_TO_POINTER (I_TEMPERATURE_COLUMN));
  column = gtk_tree_view_column_new_with_attributes(_("correction"), d->framelist_renderer, "text", COL_CORRECTION, NULL);
  gtk_tree_view_column_set_sizing(column , GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(column , 100);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->framelist_tree), column);

  d->framelist_renderer = gtk_cell_renderer_text_new();
  g_object_set(d->framelist_renderer, "editable", TRUE, NULL);
  column = gtk_tree_view_column_new_with_attributes(_("brightness"), d->framelist_renderer, "text", COL_BRIGHTNESS, NULL);
  gtk_tree_view_column_set_sizing(column , GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(column , 100);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->framelist_tree), column);

  d->framelist_renderer = gtk_cell_renderer_text_new();
  g_object_set(d->framelist_renderer, "editable", TRUE, NULL);
  column = gtk_tree_view_column_new_with_attributes(_("exposure"), d->framelist_renderer, "text", COL_EXPOSURE, NULL);
  gtk_tree_view_column_set_sizing(column , GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(column , 100);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->framelist_tree), column);

  d->framelist_renderer = gtk_cell_renderer_text_new();
  g_object_set(d->framelist_renderer, "editable", TRUE, NULL);
  //g_signal_connect (renderer, "edited", G_CALLBACK(cell_edited), model);
  //g_object_set_data (G_OBJECT (renderer), "column", GINT_TO_POINTER (I_TARGET_LEVEL_COLUMN));
  column = gtk_tree_view_column_new_with_attributes(_("aperture"), d->framelist_renderer, "text", COL_APERTURE, NULL);
  gtk_tree_view_column_set_sizing(column , GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(column , 100);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->framelist_tree), column);

  d->framelist_renderer = gtk_cell_renderer_text_new();
  g_object_set(d->framelist_renderer, "editable", TRUE, NULL);
  column = gtk_tree_view_column_new_with_attributes(_("ISO"), d->framelist_renderer, "text", COL_ISO, NULL);
  gtk_tree_view_column_set_sizing(column , GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(column , 100);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->framelist_tree), column);

  d->framelist_renderer = gtk_cell_renderer_text_new();
  g_object_set(d->framelist_renderer, "editable", TRUE, NULL);
  column = gtk_tree_view_column_new_with_attributes(_("focal length"), d->framelist_renderer, "text", COL_FOCAL_LENGTH, NULL);
  gtk_tree_view_column_set_sizing(column , GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(column , 100);
  gtk_tree_view_append_column(GTK_TREE_VIEW(d->framelist_tree), column);

  // Attaching the model to the treeview
  gtk_tree_view_set_model(GTK_TREE_VIEW(tree), GTK_TREE_MODEL(model));

  GtkWidget *parent = gtk_widget_get_parent(dt_ui_center(darktable.gui->ui));
  GtkWidget *fixed;

  fixed = gtk_fixed_new();
  gtk_container_add(GTK_CONTAINER(parent), fixed);
  gtk_widget_show(fixed);

  d->framelist_store = gtk_list_store_new(9,
      G_TYPE_BOOLEAN,
      G_TYPE_INT,
      G_TYPE_STRING,
      G_TYPE_STRING,
      G_TYPE_STRING,
      G_TYPE_STRING,
      G_TYPE_STRING,
      G_TYPE_STRING,
      G_TYPE_STRING
  );

  gtk_tree_view_set_model(GTK_TREE_VIEW(d->framelist_tree), GTK_TREE_MODEL(d->framelist_store));
  gtk_list_store_append(d->framelist_store, &d->framelist_iter);

  char exposure_correction[256];
  snprintf(exposure_correction, sizeof(exposure_correction), "%.2g", 0.1454);

  char average_brightness[256];
  snprintf(average_brightness, sizeof(average_brightness), "%.6Lg", (long double)104.4785211);

  char exif_exposure[256];
  snprintf(exif_exposure, sizeof(exif_exposure), "1/%.2g", 1 / 0.5);

  char exif_aperture[256];
  snprintf(exif_aperture, sizeof(exif_aperture), "%.2g", 16.0);

  char exif_iso[256];
  snprintf(exif_iso, sizeof(exif_iso), "%.0f", 100.0);

  char exif_focal_length[256];
  snprintf(exif_focal_length, sizeof(exif_focal_length), "%.0f mm", 16.0);

  gtk_list_store_set(d->framelist_store, &d->framelist_iter, 
                     COL_KEYFRAME, FALSE, 
                     COL_FRAME, 158, 
                     COL_FILENAME, "photocopy 4", 
                     COL_CORRECTION, exposure_correction, 
                     COL_BRIGHTNESS, average_brightness, 
                     COL_EXPOSURE, exif_exposure, 
                     COL_APERTURE, exif_aperture, 
                     COL_ISO, exif_iso, 
                     COL_FOCAL_LENGTH, exif_focal_length, 
                     NULL);

  gtk_container_add(GTK_CONTAINER(fixed), d->framelist_tree);
  gtk_widget_show(d->framelist_tree);*/
}



void leave(dt_view_t *self)
{
  dt_control_signal_disconnect(darktable.signals,G_CALLBACK(_reset_frames), (gpointer)self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_read_frames), (gpointer)self);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_equalize_frames), (gpointer)self);
  dt_control_set_mouse_over_id(-1);
}



void reset(dt_view_t *self)
{
  dt_control_set_mouse_over_id(-1);
}



void mouse_enter(dt_view_t *self)
{
}


void mouse_leave(dt_view_t *self)
{
}


/*static gboolean _expose_again(gpointer user_data)
{
  dt_control_queue_redraw_center();
  dt_control_queue_redraw();
  return FALSE;
}*/



void expose(
    dt_view_t *self, 
    cairo_t *cr, 
    int32_t width, 
    int32_t height, 
    int32_t pointerx, 
    int32_t pointery)
{
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;
  int row = 0;
  char text[256];
  int row_height = (DT_PIXEL_APPLY_DPI(11) * 2.5);
  height -= d->preview_height;
  d->missed_frames = 0;

  if(d->keyframing)
    return;

  /* TO-DO */
  //d->progress_fraction = 0;
  //d->progress_total = 0;

  cairo_set_source_rgb(cr, .2, .2, .2);
  cairo_paint(cr);

  d->collection_count = dt_collection_get_selected_count(darktable.collection);
  //fprintf(stderr, "collection count: %d\n", d->collection_count);

  d->table_length = d->collection_count - (height / row_height);
  d->progress_total = d->collection_count;

  /* divider */
  cairo_set_line_width(cr, 0.1);
  cairo_set_source_rgb(cr, .6, .6, .6);
  cairo_move_to(cr, 0, d->preview_height-1);
  cairo_line_to(cr, width, d->preview_height-1);
  cairo_stroke(cr);

  cairo_set_line_width(cr, 0.2);
  cairo_set_source_rgb(cr, .09, .09, .09);
  cairo_move_to(cr, 0, d->preview_height);
  cairo_line_to(cr, width, d->preview_height);
  cairo_stroke(cr);

  /* table header - start */
  cairo_set_source_rgba(cr, .6, .6, .6, 1.0f);
  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, DT_PIXEL_APPLY_DPI(11));

  int row_pos = row_height + d->preview_height;
  int tab_pos = 50;

  cairo_move_to(cr, tab_pos, row_pos);
  cairo_show_text(cr, _("frame"));
  tab_pos += 50;

  cairo_move_to(cr, tab_pos, row_pos);
  cairo_show_text(cr, _("filename"));
  tab_pos += 200;

  cairo_set_source_rgb(cr, .7, .7, .2);
  if(d->correction_curve_visible) cairo_set_source_rgb(cr, .4, .4, .0);
  cairo_move_to(cr, tab_pos, row_pos);
  cairo_show_text(cr, _("correction"));
  tab_pos += 100;

  cairo_set_source_rgb(cr, .7, .2, .2);
  if(d->average_curve_visible) cairo_set_source_rgb(cr, .5, .0, .0);
  cairo_move_to(cr, tab_pos, row_pos);
  cairo_show_text(cr, _("brightness"));
  tab_pos += 100;

  cairo_set_source_rgb(cr, .0, .7, .0);
  if(d->exposure_curve_visible) cairo_set_source_rgb(cr, .0, .4, .0);
  cairo_move_to(cr, tab_pos, row_pos);
  cairo_show_text(cr, _("exposure"));
  tab_pos += 100;

  cairo_set_source_rgb(cr, .7, .1, .9);
  if(d->aperture_curve_visible) cairo_set_source_rgb(cr, .4, .0, .6);
  cairo_move_to(cr, tab_pos, row_pos);
  cairo_show_text(cr, _("aperture"));
  tab_pos += 100;

  cairo_set_source_rgb(cr, .0, .7, .7);
  if(d->iso_curve_visible) cairo_set_source_rgb(cr, .0, .4, .4);
  cairo_move_to(cr, tab_pos, row_pos);
  cairo_show_text(cr, _("ISO"));
  tab_pos += 100;

  cairo_set_source_rgba(cr, .6, .6, .6, 1.0f);
  cairo_move_to(cr, tab_pos, row_pos);
  cairo_show_text(cr, _("focal length"));
  tab_pos += 100;

  cairo_set_line_width(cr, 0.2);
  cairo_set_source_rgb(cr, .09, .09, .09);
  cairo_move_to(cr, 0, row_pos + (row_height * .25));
  cairo_line_to(cr, width, row_pos + (row_height * .25));
  cairo_stroke(cr);
  /* table header - end */

  d->exposure_curve = (float*)malloc(sizeof(float) * d->collection_count);
  d->aperture_curve = (float*)malloc(sizeof(float) * d->collection_count);
  d->average_curve = (float*)malloc(sizeof(float) * d->collection_count);
  d->correction_curve = (float*)malloc(sizeof(float) * d->collection_count);
  d->iso_curve = (float*)malloc(sizeof(float) * d->collection_count);


  /* boundaries - scroll to current frame in table */
  if(d->full_preview_mouse_x > MARGIN && d->full_preview_mouse_x < (width - MARGIN))
  {
    d->table_select = (int)(((float)d->collection_count / (float)(width - (2 * MARGIN))) * (float)(d->full_preview_mouse_x - MARGIN));

    /* scroll table */
    if(d->table_select != d->table_scroll)
      d->table_scroll = d->table_select;

    if(d->table_scroll > d->table_length)
      d->table_scroll = d->table_length + 1;
  }
  /* mouse on left */
  else if(d->full_preview_mouse_x > 0 && d->full_preview_mouse_x < MARGIN)
  {
    d->table_select = 0;
    d->table_scroll = d->table_select;
  }
  /* mouse on right */
  else if(d->full_preview_mouse_x > (width - MARGIN))
  {
    d->table_select = d->collection_count - 1;
    d->table_scroll = d->table_length + 1;
  }

  if(d->table_length < 0)
      d->table_scroll = 0;

  int num_of_rows = 1, start_row = -1;

  row = 0;
  GList *all_images = dt_collection_get_selected(darktable.collection, d->collection_count);

  /* native treeview */
  //init_framelist(self);

  dt_image_t *img = NULL;

  while(all_images)
  {
    int imgid = GPOINTER_TO_INT(all_images->data);

    /* to-do ... select collection not only selected imaged */
    img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    if(img)
    {
      
      if(img->timelapse_keyframe)
        cairo_set_source_rgba(cr, .7, .7, .7, 1.0f);
      else
        cairo_set_source_rgba(cr, .4, .4, .4, 1.0f);

      int row_pos = (row_height * (row - d->table_scroll + 2)) + d->preview_height;
      int row_top = row_pos - row_height + (row_height * 0.25);
      int row_bottom = row_top + row_height;

      if(img->average_brightness < 0)
      {
        img->average_brightness = get_average_brightness(self, img->id, 
                                                         width - (MARGIN * 2.0f), d->preview_height - (MARGIN * 2.0f),
                                                         width, height);
        d->missed_frames++;
      }


      /* get preview and select image-row in table */
      if(d->table_select == row) 
      {
        d->image_over = DT_VIEW_DESERT;
        cairo_set_source_rgba(cr, .5, .35, 0, 1.0f);
        cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        dt_control_set_mouse_over_id(d->full_preview_id);

        d->missed_frames += timelapse_image_expose(self, img->id, cr, 
                                                   width - (MARGIN * 2.0f), d->preview_height - (MARGIN * 2.0f),
                                                   width, height);
      }

      /* mouseover highlight image-row in table */
      if(d->mouse_y > row_top && d->mouse_y <= row_bottom && d->mouse_y > row_height + d->preview_height + (row_height * .25)) 
      {
        cairo_set_source_rgba(cr, 1, 1, 1, 1.0f);
        d->table_highlight = row;

        if(row == d->table_select) 
          cairo_set_source_rgba(cr, 1, .7, 0, 1.0f);

        dt_control_set_mouse_over_id(img->id);
      }

      if(row_pos >= ((row_height * 2) + d->preview_height) && row_top < (height + d->preview_height))
      {
        int tab_pos = 50;

        if(start_row < 0)
          start_row = row;

        /* native treeview */
        //draw_framelist(self, img, row, 0);

        /* table row - start */
        cairo_set_line_width(cr, 0.5);
        //cairo_set_source_rgb(cr, .4, .4, .4);
        cairo_move_to(cr, 20, row_pos);
        cairo_line_to(cr, 24, row_pos - 4);
        cairo_line_to(cr, 20, row_pos - 8);
        cairo_line_to(cr, 16, row_pos - 4);
        cairo_close_path(cr);

        if(img->timelapse_keyframe)
          cairo_fill(cr); 
        else
         cairo_stroke(cr);

        cairo_move_to(cr, tab_pos, row_pos);
        snprintf(text, sizeof(text), "%d", row + 1);
        cairo_show_text(cr, text);
        tab_pos += 50;

        /* show unloaded filenames wrapped in asterisks */
        cairo_move_to(cr, tab_pos, row_pos);
        //if(d->full_preview_id == d->image_storage.id && missing_thumbnails)
        //if(img->flags & DT_IMAGE_RAW)
        //  snprintf(text, sizeof(text), "%s", img->filename);
        //else
        snprintf(text, sizeof(text), "%s", img->filename);
        cairo_show_text(cr, text);
        tab_pos += 200;

        cairo_move_to(cr, tab_pos, row_pos);
        if(img->exposure_correction > -1)
          snprintf(text, sizeof(text), "%.2g", img->exposure_correction);
        else
          snprintf(text, sizeof(text), "---");
        cairo_show_text(cr, text);
        tab_pos += 100;

        cairo_move_to(cr, tab_pos, row_pos);
        if(img->average_brightness > -1)
          snprintf(text, sizeof(text), "%.6Lg", img->average_brightness);
        else
          snprintf(text, sizeof(text), "---");
        cairo_show_text(cr, text);
        tab_pos += 100;


        cairo_move_to(cr, tab_pos, row_pos);
        if(img->exif_exposure <= 0.5)
          snprintf(text, sizeof(text), "1/%.2g", 1 / img->exif_exposure);
        else if(img->exif_exposure <= 1)
          snprintf(text, sizeof(text), "%.1g''", img->exif_exposure);
        else
          snprintf(text, sizeof(text), "%.2g''", img->exif_exposure);
        cairo_show_text(cr, text);
        tab_pos += 100;


        cairo_move_to(cr, tab_pos, row_pos);
        snprintf(text, sizeof(text), "%.2g", img->exif_aperture);
        cairo_show_text(cr, text);
        tab_pos += 100;

        cairo_move_to(cr, tab_pos, row_pos);
        snprintf(text, sizeof(text), "%.0f", img->exif_iso);
        cairo_show_text(cr, text);
        tab_pos += 100;

        cairo_move_to(cr, tab_pos, row_pos);
        snprintf(text, sizeof(text), "%.0f mm", img->exif_focal_length);
        cairo_show_text(cr, text);
        tab_pos += 100;

        cairo_set_line_width(cr, 0.2);
        cairo_set_source_rgb(cr, .09, .09, .09);
        cairo_move_to(cr, 0, row_pos + (row_height * .25));
        cairo_line_to(cr, width, row_pos + (row_height * .25));
        cairo_stroke(cr);
        /* table row - end */

        num_of_rows++;
      }
    
    //printf("row: %d %f\n", row, average_brightness);

      d->average_curve[row] = img->average_brightness;
      d->exposure_curve[row] = img->exif_exposure;
      d->aperture_curve[row] = img->exif_aperture;
      d->iso_curve[row] = img->exif_iso;
      d->correction_curve[row] = img->exposure_correction;
      dt_image_cache_read_release(darktable.image_cache, img);

      /* scroll bar background */
      cairo_set_source_rgb(cr, .2, .2, .2);
      cairo_rectangle(cr, width - 5, height + row_height + 5, 5, height);
      cairo_fill(cr);

      /* scroll bar */
      cairo_set_source_rgb(cr, .15, .15, .15);
      cairo_rectangle(cr, width - 5, d->preview_height + row_height + 5 + (int)(((float)((height - 5) - row_height) / (float)d->collection_count) * (float)start_row), 5, (int)(((float)(height - 5) / (float)d->collection_count) * (float)num_of_rows));
      cairo_fill(cr);
      //fprintf(stderr, "count: %d ...  start: %d ... numof: %d\n", d->collection_count, start_row, num_of_rows);
    }

    row++;
    all_images = g_list_delete_link(all_images, all_images);
  }

  /* native treeview */
  //draw_framelist(self, img, row, -1);
    //else
    //  res = 1;

  //fprintf(stderr, "rows parsed: %d\n", row);
  //sqlite3_finalize(stmt);

  if(row > 0)
  {
    draw_diagram(d->average_curve, d->collection_count, 1, cr, width, d->preview_height, 1.5, .7, .2, .2, FALSE);
    draw_diagram(d->exposure_curve, d->collection_count, 0, cr, width, d->preview_height, 1.5, 0, .7, 0, FALSE);
    draw_diagram(d->aperture_curve, d->collection_count, 1, cr, width, d->preview_height, 1.5, .7, .1, .9, FALSE);
    draw_diagram(d->iso_curve, d->collection_count, 1, cr, width, d->preview_height, 1.5, 0, .7, .7, FALSE);
    draw_diagram(d->correction_curve, d->collection_count, 1, cr, width, d->preview_height, 1.5, .7, .7, .2, FALSE);
  }
  else
  {
    cairo_move_to(cr, 50, (row_height * 2) + d->preview_height);
    cairo_show_text(cr, "sorry, no images found in collection");
  }

  /* draw frame marker vertical-line */
  if((d->full_preview_mouse_x > 0 && d->full_preview_mouse_x < width)
     || d->full_preview_mouse_x < 0)
  {
    int frame_cur_x = 0;
    float frame_width = ((float)(width - (2 * MARGIN)) / ((float)d->collection_count - 1));

    if(d->full_preview_mouse_x > MARGIN && d->full_preview_mouse_x < (width - MARGIN))
    {
      frame_cur_x = d->full_preview_mouse_x;
    }
    else
    {
      frame_cur_x = MARGIN + (int)(frame_width * (float)(d->table_select));
    }

    cairo_set_line_width(cr, 0.2);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_move_to(cr, frame_cur_x, 0);
    cairo_line_to(cr, frame_cur_x, d->preview_height);
    cairo_stroke(cr);
  }

  if(d->keyframing)
  {

    if(d->missed_frames == 0)
    {
      d->keyframing = FALSE;
      write_frames(self);
    }
    else if(d->missed_frames > 0)
    {
      //fprintf(stderr, "d->missed_frames: %d\n", d->missed_frames);
      //read_frames(self);
      //g_timeout_add(500, _expose_again, 0);
    }
  }

  //fprintf(stderr, "%d\n", d->image_over.width);
}

void scrolled(dt_view_t *self, double x, double y, int up, int state)
{
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;

  /* TO-DO */
  /* zoom on preview area 
  if(y < d->preview_height)
  {

    if(up && d->full_preview_zoom < 20)
      d->full_preview_zoom += 0.2;
    else if(d->full_preview_zoom > 1)
      d->full_preview_zoom -= 0.2;

    goto end;
  }*/

  /* scroll the table */
  if(up > 0 && d->table_scroll >= 1)
    --d->table_scroll;
  else if(up < 1 && d->table_scroll <= d->table_length)
    ++d->table_scroll;

  //fprintf(stderr, "x: %f ... y: %f ... up: %d ... state: %d ... state: %d / %d\n", x, y, up, state, d->table_scroll, d->table_length);
  dt_control_queue_redraw_center();
  return;
}


void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  //fprintf(stderr, "x: %f ... y: %f ... which: %d\n", x, y, which);
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;
  d->mouse_moved = TRUE;
  dt_control_change_cursor(GDK_ARROW);

  if(!d->keyframing)
  {
    d->full_preview_mouse_x = -1;

    /* frame previed */
    if(y < d->preview_height - MARGIN)
    {
      d->full_preview_mouse_x = x;
    }
  }

  /* height change of preview area */
  if(y > d->preview_height - 2 && y < d->preview_height + 3)
  {
    dt_control_change_cursor(GDK_SB_V_DOUBLE_ARROW);
  }

  if(d->preview_height_change && y >= 240)
  {
    dt_control_change_cursor(GDK_SB_V_DOUBLE_ARROW);
    d->preview_height = (int)y;
  }

  d->mouse_y = y;
  dt_control_queue_redraw();
}


int button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  //fprintf(stderr, "x: %f ... y: %f ... which: %d ... type: %d ... state: %lu\n", x, y, which, type, (unsigned long)state);
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;

  if(!d->keyframing && which == 1)
  {
    d->table_select = d->table_highlight;
  }

  if(d->preview_height_change)
  {
    d->preview_height_change = FALSE;
  }

  d->redraw_timeout = g_timeout_add(500, _redraw, NULL);
  return 0;
}


int button_pressed(dt_view_t *self, double x, double y, int which, int type, uint32_t state)
{
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;

  if(y > d->preview_height - 2 && y < d->preview_height + 3 && which == 1)
  {
    d->preview_height_change = TRUE;
  }

  return 0;
}

int key_released(dt_view_t *self, guint key, guint state)
{
  return 0;
}

int key_pressed(dt_view_t *self, guint key, guint state)
{
  dt_control_accels_t *accels = &darktable.control->accels;
  dt_timelapse_t *d = (dt_timelapse_t *)self->data;

  const int layout = dt_conf_get_int("plugins/timelapse/layout");
  fprintf(stderr, "%d %d\n", (int)key, (int)accels->timelapse_left.accel_key);

  if(((key == accels->timelapse_left.accel_key && state == accels->timelapse_left.accel_mods) 
      || key == accels->timelapse_left.accel_key)
    && !d->full_preview_step_backward && layout == 1)
    d->full_preview_step_backward = TRUE;

  if(((key == accels->timelapse_right.accel_key && state == accels->timelapse_right.accel_mods) 
      || key == accels->timelapse_right.accel_key)
    && !d->full_preview_step_forward && layout == 1)
    d->full_preview_step_forward = TRUE;
    
  return 0;
}

void border_scrolled(dt_view_t *view, double x, double y, int which, int up)
{
}

void init_key_accels(dt_view_t *self)
{
  dt_accel_register_view(self, NC_("accel", "start and stop"), GDK_KEY_space, 0);
}

void connect_key_accels(dt_view_t *self)
{
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
