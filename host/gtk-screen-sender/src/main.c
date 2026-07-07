#include <gtk/gtk.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "serial_port.h"

#define SERIAL_BAUD_RATE 115200
#define DEVICE_TEXT_LIMIT 32U

typedef struct
{
    GtkWidget *window;
    GtkComboBoxText *device_combo;
    GtkButton *connect_button;
    GtkButton *send_button;
    GtkTextView *message_view;
    GtkTextBuffer *message_buffer;
    GtkTextView *log_view;
    GtkTextBuffer *log_buffer;
    GtkLabel *status_label;
    GtkLabel *counter_label;
    int serial_fd;
    guint read_timer_id;
    guint connect_watch_id;
    unsigned int connect_generation;
    gint64 connect_started_us;
    bool connecting;
} AppData;

typedef struct
{
    AppData *app;
    gchar *path;
    int fd;
    unsigned int generation;
    gint64 started_us;
    gint64 worker_started_us;
    gint64 open_returned_us;
    char error[160];
} ConnectRequest;

static gboolean poll_serial(gpointer user_data);

static long long elapsed_ms(gint64 start_us, gint64 end_us)
{
    if(start_us <= 0 || end_us < start_us)
    {
        return 0;
    }

    return (long long)((end_us - start_us) / 1000);
}

static GtkWidget *make_icon_label(const char *icon_name, const char *label)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *image = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    GtkWidget *text = gtk_label_new(label);

    gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 0);

    return box;
}

static void set_button_content(GtkButton *button, const char *icon_name, const char *label)
{
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(button));

    if(child != NULL)
    {
        gtk_container_remove(GTK_CONTAINER(button), child);
    }

    gtk_container_add(GTK_CONTAINER(button), make_icon_label(icon_name, label));
    gtk_widget_show_all(GTK_WIDGET(button));
}

static void set_status(AppData *app, const char *message)
{
    gtk_label_set_text(app->status_label, message);
}

static void append_log(AppData *app, const char *prefix, const char *message)
{
    GtkTextIter end;
    GtkTextMark *insert_mark;
    GDateTime *now;
    gchar *clock_text;
    char stamp[32];

    gtk_text_buffer_get_end_iter(app->log_buffer, &end);

    now = g_date_time_new_now_local();
    clock_text = g_date_time_format(now, "%H:%M:%S");
    snprintf(stamp, sizeof(stamp), "[%s.%03d] ", clock_text, g_date_time_get_microsecond(now) / 1000);

    gtk_text_buffer_insert(app->log_buffer, &end, stamp, -1);

    if(prefix != NULL)
    {
        gtk_text_buffer_insert(app->log_buffer, &end, prefix, -1);
    }

    gtk_text_buffer_insert(app->log_buffer, &end, message, -1);
    gtk_text_buffer_insert(app->log_buffer, &end, "\n", -1);

    insert_mark = gtk_text_buffer_get_insert(app->log_buffer);
    gtk_text_view_scroll_mark_onscreen(app->log_view, insert_mark);

    g_free(clock_text);
    g_date_time_unref(now);
}

static void append_logf(AppData *app, const char *prefix, const char *format, ...)
{
    va_list args;
    gchar *message;

    va_start(args, format);
    message = g_strdup_vprintf(format, args);
    va_end(args);

    append_log(app, prefix, message);
    g_free(message);
}

