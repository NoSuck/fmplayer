#include <gtk/gtk.h>
#include <portaudio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <cairo.h>
#include <stdatomic.h>

#include "common/fmplayer_file.h"
#include "fmdriver/fmdriver_fmp.h"
#include "fmdriver/fmdriver_pmd.h"
#include "fmdriver/ppz8.h"
#include "libopna/opna.h"
#include "libopna/opnatimer.h"
#include "fmdsp/fmdsp.h"
#include "toneview.h"
#include "oscillo/oscillo.h"
#include "oscilloview.h"
#include "wavesave.h"
#include "common/fmplayer_common.h"
#include "fft/fft.h"

#include "fmplayer.xpm"
#include "fmplayer32.xpm"

#define DATADIR "/.local/share/fmplayer/"
//#define FMDSP_2X

enum {
  SRATE = 55467,
  PPZ8MIX = 0xa000,
  AUDIOBUFLEN = 0,
};

static struct {
  GtkWidget *mainwin;
  bool fmdsp_2x;
  GtkWidget *root_box_widget;
  GtkWidget *box_widget;
  GtkWidget *fmdsp_widget;
  GtkWidget *filechooser_widget;
  bool pa_initialized;
  bool pa_paused;
  PaStream *pastream;
  struct opna opna;
  struct opna_timer opna_timer;
  struct ppz8 ppz8;
  struct fmdriver_work work;
  struct fmdsp fmdsp;
  char adpcm_ram[OPNA_ADPCM_RAM_SIZE];
  struct fmplayer_file *fmfile;
  void *data;
  uint8_t vram[PC98_W*PC98_H];
  struct fmdsp_font font98;
  uint8_t font98data[FONT_ROM_FILESIZE];
  void *vram32;
  int vram32_stride;
  const char *current_uri;
  bool oscillo_should_update;
  struct oscillodata oscillodata_audiothread[LIBOPNA_OSCILLO_TRACK_COUNT];
  atomic_flag at_fftdata_flag;
  struct fmplayer_fft_data at_fftdata;
  struct fmplayer_fft_input_data fftdata;
} g = {
  .oscillo_should_update = true,
  .at_fftdata_flag = ATOMIC_FLAG_INIT,
};

static void quit(void) {
  if (g.pastream) {
    Pa_CloseStream(g.pastream);
  }
  if (g.pa_initialized) Pa_Terminate();
  fmplayer_file_free(g.fmfile);
  gtk_main_quit();
}

static void on_destroy(GtkWidget *w, gpointer ptr) {
  (void)w;
  (void)ptr;
  quit();
}

static void on_menu_quit(GtkMenuItem *menuitem, gpointer ptr) {
  (void)menuitem;
  (void)ptr;
  quit();
}

static void on_menu_save(GtkMenuItem *menuitem, gpointer ptr) {
  (void)menuitem;
  (void)ptr;
  if (g.current_uri) {
    char *uri = g_strdup(g.current_uri);
    wavesave_dialog(GTK_WINDOW(g.mainwin), uri);
    g_free(uri);
  }
}

static void on_tone_view(GtkMenuItem *menuitem, gpointer ptr) {
  (void)menuitem;
  (void)ptr;
  show_toneview();
}

static void on_oscillo_view(GtkMenuItem *menuitem, gpointer ptr) {
  (void)menuitem;
  (void)ptr;
  show_oscilloview();
}

static void msgbox_err(const char *msg) {
  GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(g.mainwin), GTK_DIALOG_MODAL,
                          GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                          msg);
  gtk_dialog_run(GTK_DIALOG(d));
  gtk_widget_destroy(d);
}


