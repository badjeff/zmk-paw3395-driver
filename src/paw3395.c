/*
 * Copyright (c) 2025 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT pixart_paw3395

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/input/input.h>
#include <zephyr/pm/device.h>
#include <zmk/keymap.h>
#include <zmk/events/activity_state_changed.h>
#include "paw3395.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(paw3395, CONFIG_PAW3395_LOG_LEVEL);

//////// PAW3395 static library header
#include "paw3395/include/paw3395.h"

// Custom ERROR loggers for PAW3395 static library
void paw3395_lib_log_err(const char *fmt, ...) {
#if CONFIG_PAW3395_LOG_LEVEL >= 0
    va_list args;
    va_start(args, fmt);
    char buf[128];
    vsnprintf(buf, sizeof(buf), fmt, args);
    LOG_ERR("PAW3395_LIB: %s", buf);
    va_end(args);
#endif
}

// Custom INFO loggers for PAW3395 static library
void paw3395_lib_log_inf(const char *fmt, ...) {
#if CONFIG_PAW3395_LOG_LEVEL >= 3
    va_list args;
    va_start(args, fmt);
    char buf[128];
    vsnprintf(buf, sizeof(buf), fmt, args);
    LOG_INF("PAW3395_LIB: %s", buf);
    va_end(args);
#endif
}


//////// Sensor initialization steps definition //////////
// init is done in non-blocking manner (i.e., async), a //
// delayable work is defined for this purpose           //
enum async_init_step {
    ASYNC_INIT_STEP_POWER_UP,         // power up reset
    ASYNC_INIT_STEP_FW_LOAD_START,    // load power-up initialzation settings, clear motion registers,
    ASYNC_INIT_STEP_CONFIGURE,        // set cpi and donwshift time (run, rest1, rest2)
    ASYNC_INIT_STEP_COUNT             // end flag
};

/* Timings (in ms) needed in between steps to allow each step finishes succussfully. */
// - Since MCU is not involved in the sensor init process, i is allowed to do other tasks.
//   Thus, k_sleep or delayed schedule can be used.
static const int32_t async_init_delay[ASYNC_INIT_STEP_COUNT] = {
    [ASYNC_INIT_STEP_POWER_UP] = 50 + CONFIG_PAW3395_INIT_POWER_UP_EXTRA_DELAY_MS,
    [ASYNC_INIT_STEP_FW_LOAD_START] = 5,
    [ASYNC_INIT_STEP_CONFIGURE] = 1,
};

static int paw3395_async_init_power_up(const struct device *dev);
static int paw3395_async_init_fw_load(const struct device *dev);
static int paw3395_async_init_configure(const struct device *dev);

static int (*const async_init_fn[ASYNC_INIT_STEP_COUNT])(const struct device *dev) = {
    [ASYNC_INIT_STEP_POWER_UP] = paw3395_async_init_power_up,
    [ASYNC_INIT_STEP_FW_LOAD_START] = paw3395_async_init_fw_load,
    [ASYNC_INIT_STEP_CONFIGURE] = paw3395_async_init_configure,
};

//////// Function definitions //////////

static int paw3395_async_init_fw_load(const struct device *dev) {
    int err = 0;
    const struct pixart_config *config = dev->config;

    // verify product id before upload power-up initialization settings
    err = paw3395_lib_verify_product_id(config->spi, config->cs_gpio);
    if (err) {
        LOG_ERR("Cannot exec paw3395_lib_verify_product_id");
        return -EIO;
    }
    LOG_INF("product id verified");

    err = paw3395_lib_power_up_init_regs(config->spi, config->cs_gpio);
    if (err) {
        LOG_ERR("Cannot exec paw3395_lib_power_up_init_regs");
        return err;
    }
    LOG_INF("power up init regs done");
    
    err = paw3395_lib_clear_motion_pin_state(config->spi, config->cs_gpio);
    if (err) {
        LOG_ERR("Cannot exec paw3395_lib_clear_motion_pin_state");
        return err;
    }
    LOG_INF("clear motion pin state");

    return err;
}

static int paw3395_set_performance(const struct device *dev, bool enabled) {
    const struct pixart_config *config = dev->config;
    int err = 0;

    if (config->force_awake) {
        err = paw3395_lib_set_performance(config->spi, config->cs_gpio, enabled);
        if (err) {
            LOG_ERR("Cannot exec paw3395_lib_set_performance");
            return err;
        }
        LOG_INF("%s performance mode", enabled ? "enable" : "disable");
    }

    return err;
}

