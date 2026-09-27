#include <gtk/gtk.h>
#include <glib-unix.h>  // For g_unix_fd_add
#include <stdlib.h>
#include <string.h>
#include <unistd.h>     // For fork, pipe, dup2, execvp
#include <pthread.h>    // For pthreads
#include <sys/wait.h>   // For waitpid
#include <errno.h>      // For errno
#include <time.h>       // For ctime
#include <signal.h>     // For kill, SIGINT
#include <stdio.h>      // For fprintf, perror
#include <fcntl.h>      // For fcntl, O_NONBLOCK
#include <stdarg.h>
#include <glib/gprintf.h>

// --- Constants ---

#define MAX_COMMANDS 10     // Max commands in parallel
#define MAX_ARGS 20         // Max arguments per command
#define MAX_BUFFER 1024     // Read buffer size
#define DEFAULT_FONT_SIZE 11
#define MIN_FONT_SIZE 6
#define MAX_FONT_SIZE 30

// --- Global Data Structures ---

// Pipe for worker threads to report job completion
int job_completion_pipe[2];
int next_job_id = 1;

// Info for a single command to be run in a thread
typedef struct {
    char *args[MAX_ARGS]; // Parsed command and arguments
    int stdout_pipe[2];   // Pipe for stdout [0]=read, [1]=write
    int stderr_pipe[2];   // Pipe for stderr
    int job_id;
    int completion_pipe_fd; // Write end of job_completion_pipe
} CommandInfo;

// Job status used by the real-time monitoring feature.
typedef enum {
    JOB_RUNNING,
    JOB_COMPLETED,
    JOB_FAILED,
    JOB_TERMINATED
} JobStatus;

// Events sent from worker threads to the GTK main thread.
typedef enum {
    JOB_EVENT_STARTED,
    JOB_EVENT_FINISHED
} JobEventType;

typedef struct {
    JobEventType type;
    int job_id;
    pid_t pid;
    JobStatus status;
    double elapsed_seconds;
    int exit_code;
} JobEvent;

// Thread-safe list of active jobs (PID + Job ID + Status + start time)
typedef struct {
    int job_id;
    pid_t pid;
    JobStatus status;
    struct timespec start_time;
} ActiveJob;

ActiveJob active_job_list[MAX_COMMANDS];
int active_job_count = 0;
pthread_mutex_t job_list_mutex = PTHREAD_MUTEX_INITIALIZER;


// Structure to hold all our application's widgets and data
typedef struct {
    GtkApplication *app;
    GtkWindow      *main_window;
    GtkTextBuffer  *output_buffer;
    GtkTextView    *output_view;
    GtkEntry       *command_entry;
    GtkScrolledWindow *scrolled_window;
    gint           current_font_size_pt; // v10.0: For zoom
    GtkCssProvider *font_provider;       // v10.1: For dynamic zoom CSS
} AppData;


// Structure to pass data to g_unix_fd_add for command pipes
typedef struct {
    int fd;
    int job_id;
    gboolean is_stderr;
    AppData *data;
} PipeWatchData;


// --- Forward Declarations ---

// Core Logic
void *run_command_thread(void *arg);
void parse_and_launch_jobs(char *command_line, AppData *data);
char** parse_single_command(char *command_str);

// UI/GTK Helpers
void print_to_output(AppData *data, const char *tag_name, const char *format, ...);
void setup_text_tags(GtkTextBuffer *buffer);
void load_css(void);
gboolean on_pipe_readable(gint fd, GIOCondition condition, gpointer user_data);
gboolean on_job_completed(gint fd, GIOCondition condition, gpointer user_data);
void add_job_to_list(int job_id, pid_t pid);
void remove_job_from_list(int job_id);
void send_interrupt_to_jobs(AppData *data);
void update_text_view_font_size(AppData *data); // v10.0: New zoom helper
const char *job_status_to_string(JobStatus status);
gboolean kill_job_by_id(int job_id, AppData *data);

