#include <gtk/gtk.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <libxfce4panel/libxfce4panel.h>
#include <xfconf/xfconf.h>
#ifndef HAVE_PANGO_FONT_MAP_ADD_FONT_FILE
#include <fontconfig/fontconfig.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PADDING 2
#define FONT_ACTIVE_N 5
#define FONT_SLEEP_N 2

static const gunichar font_active[FONT_ACTIVE_N] = {
    0xE905, 0xE904, 0xE903, 0xE902, 0xE901
};

static const gunichar font_sleep[FONT_SLEEP_N] = {
    0xE900, 0xE8FF
};

typedef struct {
    gint high_rate;
    gint low_rate;
    gint poll_period;
    gboolean smoothing_enabled;
    gint smoothing_value;
    gboolean sleeping_enabled;
    gint sleeping_threshold;
    gint wakeup_threshold;
    gint sleeping_rate;
    gboolean sleep_animation;
} XfruncatSettings;

static const XfruncatSettings XFRUNCAT_SETTINGS_DEFAULT = {
    .high_rate = 30,
    .low_rate = 2,
    .poll_period = 1000,
    .smoothing_enabled = TRUE,
    .smoothing_value = 2000,
    .sleeping_enabled = TRUE,
    .sleeping_threshold = 8,
    .wakeup_threshold = 12,
    .sleeping_rate = 4,
    .sleep_animation = TRUE,
};

typedef struct {
    XfcePanelPlugin *plugin;
    GtkWidget *da;
    gint panel_size;
    XfruncatSettings settings;
    gdouble smoothed;
    guint tick_timer;
    guint frame;
    gint sleep_counter;
    gboolean sleeping;
    gboolean font_loaded;
    gint64 last_cpu_time;
    gint64 last_frame_time;
    gint font_width;
    gint font_height;
    PangoLayout *layout;
    PangoFontDescription *font_desc;
    GdkRGBA color;
    cairo_surface_t *glyph_cache[FONT_ACTIVE_N + FONT_SLEEP_N];
    gint cached_panel_size;
} XfruncatPlugin;

__attribute__((constructor))
static void early_xfconf_init(void)
{
    GError *error = NULL;
    if (!xfconf_init(&error))
    {
        g_warning("xfruncat: xfconf_init failed: %s", error->message);
        g_error_free(error);
    }
}

static void load_settings(XfruncatPlugin *xf);
static void save_settings(XfruncatPlugin *xf);
static gboolean tick_cb(gpointer data);
static gboolean draw_cb(GtkWidget *w, cairo_t *cr, gpointer data);
static void on_free(XfcePanelPlugin *plugin, XfruncatPlugin *xf);
static gboolean on_size_changed(XfcePanelPlugin *plugin, gint size, XfruncatPlugin *xf);
static void on_configure(XfcePanelPlugin *plugin, XfruncatPlugin *xf);
static void update_color(XfruncatPlugin *xf);
static void invalidate_cache(XfruncatPlugin *xf);
static void xfruncat_construct(XfcePanelPlugin *plugin);

static gdouble
read_cpu_load(void)
{
    static gulong prev_user, prev_nice, prev_system, prev_idle;
    static gulong prev_iowait, prev_irq, prev_softirq, prev_steal;
    static gint first = 1;

    FILE *f = fopen("/proc/stat", "r");
    if (!f) return 0.0;

    char buf[256];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0.0; }
    fclose(f);

    gulong user, nice, system, idle, iowait, irq, softirq, steal;
    gint n = sscanf(buf, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
                    &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    if (n < 4) return 0.0;

    if (first) {
        prev_user = user; prev_nice = nice; prev_system = system;
        prev_idle = idle; prev_iowait = iowait;
        prev_irq = irq; prev_softirq = softirq; prev_steal = steal;
        first = 0;
        return 0.0;
    }

    gulong d_user = user - prev_user;
    gulong d_nice = nice - prev_nice;
    gulong d_system = system - prev_system;
    gulong d_idle = idle - prev_idle;
    gulong d_iowait = iowait - prev_iowait;
    gulong d_irq = irq - prev_irq;
    gulong d_softirq = softirq - prev_softirq;
    gulong d_steal = steal - prev_steal;

    gulong d_used = d_user + d_nice + d_system + d_irq + d_softirq + d_steal;
    gulong d_total = d_used + d_idle + d_iowait;

    prev_user = user; prev_nice = nice; prev_system = system;
    prev_idle = idle; prev_iowait = iowait;
    prev_irq = irq; prev_softirq = softirq; prev_steal = steal;

    return d_total > 0 ? CLAMP((gdouble)d_used / (gdouble)d_total, 0.0, 1.0) : 0.0;
}

