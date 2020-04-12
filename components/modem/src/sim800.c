// Copyright 2015-2018 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_modem_dce_service.h"
#include "sim800.h"
#include "driver/gpio.h"

#define MODEM_RESULT_CODE_POWERDOWN "POWER DOWN"

#define SIM800_PWKEY CONFIG_EXAMPLE_UART_MODEM_PWKEY
#define set_sim800_pwkey() gpio_set_level(SIM800_PWKEY, 1)
#define clear_sim800_pwkey() gpio_set_level(SIM800_PWKEY, 0)

#define SIM800_RST CONFIG_EXAMPLE_UART_MODEM_RST
#define set_sim800_rst() gpio_set_level(SIM800_RST, 1)
#define clear_sim800_rst() gpio_set_level(SIM800_RST, 0)

#define SIM800_POWER 23
#define set_sim800_pwrsrc() gpio_set_level(SIM800_POWER, 1)
#define clear_sim800_pwrsrc() gpio_set_level(SIM800_POWER, 0)

static esp_err_t sim800_handle_cfun(modem_dce_t *dce, const char *line);
static esp_err_t sim800_print_buffer(modem_dce_t *dce, const char *buffer);
static esp_err_t sim800_handle_cclk(modem_dce_t *dce, const char *buffer);
static esp_err_t sim800_handle_creg(modem_dce_t *dce, const char *buffer);
/**
 * @brief Macro defined for error checking
 *
 */
static const char *DCE_TAG = "sim800";
#define DCE_CHECK(a, str, goto_tag, ...)                                              \
    do                                                                                \
    {                                                                                 \
        if (!(a))                                                                     \
        {                                                                             \
            ESP_LOGE(DCE_TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__); \
            goto goto_tag;                                                            \
        }                                                                             \
    } while (0)

/**
 *  @brief SIM800 Modem
 *  
 */
typedef struct
{
    void *priv_resource; /*!< Private resource */
    size_t command_timeout;
    modem_dce_t parent; /*!< DCE parent class */
} sim800_modem_dce_t;

static const char *const sim800_urc_responses[] = {
    "+CFUN: ",
    "+CREG: ",
    "*PSUTTZ: ",       /* AT+CLTS time */
    "+CTZV: ",         /* AT+CLTS timezone */
    "DST: ",           /* AT+CLTS dst information */
    "+CIEV: ",         /* AT+CMER level bar change indicator */
    "+CIPRXGET: 1,",   /* incoming socket data notification */
    "+FTPGET: 1,",     /* FTP state change notification */
    "+PDP: DEACT",     /* PDP disconnected */
    "+SAPBR 1: DEACT", /* PDP disconnected (for SAPBR apps) */
    "*PSNWID: ",       /* AT+CLTS network name */
    "+CGREG: ",
    "CONNECT",
    "CLOSED",
    "RDY",
    "+CSSI:",
    "+CSSU:",
    "+CSQN:",
    "Call Ready",
    "SMS Ready",
    "NORMAL POWER DOWN",
    "UNDER-VOLTAGE",
    "OVER-VOLTAGE",
    NULL};

esp_err_t (*sim800_urc_responses_fn[])(modem_dce_t *dce, const char *buffer) = {
    sim800_handle_cfun,
    sim800_handle_creg,
    sim800_handle_cclk,
    sim800_print_buffer, /* AT+CLTS time */
    sim800_print_buffer, /* AT+CLTS timezone */
    sim800_print_buffer, /* AT+CLTS dst information */
    sim800_print_buffer, /* AT+CMER level bar change indicator */
    sim800_print_buffer, /* incoming socket data notification */
    sim800_print_buffer, /* FTP state change notification */
    sim800_print_buffer, /* PDP disconnected */
    sim800_print_buffer, /* PDP disconnected (for SAPBR apps) */
    sim800_print_buffer, /* AT+CLTS network name */
    sim800_print_buffer,
    sim800_print_buffer,
    sim800_print_buffer,
    sim800_print_buffer,
    sim800_print_buffer,
    sim800_print_buffer,
    sim800_print_buffer,
    sim800_print_buffer,
    sim800_print_buffer,
    sim800_print_buffer,
    sim800_print_buffer,
    sim800_print_buffer,
    sim800_print_buffer,
    sim800_print_buffer};