// GTK Signal Callbacks
static void on_command_activate(GtkEntry *entry, gpointer user_data);
static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data);
static void activate (GtkApplication* app, gpointer user_data);


// --- Main Function ---

int main(int argc, char **argv) {
    AppData *data = g_new0(AppData, 1);
    data->current_font_size_pt = DEFAULT_FONT_SIZE; // v10.0: Init font size
    data->font_provider = NULL; // v10.1: Init to NULL
    
    // Create the job completion pipe
    if (pipe(job_completion_pipe) < 0) {
        perror("Failed to create job completion pipe");
        return 1;
    }
    // Set the read end to non-blocking
    fcntl(job_completion_pipe[0], F_SETFL, O_NONBLOCK);
    
    // We ignore SIGINT in the main thread (same as ncurses)
    signal(SIGINT, SIG_IGN);

    GtkApplication *app = gtk_application_new ("com.example.gtkshell", G_APPLICATION_FLAGS_NONE);
    data->app = app;
    g_signal_connect (app, "activate", G_CALLBACK (activate), data);
    
    int status = g_application_run (G_APPLICATION (app), argc, argv);
    
    g_object_unref (app);
    close(job_completion_pipe[0]);
    close(job_completion_pipe[1]);
    
    // v10.1: Clean up the dynamic font provider
    if (data->font_provider) {
        g_object_unref(data->font_provider);
    }
    
    g_free(data);
    pthread_mutex_destroy(&job_list_mutex);

    return status;
}

// --- GTK UI Setup ---

/**
 * @brief Loads custom CSS to style the widgets.
 * *** v10.0: Added border to scrolledwindow ***
 */
void load_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css = 
        // --- Hacker Theme ---
        // Dark, slightly desaturated background for the window and headerbar
        "window, headerbar {"
        "    background-color: #2a2a2a;" // Very dark grey
        "    color: #00FF00;" // Neon green for title
        "}"
        // Make text view background match window
        "textview {"
        "    background-color: @theme_bg_color;"
        "    font-family: Monospace;"
        // "    font-size: 11pt;" // v10.1: Removed, now handled dynamically
        "    color: @theme_fg_color;" // Default text: Neon green
        "}"
        // Style the command entry
        "entry {"
        "    font-family: Monospace;"
        "    font-size: 11pt;"
        "    padding: 4px;"
        "    background-color: #2a2a2a;" // Slightly lighter dark grey for contrast
        "    border: 1px solid #00AA00;" // Darker green border
        "    color: #00FF00;" // Neon green text
        "    caret-color: #00FF00;" // Green cursor
        "}"
        // v10.0: Add border to the output area
        "scrolledwindow {"
        "    border: 1px solid #00AA00;"
        "    border-radius: 3px;"
        "}"
        // Scrollbar styling (optional, but nice for theme)
        "scrollbar trough {"
        "    background-color: #2a2a2a;"
        "}"
        "scrollbar slider {"
        "    background-color: #00AA00;"
        "    border-radius: 3px;"
        "}";
        
    // In GTK 3, this is how you load CSS data
    GError *error = NULL;
    gtk_css_provider_load_from_data(provider, css, -1, &error);
    if (error) {
        g_warning("Failed to load CSS: %s", error->message);
        g_error_free(error);
        g_object_unref(provider);
        return;
    }

    // Apply the provider to the whole application
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );
    g_object_unref(provider);
}


/**
 * @brief Creates all GTK widgets and sets up the UI.
 * This is called when the GtkApplication is activated.
 */
