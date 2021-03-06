/*
 * Copyright (C) 2016-2018 Unwired Devices LLC <info@unwds.com>

 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software
 * is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
 * FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @defgroup
 * @ingroup
 * @brief
 * @{
 * @file        main-node.c
 * @brief       LoRaLAN node device
 * @author      Evgeniy Ponomarev
 * @author      Oleg Artamonov
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "thread.h"
#include "periph/pm.h"
#include "pm_layered.h"
#include "periph/rtc.h"
#include "periph/gpio.h"
#include "periph/adc.h"
#include "random.h"

#include "net/lora.h"
#include "net/netdev.h"
#include "sx127x_internal.h"
#include "sx127x_params.h"
#include "sx127x_netdev.h"

#include "fmt.h"
#include "byteorder.h"

#include "ls-settings.h"
#include "ls-config.h"
#include "ls-init-device.h"

#include "board.h"

#include "unwds-common.h"
#include "unwds-gpio.h"

#include "main.h"
#include "utils.h"

#include "periph/wdg.h"
#include "rtctimers-millis.h"

#include "net/loramac.h"
#include "semtech_loramac.h"

#define ENABLE_DEBUG (0)
#include "debug.h"

static msg_t msg_join;
static kernel_pid_t sender_pid;
static rtctimers_millis_t send_retry_timer;

static kernel_pid_t main_thread_pid;
static kernel_pid_t loramac_pid;

static char sender_stack[2048];

static semtech_loramac_t ls;

static uint8_t current_join_retries = 0;
static uint8_t uplinks_failed = 0;

static bool appdata_received(uint8_t *buf, size_t buflen);
static void unwds_callback(module_data_t *buf);

void radio_init(void)
{
    sx127x_params_t sx127x_params;
    
    sx127x_params.nss_pin = SX127X_SPI_NSS;
    sx127x_params.spi = SX127X_SPI;

    sx127x_params.dio0_pin = SX127X_DIO0;
    sx127x_params.dio1_pin = SX127X_DIO1;
    sx127x_params.dio2_pin = SX127X_DIO2;
    sx127x_params.dio3_pin = SX127X_DIO3;
    sx127x_params.dio4_pin = SX127X_DIO4;
    sx127x_params.dio5_pin = SX127X_DIO5;
    sx127x_params.reset_pin = SX127X_RESET;
   
    sx127x_params.rfswitch_pin = SX127X_RFSWITCH;
    sx127x_params.rfswitch_active_level = SX127X_GET_RFSWITCH_ACTIVE_LEVEL();
    
    loramac_pid = semtech_loramac_init(&ls, &sx127x_params);

    if (loramac_pid > KERNEL_PID_UNDEF) {
        puts("[LoRa] LoRaMAC successfully initialized");
    } else {
        puts("[LoRa] LoRaMAC initialization failed");
    }
}

static int node_join(semtech_loramac_t *ls) {
    /* limit max delay between attempts to 1 hour */
    if (current_join_retries < 120) {
        current_join_retries++;
    }
    
    blink_led(LED_GREEN);
    
    if (unwds_get_node_settings().nodeclass == LS_ED_CLASS_A) {
        printf("[LoRa] joining, attempt %d / %d\n", current_join_retries, unwds_get_node_settings().max_retr + 1);
    } else {
        puts("[LoRa] joining");
    }
    
    return (semtech_loramac_join(ls, LORAMAC_JOIN_OTAA));
}