static int paw3395_set_interrupt(const struct device *dev, const bool en) {
    const struct pixart_config *config = dev->config;
    int ret = gpio_pin_interrupt_configure_dt(&config->irq_gpio,
                                              en ? GPIO_INT_LEVEL_ACTIVE : GPIO_INT_DISABLE);
    if (ret < 0) {
        LOG_ERR("can't set interrupt");
    }
    return ret;
}

static int paw3395_async_init_power_up(const struct device *dev) {
    int err = 0;
    const struct pixart_config *config = dev->config;

    err = paw3395_lib_power_up_reset(config->spi, config->cs_gpio);
    if (err) {
        LOG_ERR("Cannot exec paw3395_lib_power_up_reset");
        return -EIO;
    }
    LOG_INF("power up reset done");

    return err;
}

static int paw3395_async_init_configure(const struct device *dev) {
    int err = 0;
    const struct pixart_config *config = dev->config;

    err = paw3395_set_performance(dev, true);

    err = paw3395_lib_set_cpi(config->spi, config->cs_gpio, config->cpi);
    if (err < 0) {
        LOG_ERR("can't set cpi");
        return err;
    }
    LOG_INF("set cpi done");

    return err;
}

static void paw3395_async_init(struct k_work *work) {
    struct k_work_delayable *work_delayable = (struct k_work_delayable *)work;
    struct pixart_data *data = CONTAINER_OF(work_delayable, struct pixart_data, init_work);
    const struct device *dev = data->dev;

    LOG_INF("PAW3395 async init step %d", data->async_init_step);

    data->err = async_init_fn[data->async_init_step](dev);
    if (data->err) {
        LOG_ERR("PAW3395 initialization failed in step %d", data->async_init_step);
    } else {
        data->async_init_step++;

        if (data->async_init_step == ASYNC_INIT_STEP_COUNT) {
            data->ready = true; // sensor is ready to work
            LOG_INF("PAW3395 initialized");
            paw3395_set_interrupt(dev, true);
        } else {
            k_work_schedule(&data->init_work, K_MSEC(async_init_delay[data->async_init_step]));
        }
    }
}

static int paw3395_report_data(const struct device *dev) {
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;
    uint8_t buf[PAW3395_BURST_SIZE];

    if (unlikely(!data->ready)) {
        LOG_WRN("Device is not initialized yet");
        return -EBUSY;
    }

    static int64_t dx = 0;
    static int64_t dy = 0;

#if CONFIG_PAW3395_REPORT_INTERVAL_MIN > 0
    static int64_t last_smp_time = 0;
    static int64_t last_rpt_time = 0;
    int64_t now = k_uptime_get();
#endif

    int err = 0;
    err = paw3395_lib_motion_burst_read(config->spi, config->cs_gpio, buf, PAW3395_BURST_SIZE);
    if (err) {
        return err;
    }

    // LOG_HEXDUMP_DBG(buf, sizeof(buf), "buf");
    int16_t x = ((int16_t)sys_get_le16(&buf[PAW3395_DX_POS]));
    int16_t y = ((int16_t)sys_get_le16(&buf[PAW3395_DY_POS]));

    if (!x && !y) {
        // LOG_DBG("skip reporting zero x/y");
        return 0;
    }

// #ifdef PAW3395_SQUAL_POS
//     LOG_DBG("motion_burst_read, X: 0x%x 0x%x, Y: 0x%x 0x%x, %d, %d, SQ: %d", 
//         buf[PAW3395_DX_POS+1], buf[PAW3395_DX_POS],
//         buf[PAW3395_DY_POS+1], buf[PAW3395_DY_POS],
//         x, y, (uint8_t)buf[PAW3395_SQUAL_POS]);
// #else
//     LOG_DBG("motion_burst_read, X: 0x%x 0x%x, Y: 0x%x 0x%x, %d, %d",
//         buf[PAW3395_DX_POS+1], buf[PAW3395_DX_POS],
//         buf[PAW3395_DY_POS+1], buf[PAW3395_DY_POS],
//         x, y);
// #endif

#if IS_ENABLED(CONFIG_PAW3395_SWAP_XY)
    int16_t a = x;
    x = y;
    y = a;
#endif
#if IS_ENABLED(CONFIG_PAW3395_INVERT_X)
    x = -x;
#endif
#if IS_ENABLED(CONFIG_PAW3395_INVERT_Y)
    y = -y;
#endif

#if CONFIG_PAW3395_REPORT_INTERVAL_MIN > 0
    // purge accumulated delta, if last sampled had not been reported on last report tick
    if (now - last_smp_time >= CONFIG_PAW3395_REPORT_INTERVAL_MIN) {
        dx = 0;
        dy = 0;
    }
    last_smp_time = now;
#endif

    // accumulate delta until report in next iteration
    dx += x;
    dy += y;

#if CONFIG_PAW3395_REPORT_INTERVAL_MIN > 0
    // strict to report inerval
    if (now - last_rpt_time < CONFIG_PAW3395_REPORT_INTERVAL_MIN) {
        return 0;
    }
#endif

    // divide to report value
    int16_t rx = (int16_t)CLAMP(dx, INT16_MIN, INT16_MAX);
    int16_t ry = (int16_t)CLAMP(dy, INT16_MIN, INT16_MAX);
    bool have_x = rx != 0;
    bool have_y = ry != 0;

    if (have_x || have_y) {
#if CONFIG_PAW3395_REPORT_INTERVAL_MIN > 0
        last_rpt_time = now;
#endif
        dx = 0;
        dy = 0;
        if (have_x) {
            input_report(dev, config->evt_type, config->x_input_code, rx, !have_y, K_NO_WAIT);
        }
        if (have_y) {
            input_report(dev, config->evt_type, config->y_input_code, ry, true, K_NO_WAIT);
        }
    }

    return err;
}

