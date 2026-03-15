/*
 * Local copy of tri-state for Gauntlet-specific extensions.
 * We keep the optional release-interrupt suppression here so the vendored
 * zmk-tri-state checkout can stay unmodified and easy to update from upstream.
 *
 * Original work Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_tri_state_local

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/hid.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define ZMK_BHV_MAX_ACTIVE_TRI_STATES_LOCAL 10

struct behavior_tri_state_local_config {
    int32_t ignored_key_positions_len;
    int32_t ignored_layers_len;
    struct zmk_behavior_binding start_behavior;
    struct zmk_behavior_binding continue_behavior;
    struct zmk_behavior_binding end_behavior;
    int32_t ignored_layers;
    int32_t timeout_ms;
    int tap_ms;
    bool ignore_release_interrupts;
    bool refresh_on_allowed_keypress;
    uint8_t ignored_key_positions[];
};

struct active_tri_state_local {
    bool is_active;
    bool is_pressed;
    bool first_press;
    uint32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    const struct behavior_tri_state_local_config *config;
    struct k_work_delayable release_timer;
    int64_t release_at;
    bool timer_started;
    bool timer_cancelled;
    uint8_t allowed_keys_pressed;
};

static void trigger_end_behavior(struct active_tri_state_local *tri_state);

static int stop_timer(struct active_tri_state_local *tri_state) {
    int timer_cancel_result = k_work_cancel_delayable(&tri_state->release_timer);
    if (timer_cancel_result == -EINPROGRESS) {
        // Too late to cancel, the timer handler will clear things up.
        tri_state->timer_cancelled = true;
    }
    return timer_cancel_result;
}

static void reset_timer(int32_t timestamp, struct active_tri_state_local *tri_state) {
    tri_state->release_at = timestamp + tri_state->config->timeout_ms;
    tri_state->timer_cancelled = false;
    int32_t ms_left = tri_state->release_at - k_uptime_get();
    if (ms_left > 0) {
        k_work_schedule(&tri_state->release_timer, K_MSEC(ms_left));
        LOG_DBG("Successfully reset tri-state timer");
    }
}

static void resume_timer(struct active_tri_state_local *tri_state) {
    tri_state->timer_cancelled = false;
    int32_t ms_left = tri_state->release_at - k_uptime_get();
    if (ms_left > 0) {
        k_work_schedule(&tri_state->release_timer, K_MSEC(ms_left));
        LOG_DBG("Successfully resumed tri-state timer");
        return;
    }
    if (!tri_state->is_active || tri_state->is_pressed || tri_state->allowed_keys_pressed > 0) {
        return;
    }
    LOG_DBG("Tri-state deactivated due to expired allowed keypress timer");
    tri_state->is_active = false;
    trigger_end_behavior(tri_state);
}

static void trigger_end_behavior(struct active_tri_state_local *tri_state) {
    struct zmk_behavior_binding_event event = {
        .position = tri_state->position,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = tri_state->source,
#endif
    };

    zmk_behavior_queue_add(&event, tri_state->config->end_behavior, true, tri_state->config->tap_ms);
    zmk_behavior_queue_add(&event, tri_state->config->end_behavior, false, 0);
}

static void behavior_tri_state_local_timer_handler(struct k_work *item) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(item);
    struct active_tri_state_local *tri_state =
        CONTAINER_OF(d_work, struct active_tri_state_local, release_timer);
    if (!tri_state->is_active || tri_state->timer_cancelled || tri_state->is_pressed ||
        tri_state->allowed_keys_pressed > 0) {
        return;
    }
    LOG_DBG("Tri-state deactivated due to timer");
    tri_state->is_active = false;
    trigger_end_behavior(tri_state);
}

static void clear_tri_state(struct active_tri_state_local *tri_state) { tri_state->is_active = false; }

static struct active_tri_state_local active_tri_states[ZMK_BHV_MAX_ACTIVE_TRI_STATES_LOCAL] = {};

static struct active_tri_state_local *find_tri_state(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_MAX_ACTIVE_TRI_STATES_LOCAL; i++) {
        if (active_tri_states[i].position == position && active_tri_states[i].is_active) {
            return &active_tri_states[i];
        }
    }
    return NULL;
}

static int new_tri_state(struct zmk_behavior_binding_event *event,
                         const struct behavior_tri_state_local_config *config,
                         struct active_tri_state_local **tri_state) {
    for (int i = 0; i < ZMK_BHV_MAX_ACTIVE_TRI_STATES_LOCAL; i++) {
        struct active_tri_state_local *const ref_tri_state = &active_tri_states[i];
        if (!ref_tri_state->is_active) {
            ref_tri_state->position = event->position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            ref_tri_state->source = event->source;
#endif
            ref_tri_state->config = config;
            ref_tri_state->is_active = true;
            ref_tri_state->is_pressed = false;
            ref_tri_state->first_press = true;
            ref_tri_state->allowed_keys_pressed = 0;
            *tri_state = ref_tri_state;
            return 0;
        }
    }
    return -ENOMEM;
}

static bool is_other_key_ignored(struct active_tri_state_local *tri_state, int32_t position) {
    for (int i = 0; i < tri_state->config->ignored_key_positions_len; i++) {
        if (tri_state->config->ignored_key_positions[i] == position) {
            return true;
        }
    }
    return false;
}

static bool is_layer_ignored(struct active_tri_state_local *tri_state, int32_t layer) {
    if ((BIT(layer) & tri_state->config->ignored_layers) != 0U) {
        return true;
    }
    return false;
}

static int on_tri_state_binding_pressed(struct zmk_behavior_binding *binding,
                                        struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_tri_state_local_config *cfg = dev->config;
    struct active_tri_state_local *tri_state = find_tri_state(event.position);
    if (tri_state == NULL) {
        if (new_tri_state(&event, cfg, &tri_state) == -ENOMEM) {
            LOG_ERR("Unable to create new tri_state. Insufficient space in active_tri_states[].");
            return ZMK_BEHAVIOR_OPAQUE;
        }
        LOG_DBG("%d created new tri_state", event.position);
    }
    LOG_DBG("%d tri_state pressed", event.position);
    tri_state->is_pressed = true;
    if (tri_state->first_press) {
        zmk_behavior_invoke_binding((struct zmk_behavior_binding *)&cfg->start_behavior, event, true);
        zmk_behavior_invoke_binding((struct zmk_behavior_binding *)&cfg->start_behavior, event, false);
        tri_state->first_press = false;
    }
    zmk_behavior_invoke_binding((struct zmk_behavior_binding *)&cfg->continue_behavior, event, true);
    return ZMK_BEHAVIOR_OPAQUE;
}

static void release_tri_state(struct zmk_behavior_binding_event event,
                              struct zmk_behavior_binding *continue_behavior) {
    struct active_tri_state_local *tri_state = find_tri_state(event.position);
    if (tri_state == NULL) {
        return;
    }
    tri_state->is_pressed = false;
    zmk_behavior_invoke_binding(continue_behavior, event, false);
    reset_timer(k_uptime_get(), tri_state);
}

static int on_tri_state_binding_released(struct zmk_behavior_binding *binding,
                                         struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_tri_state_local_config *cfg = dev->config;
    LOG_DBG("%d tri_state keybind released", event.position);
    release_tri_state(event, (struct zmk_behavior_binding *)&cfg->continue_behavior);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int behavior_tri_state_local_init(const struct device *dev) {
    static bool init_first_run = true;
    if (init_first_run) {
        for (int i = 0; i < ZMK_BHV_MAX_ACTIVE_TRI_STATES_LOCAL; i++) {
            k_work_init_delayable(&active_tri_states[i].release_timer,
                                  behavior_tri_state_local_timer_handler);
            clear_tri_state(&active_tri_states[i]);
        }
    }
    init_first_run = false;
    return 0;
}

static const struct behavior_driver_api behavior_tri_state_local_driver_api = {
    .binding_pressed = on_tri_state_binding_pressed,
    .binding_released = on_tri_state_binding_released,
};

static int tri_state_local_listener(const zmk_event_t *eh);
static int tri_state_local_position_state_changed_listener(const zmk_event_t *eh);
static int tri_state_local_layer_state_changed_listener(const zmk_event_t *eh);

ZMK_LISTENER(behavior_tri_state_local, tri_state_local_listener);
ZMK_SUBSCRIPTION(behavior_tri_state_local, zmk_position_state_changed);
ZMK_SUBSCRIPTION(behavior_tri_state_local, zmk_layer_state_changed);

static int tri_state_local_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return tri_state_local_position_state_changed_listener(eh);
    } else if (as_zmk_layer_state_changed(eh) != NULL) {
        return tri_state_local_layer_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int tri_state_local_position_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (int i = 0; i < ZMK_BHV_MAX_ACTIVE_TRI_STATES_LOCAL; i++) {
        struct active_tri_state_local *tri_state = &active_tri_states[i];
        if (!tri_state->is_active) {
            continue;
        }
        if (tri_state->position == ev->position) {
            if (!tri_state->config->refresh_on_allowed_keypress) {
                continue;
            }
            if (ev->state) {
                tri_state->allowed_keys_pressed++;
                reset_timer(ev->timestamp, tri_state);
                stop_timer(tri_state);
            } else {
                if (tri_state->allowed_keys_pressed > 0) {
                    tri_state->allowed_keys_pressed--;
                }
                if (tri_state->allowed_keys_pressed == 0) {
                    resume_timer(tri_state);
                }
            }
            continue;
        }
        if (!is_other_key_ignored(tri_state, ev->position)) {
            if (tri_state->config->ignore_release_interrupts && !ev->state) {
                continue;
            }
            LOG_DBG("Tri-State interrupted, ending at %d %d", tri_state->position, ev->position);
            tri_state->is_active = false;
            struct zmk_behavior_binding_event event = {
                .position = tri_state->position,
                .timestamp = k_uptime_get(),
            };
            if (tri_state->is_pressed) {
                zmk_behavior_invoke_binding(
                    (struct zmk_behavior_binding *)&tri_state->config->continue_behavior, event, false);
            }
            trigger_end_behavior(tri_state);
            return ZMK_EV_EVENT_BUBBLE;
        }
        if (ev->state) {
            if (tri_state->config->refresh_on_allowed_keypress) {
                tri_state->allowed_keys_pressed++;
                reset_timer(ev->timestamp, tri_state);
            }
            stop_timer(tri_state);
        } else {
            if (tri_state->config->refresh_on_allowed_keypress) {
                if (tri_state->allowed_keys_pressed > 0) {
                    tri_state->allowed_keys_pressed--;
                }
                if (tri_state->allowed_keys_pressed == 0) {
                    resume_timer(tri_state);
                }
            } else {
                reset_timer(ev->timestamp, tri_state);
            }
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int tri_state_local_layer_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (!ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    for (int i = 0; i < ZMK_BHV_MAX_ACTIVE_TRI_STATES_LOCAL; i++) {
        struct active_tri_state_local *tri_state = &active_tri_states[i];
        if (!tri_state->is_active) {
            continue;
        }
        if (!is_layer_ignored(tri_state, ev->layer)) {
            LOG_DBG("Tri-State layer changed, ending at %d %d", tri_state->position, ev->layer);
            tri_state->is_active = false;
            struct zmk_behavior_binding_event event = {
                .position = tri_state->position,
                .timestamp = k_uptime_get(),
            };
            if (tri_state->is_pressed) {
                zmk_behavior_invoke_binding(
                    (struct zmk_behavior_binding *)&tri_state->config->continue_behavior, event, false);
            }
            zmk_behavior_invoke_binding(
                (struct zmk_behavior_binding *)&tri_state->config->end_behavior, event, true);
            zmk_behavior_invoke_binding(
                (struct zmk_behavior_binding *)&tri_state->config->end_behavior, event, false);
            return ZMK_EV_EVENT_BUBBLE;
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

#define _TRANSFORM_ENTRY(idx, node)                                                                \
    {                                                                                              \
        .behavior_dev = DEVICE_DT_NAME(DT_INST_PHANDLE_BY_IDX(node, bindings, idx)),              \
        .param1 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param1), (0),      \
                              (DT_INST_PHA_BY_IDX(node, bindings, idx, param1))),                  \
        .param2 = COND_CODE_0(DT_INST_PHA_HAS_CELL_AT_IDX(node, bindings, idx, param2), (0),      \
                              (DT_INST_PHA_BY_IDX(node, bindings, idx, param2))),                  \
    }

#define IF_BIT(n, prop, i) BIT(DT_PROP_BY_IDX(n, prop, i)) |

#define TRI_STATE_LOCAL_INST(n)                                                                     \
    static struct behavior_tri_state_local_config behavior_tri_state_local_config_##n = {          \
        .ignored_key_positions = DT_INST_PROP(n, ignored_key_positions),                           \
        .ignored_key_positions_len = DT_INST_PROP_LEN(n, ignored_key_positions),                   \
        .ignored_layers = DT_INST_FOREACH_PROP_ELEM(n, ignored_layers, IF_BIT) 0,                 \
        .ignored_layers_len = DT_INST_PROP_LEN(n, ignored_layers),                                 \
        .timeout_ms = DT_INST_PROP(n, timeout_ms),                                                 \
        .tap_ms = DT_INST_PROP(n, tap_ms),                                                         \
        .ignore_release_interrupts = DT_INST_PROP(n, ignore_release_interrupts),                   \
        .refresh_on_allowed_keypress = DT_INST_PROP(n, refresh_on_allowed_keypress),               \
        .start_behavior = _TRANSFORM_ENTRY(0, n),                                                  \
        .continue_behavior = _TRANSFORM_ENTRY(1, n),                                               \
        .end_behavior = _TRANSFORM_ENTRY(2, n)};                                                   \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_tri_state_local_init, NULL, NULL,                          \
                            &behavior_tri_state_local_config_##n, POST_KERNEL,                     \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_tri_state_local_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TRI_STATE_LOCAL_INST)