static void *sender_thread(void *arg) {
    semtech_loramac_t *ls = (semtech_loramac_t *)arg;
    
    msg_t msg;
    
    puts("[LoRa] sender thread started");
    
    while (1) {
        msg_receive(&msg);
        
        if (msg.sender_pid != loramac_pid) {
            int res = node_join(ls);
            
            switch (res) {
            case SEMTECH_LORAMAC_JOIN_SUCCEEDED: {
                current_join_retries = 0;
                puts("[LoRa] successfully joined to the network");
                break;
            }
            case SEMTECH_LORAMAC_RESTRICTED:
            case SEMTECH_LORAMAC_BUSY:
            case SEMTECH_LORAMAC_NOT_JOINED:
            case SEMTECH_LORAMAC_JOIN_FAILED:
            {
                printf("[LoRa] LoRaMAC join failed: code %d\n", res);
                if ((current_join_retries > unwds_get_node_settings().max_retr) &&
                    (unwds_get_node_settings().nodeclass == LS_ED_CLASS_A)) {
                    /* class A node: go to sleep */
                    puts("[LoRa] maximum join retries exceeded, stopping");
                    current_join_retries = 0;
                } else {
                    puts("[LoRa] join request timed out, resending");
                    
                    /* Pseudorandom delay for collision avoidance */
                    unsigned int delay = random_uint32_range(10000 + (current_join_retries - 1)*30000, 30000 + (current_join_retries - 1)*30000);
                    printf("[LoRa] random delay %d s\n", delay/1000);
                    rtctimers_millis_set_msg(&send_retry_timer, delay, &msg_join, sender_pid);
                }
                break;
            }
            default:
                printf("[LoRa] join request: unknown response %d\n", res);
                break;
            }
        } else {
            switch (msg.type) {
                case MSG_TYPE_LORAMAC_TX_DONE:
                    puts("[LoRa] TX done");
                    break;
                case MSG_TYPE_LORAMAC_TX_CNF_FAILED:
                    puts("[LoRa] Uplink confirmation failed");
                    uplinks_failed++;
                    
                    if (uplinks_failed > unwds_get_node_settings().max_retr) {
                        puts("[LoRa] Too many uplinks failed, rejoining");
                        current_join_retries = 0;
                        uplinks_failed = 0;
                        msg_send(&msg_join, sender_pid);
                    }
                    break;
                case MSG_TYPE_LORAMAC_RX:
                    if ((ls->rx_data.payload_len == 0) && ls->rx_data.ack) {
                        printf("[LoRa] Ack received: RSSI %d, DR %d\n",
                                ls->rx_data.rssi,
                                ls->rx_data.datarate);
                    } else {
                        printf("[LoRa] Data received: %d bytes, port %d, RSSI %d, DR %d\n",
                                ls->rx_data.payload_len,
                                ls->rx_data.port,
                                ls->rx_data.rssi,
                                ls->rx_data.datarate);
#if ENABLE_DEBUG
                        printf("[LoRa] Hex data: ");
                        for (int l = 0; l < ls->rx_data.payload_len; l++) {
                            printf("%02X ", ls->rx_data.payload[l]);
                        }
                        printf("\n");
#endif
                        appdata_received(ls->rx_data.payload, ls->rx_data.payload_len);
                    }
                    break;
                case MSG_TYPE_LORAMAC_JOIN:
                    puts("[LoRa] LoRaMAC join notification\n");
                    break;
                default:
                    DEBUG("[LoRa] Unidentified LoRaMAC msg type %d\n", msg.type);
                    break;
            }
        }
    }
    return NULL;
}

static bool appdata_received(uint8_t *buf, size_t buflen)
{
    char hex[100] = {};

    bytes_to_hex(buf, buflen, hex, false);

    printf("[LoRa] received data: \"%s\"\n", hex);
    blink_led(LED_GREEN);

    if (buflen < 2) {
        return true;
    }

    unwds_module_id_t modid = buf[0];

    module_data_t cmd;
    /* Save command data */
    memcpy(cmd.data, buf + 1, buflen - 1);
    cmd.length = buflen - 1;

    /* Save RSSI value */
    /* cmd.rssi = ls._internal.last_rssi; */

    /* Send command to the module */
    module_data_t reply = {};

    /* Send app. data */
    int result = unwds_send_to_module(modid, &cmd, &reply);
    
    if (result == UNWDS_MODULE_NOT_FOUND) {
        /* No module with specified ID present */
        reply.as_ack = true;
        reply.length = 2;
        reply.data[0] = UNWDS_MODULE_NOT_FOUND;
        reply.data[1] = modid;
    }
    
    if (result != UNWDS_MODULE_NO_DATA) {
        unwds_callback(&reply);
    }

    /* Don't allow to send app. data ACK by the network.
     * The ACK will be sent either by the callback with the actual app. data or
     * with the command response itself */
    return false;
}