esp_err_t sim800_handle_response_default(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;

    for (int i = 0; sim800_urc_responses[i] != NULL; i++)
    {
        if (!strncmp(line, sim800_urc_responses[i], strlen(sim800_urc_responses[i] + 2)))
        {
            sim800_urc_responses_fn[i](dce, line);
            return ESP_OK;
        }
    }

    if (strstr(line, MODEM_RESULT_CODE_SUCCESS))
    {
        printf("\033[1m;\033[38;5;35m%s\033[0m", line);
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);

    }
    else if (strstr(line, MODEM_RESULT_CODE_ERROR))
    {
        printf("\033[1m;\033[38;5;31m%s\033[0m", line);
        err = esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    
    }
    else
    {
        //ESP_LOG_BUFFER_HEXDUMP("", line, strlen(line), 0);
        printf("\033[1m;\033[38;5;202m%s\033[0", line);
        err = esp_modem_process_command_done(dce, MODEM_STATE_PROCESSING);
    }

    return err;
}

esp_err_t sim800_handle_at_response(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;

    if (strstr(line, MODEM_RESULT_CODE_SUCCESS))
    {
        printf("\033[38;5;46m%s\033[0m", line);
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    }
    else if (strstr(line, MODEM_RESULT_CODE_ERROR))
    {
        printf("\033[38;5;31m%s\033[0m", line);
        err = esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    }
    else
    {
            printf("\033[38;5;202m%s", line);
            err=ESP_OK;
    }

    return err;
}

static esp_err_t sim800_print_buffer(modem_dce_t *dce, const char *buffer)
{
    esp_err_t err = ESP_OK;

    printf("\033[1m\033[38;5;201m%s\033[0m", buffer);

    return err;
}

/**
 * @brief Execute an generic AT  Modem Command
 * @param dce Modem DCE object
 * @param at_command AT command to execute
 * @param timeout Command execution timeout
 *
 */

esp_err_t sim800_at(modem_dce_t *dce, const char *at_command, uint16_t timeout)
{
    DCE_CHECK(dce, "Data Communication Equipment is not Connected to Data Terminal", err);
    modem_dte_t *dte = dce->dte;
    char send_cmd[50] = "AT";
    strcat(send_cmd, at_command);
    strcat(send_cmd, "\r");

    if (timeout == 0)
        timeout = MODEM_COMMAND_TIMEOUT_DEFAULT;
    dce->handle_line = sim800_handle_at_response;
    DCE_CHECK(dte->send_cmd(dte, send_cmd, MODEM_COMMAND_TIMEOUT_DEFAULT) == ESP_OK, "send command failed", err);
    DCE_CHECK(dce->state == MODEM_STATE_SUCCESS, "AT command failed", err);
    return ESP_OK;
err:
    dce->handle_line = dce->handle_line_default;
    return ESP_FAIL;
}

esp_err_t sim800_send_raw(modem_dce_t *dce, const char *line, uint16_t timeout)
{
    DCE_CHECK(dce, "Data Communication Equipment is not Connected to Data Terminal", err);
    modem_dte_t *dte = dce->dte;
    char send_line[128] = "";
    strcat(send_line, line);
    strcat(send_line, "\r\n\x1A");
    printf("\r\n");
    if (timeout == 0)
        timeout = MODEM_COMMAND_TIMEOUT_DEFAULT;
    dce->handle_line = sim800_handle_at_response;
    DCE_CHECK(dte->send_cmd(dte, send_line, timeout * 2) == ESP_OK, "send line timeout", err);
    DCE_CHECK(dce->state == MODEM_STATE_SUCCESS, "line command failed", err);
    return ESP_OK;
err:
    return ESP_FAIL;
}

/**
 * @brief Handle response from AT+CCLK
 */