static gboolean
find_and_load_font(void)
{
    const gchar *paths[] = {
        POLYCAT_FONT_SRC_DIR "/polycat.ttf",
        POLYCAT_FONT_INSTALL_DIR "/polycat.ttf",
        NULL
    };

    gchar *xdg_path = g_build_filename(g_get_user_data_dir(),
        "xfce4", "xfruncat", "polycat.ttf", NULL);

#ifdef HAVE_PANGO_FONT_MAP_ADD_FONT_FILE
    PangoFontMap *fontmap = pango_cairo_font_map_get_default();
    GError *error = NULL;
#else
    FcConfig *config = FcConfigGetCurrent();
#endif

    for (const gchar **p = paths; *p; p++)
    {
        if (g_file_test(*p, G_FILE_TEST_EXISTS))
        {
            g_debug("loading font from %s", *p);
#ifdef HAVE_PANGO_FONT_MAP_ADD_FONT_FILE
            if (pango_font_map_add_font_file(fontmap, *p, &error))
#else
            if (FcConfigAppFontAddFile(config, (const FcChar8 *)*p))
#endif
            {
                g_free(xdg_path);
                return TRUE;
            }
#ifdef HAVE_PANGO_FONT_MAP_ADD_FONT_FILE
            g_clear_error(&error);
#endif
        }
    }

    if (g_file_test(xdg_path, G_FILE_TEST_EXISTS))
    {
        g_debug("loading font from %s", xdg_path);
#ifdef HAVE_PANGO_FONT_MAP_ADD_FONT_FILE
        if (pango_font_map_add_font_file(fontmap, xdg_path, &error))
#else
        if (FcConfigAppFontAddFile(config, (const FcChar8 *)xdg_path))
#endif
        {
            g_free(xdg_path);
            return TRUE;
        }
#ifdef HAVE_PANGO_FONT_MAP_ADD_FONT_FILE
        g_clear_error(&error);
#endif
    }

    g_free(xdg_path);
    g_warning("polycat.ttf not found");
    return FALSE;
}

static void
rebuild_glyph_cache(XfruncatPlugin *xf)
{
    for (gint i = 0; i < FONT_ACTIVE_N + FONT_SLEEP_N; i++)
    {
        if (xf->glyph_cache[i])
            cairo_surface_destroy(xf->glyph_cache[i]);
        xf->glyph_cache[i] = NULL;
    }
    xf->cached_panel_size = xf->panel_size;

    gint size = xf->panel_size;
    if (size <= 0) return;

    const gunichar *glyph_sets[] = { font_active, font_sleep };
    gint set_counts[] = { FONT_ACTIVE_N, FONT_SLEEP_N };
    gint idx = 0;

    for (gint s = 0; s < 2; s++)
    {
        for (gint i = 0; i < set_counts[s]; i++)
        {
            gchar utf8[8];
            gint len = g_unichar_to_utf8(glyph_sets[s][i], utf8);
            utf8[len] = '\0';

            cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
            cairo_t *cache_cr = cairo_create(surf);

            pango_layout_set_text(xf->layout, utf8, -1);

            gdouble gsize = size - PADDING * 2;
            gdouble scale = gsize / MAX(xf->font_width, xf->font_height);
            gdouble ox = (size - xf->font_width * scale) / 2.0;
            gdouble oy = (size - xf->font_height * scale) / 2.0;

            cairo_translate(cache_cr, ox, oy);
            cairo_scale(cache_cr, scale, scale);
            cairo_set_source_rgba(cache_cr, xf->color.red, xf->color.green, xf->color.blue, xf->color.alpha);
            pango_cairo_show_layout(cache_cr, xf->layout);

            cairo_destroy(cache_cr);
            xf->glyph_cache[idx++] = surf;
        }
    }
}

static void
render_frame(cairo_t *cr, XfruncatPlugin *xf)
{
    guint n = xf->sleeping ? FONT_SLEEP_N : FONT_ACTIVE_N;
    gint base = xf->sleeping ? FONT_ACTIVE_N : 0;
    gint cache_idx = base + (xf->frame % n);

    if (xf->cached_panel_size != xf->panel_size || !xf->glyph_cache[cache_idx])
    {
        rebuild_glyph_cache(xf);
        if (!xf->glyph_cache[cache_idx]) return;
    }

    cairo_set_source_surface(cr, xf->glyph_cache[cache_idx], 0, 0);
    cairo_paint(cr);
}

