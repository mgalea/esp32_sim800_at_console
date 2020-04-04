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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_modem_dce_service.h"
#include "sim800.h"
#include <time.h>

#define MODEM_RESULT_CODE_POWERDOWN "POWER DOWN"

#define SIM800_AUTOBAUD_ATTEMPTS 5
#define SIM800_WAITACK_TIMEOUT 40
#define SIM800_FTP_TIMEOUT 60
#define SET_TIMEOUT 60
#define NTP_BUF_SIZE 4
#define SIM800_NSOCKETS 6
#define OPERATOR_NETWORK_RETRIES 3

#define MODEM_READY BIT0
#define NETWORK_OK BIT1

static const char *const sim800_urc_responses[] = {
    "+CIPRXGET: 1,",   /* incoming socket data notification */
    "+FTPGET: 1,",     /* FTP state change notification */
    "+PDP: DEACT",     /* PDP disconnected */
    "+SAPBR 1: DEACT", /* PDP disconnected (for SAPBR apps) */
    "*PSNWID: ",       /* AT+CLTS network name */
    "*PSUTTZ: ",       /* AT+CLTS time */
    "+CTZV: ",         /* AT+CLTS timezone */
    "DST: ",           /* AT+CLTS dst information */
    "+CIEV: ",         /* AT+CMER level bar change indicator */
    "RDY",
    "+CPIN: ",
    "+CSSI:",
    "+CSSU:",
    "+CREG: ",
    "+CSQN:",
    "+CFUN: ",
    "Call Ready",
    "SMS Ready",
    "NORMAL POWER DOWN",
    "UNDER-VOLTAGE POWER DOWN",
    "UNDER-VOLTAGE WARNNING",
    "OVER-VOLTAGE POWER DOWN",
    "OVER-VOLTAGE WARNNING",
    NULL};

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
 * @brief SIM800 Modem
 *
 */
typedef struct
{

    void *priv_resource; /*!< Private resource */
    modem_dce_t parent;  /*!< DCE parent class */
    modem_connect_state_t connection;
    modem_status_t status;
} sim800_modem_dce_t;

static bool at_prefix_in_table(const char *line, const char *const table[])
{
    for (int i = 0; table[i] != NULL; i++)
        if (!strncmp(line, table[i], strlen(table[i])))
            return true;

    return false;
}

esp_err_t sim800_handle_response_default(modem_dce_t *dce, const char *line)
{
    esp_err_t err = ESP_FAIL;

    if (at_prefix_in_table(line, sim800_urc_responses))
    {
        printf("\033[1;33m%s\033[0m", line);
        return ESP_OK;
    }

    /* Socket status notifications in form of "%d, <status>". */
    if (line[0] >= '0' && line[0] <= '0' + SIM800_NSOCKETS &&
        !strncmp(line + 1, ", ", 2))
    {
        int socket = line[0] - '0';

        if (!strcmp(line + 3, "CONNECT OK"))
        {
            //priv->socket_status[socket] = SIM800_SOCKET_STATUS_CONNECTED;
            return ESP_OK;
        }

        if (!strcmp(line + 3, "CONNECT FAIL") ||
            !strcmp(line + 3, "ALREADY CONNECT") ||
            !strcmp(line + 3, "CLOSED"))
        {
            //priv->socket_status[socket] = SIM800_SOCKET_STATUS_ERROR;
            return ESP_OK;
        }
    }

    if (strstr(line, MODEM_RESULT_CODE_SUCCESS))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
        return ESP_OK;
    }
    else if (strstr(line, MODEM_RESULT_CODE_ERROR))
    {
        err = esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    }

    return err;
}

esp_err_t sim800_handle_at_command(modem_dce_t *dce, const char *line)
{
    sim800_modem_dce_t *sim800_dce = __containerof(dce, sim800_modem_dce_t, parent);

    esp_err_t err = ESP_OK;

    printf("\033[1;32m%s\033[0m", line);

    if (strstr(line, MODEM_RESULT_CODE_SUCCESS))
    {
        xSemaphoreGive(sim800_dce->parent.atcmdHandle);
        err = esp_modem_process_command_done(dce, MODEM_STATE_SUCCESS);
    }
    else if (strstr(line, MODEM_RESULT_CODE_ERROR))
    {
        xSemaphoreGive(sim800_dce->parent.atcmdHandle);
        err = esp_modem_process_command_done(dce, MODEM_STATE_FAIL);
    }
    else
    {
        xSemaphoreTake(sim800_dce->parent.atcmdHandle, 1);
    }

    return err;
}

