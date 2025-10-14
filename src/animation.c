/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_animation

#include <math.h>
#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk_driver_animation/animation.h>
#include <zmk_driver_animation/color.h>
#include <zmk_driver_animation/drivers/animation.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define PHANDLE_TO_DEVICE(node_id, prop, idx) \
    DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

#define PHANDLE_TO_PIXEL(node_id, prop, idx)                         \
    {                                                                \
        .position_x = DT_PHA_BY_IDX(node_id, prop, idx, position_x), \
        .position_y = DT_PHA_BY_IDX(node_id, prop, idx, position_y), \
    },

/**
 * LED Driver device pointers.
 */
static const struct device *drivers[] = {
    DT_INST_FOREACH_PROP_ELEM(0, drivers, PHANDLE_TO_DEVICE)};

/**
 * Size of the LED driver device pointers array.
 */
static const size_t drivers_size = DT_INST_PROP_LEN(0, drivers);

/**
 * Array containing the number of LEDs handled by each device.
 */
static const size_t pixels_per_driver[] = DT_INST_PROP(0, chain_lengths);

/**
 * Pointer to the root animation
 */
static const struct device *animation_root =
    DEVICE_DT_GET(DT_CHOSEN(zmk_animation));

/**
 * Pixel configuration.
 */
static struct animation_pixel pixels[] = {
    DT_INST_FOREACH_PROP_ELEM(0, pixels, PHANDLE_TO_PIXEL)};

/**
 * Size of the pixels array.
 */
static const size_t pixels_size = DT_INST_PROP_LEN(0, pixels);

/**
 * Buffer for RGB values ready to be sent to the drivers.
 */
static struct led_rgb px_buffer[DT_INST_PROP_LEN(0, pixels)];

/**
 * Counter for animation frames that have been requested but have yet to be
 * executed.
 */
static uint32_t animation_timer_countdown = 0;

/**
 * Conditional implementation of zmk_animation_get_pixel_by_key_position
 * if key-pixels is set.
 */
#if DT_INST_NODE_HAS_PROP(0, key_position)
static const uint8_t pixels_by_key_position[] = DT_INST_PROP(0, key_pixels);

size_t zmk_animation_get_pixel_by_key_position(size_t key_position) {
    return pixels_by_key_position[key_position];
}
#endif

#if defined(CONFIG_ZMK_ANIMATION_PIXEL_DISTANCE) && \
    (CONFIG_ZMK_ANIMATION_PIXEL_DISTANCE == 1)

/**
 * Lookup table for distance between any two pixels.
 *
 * The values are stored as a triangular matrix which cuts the space requirement
 * roughly in half.
 */
static uint8_t pixel_distance[((DT_INST_PROP_LEN(0, pixels) + 1) *
                               DT_INST_PROP_LEN(0, pixels)) /
                              2];

uint8_t zmk_animation_get_pixel_distance(size_t pixel_idx,
                                         size_t other_pixel_idx) {
    if (pixel_idx < other_pixel_idx) {
        return zmk_animation_get_pixel_distance(other_pixel_idx, pixel_idx);
    }

    return pixel_distance[(((pixel_idx + 1) * pixel_idx) >> 1) +
                          other_pixel_idx];
}

#endif

static void zmk_animation_tick(struct k_work *work) {
    animation_render_frame(animation_root, &pixels[0], pixels_size);

    for (size_t i = 0; i < pixels_size; ++i) {
        zmk_rgb_to_led_rgb(&pixels[i].value, &px_buffer[i]);

        // Reset values for the next cycle
        pixels[i].value.r = 0;
        pixels[i].value.g = 0;
        pixels[i].value.b = 0;
    }

    size_t pixels_updated = 0;

    for (size_t i = 0; i < drivers_size; ++i) {
        led_strip_update_rgb(drivers[i], &px_buffer[pixels_updated],
                             pixels_per_driver[i]);

        pixels_updated += pixels_per_driver[i];
    }
}

K_WORK_DEFINE(animation_work, zmk_animation_tick);

static void zmk_animation_tick_handler(struct k_timer *timer) {
    if (--animation_timer_countdown == 0) {
        k_timer_stop(timer);
    }

    k_work_submit(&animation_work);
}

K_TIMER_DEFINE(animation_tick, zmk_animation_tick_handler, NULL);

void zmk_animation_request_frames(uint32_t frames) {
    if (frames <= animation_timer_countdown) {
        return;
    }

    if (animation_timer_countdown == 0) {
        k_timer_start(&animation_tick, K_MSEC(1000 / CONFIG_ZMK_ANIMATION_FPS),
                      K_MSEC(1000 / CONFIG_ZMK_ANIMATION_FPS));
    }

    animation_timer_countdown = frames;
}

void zmk_animation_request_frames_if_required(uint32_t decrenetal_counter,
                                              bool initial) {
    if (initial) {
        zmk_animation_request_frames(decrenetal_counter >
                                             CONFIG_ZMK_ANIMATION_FPS
                                         ? CONFIG_ZMK_ANIMATION_FPS
                                         : decrenetal_counter);
    } else if (decrenetal_counter % CONFIG_ZMK_ANIMATION_FPS == 0) {
        zmk_animation_request_frames(CONFIG_ZMK_ANIMATION_FPS);
    }
}

static int zmk_animation_on_activity_state_changed(const zmk_event_t *event) {
    const struct zmk_activity_state_changed *activity_state_event;

    if ((activity_state_event = as_zmk_activity_state_changed(event)) == NULL) {
        // Event not supported.
        return -ENOTSUP;
    }

    switch (activity_state_event->state) {
        case ZMK_ACTIVITY_ACTIVE:
            animation_start(animation_root, ANIMATION_DURATION_FOREVER);
            return 0;
#if defined(CONFIG_ZMK_ANIMATION_STOP_ON_IDLE) && \
    (CONFIG_ZMK_ANIMATION_STOP_ON_IDLE == 1)
        case ZMK_ACTIVITY_IDLE:
#endif
        case ZMK_ACTIVITY_SLEEP:
            animation_stop(animation_root);
            k_timer_stop(&animation_tick);
            animation_timer_countdown = 0;
            return 0;
        default:
            return 0;
    }
}

static int zmk_animation_init(const struct device *dev) {
#if defined(CONFIG_ZMK_ANIMATION_PIXEL_DISTANCE) && \
    (CONFIG_ZMK_ANIMATION_PIXEL_DISTANCE == 1)
    // Prefill the pixel distance lookup table
    int k = 0;
    for (size_t i = 0; i < pixels_size; ++i) {
        for (size_t j = 0; j <= i; ++j) {
            // Distances are normalized to fit inside 0-255 range to fit inside
            // uint8_t for better space efficiency
            pixel_distance[k++] =
                sqrt(pow(pixels[i].position_x - pixels[j].position_x, 2) +
                     pow(pixels[i].position_y - pixels[j].position_y, 2)) *
                255 / 360;
        }
    }
#endif

    LOG_INF("ZMK Animation Ready");
    animation_start(animation_root, ANIMATION_DURATION_FOREVER);

    return 0;
}

SYS_INIT(zmk_animation_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

ZMK_LISTENER(amk_animation, zmk_animation_on_activity_state_changed);
ZMK_SUBSCRIPTION(amk_animation, zmk_activity_state_changed);
