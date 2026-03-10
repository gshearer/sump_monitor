#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <errno.h>
#include <gpiod.h>

#define SOCKET_PATH "/tmp/sump_monitor.sock"
#define POLL_INTERVAL_SECONDS 1
#define STATE_THRESHOLD_COUNT 10 
#define GPIO_CHIP "/dev/gpiochip0" 
#define GPIO_PIN 17

int current_state = 0; 
int event_counter = 0; 
int consecutive_wet_reads = 0;
int consecutive_dry_reads = 0;
const char *notify_script_path = NULL;

// Global flag for the signal handler
volatile sig_atomic_t keep_running = 1;

void handle_signal(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        keep_running = 0;
    }
}

void trigger_notification(const char *state_str) {
    printf("STATE TRANSITION CONFIRMED: %s! Executing %s %s\n", state_str, notify_script_path, state_str);
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s %s &", notify_script_path, state_str);
    system(cmd);
}

int main(void) {
    int server_sock, client_sock;
    struct sockaddr_un server_addr;
    struct pollfd fds[1]; 
    time_t last_check_time = 0;

    // 1. Setup Signal Handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    notify_script_path = getenv("SUMP_NOTIFY_SCRIPT");
    if (notify_script_path == NULL) {
        notify_script_path = "/usr/local/bin/sump_notify.sh";
    }

    // 2. libgpiod v2 Setup
    struct gpiod_chip *chip = gpiod_chip_open(GPIO_CHIP);
    if (!chip) {
        perror("Failed to open GPIO chip");
        return EXIT_FAILURE;
    }

    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);

    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    unsigned int offset = GPIO_PIN;
    gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);

    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(req_cfg, "sump_monitord");

    struct gpiod_line_request *request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    if (!request) {
        perror("Failed to request GPIO line");
        return EXIT_FAILURE;
    }

    // 3. Setup Unix Domain Socket
    server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    unlink(SOCKET_PATH); 
    memset(&server_addr, 0, sizeof(struct sockaddr_un));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, SOCKET_PATH, sizeof(server_addr.sun_path) - 1);

    bind(server_sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_un));
    listen(server_sock, 5);
    chmod(SOCKET_PATH, 0666); 

    fds[0].fd = server_sock;
    fds[0].events = POLLIN;     

    printf("Daemon active. Polling every %d sec. Socket: %s\n", POLL_INTERVAL_SECONDS, SOCKET_PATH);

    // 4. Main Event Loop
    while (keep_running) {
        int ret = poll(fds, 1, 1000); 

        // Handle signal interrupt during poll
        if (ret < 0) {
            if (errno == EINTR) {
                continue; // Signal caught, loop will evaluate keep_running and exit
            }
            perror("Poll error");
            break;
        }

        if (ret > 0 && (fds[0].revents & POLLIN)) {
            client_sock = accept(server_sock, NULL, NULL);
            if (client_sock >= 0) {
                char response[32];
                snprintf(response, sizeof(response), "%d\n%d\n", current_state, event_counter);
                write(client_sock, response, strlen(response));
                close(client_sock);
            }
        }

        time_t now = time(NULL);
        if (now - last_check_time >= POLL_INTERVAL_SECONDS) {
            
            int val = gpiod_line_request_get_value(request, GPIO_PIN);
            
            if (val == 0) {
                consecutive_wet_reads++;
                consecutive_dry_reads = 0;

                if (current_state == 0 && consecutive_wet_reads >= STATE_THRESHOLD_COUNT) {
                    current_state = 1;
                    event_counter++; 
                    trigger_notification("WET");
                }
            } else {
                consecutive_dry_reads++;
                consecutive_wet_reads = 0;

                if (current_state == 1 && consecutive_dry_reads >= STATE_THRESHOLD_COUNT) {
                    current_state = 0;
                    trigger_notification("DRY");
                }
            }
            
            last_check_time = now;
        }
    }

    // 5. Graceful Cleanup
    printf("\nShutting down gracefully. Releasing hardware and sockets...\n");
    gpiod_line_request_release(request);
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
    gpiod_chip_close(chip);
    close(server_sock);
    unlink(SOCKET_PATH);
    
    return EXIT_SUCCESS;
}