static void ls_setup(semtech_loramac_t *ls)
{
    uint64_t id = config_get_nodeid();
    uint8_t deveui[LORAMAC_DEVEUI_LEN];
    memcpy(deveui, &id, LORAMAC_DEVEUI_LEN);
    byteorder_swap(deveui, LORAMAC_DEVEUI_LEN);
    semtech_loramac_set_deveui(ls, deveui);
    
    id = config_get_appid();
    uint8_t appeui[LORAMAC_APPEUI_LEN];
    memcpy(appeui, &id, LORAMAC_APPEUI_LEN);
    byteorder_swap(appeui, LORAMAC_APPEUI_LEN);
    semtech_loramac_set_appeui(ls, appeui);
    
    uint8_t appkey[LORAMAC_APPKEY_LEN];
    memcpy(appkey, config_get_joinkey(), LORAMAC_APPKEY_LEN);
    semtech_loramac_set_appkey(ls, appkey);
    
    semtech_loramac_set_dr(ls, unwds_get_node_settings().dr);
    
    semtech_loramac_set_adr(ls, unwds_get_node_settings().adr);
    semtech_loramac_set_class(ls, unwds_get_node_settings().nodeclass);
    
    /* Maximum number of confirmed data retransmissions */
    semtech_loramac_set_retries(ls, unwds_get_node_settings().max_retr);
    
    if (unwds_get_node_settings().confirmation) {
        semtech_loramac_set_tx_mode(ls, LORAMAC_TX_CNF);   /* confirmed packets */
    } else {
        semtech_loramac_set_tx_mode(ls, LORAMAC_TX_UNCNF); /* unconfirmed packets */
    }

    semtech_loramac_set_tx_port(ls, LORAMAC_DEFAULT_TX_PORT); /* port 2 */
    
    puts("[LoRa] LoRaMAC values set");
}

int ls_set_cmd(int argc, char **argv)
{
    if (argc != 3) {
        puts("usage: set <key> <value>");
        puts("keys:");
        if (unwds_get_node_settings().no_join)
        	puts("\taddr <address> -- sets predefined device address for statically personalized devices");

        puts("\totaa <0/1> -- select between OTAA and ABP");
//        puts("\tch <ch> -- sets device channel in selected region");
        puts("\tdr <0-6> -- sets default data rate [0 - slowest, 3 - average, 6 - fastest]");
        puts("\tmaxretr <0-5> -- sets maximum number of retransmissions of confirmed app. data [2 is recommended]");
        puts("\tclass <A/C> -- sets device class");
        puts("\tadr <0/1> -- enable or disable ADR");
    }
    
    char *key = argv[1];
    char *value = argv[2];
    
    if (strcmp(key, "otaa") == 0) {
        int v = atoi(value);
        if (v) {
            unwds_set_nojoin(true);
        } else {
            unwds_set_nojoin(false);
        }
    }
    
    if (strcmp(key, "maxretr") == 0) {
        int v = atoi(value);
        if (v > 5) {
            v = 5;
        }
        unwds_set_max_retr(v);
    }
    
    if (strcmp(key, "adr") == 0) {
        int v = atoi(value);
        unwds_set_adr(v);
    }

    if (strcmp(key, "class") == 0) {
        char v = value[0];

        if (v != 'A' && v != 'C') {
            puts("set сlass: A or C");
            return 1;
        }

        if (v == 'A') {
            unwds_set_class(LS_ED_CLASS_A);
        }
        /*
        else if (v == 'B') {
            unwds_set_class(LS_ED_CLASS_B);
        }
        */
        else if (v == 'C') {
            unwds_set_class(LS_ED_CLASS_C);
        }
    }

    return 0;
}