static void paw3395_gpio_callback(const struct device *gpiob, struct gpio_callback *cb,
                                  uint32_t pins) {
    struct pixart_data *data = CONTAINER_OF(cb, struct pixart_data, irq_gpio_cb);
    const struct device *dev = data->dev;
    paw3395_set_interrupt(dev, false);
    k_work_submit(&data->trigger_work);
}

static void paw3395_work_callback(struct k_work *work) {
    struct pixart_data *data = CONTAINER_OF(work, struct pixart_data, trigger_work);
    const struct device *dev = data->dev;
    paw3395_report_data(dev);
    paw3395_set_interrupt(dev, true);
}

static int paw3395_init_irq(const struct device *dev) {
    int err = 0;
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;

    // check readiness of irq gpio pin
    if (!device_is_ready(config->irq_gpio.port)) {
        LOG_ERR("IRQ GPIO device not ready");
        return -ENODEV;
    }

    // init the irq pin
    err = gpio_pin_configure_dt(&config->irq_gpio, GPIO_INPUT);
    if (err) {
        LOG_ERR("Cannot configure IRQ GPIO");
        return err;
    }

    // setup and add the irq callback associated
    gpio_init_callback(&data->irq_gpio_cb, paw3395_gpio_callback, BIT(config->irq_gpio.pin));

    err = gpio_add_callback(config->irq_gpio.port, &data->irq_gpio_cb);
    if (err) {
        LOG_ERR("Cannot add IRQ GPIO callback");
    }

    return err;
}

static int paw3395_init(const struct device *dev) {
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;
    int err = 0;

    // init device pointer
    data->dev = dev;

    // init trigger handler work
    k_work_init(&data->trigger_work, paw3395_work_callback);

    // check readiness of cs gpio pin and init it to inactive
    if (!device_is_ready(config->cs_gpio.port)) {
        LOG_ERR("SPI CS device not ready");
        return -ENODEV;
    }

    err = gpio_pin_configure_dt(&config->cs_gpio, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("Cannot configure SPI CS GPIO");
        return err;
    }

    // init irq routine
    err = paw3395_init_irq(dev);
    if (err) {
        return err;
    }

    // Setup delayable and non-blocking init jobs, including following steps:
    // 1. power reset
    // 2. clear motion registers
    // 3. srom firmware download and checking
    // 4. eable rest mode
    // 5. set cpi and downshift time and sample rate
    // The sensor is ready to work (i.e., data->ready=true after the above steps are finished)
    k_work_init_delayable(&data->init_work, paw3395_async_init);

    k_work_schedule(&data->init_work, K_MSEC(async_init_delay[data->async_init_step]));

    return err;
}