static int pastream_cb(const void *inptr, void *outptr, unsigned long frames,
                       const PaStreamCallbackTimeInfo *timeinfo,
                       PaStreamCallbackFlags statusFlags,
                       void *userdata) {
  (void)inptr;
  (void)timeinfo;
  (void)statusFlags;
  struct opna_timer *timer = (struct opna_timer *)userdata;
  int16_t *buf = (int16_t *)outptr;
  memset(outptr, 0, sizeof(int16_t)*frames*2);
  opna_timer_mix_oscillo(timer, buf, frames,
                         g.oscillo_should_update ?
                         g.oscillodata_audiothread : 0);

  if (!atomic_flag_test_and_set_explicit(
      &toneview_g.flag, memory_order_acquire)) {
    tonedata_from_opna(&toneview_g.tonedata, &g.opna);
    atomic_flag_clear_explicit(&toneview_g.flag, memory_order_release);
  }
  if (g.oscillo_should_update) {
    if (!atomic_flag_test_and_set_explicit(
      &oscilloview_g.flag, memory_order_acquire)) {
      memcpy(oscilloview_g.oscillodata, g.oscillodata_audiothread, sizeof(oscilloview_g.oscillodata));
      atomic_flag_clear_explicit(&oscilloview_g.flag, memory_order_release);
    }
  }
  if (!atomic_flag_test_and_set_explicit(
    &g.at_fftdata_flag, memory_order_acquire)) {
    fft_write(&g.at_fftdata, buf, frames);
    atomic_flag_clear_explicit(&g.at_fftdata_flag, memory_order_release);
  }
  return paContinue;
}

static void load_fontrom(void) {
  const char *path = "font.rom";
  const char *home = getenv("HOME");
  char *dpath = 0;
  fmdsp_font_from_font_rom(&g.font98, g.font98data);
  if (home) {
    const char *datadir = DATADIR;
    dpath = malloc(strlen(home)+strlen(datadir)+strlen(path) + 1);
    if (dpath) {
      strcpy(dpath, home);
      strcat(dpath, datadir);
      strcat(dpath, path);
      path = dpath;
    }
  }
  FILE *font = fopen(path, "r");
  free(dpath);
  if (!font) goto err;
  if (fseek(font, 0, SEEK_END) != 0) goto err_file;
  long size = ftell(font);
  if (size != FONT_ROM_FILESIZE) goto err_file;
  if (fseek(font, 0, SEEK_SET) != 0) goto err_file;
  if (fread(g.font98data, 1, FONT_ROM_FILESIZE, font) != FONT_ROM_FILESIZE) {
    goto err_file;
  }
  fclose(font);
  return;
err_file:
  fclose(font);
err:
  return;
}

static bool openfile(const char *uri) {
  struct fmplayer_file *fmfile = 0;
  if (!g.pa_initialized) {
    msgbox_err("Could not initialize Portaudio");
    goto err;
  }
  enum fmplayer_file_error error;
  fmfile = fmplayer_file_alloc(uri, &error);
  if (!fmfile) {
    const char *errstr = fmplayer_file_strerror(error);
    const char *errmain = "cannot load file: ";
    char *errbuf = malloc(strlen(errstr) + strlen(errmain) + 1);
    if (errbuf) {
      strcpy(errbuf, errmain);
      strcat(errbuf, errstr);
    }
    msgbox_err(errbuf ? errbuf : "cannot load file");
    free(errbuf);
    goto err;
  }
  if (!g.pastream) {
    PaError pe = Pa_OpenDefaultStream(&g.pastream, 0, 2, paInt16, SRATE, AUDIOBUFLEN,
                                      pastream_cb, &g.opna_timer);
    if (pe != paNoError) {
      msgbox_err("cannot open portaudio stream");
      goto err;
    }
  } else if (!g.pa_paused) {
    PaError pe = Pa_StopStream(g.pastream);
    if (pe != paNoError) {
      msgbox_err("Portaudio Error");
      goto err;
    }
  }
  fmplayer_file_free(g.fmfile);
  g.fmfile = fmfile;
  unsigned mask = opna_get_mask(&g.opna);
  g.work = (struct fmdriver_work){0};
  memset(g.adpcm_ram, 0, sizeof(g.adpcm_ram));
  fmplayer_init_work_opna(&g.work, &g.ppz8, &g.opna, &g.opna_timer, g.adpcm_ram);
  opna_set_mask(&g.opna, mask);
  char *disppath = g_filename_from_uri(uri, 0, 0);
  if (disppath) {
    strncpy(g.work.filename, disppath, sizeof(g.work.filename)-1);
    g_free(disppath);
  } else {
    strncpy(g.work.filename, uri, sizeof(g.work.filename)-1);
  }
  fmplayer_file_load(&g.work, g.fmfile, 1);
  fmdsp_vram_init(&g.fmdsp, &g.work, g.vram);
  Pa_StartStream(g.pastream);
  g.pa_paused = false;
  g.work.paused = false;
  {
    const char *turi = strdup(uri);
    free((void *)g.current_uri);
    g.current_uri = turi;
  }
  return true;
err:
  fmplayer_file_free(fmfile);
  return false;
}

