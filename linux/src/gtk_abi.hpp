#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

// --- GLib & GObject Minimal ABI Declarations ---
using gboolean = int;
using gint = int;
using guint = unsigned int;
using gdouble = double;
using gchar = char;
using guchar = unsigned char;
using gpointer = void*;
using GQuark = uint32_t;
using GType = size_t;
using GCallback = void (*)();
using GClosureNotify = void (*)(gpointer data, void* closure);
using GSourceFunc = gboolean (*)(gpointer user_data);

constexpr gboolean TRUE_VAL = 1;
constexpr gboolean FALSE_VAL = 0;

struct GObject;
struct GtkWidget;
struct GtkWindow;
struct GtkBox;
struct GtkLabel;
struct GtkButton;
struct GtkProgressBar;
struct GtkTextView;
struct GtkTextBuffer;
struct GtkTextIter {
    gpointer dummy1;
    gpointer dummy2;
    gint dummy3;
    gint dummy4;
    gint dummy5;
    gint dummy6;
    gint dummy7;
    gint dummy8;
    gpointer dummy9;
    gpointer dummy10;
    gint dummy11;
    gint dummy12;
    gint dummy13;
    gpointer dummy14;
};
struct GtkMenu;
struct GtkMenuShell;
struct GtkMenuItem;
struct GtkCheckMenuItem;
struct GtkFileChooser;
struct GtkClipboard;
struct GtkSelectionData;
struct GdkDragContext;
struct GdkEvent;
struct GdkAtom_;
using GdkAtom = GdkAtom_*;

struct GtkTargetEntry {
    const gchar* target;
    guint flags;
    guint info;
};

enum GtkWindowType {
    GTK_WINDOW_TOPLEVEL = 0,
    GTK_WINDOW_POPUP = 1
};

enum GtkOrientation {
    GTK_ORIENTATION_HORIZONTAL = 0,
    GTK_ORIENTATION_VERTICAL = 1
};

enum GtkDestDefaults {
    GTK_DEST_DEFAULT_MOTION = 1 << 0,
    GTK_DEST_DEFAULT_HIGHLIGHT = 1 << 1,
    GTK_DEST_DEFAULT_DROP = 1 << 2,
    GTK_DEST_DEFAULT_ALL = 0x07
};

enum GdkDragAction {
    GDK_ACTION_DEFAULT = 1 << 0,
    GDK_ACTION_COPY = 1 << 1,
    GDK_ACTION_MOVE = 1 << 2,
    GDK_ACTION_LINK = 1 << 3
};

enum GtkFileChooserAction {
    GTK_FILE_CHOOSER_ACTION_OPEN = 0,
    GTK_FILE_CHOOSER_ACTION_SAVE = 1,
    GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER = 2,
    GTK_FILE_CHOOSER_ACTION_CREATE_FOLDER = 3
};

enum GtkResponseType {
    GTK_RESPONSE_ACCEPT = -3,
    GTK_RESPONSE_CANCEL = -6
};

struct GSList {
    gpointer data;
    GSList* next;
};

// GLib / GObject functions
guint g_idle_add(GSourceFunc function, gpointer data);
guint g_timeout_add(guint interval, GSourceFunc function, gpointer data);
void g_free(gpointer mem);
void g_strfreev(gchar** str_array);
void g_slist_free_full(GSList* list, void (*free_func)(gpointer));
unsigned long g_signal_connect_data(
    gpointer instance,
    const gchar* detailed_signal,
    GCallback c_handler,
    gpointer data,
    GClosureNotify destroy_data,
    int connect_flags);

// GTK3 core functions
gboolean gtk_init_check(int* argc, char*** argv);
void gtk_main();
void gtk_main_quit();

GtkWidget* gtk_window_new(GtkWindowType type);
void gtk_window_set_title(GtkWindow* window, const gchar* title);
void gtk_window_set_default_size(GtkWindow* window, gint width, gint height);
void gtk_window_present(GtkWindow* window);
struct GList {
    gpointer data;
    GList* next;
    GList* prev;
};

void gtk_window_set_modal(GtkWindow* window, gboolean modal);
void gtk_window_set_transient_for(GtkWindow* window, GtkWindow* parent);
void gtk_container_set_border_width(gpointer container, guint border_width);
void gtk_container_add(gpointer container, GtkWidget* widget);
void gtk_container_remove(gpointer container, GtkWidget* widget);
GList* gtk_container_get_children(gpointer container);
void g_list_free(GList* list);

GtkWidget* gtk_box_new(GtkOrientation orientation, gint spacing);
void gtk_box_pack_start(GtkBox* box, GtkWidget* child, gboolean expand, gboolean fill, guint padding);