esp_err_t sim800_handle_cclk(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;
    sim800_modem_dce_t *sim800_dce = __containerof(dce, sim800_modem_dce_t, parent);

    struct tm tm;
    printf("\033[1mTime set to: %s\033[0m", line);
    memset(&tm, 0, sizeof(struct tm));
    at_simple_scanf(line, "*PSUTTZ: %d,%d,%d,%d,%d,%d,\"%*d\",%d",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec,&tm.tm_isdst);

    /* Most modems report some starting date way in the past when they have
     * no date/time estimation. */
    if (tm.tm_year < 14)
    {
        return err;
    }

    /* Adjust values and perform conversion. */
    tm.tm_year += 2000 - 1900;
    tm.tm_mon -= 1;
    time_t unix_time = mktime(&tm);
    if (unix_time == -1)
    {
        return err;
    }
    printf("\033Unix Time: %ld\033[0m\n", unix_time);
    /* All good. Return the result. */
    err = ESP_OK;
    
    struct timespec *ts = sim800_dce->priv_resource;
    ts->tv_sec = unix_time;
    ts->tv_nsec = 0;

    return err;
}

/**
 * @brief Handle response from +CFUN
 */
static esp_err_t sim800_handle_cfun(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;
    sim800_modem_dce_t *sim800_dce = __containerof(dce, sim800_modem_dce_t, parent);
    if (!strncmp(line, "+CFUN", strlen("+CFUN")))
    {
        /* store value of rssi and ber */
        uint32_t fun = 5;
        /* +CFUN: <stat>*/
        sscanf(line, "%*s%d", &fun);
        switch (fun)
        {
        case 0:
            printf("Minimum functionality.");
            break;
        case 1:
            printf("full functionality.");
            break;
        case 2:
            printf("TX RF Circuit disabled");
            break;
        case 3:
            printf("RX RF Circuit disabled");
            break;
        case 4:
            printf("TX and RX RF Circuits disabled");
            break;
        default:
            printf("Reserved.");
        }

        err = ESP_OK;
    }
    return err;
}

/**
 * @brief Handle response from AT+CSQ
 */
static esp_err_t sim800_handle_csq(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;
    sim800_modem_dce_t *sim800_dce = __containerof(dce, sim800_modem_dce_t, parent);
    if (strstr(line, MODEM_RESULT_CODE_SUCCESS))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    }
    else if (strstr(line, MODEM_RESULT_CODE_ERROR))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    }
    else if (!strncmp(line, "+CSQ", strlen("+CSQ")))
    {
        /* store value of rssi and ber */
        uint32_t **csq = sim800_dce->priv_resource;
        /* +CSQ: <rssi>,<ber> */
        sscanf(line, "%*s%d,%d", csq[0], csq[1]);
        err = ESP_OK;
    }
    return err;
}

/**
 * @brief Handle response from AT+CBC
 */
static esp_err_t sim800_handle_cbc(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;
    sim800_modem_dce_t *sim800_dce = __containerof(dce, sim800_modem_dce_t, parent);
    if (strstr(line, MODEM_RESULT_CODE_SUCCESS))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    }
    else if (strstr(line, MODEM_RESULT_CODE_ERROR))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    }
    else if (!strncmp(line, "+CBC", strlen("+CBC")))
    {
        /* store value of bcs, bcl, voltage */
        uint32_t **cbc = sim800_dce->priv_resource;
        /* +CBC: <bcs>,<bcl>,<voltage> */
        sscanf(line, "%*s%d,%d,%d", cbc[0], cbc[1], cbc[2]);
        err = ESP_OK;
    }
    return err;
}

/**
 * @brief Handle response from +++
 */
static esp_err_t sim800_handle_exit_data_mode(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;
    if (strstr(line, MODEM_RESULT_CODE_SUCCESS))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    }
    else if (strstr(line, MODEM_RESULT_CODE_NO_CARRIER))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    }
    else if (strstr(line, MODEM_RESULT_CODE_ERROR))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    }
    return err;
}

/**
 * @brief Handle response from ATD*99#
 */
static esp_err_t sim800_handle_atd_ppp(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;
    if (strstr(line, MODEM_RESULT_CODE_CONNECT))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    }
    else if (strstr(line, MODEM_RESULT_CODE_ERROR))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    }
    return err;
}

/**
 * @brief Handle response from AT+CGMM
 */
static esp_err_t sim800_handle_cgmm(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;
    if (strstr(line, MODEM_RESULT_CODE_SUCCESS))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    }
    else if (strstr(line, MODEM_RESULT_CODE_ERROR))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    }
    else
    {
        int len = snprintf(dce->name, MODEM_MAX_NAME_LENGTH, "%s", line);
        if (len > 2)
        {
            /* Strip "\r\n" */
            strip_cr_lf_tail(dce->name, len);
            err = ESP_OK;
        }
    }
    return err;
}