static void on_file_activated(GtkFileChooser *chooser, gpointer ptr) {
  (void)ptr;
  gchar *filename = gtk_file_chooser_get_uri(chooser);
  if (filename) {
    openfile(filename);
    g_free(filename);
  }
}

static GtkWidget *create_menubar() {
  GtkWidget *menubar = gtk_menu_bar_new();
  GtkWidget *menu = gtk_menu_new();
  GtkWidget *file = gtk_menu_item_new_with_label("File");
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(file), menu);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file);
  GtkWidget *open = gtk_menu_item_new_with_label("Open");
  //g_signal_connect(open, "activate", G_CALLBACK(on_menu_open), 0);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), open);
  GtkWidget *save = gtk_menu_item_new_with_label("Save wavefile");
  g_signal_connect(save, "activate", G_CALLBACK(on_menu_save), 0);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), save);
  GtkWidget *quit = gtk_menu_item_new_with_label("Quit");
  g_signal_connect(quit, "activate", G_CALLBACK(on_menu_quit), 0);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit);

  GtkWidget *window = gtk_menu_item_new_with_label("Window");
  GtkWidget *filemenu = gtk_menu_new();
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(window), filemenu);
  gtk_menu_shell_append(GTK_MENU_SHELL(menubar), window);
  GtkWidget *toneview = gtk_menu_item_new_with_label("Tone view");
  g_signal_connect(toneview, "activate", G_CALLBACK(on_tone_view), 0);
  gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), toneview);
  GtkWidget *oscilloview = gtk_menu_item_new_with_label("Oscillo view");
  g_signal_connect(oscilloview, "activate", G_CALLBACK(on_oscillo_view), 0);
  gtk_menu_shell_append(GTK_MENU_SHELL(filemenu), oscilloview);
  return menubar;
}

static gboolean draw_cb(GtkWidget *w,
                 cairo_t *cr,
                 gpointer p) {
  (void)w;
  (void)p;
  if (!atomic_flag_test_and_set_explicit(
    &g.at_fftdata_flag, memory_order_acquire)) {
    memcpy(&g.fftdata.fdata, &g.at_fftdata, sizeof(g.fftdata));
    atomic_flag_clear_explicit(&g.at_fftdata_flag, memory_order_release);
  }
  fmdsp_update(&g.fmdsp, &g.work, &g.opna, g.vram, &g.fftdata);
  fmdsp_vrampalette(&g.fmdsp, g.vram, g.vram32, g.vram32_stride);
  cairo_surface_t *s = cairo_image_surface_create_for_data(
    g.vram32, CAIRO_FORMAT_RGB24, PC98_W, PC98_H, g.vram32_stride);
  if (g.fmdsp_2x) cairo_scale(cr, 2.0, 2.0);
  cairo_set_source_surface(cr, s, 0.0, 0.0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
  cairo_paint(cr);
  cairo_surface_destroy(s);
  return FALSE;
}

static gboolean tick_cb(GtkWidget *w,
                        GdkFrameClock *frame_clock,
                        gpointer p) {
  (void)w;
  (void)frame_clock;
  gtk_widget_queue_draw(GTK_WIDGET(p));
  return G_SOURCE_CONTINUE;
}

static void destroynothing(gpointer p) {
  (void)p;
}

static void mask_set(int p, bool shift, bool control) {
  if (!control) {
    if (p >= 11) return;
    static const unsigned masktbl[11] = {
      LIBOPNA_CHAN_FM_1,
      LIBOPNA_CHAN_FM_2,
      LIBOPNA_CHAN_FM_3,
      LIBOPNA_CHAN_FM_4,
      LIBOPNA_CHAN_FM_5,
      LIBOPNA_CHAN_FM_6,
      LIBOPNA_CHAN_SSG_1,
      LIBOPNA_CHAN_SSG_2,
      LIBOPNA_CHAN_SSG_3,
      LIBOPNA_CHAN_DRUM_ALL,
      LIBOPNA_CHAN_ADPCM,
    };
    unsigned mask = masktbl[p];
    if (shift) {
      opna_set_mask(&g.opna, ~mask);
      ppz8_set_mask(&g.ppz8, -1);
    } else {
      opna_set_mask(&g.opna, opna_get_mask(&g.opna) ^ mask);
    }
  } else {
    if (p >= 8) return;
    unsigned mask = 1u<<p;
    if (shift) {
      ppz8_set_mask(&g.ppz8, ~mask);
      opna_set_mask(&g.opna, -1);
    } else {
      ppz8_set_mask(&g.ppz8, ppz8_get_mask(&g.ppz8) ^ mask);
    }
  }
}

static void create_box(void) {
  if (g.box_widget) {
    g_object_ref(G_OBJECT(g.fmdsp_widget));
    gtk_container_remove(GTK_CONTAINER(g.box_widget), g.fmdsp_widget);
    g_object_ref(G_OBJECT(g.filechooser_widget));
    gtk_container_remove(GTK_CONTAINER(g.box_widget), g.filechooser_widget);
    gtk_container_remove(GTK_CONTAINER(g.root_box_widget), g.box_widget);
  }
  gtk_widget_set_size_request(g.fmdsp_widget,
                              PC98_W * (g.fmdsp_2x + 1),
                              PC98_H * (g.fmdsp_2x + 1));
  GtkOrientation o = g.fmdsp_2x ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL;
  g.box_widget = gtk_box_new(o, 0);
  gtk_box_pack_start(GTK_BOX(g.root_box_widget), g.box_widget, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g.box_widget), g.fmdsp_widget, FALSE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g.box_widget), g.filechooser_widget, TRUE, TRUE, 0);
  gtk_widget_show_all(g.root_box_widget);
}