static int paw3395_attr_set(const struct device *dev, enum sensor_channel chan,
                            enum sensor_attribute attr, const struct sensor_value *val) {
    int err = 0;
    struct pixart_data *data = dev->data;
    const struct pixart_config *config = dev->config;

    if (unlikely(chan != SENSOR_CHAN_ALL)) {
        return -ENOTSUP;
    }

    if (unlikely(!data->ready)) {
        LOG_DBG("Device is not initialized yet");
        return -EBUSY;
    }

    switch ((uint32_t)attr) {
    case PAW3395_ATTR_CPI:
        err = paw3395_lib_set_cpi(config->spi, config->cs_gpio, PAW3395_SVALUE_TO_CPI(*val));
        break;

    default:
        LOG_ERR("Unknown attribute");
        err = -ENOTSUP;
    }

    return err;
}

static const struct sensor_driver_api paw3395_driver_api = {
    .attr_set = paw3395_attr_set,
};

// #if IS_ENABLED(CONFIG_PM_DEVICE)
// static int paw3395_pm_action(const struct device *dev, enum pm_device_action action) {
//     switch (action) {
//     case PM_DEVICE_ACTION_SUSPEND:
//         return paw3395_set_interrupt(dev, false);
//     case PM_DEVICE_ACTION_RESUME:
//         return paw3395_set_interrupt(dev, true);
//     default:
//         return -ENOTSUP;
//     }
// }
// #endif // IS_ENABLED(CONFIG_PM_DEVICE)
// PM_DEVICE_DT_INST_DEFINE(n, paw3395_pm_action);

#define PAW3395_SPI_MODE (SPI_WORD_SET(8) | SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_TRANSFER_MSB)

#define PAW3395_DEFINE(n)                                                                          \
    static struct pixart_data data##n;                                                             \
    static const struct pixart_config config##n = {                                                \
        .spi = {                                                                                   \
            .bus = DEVICE_DT_GET(DT_INST_BUS(n)),                                                  \
            .config = {                                                                            \
                .frequency = DT_INST_PROP(n, spi_max_frequency),                                   \
                .operation = PAW3395_SPI_MODE,                                                     \
                .slave = DT_INST_REG_ADDR(n),                                                      \
             },                                                                                    \
        },                                                                                         \
        .cs_gpio = SPI_CS_GPIOS_DT_SPEC_GET(DT_DRV_INST(n)),                                       \
        .irq_gpio = GPIO_DT_SPEC_INST_GET(n, irq_gpios),                                           \
        .cpi = DT_PROP(DT_DRV_INST(n), cpi),                                                       \
        .evt_type = DT_PROP(DT_DRV_INST(n), evt_type),                                             \
        .x_input_code = DT_PROP(DT_DRV_INST(n), x_input_code),                                     \
        .y_input_code = DT_PROP(DT_DRV_INST(n), y_input_code),                                     \
        .force_awake = DT_PROP(DT_DRV_INST(n), force_awake),                                       \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, paw3395_init, NULL, &data##n, &config##n, POST_KERNEL,                \
                          CONFIG_INPUT_PAW3395_INIT_PRIORITY, &paw3395_driver_api);

DT_INST_FOREACH_STATUS_OKAY(PAW3395_DEFINE)


#define GET_PAW3395_DEV(node_id) DEVICE_DT_GET(node_id),

static const struct device *paw3395_devs[] = {
    DT_FOREACH_STATUS_OKAY(pixart_paw3395, GET_PAW3395_DEV)
};

static int on_activity_state(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *state_ev = as_zmk_activity_state_changed(eh);

    if (!state_ev) {
        LOG_WRN("NO EVENT, leaving early");
        return 0;
    }

    bool enable = state_ev->state == ZMK_ACTIVITY_ACTIVE ? 1 : 0;
    for (size_t i = 0; i < ARRAY_SIZE(paw3395_devs); i++) {
        paw3395_set_performance(paw3395_devs[i], enable);
    }

    return 0;
}

ZMK_LISTENER(zmk_paw3395_idle_sleeper, on_activity_state);
ZMK_SUBSCRIPTION(zmk_paw3395_idle_sleeper, zmk_activity_state_changed);