esp_err_t sim800_at(modem_dce_t *dce, const char *at_command, uint8_t timeout)
{
    DCE_CHECK(dce, "Data Communication Equipment is not Connected to Data Terminal", err);
    modem_dte_t *dte = dce->dte;
    char send_cmd[50] = "AT";
    strcat(send_cmd, at_command);
    strcat(send_cmd, "\r\n");

    if (timeout == 0)
        timeout++;
    dce->handle_line = sim800_handle_at_command;
    //DCE_CHECK(dte->send_wait(dte,send_cmd, 255, "OK\r\n", MODEM_COMMAND_TIMEOUT_DEFAULT) == ESP_OK, "send command failed", err);
    // DCE_CHECK(dte->send_cmd(dte, send_cmd, MODEM_COMMAND_TIMEOUT_DEFAULT) == ESP_OK, "send command failed", err);
    DCE_CHECK(dte->send_at(dte, send_cmd, timeout * 50) == ESP_OK, "send command failed", err);
    DCE_CHECK(dce->state == MODEM_STATE_SUCCESS, "ERROR response", err);
    dce->handle_line = sim800_handle_response_default;
    return ESP_OK;
err:
    return ESP_FAIL;
}

esp_err_t sim800_sync(modem_dce_t *dce)
{
    modem_dte_t *dte = dce->dte;

fail:
    vTaskDelay(pdMS_TO_TICKS(500));
    dce->handle_line = sim800_handle_at_command;
    DCE_CHECK(sim800_at(dce, " ", 2) == ESP_OK, "send command failed", fail);
    DCE_CHECK(dce->state == MODEM_STATE_SUCCESS, "sync failed", err);
    ESP_LOGD(DCE_TAG, "sync ok");
    return ESP_OK;
err:

    return ESP_FAIL;
}