static void print_config(void)
{
    puts("[ node configuration ]");

    uint64_t eui64 = config_get_nodeid();
    uint64_t appid = config_get_appid();

    printf("OTAA = %s\n", (unwds_get_node_settings().no_join) ? "no" : "yes");

    if (!unwds_get_node_settings().no_join && DISPLAY_JOINKEY_2BYTES) {
        uint8_t *key = config_get_joinkey();
        printf("JOINKEY = 0x....%01X%01X\n", key[14], key[15]);
    }

    if (unwds_get_node_settings().no_join && DISPLAY_DEVNONCE_BYTE) {
    	uint8_t devnonce = config_get_devnonce();
    	printf("DEVNONCE = 0x...%01X\n", devnonce & 0x0F);
    }

    if (unwds_get_node_settings().no_join) {
    	printf("ADDR = 0x%08X\n", (unsigned int) unwds_get_node_settings().dev_addr);
    }

    printf("EUI64 = 0x%08x%08x\n", (unsigned int) (eui64 >> 32), (unsigned int) (eui64 & 0xFFFFFFFF));
    printf("APPID64 = 0x%08x%08x\n", (unsigned int) (appid >> 32), (unsigned int) (appid & 0xFFFFFFFF));

    printf("DATARATE = %d\n", unwds_get_node_settings().dr);
    
    printf("ADR = %s\n", (unwds_get_node_settings().adr)?  "yes" : "no");
    
    printf("CONFIRMED = %s\n", (unwds_get_node_settings().confirmation) ? "yes" : "no");

    char nodeclass = 'A'; // unwds_get_node_settings().nodeclass == LS_ED_CLASS_A
    if (unwds_get_node_settings().nodeclass == LS_ED_CLASS_B) {
        nodeclass = 'B';
    }
    else if (unwds_get_node_settings().nodeclass == LS_ED_CLASS_C) {
        nodeclass = 'C';
    }
    printf("CLASS = %c\n", nodeclass);

    printf("MAXRETR = %d\n", unwds_get_node_settings().max_retr);

    puts("[ enabled modules ]");
    unwds_list_modules(unwds_get_node_settings().enabled_mods, true);
}

static int ls_printc_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    
    print_config();

    return 0;
}

int ls_cmd_cmd(int argc, char **argv)
{
    if (argc < 2) {
        puts("Usage: cmd <modid> <cmdhex>");
        return 1;
    }

    uint8_t modid = atoi(argv[1]);
    if (!unwds_is_module_exists(modid)) {
        printf("cmd: module with ID %d does not exists\n", modid);
        return 1;
    }

    module_data_t cmd = {};

    int len = strlen(argv[2]);
    if (len % 2 != 0) {
        puts("cmd: invalid hex number");
        return 1;
    }

    if (len / 2 > UNWDS_MAX_DATA_LEN) {
        printf("cmd: command too long. Maximum is %d bytes\n", UNWDS_MAX_DATA_LEN);
        return 1;
    }

    hex_to_bytes(argv[2], cmd.data, false);
    cmd.length = len / 2;

    /* No RSSI from console commands */
    cmd.rssi = 0;

    module_data_t reply = {};
    bool res = unwds_send_to_module(modid, &cmd, &reply);
    char replystr[2 * UNWDS_MAX_DATA_LEN] = {};
    bytes_to_hex(reply.data, reply.length, replystr, false);

    if (res) {
        printf("[ok] Reply: %s\n", replystr);
    }
    else {
        printf("[fail] Reply: %s\n", replystr);
    }

    return 0;
}

static int ls_listmodules_cmd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    
    puts("[ available modules ]");
    unwds_list_modules(unwds_get_node_settings().enabled_mods, false);

    return 0;
}

static int ls_module_cmd(int argc, char **argv)
{
    if (argc < 3) {
        puts("Usage: mod <name> <enable|disable>. Example: mod adc enable");
        return 1;
    }

    int modid = 0;
    
    if (is_number(argv[1])) {
        modid = atoi(argv[1]);
    } else {
        modid = unwds_modid_by_name(argv[1]);
    }
    
    if (modid < 0) {
        printf("mod: module %s does not exist\n", argv[1]);
        return 1;
    }
    
    if (!unwds_is_module_exists(modid)) {
        printf("mod: module with ID %d does not exist\n", modid);
        return 1;
    }

    bool modenable = false;
    if (is_number(argv[2])) {
        modenable = atoi(argv[2]);
    } else {
        if (strcmp(argv[2], "enable") == 0) {
            modenable = true;
        } else {
            if (strcmp(argv[2], "disable") != 0) {
                printf("mod: unknown command: %s\n", argv[2]);
                return 1;
            }
        }
    }
    
    unwds_set_module(modid, modenable);
        
    return 0;
}

static int ls_safe_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;

    uint32_t bootmode = UNWDS_BOOT_SAFE_MODE;
    rtc_save_backup(bootmode, RTC_REGBACKUP_BOOTMODE);
    puts("Rebooting in safe mode");
    NVIC_SystemReset();
    return 0;
}

static int ls_join_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    msg_send(&msg_join, sender_pid);
    return 0;
}

