#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/services/bas.h>

#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/keymap.h>
#include <zmk/battery.h>
#include <zmk/rgb_underglow.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/activity_state_changed.h>

#include <zmk/reset.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

enum color_t {
    LED_BLACK,   // 0b000
    LED_RED,     // 0b001
    LED_GREEN,   // 0b010
    LED_YELLOW,  // 0b011
    LED_BLUE,    // 0b100
    LED_MAGENTA, // 0b101
    LED_CYAN,    // 0b110
    LED_WHITE    // 0b111
};

// a blink work item as specified by the color and duration
struct blink_item {
    enum color_t color;
    uint16_t duration_ms;
    bool first_item;
    uint16_t sleep_ms;
};

bool previous_underglow_on_off;
int previous_underglow_state;
bool reset_latch[] = {0, 0, 0};
bool will_reset = 0;
bool backlight_test_latch[] = {0, 0, 0};
bool will_backlight_test = 0;
int backlight_cycle = 0;
bool testing_backlight = 0;

// define message queue of blink work items, that will be processed by a separate thread
K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 16, 4);

#if IS_ENABLED(CONFIG_ZMK_BLE)
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static int led_profile_listener_cb(const zmk_event_t *eh) {
    uint8_t profile_index = zmk_ble_active_profile_index();
    if (zmk_ble_active_profile_is_connected()) {
        LOG_INF("Profile %d connected, blinking blue", profile_index);
        if (previous_underglow_on_off == 0) {
            LOG_ERR("Restored previous RGB On Off: %d", previous_underglow_on_off);
            zmk_rgb_underglow_select_effect(previous_underglow_state);
            zmk_rgb_underglow_off();
        } else {
            LOG_ERR("Restored previous RGB state: %d", previous_underglow_state);
            zmk_rgb_underglow_select_effect(previous_underglow_state);
            zmk_rgb_underglow_on();
        }
    } else if (zmk_ble_active_profile_is_open()) {
        LOG_INF("Profile %d open, blinking yellow", profile_index);
        previous_underglow_state = zmk_rgb_underglow_calc_effect(0);
        zmk_rgb_underglow_get_state(&previous_underglow_on_off);
        LOG_ERR("Previous RGB state: %d", previous_underglow_state);
        LOG_ERR("Previous RGB On Off: %d", previous_underglow_on_off);
        zmk_rgb_underglow_off();
        zmk_rgb_underglow_set_profile_number(profile_index);
        zmk_rgb_underglow_select_effect(4);
        zmk_rgb_underglow_on();
    }
    return 0;
}

// run led_profile_listener_cb on BLE profile change (on central)
ZMK_LISTENER(led_profile_listener, led_profile_listener_cb);
ZMK_SUBSCRIPTION(led_profile_listener, zmk_ble_active_profile_changed);
#endif
#endif // IS_ENABLED(CONFIG_ZMK_BLE)

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
uint8_t central_battery_level;
static int led_battery_listener_cb(const zmk_event_t *eh) {
    // check if we are in critical battery levels at state change, blink if we are
    central_battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;
    LOG_INF("Central battery level callback %d percent", central_battery_level);

    if (central_battery_level <= 0) {
        // disconnect all BT devices, turns off underglow
        zmk_rgb_underglow_off();
        zmk_ble_prof_disconnect_all();
        LOG_ERR("Central critically low battery level %d, shutting off", central_battery_level);
        k_msleep(500);
        k_panic();
    }

    return 0;
}