GtkWidget* gtk_label_new(const gchar* str);
void gtk_label_set_text(GtkLabel* label, const gchar* str);
void gtk_label_set_markup(GtkLabel* label, const gchar* str);
void gtk_label_set_xalign(GtkLabel* label, float xalign);
void gtk_label_set_line_wrap(GtkLabel* label, gboolean wrap);

GtkWidget* gtk_button_new_with_label(const gchar* label);
GtkWidget* gtk_progress_bar_new();
void gtk_progress_bar_set_fraction(GtkProgressBar* pbar, gdouble fraction);
void gtk_progress_bar_set_text(GtkProgressBar* pbar, const gchar* text);
void gtk_progress_bar_set_show_text(GtkProgressBar* pbar, gboolean show_text);

GtkWidget* gtk_scrolled_window_new(gpointer hadjustment, gpointer vadjustment);
void gtk_widget_set_size_request(GtkWidget* widget, gint width, gint height);
GtkWidget* gtk_text_view_new();
void gtk_text_view_set_editable(GtkTextView* text_view, gboolean setting);
void gtk_text_view_set_wrap_mode(GtkTextView* text_view, int wrap_mode);
GtkTextBuffer* gtk_text_view_get_buffer(GtkTextView* text_view);
void gtk_text_buffer_get_end_iter(GtkTextBuffer* buffer, GtkTextIter* iter);
void gtk_text_buffer_insert(GtkTextBuffer* buffer, GtkTextIter* iter, const gchar* text, gint len);

GtkWidget* gtk_frame_new(const gchar* label);
GtkWidget* gtk_separator_new(GtkOrientation orientation);

void gtk_widget_show(GtkWidget* widget);
void gtk_widget_show_all(GtkWidget* widget);
void gtk_widget_hide(GtkWidget* widget);
gboolean gtk_widget_get_visible(GtkWidget* widget);
void gtk_widget_destroy(GtkWidget* widget);
gboolean gtk_widget_hide_on_delete(GtkWidget* widget);

// Drag & Drop
void gtk_drag_dest_set(
    GtkWidget* widget,
    GtkDestDefaults flags,
    const GtkTargetEntry* targets,
    gint n_targets,
    GdkDragAction actions);
void gtk_drag_dest_add_uri_targets(GtkWidget* widget);
gchar** gtk_selection_data_get_uris(const GtkSelectionData* selection_data);
void gtk_drag_finish(GdkDragContext* context, gboolean success, gboolean del, uint32_t time);
gchar* g_filename_from_uri(const gchar* uri, gchar** hostname, void** error);

// File Chooser Dialog
GtkWidget* gtk_file_chooser_dialog_new(
    const gchar* title,
    GtkWindow* parent,
    GtkFileChooserAction action,
    const gchar* first_button_text,
    ...);
void gtk_file_chooser_set_select_multiple(GtkFileChooser* chooser, gboolean select_multiple);
gint gtk_dialog_run(gpointer dialog);
GSList* gtk_file_chooser_get_filenames(GtkFileChooser* chooser);

// Menu & Tray
GtkWidget* gtk_menu_new();
GtkWidget* gtk_menu_item_new_with_label(const gchar* label);
GtkWidget* gtk_separator_menu_item_new();
GtkWidget* gtk_check_menu_item_new_with_label(const gchar* label);
void gtk_check_menu_item_set_active(GtkCheckMenuItem* check_menu_item, gboolean is_active);
gboolean gtk_check_menu_item_get_active(GtkCheckMenuItem* check_menu_item);
void gtk_menu_item_set_label(GtkMenuItem* menu_item, const gchar* label);
void gtk_menu_shell_append(GtkMenuShell* menu_shell, GtkWidget* child);

// Clipboard (Wayland & X11 GdkClipboard wrapper)
GdkAtom gdk_atom_intern_static_string(const gchar* atom_name);
GtkClipboard* gtk_clipboard_get(GdkAtom selection);
using GtkClipboardTextReceivedFunc = void (*)(GtkClipboard* clipboard, const gchar* text, gpointer data);
void gtk_clipboard_request_text(GtkClipboard* clipboard, GtkClipboardTextReceivedFunc callback, gpointer user_data);
void gtk_clipboard_set_text(GtkClipboard* clipboard, const gchar* text, gint len);