static void activate (GtkApplication* app, gpointer user_data) {
    AppData *data = (AppData *)user_data;
    
    // --- Load CSS ---
    // (We load CSS before widgets to ensure styles are ready)
    load_css();

    // --- Create Widgets ---
    data->main_window = GTK_WINDOW(gtk_application_window_new(app));
    data->command_entry = GTK_ENTRY(gtk_entry_new());
    data->output_view = GTK_TEXT_VIEW(gtk_text_view_new());
    data->output_buffer = gtk_text_view_get_buffer(data->output_view);
    data->scrolled_window = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new(NULL, NULL));
    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); // Spacing 0, use margins
    GtkWidget *title_bar = gtk_header_bar_new();

    // --- Configure Widgets ---
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(title_bar), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(title_bar), "Multithreaded Shell");
    gtk_window_set_titlebar(data->main_window, title_bar);
    
    gtk_window_set_title(data->main_window, "Multi Thread Shell ");
    gtk_window_set_default_size(data->main_window, 800, 600);

    gtk_text_view_set_editable(data->output_view, FALSE);
    gtk_text_view_set_cursor_visible(data->output_view, FALSE);
    gtk_text_view_set_wrap_mode(data->output_view, GTK_WRAP_WORD_CHAR);
    // Add padding inside the text view
    gtk_text_view_set_left_margin(data->output_view, 10);
    gtk_text_view_set_right_margin(data->output_view, 10);
    gtk_text_view_set_top_margin(data->output_view, 10);
    gtk_text_view_set_bottom_margin(data->output_view, 10);

    // v10.1: Create and attach a dynamic CSS provider for font size
    data->font_provider = gtk_css_provider_new();
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(GTK_WIDGET(data->output_view)),
        GTK_STYLE_PROVIDER(data->font_provider),
        GTK_STYLE_PROVIDER_PRIORITY_USER
    );

    // v10.0: Set initial font size programmatically
    update_text_view_font_size(data);

    gtk_entry_set_placeholder_text(data->command_entry, "Enter command (use '&&' for parallel) or 'exit'");

    setup_text_tags(data->output_buffer);

    // --- Layout ---
    gtk_container_add(GTK_CONTAINER(data->scrolled_window), GTK_WIDGET(data->output_view));
    gtk_widget_set_vexpand(GTK_WIDGET(data->scrolled_window), TRUE);
    // Add margins *around* the scrolled window (for breathing room)
    gtk_widget_set_margin_top(GTK_WIDGET(data->scrolled_window), 5);
    gtk_widget_set_margin_start(GTK_WIDGET(data->scrolled_window), 5);
    gtk_widget_set_margin_end(GTK_WIDGET(data->scrolled_window), 5);

    // Add margins *around* the command entry
    gtk_widget_set_margin_start(GTK_WIDGET(data->command_entry), 5);
    gtk_widget_set_margin_end(GTK_WIDGET(data->command_entry), 5);
    gtk_widget_set_margin_bottom(GTK_WIDGET(data->command_entry), 5);

    gtk_box_pack_start(GTK_BOX(main_box), GTK_WIDGET(data->scrolled_window), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(main_box), GTK_WIDGET(data->command_entry), FALSE, FALSE, 5); // 5px padding below entry

    gtk_container_add(GTK_CONTAINER(data->main_window), main_box);

    // --- Connect Signals ---
    
    // 1. Enter key in command entry
    g_signal_connect(data->command_entry, "activate", G_CALLBACK(on_command_activate), data);
    
    // 2. Key press handler for main window (Ctrl+B, Ctrl++, Ctrl+-)
    g_signal_connect(data->main_window, "key-press-event", G_CALLBACK(on_key_press), data);

    // 3. Monitor the job completion pipe
    g_unix_fd_add(job_completion_pipe[0], G_IO_IN, on_job_completed, data);

    // --- Show ---
    gtk_widget_show_all(GTK_WIDGET(data->main_window));

    // Welcome messages
    print_to_output(data, "shell", "Welcome to the GTK Multithreaded Shell! (v10.1)\n");
    print_to_output(data, "shell", "Type commands, separate parallel jobs with '&&'.\n");
    print_to_output(data, "shell", "Press Ctrl+B to send SIGINT to all running jobs.\n");
    print_to_output(data, "shell", "Use 'kill <job_id>' to terminate one running job.\n");
    print_to_output(data, "shell", "Use Ctrl+'+' and Ctrl+'-' to change font size.\n");
    print_to_output(data, "shell", "Type 'exit' to quit.\n");
}


// --- GTK Signal Callbacks ---

