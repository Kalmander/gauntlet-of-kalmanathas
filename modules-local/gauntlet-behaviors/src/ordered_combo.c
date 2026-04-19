/*
 * Local ordered-combo listener for Gauntlet-specific roll-style combos.
 * We keep this separate from stock zmk,combos so upstream combo behavior can
 * stay unmodified and easy to update.
 *
 * Original work Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT gauntlet_ordered_combos

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/hid.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define ORDERED_COMBO_KEYS_BYTE_ARRAY(node_id)                                                    \
    uint8_t _CONCAT(ordered_combo_prop_, node_id)[DT_PROP_LEN(node_id, key_positions)];

#define MAX_ORDERED_COMBO_KEYS                                                                    \
    sizeof(union {DT_INST_FOREACH_CHILD(0, ORDERED_COMBO_KEYS_BYTE_ARRAY)})

struct ordered_combo_cfg {
    int32_t key_positions[MAX_ORDERED_COMBO_KEYS];
    int16_t key_position_len;
    int16_t require_prior_idle_ms;
    int32_t timeout_ms;
    uint32_t layer_mask;
    struct zmk_behavior_binding behavior;
    bool slow_release;
};

struct active_ordered_combo {
    uint16_t combo_idx;
    uint16_t key_positions_pressed_count;
    struct zmk_position_state_changed_event key_positions_pressed[MAX_ORDERED_COMBO_KEYS];
};

#define PROP_BIT_AT_IDX(n, prop, idx) BIT(DT_PROP_BY_IDX(n, prop, idx))

#define NODE_PROP_BITMASK(n, prop)                                                                \
    COND_CODE_1(DT_NODE_HAS_PROP(n, prop),                                                        \
                (DT_FOREACH_PROP_ELEM_SEP(n, prop, PROP_BIT_AT_IDX, (|))), (0))

#define ORDERED_COMBO_INST(n, positions)                                                          \
    COND_CODE_1(IS_EQ(DT_PROP_LEN(n, key_positions), positions),                                  \
                (                                                                                 \
                    {                                                                             \
                        .timeout_ms = DT_PROP(n, timeout_ms),                                     \
                        .require_prior_idle_ms = DT_PROP(n, require_prior_idle_ms),               \
                        .key_positions = DT_PROP(n, key_positions),                               \
                        .key_position_len = DT_PROP_LEN(n, key_positions),                        \
                        .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, n),                             \
                        .slow_release = DT_PROP(n, slow_release),                                 \
                        .layer_mask = NODE_PROP_BITMASK(n, layers),                               \
                    }, ),                                                                         \
                ())

#define ORDERED_COMBO_CONFIGS_WITH_MATCHING_POSITIONS_LEN(positions, _ignore)                     \
    DT_INST_FOREACH_CHILD_VARGS(0, ORDERED_COMBO_INST, positions)

static const struct ordered_combo_cfg ordered_combos[] = {
    LISTIFY(20, ORDERED_COMBO_CONFIGS_WITH_MATCHING_POSITIONS_LEN, (), 0)};

#define ORDERED_COMBO_ONE(n) +1

#define ORDERED_COMBO_CHILDREN_COUNT (0 DT_INST_FOREACH_CHILD(0, ORDERED_COMBO_ONE))

#define BYTES_FOR_ORDERED_COMBOS_MASK DIV_ROUND_UP(ORDERED_COMBO_CHILDREN_COUNT, 32)

#define GAUNTLET_ORDERED_COMBO_VIRT_BASE ((uint32_t)(INT32_MAX - 8192))
#define GAUNTLET_VIRTUAL_KEY_POSITION_ORDERED_COMBO(index)                                        \
    (GAUNTLET_ORDERED_COMBO_VIRT_BASE + (index))

static uint8_t pressed_keys_count = 0;
static struct zmk_position_state_changed_event pressed_keys[MAX_ORDERED_COMBO_KEYS] = {};
static uint32_t candidates[BYTES_FOR_ORDERED_COMBOS_MASK];
static uint32_t combo_lookup[ZMK_KEYMAP_LEN][BYTES_FOR_ORDERED_COMBOS_MASK] = {};
static struct active_ordered_combo active_combos[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS] = {};
static uint8_t active_combo_count = 0;

static struct k_work_delayable timeout_task;
static int64_t timeout_task_timeout_at;

static int64_t last_tapped_timestamp = INT32_MIN;
static int64_t last_combo_timestamp = INT32_MIN;

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_combo_timestamp) {
        last_tapped_timestamp = timestamp;
    }
}

static int initialize_ordered_combo(size_t index) {
    const struct ordered_combo_cfg *new_combo = &ordered_combos[index];

    sys_bitfield_set_bit((mem_addr_t)&combo_lookup[new_combo->key_positions[0]], index);
    return 0;
}

static bool ordered_combo_active_on_layer(const struct ordered_combo_cfg *combo, uint8_t layer) {
    if (!combo->layer_mask) {
        return true;
    }

    return combo->layer_mask & BIT(layer);
}

static bool is_quick_tap(const struct ordered_combo_cfg *combo, int64_t timestamp) {
    return (last_tapped_timestamp + combo->require_prior_idle_ms) > timestamp;
}

static int setup_candidates_for_first_keypress(int32_t position, int64_t timestamp) {
    int number_of_combo_candidates = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();

    for (size_t i = 0; i < ARRAY_SIZE(ordered_combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&combo_lookup[position], i)) {
            const struct ordered_combo_cfg *combo = &ordered_combos[i];
            if (ordered_combo_active_on_layer(combo, highest_active_layer) &&
                !is_quick_tap(combo, timestamp)) {
                sys_bitfield_set_bit((mem_addr_t)&candidates, i);
                number_of_combo_candidates++;
            }
        }
    }

    return number_of_combo_candidates;
}

static inline uint8_t zero_one_or_more_bits(uint32_t field) {
    if (field == 0) {
        return 0;
    }
    if ((field & (field - 1)) == 0) {
        return 1;
    }
    return 2;
}

static int filter_candidates(int32_t position) {
    int matches = 0;
    uint8_t expected_index = pressed_keys_count;

    for (int i = 0; i < BYTES_FOR_ORDERED_COMBOS_MASK; i++) {
        uint32_t filtered = candidates[i];

        while (filtered) {
            int bit = find_lsb_set(filtered) - 1;
            int combo_idx = (i * 32) + bit;
            const struct ordered_combo_cfg *combo = &ordered_combos[combo_idx];

            if (combo->key_position_len <= expected_index ||
                combo->key_positions[expected_index] != position) {
                sys_bitfield_clear_bit((mem_addr_t)&candidates, combo_idx);
                filtered &= ~BIT(bit);
            } else {
                filtered &= ~BIT(bit);
            }
        }

        if (matches < 2) {
            matches += zero_one_or_more_bits(candidates[i]);
        }
    }

    LOG_DBG("ordered combo matches after filter %d", matches);
    return matches;
}

static int64_t first_candidate_timeout(void) {
    if (pressed_keys_count == 0) {
        return LLONG_MAX;
    }

    int64_t first_timeout = LLONG_MAX;
    for (int i = 0; i < ARRAY_SIZE(ordered_combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
            first_timeout = MIN(first_timeout, ordered_combos[i].timeout_ms);
        }
    }

    return pressed_keys[0].data.timestamp + first_timeout;
}

static int16_t find_complete_candidate(void) {
    for (int i = 0; i < ARRAY_SIZE(ordered_combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i) &&
            ordered_combos[i].key_position_len == pressed_keys_count) {
            return i;
        }
    }

    return INT16_MAX;
}

static int release_pressed_keys(void) {
    uint8_t count = pressed_keys_count;
    pressed_keys_count = 0;

    for (int i = 0; i < count; i++) {
        struct zmk_position_state_changed_event *ev = &pressed_keys[i];
        if (i == 0) {
            LOG_DBG("ordered combo: releasing position event %d", ev->data.position);
            ZMK_EVENT_RELEASE(*ev);
        } else {
            LOG_DBG("ordered combo: reraising position event %d", ev->data.position);
            ZMK_EVENT_RAISE(*ev);
        }
    }

    return count;
}

static inline int press_combo_behavior(int combo_idx, const struct ordered_combo_cfg *combo,
                                       int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = GAUNTLET_VIRTUAL_KEY_POSITION_ORDERED_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    last_combo_timestamp = timestamp;

    return zmk_behavior_invoke_binding(&combo->behavior, event, true);
}

static inline int release_combo_behavior(int combo_idx, const struct ordered_combo_cfg *combo,
                                         int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = GAUNTLET_VIRTUAL_KEY_POSITION_ORDERED_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    return zmk_behavior_invoke_binding(&combo->behavior, event, false);
}

static void move_pressed_keys_to_active_combo(struct active_ordered_combo *active_combo) {
    int combo_length = MIN(pressed_keys_count, ordered_combos[active_combo->combo_idx].key_position_len);

    for (int i = 0; i < combo_length; i++) {
        active_combo->key_positions_pressed[i] = pressed_keys[i];
    }
    active_combo->key_positions_pressed_count = combo_length;

    for (int i = 0; i + combo_length < pressed_keys_count; i++) {
        pressed_keys[i] = pressed_keys[i + combo_length];
    }

    pressed_keys_count -= combo_length;
}

static struct active_ordered_combo *store_active_combo(int32_t combo_idx) {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        if (active_combos[i].combo_idx == UINT16_MAX) {
            active_combos[i].combo_idx = combo_idx;
            active_combo_count++;
            return &active_combos[i];
        }
    }

    LOG_ERR("Unable to store ordered combo; already %d active. Increase "
            "CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS",
            CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS);
    return NULL;
}

static void activate_combo(int combo_idx) {
    struct active_ordered_combo *active_combo = store_active_combo(combo_idx);
    if (active_combo == NULL) {
        release_pressed_keys();
        return;
    }

    move_pressed_keys_to_active_combo(active_combo);
    press_combo_behavior(combo_idx, &ordered_combos[combo_idx],
                         active_combo->key_positions_pressed[0].data.timestamp);
}

static void deactivate_combo(int active_combo_index) {
    active_combo_count--;
    if (active_combo_index != active_combo_count) {
        memcpy(&active_combos[active_combo_index], &active_combos[active_combo_count],
               sizeof(struct active_ordered_combo));
    }
    active_combos[active_combo_count] = (struct active_ordered_combo){0};
    active_combos[active_combo_count].combo_idx = UINT16_MAX;
}

static bool release_combo_key(int32_t position, int64_t timestamp) {
    for (int combo_idx = 0; combo_idx < active_combo_count; combo_idx++) {
        struct active_ordered_combo *active_combo = &active_combos[combo_idx];

        bool key_released = false;
        bool all_keys_pressed = active_combo->key_positions_pressed_count ==
                                ordered_combos[active_combo->combo_idx].key_position_len;
        bool all_keys_released = true;

        for (int i = 0; i < active_combo->key_positions_pressed_count; i++) {
            if (key_released) {
                active_combo->key_positions_pressed[i - 1] = active_combo->key_positions_pressed[i];
                all_keys_released = false;
            } else if (active_combo->key_positions_pressed[i].data.position != position) {
                all_keys_released = false;
            } else {
                key_released = true;
            }
        }

        if (key_released) {
            active_combo->key_positions_pressed_count--;
            const struct ordered_combo_cfg *combo = &ordered_combos[active_combo->combo_idx];
            if ((combo->slow_release && all_keys_released) ||
                (!combo->slow_release && all_keys_pressed)) {
                release_combo_behavior(active_combo->combo_idx, combo, timestamp);
            }
            if (all_keys_released) {
                deactivate_combo(combo_idx);
            }
            return true;
        }
    }

    return false;
}

static void update_timeout_task(void) {
    int64_t first_timeout = first_candidate_timeout();
    if (timeout_task_timeout_at == first_timeout) {
        return;
    }
    if (first_timeout == LLONG_MAX) {
        timeout_task_timeout_at = 0;
        k_work_cancel_delayable(&timeout_task);
        return;
    }
    if (k_work_schedule(&timeout_task, K_MSEC(MAX((int64_t)0, first_timeout - k_uptime_get()))) >=
        0) {
        timeout_task_timeout_at = first_timeout;
    }
}

static int cleanup(int16_t combo_to_activate) {
    k_work_cancel_delayable(&timeout_task);
    timeout_task_timeout_at = 0;
    memset(candidates, 0, BYTES_FOR_ORDERED_COMBOS_MASK * sizeof(uint32_t));

    if (combo_to_activate != INT16_MAX) {
        activate_combo(combo_to_activate);
    }

    return release_pressed_keys();
}

static int filter_timed_out_candidates(int64_t timestamp) {
    __ASSERT(pressed_keys_count > 0, "Searching for an ordered combo timeout with no keys pressed");

    int remaining_candidates = 0;
    for (int i = 0; i < ARRAY_SIZE(ordered_combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
            if (pressed_keys[0].data.timestamp + ordered_combos[i].timeout_ms > timestamp) {
                remaining_candidates++;
            } else {
                sys_bitfield_clear_bit((mem_addr_t)&candidates, i);
            }
        }
    }

    LOG_DBG("ordered combos after timeout filtering: remaining_candidates=%d timestamp=%lld",
            remaining_candidates, timestamp);

    return remaining_candidates;
}

static int capture_pressed_key(const struct zmk_position_state_changed *ev) {
    if (pressed_keys_count == MAX_ORDERED_COMBO_KEYS) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    pressed_keys[pressed_keys_count++] = copy_raised_zmk_position_state_changed(ev);
    return ZMK_EV_EVENT_CAPTURED;
}

static int position_state_down(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int num_candidates;
    if (!pressed_keys_count) {
        num_candidates = setup_candidates_for_first_keypress(data->position, data->timestamp);
        if (num_candidates == 0) {
            return ZMK_EV_EVENT_BUBBLE;
        }
    } else {
        filter_timed_out_candidates(data->timestamp);
        num_candidates = filter_candidates(data->position);
    }

    LOG_DBG("ordered combo: capturing position event %d", data->position);
    int ret = capture_pressed_key(data);
    update_timeout_task();

    if (num_candidates) {
        int16_t combo_to_activate = find_complete_candidate();
        if (combo_to_activate != INT16_MAX) {
            cleanup(combo_to_activate);
        }
        return ret;
    }

    cleanup(INT16_MAX);
    return ret;
}

static int position_state_up(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int released_keys = cleanup(INT16_MAX);
    if (release_combo_key(data->position, data->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (released_keys > 1) {
        struct zmk_position_state_changed_event dupe_ev =
            copy_raised_zmk_position_state_changed(data);
        ZMK_EVENT_RAISE(dupe_ev);
        return ZMK_EV_EVENT_CAPTURED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void ordered_combo_timeout_handler(struct k_work *item) {
    ARG_UNUSED(item);

    if (timeout_task_timeout_at == 0 || k_uptime_get() < timeout_task_timeout_at) {
        return;
    }
    if (filter_timed_out_candidates(timeout_task_timeout_at) == 0) {
        cleanup(INT16_MAX);
        return;
    }

    update_timeout_task();
}

static int position_state_changed_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
    if (data == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (data->state) {
        return position_state_down(ev, data);
    }

    return position_state_up(ev, data);
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        store_last_tapped(ev->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int ordered_combo_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return position_state_changed_listener(eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        return keycode_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(gauntlet_ordered_combo, ordered_combo_listener);
ZMK_SUBSCRIPTION(gauntlet_ordered_combo, zmk_position_state_changed);
ZMK_SUBSCRIPTION(gauntlet_ordered_combo, zmk_keycode_state_changed);

static int ordered_combo_init(void) {
    for (size_t i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        active_combos[i].combo_idx = UINT16_MAX;
    }

    k_work_init_delayable(&timeout_task, ordered_combo_timeout_handler);

    LOG_WRN("Have %d ordered combos!", ARRAY_SIZE(ordered_combos));
    for (int i = 0; i < ARRAY_SIZE(ordered_combos); i++) {
        initialize_ordered_combo(i);
    }
    return 0;
}

SYS_INIT(ordered_combo_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#endif