static gboolean
draw_cb(GtkWidget *w, cairo_t *cr, gpointer data)
{
    XfruncatPlugin *xf = (XfruncatPlugin *)data;
    (void)w;

    if (xf->font_loaded)
        render_frame(cr, xf);

    return FALSE;
}

static guint
calc_anim_interval(XfruncatPlugin *xf)
{
    if (xf->sleeping)
        return 1000 / xf->settings.sleeping_rate;

    gdouble load = CLAMP(xf->smoothed, 0.0, 1.0);
    guint rate = xf->settings.low_rate
        + (guint)(load * (xf->settings.high_rate - xf->settings.low_rate));
    return 1000 / CLAMP(rate, 1, 255);
}

static gboolean
tick_cb(gpointer data)
{
    XfruncatPlugin *xf = (XfruncatPlugin *)data;
    gint64 now = g_get_monotonic_time();
    if (now - xf->last_cpu_time >= xf->settings.poll_period * 1000)
    {
        gdouble load = read_cpu_load();

        if (xf->settings.smoothing_enabled && xf->settings.smoothing_value > 0)
        {
            gdouble alpha = 1.0 - exp(-(gdouble)xf->settings.poll_period
                                      / xf->settings.smoothing_value);
            xf->smoothed += (load - xf->smoothed) * CLAMP(alpha, 0.01, 0.99);
        }
        else
        {
            xf->smoothed = load;
        }

        gchar tooltip[32];
        g_snprintf(tooltip, sizeof(tooltip), "CPU: %.0f%%", xf->smoothed * 100.0);
        gtk_widget_set_tooltip_text(xf->da, tooltip);

        if (xf->settings.sleeping_enabled)
        {
            gdouble s_thresh = xf->settings.sleeping_threshold / 100.0;
            gdouble w_thresh = xf->settings.wakeup_threshold / 100.0;

            if (xf->sleeping)
            {
                if (xf->smoothed > w_thresh)
                {
                    xf->sleeping = FALSE;
                    xf->sleep_counter = 0;
                }
            }
            else
            {
                if (xf->smoothed < s_thresh)
                {
                    xf->sleep_counter++;
                    if (xf->sleep_counter > 3)
                    {
                        xf->sleeping = TRUE;
                        gtk_widget_queue_draw(xf->da);
                    }
                }
                else
                {
                    xf->sleep_counter = 0;
                }
            }
        }
        else
        {
            xf->sleeping = FALSE;
        }

        xf->last_cpu_time = now;
    }

    if (xf->sleeping && !xf->settings.sleep_animation)
    {
        xf->tick_timer = g_timeout_add_seconds(MAX(xf->settings.poll_period / 1000, 1), tick_cb, xf);
        return G_SOURCE_REMOVE;
    }

    guint interval = calc_anim_interval(xf);
    guint n = xf->sleeping ? FONT_SLEEP_N : FONT_ACTIVE_N;

    if (xf->last_frame_time == 0 || now - xf->last_frame_time >= interval * 1000)
    {
        xf->frame = (xf->frame + 1) % n;
        gtk_widget_queue_draw(xf->da);
        xf->last_frame_time = now;
    }

    xf->tick_timer = g_timeout_add(interval, tick_cb, xf);
    return G_SOURCE_REMOVE;
}

static void
on_free(XfcePanelPlugin *plugin, XfruncatPlugin *xf)
{
    (void)plugin;
    if (xf->tick_timer) g_source_remove(xf->tick_timer);
    for (gint i = 0; i < FONT_ACTIVE_N + FONT_SLEEP_N; i++)
        if (xf->glyph_cache[i]) cairo_surface_destroy(xf->glyph_cache[i]);
    if (xf->layout) g_object_unref(xf->layout);
    if (xf->font_desc) pango_font_description_free(xf->font_desc);
    g_free(xf);
}

static gboolean
on_size_changed(XfcePanelPlugin *plugin, gint size, XfruncatPlugin *xf)
{
    (void)plugin;
    xf->panel_size = size;
    invalidate_cache(xf);
    gtk_widget_set_size_request(xf->da, size, size);
    return TRUE;
}