/**
 * @brief Handle response from AT+CGSN
 */
static esp_err_t sim800_handle_cgsn(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;
    if (strstr(line, MODEM_RESULT_CODE_SUCCESS))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    }
    else if (strstr(line, MODEM_RESULT_CODE_ERROR))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    }
    else
    {
        int len = snprintf(dce->imei, MODEM_IMEI_LENGTH + 1, "%s", line);
        if (len > 2)
        {
            /* Strip "\r\n" */
            strip_cr_lf_tail(dce->imei, len);
            err = ESP_OK;
        }
    }
    return err;
}

/**
 * @brief Handle response from AT+CIMI
 */
static esp_err_t sim800_handle_cimi(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;
    if (strstr(line, MODEM_RESULT_CODE_SUCCESS))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    }
    else if (strstr(line, MODEM_RESULT_CODE_ERROR))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    }
    else
    {
        int len = snprintf(dce->imsi, MODEM_IMSI_LENGTH + 1, "%s", line);
        if (len > 2)
        {
            /* Strip "\r\n" */
            strip_cr_lf_tail(dce->imsi, len);
            err = ESP_OK;
        }
    }
    return err;
}

/**
 * @brief Handle response from AT+CREG?
 */
static esp_err_t sim800_handle_creg(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;
    if (strstr(line, MODEM_RESULT_CODE_SUCCESS))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    }
    else if (strstr(line, MODEM_RESULT_CODE_ERROR))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    }
    else if (!strncmp(line, "+CREG", strlen("+CREG")))
    {
        sim800_modem_dce_t *sim800_dce = __containerof(dce, sim800_modem_dce_t, parent);
        /* +CREG: <n>,<stat> */
        uint32_t **resource = sim800_dce->priv_resource;
        if(strlen(line)>10)
        {
            sscanf(line, "%*s%d,%d%*s", resource[0], resource[1]);
        }
        else // The URC switches mode with stat !!!
        {
           sscanf(line, "%*s%d", resource[1]);
           printf("\033[1m%s\033[0m", line); 
        }
        err = ESP_OK;
    }
    return err;
}

/**
 * @brief Handle response from AT+COPS?
 */
static esp_err_t sim800_handle_cops(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;
    if (strstr(line, MODEM_RESULT_CODE_SUCCESS))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    }
    else if (strstr(line, MODEM_RESULT_CODE_ERROR))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    }
    else if (!strncmp(line, "+COPS", strlen("+COPS")))
    {
        /* there might be some random spaces in operator's name, we can not use sscanf to parse the result */
        /* strtok will break the string, we need to create a copy */
        size_t len = strlen(line);
        char *line_copy = malloc(len + 1);
        strcpy(line_copy, line);
        /* +COPS: <mode>[, <format>[, <oper>]] */
        char *str_ptr = NULL;
        char *p[3];
        uint8_t i = 0;
        /* strtok will broke string by replacing delimiter with '\0' */
        p[i] = strtok_r(line_copy, ",", &str_ptr);
        while (p[i])
        {
            p[++i] = strtok_r(NULL, ",", &str_ptr);
        }
        if (i >= 3)
        {
            int len = snprintf(dce->oper, MODEM_MAX_OPERATOR_LENGTH, "%s", p[2]);
            if (len > 2)
            {
                /* Strip "\r\n" */
                strip_cr_lf_tail(dce->oper, len);
                err = ESP_OK;
            }
        }
        free(line_copy);
    }
    return err;
}

/**
 * @brief Handle response from AT+CPOWD=1
 */
static esp_err_t sim800_handle_power_down(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;
    if (strstr(line, MODEM_RESULT_CODE_POWERDOWN))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    }
    return err;
}

