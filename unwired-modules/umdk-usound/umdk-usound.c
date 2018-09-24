/*
 * Copyright (C) 2016 Unwired Devices [info@unwds.com]
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @defgroup    
 * @ingroup     
 * @brief       
 * @{
 * @file        umdk-usound.c
 * @brief       umdk-usound module implementation
 * @author      Dmitry Golik <info@unwds.com>
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

/* define is autogenerated, do not change */
#undef _UMDK_MID_
#define _UMDK_MID_ UNWDS_USOUND_MODULE_ID

/* define is autogenerated, do not change */
#undef _UMDK_NAME_
#define _UMDK_NAME_ "usound"

#include "periph/gpio.h"

#include "board.h"

#include "ultrasoundrange.h"

#include "unwds-common.h"
#include "umdk-usound.h"
#include "unwds-gpio.h"

#include "thread.h"
#include "rtctimers-millis.h"

static ultrasoundrange_t dev;

static uwnds_cb_t *callback;

static kernel_pid_t timer_pid;
static kernel_pid_t timer_24hrs_pid;

static msg_t timer_msg = {};
static msg_t timer_24hrs_msg = {};
static rtctimers_millis_t timer;
static rtctimers_millis_t timer_24hrs;

static bool is_polled = false;

#define UMDK_USOUND_TRANSMIT_PULSES     10
#define UMDK_USOUND_SILENCING_PULSES    5
#define UMDK_USOUND_PERIOD_US           790
#define UMDK_USOUND_SILENCING_PERIOD_US 800
#define UMDK_USOUND_IDLE_PERIOD_US      315
#define UMDK_USOUND_DUTY                350
#define UMDK_USOUND_DUTY2               300

static struct {
    uint8_t is_valid;
    uint8_t publish_period_min;
    uint16_t sensitivity;
    uint16_t min_distance;
    uint16_t max_distance;
    uint16_t threshold;
    uint8_t  mode;
} ultrasoundrange_config;

static bool init_sensor(void) {
    
    printf("[umdk-" _UMDK_NAME_ "] Initializing ultrasound distance meter\n");

    bool o = ultrasoundrange_init(&dev) == 0;

    dev.transmit_pulses = UMDK_USOUND_TRANSMIT_PULSES;
    dev.silencing_pulses = UMDK_USOUND_SILENCING_PULSES;
    dev.period_us = UMDK_USOUND_PERIOD_US;
    dev.silencing_period_us = UMDK_USOUND_SILENCING_PERIOD_US;
    dev.idle_period_us = UMDK_USOUND_IDLE_PERIOD_US;
    dev.duty = UMDK_USOUND_DUTY;
    dev.duty2 = UMDK_USOUND_DUTY2;
    dev.sensitivity = ultrasoundrange_config.sensitivity;
    dev.min_distance = ultrasoundrange_config.min_distance;
    dev.max_distance = ultrasoundrange_config.max_distance;
    dev.disrupting_pin = GPIO_PIN(PORT_A, 4);
    dev.silencing_pin = GPIO_PIN(PORT_A, 2);
    dev.beeping_pin = GPIO_PIN(PORT_A, 3);
//    dev.sens_pin = GPIO_PIN(PORT_A, 1);
    dev.adc_pin = GPIO_PIN(PORT_A, 5);
    return o;

}

static int prepare_result(module_data_t *buf) {
    /* enable power and wait for it to stabilize */
    gpio_clear(UMDK_USOUND_PWREN);
    rtctimers_millis_sleep(500);
    
    ultrasoundrange_measure_t measure = {};
    ultrasoundrange_measure(&dev, &measure);
    
    gpio_set(UMDK_USOUND_PWREN);
    
    int range;
    range = measure.range;
    
    printf("[umdk-" _UMDK_NAME_ "] Echo distance %d mm\n", range);

    if (buf) {
        buf->length = 1 + sizeof(range); /* Additional byte for module ID */
        buf->data[0] = _UMDK_MID_;
        /* Copy measurements into response */
        memcpy(buf->data + 1, (uint8_t *) &range, sizeof(range));
    }
    
    return range;
}

static void *timer_thread(void *arg) {
    (void)arg;
    
    msg_t msg;
    msg_t msg_queue[4];
    msg_init_queue(msg_queue, 4);
    
    puts("[umdk-" _UMDK_NAME_ "] Periodic publisher thread started");

    while (1) {
        msg_receive(&msg);

        module_data_t data = {};
        data.as_ack = is_polled;
        is_polled = false;

        int range = prepare_result(&data);

        if ((ultrasoundrange_config.mode == UMDK_SOUND_MODE_DISTANCE) ||
            ((ultrasoundrange_config.mode == UMDK_SOUND_MODE_THRESHOLD) &&
                (range < ultrasoundrange_config.threshold) && (range > 0))) {
                    
            /* Notify the application */
            callback(&data);
        } else {
            puts("[umdk-" _UMDK_NAME_ "] Distance above threshold, ignoring");
        }

        /* Restart after delay */
        if (ultrasoundrange_config.publish_period_min) {
            rtctimers_millis_set_msg(&timer, 60 * 1000 * ultrasoundrange_config.publish_period_min, &timer_msg, timer_pid);
        }
    }

    return NULL;
}

