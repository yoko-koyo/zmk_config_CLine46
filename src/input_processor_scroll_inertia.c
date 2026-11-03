/*
 * CLine46: スクロールに慣性をつける input processor
 *
 * スクロールイベント (REL_WHEEL / REL_HWHEEL) を記録して離す直前の速度を測り、
 * 入力が release-ms 途切れたら (= ボールから手を離したら)、その速度から
 * 減衰させながらスクロールを送り続ける。
 *
 * - 縮小 (scroll_runtime_input_processor) より前に置き、細かい値で速度を測る。
 *   慣性分は divisor で割ってから送る (縮小と同じ値にしておくこと)
 * - min-stroke-ms 以上続けて転がした時だけ慣性を付ける (操作中の途切れで滑らない)
 * - 主に動かしていた軸だけに慣性を付ける
 * - 再びスクロール入力が来るか、キーを押すと即停止
 *
 * 慣性分の送信は input スレッドではなくシステムワークキューから
 * HID レポートを直接送る (input スレッドはスタックが小さいため。
 * input_processor_key_tapper.c の注意書きを参照)。
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_scroll_inertia

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>

#include <drivers/input_processor.h>

#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* 軸: 0 = 横 (REL_HWHEEL), 1 = 縦 (REL_WHEEL) */
#define SI_AXES 2
/* 速度計算に使う直近のレポート数 (センサーは最短12ms間隔なので16件で約190ms分) */
#define SI_HISTORY 16

struct si_config {
    uint32_t release_ms;     /* 入力がこの時間途切れたら慣性開始 */
    uint32_t window_ms;      /* 離す直前のこの時間の平均速度を初速にする */
    uint32_t min_stroke_ms;  /* これ以上続けて転がした時だけ慣性を付ける */
    uint32_t tick_ms;        /* 慣性スクロールの送信間隔 */
    int32_t divisor;         /* 入力値 → 送信値の割り算 (後段の縮小と同じ値) */
    int32_t decay;           /* tick ごとの速度の残存率 (千分率) */
    int32_t gain;            /* 慣性開始時の速度倍率 (千分率) */
    int32_t start_velocity;  /* 送信単位/秒。これ未満なら慣性を付けない */
    int32_t max_velocity;    /* 送信単位/秒。慣性の初速の上限 (0 = 上限なし) */
    int32_t stop_velocity;   /* 送信単位/秒。これ未満になったら慣性終了 */
};

struct si_sample {
    int64_t time;
    int32_t value[SI_AXES];
};

struct si_data {
    const struct device *dev;
    struct k_spinlock lock;
    struct k_work_delayable release_work;
    struct k_work_delayable tick_work;

    /* 速度計測 */
    int64_t stroke_start;
    int64_t last_event;
    int32_t pending[SI_AXES];
    struct si_sample history[SI_HISTORY];
    uint8_t head;  /* 次に書き込む位置 */
    uint8_t count; /* 有効な件数 */

    /* 慣性 (送信単位 × 1000 で保持) */
    bool active;
    int32_t inertia_v[SI_AXES]; /* milli-units/sec */
    int32_t acc[SI_AXES];       /* 端数 (milli-units × ms / 1000) */
};

static void si_stop_locked(struct si_data *data) {
    data->active = false;
    for (int i = 0; i < SI_AXES; i++) {
        data->inertia_v[i] = 0;
        data->acc[i] = 0;
    }
}

static void si_stop(const struct device *dev) {
    struct si_data *data = dev->data;

    K_SPINLOCK(&data->lock) { si_stop_locked(data); }
    k_work_cancel_delayable(&data->tick_work);
}

static void si_send_scroll(int32_t x, int32_t y) {
    x = CLAMP(x, -127, 127);
    y = CLAMP(y, -127, 127);
    zmk_hid_mouse_scroll_set(x, y);
    zmk_endpoints_send_mouse_report();
    zmk_hid_mouse_scroll_set(0, 0);
}

/* 慣性スクロールを1tick分送る (システムワークキュー) */
static void si_tick_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct si_data *data = CONTAINER_OF(dwork, struct si_data, tick_work);
    const struct si_config *cfg = data->dev->config;
    int32_t out[SI_AXES] = {0};
    bool keep = false;

    K_SPINLOCK(&data->lock) {
        if (!data->active) {
            K_SPINLOCK_BREAK;
        }
        for (int i = 0; i < SI_AXES; i++) {
            /* milli-units/sec × ms = micro-units → 1e6 で1単位 */
            data->acc[i] += data->inertia_v[i] * (int32_t)cfg->tick_ms;
            out[i] = data->acc[i] / 1000000;
            data->acc[i] -= out[i] * 1000000;
            data->inertia_v[i] = data->inertia_v[i] * cfg->decay / 1000;
            if (abs(data->inertia_v[i]) >= cfg->stop_velocity * 1000) {
                keep = true;
            }
        }
        if (!keep) {
            data->active = false;
        }
    }

    if (out[0] != 0 || out[1] != 0) {
        si_send_scroll(out[0], out[1]);
    }
    if (keep) {
        k_work_reschedule(&data->tick_work, K_MSEC(cfg->tick_ms));
    }
}

