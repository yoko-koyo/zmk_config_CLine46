/*
 * CLine46: トラックボール用スクロール input processor
 *
 * センサーの生の REL_X / REL_Y を受け取り、スクロール (REL_WHEEL / REL_HWHEEL) に
 * 変換する。加速・軸固定・端数処理・慣性をまとめて扱う。
 *
 * 加速
 *   倍率は速度 v (counts/sec) に対して滑らかに変化する:
 *     scale(v) = slow + (fast - slow) * v^2 / (v^2 + half^2)
 *   しきい値による折れ目がないので、速度が変わっても急に量が変わらない。
 *
 * 端数処理
 *   スクロールは1段単位でしか送れないため、milli-units で蓄積して1段ごとに送る。
 *
 * 不感帯
 *   動き出し・反転・軸切り替えの直後は deadzone カウント分の移動を捨てる。
 *   ボールに軽く触れた時や指を離す時の揺れでスクロールしないようにする。
 *
 * 軸固定
 *   既定は縦。横の動きが縦の2倍以上かつ lock-margin 以上勝った時だけ横に切り替え、
 *   逆も同様 (ヒステリシス)。斜めに転がしても片方の軸にしか流れない。
 *
 * 慣性
 *   入力が release-ms 途切れたら手を離したとみなし、離す直前 window-ms に実際に
 *   送っていたスクロール速度から、時定数 inertia-tau-ms で減速させて送り続ける。
 *   再びボールを動かすか、キーを押すと即停止する。
 *   慣性分は input スレッドではなくシステムワークキューから HID レポートを直接送る
 *   (input スレッドはスタックが小さいため。input_processor_key_tapper.c を参照)。
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_trackball_scroll

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

#define AXIS_X 0
#define AXIS_Y 1

/* 速度計算用に保持するレポート数 (センサーは最短12ms間隔なので約190ms分) */
#define TS_HISTORY 16

struct ts_config {
    bool invert[2];
    int32_t slow_scale;      /* milli-units / count (遅い時) */
    int32_t fast_scale;      /* milli-units / count (速い時) */
    int32_t half_speed;      /* counts/sec。slow と fast の中間になる速度 */
    int32_t deadzone;        /* counts。動き出し・反転時に捨てる移動量 */
    int32_t lock_margin;     /* counts。軸を切り替えるのに必要な差 */
    uint32_t release_ms;
    uint32_t resume_ms;      /* この時間内の触り直しは前の操作の続きとして扱う */
    uint32_t window_ms;
    uint32_t min_stroke_ms;
    uint32_t tick_ms;
    int32_t tau_ms;          /* 慣性の減速の時定数 */
    int32_t quick_velocity;  /* units/sec。これ未満の慣性は時間を縮めて素早く滑り終える */
    int32_t start_velocity;  /* units/sec */
    int32_t max_velocity;    /* units/sec (0 = 上限なし) */
    int32_t stop_velocity;   /* units/sec */
};

struct ts_sample {
    int64_t time;
    int32_t milli; /* このレポートで積んだ量 (milli-units, 固定軸) */
};

struct ts_data {
    const struct device *dev;
    struct k_spinlock lock;
    struct k_work_delayable release_work;
    struct k_work_delayable tick_work;

    /* ストローク (触ってから離すまで) の状態 */
    int64_t stroke_start;
    int64_t last_event;
    int64_t last_sync;
    int32_t speed;       /* 平滑化した速度 (counts/sec) */
    int32_t report_peak; /* 現在のレポート内の瞬間速度の最大 */
    int32_t dom[2];      /* 軸ごとの直近の移動量 (軸固定の判定用, 減衰あり) */
    uint8_t axis;        /* 固定中の軸 */
    bool fresh;          /* 次の移動で不感帯をやり直すか */
    int32_t dz_left;     /* 不感帯の残り (counts) */
    int8_t dir;          /* 直近の移動方向 (+1 / -1) */
    int32_t acc;         /* 送信待ちの端数 (milli-units)。慣性にも引き継ぐ */

    int32_t pending;     /* 現在のレポートで積んだ量 */
    struct ts_sample history[TS_HISTORY];
    uint8_t head;
    uint8_t count;

    /* 慣性 */
    bool coasting;
    int32_t velocity; /* milli-units/sec */
    int32_t decay;    /* tick ごとの速度の残存率 (千分率)。慣性の開始ごとに決める */
};