esp_err_t sim800_echo(modem_dce_t *dce, bool on)
{
    dce->handle_line = sim800_handle_at_command;
    if (on)
    {
        DCE_CHECK(sim800_at(dce, "E1", 1) == ESP_OK, "send command failed", err);
        DCE_CHECK(dce->state == MODEM_STATE_SUCCESS, "enable echo failed", err);
        ESP_LOGD(DCE_TAG, "enable echo ok");
    }
    else
    {
        DCE_CHECK(sim800_at(dce, "E0", 1) == ESP_OK, "send command failed", err);
        DCE_CHECK(dce->state == MODEM_STATE_SUCCESS, "disable echo failed", err);
        ESP_LOGD(DCE_TAG, "disable echo ok");
    }
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

    memset(&tm, 0, sizeof(struct tm));
    at_simple_scanf(line, "+CCLK: \"%d/%d/%d,%d:%d:%d%*d\"",
                    &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                    &tm.tm_hour, &tm.tm_min, &tm.tm_sec);

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

    /* All good. Return the result. */
    err = ESP_OK;
    struct timespec *ts = sim800_dce->priv_resource;
    ts->tv_sec = unix_time;
    ts->tv_nsec = 0;
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
        /* strtok will break the string so we need to create a copy */
        size_t len = strlen(line);
        char *line_copy = malloc(len + 1);
        strcpy(line_copy, line);
        /* +COPS: <mode>[, <format>, <oper>] */
        char *str_ptr = NULL;
        char *p[4];
        uint8_t i = 0;
        /* strtok will break string by replacing delimiter with '\0' */
        p[i] = strtok_r(line_copy, ":,", &str_ptr);
        while (p[i])
        {
            p[++i] = strtok_r(NULL, ",", &str_ptr);
        }
        if (i >= 4)
        {
            int len = snprintf(dce->oper, MODEM_MAX_OPERATOR_LENGTH, "%s", p[i - 1]);
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
        uint32_t **creg = sim800_dce->priv_resource;

        /* +CREG: <n>,<stat> */
        sscanf(line, "%*s%d,%d%*s", creg[0], creg[1]);
        err = ESP_OK;
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
 * @brief Get signal quality
 *
 * @param dce Modem DCE object
 * @param rssi received signal strength indication
 * @param ber bit error ratio
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
static esp_err_t sim800_get_network_registration_status(modem_dce_t *dce, uint32_t *mode, uint32_t *stat)
{
    modem_dte_t *dte = dce->dte;
    sim800_modem_dce_t *sim800_dce = __containerof(dce, sim800_modem_dce_t, parent);
    uint32_t *resource[2] = {mode, stat};
    sim800_dce->priv_resource = resource;
    dce->handle_line = sim800_handle_creg;
    DCE_CHECK(dte->send_cmd(dte, "AT+CREG?\r", MODEM_COMMAND_TIMEOUT_DEFAULT) == ESP_OK, "send command failed", err);
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
 * @brief Get Network Clock/time
 *
 * @param dce Modem DCE object
 * @param ts POSIX Timespec structure
 * @return esp_err_t
 *      - ESP_OK on success
 *      - ESP_FAIL on error
 */
esp_err_t sim800_op_clock_gettime(modem_dce_t *dce)
{
    modem_dte_t *dte = dce->dte;
    dce->handle_line = sim800_handle_cclk;
    DCE_CHECK(dte->send_cmd(dte, "AT+CCLK?\r", MODEM_COMMAND_TIMEOUT_DEFAULT) == ESP_OK, "send command failed", err);
    DCE_CHECK(dce->state == MODEM_STATE_SUCCESS, "op clock failed", err);
    ESP_LOGD(DCE_TAG, "sync ok");
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
    DCE_CHECK(sim800_dce, "Calloc sim800_dce failed", err);

    /* Bind DTE with DCE */
    sim800_dce->parent.dte = dte;
    dte->dce = &(sim800_dce->parent);
    printf("DCE");
    /* Bind methods */
    sim800_dce->parent.handle_line = sim800_handle_response_default;
    sim800_dce->parent.sync = sim800_sync;
    sim800_dce->parent.echo_mode = sim800_echo;
    sim800_dce->parent.store_profile = esp_modem_dce_store_profile;
    sim800_dce->parent.set_flow_ctrl = esp_modem_dce_set_flow_ctrl;
    sim800_dce->parent.define_pdp_context = esp_modem_dce_define_pdp_context;
    sim800_dce->parent.hang_up = esp_modem_dce_hang_up;
    sim800_dce->parent.get_signal_quality = sim800_get_signal_quality;
    sim800_dce->parent.get_battery_status = sim800_get_battery_status;
    sim800_dce->parent.set_working_mode = sim800_set_working_mode;
    sim800_dce->parent.power_down = sim800_power_down;
    sim800_dce->parent.deinit = sim800_deinit;
    sim800_dce->parent.mode = MODEM_COMMAND_MODE;
    sim800_dce->status = 0;
    sim800_dce->parent.handle_line_default = sim800_handle_response_default;

    sim800_dce->parent.atcmdHandle = xSemaphoreCreateBinary();

    DCE_CHECK(sim800_dce->parent.atcmdHandle, "Semaphore failed", err_io);

    /* Perform autobauding. */
    /* Sync between DTE and DCE */

    sync_again:
    vTaskDelay(1);
    DCE_CHECK(sim800_sync(dte->dce)==ESP_OK,"SYNC fail.",sync_again);

     /* Initialize modem. */
    static const char *const init_strings[] = {
        "E0;+IPR=0;",
        "+CMER=0,0,0,1,1;+CMEE=1;+CIURC=0;+CLTS=1;+IFC=0,0;&W0",
        NULL};

    for (const char *const *command = init_strings; *command; command++)
    {
        if(sim800_at(&(sim800_dce->parent), *command,4) == ESP_FAIL)
            printf("Setup AT commands failed: %s\n",*command);
    }

    uint32_t stat = 0, mode = 0;

    while (stat != 1)
    {
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        sim800_get_network_registration_status(&(sim800_dce->parent), &mode, &stat);
    }

    /* Get Module name */
    DCE_CHECK(sim800_get_module_name(sim800_dce) == ESP_OK, "get module name failed", err_io);
    /* Get IMEI number */
    DCE_CHECK(sim800_get_imei_number(sim800_dce) == ESP_OK, "get imei failed", err_io);
    /* Get IMSI number */
    DCE_CHECK(sim800_get_imsi_number(sim800_dce) == ESP_OK, "get imsi failed", err_io);
    /* Get operator name */
    DCE_CHECK(sim800_get_operator_name(sim800_dce) == ESP_OK, "get imsi failed", err_io);

    return &(sim800_dce->parent);

err_io:
    free(sim800_dce);

err:
    return NULL;
}