/**
 * @brief Called when Enter is pressed in the command entry.
 */
static void on_command_activate(GtkEntry *entry, gpointer user_data) {
    AppData *data = (AppData *)user_data;
    const gchar *command_line = gtk_entry_get_text(entry);

    if (g_str_equal(command_line, "")) {
        return; // Do nothing on empty command
    }

    // Print the command to the output
    print_to_output(data, "prompt", "\n> %s\n", command_line);

    if (g_str_equal(command_line, "exit")) {
        g_application_quit(G_APPLICATION(data->app));
        return;
    }

    // Need to pass a copy, as command_line will be freed
    char *cmd_line_copy = g_strdup(command_line);
    parse_and_launch_jobs(cmd_line_copy, data);
    g_free(cmd_line_copy); // Free the copy

    // Clear the entry
    gtk_entry_set_text(entry, "");
}

/**
 * @brief Called on any key press in the main window.
 * Used to catch Ctrl+B (interrupt) and Ctrl +/- (zoom).
 * v10.0: Added zoom controls.
 */
static gboolean on_key_press(GtkWidget *widget, GdkEventKey *event, gpointer user_data) {
    AppData *data = (AppData *)user_data;

    // Check for Ctrl mask
    if (event->state & GDK_CONTROL_MASK) {
        
        // Check for Ctrl+B
        if (event->keyval == GDK_KEY_b) {
            send_interrupt_to_jobs(data);
            return TRUE; // We handled this keypress
        }
        
        // Check for Ctrl + '+' (or Ctrl + '=')
        if (event->keyval == GDK_KEY_plus || event->keyval == GDK_KEY_equal) {
            if (data->current_font_size_pt < MAX_FONT_SIZE) {
                data->current_font_size_pt++;
                update_text_view_font_size(data);
            }
            return TRUE; // We handled this keypress
        }
        
        // Check for Ctrl + '-'
        if (event->keyval == GDK_KEY_minus) {
            if (data->current_font_size_pt > MIN_FONT_SIZE) {
                data->current_font_size_pt--;
                update_text_view_font_size(data);
            }
            return TRUE; // We handled this keypress
        }
    }
    
    return FALSE; // Propagate other keypresses
}


// --- GTK I/O Callbacks ---

/**
 * @brief Callback for g_unix_fd_add on a command's stdout/stderr pipe.
 * This runs in the main thread when the pipe has data.
 * *** v9.5: Added sanitization for carriage returns (\r) ***
 */
gboolean on_pipe_readable(gint fd, GIOCondition condition, gpointer user_data) {
    PipeWatchData *watch_data = (PipeWatchData *)user_data;
    AppData *data = watch_data->data;
    char buffer[MAX_BUFFER];

    ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);

    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';

        // *** NEW: Sanitize buffer, remove \r ***
        char *p_read = buffer;
        char *p_write = buffer;
        while (*p_read) {
            if (*p_read != '\r') {
                *p_write++ = *p_read;
            }
            p_read++;
        }
        *p_write = '\0';
        // ***************************************
        
        const char* type = watch_data->is_stderr ? "stderr" : "stdout";
        print_to_output(data, type, "[Job %d %s]: %s", watch_data->job_id, type, buffer);
        
        return TRUE; // Continue watching
    } else {
        // bytes_read == 0 (EOF) or bytes_read < 0 (Error)
        close(fd);
        g_free(watch_data);
        return FALSE; // Stop watching this fd
    }
}

/**
 * @brief Callback for g_unix_fd_add on the job_completion_pipe.
 * This runs in the main thread when a job finishes.
 */