static int system_checks_cb(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    LOG_INF("Custom listener: ZMK position state at %d is %d!", ev->position, ev->state);

    struct zmk_endpoint_instance endpoint_instance = zmk_endpoints_selected();
    enum zmk_transport selected_transport = endpoint_instance.transport;
    LOG_WRN("Current endpoint: %d", selected_transport);

    LOG_WRN("Current battery level: %d", central_battery_level);

    if (will_reset == 1) {
        LOG_INF("ENDED RESET");
        struct blink_item blink = {.duration_ms = 4321};
        k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
        will_reset = 0;
    }

    if (ev->position == 90 && ev->state == 1) {
        LOG_INF("right arrow pressed %d", ev->state);
        if (testing_backlight == 1) {
            if (backlight_cycle == 1) {
                zmk_rgb_underglow_set_hsb(
                    (struct zmk_led_hsb){.h = 240, .s = 100, .b = 100}); // green
            }
            if (backlight_cycle == 2) {
                zmk_rgb_underglow_set_hsb((struct zmk_led_hsb){.h = 0, .s = 100, .b = 100}); // blue
            }
            if (backlight_cycle == 3) {
                zmk_rgb_underglow_set_hsb((struct zmk_led_hsb){.h = 0, .s = 0, .b = 100}); // white
            }
        }
        backlight_cycle++;
        if (backlight_cycle >= 5) {
            testing_backlight = 0;
            backlight_cycle = 0;
            zmk_rgb_underglow_reset();
            zmk_rgb_underglow_off();
        }
    }

    // 86 = fn key on right, 57 = j, 66 = z for factory reset
    if (ev->position == 86 || ev->position == 57 || ev->position == 66) {
        if (ev->position == 86) {
            reset_latch[0] = ev->state;
        }
        if (ev->position == 57) {
            reset_latch[1] = ev->state;
        }
        if (ev->position == 66) {
            reset_latch[2] = ev->state;
        }
    }

    // 86 = fn key on right, 63 = home, 90 = right for backlight test
    if (ev->position == 86 || ev->position == 63 || ev->position == 90) {
        if (ev->position == 86) {
            backlight_test_latch[0] = ev->state;
        }
        if (ev->position == 63) {
            backlight_test_latch[1] = ev->state;
        }
        if (ev->position == 90) {
            backlight_test_latch[2] = ev->state;
        }
    }

    if (reset_latch[0] == 1 && reset_latch[1] == 1 && reset_latch[2] == 1) {
        will_reset = 1;
        LOG_INF("Factory reset %d %d %d", reset_latch[0], reset_latch[1], reset_latch[2]);
        struct blink_item blink = {.duration_ms = 1111};
        k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    }

    if (backlight_test_latch[0] == 1 && backlight_test_latch[1] == 1 &&
        backlight_test_latch[2] == 1) {
        will_reset = 1;
        LOG_INF("Backlight test %d %d %d", reset_latch[0], reset_latch[1], reset_latch[2]);
        if (testing_backlight == 0) {
            struct blink_item blink = {.duration_ms = 2222};
            k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
        }
    }

    if (central_battery_level <= 8) {
        LOG_ERR("Central battery level %d, blinking red for critical", central_battery_level);
        zmk_rgb_underglow_on();
        zmk_rgb_underglow_select_effect(5);
    }

    return 0;
}

// run led_battery_listener_cb on battery state change event
ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_LISTENER(key_press_listener, system_checks_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(key_press_listener, zmk_position_state_changed);

#else

uint8_t peripheral_battery_level;
static int led_battery_peripheral_listener_cb(const zmk_event_t *eh) {
    // check if we are in critical battery levels at state change, blink if we are
    peripheral_battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;
    LOG_INF("Peripheral battery level callback %d percent", peripheral_battery_level);

    if (peripheral_battery_level <= 0) {
        // disconnect all BT devices, turns off underglow
        zmk_rgb_underglow_off();
        LOG_ERR("Peripheral critically low battery level %d, shutting off",
                peripheral_battery_level);
        k_msleep(500);
        k_panic();
    }

    return 0;
}
static int system_checks_cb(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (peripheral_battery_level <= 8) {
        LOG_ERR("Peripheral battery level %d, blinking red for critical", peripheral_battery_level);
        zmk_rgb_underglow_on();
        zmk_rgb_underglow_select_effect(5);
    }

    return 0;
}

ZMK_LISTENER(key_press_listener, system_checks_cb);
ZMK_LISTENER(led_battery_peripheral_listener, led_battery_peripheral_listener_cb);
ZMK_SUBSCRIPTION(led_battery_peripheral_listener, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(key_press_listener, zmk_position_state_changed);

#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

#endif // !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

extern void led_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    while (true) {
        // wait until a blink item is received and process it
        struct blink_item blink;
        k_msgq_get(&led_msgq, &blink, K_FOREVER);
        LOG_DBG("Got a blink item from msgq, color %d, duration %d", blink.color,
                blink.duration_ms);
        LOG_INF("LED THREAD SLEEPING FOR 3 SECONDS");
        k_msleep(3000);
        k_msgq_peek(&led_msgq, &blink);
        LOG_DBG("PEEKED a link item from msgq, color %d, duration %d", blink.color,
                blink.duration_ms);
        if (blink.duration_ms == 1111) {
            LOG_WRN("WARNING: RESETTING %d", blink.duration_ms);
            // Preparing to reset animation
            zmk_rgb_underglow_on();
            zmk_rgb_underglow_select_effect(6);
            zmk_rgb_underglow_off();

            // Reset underglow configurations
            zmk_rgb_underglow_reset();

// Unpair and clear bluetooth bonds
#if ZMK_BLE_IS_CENTRAL
            zmk_ble_unpair_all();
#endif
            zmk_reset(ZMK_RESET_WARM);
        }
        if (blink.duration_ms == 2222) {
            LOG_WRN("WARNING: TESTING BACKLIGHT %d", blink.duration_ms);
            zmk_rgb_underglow_select_effect(0); // solid color
            zmk_rgb_underglow_change_sat(100);
            zmk_rgb_underglow_change_brt(100);
            zmk_rgb_underglow_set_hsb((struct zmk_led_hsb){.h = 120, .s = 100, .b = 100}); // red
            zmk_rgb_underglow_on();
            testing_backlight = 1;
        }
        k_msgq_purge(&led_msgq);
    }
}

// define led_thread with stack size 512, start running it 500 ms after boot
K_THREAD_DEFINE(led_tid, 512, led_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0,
                500);