static int32_t ts_scale(const struct ts_config *cfg, int32_t speed) {
    int64_t v2 = (int64_t)speed * speed;
    int64_t h2 = (int64_t)cfg->half_speed * cfg->half_speed;
    return cfg->slow_scale + (int32_t)((cfg->fast_scale - cfg->slow_scale) * v2 / (v2 + h2));
}

static void ts_send_scroll(uint8_t axis, int32_t units) {
    units = CLAMP(units, -127, 127);
    if (axis == AXIS_X) {
        zmk_hid_mouse_scroll_set(units, 0);
    } else {
        zmk_hid_mouse_scroll_set(0, units);
    }
    zmk_endpoints_send_mouse_report();
    zmk_hid_mouse_scroll_set(0, 0);
}

static void ts_stop_coast(const struct device *dev) {
    struct ts_data *data = dev->data;

    K_SPINLOCK(&data->lock) {
        data->coasting = false;
        data->velocity = 0;
    }
    k_work_cancel_delayable(&data->tick_work);
}

/* 慣性: 1tick 分を送る (システムワークキュー) */
static void ts_tick_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct ts_data *data = CONTAINER_OF(dwork, struct ts_data, tick_work);
    const struct ts_config *cfg = data->dev->config;
    int32_t out = 0;
    uint8_t axis = AXIS_Y;
    bool keep = false;

    K_SPINLOCK(&data->lock) {
        if (!data->coasting) {
            K_SPINLOCK_BREAK;
        }
        axis = data->axis;
        data->acc += (int32_t)((int64_t)data->velocity * cfg->tick_ms / 1000);
        out = data->acc / 1000;
        data->acc -= out * 1000;
        data->velocity = (int32_t)((int64_t)data->velocity * data->decay / 1000);
        keep = abs(data->velocity) >= cfg->stop_velocity * 1000;
        data->coasting = keep;
    }

    if (out != 0) {
        ts_send_scroll(axis, out);
    }
    if (keep) {
        k_work_reschedule(&data->tick_work, K_MSEC(cfg->tick_ms));
    }
}

/* 入力が途切れた: 慣性を始めるか判定する (システムワークキュー) */
static void ts_release_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct ts_data *data = CONTAINER_OF(dwork, struct ts_data, release_work);
    const struct ts_config *cfg = data->dev->config;
    bool start = false;

    K_SPINLOCK(&data->lock) {
        int64_t newest = data->last_event;
        int64_t oldest = newest;
        int64_t sum = 0;
        int32_t newer_milli = 0;
        bool have_newer = false;

        if (data->count < 3 || newest - data->stroke_start < cfg->min_stroke_ms) {
            K_SPINLOCK_BREAK;
        }

        /* 新しい方から window_ms 以内のレポートで平均速度を出す。
         * 最古のレポートは区間の始点として時刻だけ使う。 */
        for (int k = 1; k <= data->count; k++) {
            const struct ts_sample *s = &data->history[(data->head + TS_HISTORY - k) % TS_HISTORY];
            if (newest - s->time > cfg->window_ms) {
                break;
            }
            if (have_newer) {
                sum += newer_milli;
            }
            newer_milli = s->milli;
            oldest = s->time;
            have_newer = true;
        }

        int64_t span = newest - oldest;
        if (span <= 0) {
            K_SPINLOCK_BREAK;
        }

        int64_t v = sum * 1000 / span; /* milli-units/sec */

        /* 初速は「離す直前の速度 - start-velocity」。遅い動きほど慣性が控えめになり、
         * しきい値を境に急に滑り出すこともない */
        int64_t mag = llabs(v) - (int64_t)cfg->start_velocity * 1000;
        if (mag <= 0) {
            K_SPINLOCK_BREAK;
        }
        if (cfg->max_velocity > 0) {
            mag = MIN(mag, (int64_t)cfg->max_velocity * 1000);
        }
        /* 小さい慣性は、滑る距離 (≒ 初速 × tau) を保ったまま時間を縮める。
         * 初速が小さいと1段たまるまで時間がかかり、遅れて動くように感じるため */
        int64_t natural = mag; /* 時間を縮める前の初速 */
        int64_t tau = cfg->tau_ms;
        int64_t quick = (int64_t)cfg->quick_velocity * 1000;
        if (quick > 0 && mag < quick) {
            int64_t k = quick * 1000 / mag; /* 時間の縮小率 (千分率で何倍か) */
            tau = MAX(tau * 1000 / k, 2 * (int64_t)cfg->tick_ms);
            mag = mag * cfg->tau_ms / tau;
        }
        data->decay = (int32_t)MAX(0, 1000 - 1000 * (int64_t)cfg->tick_ms / tau);
        v = v > 0 ? mag : -mag;

        /* 反対向きの端数が残っていると慣性の出だしが遅れるので捨てる */
        if ((v > 0 && data->acc < 0) || (v < 0 && data->acc > 0)) {
            data->acc = 0;
        }
        data->velocity = (int32_t)v;

        /* 手を離したと判定するまで release-ms 待った分、進んでいたはずの量を先に積む。
         * これがないと、離してから慣性が始まるまでスクロールが一瞬止まって見える */
        int64_t gap = natural * (int64_t)cfg->release_ms / 1000;
        data->acc += (int32_t)(v > 0 ? gap : -gap);

        start = true;
        data->coasting = true;
    }

    if (start) {
        /* 途切れ判定で待った分、すぐに1tick目を送る */
        k_work_reschedule(&data->tick_work, K_NO_WAIT);
    }
}

