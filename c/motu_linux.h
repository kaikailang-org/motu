/* Inside the Linux shim: the slice of GLib, GTK 4 and WebKitGTK 6.0 it uses,
 * as pointers filled by dlsym (objects are opaque), and what its files share.
 * Everything here is hidden, so nothing interposes on the libraries' own symbols. */
#ifndef MOTU_LINUX_H
#define MOTU_LINUX_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef size_t GType;
typedef struct { uint32_t domain; int code; char *message; } GError;
typedef struct GSList { void *data; struct GSList *next; } GSList;
typedef void (*GCallback)(void);
typedef void (*AsyncDone)(void *source, void *result, void *ud);

#define PRIORITY_DEFAULT 0
#define ORIENTATION_VERTICAL 1
#define PHASE_CAPTURE 1
#define ACTION_COPY 1
#define LOAD_FINISHED 3
#define INJECT_TOP_FRAME 1
#define INJECT_AT_START 0
#define BUS_SESSION 2
#define SOUP_HEADERS_RESPONSE 1

#define MOTU_LIBS(X) \
  X(int, gtk_init_check, (void)) \
  X(void *, g_main_loop_new, (void *, int)) \
  X(void, g_main_loop_run, (void *)) \
  X(void, g_main_loop_quit, (void *)) \
  X(unsigned, g_idle_add_full, (int, int (*)(void *), void *, void (*)(void *))) \
  X(void, g_free, (void *)) \
  X(void, g_set_application_name, (const char *)) \
  X(void, g_error_free, (GError *)) \
  X(int, g_file_get_contents, (const char *, char **, size_t *, GError **)) \
  X(char *, g_uri_unescape_string, (const char *, const char *)) \
  X(char *, g_filename_to_uri, (const char *, const char *, GError **)) \
  X(char *, g_canonicalize_filename, (const char *, const char *)) \
  X(char *, g_path_get_dirname, (const char *)) \
  X(char *, g_utf8_make_valid, (const char *, long)) \
  X(char **, g_strsplit, (const char *, const char *, int)) \
  X(void, g_strfreev, (char **)) \
  X(void, g_slist_free, (GSList *)) \
  X(void *, g_variant_new, (const char *, ...)) \
  X(void *, g_variant_new_boolean, (int)) \
  X(void *, g_variant_new_string, (const char *)) \
  X(void, g_variant_unref, (void *)) \
  X(void *, g_object_new, (GType, const char *, ...)) \
  X(void, g_object_unref, (void *)) \
  X(unsigned long, g_signal_connect_data, (void *, const char *, GCallback, void *, void *, int)) \
  X(void *, g_value_get_boxed, (const void *)) \
  X(void *, g_memory_input_stream_new_from_data, (const void *, long, void (*)(void *))) \
  X(int, g_app_info_launch_default_for_uri, (const char *, void *, GError **)) \
  X(void *, g_bus_get_sync, (int, void *, GError **)) \
  X(void, g_dbus_connection_call, (void *, const char *, const char *, const char *, const char *, \
      void *, const void *, int, int, void *, AsyncDone, void *)) \
  X(void *, g_dbus_connection_call_finish, (void *, void *, GError **)) \
  X(char *, g_file_get_path, (void *)) \
  X(unsigned, g_list_model_get_n_items, (void *)) \
  X(void *, g_list_model_get_item, (void *, unsigned)) \
  X(void *, g_list_store_new, (GType)) \
  X(void, g_list_store_append, (void *, void *)) \
  X(void *, g_menu_new, (void)) \
  X(void, g_menu_append_submenu, (void *, const char *, void *)) \
  X(void, g_menu_append_section, (void *, const char *, void *)) \
  X(void, g_menu_append_item, (void *, void *)) \
  X(void *, g_menu_item_new, (const char *, const char *)) \
  X(void, g_menu_item_set_attribute_value, (void *, const char *, void *)) \
  X(void *, g_simple_action_group_new, (void)) \
  X(void *, g_simple_action_new, (const char *, const void *)) \
  X(void *, g_simple_action_new_stateful, (const char *, const void *, void *)) \
  X(void, g_simple_action_set_enabled, (void *, int)) \
  X(void, g_simple_action_set_state, (void *, void *)) \
  X(void, g_action_map_add_action, (void *, void *)) \
  X(void, g_action_map_remove_action, (void *, const char *)) \
  X(void *, g_action_map_lookup_action, (void *, const char *)) \
  X(const void *, g_action_get_state_type, (void *)) \
  X(void *, gtk_window_new, (void)) \
  X(void, gtk_window_set_title, (void *, const char *)) \
  X(void, gtk_window_set_default_size, (void *, int, int)) \
  X(void, gtk_window_set_child, (void *, void *)) \
  X(void, gtk_window_present, (void *)) \
  X(void, gtk_window_destroy, (void *)) \
  X(void, gtk_window_fullscreen, (void *)) \
  X(void, gtk_window_unfullscreen, (void *)) \
  X(int, gtk_window_is_fullscreen, (void *)) \
  X(int, gtk_window_is_active, (void *)) \
  X(void, gtk_window_minimize, (void *)) \
  X(void, gtk_widget_set_size_request, (void *, int, int)) \
  X(void, gtk_widget_set_vexpand, (void *, int)) \
  X(void, gtk_widget_set_hexpand, (void *, int)) \
  X(void, gtk_widget_insert_action_group, (void *, const char *, void *)) \
  X(void, gtk_widget_add_controller, (void *, void *)) \
  X(void, gtk_widget_remove_controller, (void *, void *)) \
  X(void *, gtk_box_new, (int, int)) \
  X(void, gtk_box_append, (void *, void *)) \
  X(void *, gtk_popover_menu_bar_new_from_model, (void *)) \
  X(void, gtk_popover_menu_bar_set_menu_model, (void *, void *)) \
  X(void *, gtk_shortcut_controller_new, (void)) \
  X(void, gtk_shortcut_controller_add_shortcut, (void *, void *)) \
  X(void, gtk_event_controller_set_propagation_phase, (void *, int)) \
  X(void *, gtk_shortcut_new, (void *, void *)) \
  X(void *, gtk_shortcut_trigger_parse_string, (const char *)) \
  X(void *, gtk_named_action_new, (const char *)) \
  X(unsigned, gdk_unicode_to_keyval, (uint32_t)) \
  X(const char *, gdk_keyval_name, (unsigned)) \
  X(void *, gtk_drop_target_new, (GType, int)) \
  X(GType, gdk_file_list_get_type, (void)) \
  X(GSList *, gdk_file_list_get_files, (void *)) \
  X(void *, gtk_file_dialog_new, (void)) \
  X(void, gtk_file_dialog_set_title, (void *, const char *)) \
  X(void, gtk_file_dialog_set_initial_name, (void *, const char *)) \
  X(void, gtk_file_dialog_set_filters, (void *, void *)) \
  X(void, gtk_file_dialog_open, (void *, void *, void *, AsyncDone, void *)) \
  X(void *, gtk_file_dialog_open_finish, (void *, void *, GError **)) \
  X(void, gtk_file_dialog_open_multiple, (void *, void *, void *, AsyncDone, void *)) \
  X(void *, gtk_file_dialog_open_multiple_finish, (void *, void *, GError **)) \
  X(void, gtk_file_dialog_select_folder, (void *, void *, void *, AsyncDone, void *)) \
  X(void *, gtk_file_dialog_select_folder_finish, (void *, void *, GError **)) \
  X(void, gtk_file_dialog_select_multiple_folders, (void *, void *, void *, AsyncDone, void *)) \
  X(void *, gtk_file_dialog_select_multiple_folders_finish, (void *, void *, GError **)) \
  X(void, gtk_file_dialog_save, (void *, void *, void *, AsyncDone, void *)) \
  X(void *, gtk_file_dialog_save_finish, (void *, void *, GError **)) \
  X(void *, gtk_file_filter_new, (void)) \
  X(void, gtk_file_filter_add_suffix, (void *, const char *)) \
  X(void, gtk_file_filter_set_name, (void *, const char *)) \
  X(GType, gtk_file_filter_get_type, (void)) \
  X(void *, gtk_alert_dialog_new, (const char *, ...)) \
  X(void, gtk_alert_dialog_set_detail, (void *, const char *)) \
  X(void, gtk_alert_dialog_set_buttons, (void *, const char *const *)) \
  X(void, gtk_alert_dialog_set_default_button, (void *, int)) \
  X(void, gtk_alert_dialog_choose, (void *, void *, void *, AsyncDone, void *)) \
  X(int, gtk_alert_dialog_choose_finish, (void *, void *, GError **)) \
  X(void *, gdk_display_get_default, (void)) \
  X(void *, gdk_display_get_clipboard, (void *)) \
  X(void, gdk_clipboard_set_text, (void *, const char *)) \
  X(void, gdk_clipboard_read_text_async, (void *, void *, AsyncDone, void *)) \
  X(char *, gdk_clipboard_read_text_finish, (void *, void *, GError **)) \
  X(void *, gtk_print_settings_new, (void)) \
  X(void, gtk_print_settings_set, (void *, const char *, const char *)) \
  X(void, gtk_print_settings_set_printer, (void *, const char *)) \
  X(void, gtk_enumerate_printers, (int (*)(void *, void *), void *, void (*)(void *), int)) \
  X(const char *, gtk_printer_get_name, (void *)) \
  X(int, gtk_printer_is_virtual, (void *)) \
  X(int, gtk_printer_accepts_pdf, (void *)) \
  X(GType, webkit_web_view_get_type, (void)) \
  X(void *, webkit_web_context_get_default, (void)) \
  X(void, webkit_web_context_register_uri_scheme, (void *, const char *, void (*)(void *, void *), void *, void (*)(void *))) \
  X(void *, webkit_web_context_get_security_manager, (void *)) \
  X(void, webkit_security_manager_register_uri_scheme_as_secure, (void *, const char *)) \
  X(void, webkit_security_manager_register_uri_scheme_as_cors_enabled, (void *, const char *)) \
  X(const char *, webkit_uri_scheme_request_get_path, (void *)) \
  X(const char *, webkit_uri_scheme_request_get_uri, (void *)) \
  X(void, webkit_uri_scheme_request_finish_with_response, (void *, void *)) \
  X(void *, webkit_uri_scheme_response_new, (void *, int64_t)) \
  X(void, webkit_uri_scheme_response_set_status, (void *, unsigned, const char *)) \
  X(void, webkit_uri_scheme_response_set_content_type, (void *, const char *)) \
  X(void, webkit_uri_scheme_response_set_http_headers, (void *, void *)) \
  X(void *, soup_message_headers_new, (int)) \
  X(void, soup_message_headers_append, (void *, const char *, const char *)) \
  X(void *, webkit_user_content_manager_new, (void)) \
  X(int, webkit_user_content_manager_register_script_message_handler, (void *, const char *, const char *)) \
  X(void, webkit_user_content_manager_add_script, (void *, void *)) \
  X(void *, webkit_user_script_new, (const char *, int, int, const char *const *, const char *const *)) \
  X(void, webkit_user_script_unref, (void *)) \
  X(char *, jsc_value_to_string, (void *)) \
  X(void, webkit_web_view_load_uri, (void *, const char *)) \
  X(void, webkit_web_view_load_html, (void *, const char *, const char *)) \
  X(void, webkit_web_view_evaluate_javascript, (void *, const char *, long, const char *, const char *, void *, AsyncDone, void *)) \
  X(void, webkit_web_view_execute_editing_command, (void *, const char *)) \
  X(void *, webkit_web_view_get_settings, (void *)) \
  X(void, webkit_settings_set_enable_developer_extras, (void *, int)) \
  X(void *, webkit_print_operation_new, (void *)) \
  X(void, webkit_print_operation_set_print_settings, (void *, void *)) \
  X(void, webkit_print_operation_print, (void *)) \
  X(void, webkit_web_view_get_snapshot, (void *, int, int, void *, AsyncDone, void *)) \
  X(void *, webkit_web_view_get_snapshot_finish, (void *, void *, GError **)) \
  X(int, gdk_texture_save_to_png, (void *, const char *))

