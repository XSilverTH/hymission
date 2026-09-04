#include <gtk/gtk.h>
#include <gtk4-layer-shell.h>
#include <gio/gdesktopappinfo.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace {
constexpr int IPC_FD = 3;
constexpr std::size_t MAX_APP_RESULTS = 12;
constexpr std::size_t MAX_PACKET_SIZE = 60000;

struct ApplicationEntry {
    std::string desktopId;
    std::string name;
    std::string iconPath;
    std::string searchable;
};

struct AppState {
    GtkWindow*     window = nullptr;
    GtkLabel*      queryLabel = nullptr;
    GtkLabel*      countLabel = nullptr;
    GtkIMContext*  im = nullptr;
    std::string    query;
    std::string    preedit;
    std::size_t    cursor = 0;
    bool           revealed = false;
    std::vector<ApplicationEntry> applications;
};

bool sendPacket(char type, const std::string& payload = {}) {
    std::string packet(1, type);
    packet += payload;
    return packet.size() <= MAX_PACKET_SIZE &&
        send(IPC_FD, packet.data(), packet.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(packet.size());
}

std::string normalizeText(const char* value) {
    if (!value || !*value)
        return {};
    gchar* normalized = g_utf8_normalize(value, -1, G_NORMALIZE_ALL);
    if (!normalized)
        return {};
    gchar* folded = g_utf8_casefold(normalized, -1);
    g_free(normalized);
    if (!folded)
        return {};
    std::string result(folded);
    g_free(folded);
    return result;
}

std::string sanitizeField(std::string value) {
    std::replace(value.begin(), value.end(), '\t', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    return value;
}

std::string iconPathFor(GDesktopAppInfo* info) {
    if (!info)
        return {};

    GIcon* icon = g_app_info_get_icon(G_APP_INFO(info));
    if (!icon)
        return {};

    std::vector<std::string> names;
    if (G_IS_FILE_ICON(icon)) {
        if (GFile* file = g_file_icon_get_file(G_FILE_ICON(icon))) {
            if (gchar* path = g_file_get_path(file)) {
                std::string result(path);
                g_free(path);
                return result;
            }
        }
    } else if (G_IS_THEMED_ICON(icon)) {
        for (const char* const* name = g_themed_icon_get_names(G_THEMED_ICON(icon)); name && *name; ++name)
            names.emplace_back(*name);
    }

    std::vector<std::filesystem::path> roots;
    if (const char* userData = g_get_user_data_dir(); userData && *userData)
        roots.emplace_back(userData);
    for (const char* const* systemData = g_get_system_data_dirs(); systemData && *systemData; ++systemData)
        roots.emplace_back(*systemData);
    roots.emplace_back("/usr/share");

    const std::array<std::string_view, 8> sizes = {"scalable", "512x512", "256x256", "128x128", "96x96", "64x64", "48x48", "32x32"};
    const std::array<std::string_view, 3> extensions = {".png", ".svg", ".xpm"};
    std::error_code error;
    for (const auto& root : roots) {
        for (const auto& name : names) {
            for (const auto& size : sizes) {
                const auto directory = root / "icons" / "hicolor" / size / "apps";
                for (const auto& extension : extensions) {
                    const auto candidate = directory / (name + std::string(extension));
                    if (std::filesystem::is_regular_file(candidate, error))
                        return candidate.string();
                }
            }
            for (const auto& extension : extensions) {
                const auto candidate = root / "pixmaps" / (name + std::string(extension));
                if (std::filesystem::is_regular_file(candidate, error))
                    return candidate.string();
            }
        }
    }
    return {};
}

bool visibleInCurrentDesktop(GDesktopAppInfo* info) {
    const char* desktops = g_getenv("XDG_CURRENT_DESKTOP");
    if (!desktops || !*desktops)
        return true;

    std::string value(desktops);
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find(':', start);
        const std::string desktop = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!desktop.empty() && g_desktop_app_info_get_show_in(info, desktop.c_str()))
            return true;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return false;
}

void buildApplicationIndex(AppState* state) {
    std::unordered_map<std::string, ApplicationEntry> byId;
    GList* all = g_app_info_get_all();
    for (GList* item = all; item; item = item->next) {
        if (!item->data || !G_IS_DESKTOP_APP_INFO(item->data))
            continue;

        auto* info = G_DESKTOP_APP_INFO(item->data);
        if (g_desktop_app_info_get_is_hidden(info) || g_desktop_app_info_get_nodisplay(info) || !visibleInCurrentDesktop(info))
            continue;

        const char* id = g_app_info_get_id(G_APP_INFO(info));
        const char* name = g_app_info_get_name(G_APP_INFO(info));
        if (!id || !*id || !name || !*name)
            continue;

        ApplicationEntry entry;
        entry.desktopId = sanitizeField(id);
        entry.name = sanitizeField(name);
        entry.iconPath = sanitizeField(iconPathFor(info));
        entry.searchable = normalizeText(name);

        if (const char* generic = g_desktop_app_info_get_generic_name(info); generic && *generic)
            entry.searchable += " " + normalizeText(generic);
        if (const char* const* keywords = g_desktop_app_info_get_keywords(info)) {
            for (const char* const* keyword = keywords; *keyword; ++keyword)
                entry.searchable += " " + normalizeText(*keyword);
        }
        entry.searchable += " " + normalizeText(id);

        const auto existing = byId.find(entry.desktopId);
        if (existing == byId.end() || entry.name < existing->second.name)
            byId[entry.desktopId] = std::move(entry);
    }
    g_list_free_full(all, g_object_unref);

    state->applications.reserve(byId.size());
    for (auto& [id, entry] : byId)
        state->applications.push_back(std::move(entry));
    std::sort(state->applications.begin(), state->applications.end(), [](const ApplicationEntry& lhs, const ApplicationEntry& rhs) {
        const std::string lhsKey = normalizeText(lhs.name.c_str());
        const std::string rhsKey = normalizeText(rhs.name.c_str());
        if (lhsKey != rhsKey)
            return lhsKey < rhsKey;
        return lhs.desktopId < rhs.desktopId;
    });
}

void sendApplicationResults(const AppState* state) {
    std::string packet;
    packet.reserve(4096);
    packet.push_back('D');
    const std::string query = normalizeText(state->query.c_str());
    if (!query.empty()) {
        std::size_t emitted = 0;
        for (const auto& app : state->applications) {
            if (!app.searchable.contains(query))
                continue;
            const std::string record = app.desktopId + "\t" + app.name + "\t" + app.iconPath + "\n";
            if (packet.size() + record.size() > MAX_PACKET_SIZE)
                break;
            packet += record;
            if (++emitted >= MAX_APP_RESULTS)
                break;
        }
    }
    send(IPC_FD, packet.data(), packet.size(), MSG_NOSIGNAL);
}

void launchApplication(AppState* state, std::string_view desktopId) {
    const auto it = std::find_if(state->applications.begin(), state->applications.end(),
                                 [&](const ApplicationEntry& app) { return app.desktopId == desktopId; });
    if (it == state->applications.end()) {
        sendPacket('X', "application is no longer available");
        return;
    }

    const gchar* argv[] = {"uwsm", "app", "--", it->desktopId.c_str(), nullptr};
    GError* error = nullptr;
    GSubprocess* process = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_NONE, &error);
    if (!process) {
        const std::string message = error && error->message ? error->message : "unable to start uwsm";
        sendPacket('X', message);
        if (error)
            g_error_free(error);
        return;
    }
    g_object_unref(process);
}