static void *timer_24hrs_thread(void *arg) {
    (void)arg;
    msg_t msg;
    msg_t msg_queue[4];
    msg_init_queue(msg_queue, 4);
    
    puts("[umdk-" _UMDK_NAME_ "] 24 hrs publisher thread started");

    while (1) {
        msg_receive(&msg);
        
        /* Send empty data every 24 hrs to check device's status */

        module_data_t data = {};
        data.as_ack = false;
        data.length = 5;
        data.data[0] = _UMDK_MID_;
        memset(&data.data[1], 0, 4);
        
        callback(&data);
        
        /* Restart after delay */
        rtctimers_millis_set_msg(&timer_24hrs, 1000 * 60*60*24, &timer_24hrs_msg, timer_24hrs_pid);
    }

    return NULL;
}

static inline void save_config(void) {
    ultrasoundrange_config.is_valid = 1;
    unwds_write_nvram_config(_UMDK_MID_, (uint8_t *) &ultrasoundrange_config, sizeof(ultrasoundrange_config));
}

static void reset_config(void) {
    ultrasoundrange_config.publish_period_min = 15;
    ultrasoundrange_config.sensitivity = 50;
    ultrasoundrange_config.min_distance = 400;
    ultrasoundrange_config.max_distance = 6000;
    ultrasoundrange_config.threshold = 500;
    ultrasoundrange_config.mode = UMDK_SOUND_MODE_DISTANCE;
    
    save_config();
    return;
}

static void init_config(void) {
    if (!unwds_read_nvram_config(_UMDK_MID_, (uint8_t *) &ultrasoundrange_config, sizeof(ultrasoundrange_config))) {
        reset_config();
        return;
    }

    if ((ultrasoundrange_config.is_valid == 0xFF) || (ultrasoundrange_config.is_valid == 0)) {
        reset_config();
        return;
    }
}

static void set_period (int period) {
    rtctimers_millis_remove(&timer);

    ultrasoundrange_config.publish_period_min = period;
    save_config();

    /* Don't restart timer if new period is zero */
    if (ultrasoundrange_config.publish_period_min) {
        rtctimers_millis_set_msg(&timer, 60 * 1000 * ultrasoundrange_config.publish_period_min, &timer_msg, timer_pid);
        printf("[umdk-" _UMDK_NAME_ "] Period set to %d minutes\n", ultrasoundrange_config.publish_period_min);
    } else {
        printf("[umdk-" _UMDK_NAME_ "] Timer stopped");
    }
}

static void umdk_usound_print_settings(void) {
    puts("[umdk-" _UMDK_NAME_ "] Current settings:");
    printf("period: %d m\n", ultrasoundrange_config.publish_period_min);
    printf("sens: %d\n", ultrasoundrange_config.sensitivity);
    printf("min: %d mm\n", ultrasoundrange_config.min_distance);
    printf("max: %d mm\n", ultrasoundrange_config.max_distance);
    printf("mode: %s\n", (ultrasoundrange_config.mode == UMDK_SOUND_MODE_DISTANCE)? "distance":"threshold");
    printf("threshold: %d mm\n", ultrasoundrange_config.threshold);
}

