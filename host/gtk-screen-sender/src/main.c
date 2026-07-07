#include <gtk/gtk.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "serial_port.h"

#define SERIAL_BAUD_RATE 115200
#define CATEGORY_COUNT 6
#define AMOUNT_COUNT 5
#define OPTION_COUNT 4
#define PLAYER_COUNT 2
#define LCD_LINE_LENGTH 16
#define SERIAL_RX_BUFFER_SIZE 1024U

typedef enum
{
    GAME_MODE_BOARD,
    GAME_MODE_ANSWER
} GameMode;

typedef enum
{
    PLAYER_ACTION_NEXT,
    PLAYER_ACTION_SELECT
} PlayerAction;

typedef struct
{
    char question[320];
    char options[OPTION_COUNT][96];
    int correct_answer;
    bool used;
} QuestionCell;

typedef struct
{
    int cursor_index;
    int answer_index;
    int selected_answer;
    int score;
    bool answered;
    bool last_correct;
} PlayerState;

typedef struct
{
    GtkWidget *window;
    GtkComboBoxText *device_combo;
    GtkButton *connect_button;
    GtkLabel *status_label;
    GtkLabel *mode_label;
    GtkLabel *score_labels[PLAYER_COUNT];
    GtkLabel *player_status_labels[PLAYER_COUNT];
    GtkEntry *category_entries[CATEGORY_COUNT];
    GtkSpinButton *amount_spins[AMOUNT_COUNT];
    GtkButton *board_buttons[CATEGORY_COUNT][AMOUNT_COUNT];
    GtkLabel *editor_heading_label;
    GtkTextView *question_view;
    GtkTextBuffer *question_buffer;
    GtkEntry *option_entries[OPTION_COUNT];
    GtkComboBoxText *correct_combo;
    GtkButton *save_button;
    GtkButton *start_button;
    GtkButton *reset_button;
    GtkTextView *log_view;
    GtkTextBuffer *log_buffer;

    int serial_fd;
    guint read_timer_id;
    guint connect_watch_id;
    guint result_timeout_id;
    unsigned int connect_generation;
    gint64 connect_started_us;
    bool connecting;

    char serial_rx_buffer[SERIAL_RX_BUFFER_SIZE];
    size_t serial_rx_length;

    char categories[CATEGORY_COUNT][48];
    int amounts[AMOUNT_COUNT];
    QuestionCell cells[CATEGORY_COUNT][AMOUNT_COUNT];
    PlayerState players[PLAYER_COUNT];
    GameMode mode;
    int selected_category;
    int selected_amount;
    int active_category;
    int active_amount;
    int chooser_player;
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
static void refresh_ui(AppData *app);
static void update_all_lcds(AppData *app);
static void load_editor_from_selected(AppData *app);
static void save_editor_to_selected(AppData *app);
static void handle_player_action(AppData *app, int player, PlayerAction action);

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

static GtkWidget *new_icon_button(const char *icon_name, const char *tooltip)
{
    GtkWidget *button = gtk_button_new_from_icon_name(icon_name, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(button, tooltip);
    return button;
}

static GtkWidget *new_scrolled_text_view(GtkTextView **view, GtkTextBuffer **buffer, int height, bool editable)
{
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);

    *view = GTK_TEXT_VIEW(gtk_text_view_new());
    *buffer = gtk_text_view_get_buffer(*view);

    gtk_text_view_set_wrap_mode(*view, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_editable(*view, editable);
    gtk_text_view_set_cursor_visible(*view, editable);
    gtk_container_add(GTK_CONTAINER(scroll), GTK_WIDGET(*view));
    gtk_widget_set_size_request(scroll, -1, height);

    return scroll;
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

static const char *player_action_name(PlayerAction action)
{
    return action == PLAYER_ACTION_NEXT ? "NEXT" : "SELECT";
}

static const char *game_mode_name(GameMode mode)
{
    return mode == GAME_MODE_BOARD ? "board" : "answer";
}

static int next_player_index(int player)
{
    return (player + 1) % PLAYER_COUNT;
}

static void copy_text(char *dest, size_t dest_size, const char *source)
{
    if(source == NULL)
    {
        source = "";
    }

    g_strlcpy(dest, source, dest_size);
}

static int cell_index(int category, int amount)
{
    return (amount * CATEGORY_COUNT) + category;
}

static void cell_from_index(int index, int *category, int *amount)
{
    if(index < 0)
    {
        index = 0;
    }

    *category = index % CATEGORY_COUNT;
    *amount = index / CATEGORY_COUNT;
}

static QuestionCell *cell_at(AppData *app, int category, int amount)
{
    return &app->cells[category][amount];
}

static QuestionCell *selected_cell(AppData *app)
{
    return cell_at(app, app->selected_category, app->selected_amount);
}

static bool cell_is_available(AppData *app, int index)
{
    int category;
    int amount;

    if(index < 0 || index >= (CATEGORY_COUNT * AMOUNT_COUNT))
    {
        return false;
    }

    cell_from_index(index, &category, &amount);
    return !cell_at(app, category, amount)->used;
}

static int find_available_cell(AppData *app, int start_index, int direction)
{
    const int total = CATEGORY_COUNT * AMOUNT_COUNT;

    if(direction == 0)
    {
        direction = 1;
    }

    for(int step = 0; step < total; step++)
    {
        int index = (start_index + (step * direction)) % total;

        if(index < 0)
        {
            index += total;
        }

        if(cell_is_available(app, index))
        {
            return index;
        }
    }

    return -1;
}

static void reset_player_cursors(AppData *app)
{
    int first = find_available_cell(app, 0, 1);

    for(int player = 0; player < PLAYER_COUNT; player++)
    {
        app->players[player].cursor_index = first;
        app->players[player].answer_index = 0;
        app->players[player].selected_answer = -1;
        app->players[player].answered = false;
        app->players[player].last_correct = false;
    }
}

static void game_init_content(AppData *app)
{
    typedef struct
    {
        const char *question;
        const char *options[OPTION_COUNT];
        int correct_answer;
    } DefaultQuestion;

    static const char *default_categories[CATEGORY_COUNT] =
    {
        "Science",
        "History",
        "Geography",
        "Movies",
        "Tech",
        "Games"
    };

    static const DefaultQuestion default_questions[CATEGORY_COUNT][AMOUNT_COUNT] =
    {
        {
            {
                "What gas do plants absorb from the air during photosynthesis?",
                { "Oxygen", "Carbon dioxide", "Nitrogen", "Helium" },
                1
            },
            {
                "What force pulls objects toward the center of Earth?",
                { "Magnetism", "Friction", "Gravity", "Electricity" },
                2
            },
            {
                "What is the chemical formula for water?",
                { "CO2", "H2O", "O2", "NaCl" },
                1
            },
            {
                "Which planet is often called the Red Planet?",
                { "Venus", "Jupiter", "Mars", "Mercury" },
                2
            },
            {
                "What is the smallest basic unit of living organisms?",
                { "Atom", "Cell", "Organ", "Tissue" },
                1
            }
        },
        {
            {
                "Who was the first president of the United States?",
                { "John Adams", "Thomas Jefferson", "George Washington", "James Madison" },
                2
            },
            {
                "Which wall fell in 1989, becoming a symbol of the Cold War ending?",
                { "Hadrian's Wall", "Berlin Wall", "Great Wall", "Western Wall" },
                1
            },
            {
                "Which ancient civilization built the pyramids at Giza?",
                { "Romans", "Egyptians", "Vikings", "Aztecs" },
                1
            },
            {
                "Which American document begins with the words 'We the People'?",
                { "Declaration of Independence", "Bill of Rights", "U.S. Constitution", "Gettysburg Address" },
                2
            },
            {
                "What ship carried the Pilgrims to New England in 1620?",
                { "Santa Maria", "Mayflower", "Endeavour", "Beagle" },
                1
            }
        },
        {
            {
                "What is the largest ocean on Earth?",
                { "Atlantic", "Indian", "Arctic", "Pacific" },
                3
            },
            {
                "Which country is famously shaped like a boot?",
                { "Italy", "Greece", "Chile", "Portugal" },
                0
            },
            {
                "Which river runs through Egypt and helped support its ancient civilization?",
                { "Amazon", "Danube", "Nile", "Ganges" },
                2
            },
            {
                "What is the capital city of Japan?",
                { "Kyoto", "Tokyo", "Osaka", "Sapporo" },
                1
            },
            {
                "What is the highest mountain above sea level?",
                { "K2", "Denali", "Mount Everest", "Kilimanjaro" },
                2
            }
        },
        {
            {
                "Which movie franchise is known for Jedi and lightsabers?",
                { "Star Trek", "Star Wars", "The Matrix", "Avatar" },
                1
            },
            {
                "What is the name of Harry Potter's school?",
                { "Hogwarts", "Narnia", "Nevermore", "Rivendell" },
                0
            },
            {
                "Which Disney movie features sisters named Elsa and Anna?",
                { "Moana", "Frozen", "Tangled", "Brave" },
                1
            },
            {
                "What fictional city is protected by Batman?",
                { "Metropolis", "Gotham City", "Central City", "Star City" },
                1
            },
            {
                "Which actor made the line 'I'll be back' famous in The Terminator?",
                { "Bruce Willis", "Sylvester Stallone", "Arnold Schwarzenegger", "Harrison Ford" },
                2
            }
        },
        {
            {
                "What does CPU stand for?",
                { "Central Processing Unit", "Computer Power Utility", "Core Program Update", "Control Panel Unit" },
                0
            },
            {
                "Binary numbers use which base?",
                { "Base 8", "Base 10", "Base 16", "Base 2" },
                3
            },
            {
                "Which protocol is the foundation for loading most web pages?",
                { "HTTP", "MIDI", "SMTP", "GPS" },
                0
            },
            {
                "What sensor measures acceleration?",
                { "Thermistor", "Accelerometer", "Barometer", "Photodiode" },
                1
            },
            {
                "What does ROM usually stand for in electronics?",
                { "Random Output Module", "Read-Only Memory", "Remote Operating Mode", "Runtime Object Map" },
                1
            }
        },
        {
            {
                "How many points is a touchdown worth in American football?",
                { "3", "6", "7", "10" },
                1
            },
            {
                "Which chess piece moves in an L shape?",
                { "Bishop", "Rook", "Knight", "Queen" },
                2
            },
            {
                "How often are the Summer Olympic Games normally held?",
                { "Every 2 years", "Every 3 years", "Every 4 years", "Every 5 years" },
                2
            },
            {
                "Which board game has players buy properties and collect rent?",
                { "Monopoly", "Risk", "Clue", "Scrabble" },
                0
            },
            {
                "In tennis, what is the score called after deuce when one player wins the next point?",
                { "Match", "Break", "Advantage", "Set" },
                2
            }
        }
    };

    for(int category = 0; category < CATEGORY_COUNT; category++)
    {
        copy_text(app->categories[category], sizeof(app->categories[category]), default_categories[category]);
    }

    for(int amount = 0; amount < AMOUNT_COUNT; amount++)
    {
        app->amounts[amount] = (amount + 1) * 100;
    }

    for(int category = 0; category < CATEGORY_COUNT; category++)
    {
        for(int amount = 0; amount < AMOUNT_COUNT; amount++)
        {
            QuestionCell *cell = cell_at(app, category, amount);
            const DefaultQuestion *question = &default_questions[category][amount];

            copy_text(cell->question, sizeof(cell->question), question->question);
            for(int option = 0; option < OPTION_COUNT; option++)
            {
                copy_text(cell->options[option],
                          sizeof(cell->options[option]),
                          question->options[option]);
            }
            cell->correct_answer = question->correct_answer;
            cell->used = false;
        }
    }

    app->selected_category = 0;
    app->selected_amount = 0;
    app->active_category = 0;
    app->active_amount = 0;
    app->mode = GAME_MODE_BOARD;
    app->chooser_player = 0;
    app->players[0].score = 0;
    app->players[1].score = 0;
    reset_player_cursors(app);
}

static void lcd_clean_line(const char *source, char *dest)
{
    size_t length = 0;

    if(source == NULL)
    {
        source = "";
    }

    while(source[length] != '\0' && length < LCD_LINE_LENGTH)
    {
        unsigned char c = (unsigned char)source[length];

        if(c == '|' || c == '\r' || c == '\n' || c == '\t')
        {
            dest[length] = ' ';
        }
        else if(c >= 32U && c <= 126U)
        {
            dest[length] = (char)c;
        }
        else
        {
            dest[length] = '?';
        }

        length++;
    }

    dest[length] = '\0';
}

static void send_lcd_lines_internal(AppData *app,
                                    int player,
                                    const char *line1,
                                    const char *line2,
                                    bool log_command)
{
    char clean1[LCD_LINE_LENGTH + 1U];
    char clean2[LCD_LINE_LENGTH + 1U];
    char command[64];
    char preview[64];
    char error[160];

    if(app->serial_fd < 0)
    {
        return;
    }

    lcd_clean_line(line1, clean1);
    lcd_clean_line(line2, clean2);

    snprintf(command, sizeof(command), "P%d:%s|%s\n", player + 1, clean1, clean2);
    g_strlcpy(preview, command, sizeof(preview));
    g_strchomp(preview);

    if(log_command)
    {
        append_logf(app, "> ", "%s", preview);
    }

    if(pic_serial_write_all(app->serial_fd, command, strlen(command), 350, error, sizeof(error)) != 0)
    {
        append_log(app, "! ", error);
        set_status(app, "LCD write failed");
    }
}

static void send_led_command(AppData *app, int player, const char *button_name, const char *mode)
{
    char command[40];
    char error[160];

    if(app->serial_fd < 0)
    {
        return;
    }

    snprintf(command, sizeof(command), "LED:P%d:%s:%s\n",
             player + 1,
             button_name,
             mode);

    if(pic_serial_write_all(app->serial_fd, command, strlen(command), 250, error, sizeof(error)) != 0)
    {
        append_log(app, "! ", error);
        set_status(app, "LED write failed");
    }
}

static void append_pic_protocol_field(GString *command, const char *text)
{
    if(text == NULL)
    {
        text = "";
    }

    for(const unsigned char *cursor = (const unsigned char *)text; *cursor != '\0'; cursor++)
    {
        unsigned char c = *cursor;

        if(c == '|' || c == '\r' || c == '\n' || c == '\t')
        {
            g_string_append_c(command, ' ');
        }
        else if(c >= 32U && c <= 126U)
        {
            g_string_append_c(command, (char)c);
        }
        else
        {
            g_string_append_c(command, '?');
        }
    }
}

static void send_question_set_command(AppData *app)
{
    QuestionCell *cell = cell_at(app, app->active_category, app->active_amount);
    GString *command;
    char error[160];

    if(app->serial_fd < 0)
    {
        return;
    }

    command = g_string_new("QSET:");
    append_pic_protocol_field(command, cell->question);

    for(int option = 0; option < OPTION_COUNT; option++)
    {
        g_string_append_c(command, '|');
        append_pic_protocol_field(command, cell->options[option]);
    }

    g_string_append_c(command, '\n');
    append_logf(app, "> ", "QSET question/options (%zu byte(s))", command->len);

    if(pic_serial_write_all(app->serial_fd, command->str, command->len, 1500, error, sizeof(error)) != 0)
    {
        append_log(app, "! ", error);
        set_status(app, "Question write failed");
    }

    g_string_free(command, TRUE);
}

static void send_question_select_command(AppData *app, int player)
{
    char command[16];
    char error[160];

    if(app->serial_fd < 0 || player < 0 || player >= PLAYER_COUNT)
    {
        return;
    }

    snprintf(command, sizeof(command), "QSEL:P%d:%c\n",
             player + 1,
             'A' + app->players[player].answer_index);

    append_logf(app, "> ", "QSEL:P%d:%c", player + 1, 'A' + app->players[player].answer_index);

    if(pic_serial_write_all(app->serial_fd, command, strlen(command), 250, error, sizeof(error)) != 0)
    {
        append_log(app, "! ", error);
        set_status(app, "Answer display write failed");
    }
}

static void update_leds_for_player(AppData *app, int player)
{
    const char *next_mode = "OFF";
    const char *select_mode = "OFF";

    if(player < 0 || player >= PLAYER_COUNT)
    {
        return;
    }

    if(app->mode == GAME_MODE_BOARD)
    {
        if(app->players[player].cursor_index >= 0 && player == app->chooser_player)
        {
            next_mode = "SLOW";
            select_mode = "FAST";
        }
    }
    else if(!app->players[player].answered)
    {
        next_mode = "ON";
        select_mode = "FAST";
    }
    else
    {
        next_mode = app->players[player].last_correct ? "FAST" : "SLOW";
        select_mode = app->players[player].last_correct ? "FAST" : "SLOW";
    }

    send_led_command(app, player, "NEXT", next_mode);
    send_led_command(app, player, "SELECT", select_mode);
}

static void update_lcd_for_player_internal(AppData *app, int player, bool log_command, bool refresh_leds)
{
    PlayerState *state = &app->players[player];
    char line1[80];
    char line2[80];
    int category;
    int amount;

    if(app->mode == GAME_MODE_ANSWER)
    {
        static const char answer_letters[OPTION_COUNT] = { 'A', 'B', 'C', 'D' };

        if(state->answered)
        {
            snprintf(line1, sizeof(line1), "%s %c %c%d",
                     state->last_correct ? "Correct" : "Wrong",
                     answer_letters[state->selected_answer],
                     state->last_correct ? '+' : '-',
                     app->amounts[app->active_amount]);
            snprintf(line2, sizeof(line2), "Score %d", state->score);
        }
        else
        {
            send_question_select_command(app, player);
            if(refresh_leds)
            {
                update_leds_for_player(app, player);
            }
            return;
        }

        send_lcd_lines_internal(app, player, line1, line2, log_command);
        if(refresh_leds)
        {
            update_leds_for_player(app, player);
        }
        return;
    }

    if(state->cursor_index < 0)
    {
        snprintf(line1, sizeof(line1), "P%d Final %d", player + 1, state->score);
        snprintf(line2, sizeof(line2), "Board complete");
        send_lcd_lines_internal(app, player, line1, line2, log_command);
        if(refresh_leds)
        {
            update_leds_for_player(app, player);
        }
        return;
    }

    if(player != app->chooser_player)
    {
        snprintf(line1, sizeof(line1), "P%d Waiting", player + 1);
        snprintf(line2, sizeof(line2), "P%d choosing", app->chooser_player + 1);
        send_lcd_lines_internal(app, player, line1, line2, log_command);
        if(refresh_leds)
        {
            update_leds_for_player(app, player);
        }
        return;
    }

    cell_from_index(state->cursor_index, &category, &amount);
    snprintf(line1, sizeof(line1), "P%d Turn $%d", player + 1, state->score);
    snprintf(line2, sizeof(line2), "%s $%d", app->categories[category], app->amounts[amount]);
    send_lcd_lines_internal(app, player, line1, line2, log_command);
    if(refresh_leds)
    {
        update_leds_for_player(app, player);
    }
}

static void update_lcd_for_player(AppData *app, int player)
{
    update_lcd_for_player_internal(app, player, true, true);
}

static void update_all_lcds(AppData *app)
{
    for(int player = 0; player < PLAYER_COUNT; player++)
    {
        update_lcd_for_player(app, player);
    }
}

static void sync_lcds_for_current_mode(AppData *app)
{
    if(app->mode == GAME_MODE_ANSWER)
    {
        send_question_set_command(app);
    }

    update_all_lcds(app);
}

static void save_editor_to_selected(AppData *app)
{
    QuestionCell *cell = selected_cell(app);
    GtkTextIter start;
    GtkTextIter end;
    gchar *question;
    int correct;

    gtk_text_buffer_get_bounds(app->question_buffer, &start, &end);
    question = gtk_text_buffer_get_text(app->question_buffer, &start, &end, FALSE);
    copy_text(cell->question, sizeof(cell->question), question);
    g_free(question);

    for(int option = 0; option < OPTION_COUNT; option++)
    {
        copy_text(cell->options[option],
                  sizeof(cell->options[option]),
                  gtk_entry_get_text(app->option_entries[option]));
    }

    correct = gtk_combo_box_get_active(GTK_COMBO_BOX(app->correct_combo));
    cell->correct_answer = (correct >= 0 && correct < OPTION_COUNT) ? correct : 0;
}

static void load_editor_from_selected(AppData *app)
{
    QuestionCell *cell = selected_cell(app);
    char heading[96];

    snprintf(heading, sizeof(heading), "%s $%d",
             app->categories[app->selected_category],
             app->amounts[app->selected_amount]);
    gtk_label_set_text(app->editor_heading_label, heading);

    gtk_text_buffer_set_text(app->question_buffer, cell->question, -1);

    for(int option = 0; option < OPTION_COUNT; option++)
    {
        gtk_entry_set_text(app->option_entries[option], cell->options[option]);
    }

    gtk_combo_box_set_active(GTK_COMBO_BOX(app->correct_combo), cell->correct_answer);
}

static void refresh_board_buttons(AppData *app)
{
    for(int category = 0; category < CATEGORY_COUNT; category++)
    {
        for(int amount = 0; amount < AMOUNT_COUNT; amount++)
        {
            GtkWidget *button = GTK_WIDGET(app->board_buttons[category][amount]);
            GtkStyleContext *style = gtk_widget_get_style_context(button);
            QuestionCell *cell = cell_at(app, category, amount);
            char label[24];

            if(cell->used)
            {
                snprintf(label, sizeof(label), "Used");
            }
            else
            {
                snprintf(label, sizeof(label), "$%d", app->amounts[amount]);
            }

            gtk_button_set_label(app->board_buttons[category][amount], label);

            if(category == app->selected_category && amount == app->selected_amount)
            {
                gtk_style_context_add_class(style, "suggested-action");
            }
            else
            {
                gtk_style_context_remove_class(style, "suggested-action");
            }
        }
    }
}

static void refresh_ui(AppData *app)
{
    char text[256];

    refresh_board_buttons(app);

    for(int player = 0; player < PLAYER_COUNT; player++)
    {
        snprintf(text, sizeof(text), "P%d  $%d", player + 1, app->players[player].score);
        gtk_label_set_text(app->score_labels[player], text);

        if(app->mode == GAME_MODE_ANSWER)
        {
            if(app->players[player].answered)
            {
                snprintf(text, sizeof(text), "P%d locked %c", player + 1,
                         'A' + app->players[player].selected_answer);
            }
            else
            {
                snprintf(text, sizeof(text), "P%d answer %c", player + 1,
                         'A' + app->players[player].answer_index);
            }
        }
        else if(app->players[player].cursor_index >= 0)
        {
            int category;
            int amount;

            if(player == app->chooser_player)
            {
                cell_from_index(app->players[player].cursor_index, &category, &amount);
                snprintf(text, sizeof(text), "P%d choosing: %s $%d", player + 1,
                         app->categories[category], app->amounts[amount]);
            }
            else
            {
                snprintf(text, sizeof(text), "P%d locked: P%d choosing",
                         player + 1,
                         app->chooser_player + 1);
            }
        }
        else
        {
            snprintf(text, sizeof(text), "P%d done", player + 1);
        }

        gtk_label_set_text(app->player_status_labels[player], text);
    }

    if(app->mode == GAME_MODE_ANSWER)
    {
        snprintf(text, sizeof(text), "Question: %s $%d",
                 app->categories[app->active_category],
                 app->amounts[app->active_amount]);
        set_button_content(app->start_button, "go-next-symbolic", "Board");
    }
    else
    {
        snprintf(text, sizeof(text), "P%d choose category", app->chooser_player + 1);
        set_button_content(app->start_button, "media-playback-start-symbolic", "Start");
    }

    gtk_label_set_text(app->mode_label, text);
}

static gboolean finish_question_timeout(gpointer user_data)
{
    AppData *app = user_data;
    int start_index = cell_index(app->active_category, app->active_amount);
    int next_index = find_available_cell(app, start_index + 1, 1);

    app->result_timeout_id = 0U;
    app->mode = GAME_MODE_BOARD;

    for(int player = 0; player < PLAYER_COUNT; player++)
    {
        app->players[player].cursor_index = next_index;
        app->players[player].answered = false;
        app->players[player].answer_index = 0;
        app->players[player].selected_answer = -1;
    }

    refresh_ui(app);
    update_all_lcds(app);

    return G_SOURCE_REMOVE;
}

static void finish_question_now(AppData *app)
{
    if(app->result_timeout_id != 0U)
    {
        g_source_remove(app->result_timeout_id);
        app->result_timeout_id = 0U;
    }

    (void)finish_question_timeout(app);
}

static void start_question(AppData *app, int category, int amount, int player)
{
    QuestionCell *cell = cell_at(app, category, amount);
    int selected_by;

    if(cell->used)
    {
        append_logf(app, "! ", "%s $%d has already been used",
                    app->categories[category], app->amounts[amount]);
        return;
    }

    save_editor_to_selected(app);
    selected_by = player >= 0 ? player : app->chooser_player;

    app->selected_category = category;
    app->selected_amount = amount;
    app->active_category = category;
    app->active_amount = amount;
    app->mode = GAME_MODE_ANSWER;
    app->chooser_player = next_player_index(selected_by);
    cell->used = true;

    for(int i = 0; i < PLAYER_COUNT; i++)
    {
        app->players[i].answer_index = 0;
        app->players[i].selected_answer = -1;
        app->players[i].answered = false;
        app->players[i].last_correct = false;
    }

    load_editor_from_selected(app);
    refresh_ui(app);
    send_question_set_command(app);
    for(int i = 0; i < PLAYER_COUNT; i++)
    {
        update_leds_for_player(app, i);
    }

    if(player >= 0)
    {
        append_logf(app, "+ ", "P%d selected %s $%d; next chooser P%d",
                    player + 1,
                    app->categories[category],
                    app->amounts[amount],
                    app->chooser_player + 1);
    }
    else
    {
        append_logf(app, "+ ", "Started %s $%d for P%d; next chooser P%d",
                    app->categories[category],
                    app->amounts[amount],
                    selected_by + 1,
                    app->chooser_player + 1);
    }
}

static void handle_answer_select(AppData *app, int player)
{
    PlayerState *state = &app->players[player];
    QuestionCell *cell = cell_at(app, app->active_category, app->active_amount);
    int amount = app->amounts[app->active_amount];
    bool correct;

    if(state->answered)
    {
        append_logf(app, "i ", "P%d select ignored; answer already locked", player + 1);
        return;
    }

    state->selected_answer = state->answer_index;
    state->answered = true;
    correct = (state->selected_answer == cell->correct_answer);
    state->last_correct = correct;

    if(correct)
    {
        state->score += amount;
    }
    else
    {
        state->score -= amount;
    }

    append_logf(app, correct ? "+ " : "! ",
                "P%d answered %c for %s $%d: %s",
                player + 1,
                'A' + state->selected_answer,
                app->categories[app->active_category],
                amount,
                correct ? "correct" : "wrong");

    update_lcd_for_player(app, player);
    refresh_ui(app);

    if(app->players[0].answered && app->players[1].answered && app->result_timeout_id == 0U)
    {
        app->result_timeout_id = g_timeout_add(1800, finish_question_timeout, app);
    }
}

static void move_player_cursor(AppData *app, int player, int direction)
{
    PlayerState *state = &app->players[player];
    int start_index = state->cursor_index < 0 ? 0 : state->cursor_index + direction;
    int next_index;

    next_index = find_available_cell(app, start_index, direction);

    if(next_index >= 0)
    {
        int category;
        int amount;

        save_editor_to_selected(app);
        state->cursor_index = next_index;
        cell_from_index(next_index, &category, &amount);
        app->selected_category = category;
        app->selected_amount = amount;
        load_editor_from_selected(app);

        append_logf(app, "+ ", "P%d next -> %s $%d",
                    player + 1,
                    app->categories[category],
                    app->amounts[amount]);
    }
    else
    {
        append_logf(app, "! ", "P%d next ignored; no unused cells left", player + 1);
    }

    refresh_ui(app);
    update_lcd_for_player(app, player);
}

static void handle_player_action(AppData *app, int player, PlayerAction action)
{
    if(player < 0 || player >= PLAYER_COUNT)
    {
        return;
    }

    append_logf(app, "i ", "P%d %s in %s mode",
                player + 1,
                player_action_name(action),
                game_mode_name(app->mode));

    if(app->mode == GAME_MODE_BOARD)
    {
        if(player != app->chooser_player)
        {
            append_logf(app, "i ", "P%d ignored; P%d chooses this round",
                        player + 1,
                        app->chooser_player + 1);
            update_lcd_for_player(app, player);
            return;
        }

        if(action == PLAYER_ACTION_NEXT)
        {
            move_player_cursor(app, player, 1);
        }
        else if(app->players[player].cursor_index >= 0)
        {
            int category;
            int amount;

            cell_from_index(app->players[player].cursor_index, &category, &amount);
            start_question(app, category, amount, player);
        }
        else
        {
            append_logf(app, "! ", "P%d select ignored; no unused cells left", player + 1);
        }

        return;
    }

    if(action == PLAYER_ACTION_NEXT && !app->players[player].answered)
    {
        app->players[player].answer_index =
            (app->players[player].answer_index + 1) % OPTION_COUNT;
        append_logf(app, "+ ", "P%d answer -> %c",
                    player + 1,
                    'A' + app->players[player].answer_index);
        refresh_ui(app);
        update_lcd_for_player(app, player);
    }
    else if(action == PLAYER_ACTION_SELECT)
    {
        handle_answer_select(app, player);
    }
}

static void process_pic_line(AppData *app, const char *line)
{
    append_log(app, "< ", line);

    if((strcmp(line, "BTN:P1:NEXT") == 0) ||
       (strcmp(line, "BTN:P1:RIGHT") == 0) ||
       (strcmp(line, "BTN:P1:LEFT") == 0))
    {
        handle_player_action(app, 0, PLAYER_ACTION_NEXT);
    }
    else if(strcmp(line, "BTN:P1:SELECT") == 0)
    {
        handle_player_action(app, 0, PLAYER_ACTION_SELECT);
    }
    else if((strcmp(line, "BTN:P2:NEXT") == 0) ||
            (strcmp(line, "BTN:P2:RIGHT") == 0) ||
            (strcmp(line, "BTN:P2:LEFT") == 0))
    {
        handle_player_action(app, 1, PLAYER_ACTION_NEXT);
    }
    else if(strcmp(line, "BTN:P2:SELECT") == 0)
    {
        handle_player_action(app, 1, PLAYER_ACTION_SELECT);
    }
    else if(g_str_has_prefix(line, "BTN:"))
    {
        append_logf(app, "! ", "Button event not mapped: %s", line);
    }
}

static void process_serial_bytes(AppData *app, const char *buffer, size_t length)
{
    for(size_t i = 0; i < length; i++)
    {
        char c = buffer[i];

        if(c == '\r')
        {
            continue;
        }

        if(c == '\n')
        {
            app->serial_rx_buffer[app->serial_rx_length] = '\0';

            if(app->serial_rx_length > 0U)
            {
                process_pic_line(app, app->serial_rx_buffer);
            }

            app->serial_rx_length = 0U;
            continue;
        }

        if(app->serial_rx_length < (SERIAL_RX_BUFFER_SIZE - 1U))
        {
            app->serial_rx_buffer[app->serial_rx_length] = c;
            app->serial_rx_length++;
        }
        else
        {
            app->serial_rx_length = 0U;
            append_log(app, "! ", "Serial line too long; dropped");
        }
    }
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

    app->serial_rx_length = 0U;
    set_button_content(app->connect_button, "network-connect-symbolic", "Connect");
    gtk_widget_set_sensitive(GTK_WIDGET(app->device_combo), TRUE);
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
        set_status(app, request->path);
        append_log(app, "+ ", "Connected; polling for PIC responses every 80 ms");
        app->read_timer_id = g_timeout_add(80, poll_serial, app);
        sync_lcds_for_current_mode(app);
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

    bytes_read = pic_serial_read_available(app->serial_fd, buffer, sizeof(buffer), error, sizeof(error));
    if(bytes_read < 0)
    {
        append_log(app, "! ", error);
        app->read_timer_id = 0U;
        disconnect_device(app);
        return G_SOURCE_REMOVE;
    }

    if(bytes_read > 0)
    {
        append_logf(app, "i ", "Read %zd byte(s) from PIC", bytes_read);
        process_serial_bytes(app, buffer, (size_t)bytes_read);
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

static void on_refresh_clicked(GtkButton *button, gpointer user_data)
{
    AppData *app = user_data;
    (void)button;

    if(app->serial_fd < 0)
    {
        refresh_ports(app);
    }
}

static void on_category_changed(GtkEditable *editable, gpointer user_data)
{
    AppData *app = user_data;
    int category = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(editable), "category-index"));

    save_editor_to_selected(app);
    copy_text(app->categories[category],
              sizeof(app->categories[category]),
              gtk_entry_get_text(GTK_ENTRY(editable)));

    load_editor_from_selected(app);
    refresh_ui(app);
    if(app->mode == GAME_MODE_BOARD)
    {
        update_all_lcds(app);
    }
}

static void on_amount_changed(GtkSpinButton *spin_button, gpointer user_data)
{
    AppData *app = user_data;
    int amount = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(spin_button), "amount-index"));

    save_editor_to_selected(app);
    app->amounts[amount] = gtk_spin_button_get_value_as_int(spin_button);
    load_editor_from_selected(app);
    refresh_ui(app);
    if(app->mode == GAME_MODE_BOARD)
    {
        update_all_lcds(app);
    }
}

static void on_board_button_clicked(GtkButton *button, gpointer user_data)
{
    AppData *app = user_data;
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "cell-index"));

    save_editor_to_selected(app);
    cell_from_index(index, &app->selected_category, &app->selected_amount);
    load_editor_from_selected(app);
    refresh_ui(app);
}

