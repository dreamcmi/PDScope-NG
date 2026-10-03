#include "my_application.h"

#include <flutter_linux/flutter_linux.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include "flutter/generated_plugin_registrant.h"

struct _MyApplication {
  GtkApplication parent_instance;
  GtkWindow* window;
  GtkHeaderBar* header_bar;
  FlMethodChannel* channel;
  GPtrArray* pending_paths;
  gboolean dart_ready;
  gboolean has_document;
  gboolean filters_shown;
  gboolean detail_shown;
};

namespace {

constexpr char kShellChannel[] = "pdscope/shell";
constexpr char kDefaultTitle[] = "PDScope";

struct CommandBinding {
  const char* action_name;
  const char* command;
};

const CommandBinding kCommandBindings[] = {
    {"open-file", "openFile"},
    {"connect-device", "connectDevice"},
    {"close-current", "closeCurrent"},
    {"close-all", "closeAll"},
    {"export-csv", "exportCsv"},
    {"export-json", "exportJson"},
    {"search", "search"},
    {"toggle-dense", "toggleDense"},
    {"toggle-theme", "toggleTheme"},
    {"about", "about"},
};

GSimpleAction* LookupWindowAction(MyApplication* self,
                                  const gchar* action_name) {
  if (self->window == nullptr) return nullptr;
  return G_SIMPLE_ACTION(
      g_action_map_lookup_action(G_ACTION_MAP(self->window), action_name));
}

void InvokeDartCommand(MyApplication* self, const gchar* command) {
  if (self->channel == nullptr || command == nullptr) return;

  FlValue* args = fl_value_new_string(command);
  fl_method_channel_invoke_method(self->channel, "command", args, nullptr,
                                  nullptr, nullptr);
  fl_value_unref(args);
}

void FlushPendingPaths(MyApplication* self) {
  if (!self->dart_ready || self->channel == nullptr ||
      self->pending_paths == nullptr || self->pending_paths->len == 0) {
    return;
  }

  FlValue* paths = fl_value_new_list();
  for (guint i = 0; i < self->pending_paths->len; ++i) {
    const gchar* path = static_cast<const gchar*>(
        g_ptr_array_index(self->pending_paths, i));
    fl_value_append_take(paths, fl_value_new_string(path));
  }

  fl_method_channel_invoke_method(self->channel, "openFiles", paths, nullptr,
                                  nullptr, nullptr);
  fl_value_unref(paths);
  g_ptr_array_set_size(self->pending_paths, 0);
}

void QueueOpenPaths(MyApplication* self, GPtrArray* paths) {
  if (paths == nullptr || self->pending_paths == nullptr) return;

  for (guint i = 0; i < paths->len; ++i) {
    const gchar* path = static_cast<const gchar*>(g_ptr_array_index(paths, i));
    if (path != nullptr && *path != '\0') {
      g_ptr_array_add(self->pending_paths, g_strdup(path));
    }
  }
  FlushPendingPaths(self);
}

gboolean ReadBool(FlValue* map, const gchar* key, gboolean fallback) {
  if (map == nullptr || fl_value_get_type(map) != FL_VALUE_TYPE_MAP) {
    return fallback;
  }
  FlValue* value = fl_value_lookup_string(map, key);
  if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_BOOL) {
    return fallback;
  }
  return fl_value_get_bool(value);
}

const gchar* ReadString(FlValue* map, const gchar* key) {
  if (map == nullptr || fl_value_get_type(map) != FL_VALUE_TYPE_MAP) {
    return nullptr;
  }
  FlValue* value = fl_value_lookup_string(map, key);
  if (value == nullptr || fl_value_get_type(value) != FL_VALUE_TYPE_STRING) {
    return nullptr;
  }
  return fl_value_get_string(value);
}