static gboolean key_press_cb(GtkWidget *w,
                             GdkEvent *e,
                             gpointer ptr) {
  (void)w;
  (void)ptr;
  const GdkModifierType ALLACCELS = GDK_CONTROL_MASK | GDK_SHIFT_MASK | GDK_MOD1_MASK;
  if (GDK_KEY_F1 <= e->key.keyval && e->key.keyval <= GDK_KEY_F10) {
    if ((e->key.state & ALLACCELS) == GDK_CONTROL_MASK) {
      fmdsp_palette_set(&g.fmdsp, e->key.keyval - GDK_KEY_F1);
      return TRUE;
    }
  }
  guint keyval;
  gdk_keymap_translate_keyboard_state(gdk_keymap_get_default(),
                                      e->key.hardware_keycode,
                                      0,
                                      e->key.group,
                                      &keyval, 0, 0, 0);
  bool shift = e->key.state & GDK_SHIFT_MASK;
  bool ctrl = e->key.state & GDK_CONTROL_MASK;
  switch (keyval) {
  case GDK_KEY_F6:
    if (g.current_uri) {
      openfile(g.current_uri);
    }
    break;
  case GDK_KEY_F7:
    if (g.pa_paused) {
      Pa_StartStream(g.pastream);
      g.pa_paused = false;
      g.work.paused = false;
    } else {
      Pa_StopStream(g.pastream);
      g.pa_paused = true;
      g.work.paused = true;
    }
    break;
  case GDK_KEY_F11:
    fmdsp_dispstyle_set(&g.fmdsp, (g.fmdsp.style+1) % FMDSP_DISPSTYLE_CNT);
    break;
  case GDK_KEY_F12:
    g.fmdsp_2x ^= 1;
    create_box();
    break;
  case GDK_KEY_1:
    mask_set(0, shift, ctrl);
    break;
  case GDK_KEY_2:
    mask_set(1, shift, ctrl);
    break;
  case GDK_KEY_3:
    mask_set(2, shift, ctrl);
    break;
  case GDK_KEY_4:
    mask_set(3, shift, ctrl);
    break;
  case GDK_KEY_5:
    mask_set(4, shift, ctrl);
    break;
  case GDK_KEY_6:
    mask_set(5, shift, ctrl);
    break;
  case GDK_KEY_7:
    mask_set(6, shift, ctrl);
    break;
  case GDK_KEY_8:
    mask_set(7, shift, ctrl);
    break;
  case GDK_KEY_9:
    mask_set(8, shift, ctrl);
    break;
  case GDK_KEY_0:
    mask_set(9, shift, ctrl);
    break;
  case GDK_KEY_minus:
    mask_set(10, shift, ctrl);
    break;
  // jp106 / pc98
  case GDK_KEY_asciicircum:
  // us
  case GDK_KEY_equal:
    opna_set_mask(&g.opna, ~opna_get_mask(&g.opna));
    ppz8_set_mask(&g.ppz8, ~ppz8_get_mask(&g.ppz8));
    break;
  case GDK_KEY_backslash:
    opna_set_mask(&g.opna, 0);
    ppz8_set_mask(&g.ppz8, 0);
    break;
  default:
    return FALSE;
  }
  return TRUE;
}