/* 入力が途切れた: 離す直前の速度から慣性を開始するか判定する (システムワークキュー) */
static void si_release_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct si_data *data = CONTAINER_OF(dwork, struct si_data, release_work);
    const struct si_config *cfg = data->dev->config;
    bool start = false;

    K_SPINLOCK(&data->lock) {
        si_stop_locked(data);

        if (data->count < 3 || data->last_event - data->stroke_start < cfg->min_stroke_ms) {
            data->count = 0;
            K_SPINLOCK_BREAK;
        }

        /* 新しい方から window_ms 以内のレポートを集める。
         * 最古のレポートは区間の始点として時刻だけ使い、値は含めない。 */
        int64_t newest = data->last_event;
        int64_t oldest = newest;
        int32_t sum[SI_AXES] = {0};
        int32_t prev_value[SI_AXES] = {0};
        bool have_prev = false;

        for (int k = 1; k <= data->count; k++) {
            const struct si_sample *s = &data->history[(data->head + SI_HISTORY - k) % SI_HISTORY];
            if (newest - s->time > cfg->window_ms) {
                break;
            }
            if (have_prev) {
                for (int i = 0; i < SI_AXES; i++) {
                    sum[i] += prev_value[i];
                }
            }
            for (int i = 0; i < SI_AXES; i++) {
                prev_value[i] = s->value[i];
            }
            oldest = s->time;
            have_prev = true;
        }
        data->count = 0;

        int64_t span = newest - oldest;
        if (span <= 0) {
            K_SPINLOCK_BREAK;
        }

        /* 主に動かしていた軸だけに慣性を付ける */
        int major = abs(sum[1]) >= abs(sum[0]) ? 1 : 0;
        for (int i = 0; i < SI_AXES; i++) {
            if (i != major) {
                continue;
            }
            /* 入力単位/秒 → 送信 milli-units/秒 */
            int64_t v = (int64_t)sum[i] * 1000 * 1000 / span;
            v = v * cfg->gain / 1000 / cfg->divisor;
            if (cfg->max_velocity > 0) {
                v = CLAMP(v, -(int64_t)cfg->max_velocity * 1000, (int64_t)cfg->max_velocity * 1000);
            }
            data->inertia_v[i] = (int32_t)CLAMP(v, -INT32_MAX / 1000, INT32_MAX / 1000);
            if (abs(data->inertia_v[i]) >= cfg->start_velocity * 1000) {
                start = true;
            }
        }
        if (!start) {
            si_stop_locked(data);
        }
        data->active = start;
    }

    if (start) {
        /* 途切れ判定で待った分、すぐに1tick目を送る */
        k_work_reschedule(&data->tick_work, K_NO_WAIT);
    }
}

static int si_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                           uint32_t param2, struct zmk_input_processor_state *state) {
    const struct si_config *cfg = dev->config;
    struct si_data *data = dev->data;
    int axis;

    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    if (event->code == INPUT_REL_HWHEEL) {
        axis = 0;
    } else if (event->code == INPUT_REL_WHEEL) {
        axis = 1;
    } else {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    bool was_active = false;
    int64_t now = k_uptime_get();

    K_SPINLOCK(&data->lock) {
        was_active = data->active;
        if (was_active) {
            si_stop_locked(data);
        }

        /* 入力が途切れていた (= 新しく触った) なら計測をやり直す */
        if (now - data->last_event > cfg->release_ms) {
            data->count = 0;
            data->head = 0;
            data->pending[0] = data->pending[1] = 0;
            data->stroke_start = now;
        }
        data->last_event = now;
        data->pending[axis] += event->value;

        /* センサーの1レポート (sync まで) を1件として記録する */
        if (event->sync) {
            struct si_sample *s = &data->history[data->head];
            s->time = now;
            s->value[0] = data->pending[0];
            s->value[1] = data->pending[1];
            data->pending[0] = data->pending[1] = 0;
            data->head = (data->head + 1) % SI_HISTORY;
            if (data->count < SI_HISTORY) {
                data->count++;
            }
        }
    }

    if (was_active) {
        k_work_cancel_delayable(&data->tick_work);
    }
    k_work_reschedule(&data->release_work, K_MSEC(cfg->release_ms));

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api si_driver_api = {
    .handle_event = si_handle_event,
};

static int si_init(const struct device *dev) {
    struct si_data *data = dev->data;

    data->dev = dev;
    k_work_init_delayable(&data->release_work, si_release_handler);
    k_work_init_delayable(&data->tick_work, si_tick_handler);

    return 0;
}

#define SI_INST(n)                                                                                 \
    BUILD_ASSERT(DT_INST_PROP(n, divisor) > 0, "divisor must be positive");                        \
    static struct si_data si_data_##n;                                                             \
    static const struct si_config si_config_##n = {                                                \
        .release_ms = DT_INST_PROP(n, release_ms),                                                 \
        .window_ms = DT_INST_PROP(n, window_ms),                                                   \
        .min_stroke_ms = DT_INST_PROP(n, min_stroke_ms),                                           \
        .tick_ms = DT_INST_PROP(n, tick_ms),                                                       \
        .divisor = DT_INST_PROP(n, divisor),                                                       \
        .decay = DT_INST_PROP(n, decay),                                                           \
        .gain = DT_INST_PROP(n, gain),                                                             \
        .start_velocity = DT_INST_PROP(n, start_velocity),                                         \
        .max_velocity = DT_INST_PROP(n, max_velocity),                                             \
        .stop_velocity = DT_INST_PROP(n, stop_velocity),                                           \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, &si_init, NULL, &si_data_##n, &si_config_##n, POST_KERNEL,             \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &si_driver_api);

DT_INST_FOREACH_STATUS_OKAY(SI_INST)

/* キーを押したら慣性を止める */

#define SI_DEV_REF(n) DEVICE_DT_INST_GET(n),

static const struct device *const si_devices[] = {DT_INST_FOREACH_STATUS_OKAY(SI_DEV_REF)};

static int si_position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev != NULL && ev->state) {
        for (size_t i = 0; i < ARRAY_SIZE(si_devices); i++) {
            si_stop(si_devices[i]);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(scroll_inertia, si_position_listener);
ZMK_SUBSCRIPTION(scroll_inertia, zmk_position_state_changed);