void reveal(AppState* state) {
    if (state->revealed)
        return;
    state->revealed = true;
    gtk_widget_set_opacity(GTK_WIDGET(state->window), 1.0);
}

void updateLabel(AppState* state) {
    std::string shown = state->query;
    if (!state->preedit.empty())
        shown.insert(state->cursor, state->preedit);
    gtk_label_set_text(state->queryLabel, shown.empty() ? "Search windows and applications" : shown.c_str());
}

void publishQuery(AppState* state) {
    reveal(state);
    updateLabel(state);
    if (!sendPacket('Q', state->query)) {
        g_application_quit(g_application_get_default());
        return;
    }
    sendApplicationResults(state);
}

void commitText(GtkIMContext*, const char* text, gpointer data) {
    auto* state = static_cast<AppState*>(data);
    state->query.insert(state->cursor, text);
    state->cursor += std::strlen(text);
    state->preedit.clear();
    publishQuery(state);
}

void preeditChanged(GtkIMContext* context, gpointer data) {
    auto* state = static_cast<AppState*>(data);
    gchar* text = nullptr;
    gint cursor = 0;
    gtk_im_context_get_preedit_string(context, &text, nullptr, &cursor);
    state->preedit = text ? text : "";
    g_free(text);
    if (!state->preedit.empty())
        reveal(state);
    updateLabel(state);
    sendPacket('P', state->preedit.empty() ? "0" : "1");
}

