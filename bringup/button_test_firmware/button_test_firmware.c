#include <stdio.h>
#include "pico/stdlib.h"

#define NUM_BUTTONS 7
#define DEBOUNCE_MS 20

/* GP0–GP6 match the prototype wiring: one side of each
   button to GND, the other side to these GPIO pins. */
static const uint button_pins[NUM_BUTTONS] = {0, 1, 2, 3, 4, 5, 6};

int main(void) {
    stdio_init_all();

    bool last_state[NUM_BUTTONS];

    for (int i = 0; i < NUM_BUTTONS; i++) {
        gpio_init(button_pins[i]);
        gpio_set_dir(button_pins[i], GPIO_IN);
        gpio_pull_up(button_pins[i]);
        last_state[i] = gpio_get(button_pins[i]);
    }

    printf("Ready. Press a button.\n");
    stdio_flush();

    while (true) {
        for (int i = 0; i < NUM_BUTTONS; i++) {
            bool current = gpio_get(button_pins[i]);

            /* Active-low: falling edge means the button was just pressed. */
            if (last_state[i] && !current) {
                sleep_ms(DEBOUNCE_MS);
                if (!gpio_get(button_pins[i])) {
                    printf("Button %d clicked!\n", i + 1);
                    stdio_flush();
                }
            }

            last_state[i] = gpio_get(button_pins[i]);
        }

        sleep_ms(5);
    }
}