static void refresh_ports(AppData *app)
{
    PicSerialPortInfo ports[32];
    size_t port_count = pic_serial_list_ports(ports, sizeof(ports) / sizeof(ports[0]));

    gtk_combo_box_text_remove_all(app->device_combo);

    if(port_count == 0U)
    {
        gtk_combo_box_text_append_text(app->device_combo, "No USB CDC devices found");
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->device_combo), 0);
        gtk_widget_set_sensitive(GTK_WIDGET(app->device_combo), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(app->connect_button), FALSE);
        set_status(app, "No device");
        append_log(app, "i ", "Refresh found no USB CDC devices");
        return;
    }

    for(size_t i = 0; i < port_count; i++)
    {
        gtk_combo_box_text_append_text(app->device_combo, ports[i].path);
    }

    gtk_combo_box_set_active(GTK_COMBO_BOX(app->device_combo), 0);
    gtk_widget_set_sensitive(GTK_WIDGET(app->device_combo), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->connect_button), TRUE);
    set_status(app, "Ready");
    append_logf(app, "i ", "Refresh found %zu device(s); selected %s", port_count, ports[0].path);
}

static void disconnect_device(AppData *app)
{
    app->connecting = false;

    if(app->connect_watch_id != 0U)
    {
        g_source_remove(app->connect_watch_id);
        app->connect_watch_id = 0U;
    }

    if(app->read_timer_id != 0U)
    {
        g_source_remove(app->read_timer_id);
        app->read_timer_id = 0U;
    }

    if(app->serial_fd >= 0)
    {
        append_logf(app, "i ", "Closing fd %d", app->serial_fd);
        pic_serial_close(app->serial_fd);
        app->serial_fd = -1;
    }

    set_button_content(app->connect_button, "network-connect-symbolic", "Connect");
    gtk_widget_set_sensitive(GTK_WIDGET(app->device_combo), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->send_button), FALSE);
    set_status(app, "Disconnected");
}

static gboolean connect_complete(gpointer user_data)
{
    ConnectRequest *request = user_data;
    AppData *app = request->app;

    if((request->generation != app->connect_generation) || !app->connecting)
    {
        append_logf(app, "i ", "Ignoring stale connect result for %s after %lld ms",
                    request->path,
                    elapsed_ms(request->started_us, request->open_returned_us));

        if(request->fd >= 0)
        {
            pic_serial_close(request->fd);
        }

        g_free(request->path);
        g_free(request);

        return G_SOURCE_REMOVE;
    }

    app->connecting = false;
    if(app->connect_watch_id != 0U)
    {
        g_source_remove(app->connect_watch_id);
        app->connect_watch_id = 0U;
    }

    gtk_widget_set_sensitive(GTK_WIDGET(app->connect_button), TRUE);

    if(request->fd < 0)
    {
        append_logf(app, "! ", "open() failed after %lld ms", elapsed_ms(request->started_us, request->open_returned_us));
        append_log(app, "! ", request->error);
        set_status(app, "Connection failed");
        gtk_widget_set_sensitive(GTK_WIDGET(app->device_combo), TRUE);
        gtk_widget_set_sensitive(GTK_WIDGET(app->send_button), FALSE);
        set_button_content(app->connect_button, "network-connect-symbolic", "Connect");
    }
    else
    {
        app->serial_fd = request->fd;
        append_logf(app, "+ ", "open() returned fd %d after %lld ms",
                    request->fd,
                    elapsed_ms(request->started_us, request->open_returned_us));
        set_button_content(app->connect_button, "network-offline-symbolic", "Disconnect");
        gtk_widget_set_sensitive(GTK_WIDGET(app->device_combo), FALSE);
        gtk_widget_set_sensitive(GTK_WIDGET(app->send_button), TRUE);
        set_status(app, request->path);
        append_log(app, "+ ", "Connected; polling for PIC responses every 80 ms");
        app->read_timer_id = g_timeout_add(80, poll_serial, app);
    }

    g_free(request->path);
    g_free(request);

    return G_SOURCE_REMOVE;
}

static gpointer connect_worker(gpointer user_data)
{
    ConnectRequest *request = user_data;

    request->worker_started_us = g_get_monotonic_time();
    request->fd = pic_serial_open(request->path, SERIAL_BAUD_RATE, request->error, sizeof(request->error));
    request->open_returned_us = g_get_monotonic_time();
    g_idle_add(connect_complete, request);

    return NULL;
}