/* 軸固定: 必要なら軸を切り替える。切り替えたら true */
static bool ts_update_axis(const struct ts_config *cfg, struct ts_data *data) {
    uint8_t other = data->axis == AXIS_X ? AXIS_Y : AXIS_X;

    if (data->dom[other] > data->dom[data->axis] * 2 + cfg->lock_margin) {
        data->axis = other;
        return true;
    }
    return false;
}

static int ts_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                           uint32_t param2, struct zmk_input_processor_state *state) {
    const struct ts_config *cfg = dev->config;
    struct ts_data *data = dev->data;
    uint8_t axis;

    if (event->type != INPUT_EV_REL) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    if (event->code == INPUT_REL_X) {
        axis = AXIS_X;
    } else if (event->code == INPUT_REL_Y) {
        axis = AXIS_Y;
    } else {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    bool was_coasting = false;
    int64_t now = k_uptime_get();
    int32_t value = cfg->invert[axis] ? -event->value : event->value;
    int32_t out = 0;
    uint8_t out_axis;

    K_SPINLOCK(&data->lock) {
        was_coasting = data->coasting;
        data->coasting = false;
        data->velocity = 0;

        /* 新しいストローク (触り直し) */
        if (now - data->last_event > cfg->release_ms) {
            data->stroke_start = now;
            data->last_sync = now - 12;
            data->speed = 0;
            data->report_peak = 0;
            data->pending = 0;
            data->count = 0;
            data->head = 0;

            /* resume-ms 以内の触り直しは小刻みな操作の続きとみなし、端数・軸・不感帯の
             * 状態を引き継ぐ (同じ向きなら不感帯なしで続きから積み上がる。
             * 反対向きなら下の反転処理で不感帯がかかる) */
            if (now - data->last_event > cfg->resume_ms) {
                data->dom[AXIS_X] = data->dom[AXIS_Y] = 0;
                data->axis = AXIS_Y;
                data->fresh = true;
                data->acc = 0;
            }
        }
        data->last_event = now;

        /* 速度 (counts/sec): レポート内の瞬間値の最大と、前回までの平滑値の平均 */
        int64_t dt = CLAMP(now - data->last_sync, 4, 100);
        int32_t inst = (int32_t)(abs(value) * 1000 / dt);
        data->report_peak = MAX(data->report_peak, inst);
        int32_t speed = data->speed == 0 ? data->report_peak
                                         : (data->speed + data->report_peak) / 2;

        /* 軸固定 */
        data->dom[axis] += abs(value);
        if (ts_update_axis(cfg, data)) {
            data->fresh = true;
            data->acc = 0;
        }

        /* 固定中の軸の移動だけをスクロール量に積む */
        if (axis == data->axis && value != 0) {
            int8_t dir = value > 0 ? 1 : -1;
            int32_t mag = abs(value);

            if (data->fresh || dir != data->dir) {
                data->acc = 0;
                data->dz_left = cfg->deadzone;
                data->fresh = false;
            }
            data->dir = dir;

            /* 不感帯を消化してから積む */
            int32_t eaten = MIN(mag, data->dz_left);
            data->dz_left -= eaten;
            mag -= eaten;

            int32_t add = dir * mag * ts_scale(cfg, speed);
            data->acc += add;
            data->pending += add;
        }

        out = data->acc / 1000;
        data->acc -= out * 1000;
        out_axis = data->axis;

        /* センサーの1レポート (sync まで) の締め */
        if (event->sync) {
            struct ts_sample *s = &data->history[data->head];
            s->time = now;
            s->milli = data->pending;
            data->head = (data->head + 1) % TS_HISTORY;
            if (data->count < TS_HISTORY) {
                data->count++;
            }
            data->pending = 0;
            data->speed = speed;
            data->report_peak = 0;
            data->last_sync = now;
            data->dom[AXIS_X] = data->dom[AXIS_X] * 3 / 4;
            data->dom[AXIS_Y] = data->dom[AXIS_Y] * 3 / 4;
        }
    }

    if (was_coasting) {
        k_work_cancel_delayable(&data->tick_work);
    }
    k_work_reschedule(&data->release_work, K_MSEC(cfg->release_ms));

    /* イベントをスクロールに書き換える (X/Y どちらのイベントでも固定軸の量を運ぶ) */
    event->code = out_axis == AXIS_X ? INPUT_REL_HWHEEL : INPUT_REL_WHEEL;
    event->value = out;

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api ts_driver_api = {
    .handle_event = ts_handle_event,
};

static int ts_init(const struct device *dev) {
    struct ts_data *data = dev->data;

    data->dev = dev;
    data->axis = AXIS_Y;
    data->dir = 1;
    k_work_init_delayable(&data->release_work, ts_release_handler);
    k_work_init_delayable(&data->tick_work, ts_tick_handler);

    return 0;
}

#define TS_INST(n)                                                                                 \
    BUILD_ASSERT(DT_INST_PROP(n, inertia_tau_ms) > DT_INST_PROP(n, tick_ms),                       \
                 "inertia-tau-ms must be longer than tick-ms");                                    \
    static struct ts_data ts_data_##n;                                                             \
    static const struct ts_config ts_config_##n = {                                                \
        .invert = {DT_INST_PROP(n, invert_x), DT_INST_PROP(n, invert_y)},                          \
        .slow_scale = DT_INST_PROP(n, slow_scale),                                                 \
        .fast_scale = DT_INST_PROP(n, fast_scale),                                                 \
        .half_speed = DT_INST_PROP(n, half_speed),                                                 \
        .deadzone = DT_INST_PROP(n, deadzone),                                                     \
        .lock_margin = DT_INST_PROP(n, lock_margin),                                               \
        .release_ms = DT_INST_PROP(n, release_ms),                                                 \
        .resume_ms = DT_INST_PROP(n, resume_ms),                                                   \
        .window_ms = DT_INST_PROP(n, window_ms),                                                   \
        .min_stroke_ms = DT_INST_PROP(n, min_stroke_ms),                                           \
        .tick_ms = DT_INST_PROP(n, tick_ms),                                                       \
        .tau_ms = DT_INST_PROP(n, inertia_tau_ms),                                                 \
        .quick_velocity = DT_INST_PROP(n, quick_velocity),                                         \
        .start_velocity = DT_INST_PROP(n, start_velocity),                                         \
        .max_velocity = DT_INST_PROP(n, max_velocity),                                             \
        .stop_velocity = DT_INST_PROP(n, stop_velocity),                                           \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, &ts_init, NULL, &ts_data_##n, &ts_config_##n, POST_KERNEL,             \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &ts_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TS_INST)

/* キーを押したら慣性を止める */

#define TS_DEV_REF(n) DEVICE_DT_INST_GET(n),

static const struct device *const ts_devices[] = {DT_INST_FOREACH_STATUS_OKAY(TS_DEV_REF)};

static int ts_position_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);

    if (ev != NULL && ev->state) {
        for (size_t i = 0; i < ARRAY_SIZE(ts_devices); i++) {
            ts_stop_coast(ts_devices[i]);
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(trackball_scroll, ts_position_listener);
ZMK_SUBSCRIPTION(trackball_scroll, zmk_position_state_changed);