static void
on_orientation_changed(XfcePanelPlugin *plugin, GtkOrientation orient,
                       XfruncatPlugin *xf)
{
    (void)plugin;
    (void)orient;
    gtk_widget_set_size_request(xf->da, xf->panel_size, xf->panel_size);
}

static void
show_about(XfcePanelPlugin *plugin, XfruncatPlugin *xf)
{
    (void)plugin;
    (void)xf;
    const gchar *authors[] = { "xfruncat authors", NULL };
    gtk_show_about_dialog(NULL,
        "program-name", "xfruncat",
        "version", PROJECT_VERSION,
        "comments", "A running cat using polycat font that speeds up with CPU load",
        "authors", authors,
        "logo-icon-name", "system-run",
        NULL);
}

static void
invalidate_cache(XfruncatPlugin *xf)
{
    for (gint i = 0; i < FONT_ACTIVE_N + FONT_SLEEP_N; i++)
    {
        if (xf->glyph_cache[i]) cairo_surface_destroy(xf->glyph_cache[i]);
        xf->glyph_cache[i] = NULL;
    }
    xf->cached_panel_size = 0;
}

static void
update_color(XfruncatPlugin *xf)
{
    GtkStyleContext *ctx = gtk_widget_get_style_context(xf->da);
    gtk_style_context_get_color(ctx, GTK_STATE_FLAG_NORMAL, &xf->color);
    invalidate_cache(xf);
    gtk_widget_queue_draw(xf->da);
}

static GtkWidget *
spinner_new(gint val, gint min, gint max, gint step)
{
    GtkAdjustment *adj = gtk_adjustment_new(val, min, max, step, step * 10, 0);
    GtkWidget *spin = gtk_spin_button_new(adj, step, 0);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
    return spin;
}