int umdk_usound_shell_cmd(int argc, char **argv) {
    if (argc == 1) {
        puts ("usound - ultrasound rangefinder");
        puts ("usound get - get results now");
        puts ("usound send - get and send results now");
        puts ("usound period <N> - set period to N minutes");
        puts ("usound sens <N> - set echo detection sensitivity");
        puts ("usound min <N> - set minimum distance in mm");
        puts ("usound max <N> - set maximum distance in mm");
        puts ("usound mode <distance|threshold> - set sensor mode");
        puts ("usound threshold <N> - set threshold in mm for threshold mode");
        puts ("usound reset - reset settings to default\n");

        umdk_usound_print_settings();
        return 0;
    }
    
    char *cmd = argv[1];
    
    if (strcmp(cmd, "get") == 0) {
        prepare_result(NULL);
        return 1;
    }
    
    if (strcmp(cmd, "send") == 0) {
        is_polled = true;
        /* Send signal to publisher thread */
        msg_send(&timer_msg, timer_pid);
        return 1;
    }
    
    if (strcmp(cmd, "period") == 0) {
        int val = atoi(argv[2]);
        set_period(val);
        return 1;
    }
    
    if (strcmp(cmd, "sens") == 0) {
        int val = atoi(argv[2]);
        ultrasoundrange_config . sensitivity = val;
        dev                    . sensitivity = val;
        save_config();
        return 1;
    }
    
    if (strcmp(cmd, "min") == 0) {
        int val = atoi(argv[2]);
        ultrasoundrange_config . min_distance = val;
        dev                    . min_distance = val;
        save_config();
        return 1;
    }
    
    if (strcmp(cmd, "max") == 0) {
        int val = atoi(argv[2]);
        ultrasoundrange_config . max_distance = val;
        dev                    . max_distance = val;
        save_config();
        return 1;
    }
    
    if (strcmp(cmd, "threshold") == 0) {
        int val = atoi(argv[2]);
        ultrasoundrange_config . threshold = val;
        save_config();
        return 1;
    }
    
    if (strcmp(cmd, "mode") == 0) {
        if (strcmp(argv[2], "threshold") == 0) {
            puts("[umdk-" _UMDK_NAME_ "] Threshold mode");
            ultrasoundrange_config.mode = UMDK_SOUND_MODE_THRESHOLD;
        } else {
            if (strcmp(argv[2], "distance") == 0) {
                ultrasoundrange_config.mode = UMDK_SOUND_MODE_DISTANCE;
                puts("[umdk-" _UMDK_NAME_ "] Distance mode");
            } else {
                puts("[umdk-" _UMDK_NAME_ "] Unknown mode");
            }
        }
        save_config();
        return 1;
    }

    if (strcmp(cmd, "reset") == 0) {
        reset_config();
        save_config();
        return 1;
    }

    puts("[umdk-" _UMDK_NAME_ "] Unknown command");
    
    return 1;
}

void umdk_usound_init(uint32_t *non_gpio_pin_map, uwnds_cb_t *event_callback) {
    (void) non_gpio_pin_map;

    callback = event_callback;
    
    gpio_init(UMDK_USOUND_PWREN, GPIO_OUT);
    gpio_set(UMDK_USOUND_PWREN);

    init_config();
    umdk_usound_print_settings();

    if (!init_sensor()) {
        printf("[umdk-" _UMDK_NAME_ "] Unable to init sensor!");
        return;
    }
    
	/* Create handler thread */
	char *stack_24hrs = (char *) allocate_stack(UMDK_USOUND_STACK_SIZE);
	if (!stack_24hrs) {
		return;
	}
    
    timer_24hrs_pid = thread_create(stack_24hrs, UMDK_USOUND_STACK_SIZE, THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, timer_24hrs_thread, NULL, "usound 24hrs thread");
    
    /* Start 24 hrs timer */
    rtctimers_millis_set_msg(&timer_24hrs, 1000 * 60 * 60 * 24, &timer_24hrs_msg, timer_24hrs_pid);

    char *stack = (char *) allocate_stack(UMDK_USOUND_STACK_SIZE);
	if (!stack) {
		return;
	}
    
    timer_pid = thread_create(stack, UMDK_USOUND_STACK_SIZE, THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, timer_thread, NULL, "usound thread");
    
    
    /* Start publishing timer */
    if (ultrasoundrange_config.publish_period_min) {
        rtctimers_millis_set_msg(&timer, ultrasoundrange_config.publish_period_min * 1000 * 60, &timer_msg, timer_pid);
    }
    
    unwds_add_shell_command("usound", "type 'usound' for commands list", umdk_usound_shell_cmd);
}

static void reply_fail(module_data_t *reply) {
    reply->length = 2;
    reply->data[0] = _UMDK_MID_;
    reply->data[1] = 255;
}

static void reply_ok(module_data_t *reply) {
    reply->length = 2;
    reply->data[0] = _UMDK_MID_;
    reply->data[1] = 0;
}

bool umdk_usound_cmd(module_data_t *cmd, module_data_t *reply) {
    if (cmd->length < 1) {
        reply_fail(reply);
        return true;
    }

    umdk_usound_cmd_t c = cmd->data[0];
    switch (c) {
    case UMDK_USOUND_CMD_SET_PERIOD: {
        if (cmd->length != 2) {
            reply_fail(reply);
            break;
        }

        uint8_t period = cmd->data[1];
        set_period(period);

        reply_ok(reply);
        break;
    }

    case UMDK_USOUND_CMD_POLL:
        is_polled = true;

        /* Send signal to publisher thread */
        msg_send(&timer_msg, timer_pid);

        return false; /* Don't reply */

    case UMDK_USOUND_CMD_INIT_SENSOR: {
        init_sensor();

        reply_ok(reply);
        break;
    }

    default:
        reply_fail(reply);
        break;
    }

    return true;
}

#ifdef __cplusplus
}
#endif
