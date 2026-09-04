/*
 * CLine46: 相対入力(トラックボール)の移動量を方向つきのキータップに変換する
 *
 * 注意: input processor のコールバックは Zephyr の input スレッドで動く。
 * このスレッドのスタックは CONFIG_INPUT_THREAD_STACK_SIZE (既定 512B) しかなく、
 * ここから zmk_behavior_invoke_binding() を呼ぶと BLE HID 送信まで潜って
 * スタックを溢れさせる。そのため実際のタップはメッセージキュー経由で
 * システムワークキューに逃がしてから発行する。
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_key_tapper

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

#include <drivers/input_processor.h>

#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct kt_tap_item {
    uint8_t binding_idx;
    uint8_t device_index;
};

struct kt_config {
    uint8_t index;
    uint16_t type;
    size_t codes_len;
    int32_t threshold;
    uint32_t tap_ms;
    uint32_t wait_ms;
    const uint16_t *codes;
    /* codes 1つにつき2エントリ: [i*2] = 負方向, [i*2+1] = 正方向 */
    const struct zmk_behavior_binding *bindings;
    struct k_msgq *msgq;
};

struct kt_data {
    const struct device *dev;
    struct k_work work;
    int32_t *acc;
};

/* システムワークキュー上で実行される。ここなら behavior を起動できる。 */
static void kt_work_handler(struct k_work *work) {
    struct kt_data *data = CONTAINER_OF(work, struct kt_data, work);
    const struct kt_config *cfg = data->dev->config;
    struct kt_tap_item item;

    while (k_msgq_get(cfg->msgq, &item, K_NO_WAIT) == 0) {
        const struct zmk_behavior_binding *binding = &cfg->bindings[item.binding_idx];

        struct zmk_behavior_binding_event behavior_event = {
            .position =
                ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(item.device_index, cfg->index),
            .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
        };

        LOG_DBG("key tapper: queueing %s", binding->behavior_dev);

        zmk_behavior_queue_add(&behavior_event, *binding, true, cfg->tap_ms);
        zmk_behavior_queue_add(&behavior_event, *binding, false, cfg->wait_ms);
    }
}

static void kt_queue_tap(const struct kt_config *cfg, struct kt_data *data, uint8_t binding_idx,
                         uint8_t device_index) {
    struct kt_tap_item item = {
        .binding_idx = binding_idx,
        .device_index = device_index,
    };

    /* 溜まりすぎている場合は取りこぼす。入力スレッドを絶対にブロックしない。 */
    if (k_msgq_put(cfg->msgq, &item, K_NO_WAIT) != 0) {
        LOG_WRN("key tapper: queue full, dropping tap");
        return;
    }

    k_work_submit(&data->work);
}

static int kt_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                           uint32_t param2, struct zmk_input_processor_state *state) {
    const struct kt_config *cfg = dev->config;
    struct kt_data *data = dev->data;

    if (event->type != cfg->type) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    for (size_t i = 0; i < cfg->codes_len; i++) {
        if (cfg->codes[i] != event->code) {
            continue;
        }

        data->acc[i] += event->value;

        while (data->acc[i] >= cfg->threshold) {
            data->acc[i] -= cfg->threshold;
            kt_queue_tap(cfg, data, (i * 2) + 1, state->input_device_index);
        }
        while (data->acc[i] <= -cfg->threshold) {
            data->acc[i] += cfg->threshold;
            kt_queue_tap(cfg, data, i * 2, state->input_device_index);
        }

        /* このレイヤーではポインタを動かさない。
         * レイヤーオーバーライド経路では ZMK_INPUT_PROC_STOP が握り潰されて
         * CONTINUE として扱われる (zmk/app/src/pointing/input_listener.c の
         * filter_with_input_config) ため、移動量そのものを 0 にしておく。 */
        event->value = 0;
        return ZMK_INPUT_PROC_STOP;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api kt_driver_api = {
    .handle_event = kt_handle_event,
};

static int kt_init(const struct device *dev) {
    struct kt_data *data = dev->data;

    data->dev = dev;
    k_work_init(&data->work, kt_work_handler);

    return 0;
}

#define KT_INST(n)                                                                                 \
    static const uint16_t kt_codes_##n[] = DT_INST_PROP(n, codes);                                 \
    static const struct zmk_behavior_binding kt_bindings_##n[] = {                                 \
        LISTIFY(DT_INST_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), DT_DRV_INST(n))}; \
    BUILD_ASSERT(ARRAY_SIZE(kt_bindings_##n) == ARRAY_SIZE(kt_codes_##n) * 2,                      \
                 "bindings needs two entries (negative, positive) per code");                      \
    K_MSGQ_DEFINE(kt_msgq_##n, sizeof(struct kt_tap_item), 16, 4);                                 \
    static int32_t kt_acc_##n[ARRAY_SIZE(kt_codes_##n)];                                           \
    static struct kt_data kt_data_##n = {                                                          \
        .acc = kt_acc_##n,                                                                         \
    };                                                                                             \
    static const struct kt_config kt_config_##n = {                                                \
        .index = n,                                                                                \
        .type = DT_INST_PROP(n, type),                                                             \
        .codes_len = DT_INST_PROP_LEN(n, codes),                                                   \
        .codes = kt_codes_##n,                                                                     \
        .threshold = DT_INST_PROP(n, threshold),                                                   \
        .tap_ms = DT_INST_PROP(n, tap_ms),                                                         \
        .wait_ms = DT_INST_PROP(n, wait_ms),                                                       \
        .bindings = kt_bindings_##n,                                                               \
        .msgq = &kt_msgq_##n,                                                                      \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, &kt_init, NULL, &kt_data_##n, &kt_config_##n, POST_KERNEL,             \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &kt_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KT_INST)