void ApplyShellState(MyApplication* self, FlValue* args) {
  self->has_document = ReadBool(args, "hasDoc", self->has_document);
  self->filters_shown = ReadBool(args, "filters", self->filters_shown);
  self->detail_shown = ReadBool(args, "detail", self->detail_shown);

  const char* document_actions[] = {"close-current", "close-all", "export-csv",
                                    "export-json", "search"};
  for (const char* name : document_actions) {
    GSimpleAction* action = LookupWindowAction(self, name);
    if (action != nullptr) {
      g_simple_action_set_enabled(action, self->has_document);
    }
  }

  GSimpleAction* filters = LookupWindowAction(self, "toggle-filters");
  if (filters != nullptr) {
    GVariant* state = g_variant_new_boolean(self->filters_shown);
    g_simple_action_set_state(filters, state);
  }
  GSimpleAction* detail = LookupWindowAction(self, "toggle-detail");
  if (detail != nullptr) {
    GVariant* state = g_variant_new_boolean(self->detail_shown);
    g_simple_action_set_state(detail, state);
  }

  const gchar* requested_title = ReadString(args, "title");
  const gchar* title = requested_title != nullptr && *requested_title != '\0'
                           ? requested_title
                           : kDefaultTitle;
  if (self->window != nullptr) {
    gtk_window_set_title(self->window, title);
  }
  if (self->header_bar != nullptr) {
    gtk_header_bar_set_title(self->header_bar, title);
  }
}

void MethodCallCb(FlMethodChannel* channel, FlMethodCall* method_call,
                  gpointer user_data) {
  (void)channel;
  auto* self = MY_APPLICATION(user_data);
  const gchar* method = fl_method_call_get_name(method_call);
  g_autoptr(FlMethodResponse) response = nullptr;

  if (g_strcmp0(method, "ready") == 0) {
    self->dart_ready = TRUE;
    FlushPendingPaths(self);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else if (g_strcmp0(method, "shellState") == 0) {
    ApplyShellState(self, fl_method_call_get_args(method_call));
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else {
    response =
        FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  g_autoptr(GError) error = nullptr;
  if (!fl_method_call_respond(method_call, response, &error)) {
    g_warning("Failed to respond to pdscope/shell method: %s",
              error != nullptr ? error->message : "unknown error");
  }
}

void CommandActivatedCb(GSimpleAction* action, GVariant* parameter,
                        gpointer user_data) {
  (void)parameter;
  auto* self = MY_APPLICATION(user_data);
  const gchar* command = static_cast<const gchar*>(
      g_object_get_data(G_OBJECT(action), "pdscope-command"));
  InvokeDartCommand(self, command);
}

void ToggleActionChangeStateCb(GSimpleAction* action, GVariant* value,
                               gpointer user_data) {
  if (value == nullptr || !g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
    return;
  }

  auto* self = MY_APPLICATION(user_data);
  const gchar* action_name = g_action_get_name(G_ACTION(action));
  const gboolean enabled = g_variant_get_boolean(value);
  if (g_strcmp0(action_name, "toggle-filters") == 0) {
    self->filters_shown = enabled;
  } else if (g_strcmp0(action_name, "toggle-detail") == 0) {
    self->detail_shown = enabled;
  }
  g_simple_action_set_state(action, value);

  const gchar* command = static_cast<const gchar*>(
      g_object_get_data(G_OBJECT(action), "pdscope-command"));
  InvokeDartCommand(self, command);
}

void AddCommandAction(GtkWindow* window, MyApplication* self,
                      const CommandBinding& binding) {
  GSimpleAction* action = g_simple_action_new(binding.action_name, nullptr);
  g_object_set_data_full(G_OBJECT(action), "pdscope-command",
                         g_strdup(binding.command), g_free);
  g_signal_connect(action, "activate", G_CALLBACK(CommandActivatedCb), self);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(action));
  g_object_unref(action);
}

void AddToggleAction(GtkWindow* window, MyApplication* self,
                     const gchar* action_name, const gchar* command,
                     gboolean initial_state) {
  GVariant* state = g_variant_new_boolean(initial_state);
  GSimpleAction* action =
      g_simple_action_new_stateful(action_name, nullptr, state);
  g_object_set_data_full(G_OBJECT(action), "pdscope-command", g_strdup(command),
                         g_free);
  g_signal_connect(action, "change-state",
                   G_CALLBACK(ToggleActionChangeStateCb), self);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(action));
  g_object_unref(action);
}

void QuitActivatedCb(GSimpleAction* action, GVariant* parameter,
                     gpointer user_data) {
  (void)action;
  (void)parameter;
  g_application_quit(G_APPLICATION(user_data));
}

void InstallActions(MyApplication* self, GtkWindow* window) {
  for (const auto& binding : kCommandBindings) {
    AddCommandAction(window, self, binding);
  }
  AddToggleAction(window, self, "toggle-filters", "toggleFilters",
                  self->filters_shown);
  AddToggleAction(window, self, "toggle-detail", "toggleDetail",
                  self->detail_shown);

  GSimpleAction* quit = g_simple_action_new("quit", nullptr);
  g_signal_connect(quit, "activate", G_CALLBACK(QuitActivatedCb), self);
  g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(quit));
  g_object_unref(quit);

  ApplyShellState(self, nullptr);
}