static void
on_configure(XfcePanelPlugin *plugin, XfruncatPlugin *xf)
{
    (void)plugin;
    XfruncatSettings s = xf->settings;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "RunCat Settings", NULL, GTK_DIALOG_MODAL,
        "Close", GTK_RESPONSE_CLOSE,
        "Apply", GTK_RESPONSE_APPLY,
        NULL);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_container_add(GTK_CONTAINER(content), box);

    GtkWidget *grid;
    gint row;

    /* Animation */
    GtkWidget *anim_frame = gtk_frame_new("Animation");
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 6);
    gtk_container_add(GTK_CONTAINER(anim_frame), grid);
    gtk_box_pack_start(GTK_BOX(box), anim_frame, FALSE, FALSE, 0);

    row = 0;
    GtkWidget *high_lab = gtk_label_new("Max rate (FPS):");
    gtk_widget_set_halign(high_lab, GTK_ALIGN_START);
    GtkWidget *high_spin = spinner_new(s.high_rate, 1, 255, 1);
    gtk_grid_attach(GTK_GRID(grid), high_lab, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), high_spin, 1, row, 1, 1);
    row++;

    GtkWidget *low_lab = gtk_label_new("Min rate (FPS):");
    gtk_widget_set_halign(low_lab, GTK_ALIGN_START);
    GtkWidget *low_spin = spinner_new(s.low_rate, 1, 255, 1);
    gtk_grid_attach(GTK_GRID(grid), low_lab, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), low_spin, 1, row, 1, 1);
    row++;

    GtkWidget *sleep_rate_lab = gtk_label_new("Sleep rate (FPS):");
    gtk_widget_set_halign(sleep_rate_lab, GTK_ALIGN_START);
    GtkWidget *sleep_rate_spin = spinner_new(s.sleeping_rate, 1, 255, 1);
    gtk_grid_attach(GTK_GRID(grid), sleep_rate_lab, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sleep_rate_spin, 1, row, 1, 1);

    /* CPU */
    GtkWidget *cpu_frame = gtk_frame_new("CPU");
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 6);
    gtk_container_add(GTK_CONTAINER(cpu_frame), grid);
    gtk_box_pack_start(GTK_BOX(box), cpu_frame, FALSE, FALSE, 0);

    row = 0;
    GtkWidget *poll_lab = gtk_label_new("Poll interval (ms):");
    gtk_widget_set_halign(poll_lab, GTK_ALIGN_START);
    GtkWidget *poll_spin = spinner_new(s.poll_period, 100, 10000, 100);
    gtk_grid_attach(GTK_GRID(grid), poll_lab, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), poll_spin, 1, row, 1, 1);
    row++;

    GtkWidget *smooth_check = gtk_check_button_new_with_label("Smoothing");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(smooth_check), s.smoothing_enabled);
    gtk_grid_attach(GTK_GRID(grid), smooth_check, 0, row, 2, 1);
    row++;

    GtkWidget *smooth_lab = gtk_label_new("Smooth time (ms):");
    gtk_widget_set_halign(smooth_lab, GTK_ALIGN_START);
    GtkWidget *smooth_spin = spinner_new(s.smoothing_value, 100, 10000, 100);
    gtk_grid_attach(GTK_GRID(grid), smooth_lab, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), smooth_spin, 1, row, 1, 1);

    /* Sleep */
    GtkWidget *sleep_frame = gtk_frame_new("Sleep");
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 6);
    gtk_container_add(GTK_CONTAINER(sleep_frame), grid);
    gtk_box_pack_start(GTK_BOX(box), sleep_frame, FALSE, FALSE, 0);

    row = 0;
    GtkWidget *sleep_enable_check = gtk_check_button_new_with_label("Enable sleeping");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sleep_enable_check), s.sleeping_enabled);
    gtk_grid_attach(GTK_GRID(grid), sleep_enable_check, 0, row, 2, 1);
    row++;

    GtkWidget *sleep_anim_check = gtk_check_button_new_with_label("Animate while sleeping");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sleep_anim_check), s.sleep_animation);
    gtk_grid_attach(GTK_GRID(grid), sleep_anim_check, 0, row, 2, 1);
    row++;

    GtkWidget *sleep_thresh_lab = gtk_label_new("Sleep threshold (%):");
    gtk_widget_set_halign(sleep_thresh_lab, GTK_ALIGN_START);
    GtkWidget *sleep_thresh_spin = spinner_new(s.sleeping_threshold, 1, 99, 1);
    gtk_grid_attach(GTK_GRID(grid), sleep_thresh_lab, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sleep_thresh_spin, 1, row, 1, 1);
    row++;

    GtkWidget *wake_thresh_lab = gtk_label_new("Wake threshold (%):");
    gtk_widget_set_halign(wake_thresh_lab, GTK_ALIGN_START);
    GtkWidget *wake_thresh_spin = spinner_new(s.wakeup_threshold, 2, 100, 1);
    gtk_grid_attach(GTK_GRID(grid), wake_thresh_lab, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), wake_thresh_spin, 1, row, 1, 1);

    gtk_widget_show_all(dialog);

    gint resp;
    do {
        resp = gtk_dialog_run(GTK_DIALOG(dialog));

        if (resp == GTK_RESPONSE_APPLY || resp == GTK_RESPONSE_CLOSE || resp == GTK_RESPONSE_DELETE_EVENT)
        {
            xf->settings.high_rate = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(high_spin));
            xf->settings.low_rate = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(low_spin));
            xf->settings.poll_period = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(poll_spin));
            xf->settings.smoothing_enabled = gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(smooth_check));
            xf->settings.smoothing_value = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(smooth_spin));
            xf->settings.sleeping_enabled = gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(sleep_enable_check));
            xf->settings.sleep_animation = gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(sleep_anim_check));
            xf->settings.sleeping_threshold = gtk_spin_button_get_value_as_int(
                GTK_SPIN_BUTTON(sleep_thresh_spin));
            xf->settings.wakeup_threshold = gtk_spin_button_get_value_as_int(
                GTK_SPIN_BUTTON(wake_thresh_spin));
            xf->settings.sleeping_rate = gtk_spin_button_get_value_as_int(
                GTK_SPIN_BUTTON(sleep_rate_spin));

            save_settings(xf);
            gtk_widget_queue_draw(xf->da);
        }
    } while (resp == GTK_RESPONSE_APPLY);

    gtk_widget_destroy(dialog);
}

static void
load_settings(XfruncatPlugin *xf)
{
    XfconfChannel *channel = xfce_panel_plugin_xfconf_channel_new(xf->plugin);
    if (!channel)
    {
        xf->settings = XFRUNCAT_SETTINGS_DEFAULT;
        return;
    }

    const XfruncatSettings *d = &XFRUNCAT_SETTINGS_DEFAULT;
    xf->settings.high_rate = xfconf_channel_get_int(channel, "/high_rate", d->high_rate);
    xf->settings.low_rate = xfconf_channel_get_int(channel, "/low_rate", d->low_rate);
    xf->settings.poll_period = xfconf_channel_get_int(channel, "/poll_period", d->poll_period);
    xf->settings.smoothing_enabled = xfconf_channel_get_bool(channel, "/smoothing_enabled", d->smoothing_enabled);
    xf->settings.smoothing_value = xfconf_channel_get_int(channel, "/smoothing_value", d->smoothing_value);
    xf->settings.sleeping_enabled = xfconf_channel_get_bool(channel, "/sleeping_enabled", d->sleeping_enabled);
    xf->settings.sleep_animation = xfconf_channel_get_bool(channel, "/sleep_animation", d->sleep_animation);
    xf->settings.sleeping_threshold = xfconf_channel_get_int(channel, "/sleeping_threshold", d->sleeping_threshold);
    xf->settings.wakeup_threshold = xfconf_channel_get_int(channel, "/wakeup_threshold", d->wakeup_threshold);
    xf->settings.sleeping_rate = xfconf_channel_get_int(channel, "/sleeping_rate", d->sleeping_rate);

    g_object_unref(channel);
}