/**
 * @brief Get signal quality
 *
 * @param dce Modem DCE object
 * @param rssi received signal strength indication
 * @param ber bit error ratio
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
static esp_err_t sim800_get_signal_quality(modem_dce_t *dce, uint32_t *rssi, uint32_t *ber)
{
    modem_dte_t *dte = dce->dte;
    sim800_modem_dce_t *sim800_dce = __containerof(dce, sim800_modem_dce_t, parent);
    uint32_t *resource[2] = {rssi, ber};
    sim800_dce->priv_resource = resource;
    dce->handle_line = sim800_handle_csq;
    DCE_CHECK(dte->send_cmd(dte, "AT+CSQ\r", MODEM_COMMAND_TIMEOUT_DEFAULT) == ESP_OK, "send command failed", err);
    DCE_CHECK(dce->state == MODEM_STATE_SUCCESS, "inquire signal quality failed", err);
    ESP_LOGD(DCE_TAG, "inquire signal quality ok");
    return ESP_OK;
err:
    return ESP_FAIL;
}

/**
 * @brief Get battery status
 *
 * @param dce Modem DCE object
 * @param bcs Battery charge status
 * @param bcl Battery connection level
 * @param voltage Battery voltage
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
static esp_err_t sim800_get_battery_status(modem_dce_t *dce, uint32_t *bcs, uint32_t *bcl, uint32_t *voltage)
{
    modem_dte_t *dte = dce->dte;
    sim800_modem_dce_t *sim800_dce = __containerof(dce, sim800_modem_dce_t, parent);
    uint32_t *resource[3] = {bcs, bcl, voltage};
    sim800_dce->priv_resource = resource;
    dce->handle_line = sim800_handle_cbc;
    DCE_CHECK(dte->send_cmd(dte, "AT+CBC\r", MODEM_COMMAND_TIMEOUT_DEFAULT) == ESP_OK, "send command failed", err);
    DCE_CHECK(dce->state == MODEM_STATE_SUCCESS, "inquire battery status failed", err);
    ESP_LOGD(DCE_TAG, "inquire battery status ok");
    return ESP_OK;
err:
    return ESP_FAIL;
}

/**
 * @brief Set Working Mode
 *
 * @param dce Modem DCE object
 * @param mode woking mode
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
static esp_err_t sim800_set_working_mode(modem_dce_t *dce, modem_mode_t mode)
{
    modem_dte_t *dte = dce->dte;
    switch (mode)
    {
    case MODEM_COMMAND_MODE:
        dce->handle_line = sim800_handle_exit_data_mode;
        DCE_CHECK(dte->send_cmd(dte, "+++", MODEM_COMMAND_TIMEOUT_MODE_CHANGE) == ESP_OK, "send command failed", err);
        DCE_CHECK(dce->state == MODEM_STATE_SUCCESS, "enter command mode failed", err);
        ESP_LOGD(DCE_TAG, "enter command mode ok");
        dce->mode = MODEM_COMMAND_MODE;
        break;
    case MODEM_PPP_MODE:
        dce->handle_line = sim800_handle_atd_ppp;
        DCE_CHECK(dte->send_cmd(dte, "ATD*99#\r", MODEM_COMMAND_TIMEOUT_MODE_CHANGE) == ESP_OK, "send command failed", err);
        DCE_CHECK(dce->state == MODEM_STATE_SUCCESS, "enter ppp mode failed", err);
        ESP_LOGD(DCE_TAG, "enter ppp mode ok");
        dce->mode = MODEM_PPP_MODE;
        break;
    default:
        ESP_LOGW(DCE_TAG, "unsupported working mode: %d", mode);
        goto err;
        break;
    }
    return ESP_OK;
err:
    return ESP_FAIL;
}

/**
 * @brief Power down
 *
 * @param sim800_dce sim800 object
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
static esp_err_t sim800_power_down(modem_dce_t *dce)
{
    modem_dte_t *dte = dce->dte;
    dce->handle_line = sim800_handle_power_down;
    DCE_CHECK(dte->send_cmd(dte, "AT+CPOWD=1\r", MODEM_COMMAND_TIMEOUT_POWEROFF) == ESP_OK, "send command failed", err);
    DCE_CHECK(dce->state == MODEM_STATE_SUCCESS, "power down failed", err);
    ESP_LOGD(DCE_TAG, "power down ok");
    return ESP_OK;
err:
    return ESP_FAIL;
}

/**
 * @brief Get DCE module name
 *
 * @param sim800_dce sim800 object
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
static esp_err_t sim800_get_module_name(sim800_modem_dce_t *sim800_dce)
{
    modem_dte_t *dte = sim800_dce->parent.dte;
    sim800_dce->parent.handle_line = sim800_handle_cgmm;
    DCE_CHECK(dte->send_cmd(dte, "AT+CGMM\r", MODEM_COMMAND_TIMEOUT_DEFAULT) == ESP_OK, "send command failed", err);
    DCE_CHECK(sim800_dce->parent.state == MODEM_STATE_SUCCESS, "get module name failed", err);
    ESP_LOGD(DCE_TAG, "get module name ok");
    return ESP_OK;
err:
    return ESP_FAIL;
}

/**
 * @brief Get DCE module IMEI number
 *
 * @param sim800_dce sim800 object
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
static esp_err_t sim800_get_imei_number(sim800_modem_dce_t *sim800_dce)
{
    modem_dte_t *dte = sim800_dce->parent.dte;
    sim800_dce->parent.handle_line = sim800_handle_cgsn;
    DCE_CHECK(dte->send_cmd(dte, "AT+CGSN\r", MODEM_COMMAND_TIMEOUT_DEFAULT) == ESP_OK, "send command failed", err);
    DCE_CHECK(sim800_dce->parent.state == MODEM_STATE_SUCCESS, "get imei number failed", err);
    ESP_LOGD(DCE_TAG, "get imei number ok");
    return ESP_OK;
err:
    return ESP_FAIL;
}

/**
 * @brief Get DCE module IMSI number
 *
 * @param sim800_dce sim800 object
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
static esp_err_t sim800_get_imsi_number(sim800_modem_dce_t *sim800_dce)
{
    modem_dte_t *dte = sim800_dce->parent.dte;
    sim800_dce->parent.handle_line = sim800_handle_cimi;
    DCE_CHECK(dte->send_cmd(dte, "AT+CIMI\r", MODEM_COMMAND_TIMEOUT_DEFAULT) == ESP_OK, "send command failed", err);
    DCE_CHECK(sim800_dce->parent.state == MODEM_STATE_SUCCESS, "get imsi number failed", err);
    ESP_LOGD(DCE_TAG, "get imsi number ok");
    return ESP_OK;
err:
    return ESP_FAIL;
}

/**
 * @brief Get Network
 *
 * @param dce Modem DCE object
 * @param rssi received signal strength indication
 * @param ber bit error ratio
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
static esp_err_t sim800_get_network_status(modem_dce_t *dce, uint32_t *mode, uint32_t *stat)
{
    modem_dte_t *dte = dce->dte;
    sim800_modem_dce_t *sim800_dce = __containerof(dce, sim800_modem_dce_t, parent);
    uint32_t *ret[] = {mode, stat};
    sim800_dce->priv_resource = ret;
    dce->handle_line = sim800_handle_creg;
    DCE_CHECK(dte->send_cmd(dte, "AT+CREG?\r", MODEM_COMMAND_TIMEOUT_DEFAULT) == ESP_OK, "send command failed", err);
    DCE_CHECK(dce->state == MODEM_STATE_SUCCESS, "Network Registration failed", err);
    ESP_LOGD(DCE_TAG, "inquire signal quality ok");
    return ESP_OK;
err:

    return ESP_FAIL;
}

/**
 * @brief Get Operator's name
 *
 * @param sim800_dce sim800 object
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
static esp_err_t sim800_get_operator_name(sim800_modem_dce_t *sim800_dce)
{
    modem_dte_t *dte = sim800_dce->parent.dte;
    sim800_dce->parent.handle_line = sim800_handle_cops;
    DCE_CHECK(dte->send_cmd(dte, "AT+COPS?\r", MODEM_COMMAND_TIMEOUT_OPERATOR) == ESP_OK, "send command failed", err);
    DCE_CHECK(sim800_dce->parent.state == MODEM_STATE_SUCCESS, "get network operator failed", err);
    ESP_LOGD(DCE_TAG, "get network operator ok");
    return ESP_OK;
err:
    return ESP_FAIL;
}

/**
 * @brief Deinitialize SIM800 object
 *
 * @param dce Modem DCE object
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on fail
 */