gboolean on_job_completed(gint fd, GIOCondition condition, gpointer user_data) {
    AppData *data = (AppData *)user_data;
    JobEvent event;

    while (1) {
        ssize_t bytes_read = read(fd, &event, sizeof(event));

        if (bytes_read == (ssize_t)sizeof(event)) {
            if (event.type == JOB_EVENT_STARTED) {
                print_to_output(data, "shell",
                                "[Job %d] RUNNING | PID: %d\n",
                                event.job_id, (int)event.pid);
            } else if (event.type == JOB_EVENT_FINISHED) {
                print_to_output(data, "shell",
                                "[Job %d] %s | PID: %d | Execution Time: %.2f sec\n",
                                event.job_id,
                                job_status_to_string(event.status),
                                (int)event.pid,
                                event.elapsed_seconds);

                remove_job_from_list(event.job_id);
            }
        } else {
            break;
        }
    }

    return TRUE; // Continue watching
}


// --- Core Logic (Threads & Parsing) ---

/**
 * @brief Worker thread function to run a single command.
 * (Largely unchanged from ncurses version)
 */
void *run_command_thread(void *arg) {
    CommandInfo *job = (CommandInfo *)arg;
    struct timespec start_time, end_time;

    clock_gettime(CLOCK_MONOTONIC, &start_time);

    pid_t pid = fork();

    if (pid < 0) {
        perror("fork failed");
        close(job->stdout_pipe[1]);
        close(job->stderr_pipe[1]);
        free(job);
        return NULL;
    }

    if (pid == 0) {
        // --- Child Process ---
        signal(SIGINT, SIG_DFL);

        dup2(job->stdout_pipe[1], STDOUT_FILENO);
        dup2(job->stderr_pipe[1], STDERR_FILENO);

        close(job->stdout_pipe[0]);
        close(job->stdout_pipe[1]);
        close(job->stderr_pipe[0]);
        close(job->stderr_pipe[1]);
        close(job_completion_pipe[0]);
        close(job->completion_pipe_fd);

        // Use stdbuf -oL to force line buffering
        char *stdbuf_args[MAX_ARGS + 3];
        stdbuf_args[0] = "stdbuf";
        stdbuf_args[1] = "-oL";

        int i = 0;
        while (job->args[i] != NULL && i < MAX_ARGS) {
            stdbuf_args[i + 2] = job->args[i];
            i++;
        }

        stdbuf_args[i + 2] = NULL;

        if (execvp(stdbuf_args[0], stdbuf_args) == -1) {
            fprintf(stderr,
                    "stdbuf failed (%s), falling back to unbuffered...\n",
                    strerror(errno));
            execvp(job->args[0], job->args);
        }

        fprintf(stderr, "execvp failed: %s\n", strerror(errno));
        exit(1);

    } else {
        // --- Parent Process (Worker Thread) ---
        close(job->stdout_pipe[1]);
        close(job->stderr_pipe[1]);

        add_job_to_list(job->job_id, pid);

        // Report that the job has started and provide its PID.
        JobEvent start_event;
        memset(&start_event, 0, sizeof(start_event));
        start_event.type = JOB_EVENT_STARTED;
        start_event.job_id = job->job_id;
        start_event.pid = pid;
        start_event.status = JOB_RUNNING;

        write(job->completion_pipe_fd, &start_event, sizeof(start_event));

        int status;
        waitpid(pid, &status, 0);

        clock_gettime(CLOCK_MONOTONIC, &end_time);

        double elapsed =
            (end_time.tv_sec - start_time.tv_sec) +
            (end_time.tv_nsec - start_time.tv_nsec) / 1000000000.0;

        // Determine final status. A job explicitly marked TERMINATED
        // remains TERMINATED even though waitpid reports a signal.
        JobStatus final_status = JOB_COMPLETED;

        pthread_mutex_lock(&job_list_mutex);

        for (int i = 0; i < active_job_count; i++) {
            if (active_job_list[i].job_id == job->job_id) {
                if (active_job_list[i].status == JOB_TERMINATED) {
                    final_status = JOB_TERMINATED;
                } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                    final_status = JOB_FAILED;
                } else if (WIFSIGNALED(status)) {
                    final_status = JOB_FAILED;
                }
                break;
            }
        }

        pthread_mutex_unlock(&job_list_mutex);

        JobEvent finish_event;
        memset(&finish_event, 0, sizeof(finish_event));
        finish_event.type = JOB_EVENT_FINISHED;
        finish_event.job_id = job->job_id;
        finish_event.pid = pid;
        finish_event.status = final_status;
        finish_event.elapsed_seconds = elapsed;
        finish_event.exit_code =
            WIFEXITED(status) ? WEXITSTATUS(status) : -1;

        write(job->completion_pipe_fd, &finish_event, sizeof(finish_event));

        for (int i = 0; job->args[i] != NULL; i++) {
            free(job->args[i]);
        }

        free(job);
    }

    pthread_exit(NULL);
}