static void drag_data_recv_cb(
  GtkWidget *w,
  GdkDragContext *ctx,
  gint x, gint y,
  GtkSelectionData *data,
  guint info, guint time, gpointer ptr) {
  (void)w;
  (void)x;
  (void)y;
  (void)info;
  (void)ptr;
  gchar **uris = gtk_selection_data_get_uris(data);
  if (uris && uris[0]) {
    openfile(uris[0]);
  }
  g_strfreev(uris);
  gtk_drag_finish(ctx, TRUE, FALSE, time);
}

int main(int argc, char **argv) {
#ifdef ENABLE_NEON
  opna_ssg_sinc_calc_func = opna_ssg_sinc_calc_neon;
  fmdsp_vramlookup_func = fmdsp_vramlookup_neon;
#endif
#ifdef ENABLE_SSE
  if (__builtin_cpu_supports("sse2")) opna_ssg_sinc_calc_func = opna_ssg_sinc_calc_sse2;
  if (__builtin_cpu_supports("ssse3")) fmdsp_vramlookup_func = fmdsp_vramlookup_ssse3;
#endif
  fft_init_table();
  load_fontrom();
  gtk_init(&argc, &argv);
  {
    GList *iconlist = 0;
    iconlist = g_list_append(iconlist, gdk_pixbuf_new_from_xpm_data(fmplayer_xpm_16));
    iconlist = g_list_append(iconlist, gdk_pixbuf_new_from_xpm_data(fmplayer_xpm_32));
    gtk_window_set_default_icon_list(iconlist);
    g_list_free(iconlist);
  }
  GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  g.mainwin = w;
  gtk_window_set_resizable(GTK_WINDOW(w), FALSE);
  gtk_window_set_title(GTK_WINDOW(w), "FMPlayer");
  g_signal_connect(w, "destroy", G_CALLBACK(on_destroy), 0);
  g.root_box_widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(w), g.root_box_widget);

  GtkWidget *menubar = create_menubar();
  gtk_box_pack_start(GTK_BOX(g.root_box_widget), menubar, FALSE, TRUE, 0);

  g.fmdsp_widget = gtk_drawing_area_new();
  g_signal_connect(g.fmdsp_widget, "draw", G_CALLBACK(draw_cb), 0);

  g.filechooser_widget = gtk_file_chooser_widget_new(GTK_FILE_CHOOSER_ACTION_OPEN);
  g_signal_connect(g.filechooser_widget, "file-activated", G_CALLBACK(on_file_activated), 0);

  create_box();

  g.pa_initialized = (Pa_Initialize() == paNoError);
  fmdsp_init(&g.fmdsp, &g.font98);
  fmdsp_vram_init(&g.fmdsp, &g.work, g.vram);
  g.vram32_stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, PC98_W);
  g.vram32 = malloc((g.vram32_stride*PC98_H)*4);

  g_signal_connect(w, "key-press-event", G_CALLBACK(key_press_cb), 0);
  gtk_drag_dest_set(
    w, GTK_DEST_DEFAULT_MOTION|GTK_DEST_DEFAULT_HIGHLIGHT|GTK_DEST_DEFAULT_DROP,
    0, 0, GDK_ACTION_COPY);
  gtk_drag_dest_add_uri_targets(w);
  g_signal_connect(w, "drag-data-received", G_CALLBACK(drag_data_recv_cb), 0);
  gtk_widget_add_events(w, GDK_KEY_PRESS_MASK);
  gtk_widget_show_all(w);
  gtk_widget_add_tick_callback(w, tick_cb, g.fmdsp_widget, destroynothing);
  gtk_main();
  return 0;
}