GtkWidget* CreateMenuBar() {
  GMenu* file_menu = g_menu_new();
  g_menu_append(file_menu, "打开抓包…", "win.open-file");
  g_menu_append(file_menu, "连接设备…", "win.connect-device");
  g_menu_append(file_menu, "关闭当前标签", "win.close-current");
  g_menu_append(file_menu, "关闭全部抓包", "win.close-all");

  GMenu* file_exports = g_menu_new();
  g_menu_append(file_exports, "导出 CSV（当前筛选结果）", "win.export-csv");
  g_menu_append(file_exports, "导出 JSON（全部报文）", "win.export-json");
  g_menu_append_section(file_menu, nullptr, G_MENU_MODEL(file_exports));
  g_object_unref(file_exports);
  g_menu_append(file_menu, "退出", "app.quit");

  GMenu* view_menu = g_menu_new();
  g_menu_append(view_menu, "搜索报文", "win.search");
  g_menu_append(view_menu, "切换明暗主题", "win.toggle-theme");
  g_menu_append(view_menu, "紧凑 / 舒适行高", "win.toggle-dense");

  GMenu* view_layout = g_menu_new();
  g_menu_append(view_layout, "显示筛选栏", "win.toggle-filters");
  g_menu_append(view_layout, "显示详情面板", "win.toggle-detail");
  g_menu_append_section(view_menu, nullptr, G_MENU_MODEL(view_layout));
  g_object_unref(view_layout);

  GMenu* help_menu = g_menu_new();
  g_menu_append(help_menu, "关于 PDScope", "win.about");

  GMenu* menu_bar = g_menu_new();
  g_menu_append_submenu(menu_bar, "文件", G_MENU_MODEL(file_menu));
  g_menu_append_submenu(menu_bar, "视图", G_MENU_MODEL(view_menu));
  g_menu_append_submenu(menu_bar, "帮助", G_MENU_MODEL(help_menu));

  GtkWidget* widget = gtk_menu_bar_new_from_model(G_MENU_MODEL(menu_bar));
  g_object_unref(menu_bar);
  g_object_unref(help_menu);
  g_object_unref(view_menu);
  g_object_unref(file_menu);
  return widget;
}

void SetAccelerators(MyApplication* self) {
  const gchar* open_accels[] = {"<Primary>o", nullptr};
  const gchar* connect_accels[] = {"<Primary>d", nullptr};
  const gchar* close_accels[] = {"<Primary>w", nullptr};
  const gchar* search_accels[] = {"<Primary>f", nullptr};
  gtk_application_set_accels_for_action(GTK_APPLICATION(self), "win.open-file",
                                        open_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(self),
                                        "win.connect-device", connect_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(self),
                                        "win.close-current", close_accels);
  gtk_application_set_accels_for_action(GTK_APPLICATION(self), "win.search",
                                        search_accels);
}

