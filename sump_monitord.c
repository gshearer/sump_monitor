#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <gpiod.h>

#define SOCKET_PATH "/tmp/sump_monitor.sock"
#define COOLDOWN_SECONDS 300 
#define GPIO_CHIP "/dev/gpiochip0" 
#define GPIO_PIN 17

int current_state = 0; // 0 = Dry (OK), 1 = Wet (ALARM)
time_t last_alarm_time = 0;
const char *notify_script_path = NULL;

void trigger_active_alarm() {
    time_t now = time(NULL);
    
    if (now - last_alarm_time >= COOLDOWN_SECONDS) {
        printf("CRITICAL: Water detected! Executing %s\n", notify_script_path);
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "%s &", notify_script_path);
        system(cmd);
        last_alarm_time = now;
    }
}

int main(void) {
    int server_sock, client_sock;
    struct sockaddr_un server_addr;
    struct pollfd fds[2]; 

    notify_script_path = getenv("SUMP_NOTIFY_SCRIPT");
    if (notify_script_path == NULL) {
        notify_script_path = "/usr/local/bin/sump_notify.sh";
    }

    // 1. libgpiod v2 Setup: Open the chip by path
    struct gpiod_chip *chip = gpiod_chip_open(GPIO_CHIP);
    if (!chip) {
        perror("Failed to open GPIO chip");
        return EXIT_FAILURE;
    }

    // 2. Build the hardware settings (Input, Both Edges, Pull-Up Resistor)
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_edge_detection(settings, GPIOD_LINE_EDGE_BOTH);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);

    // 3. Apply settings to our specific pin
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    unsigned int offset = GPIO_PIN;
    gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);

    // 4. Configure the consumer (how we appear in system logs)
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(req_cfg, "sump_monitord");

    // 5. Commit the request to the kernel
    struct gpiod_line_request *request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    if (!request) {
        perror("Failed to request GPIO line");
        return EXIT_FAILURE;
    }

    // Extract the file descriptor for poll()
    int gpio_fd = gpiod_line_request_get_fd(request);

    // Allocate a buffer to hold the edge events
    struct gpiod_edge_event_buffer *event_buffer = gpiod_edge_event_buffer_new(1);

    // 6. Setup Unix Domain Socket
    server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    unlink(SOCKET_PATH); 
    memset(&server_addr, 0, sizeof(struct sockaddr_un));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, SOCKET_PATH, sizeof(server_addr.sun_path) - 1);

    bind(server_sock, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_un));
    listen(server_sock, 5);

    // 7. Prepare poll() structure
    fds[0].fd = server_sock;
    fds[0].events = POLLIN;     
    fds[1].fd = gpio_fd;
    fds[1].events = POLLIN; 

    // Read initial state (0 means the pin is grounded/wet)
    int init_val = gpiod_line_request_get_value(request, GPIO_PIN);
    current_state = (init_val == 0) ? 1 : 0;

    printf("libgpiod v2 Daemon active. Listening on %s\n", SOCKET_PATH);

    // 8. Main Event Loop
    while (1) {
        int ret = poll(fds, 2, -1); 

        if (ret > 0) {
            // Event 1: Incoming Socket Connection
            if (fds[0].revents & POLLIN) {
                client_sock = accept(server_sock, NULL, NULL);
                if (client_sock >= 0) {
                    char response[8];
                    snprintf(response, sizeof(response), "%d\n", current_state);
                    write(client_sock, response, strlen(response));
                    close(client_sock);
                }
            }

            // Event 2: GPIO Edge Event
            if (fds[1].revents & POLLIN) {
                // Read the event into our buffer
                if (gpiod_line_request_read_edge_events(request, event_buffer, 1) > 0) {
                    struct gpiod_edge_event *event = gpiod_edge_event_buffer_get_event(event_buffer, 0);
                    int event_type = gpiod_edge_event_get_event_type(event);

                    if (event_type == GPIOD_EDGE_EVENT_FALLING_EDGE) {
                        // Voltage fell to 0 (Water bridged the pins)
                        if (current_state == 0) {
                            current_state = 1;
                            trigger_active_alarm();
                        }
                    } else if (event_type == GPIOD_EDGE_EVENT_RISING_EDGE) {
                        // Voltage returned to 3.3v (Water receded)
                        if (current_state == 1) {
                            printf("Water receded. State returned to Normal.\n");
                            current_state = 0;
                        }
                    }
                }
            }
        }
    }

    // Cleanup (Good C practice)
    gpiod_edge_event_buffer_free(event_buffer);
    gpiod_line_request_release(request);
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
    gpiod_chip_close(chip);
    close(server_sock);
    unlink(SOCKET_PATH);
    return EXIT_SUCCESS;
}