#define HIDDEN __attribute__((visibility("hidden")))
#define DECLARE(ret, name, params) extern HIDDEN ret (*name) params;
MOTU_LIBS(DECLARE)
#undef DECLARE

#define CONNECT(obj, sig, fn, data) g_signal_connect_data((obj), (sig), (GCallback)(fn), (data), 0, 0)

typedef struct Job Job;
struct Job {
  void (*fn)(Job *);
  Job *next;
  void *p;
  int64_t a, b, c;
  char *s[3];
};

typedef struct { char *p; size_t n, cap; } Buf;

enum { CB_MESSAGE, CB_WINDOW, CB_MENU, CB_DROP, CB_REPLY, CB_STOP };

HIDDEN Job *lx_job(void (*fn)(Job *), void *p, int64_t a, int64_t b, const char *s0, const char *s1, const char *s2);
/* Runs `j` on the GTK thread: now if already there, else queued in order. */
HIDDEN void lx_on_ui(Job *j);
/* Queues a callback for the kaikai main thread, which makes it inside motu_run. */
HIDDEN void lx_deliver(int kind, void *reply, int64_t win, const char *text);
HIDDEN void lx_buf_put(Buf *b, const char *s, size_t n);
HIDDEN void lx_buf_json_string(Buf *b, const char *s);
HIDDEN char *lx_paths_json(void *file, void *files);
HIDDEN void lx_reply_owned(void *reply, char *json);
HIDDEN void lx_clear_error(GError **err);
HIDDEN void *lx_parent_of(int64_t win);
HIDDEN void *lx_view_of(int64_t win);
HIDDEN void *lx_new_web_view(void *ucm);

#endif