void AddDropTarget(GtkWidget* widget, MyApplication* self);

void DropDataReceivedCb(GtkWidget* widget, GdkDragContext* context, gint x,
                        gint y, GtkSelectionData* selection_data, guint info,
                        guint time, gpointer user_data) {
  (void)widget;
  (void)x;
  (void)y;
  (void)info;
  auto* self = MY_APPLICATION(user_data);
  GPtrArray* paths = g_ptr_array_new_with_free_func(g_free);
  gchar** uris = gtk_selection_data_get_uris(selection_data);

  if (uris != nullptr) {
    for (guint i = 0; uris[i] != nullptr; ++i) {
      GFile* file = g_file_new_for_uri(uris[i]);
      gchar* path = g_file_get_path(file);
      g_object_unref(file);
      if (path == nullptr) continue;

      gchar* absolute_path = g_canonicalize_filename(path, nullptr);
      g_free(path);
      g_ptr_array_add(paths, absolute_path);
    }
  }

  gtk_drag_finish(context, paths->len > 0, FALSE, time);
  QueueOpenPaths(self, paths);
  g_strfreev(uris);
  g_ptr_array_unref(paths);
}

void AddDropTarget(GtkWidget* widget, MyApplication* self) {
  (void)self;
  GtkTargetEntry targets[] = {{const_cast<gchar*>("text/uri-list"), 0, 0}};
  gtk_drag_dest_set(widget, GTK_DEST_DEFAULT_ALL, targets, G_N_ELEMENTS(targets),
                    GDK_ACTION_COPY);
  g_signal_connect(widget, "drag-data-received",
                   G_CALLBACK(DropDataReceivedCb), self);
}

void WindowDestroyedCb(GtkWidget* widget, gpointer user_data) {
  auto* self = MY_APPLICATION(user_data);
  if (self->window != GTK_WINDOW(widget)) return;

  self->window = nullptr;
  self->header_bar = nullptr;
  self->dart_ready = FALSE;
  if (self->channel != nullptr) {
    fl_method_channel_set_method_call_handler(self->channel, nullptr, nullptr,
                                              nullptr);
    g_clear_object(&self->channel);
  }
}

void FirstFrameCb(MyApplication* self, FlView* view) {
  (void)view;
  if (self->window != nullptr) {
    gtk_widget_show(GTK_WIDGET(self->window));
  }
}

GtkWindow* EnsureWindow(MyApplication* self) {
  if (self->window != nullptr) {
    gtk_window_present(self->window);
    return self->window;
  }

  GtkWindow* window = GTK_WINDOW(
      gtk_application_window_new(GTK_APPLICATION(self)));
  self->window = window;
  gtk_window_set_title(window, kDefaultTitle);
  gtk_window_set_default_size(window, 1280, 800);
  g_signal_connect(window, "destroy", G_CALLBACK(WindowDestroyedCb), self);

  GtkHeaderBar* header_bar = GTK_HEADER_BAR(gtk_header_bar_new());
  self->header_bar = header_bar;
  gtk_header_bar_set_title(header_bar, kDefaultTitle);
  gtk_header_bar_set_show_close_button(header_bar, TRUE);
  gtk_window_set_titlebar(window, GTK_WIDGET(header_bar));

#ifdef GDK_WINDOWING_X11
  GdkScreen* screen = gtk_window_get_screen(window);
  if (GDK_IS_X11_SCREEN(screen)) {
    const gchar* wm_name = gdk_x11_screen_get_window_manager_name(screen);
    if (g_strcmp0(wm_name, "GNOME Shell") != 0) {
      // Keep the regular window-manager title bar on non-GNOME X11 desktops.
      gtk_window_set_titlebar(window, nullptr);
      self->header_bar = nullptr;
    }
  }
#endif

  InstallActions(self, window);
  SetAccelerators(self);

  GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget* menu_bar = CreateMenuBar();
  gtk_box_pack_start(GTK_BOX(content), menu_bar, FALSE, FALSE, 0);

  FlDartProject* project = fl_dart_project_new();
  FlView* view = fl_view_new(project);
  g_object_unref(project);
  gtk_widget_set_hexpand(GTK_WIDGET(view), TRUE);
  gtk_widget_set_vexpand(GTK_WIDGET(view), TRUE);
  gtk_box_pack_start(GTK_BOX(content), GTK_WIDGET(view), TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(window), content);

  FlEngine* engine = fl_view_get_engine(view);
  FlStandardMethodCodec* codec = fl_standard_method_codec_new();
  self->channel = fl_method_channel_new(
      fl_engine_get_binary_messenger(engine), kShellChannel,
      FL_METHOD_CODEC(codec));
  g_object_unref(codec);
  fl_method_channel_set_method_call_handler(self->channel, MethodCallCb, self,
                                            nullptr);

  fl_register_plugins(FL_PLUGIN_REGISTRY(view));
  g_signal_connect_swapped(view, "first-frame", G_CALLBACK(FirstFrameCb), self);
  AddDropTarget(GTK_WIDGET(view), self);
  AddDropTarget(GTK_WIDGET(window), self);

  gtk_widget_show_all(content);
  gtk_widget_realize(GTK_WIDGET(view));
  return window;
}

