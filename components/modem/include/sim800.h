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
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_modem_dce_service.h"
#include "esp_modem.h"

typedef enum {
    MODEM_DISCONNECTED=0, /*!< In processing */
    MODEM_CONNECTED,    
} modem_connect_state_t;

typedef enum {
    UNKNOWN=0, /*!< In processing */
    DCE_READY,    
} modem_status_t;


/**
 * @brief Create and initialize SIM800 object
 *
 * @param dte Modem DTE object
 * @return modem_dce_t* Modem DCE object
 */
modem_dce_t *sim800_init(modem_dte_t *dte);
void sim800_set_default_line_handler(modem_dce_t *dce);
esp_err_t sim800_at(modem_dce_t *dce, const char *at_command,uint16_t timeout);
esp_err_t sim800_send_raw(modem_dce_t *dce, const char *line, uint16_t timeout);
void sim800_power_on();
void sim800_power_off();

#define _NUMARGS(...) (sizeof((void *[]){0, ##__VA_ARGS__})/sizeof(void *)-1)

/**
 * Scanf a response and return -1 if it fails.
 */
#define at_simple_scanf(_response, format, ...)                             \
    do {                                                                    \
        if (!_response)                                                     \
            return -1; /* timeout */                                        \
        if (sscanf(_response, format, __VA_ARGS__) != _NUMARGS(__VA_ARGS__)) { \
            return -1;                                                      \
        }                                                                   \
    } while (0)