/**
 * @brief Parses the main command line and launches jobs.
 * (Modified for GTK I/O)
 */
void parse_and_launch_jobs(char *command_line, AppData *data) {
    char *saveptr1;
    char *cmd_str = strtok_r(command_line, "&&", &saveptr1);
    
    while (cmd_str != NULL) {
        while (*cmd_str == ' ') cmd_str++;
        
        if (strlen(cmd_str) == 0) {
            cmd_str = strtok_r(NULL, "&&", &saveptr1);
            continue;
        }

        // Process-control command: kill <job_id>
        if (strncmp(cmd_str, "kill ", 5) == 0) {
            char *endptr = NULL;
            long requested_job_id = strtol(cmd_str + 5, &endptr, 10);

            while (endptr && (*endptr == ' ' || *endptr == '\t')) {
                endptr++;
            }

            if (cmd_str[5] == '\0' ||
                endptr == cmd_str + 5 ||
                (endptr && *endptr != '\0')) {
                print_to_output(data, "stderr", "Usage: kill <job_id>\n");
            } else if (requested_job_id <= 0 ||
                       requested_job_id > 2147483647L) {
                print_to_output(data, "stderr",
                                "Invalid job ID: %s\n", cmd_str + 5);
            } else {
                kill_job_by_id((int)requested_job_id, data);
            }

            cmd_str = strtok_r(NULL, "&&", &saveptr1);
            continue;
        }

        CommandInfo *job = (CommandInfo *)calloc(1, sizeof(CommandInfo));
        if (!job) {
            print_to_output(data, "stderr", "Failed to allocate memory for job\n");
            return;
        }
        
        job->job_id = next_job_id++;
        job->completion_pipe_fd = job_completion_pipe[1]; // Pass write end

        char *cmd_str_copy = strdup(cmd_str);
        char **args = parse_single_command(cmd_str_copy);
        free(cmd_str_copy);

        if (!args || args[0] == NULL) {
            print_to_output(data, "stderr", "Failed to parse command: %s\n", cmd_str);
            free(job);
            if(args) free(args);
            cmd_str = strtok_r(NULL, "&&", &saveptr1);
            continue;
        }
        memcpy(job->args, args, sizeof(char*) * MAX_ARGS);
        free(args);

        if (pipe(job->stdout_pipe) < 0 || pipe(job->stderr_pipe) < 0) {
            print_to_output(data, "stderr", "Failed to create pipes for Job %d\n", job->job_id);
            for (int i = 0; job->args[i] != NULL; i++) free(job->args[i]);
            free(job);
            return;
        }
        
        // --- GTK Integration ---
        // Set read ends to non-blocking
        fcntl(job->stdout_pipe[0], F_SETFL, O_NONBLOCK);
        fcntl(job->stderr_pipe[0], F_SETFL, O_NONBLOCK);
        
        // Create watch data for stdout
        PipeWatchData *stdout_watch = g_new(PipeWatchData, 1);
        stdout_watch->fd = job->stdout_pipe[0];
        stdout_watch->job_id = job->job_id;
        stdout_watch->is_stderr = FALSE;
        stdout_watch->data = data;
        g_unix_fd_add(stdout_watch->fd, G_IO_IN, on_pipe_readable, stdout_watch);

        // Create watch data for stderr
        PipeWatchData *stderr_watch = g_new(PipeWatchData, 1);
        stderr_watch->fd = job->stderr_pipe[0];
        stderr_watch->job_id = job->job_id;
        stderr_watch->is_stderr = TRUE;
        stderr_watch->data = data;
        g_unix_fd_add(stderr_watch->fd, G_IO_IN, on_pipe_readable, stderr_watch);
        // ---------------------

        pthread_t tid;
        if (pthread_create(&tid, NULL, run_command_thread, (void *)job) != 0) {
            print_to_output(data, "stderr", "Failed to create thread for Job %d\n", job->job_id);
            // v10.0: Fix FD leak in this rare error case
            close(job->stdout_pipe[0]);
            close(job->stdout_pipe[1]); // <-- Bug fix
            close(job->stderr_pipe[0]);
            close(job->stderr_pipe[1]); // <-- Bug fix
            
            for (int i = 0; job->args[i] != NULL; i++) free(job->args[i]);
            free(job);
            return;
        }
        
        pthread_detach(tid);
        print_to_output(data, "shell",
                        "[Job %d] LAUNCHED | Command: %s\n",
                        job->job_id, cmd_str);
        cmd_str = strtok_r(NULL, "&&", &saveptr1);
    }
}