static void on_save_clicked(GtkButton *button, gpointer user_data)
{
    AppData *app = user_data;
    (void)button;

    save_editor_to_selected(app);
    append_logf(app, "+ ", "Saved %s $%d",
                app->categories[app->selected_category],
                app->amounts[app->selected_amount]);
}

static void on_start_clicked(GtkButton *button, gpointer user_data)
{
    AppData *app = user_data;
    (void)button;

    if(app->mode == GAME_MODE_ANSWER)
    {
        finish_question_now(app);
        return;
    }

    start_question(app, app->selected_category, app->selected_amount, -1);
}

static void on_reset_clicked(GtkButton *button, gpointer user_data)
{
    AppData *app = user_data;
    (void)button;

    if(app->result_timeout_id != 0U)
    {
        g_source_remove(app->result_timeout_id);
        app->result_timeout_id = 0U;
    }

    save_editor_to_selected(app);

    for(int category = 0; category < CATEGORY_COUNT; category++)
    {
        for(int amount = 0; amount < AMOUNT_COUNT; amount++)
        {
            cell_at(app, category, amount)->used = false;
        }
    }

    for(int player = 0; player < PLAYER_COUNT; player++)
    {
        app->players[player].score = 0;
    }

    app->mode = GAME_MODE_BOARD;
    app->chooser_player = 0;
    reset_player_cursors(app);
    refresh_ui(app);
    update_all_lcds(app);
    append_log(app, "+ ", "Game reset");
}

