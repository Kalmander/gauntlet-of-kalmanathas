/*
 * Local sticky-layer copy for Gauntlet-specific cancel helpers and overlap-aware
 * release behavior. This lets repo-owned behaviors explicitly consume one-shot
 * NEXUS state without patching upstream ZMK sticky-key code, and avoids
 * latching the layer after chorded/rolled uses of the NEXUS thumb key.
 *
 * Original work Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_sticky_layer_local

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>

#include <zmk/matrix.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/modifiers_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>

#include "sticky_layer_local.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define KEY_PRESS DEVICE_DT_NAME(DT_INST(0, zmk_behavior_key_press))

#define ZMK_BHV_STICKY_LAYER_LOCAL_MAX_HELD CONFIG_ZMK_BEHAVIOR_STICKY_KEY_MAX_HELD
#define ZMK_BHV_STICKY_LAYER_LOCAL_POSITION_FREE UINT32_MAX

struct behavior_sticky_layer_local_config {
    uint32_t release_after_ms;
    bool quick_release;
    bool lazy;
    bool ignore_modifiers;
    struct zmk_behavior_binding behavior;
};

struct active_sticky_layer_local {
    uint32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    uint32_t param1;
    const struct behavior_sticky_layer_local_config *config;
    bool timer_started;
    bool timer_cancelled;
    int64_t release_at;
    struct k_work_delayable release_timer;
    uint8_t modified_key_usage_page;
    uint32_t modified_key_keycode;
    bool had_other_position_pressed_on_press;
};

static struct active_sticky_layer_local
    active_sticky_layers_local[ZMK_BHV_STICKY_LAYER_LOCAL_MAX_HELD] = {};
static bool pressed_positions[ZMK_KEYMAP_LEN] = {};

/* Treat chorded use as momentary: if another key is already down when the sticky
 * layer key is pressed, releasing it should not leave the layer latched on.
 */
static bool had_other_position_pressed(uint32_t self_position) {
    for (int i = 0; i < ZMK_KEYMAP_LEN; i++) {
        if ((uint32_t)i == self_position) {
            continue;
        }
        if (pressed_positions[i]) {
            return true;
        }
    }
    return false;
}

static struct active_sticky_layer_local *
store_sticky_layer_local(struct zmk_behavior_binding_event *event, uint32_t param1,
                         const struct behavior_sticky_layer_local_config *config) {
    for (int i = 0; i < ZMK_BHV_STICKY_LAYER_LOCAL_MAX_HELD; i++) {
        struct active_sticky_layer_local *const sticky_layer = &active_sticky_layers_local[i];
        if (sticky_layer->position != ZMK_BHV_STICKY_LAYER_LOCAL_POSITION_FREE ||
            sticky_layer->timer_cancelled) {
            continue;
        }
        sticky_layer->position = event->position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        sticky_layer->source = event->source;
#endif
        sticky_layer->param1 = param1;
        sticky_layer->config = config;
        sticky_layer->release_at = 0;
        sticky_layer->timer_cancelled = false;
        sticky_layer->timer_started = false;
        sticky_layer->modified_key_usage_page = 0;
        sticky_layer->modified_key_keycode = 0;
        sticky_layer->had_other_position_pressed_on_press = false;
        return sticky_layer;
    }
    return NULL;
}

static void clear_sticky_layer_local(struct active_sticky_layer_local *sticky_layer) {
    LOG_DBG("clearing local sticky layer at position %d, param %d", sticky_layer->position,
            sticky_layer->param1);
    sticky_layer->position = ZMK_BHV_STICKY_LAYER_LOCAL_POSITION_FREE;
}

static struct active_sticky_layer_local *
find_sticky_layer_local(uint32_t position, struct zmk_behavior_binding behavior,
                        uint32_t binding_param) {
    for (int i = 0; i < ZMK_BHV_STICKY_LAYER_LOCAL_MAX_HELD; i++) {
        if (active_sticky_layers_local[i].position == position &&
            active_sticky_layers_local[i].config->behavior.behavior_dev == behavior.behavior_dev &&
            active_sticky_layers_local[i].param1 == binding_param &&
            !active_sticky_layers_local[i].timer_cancelled) {
            return &active_sticky_layers_local[i];
        }
    }
    return NULL;
}

