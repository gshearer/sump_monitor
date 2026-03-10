#include <stdio.h>
#include <stdlib.h>
#include <gpiod.h>

#define GPIO_CHIP "/dev/gpiochip0"
#define GPIO_PIN 17

int main(void) {
    // 1. Open the chip
    struct gpiod_chip *chip = gpiod_chip_open(GPIO_CHIP);
    if (!chip) {
        perror("Failed to open GPIO chip");
        return EXIT_FAILURE;
    }

    // 2. Configure Pin 17 (Input + Internal Pull-Up)
    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
    gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);

    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    unsigned int offset = GPIO_PIN;
    gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);

    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    gpiod_request_config_set_consumer(req_cfg, "pin_tester");

    // 3. Request the line
    struct gpiod_line_request *request = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    if (!request) {
        perror("Failed to request GPIO line");
        return EXIT_FAILURE;
    }

    // 4. Read the instantaneous value
    int val = gpiod_line_request_get_value(request, GPIO_PIN);
    
    if (val == 0) {
        printf("Result: LOW (0) -> WET / SHORTED\n");
    } else if (val == 1) {
        printf("Result: HIGH (1) -> DRY / OPEN\n");
    } else {
        printf("Error reading pin state.\n");
    }

    // 5. Cleanup
    gpiod_line_request_release(request);
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
    gpiod_chip_close(chip);

    return EXIT_SUCCESS;
}