static void on_test_action_clicked(GtkButton *button, gpointer user_data)
{
    AppData *app = user_data;
    int packed = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "test-action"));
    int player = packed / 2;
    PlayerAction action = (PlayerAction)(packed % 2);

    handle_player_action(app, player, action);
}

static GtkWidget *new_named_label(const char *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

static GtkWidget *new_tool_button(const char *icon_name, const char *label, const char *tooltip)
{
    GtkWidget *button = gtk_button_new();
    set_button_content(GTK_BUTTON(button), icon_name, label);
    gtk_widget_set_tooltip_text(button, tooltip);
    return button;
}

static void build_button_test_row(AppData *app, GtkWidget *parent, int player)
{
    GtkWidget *label;
    GtkWidget *next;
    GtkWidget *select;
    char player_text[8];

    snprintf(player_text, sizeof(player_text), "P%d", player + 1);
    label = gtk_label_new(player_text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(parent), label, 0, player, 1, 1);

    next = new_tool_button("go-next-symbolic", "Next", "Player next");
    select = new_tool_button("object-select-symbolic", "Select", "Player select");

    g_object_set_data(G_OBJECT(next), "test-action", GINT_TO_POINTER((player * 2) + PLAYER_ACTION_NEXT));
    g_object_set_data(G_OBJECT(select), "test-action", GINT_TO_POINTER((player * 2) + PLAYER_ACTION_SELECT));

    g_signal_connect(next, "clicked", G_CALLBACK(on_test_action_clicked), app);
    g_signal_connect(select, "clicked", G_CALLBACK(on_test_action_clicked), app);

    gtk_grid_attach(GTK_GRID(parent), next, 1, player, 1, 1);
    gtk_grid_attach(GTK_GRID(parent), select, 2, player, 1, 1);
}

static void build_ui(AppData *app)
{
    GtkWidget *main_box;
    GtkWidget *device_row;
    GtkWidget *device_label;
    GtkWidget *refresh_button;
    GtkWidget *content_box;
    GtkWidget *left_panel;
    GtkWidget *right_panel;
    GtkWidget *score_row;
    GtkWidget *board_grid;
    GtkWidget *editor_controls;
    GtkWidget *option_grid;
    GtkWidget *test_grid;
    GtkWidget *log_scroll;
    GtkWidget *question_scroll;

    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "Quest Game Controller");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1120, 760);
    gtk_container_set_border_width(GTK_CONTAINER(app->window), 14);

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

    content_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_box_pack_start(GTK_BOX(main_box), content_box, TRUE, TRUE, 0);

    left_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_hexpand(left_panel, TRUE);
    gtk_box_pack_start(GTK_BOX(content_box), left_panel, TRUE, TRUE, 0);

    right_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(right_panel, 360, -1);
    gtk_box_pack_start(GTK_BOX(content_box), right_panel, FALSE, TRUE, 0);

    score_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    app->score_labels[0] = GTK_LABEL(gtk_label_new("P1  $0"));
    app->score_labels[1] = GTK_LABEL(gtk_label_new("P2  $0"));
    app->mode_label = GTK_LABEL(gtk_label_new("P1 choose category"));
    gtk_box_pack_start(GTK_BOX(score_row), GTK_WIDGET(app->score_labels[0]), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(score_row), GTK_WIDGET(app->score_labels[1]), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(score_row), GTK_WIDGET(app->mode_label), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_panel), score_row, FALSE, FALSE, 0);

    board_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(board_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(board_grid), 6);
    gtk_box_pack_start(GTK_BOX(left_panel), board_grid, FALSE, FALSE, 0);

    for(int category = 0; category < CATEGORY_COUNT; category++)
    {
        app->category_entries[category] = GTK_ENTRY(gtk_entry_new());
        gtk_entry_set_width_chars(app->category_entries[category], 12);
        gtk_entry_set_text(app->category_entries[category], app->categories[category]);
        g_object_set_data(G_OBJECT(app->category_entries[category]), "category-index", GINT_TO_POINTER(category));
        g_signal_connect(app->category_entries[category], "changed", G_CALLBACK(on_category_changed), app);
        gtk_grid_attach(GTK_GRID(board_grid), GTK_WIDGET(app->category_entries[category]), category + 1, 0, 1, 1);
    }

    for(int amount = 0; amount < AMOUNT_COUNT; amount++)
    {
        app->amount_spins[amount] = GTK_SPIN_BUTTON(gtk_spin_button_new_with_range(0, 10000, 100));
        gtk_spin_button_set_value(app->amount_spins[amount], app->amounts[amount]);
        gtk_entry_set_width_chars(GTK_ENTRY(app->amount_spins[amount]), 5);
        g_object_set_data(G_OBJECT(app->amount_spins[amount]), "amount-index", GINT_TO_POINTER(amount));
        g_signal_connect(app->amount_spins[amount], "value-changed", G_CALLBACK(on_amount_changed), app);
        gtk_grid_attach(GTK_GRID(board_grid), GTK_WIDGET(app->amount_spins[amount]), 0, amount + 1, 1, 1);

        for(int category = 0; category < CATEGORY_COUNT; category++)
        {
            GtkWidget *button = gtk_button_new_with_label("");
            gtk_widget_set_size_request(button, 110, 48);
            app->board_buttons[category][amount] = GTK_BUTTON(button);
            g_object_set_data(G_OBJECT(button), "cell-index", GINT_TO_POINTER(cell_index(category, amount)));
            g_signal_connect(button, "clicked", G_CALLBACK(on_board_button_clicked), app);
            gtk_grid_attach(GTK_GRID(board_grid), button, category + 1, amount + 1, 1, 1);
        }
    }

    app->player_status_labels[0] = GTK_LABEL(gtk_label_new("P1 cursor"));
    app->player_status_labels[1] = GTK_LABEL(gtk_label_new("P2 cursor"));
    gtk_widget_set_halign(GTK_WIDGET(app->player_status_labels[0]), GTK_ALIGN_START);
    gtk_widget_set_halign(GTK_WIDGET(app->player_status_labels[1]), GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(left_panel), GTK_WIDGET(app->player_status_labels[0]), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left_panel), GTK_WIDGET(app->player_status_labels[1]), FALSE, FALSE, 0);

    test_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(test_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(test_grid), 6);
    build_button_test_row(app, test_grid, 0);
    build_button_test_row(app, test_grid, 1);
    gtk_box_pack_start(GTK_BOX(left_panel), test_grid, FALSE, FALSE, 0);

    app->editor_heading_label = GTK_LABEL(gtk_label_new(""));
    gtk_widget_set_halign(GTK_WIDGET(app->editor_heading_label), GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(right_panel), GTK_WIDGET(app->editor_heading_label), FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(right_panel), new_named_label("Question"), FALSE, FALSE, 0);
    question_scroll = new_scrolled_text_view(&app->question_view, &app->question_buffer, 120, true);
    gtk_box_pack_start(GTK_BOX(right_panel), question_scroll, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(right_panel), new_named_label("Options"), FALSE, FALSE, 0);
    option_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(option_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(option_grid), 6);

    for(int option = 0; option < OPTION_COUNT; option++)
    {
        char option_label[4];
        snprintf(option_label, sizeof(option_label), "%c", 'A' + option);
        gtk_grid_attach(GTK_GRID(option_grid), gtk_label_new(option_label), 0, option, 1, 1);
        app->option_entries[option] = GTK_ENTRY(gtk_entry_new());
        gtk_widget_set_hexpand(GTK_WIDGET(app->option_entries[option]), TRUE);
        gtk_grid_attach(GTK_GRID(option_grid), GTK_WIDGET(app->option_entries[option]), 1, option, 1, 1);
    }

    gtk_grid_attach(GTK_GRID(option_grid), gtk_label_new("Correct"), 0, OPTION_COUNT, 1, 1);
    app->correct_combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
    gtk_combo_box_text_append_text(app->correct_combo, "A");
    gtk_combo_box_text_append_text(app->correct_combo, "B");
    gtk_combo_box_text_append_text(app->correct_combo, "C");
    gtk_combo_box_text_append_text(app->correct_combo, "D");
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->correct_combo), 0);
    gtk_grid_attach(GTK_GRID(option_grid), GTK_WIDGET(app->correct_combo), 1, OPTION_COUNT, 1, 1);
    gtk_box_pack_start(GTK_BOX(right_panel), option_grid, FALSE, FALSE, 0);

    editor_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    app->save_button = GTK_BUTTON(new_tool_button("document-save-symbolic", "Save", "Save question"));
    app->start_button = GTK_BUTTON(new_tool_button("media-playback-start-symbolic", "Start", "Start selected question"));
    app->reset_button = GTK_BUTTON(new_tool_button("view-refresh-symbolic", "Reset", "Reset game"));
    g_signal_connect(app->save_button, "clicked", G_CALLBACK(on_save_clicked), app);
    g_signal_connect(app->start_button, "clicked", G_CALLBACK(on_start_clicked), app);
    g_signal_connect(app->reset_button, "clicked", G_CALLBACK(on_reset_clicked), app);
    gtk_box_pack_start(GTK_BOX(editor_controls), GTK_WIDGET(app->save_button), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(editor_controls), GTK_WIDGET(app->start_button), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(editor_controls), GTK_WIDGET(app->reset_button), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(right_panel), editor_controls, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(main_box), new_named_label("Device log"), FALSE, FALSE, 0);
    log_scroll = new_scrolled_text_view(&app->log_view, &app->log_buffer, 130, false);
    gtk_box_pack_start(GTK_BOX(main_box), log_scroll, FALSE, TRUE, 0);

    app->status_label = GTK_LABEL(gtk_label_new("Ready"));
    gtk_widget_set_halign(GTK_WIDGET(app->status_label), GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(main_box), GTK_WIDGET(app->status_label), FALSE, FALSE, 0);

    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    load_editor_from_selected(app);
    refresh_ui(app);
}

int main(int argc, char **argv)
{
    AppData app;

    memset(&app, 0, sizeof(app));
    app.serial_fd = -1;

    game_init_content(&app);

    gtk_init(&argc, &argv);
    build_ui(&app);
    refresh_ports(&app);

    gtk_widget_show_all(app.window);
    gtk_main();

    disconnect_device(&app);

    return 0;
}