/**
 * @brief Parses a single command string into an array of args.
 * (Unchanged from ncurses version)
 */
char** parse_single_command(char *command_str) {
    char **args = (char **)calloc(MAX_ARGS, sizeof(char *));
    if (!args) return NULL;

    char *saveptr2;
    char *token = strtok_r(command_str, " \t\n", &saveptr2);
    int i = 0;
    while (token != NULL && i < MAX_ARGS - 1) {
        args[i] = strdup(token);
        if (!args[i]) {
            for (int j = 0; j < i; j++) free(args[j]);
            free(args);
            return NULL;
        }
        i++;
        token = strtok_r(NULL, " \t\n", &saveptr2);
    }
    args[i] = NULL;
    return args;
}


// --- Utility Functions ---

/**
 * @brief Thread-safely adds a job to the global list.
 */
void add_job_to_list(int job_id, pid_t pid) {
    pthread_mutex_lock(&job_list_mutex);

    if (active_job_count < MAX_COMMANDS) {
        active_job_list[active_job_count].job_id = job_id;
        active_job_list[active_job_count].pid = pid;
        active_job_list[active_job_count].status = JOB_RUNNING;

        clock_gettime(CLOCK_MONOTONIC,
                      &active_job_list[active_job_count].start_time);

        active_job_count++;
    }

    pthread_mutex_unlock(&job_list_mutex);
}

const char *job_status_to_string(JobStatus status) {
    switch (status) {
        case JOB_RUNNING:
            return "RUNNING";
        case JOB_COMPLETED:
            return "COMPLETED";
        case JOB_FAILED:
            return "FAILED";
        case JOB_TERMINATED:
            return "TERMINATED";
        default:
            return "UNKNOWN";
    }
}

/**
 * @brief Terminates one active job using its Job ID.
 */
gboolean kill_job_by_id(int job_id, AppData *data) {
    pid_t pid = -1;

    pthread_mutex_lock(&job_list_mutex);

    for (int i = 0; i < active_job_count; i++) {
        if (active_job_list[i].job_id == job_id) {
            pid = active_job_list[i].pid;
            active_job_list[i].status = JOB_TERMINATED;
            break;
        }
    }

    pthread_mutex_unlock(&job_list_mutex);

    if (pid == -1) {
        print_to_output(data, "stderr",
                        "[Job %d] No such active job.\n", job_id);
        return FALSE;
    }

    if (kill(pid, SIGINT) == 0) {
        print_to_output(data, "shell",
                        "[Job %d] TERMINATING | PID: %d\n",
                        job_id, (int)pid);
        return TRUE;
    }

    print_to_output(data, "stderr",
                    "[Job %d] Failed to send SIGINT to PID %d: %s\n",
                    job_id, (int)pid, strerror(errno));
    return FALSE;
}