static gboolean watch_connect(gpointer user_data)
{
    AppData *app = user_data;

    if(!app->connecting)
    {
        app->connect_watch_id = 0U;
        return G_SOURCE_REMOVE;
    }

    append_logf(app, "i ", "Still waiting for open() after %lld ms",
                elapsed_ms(app->connect_started_us, g_get_monotonic_time()));

    return G_SOURCE_CONTINUE;
}

static gboolean poll_serial(gpointer user_data)
{
    AppData *app = user_data;
    char buffer[256];
    char error[160];
    ssize_t bytes_read;

    bytes_read = pic_serial_read_available(app->serial_fd, buffer, sizeof(buffer) - 1U, error, sizeof(error));
    if(bytes_read < 0)
    {
        append_log(app, "! ", error);
        app->read_timer_id = 0U;
        disconnect_device(app);
        return G_SOURCE_REMOVE;
    }

    if(bytes_read > 0)
    {
        buffer[bytes_read] = '\0';
        g_strchomp(buffer);
        append_logf(app, "i ", "Read %zd byte(s) from PIC", bytes_read);
        if(buffer[0] != '\0')
        {
            append_log(app, "< ", buffer);
        }
    }

    return G_SOURCE_CONTINUE;
}

static void connect_device(AppData *app)
{
    ConnectRequest *request;
    GThread *thread;
    gchar *path = gtk_combo_box_text_get_active_text(app->device_combo);

    if(path == NULL || path[0] == '\0')
    {
        set_status(app, "No device selected");
        g_free(path);
        return;
    }

    request = g_new0(ConnectRequest, 1);
    request->app = app;
    request->path = path;
    request->fd = -1;
    request->started_us = g_get_monotonic_time();
    app->connect_generation++;
    request->generation = app->connect_generation;

    app->connecting = true;
    app->connect_started_us = request->started_us;
    set_status(app, "Connecting...");
    append_logf(app, "+ ", "Starting connect to %s", request->path);
    append_log(app, "i ", "Worker thread will call nonblocking open()");
    set_button_content(app->connect_button, "process-stop-symbolic", "Cancel");
    gtk_widget_set_sensitive(GTK_WIDGET(app->device_combo), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->connect_button), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(app->send_button), FALSE);

    app->connect_watch_id = g_timeout_add(1000, watch_connect, app);
    thread = g_thread_new("pic32-serial-connect", connect_worker, request);
    g_thread_unref(thread);
}

static void on_connect_clicked(GtkButton *button, gpointer user_data)
{
    AppData *app = user_data;
    (void)button;

    if(app->connecting)
    {
        app->connect_generation++;
        app->connecting = false;
        if(app->connect_watch_id != 0U)
        {
            g_source_remove(app->connect_watch_id);
            app->connect_watch_id = 0U;
        }
        append_logf(app, "! ", "Connection cancelled after %lld ms",
                    elapsed_ms(app->connect_started_us, g_get_monotonic_time()));
        set_status(app, "Connection cancelled");
        set_button_content(app->connect_button, "network-connect-symbolic", "Connect");
        gtk_widget_set_sensitive(GTK_WIDGET(app->device_combo), TRUE);
        gtk_widget_set_sensitive(GTK_WIDGET(app->send_button), FALSE);
        return;
    }

    if(app->serial_fd >= 0)
    {
        disconnect_device(app);
    }
    else
    {
        connect_device(app);
    }
}

static char *build_device_payload(const char *text, bool *truncated)
{
    GString *payload = g_string_new(NULL);
    size_t accepted = 0;

    *truncated = false;

    for(const unsigned char *cursor = (const unsigned char *)text; *cursor != '\0'; cursor++)
    {
        char out;

        if(*cursor == '\r' || *cursor == '\n' || *cursor == '\t')
        {
            out = ' ';
        }
        else if(*cursor >= 32U && *cursor <= 126U)
        {
            out = (char)*cursor;
        }
        else
        {
            out = '?';
        }

        if(accepted >= DEVICE_TEXT_LIMIT)
        {
            *truncated = true;
            break;
        }

        g_string_append_c(payload, out);
        accepted++;
    }

    g_string_append_c(payload, '\n');

    return g_string_free(payload, FALSE);
}