static esp_err_t sim800_deinit(modem_dce_t *dce)
{
    sim800_modem_dce_t *sim800_dce = __containerof(dce, sim800_modem_dce_t, parent);
    if (dce->dte)
    {
        dce->dte->dce = NULL;
    }
    free(sim800_dce);
    return ESP_OK;
}

modem_dce_t *sim800_init(modem_dte_t *dte)
{
    DCE_CHECK(dte, "DCE should bind with a DTE", err);
    /* malloc memory for sim800_dce object */
    sim800_modem_dce_t *sim800_dce = calloc(1, sizeof(sim800_modem_dce_t));
    DCE_CHECK(sim800_dce, "calloc sim800_dce failed", err);
    /* Bind DTE with DCE */
    sim800_dce->parent.dte = dte;
    dte->dce = &(sim800_dce->parent);
    /* Bind methods */
    sim800_dce->parent.handle_line = sim800_handle_response_default;
    sim800_dce->parent.sync = esp_modem_dce_sync;
    sim800_dce->parent.echo_mode = esp_modem_dce_echo;
    sim800_dce->parent.store_profile = esp_modem_dce_store_profile;
    sim800_dce->parent.set_flow_ctrl = esp_modem_dce_set_flow_ctrl;
    sim800_dce->parent.define_pdp_context = esp_modem_dce_define_pdp_context;
    sim800_dce->parent.hang_up = esp_modem_dce_hang_up;
    sim800_dce->parent.get_signal_quality = sim800_get_signal_quality;
    sim800_dce->parent.get_battery_status = sim800_get_battery_status;
    sim800_dce->parent.set_working_mode = sim800_set_working_mode;
    sim800_dce->parent.power_down = sim800_power_down;
    sim800_dce->parent.deinit = sim800_deinit;
    sim800_dce->parent.handle_line_default = sim800_handle_response_default;
    
sync:
    /* Sync between DTE and DCE */
    vTaskDelay(500);
    DCE_CHECK(esp_modem_dce_sync(&(sim800_dce->parent)) == ESP_OK, "sync failed", sync);

    /* Close echo */
    DCE_CHECK(esp_modem_dce_echo(&(sim800_dce->parent), false) == ESP_OK, "close echo mode failed", err_io);

    /* Initialize modem. */
    static const char *const init_strings[] = {
        "+IPR=0",
        "+CMEE = 2",
        "+CMER=2,0,0,2,1",
        "+CLTS=1",
        "+IFC=0,0",
        "Q0",
        "&W",
        "+CFUN=1",
        NULL};

    for (const char *const *command = init_strings; *command; command++)
    {
        if (sim800_at(&(sim800_dce->parent), *command, MODEM_COMMAND_TIMEOUT_DEFAULT) == ESP_FAIL)
            printf("Setup AT commands failed: %s\n", *command);
    }

    uint32_t stat = 0, mode = 0;
    int attempts = 0;
    while (stat != 1 && attempts++ < 5)
    {
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        sim800_get_network_status(&(sim800_dce->parent), &mode,&stat);
    }

    /* Get Module name */
    DCE_CHECK(sim800_get_module_name(sim800_dce) == ESP_OK, "get module name failed", err_io);
    /* Get IMEI number */
    DCE_CHECK(sim800_get_imei_number(sim800_dce) == ESP_OK, "get imei failed", err_io);
    /* Get IMSI number */
    DCE_CHECK(sim800_get_imsi_number(sim800_dce) == ESP_OK, "get imsi failed. Check SIM Card.", err_io);
    /* Get operator name */
    DCE_CHECK(sim800_get_operator_name(sim800_dce) == ESP_OK, "get operator name failed", err_io);

    return &(sim800_dce->parent);
err_io:
    free(sim800_dce);
    
err:
    return NULL;
}

void sim800_power_on()
{
    gpio_config_t io_conf;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1 << SIM800_PWKEY) + (1 << SIM800_RST) + (1 << SIM800_POWER);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    ESP_LOGI(DCE_TAG, "Power up ...");
    set_sim800_pwrsrc();
    set_sim800_rst();
    clear_sim800_pwkey();

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    set_sim800_pwkey();
    vTaskDelay(1900 / portTICK_PERIOD_MS);
}

void sim800_power_off()
{
    ESP_LOGI(DCE_TAG, "Power down.");
    clear_sim800_pwrsrc();
}