shell_command_t shell_commands[UNWDS_SHELL_COMMANDS_MAX] = {
    { "set", "<config> <value> -- set value for the configuration entry", ls_set_cmd },
    { "lscfg", "-- print out current configuration", ls_printc_cmd },
    { "lsmod", "-- list available modules", ls_listmodules_cmd },
    { "mod", "<name> <enable|disable>	-- disable or enable selected module", ls_module_cmd },
    { "cmd", "<modid> <cmdhex> -- send command to another UNWDS device", ls_cmd_cmd },
    { "safe", " -- reboot in safe mode", ls_safe_cmd },
    { "join", " -- join now", ls_join_cmd },
    { NULL, NULL, NULL },
};

static void unwds_callback(module_data_t *buf)
{
    uint8_t bytes = 0;
    
    if (buf->length < 15) {
        bytes = 16;
    } else {
        if (buf->length < 31) {
            bytes = 32;
        } else {
            printf("[LoRa] Payload too big: %d bytes (should be 30 bytes max)\n", buf->length);
            return;
        }
    }
    
    printf("[LoRa] Payload size %d bytes + 2 status bytes -> %d bytes\n", buf->length, bytes);
    buf->length = bytes;
    
    if (adc_init(ADC_LINE(ADC_TEMPERATURE_INDEX)) == 0) {
        int8_t temperature = adc_sample(ADC_LINE(ADC_TEMPERATURE_INDEX), ADC_RES_12BIT);
        
        /* convert to sign-and-magnitude format */
        convert_to_be_sam((void *)&temperature, 1);
        
        buf->data[bytes - 2] = (uint8_t)temperature;
        printf("MCU temperature is %d C\n", buf->data[bytes - 2]);
    }
    
    if (adc_init(ADC_LINE(ADC_VREF_INDEX)) == 0) {
        buf->data[bytes - 1] = adc_sample(ADC_LINE(ADC_VREF_INDEX), ADC_RES_12BIT)/50;
        printf("Battery voltage %d mV\n", buf->data[bytes - 1] * 50);
    }
    
#if ENABLE_DEBUG
    for (int k = 0; k < buf->length; k++) {
        printf("%02X ", buf->data[k]);
    }
    printf("\n");
#endif
    
    int res = semtech_loramac_send(&ls, buf->data, buf->length);

    switch (res) {
        case SEMTECH_LORAMAC_BUSY:
            puts("[error] MAC already busy");
            break;
        case SEMTECH_LORAMAC_NOT_JOINED: {
            puts("[error] Not joined to the network");

            if (current_join_retries == 0) {
                puts("[info] Attempting to rejoin");
                msg_send(&msg_join, sender_pid);
            } else {
                puts("[info] Waiting for the node to join");
            }
            break;
        }
        case SEMTECH_LORAMAC_TX_SCHEDULED:
            puts("[info] TX scheduled");
            break;
        default:
            puts("[warning] Unknown response");
            break;
    }

    blink_led(LED_GREEN);
}

static int unwds_init(void) {
    radio_init();
    ls_setup(&ls);
    
    return 0;
}

static void unwds_join(void) {
    msg_send(&msg_join, sender_pid);
}

static void unwds_sleep(void) {
    semtech_loramac_set_class(&ls, LS_ED_CLASS_A);
}

void init_normal(shell_command_t *commands)
{
    /* should always be 2 */
    main_thread_pid = thread_getpid();
    
    bool cfg_valid = unwds_config_load();
    print_config();
    
    if (!cfg_valid) {
        puts("[!] Device is not configured yet. Type \"help\" to see list of possible configuration commands.");
        puts("[!] Configure the node and type \"reboot\" to reboot and apply settings.");
    } else {
        sender_pid = thread_create(sender_stack, sizeof(sender_stack), THREAD_PRIORITY_MAIN - 2,
                                   THREAD_CREATE_STACKTEST, sender_thread, &ls,  "LoRa sender thread");

        unwds_device_init(unwds_callback, unwds_init, unwds_join, unwds_sleep);
    }
    
    /* Add our commands to shell */
    int i = 0;
    do {
        i++;
    } while (commands[i].name);
    
    int k = 0;
    do {
        k++;
    } while (shell_commands[k].name);
    
    assert(i + k < UNWDS_SHELL_COMMANDS_MAX - 1);
    
    memcpy((void *)&commands[i], (void *)shell_commands, k*sizeof(shell_commands[i]));
}

#ifdef __cplusplus
}
#endif