static void on_send_clicked(GtkButton *button, gpointer user_data)
{
    AppData *app = user_data;
    GtkTextIter start;
    GtkTextIter end;
    char error[160];
    gchar *message;
    gchar *payload;
    gchar *payload_preview;
    bool truncated;
    gint64 send_started_us;

    (void)button;

    if(app->serial_fd < 0)
    {
        set_status(app, "Not connected");
        return;
    }

    gtk_text_buffer_get_bounds(app->message_buffer, &start, &end);
    message = gtk_text_buffer_get_text(app->message_buffer, &start, &end, FALSE);

    if(message == NULL || message[0] == '\0')
    {
        set_status(app, "Message is empty");
        g_free(message);
        return;
    }

    payload = build_device_payload(message, &truncated);
    payload_preview = g_strdup(payload);
    g_strchomp(payload_preview);
    send_started_us = g_get_monotonic_time();
    append_logf(app, "> ", "Sending %zu byte(s): %s", strlen(payload), payload_preview);

    if(pic_serial_write_all(app->serial_fd, payload, strlen(payload), 1000, error, sizeof(error)) != 0)
    {
        append_logf(app, "! ", "Write failed after %lld ms",
                    elapsed_ms(send_started_us, g_get_monotonic_time()));
        append_log(app, "! ", error);
        set_status(app, "Send failed");
    }
    else
    {
        append_logf(app, "+ ", "Write completed after %lld ms",
                    elapsed_ms(send_started_us, g_get_monotonic_time()));
        set_status(app, truncated ? "Sent first 32 characters" : "Sent");
    }

    g_free(payload_preview);
    g_free(payload);
    g_free(message);
}

static void on_refresh_clicked(GtkButton *button, gpointer user_data)
{
    AppData *app = user_data;
    (void)button;

    if(app->serial_fd < 0)
    {
        refresh_ports(app);
    }
}

static void on_clear_clicked(GtkButton *button, gpointer user_data)
{
    AppData *app = user_data;
    (void)button;

    gtk_text_buffer_set_text(app->message_buffer, "", -1);
    set_status(app, "Ready");
}

static void on_message_changed(GtkTextBuffer *buffer, gpointer user_data)
{
    AppData *app = user_data;
    GtkTextIter start;
    GtkTextIter end;
    gchar *text;
    size_t count = 0;
    char label[24];

    gtk_text_buffer_get_bounds(buffer, &start, &end);
    text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    for(const unsigned char *cursor = (const unsigned char *)text; cursor != NULL && *cursor != '\0'; cursor++)
    {
        count++;
    }

    snprintf(label, sizeof(label), "%zu/%u", count > DEVICE_TEXT_LIMIT ? DEVICE_TEXT_LIMIT : count, (unsigned int)DEVICE_TEXT_LIMIT);
    gtk_label_set_text(app->counter_label, label);
    g_free(text);
}

static GtkWidget *new_icon_button(const char *icon_name, const char *tooltip)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(button, tooltip);
    return button;
}

static GtkWidget *new_scrolled_text_view(GtkTextView **view, GtkTextBuffer **buffer, int height)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);

    *view = GTK_TEXT_VIEW(gtk_text_view_new());
    *buffer = gtk_text_view_get_buffer(*view);

    gtk_text_view_set_wrap_mode(*view, GTK_WRAP_WORD_CHAR);
    gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(*view));
    gtk_widget_set_size_request(scroll, -1, height);

    return scroll;
}