void remove_job_from_list(int job_id) {
    pthread_mutex_lock(&job_list_mutex);

    int job_index = -1;

    for (int i = 0; i < active_job_count; i++) {
        if (active_job_list[i].job_id == job_id) {
            job_index = i;
            break;
        }
    }

    if (job_index != -1) {
        for (int i = job_index; i < active_job_count - 1; i++) {
            active_job_list[i] = active_job_list[i + 1];
        }
        active_job_count--;
    }

    pthread_mutex_unlock(&job_list_mutex);
}

void send_interrupt_to_jobs(AppData *data) {
    print_to_output(data, "shell",
                    "[Shell]: Sending SIGINT to all active jobs...\n");

    pthread_mutex_lock(&job_list_mutex);

    for (int i = 0; i < active_job_count; i++) {
        active_job_list[i].status = JOB_TERMINATED;
        kill(active_job_list[i].pid, SIGINT);
    }

    pthread_mutex_unlock(&job_list_mutex);
}

/**
 * @brief Creates the GtkTextTags for styling output.
 * *** v9.9: Unified Hacker Theme Colors ***
 */
void setup_text_tags(GtkTextBuffer *buffer) {
    // Timestamp - Bright neon green
    gtk_text_buffer_create_tag(buffer, "timestamp",
        "foreground", "#00FF00", NULL);
    // Shell message - Bright neon green
    gtk_text_buffer_create_tag(buffer, "shell",
        "foreground", "#00FF00", "weight", PANGO_WEIGHT_BOLD, NULL);
    // User prompt - Bright neon green
    gtk_text_buffer_create_tag(buffer, "prompt",
        "foreground", "#00FF00", "weight", PANGO_WEIGHT_BOLD, NULL);
    // stderr - A bright red for warnings/errors
    gtk_text_buffer_create_tag(buffer, "stderr",
        "foreground", "#FF0000", "weight", PANGO_WEIGHT_BOLD, NULL);
    // stdout - Default neon green
    gtk_text_buffer_create_tag(buffer, "stdout",
        "foreground", "#00FF00", NULL);
}

/**
 * @brief Safely prints formatted text to the GtkTextBuffer from the main thread.
 * *** BUG FIX v9.3: Insert timestamp and message separately ***
 */
void print_to_output(AppData *data, const char *tag_name, const char *format, ...) {
    va_list args;
    char *message;
    char *timestamp_str;
    GtkTextIter iter;

    // Format the message
    va_start(args, format);
    g_vasprintf(&message, format, args);
    va_end(args);

    // Get time
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_buf[10];
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", t);
    
    // Create timestamp string
    timestamp_str = g_strdup_printf("[%s] ", time_buf);

    // Get end of buffer
    gtk_text_buffer_get_end_iter(data->output_buffer, &iter);
    
    // Insert timestamp with "timestamp" tag
    gtk_text_buffer_insert_with_tags_by_name(data->output_buffer, &iter, 
        timestamp_str, -1, "timestamp", NULL);

    // Insert message with its own "tag_name"
    gtk_text_buffer_insert_with_tags_by_name(data->output_buffer, &iter, 
        message, -1, tag_name, NULL);

    // --- Auto-scroll ---
    // This is the GTK way to scroll to the end
    GtkTextMark *end_mark = gtk_text_buffer_get_insert(data->output_buffer);
    gtk_text_view_scroll_to_mark(data->output_view, end_mark, 0.0, TRUE, 0.5, 1.0);
    // -------------------

    g_free(message);
    g_free(timestamp_str);
}

/**
 * @brief v10.1: Helper to apply the current font size to the text view.
 * This uses a dynamic CSS provider to avoid deprecated functions.
 */
void update_text_view_font_size(AppData *data) {
    char *css_str = g_strdup_printf(
        "textview { font-size: %dpt; }", 
        data->current_font_size_pt
    );
    
    GError *error = NULL;
    // Load the CSS data into our dedicated provider
    gtk_css_provider_load_from_data(data->font_provider, css_str, -1, &error);
    
    if (error) {
        g_warning("Failed to load font size CSS: %s", error->message);
        g_error_free(error);
    }
    
    g_free(css_str);
}