static void
save_settings(XfruncatPlugin *xf)
{
    XfconfChannel *channel = xfce_panel_plugin_xfconf_channel_new(xf->plugin);
    if (!channel) return;

    xfconf_channel_set_int(channel, "/high_rate", xf->settings.high_rate);
    xfconf_channel_set_int(channel, "/low_rate", xf->settings.low_rate);
    xfconf_channel_set_int(channel, "/poll_period", xf->settings.poll_period);
    xfconf_channel_set_bool(channel, "/smoothing_enabled", xf->settings.smoothing_enabled);
    xfconf_channel_set_int(channel, "/smoothing_value", xf->settings.smoothing_value);
    xfconf_channel_set_bool(channel, "/sleeping_enabled", xf->settings.sleeping_enabled);
    xfconf_channel_set_bool(channel, "/sleep_animation", xf->settings.sleep_animation);
    xfconf_channel_set_int(channel, "/sleeping_threshold", xf->settings.sleeping_threshold);
    xfconf_channel_set_int(channel, "/wakeup_threshold", xf->settings.wakeup_threshold);
    xfconf_channel_set_int(channel, "/sleeping_rate", xf->settings.sleeping_rate);

    g_object_unref(channel);
}

static void
xfruncat_construct(XfcePanelPlugin *plugin)
{
    XfruncatPlugin *xf = g_new0(XfruncatPlugin, 1);
    xf->plugin = plugin;
    xf->panel_size = xfce_panel_plugin_get_size(plugin);
    xf->font_loaded = find_and_load_font();

    if (!xf->font_loaded)
        g_warning("xfruncat: polycat font not available, plugin disabled");

    load_settings(xf);

    xf->da = gtk_drawing_area_new();
    gtk_widget_set_size_request(xf->da, xf->panel_size, xf->panel_size);
    gtk_widget_set_valign(xf->da, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(xf->da, GTK_ALIGN_CENTER);
    gtk_widget_add_events(xf->da, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(xf->da, "draw", G_CALLBACK(draw_cb), xf);
    gtk_widget_show(xf->da);

    gtk_container_add(GTK_CONTAINER(plugin), xf->da);

    xf->layout = gtk_widget_create_pango_layout(xf->da, NULL);
    xf->font_desc = pango_font_description_from_string("polycat");
    pango_layout_set_font_description(xf->layout, xf->font_desc);

    {
        gchar tmp[8];
        gint len = g_unichar_to_utf8(font_active[0], tmp);
        tmp[len] = '\0';
        pango_layout_set_text(xf->layout, tmp, -1);
        pango_layout_get_pixel_size(xf->layout, &xf->font_width, &xf->font_height);
    }

    update_color(xf);
    g_signal_connect_swapped(xf->da, "style-updated", G_CALLBACK(update_color), xf);

    xfce_panel_plugin_menu_show_configure(plugin);
    xfce_panel_plugin_menu_show_about(plugin);

    g_signal_connect(plugin, "free-data", G_CALLBACK(on_free), xf);
    g_signal_connect(plugin, "size-changed", G_CALLBACK(on_size_changed), xf);
    g_signal_connect(plugin, "orientation-changed", G_CALLBACK(on_orientation_changed), xf);
    g_signal_connect(plugin, "configure-plugin", G_CALLBACK(on_configure), xf);
    g_signal_connect(plugin, "about", G_CALLBACK(show_about), xf);

    xf->last_cpu_time = g_get_monotonic_time();
    xf->tick_timer = g_timeout_add(calc_anim_interval(xf), tick_cb, xf);
}

XFCE_PANEL_PLUGIN_REGISTER(xfruncat_construct)