void AddFilesFromGFile(GPtrArray* paths, GFile* file) {
  if (file == nullptr) return;

  gchar* path = g_file_get_path(file);
  if (path == nullptr) return;
  gchar* absolute_path = g_canonicalize_filename(path, nullptr);
  g_free(path);
  g_ptr_array_add(paths, absolute_path);
}

void ApplicationActivate(GApplication* application) {
  auto* self = MY_APPLICATION(application);
  GtkWindow* window = EnsureWindow(self);
  gtk_window_present(window);
}

void ApplicationOpen(GApplication* application, GFile** files, gint n_files,
                     const gchar* hint) {
  (void)hint;
  auto* self = MY_APPLICATION(application);
  GtkWindow* window = EnsureWindow(self);
  gtk_window_present(window);

  GPtrArray* paths = g_ptr_array_new_with_free_func(g_free);
  for (gint i = 0; i < n_files; ++i) {
    AddFilesFromGFile(paths, files[i]);
  }
  QueueOpenPaths(self, paths);
  g_ptr_array_unref(paths);
}

}  // namespace

G_DEFINE_TYPE(MyApplication, my_application, GTK_TYPE_APPLICATION)

static void my_application_dispose(GObject* object) {
  MyApplication* self = MY_APPLICATION(object);
  if (self->channel != nullptr) {
    fl_method_channel_set_method_call_handler(self->channel, nullptr, nullptr,
                                              nullptr);
    g_clear_object(&self->channel);
  }
  g_clear_pointer(&self->pending_paths, g_ptr_array_unref);
  G_OBJECT_CLASS(my_application_parent_class)->dispose(object);
}

static void my_application_class_init(MyApplicationClass* klass) {
  G_APPLICATION_CLASS(klass)->activate = ApplicationActivate;
  G_APPLICATION_CLASS(klass)->open = ApplicationOpen;
  G_OBJECT_CLASS(klass)->dispose = my_application_dispose;
}

static void my_application_init(MyApplication* self) {
  self->pending_paths = g_ptr_array_new_with_free_func(g_free);
  self->has_document = FALSE;
  self->filters_shown = TRUE;
  self->detail_shown = TRUE;
}

MyApplication* my_application_new() {
  g_set_prgname(APPLICATION_ID);
  g_set_application_name(kDefaultTitle);
  return MY_APPLICATION(g_object_new(
      my_application_get_type(), "application-id", APPLICATION_ID, "flags",
      G_APPLICATION_HANDLES_OPEN, nullptr));
}