// Image clipboard & GdkPixbuf
struct GdkPixbuf;
using gsize = size_t;
gchar* gtk_clipboard_wait_for_text(GtkClipboard* clipboard);
GdkPixbuf* gtk_clipboard_wait_for_image(GtkClipboard* clipboard);
gboolean gtk_clipboard_wait_is_image_available(GtkClipboard* clipboard);
void gtk_clipboard_set_image(GtkClipboard* clipboard, GdkPixbuf* pixbuf);
GdkPixbuf* gdk_pixbuf_new_from_file(const char* filename, void** error);
gboolean gdk_pixbuf_save_to_buffer(GdkPixbuf* pixbuf, gchar** buffer, gsize* buffer_size, const char* type, void** error, ...);
void g_object_unref(gpointer object);

// Switch & Spin Button & Dialog
GtkWidget* gtk_switch_new();
void gtk_switch_set_active(gpointer sw, gboolean is_active);
gboolean gtk_switch_get_active(gpointer sw);

GtkWidget* gtk_spin_button_new_with_range(gdouble min, gdouble max, gdouble step);
void gtk_spin_button_set_value(gpointer spin, gdouble value);
gdouble gtk_spin_button_get_value(gpointer spin);

GtkWidget* gtk_dialog_new_with_buttons(
    const gchar* title,
    GtkWindow* parent,
    int flags,
    const gchar* first_button_text,
    ...);
GtkWidget* gtk_dialog_get_content_area(gpointer dialog);
GtkWidget* gtk_list_box_new();

// GTK3 CSS Provider, StyleContext, RGBA Visual & Symbolic Icons (Liquid Glass Theme)
struct GtkCssProvider;
struct GtkStyleContext;
struct GdkScreen;
struct GdkVisual;

constexpr guint GTK_STYLE_PROVIDER_PRIORITY_APPLICATION = 600;
constexpr guint GTK_STYLE_PROVIDER_PRIORITY_USER = 800;

GtkCssProvider* gtk_css_provider_new();
gboolean gtk_css_provider_load_from_data(
    GtkCssProvider* css_provider,
    const gchar* data,
    long length,
    void** error);
GdkScreen* gdk_screen_get_default();
GdkScreen* gtk_widget_get_screen(GtkWidget* widget);
GdkVisual* gdk_screen_get_rgba_visual(GdkScreen* screen);
void gtk_widget_set_visual(GtkWidget* widget, GdkVisual* visual);
void gtk_widget_set_app_paintable(GtkWidget* widget, gboolean app_paintable);
void gtk_style_context_add_provider_for_screen(
    GdkScreen* screen,
    gpointer provider,
    guint priority);
GtkStyleContext* gtk_widget_get_style_context(GtkWidget* widget);
void gtk_style_context_add_class(GtkStyleContext* context, const gchar* class_name);
void gtk_style_context_remove_class(GtkStyleContext* context, const gchar* class_name);

GtkWidget* gtk_image_new_from_icon_name(const gchar* icon_name, int size);
GtkWidget* gtk_image_new_from_pixbuf(GdkPixbuf* pixbuf);
GdkPixbuf* gdk_pixbuf_scale_simple(
    const GdkPixbuf* src,
    int dest_width,
    int dest_height,
    int interp_type);
void gtk_button_set_image(GtkButton* button, GtkWidget* image);
void gtk_button_set_always_show_image(GtkButton* button, gboolean always_show);
void gtk_frame_set_shadow_type(gpointer frame, int type);

// Ayatana AppIndicator3
struct AppIndicator;
enum AppIndicatorCategory {
    APP_INDICATOR_CATEGORY_APPLICATION_STATUS = 0,
    APP_INDICATOR_CATEGORY_COMMUNICATIONS = 1
};
enum AppIndicatorStatus {
    APP_INDICATOR_STATUS_PASSIVE = 0,
    APP_INDICATOR_STATUS_ACTIVE = 1,
    APP_INDICATOR_STATUS_ATTENTION = 2
};

AppIndicator* app_indicator_new(
    const gchar* id,
    const gchar* icon_name,
    AppIndicatorCategory category);
void app_indicator_set_status(AppIndicator* self, AppIndicatorStatus status);
void app_indicator_set_icon_full(AppIndicator* self, const gchar* icon_name, const gchar* icon_desc);
void app_indicator_set_title(AppIndicator* self, const gchar* title);
void app_indicator_set_menu(AppIndicator* self, GtkMenu* menu);

}  // extern "C"

#define G_CALLBACK_CAST(f) reinterpret_cast<GCallback>(f)
#define g_signal_connect(instance, detailed_signal, c_handler, data) \
    g_signal_connect_data((instance), (detailed_signal), G_CALLBACK_CAST(c_handler), (data), nullptr, 0)