static inline int press_sticky_layer_local_behavior(struct active_sticky_layer_local *sticky_layer,
                                                    int64_t timestamp) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = sticky_layer->config->behavior.behavior_dev,
        .param1 = sticky_layer->param1,
    };
    struct zmk_behavior_binding_event event = {
        .position = sticky_layer->position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = sticky_layer->source,
#endif
    };
    return zmk_behavior_invoke_binding(&binding, event, true);
}

static inline int release_sticky_layer_local_behavior(struct active_sticky_layer_local *sticky_layer,
                                                      int64_t timestamp) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = sticky_layer->config->behavior.behavior_dev,
        .param1 = sticky_layer->param1,
    };
    struct zmk_behavior_binding_event event = {
        .position = sticky_layer->position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = sticky_layer->source,
#endif
    };

    clear_sticky_layer_local(sticky_layer);
    return zmk_behavior_invoke_binding(&binding, event, false);
}

static inline void on_sticky_layer_local_timeout(struct active_sticky_layer_local *sticky_layer) {
    if (sticky_layer->config->lazy) {
        clear_sticky_layer_local(sticky_layer);
    } else {
        release_sticky_layer_local_behavior(sticky_layer, sticky_layer->release_at);
    }
}

static int stop_timer(struct active_sticky_layer_local *sticky_layer) {
    int timer_cancel_result = k_work_cancel_delayable(&sticky_layer->release_timer);
    if (timer_cancel_result == -EINPROGRESS) {
        sticky_layer->timer_cancelled = true;
    }
    return timer_cancel_result;
}

int gauntlet_sticky_layer_local_cancel(uint32_t layer, int64_t timestamp) {
    for (int i = 0; i < ZMK_BHV_STICKY_LAYER_LOCAL_MAX_HELD; i++) {
        struct active_sticky_layer_local *sticky_layer = &active_sticky_layers_local[i];
        if (sticky_layer->position == ZMK_BHV_STICKY_LAYER_LOCAL_POSITION_FREE ||
            sticky_layer->param1 != layer) {
            continue;
        }
        stop_timer(sticky_layer);
        release_sticky_layer_local_behavior(sticky_layer, timestamp);
    }
    return 0;
}