void erasePreviousCodepoint(AppState* state) {
    if (state->cursor == 0)
        return;
    const char* begin = state->query.data();
    const char* previous = g_utf8_find_prev_char(begin, begin + state->cursor);
    if (!previous)
        return;
    const auto offset = static_cast<std::size_t>(previous - begin);
    state->query.erase(offset, state->cursor - offset);
    state->cursor = offset;
    publishQuery(state);
}

gboolean keyPressed(GtkEventControllerKey* controller, guint keyval, guint, GdkModifierType modifiers, gpointer data) {
    auto* state = static_cast<AppState*>(data);
    if (GdkEvent* event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller)); event && gtk_im_context_filter_keypress(state->im, event))
        return TRUE;

    if (modifiers & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK | GDK_META_MASK))
        return FALSE;

    switch (keyval) {
        case GDK_KEY_BackSpace:
            erasePreviousCodepoint(state);
            return TRUE;
        case GDK_KEY_Delete:
            if (state->cursor < state->query.size()) {
                const char* begin = state->query.data();
                const char* next = g_utf8_next_char(begin + state->cursor);
                state->query.erase(state->cursor, static_cast<std::size_t>(next - (begin + state->cursor)));
                publishQuery(state);
            }
            return TRUE;
        case GDK_KEY_Left:
            if (state->cursor > 0) {
                const char* previous = g_utf8_find_prev_char(state->query.data(), state->query.data() + state->cursor);
                if (previous)
                    state->cursor = static_cast<std::size_t>(previous - state->query.data());
            }
            return TRUE;
        case GDK_KEY_Right:
            if (state->cursor < state->query.size())
                state->cursor = static_cast<std::size_t>(g_utf8_next_char(state->query.data() + state->cursor) - state->query.data());
            return TRUE;
        case GDK_KEY_Up:
            return sendPacket('N', "-1") ? TRUE : FALSE;
        case GDK_KEY_Down:
            return sendPacket('N', "1") ? TRUE : FALSE;
        case GDK_KEY_Return:
        case GDK_KEY_KP_Enter:
            return sendPacket('A') ? TRUE : FALSE;
        case GDK_KEY_Escape:
            return sendPacket('E') ? TRUE : FALSE;
        default:
            break;
    }

    const gunichar ch = gdk_keyval_to_unicode(keyval);
    if (ch != 0 && !g_unichar_iscntrl(ch)) {
        char utf8[7] = {};
        const int length = g_unichar_to_utf8(ch, utf8);
        state->query.insert(state->cursor, utf8, static_cast<std::size_t>(length));
        state->cursor += static_cast<std::size_t>(length);
        publishQuery(state);
        return TRUE;
    }
    return FALSE;
}

