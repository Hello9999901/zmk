/*
 * Copyright (c) 2024 Byran Huang, Keychron
 *
 * SPDX-License-Identifier: MIT
 */

// Out-of-tree Keychron specific features
// Structure based on: https://github.com/caksoylar/zmk-rgbled-widget by Cem Aksoylar
// PRs used: #2036 by ReFil and #1757 by joelspadin and #1434 by xingrz and #1433 by xingrz

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/drivers/gpio.h>

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

// a info work item as specified by state (extendable)
struct info_item {
    uint16_t state;
};

bool previous_underglow_on_off;
int previous_underglow_state;
// latch structure to check if 3 keys are pressed (any order is allowed)
bool backlight_test_latch[] = {0, 0, 0};
bool reset_latch[] = {0, 0, 0};
bool will_reset = 0;
bool will_backlight_test = 0;
int backlight_cycle = 0;
bool testing_backlight = 0;

// define message queue of info work items, that will be processed by a separate thread
K_MSGQ_DEFINE(keychron_msgq, sizeof(struct info_item), 16, 4);

#if IS_ENABLED(CONFIG_ZMK_BLE)
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static int led_profile_listener_cb(const zmk_event_t *eh) {
    uint8_t profile_index = zmk_ble_active_profile_index();
    if (zmk_ble_active_profile_is_connected()) {
        LOG_INF("Profile %d connected", profile_index);
        if (previous_underglow_on_off == 0) {
            LOG_INF("Restored previous RGB On Off: %d", previous_underglow_on_off);
            zmk_rgb_underglow_select_effect(previous_underglow_state);
            zmk_rgb_underglow_off();
        } else {
            LOG_ERR("Restored previous RGB state: %d", previous_underglow_state);
            zmk_rgb_underglow_select_effect(previous_underglow_state);
            zmk_rgb_underglow_on();
        }
    } else if (zmk_ble_active_profile_is_open()) {
        LOG_INF("Profile %d open", profile_index);
        previous_underglow_state = zmk_rgb_underglow_calc_effect(0);
        zmk_rgb_underglow_get_state(&previous_underglow_on_off);
        LOG_INF("Previous RGB state: %d", previous_underglow_state);
        LOG_INF("Previous RGB On Off: %d", previous_underglow_on_off);
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

uint8_t central_battery_level;
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static int system_checks_cb(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    struct zmk_endpoint_instance endpoint_instance = zmk_endpoints_selected();
    enum zmk_transport selected_transport = endpoint_instance.transport;
    LOG_WRN("Current endpoint: %d", selected_transport);

    // reset checker
    if (will_reset == 1) {
        LOG_INF("Reset process terminated");
        struct info_item info = {.state = 0};
        k_msgq_put(&keychron_msgq, &info, K_NO_WAIT);
        will_reset = 0;
    }

    // backlight testing, red starts when hold time is triggered
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
            backlight_cycle++;
            if (backlight_cycle >= 5) {
                testing_backlight = 0;
                backlight_cycle = 0;
                zmk_rgb_underglow_reset();
                zmk_rgb_underglow_off();
            }
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
        struct info_item info = {.state = 1};
        k_msgq_put(&keychron_msgq, &info, K_NO_WAIT);
    }

    if (backlight_test_latch[0] == 1 && backlight_test_latch[1] == 1 &&
        backlight_test_latch[2] == 1) {
        will_reset = 1;
        LOG_INF("Backlight test %d %d %d", reset_latch[0], reset_latch[1], reset_latch[2]);
        if (testing_backlight == 0) {
            struct info_item info = {.state = 2};
            k_msgq_put(&keychron_msgq, &info, K_NO_WAIT);
        }
    }

    if (central_battery_level <= 8) {
        LOG_ERR("Central battery level %d, blinking red for critical", central_battery_level);
        zmk_rgb_underglow_on();
        zmk_rgb_underglow_select_effect(5);
    }

    return 0;
}

ZMK_LISTENER(key_press_listener, system_checks_cb);
ZMK_SUBSCRIPTION(key_press_listener, zmk_position_state_changed);

static int battery_listener_cb(const zmk_event_t *eh) {
    // check if we are in critical battery levels at state change, info if we are
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

// run battery_listener_cb on battery state change event
ZMK_LISTENER(battery_listener, battery_listener_cb);
ZMK_SUBSCRIPTION(battery_listener, zmk_battery_state_changed);
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

#endif // !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

uint8_t peripheral_battery_level;

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
static int battery_peripheral_listener_cb(const zmk_event_t *eh) {
    // check if we are in critical battery levels at state change, info if we are
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
    if (peripheral_battery_level <= 8) {
        LOG_ERR("Peripheral battery level %d, blinking red for critical", peripheral_battery_level);
        zmk_rgb_underglow_on();
        zmk_rgb_underglow_select_effect(5);
    }

    return 0;
}

ZMK_LISTENER(key_press_listener, system_checks_cb);
ZMK_LISTENER(battery_peripheral_listener, battery_peripheral_listener_cb);
ZMK_SUBSCRIPTION(battery_peripheral_listener, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(key_press_listener, zmk_position_state_changed);

#endif

extern void keychron_config_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    while (true) {
        // wait until a info item is received and process it
        struct info_item info;
        k_msgq_get(&keychron_msgq, &info, K_FOREVER);
        LOG_DBG("Got a info item from msgq, state %d", info.state);
        LOG_INF("Config thread 3 second check");
        k_msleep(3000);
        k_msgq_peek(&keychron_msgq, &info);
        LOG_DBG("Peekd a info item from msgq, state %d", info.state);
        if (info.state == 1) {
            LOG_WRN("Resetting, state: %d", info.state);
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
        if (info.state == 2) {
            LOG_WRN("Underglow debugging, state: %d", info.state);
            zmk_rgb_underglow_select_effect(0); // solid color
            zmk_rgb_underglow_change_sat(100);
            zmk_rgb_underglow_change_brt(100);
            zmk_rgb_underglow_set_hsb((struct zmk_led_hsb){.h = 120, .s = 100, .b = 100}); // red
            zmk_rgb_underglow_on();
            testing_backlight = 1;
        }
        k_msgq_purge(&keychron_msgq);
    }
}

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static int output_selection_cb(void) {
    k_msleep(100);
    const struct device *dev;
    dev = DEVICE_DT_GET(DT_NODELABEL(gpio1));
    int sel_pin_state = gpio_pin_get(dev, 10);
    LOG_INF("Set keychron sel pin %d", sel_pin_state);
    keychron_set_sel_pin(sel_pin_state);
    if (sel_pin_state) {
        return zmk_endpoints_select_transport(ZMK_TRANSPORT_BLE);
    } else {
        return zmk_endpoints_select_transport(ZMK_TRANSPORT_USB);
    }
}

ZMK_LISTENER(peripheral_connection_listener, output_selection_cb);
ZMK_SUBSCRIPTION(peripheral_connection_listener, zmk_split_peripheral_status_changed);

#endif

// initiate configuration thread with 500ms delay and 512 stack size
K_THREAD_DEFINE(keychron_tid, 512, keychron_config_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 500);