static int on_sticky_layer_local_binding_pressed(struct zmk_behavior_binding *binding,
                                                 struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_sticky_layer_local_config *cfg = dev->config;
    struct active_sticky_layer_local *sticky_layer;
    sticky_layer = find_sticky_layer_local(event.position, cfg->behavior, binding->param1);
    if (sticky_layer != NULL) {
        LOG_DBG("found same local sticky layer pressed at position %d, release it first",
                event.position);
        stop_timer(sticky_layer);
        release_sticky_layer_local_behavior(sticky_layer, event.timestamp);
    }
    sticky_layer = store_sticky_layer_local(&event, binding->param1, cfg);
    if (sticky_layer == NULL) {
        LOG_ERR("unable to store local sticky layer, did you press more than %d sticky layers?",
                ZMK_BHV_STICKY_LAYER_LOCAL_MAX_HELD);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    LOG_DBG("%d new local sticky layer", event.position);
    sticky_layer->had_other_position_pressed_on_press = had_other_position_pressed(event.position);
    if (!sticky_layer->config->lazy) {
        press_sticky_layer_local_behavior(sticky_layer, event.timestamp);
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_sticky_layer_local_binding_released(struct zmk_behavior_binding *binding,
                                                  struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_sticky_layer_local_config *cfg = dev->config;
    struct active_sticky_layer_local *sticky_layer =
        find_sticky_layer_local(event.position, cfg->behavior, binding->param1);
    if (sticky_layer == NULL) {
        LOG_ERR("ACTIVE LOCAL STICKY LAYER CLEARED TOO EARLY");
        return ZMK_BEHAVIOR_OPAQUE;
    }

    if (sticky_layer->modified_key_usage_page != 0 && sticky_layer->modified_key_keycode != 0) {
        LOG_DBG("Another key was pressed while the local sticky layer was pressed.");
        return release_sticky_layer_local_behavior(sticky_layer, event.timestamp);
    }

    if (sticky_layer->had_other_position_pressed_on_press) {
        LOG_DBG("Another key was already held when the local sticky layer was pressed.");
        return release_sticky_layer_local_behavior(sticky_layer, event.timestamp);
    }

    sticky_layer->timer_started = true;
    sticky_layer->release_at = event.timestamp + sticky_layer->config->release_after_ms;
    int32_t ms_left = sticky_layer->release_at - k_uptime_get();
    if (ms_left > 0) {
        k_work_schedule(&sticky_layer->release_timer, K_MSEC(ms_left));
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_sticky_layer_local_driver_api = {
    .binding_pressed = on_sticky_layer_local_binding_pressed,
    .binding_released = on_sticky_layer_local_binding_released,
};

static int sticky_layer_local_position_state_changed_listener(const zmk_event_t *eh);
static int sticky_layer_local_keycode_state_changed_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_sticky_layer_local_positions, sticky_layer_local_position_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_sticky_layer_local_positions, zmk_position_state_changed);
ZMK_LISTENER(behavior_sticky_layer_local, sticky_layer_local_keycode_state_changed_listener);
ZMK_SUBSCRIPTION(behavior_sticky_layer_local, zmk_keycode_state_changed);

static int sticky_layer_local_position_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (ev->position < ZMK_KEYMAP_LEN) {
        pressed_positions[ev->position] = ev->state;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static int sticky_layer_local_keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct active_sticky_layer_local
        *sticky_layers_to_press_before_reraise[ZMK_BHV_STICKY_LAYER_LOCAL_MAX_HELD];
    struct active_sticky_layer_local
        *sticky_layers_to_release_after_reraise[ZMK_BHV_STICKY_LAYER_LOCAL_MAX_HELD];

    const struct zmk_keycode_state_changed ev_copy = *ev;

    for (int i = 0; i < ZMK_BHV_STICKY_LAYER_LOCAL_MAX_HELD; i++) {
        sticky_layers_to_press_before_reraise[i] = NULL;
        sticky_layers_to_release_after_reraise[i] = NULL;

        struct active_sticky_layer_local *sticky_layer = &active_sticky_layers_local[i];
        if (sticky_layer->position == ZMK_BHV_STICKY_LAYER_LOCAL_POSITION_FREE) {
            continue;
        }

        if (strcmp(sticky_layer->config->behavior.behavior_dev, KEY_PRESS) == 0 &&
            ZMK_HID_USAGE_ID(sticky_layer->param1) == ev_copy.keycode &&
            ZMK_HID_USAGE_PAGE(sticky_layer->param1) == ev_copy.usage_page &&
            SELECT_MODS(sticky_layer->param1) == ev_copy.implicit_modifiers) {
            continue;
        }

        if (ev_copy.state) {
            if (sticky_layer->config->ignore_modifiers &&
                is_mod(ev_copy.usage_page, ev_copy.keycode)) {
                continue;
            }
            if (sticky_layer->modified_key_usage_page != 0 ||
                sticky_layer->modified_key_keycode != 0) {
                continue;
            }

            stop_timer(sticky_layer);

            if (sticky_layer->release_at != 0 && ev_copy.timestamp > sticky_layer->release_at) {
                on_sticky_layer_local_timeout(sticky_layer);
                continue;
            }

            if (sticky_layer->config->lazy) {
                sticky_layers_to_press_before_reraise[i] = sticky_layer;
            }
            if (sticky_layer->timer_started && sticky_layer->config->quick_release) {
                sticky_layers_to_release_after_reraise[i] = sticky_layer;
            }
            sticky_layer->modified_key_usage_page = ev_copy.usage_page;
            sticky_layer->modified_key_keycode = ev_copy.keycode;
        } else {
            if (sticky_layer->timer_started &&
                sticky_layer->modified_key_usage_page == ev_copy.usage_page &&
                sticky_layer->modified_key_keycode == ev_copy.keycode) {
                stop_timer(sticky_layer);
                sticky_layers_to_release_after_reraise[i] = sticky_layer;
            }
        }
    }

    for (int i = 0; i < ZMK_BHV_STICKY_LAYER_LOCAL_MAX_HELD; i++) {
        struct active_sticky_layer_local *sticky_layer = sticky_layers_to_press_before_reraise[i];
        if (!sticky_layer) {
            continue;
        }

        press_sticky_layer_local_behavior(sticky_layer, ev_copy.timestamp);
    }

    bool event_reraised = false;
    for (int i = 0; i < ZMK_BHV_STICKY_LAYER_LOCAL_MAX_HELD; i++) {
        struct active_sticky_layer_local *sticky_layer =
            sticky_layers_to_release_after_reraise[i];
        if (!sticky_layer) {
            continue;
        }

        if (!event_reraised) {
            struct zmk_keycode_state_changed_event dupe_ev =
                copy_raised_zmk_keycode_state_changed(ev);
            ZMK_EVENT_RAISE_AFTER(dupe_ev, behavior_sticky_layer_local);
            event_reraised = true;
        }
        release_sticky_layer_local_behavior(sticky_layer, ev_copy.timestamp);
    }

    return event_reraised ? ZMK_EV_EVENT_CAPTURED : ZMK_EV_EVENT_BUBBLE;
}

static void behavior_sticky_layer_local_timer_handler(struct k_work *item) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(item);
    struct active_sticky_layer_local *sticky_layer =
        CONTAINER_OF(d_work, struct active_sticky_layer_local, release_timer);
    if (sticky_layer->position == ZMK_BHV_STICKY_LAYER_LOCAL_POSITION_FREE) {
        return;
    }
    if (sticky_layer->timer_cancelled) {
        sticky_layer->timer_cancelled = false;
    } else {
        on_sticky_layer_local_timeout(sticky_layer);
    }
}

static int behavior_sticky_layer_local_init(const struct device *dev) {
    ARG_UNUSED(dev);
    static bool init_first_run = true;
    if (init_first_run) {
        for (int i = 0; i < ZMK_BHV_STICKY_LAYER_LOCAL_MAX_HELD; i++) {
            k_work_init_delayable(&active_sticky_layers_local[i].release_timer,
                                  behavior_sticky_layer_local_timer_handler);
            active_sticky_layers_local[i].position = ZMK_BHV_STICKY_LAYER_LOCAL_POSITION_FREE;
        }
    }
    init_first_run = false;
    return 0;
}

#define STICKY_LAYER_LOCAL_INST(n)                                                                  \
    static const struct behavior_sticky_layer_local_config                                          \
        behavior_sticky_layer_local_config_##n = {                                                  \
            .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, DT_DRV_INST(n)),                             \
            .release_after_ms = DT_INST_PROP(n, release_after_ms),                                 \
            .quick_release = DT_INST_PROP(n, quick_release),                                       \
            .lazy = DT_INST_PROP(n, lazy),                                                         \
            .ignore_modifiers = DT_INST_PROP(n, ignore_modifiers),                                 \
        };                                                                                         \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_sticky_layer_local_init, NULL, NULL,                       \
                            &behavior_sticky_layer_local_config_##n, POST_KERNEL,                  \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                    \
                            &behavior_sticky_layer_local_driver_api);

DT_INST_FOREACH_STATUS_OKAY(STICKY_LAYER_LOCAL_INST)

#endif