gboolean ipcReady(GIOChannel* channel, GIOCondition condition, gpointer data) {
    auto* state = static_cast<AppState*>(data);
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        g_application_quit(g_application_get_default());
        return G_SOURCE_REMOVE;
    }
    std::array<char, 256> packet{};
    const ssize_t size = recv(g_io_channel_unix_get_fd(channel), packet.data(), packet.size() - 1, 0);
    if (size <= 0) {
        g_application_quit(g_application_get_default());
        return G_SOURCE_REMOVE;
    }
    if (packet[0] == 'C')
        gtk_label_set_text(state->countLabel, (std::string(packet.data() + 1, static_cast<std::size_t>(size - 1)) + " results").c_str());
    else if (packet[0] == 'L')
        launchApplication(state, std::string_view(packet.data() + 1, static_cast<std::size_t>(size - 1)));
    return G_SOURCE_CONTINUE;
}

void activate(GtkApplication* app, gpointer data) {
    auto* state = static_cast<AppState*>(data);
    buildApplicationIndex(state);
    state->window = GTK_WINDOW(gtk_application_window_new(app));
    gtk_window_set_decorated(state->window, FALSE);
    gtk_layer_init_for_window(state->window);
    gtk_layer_set_layer(state->window, GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_namespace(state->window, "hymission-search");
    gtk_layer_set_anchor(state->window, GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_margin(state->window, GTK_LAYER_SHELL_EDGE_TOP, 28);
    gtk_layer_set_keyboard_mode(state->window, GTK_LAYER_SHELL_KEYBOARD_MODE_EXCLUSIVE);

    auto* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(box, "searchbar");
    state->queryLabel = GTK_LABEL(gtk_label_new("Search windows and applications"));
    gtk_label_set_xalign(state->queryLabel, 0.0F);
    gtk_widget_set_size_request(GTK_WIDGET(state->queryLabel), 360, -1);
    state->countLabel = GTK_LABEL(gtk_label_new(""));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(state->queryLabel));
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(state->countLabel));
    gtk_window_set_child(state->window, box);

    auto* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider,
        ".searchbar { background: rgba(30,30,34,0.96); color: white; border-radius: 8px; padding: 12px 16px; box-shadow: 0 8px 24px rgba(0,0,0,0.35); }"
        ".searchbar label:last-child { color: rgba(255,255,255,0.62); }");
    gtk_style_context_add_provider_for_display(gtk_widget_get_display(GTK_WIDGET(state->window)), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    state->im = gtk_im_multicontext_new();
    gtk_im_context_set_client_widget(state->im, GTK_WIDGET(state->window));
    g_signal_connect(state->im, "commit", G_CALLBACK(commitText), state);
    g_signal_connect(state->im, "preedit-changed", G_CALLBACK(preeditChanged), state);
    gtk_im_context_focus_in(state->im);

    auto* keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(keyPressed), state);
    gtk_widget_add_controller(GTK_WIDGET(state->window), keys);
    gtk_widget_set_focusable(GTK_WIDGET(state->window), TRUE);
    gtk_widget_set_opacity(GTK_WIDGET(state->window), 0.0);
    gtk_window_present(state->window);
    gtk_widget_grab_focus(GTK_WIDGET(state->window));

    GIOChannel* channel = g_io_channel_unix_new(IPC_FD);
    g_io_channel_set_encoding(channel, nullptr, nullptr);
    g_io_add_watch(channel, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL), ipcReady, state);
    g_io_channel_unref(channel);
    sendPacket('R');
}
} // namespace

int main(int argc, char** argv) {
    AppState state;
    GtkApplication* app = gtk_application_new("io.github.wilf.hymission.search", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &state);
    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    if (state.im) {
        gtk_im_context_focus_out(state.im);
        g_object_unref(state.im);
    }
    g_object_unref(app);
    return status;
}
