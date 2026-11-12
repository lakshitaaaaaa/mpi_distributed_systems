#include <mpi.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>

enum {
    MESSAGE_SIZE = 256,
    HISTORY_LIMIT = 256,
    SEND_SLOTS = 16
};

static struct termios original_terminal;
static int terminal_changed = 0;

static void restore_terminal(void)
{
    if (terminal_changed) {
        tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal);
        printf("\033[?25h\033[0m\n");
        fflush(stdout);
        terminal_changed = 0;
    }
}

static void add_history(char history[][MESSAGE_SIZE + 16], int *count,
                        int pe, const char *message)
{
    if (*count == HISTORY_LIMIT) {
        memmove(history, history + 1,
                (HISTORY_LIMIT - 1) * sizeof(history[0]));
        --(*count);
    }
    snprintf(history[*count], sizeof(history[*count]), "PE %d: %s", pe, message);
    ++(*count);
}

static void draw_screen(char history[][MESSAGE_SIZE + 16], int history_count,
                        int rank, const char *input, size_t input_length)
{
    struct winsize window_size;
    int rows = 24;
    int columns = 80;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window_size) == 0 &&
        window_size.ws_row > 1 && window_size.ws_col > 0) {
        rows = window_size.ws_row;
        columns = window_size.ws_col;
    }

    printf("\033[2J\033[H");

    int visible_lines = rows - 1;
    int first_line = history_count > visible_lines
                         ? history_count - visible_lines
                         : 0;
    for (int i = first_line; i < history_count; ++i) {
        printf("%.*s\033[K\r\n", columns, history[i]);
    }

    int prompt_width = snprintf(NULL, 0, "PE %d> ", rank + 1);
    int available = columns - prompt_width;
    if (available < 0) {
        available = 0;
    }
    size_t input_start = input_length > (size_t)available
                             ? input_length - (size_t)available
                             : 0;

    printf("\033[%d;1H\033[KPE %d> ", rows, rank + 1);
    if (available > 0 && input_length > input_start) {
        printf("%.*s", available, input + input_start);
    }
    printf("\033[K\033[%d;%dH", rows, columns);
    fflush(stdout);
}

static int free_send_slot(int active[SEND_SLOTS])
{
    for (int i = 0; i < SEND_SLOTS; ++i) {
        if (!active[i]) {
            return i;
        }
    }
    return -1;
}

static void queue_send(const char *message, int slot,
                       char buffers[SEND_SLOTS][MESSAGE_SIZE],
                       MPI_Request requests[SEND_SLOTS], int active[SEND_SLOTS],
                       int peer)
{
    snprintf(buffers[slot], MESSAGE_SIZE, "%s", message);
    MPI_Isend(buffers[slot], MESSAGE_SIZE, MPI_CHAR, peer, 1,
              MPI_COMM_WORLD, &requests[slot]);
    active[slot] = 1;
}

int main(int argc, char **argv)
{
    int rank, process_count;
    char history[HISTORY_LIMIT][MESSAGE_SIZE + 16];
    int history_count = 0;
    char send_buffers[SEND_SLOTS][MESSAGE_SIZE];
    MPI_Request send_requests[SEND_SLOTS];
    int send_active[SEND_SLOTS] = {0};
    char input[MESSAGE_SIZE] = {0};
    size_t input_length = 0;
    int submit_pending = 0;
    int local_quit_sent = 0;
    int remote_quit_received = 0;
    int redraw = 1;

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &process_count);

    if (process_count != 2) {
        if (rank == 0) {
            fprintf(stderr, "Run this program with exactly two MPI processes.\n");
        }
        MPI_Finalize();
        return 1;
    }

    if (tcgetattr(STDIN_FILENO, &original_terminal) != 0) {
        fprintf(stderr, "PE %d needs an interactive terminal.\n", rank + 1);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    struct termios chat_terminal = original_terminal;
    chat_terminal.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    chat_terminal.c_cc[VMIN] = 1;
    chat_terminal.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &chat_terminal) != 0) {
        fprintf(stderr, "PE %d could not configure its terminal.\n", rank + 1);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    terminal_changed = 1;
    atexit(restore_terminal);
    printf("\033[?25l");

    while (1) {
        for (int i = 0; i < SEND_SLOTS; ++i) {
            if (send_active[i]) {
                int complete = 0;
                MPI_Test(&send_requests[i], &complete, MPI_STATUS_IGNORE);
                if (complete) {
                    send_active[i] = 0;
                }
            }
        }

        int incoming_ready = 0;
        MPI_Status status;
        MPI_Iprobe(1 - rank, 1, MPI_COMM_WORLD, &incoming_ready, &status);
        if (incoming_ready) {
            char incoming[MESSAGE_SIZE] = {0};
            MPI_Recv(incoming, MESSAGE_SIZE, MPI_CHAR, 1 - rank, 1,
                     MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            incoming[MESSAGE_SIZE - 1] = '\0';
            add_history(history, &history_count, 2 - rank, incoming);
            redraw = 1;

            if (strcmp(incoming, "/quit") == 0) {
                remote_quit_received = 1;
            }
        }

        int slot = free_send_slot(send_active);
        if (submit_pending && slot >= 0) {
            input[input_length] = '\0';
            add_history(history, &history_count, rank + 1, input);
            queue_send(input, slot, send_buffers, send_requests, send_active,
                       1 - rank);
            if (strcmp(input, "/quit") == 0) {
                local_quit_sent = 1;
            }
            input_length = 0;
            input[0] = '\0';
            submit_pending = 0;
            redraw = 1;
            slot = free_send_slot(send_active);
        }

        if (remote_quit_received && !local_quit_sent && slot >= 0) {
            add_history(history, &history_count, rank + 1, "/quit");
            queue_send("/quit", slot, send_buffers, send_requests, send_active,
                       1 - rank);
            local_quit_sent = 1;
            redraw = 1;
        }

        int sends_pending = 0;
        for (int i = 0; i < SEND_SLOTS; ++i) {
            sends_pending |= send_active[i];
        }
        if (local_quit_sent && remote_quit_received && !sends_pending &&
            !submit_pending) {
            break;
        }

        if (!local_quit_sent && !remote_quit_received && !submit_pending) {
            struct pollfd input_fd = { STDIN_FILENO, POLLIN, 0 };
            if (poll(&input_fd, 1, 20) > 0 &&
                (input_fd.revents & (POLLIN | POLLHUP))) {
                char keys[64];
                ssize_t bytes_read = read(STDIN_FILENO, keys, sizeof(keys));
                if (bytes_read <= 0) {
                    snprintf(input, sizeof(input), "/quit");
                    input_length = strlen(input);
                    submit_pending = 1;
                    redraw = 1;
                } else {
                    for (ssize_t i = 0; i < bytes_read; ++i) {
                        unsigned char key = (unsigned char)keys[i];
                        if (key == '\r' || key == '\n') {
                            submit_pending = 1;
                            redraw = 1;
                            break;
                        } else if (key == 8 || key == 127) {
                            if (input_length > 0) {
                                input[--input_length] = '\0';
                                redraw = 1;
                            }
                        } else if (key >= 32 && input_length < MESSAGE_SIZE - 1) {
                            input[input_length++] = (char)key;
                            input[input_length] = '\0';
                            redraw = 1;
                        }
                    }
                }
            }
        } else {
            poll(NULL, 0, 1);
        }

        if (redraw) {
            draw_screen(history, history_count, rank, input, input_length);
            redraw = 0;
        }
    }

    MPI_Finalize();
    restore_terminal();
    return 0;
}