static void build_ui(AppData *app)
{
    GtkWidget *main_box;
    GtkWidget *device_row;
    GtkWidget *device_label;
    GtkWidget *refresh_button;
    GtkWidget *message_header;
    GtkWidget *message_label;
    GtkWidget *message_scroll;
    GtkWidget *send_row;
    GtkWidget *clear_button;
    GtkWidget *log_label;
    GtkWidget *log_scroll;

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "PIC32 Screen Sender");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 520, 420);
    gtk_container_set_border_width(GTK_CONTAINER(app->window), 16);

    main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_add(GTK_CONTAINER(app->window), main_box);

    device_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    device_label = gtk_label_new("Device");
    gtk_widget_set_halign(device_label, GTK_ALIGN_START);
    app->device_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_widget_set_hexpand(GTK_WIDGET(app->device_combo), TRUE);

    refresh_button = new_icon_button("view-refresh-symbolic", "Refresh devices");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), app);

    app->connect_button = GTK_BUTTON(gtk_button_new());
    set_button_content(app->connect_button, "network-connect-symbolic", "Connect");
    g_signal_connect(app->connect_button, "clicked", G_CALLBACK(on_connect_clicked), app);

    gtk_box_pack_start(GTK_BOX(device_row), device_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(device_row), GTK_WIDGET(app->device_combo), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(device_row), refresh_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(device_row), GTK_WIDGET(app->connect_button), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), device_row, FALSE, FALSE, 0);

    message_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    message_label = gtk_label_new("Message");
    app->counter_label = GTK_LABEL(gtk_label_new("0/32"));
    gtk_widget_set_halign(message_label, GTK_ALIGN_START);
    gtk_widget_set_halign(GTK_WIDGET(app->counter_label), GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(message_header), message_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(message_header), GTK_WIDGET(app->counter_label), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), message_header, FALSE, FALSE, 0);

    message_scroll = new_scrolled_text_view(&app->message_view, &app->message_buffer, 96);
    g_signal_connect(app->message_buffer, "changed", G_CALLBACK(on_message_changed), app);
    gtk_box_pack_start(GTK_BOX(main_box), message_scroll, FALSE, FALSE, 0);

    send_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    clear_button = new_icon_button("edit-clear-symbolic", "Clear message");
    g_signal_connect(clear_button, "clicked", G_CALLBACK(on_clear_clicked), app);

    app->send_button = GTK_BUTTON(gtk_button_new());
    set_button_content(app->send_button, "mail-send-symbolic", "Send");
    gtk_widget_set_sensitive(GTK_WIDGET(app->send_button), FALSE);
    g_signal_connect(app->send_button, "clicked", G_CALLBACK(on_send_clicked), app);

    gtk_box_pack_end(GTK_BOX(send_row), GTK_WIDGET(app->send_button), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(send_row), clear_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), send_row, FALSE, FALSE, 0);

    log_label = gtk_label_new("Device log");
    gtk_widget_set_halign(log_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(main_box), log_label, FALSE, FALSE, 0);

    log_scroll = new_scrolled_text_view(&app->log_view, &app->log_buffer, 130);
    gtk_text_view_set_editable(app->log_view, FALSE);
    gtk_text_view_set_cursor_visible(app->log_view, FALSE);
    gtk_box_pack_start(GTK_BOX(main_box), log_scroll, TRUE, TRUE, 0);

    app->status_label = GTK_LABEL(gtk_label_new("Ready"));
    gtk_widget_set_halign(GTK_WIDGET(app->status_label), GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(main_box), GTK_WIDGET(app->status_label), FALSE, FALSE, 0);

    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
}

int main(int argc, char **argv)
{
    AppData app;

    memset(&app, 0, sizeof(app));
    app.serial_fd = -1;

    gtk_init(&argc, &argv);
    build_ui(&app);
    refresh_ports(&app);

    gtk_widget_show_all(app.window);
    gtk_main();

    disconnect_device(&app);

    return 0;
}
