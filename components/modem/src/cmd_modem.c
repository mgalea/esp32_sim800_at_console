#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "mqtt_client.h"
#include "esp_modem.h"
#include "esp_log.h"
#include "sim800.h"
#include "bg96.h"

#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "sdkconfig.h"

#define BROKER_URL "mqtt://mqtt.eclipse.org"

static EventGroupHandle_t event_group = NULL;
static const int CONNECT_BIT = BIT0;
static const int STOP_BIT = BIT1;
static const int GOT_DATA_BIT = BIT2;

static const char *TAG = "modem_cmd";

modem_dce_t *dce = NULL;
modem_dte_t *dte = NULL;

static void register_start();
static void register_stop();
static void register_get_operator();
static void register_at_command();
static void register_cls();

void register_modem_commands()
{
    register_start();
    register_stop();
    register_get_operator();
    register_at_command();
    register_cls();
}

static void modem_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case MODEM_EVENT_PPP_START:
        ESP_LOGI(TAG, "Modem PPP Started");
        break;

    case MODEM_EVENT_PPP_CONNECT:
        ESP_LOGI(TAG, "Modem Connected to PPP Server");
        ppp_client_ip_info_t *ipinfo = (ppp_client_ip_info_t *)(event_data);
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&ipinfo->ip));
        ESP_LOGI(TAG, "Netmask     : " IPSTR, IP2STR(&ipinfo->netmask));
        ESP_LOGI(TAG, "Gateway     : " IPSTR, IP2STR(&ipinfo->gw));
        ESP_LOGI(TAG, "Name Server1: " IPSTR, IP2STR(&ipinfo->ns1));
        ESP_LOGI(TAG, "Name Server2: " IPSTR, IP2STR(&ipinfo->ns2));
        ESP_LOGI(TAG, "~~~~~~~~~~~~~~");
        xEventGroupSetBits(event_group, CONNECT_BIT);
        break;

    case MODEM_EVENT_PPP_DISCONNECT:
        ESP_LOGI(TAG, "Modem Disconnect from PPP Server");
        xEventGroupClearBits(event_group, CONNECT_BIT);
        break;

    case MODEM_EVENT_PPP_STOP:
        ESP_LOGI(TAG, "Modem PPP Stopped");
        xEventGroupClearBits(event_group, CONNECT_BIT);
        xEventGroupSetBits(event_group, STOP_BIT);
        break;

    case MODEM_EVENT_UNKNOWN:
        ESP_LOGW(TAG, "Response received: %s", (char *)event_data);
        break;

    default:
        break;
    }
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        msg_id = esp_mqtt_client_subscribe(client, "/topic/esp-pppos", 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        msg_id = esp_mqtt_client_publish(client, "/topic/esp-pppos", "esp32-pppos", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);
        xEventGroupSetBits(event_group, GOT_DATA_BIT);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        ESP_LOGI(TAG, "MQTT other event id: %d\n", event->event_id);
        break;
    }
    return ESP_OK;
}

/****************************************************************/
/** @brief Start/stop modem - Start/stop modem module          */

static int start_modem()
{
    sim800_power_on();

    /* create dte object */
    esp_modem_dte_config_t config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte = esp_modem_dte_init(&config);

    printf("data terminal read");
   
    /* Register event handler */
    ESP_ERROR_CHECK(esp_modem_add_event_handler(dte, modem_event_handler, NULL));

   
    /* create dce object */
#if CONFIG_EXAMPLE_MODEM_DEVICE_SIM800
    dce = sim800_init(dte);

#elif CONFIG_EXAMPLE_MODEM_DEVICE_BG96
    dce = bg96_init(dte);
#else
    dce = sim800_init(dte);
#endif
    if (dce == NULL)
        goto err;
    return 0;
    ESP_ERROR_CHECK(dce->set_flow_ctrl(dce, MODEM_FLOW_CONTROL_NONE));
    ESP_ERROR_CHECK(dce->store_profile(dce));

    /* Print Module ID, Operator, IMEI, IMSI */
    ESP_LOGI(TAG, "Module: %s", dce->name);
    ESP_LOGI(TAG, "IMEI: %s", dce->imei);
    ESP_LOGI(TAG, "IMSI: %s", dce->imsi);

    return 0;
err:
    esp_modem_remove_event_handler(dte, modem_event_handler);
    esp_modem_dte_deinit(dte);
    dce = NULL;
    dte = NULL;
    printf("Error starting modem");
    return 0;
}

/* Power down Modem module and stop the DCE/DTE interface*/
int stop_modem()
{


    if (dce != NULL)
    {
        modem_dte_t *dte = dce->dte;
        ESP_ERROR_CHECK(dce->power_down(dce));
        ESP_ERROR_CHECK(dce->deinit(dce));
        dce = NULL;
    }

    if (dte != NULL)
    {
        ESP_ERROR_CHECK(dte->deinit(dte));
        dte = NULL;
    }

    sim800_power_off();
    return 0;
}

static int get_operator()
{
    printf("%s\r\n", dce->oper);
    return 0;
}

static void register_get_operator()
{
    const esp_console_cmd_t cmd = {
        .command = "operator",
        .help = "Get Network Operator Name",
        .hint = "no arguments",
        .func = &get_operator,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static struct
{
    struct arg_str *suffix;
    struct arg_end *end;
} at_args;

static int at_command(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&at_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, at_args.end, argv[0]);
        return 1;
    }
    if (at_args.suffix->count)
    {
        const char *at_command = at_args.suffix->sval[0];
        sim800_at(dce, at_command, 3000);
    }
    return 0;
}

static void register_at_command()
{
    at_args.suffix = arg_str1(NULL, NULL, "<command>", "at command");
    at_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "AT",
        .help = "AT command based on the 3GPP standard",
        .hint = "no spaces between commands.",
        .func = &at_command,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/****************************************************************/
/** @brief ppp - enter/exit PPP mode                           */

void init_ppp(modem_dce_t *dce)
{
    modem_dte_t *dte = dce->dte;

    if (!event_group)
        event_group = xEventGroupCreate();

    esp_modem_setup_ppp(dte);
}

/* Exit PPP mode */
void de_init_ppp(modem_dce_t *dce)
{
    modem_dte_t *dte = dce->dte;
    ESP_ERROR_CHECK(esp_modem_exit_ppp(dte));
    xEventGroupWaitBits(event_group, STOP_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
}

/****************************************************************/
/** @brief CLS - Clear Screen Command                          */
static int cls()
{
    printf("\033[2J\n\n\033[H\n");
    return 0;
}

static void register_cls()
{
    const esp_console_cmd_t cmd = {
        .command = "cls",
        .help = "Clear screen",
        .hint = "no arguments",
        .func = &cls,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void start_mqtt_connection()
{

    /* Wait for IP address */
    xEventGroupWaitBits(event_group, CONNECT_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
    /* Config MQTT */
    esp_mqtt_client_config_t mqtt_config = {
        .uri = BROKER_URL,
        .event_handle = mqtt_event_handler,
    };
    esp_mqtt_client_handle_t mqtt_client = esp_mqtt_client_init(&mqtt_config);
    esp_mqtt_client_start(mqtt_client);

    xEventGroupWaitBits(event_group, GOT_DATA_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
    esp_mqtt_client_destroy(mqtt_client);
}

void start_life()
{
    printf(
        "                                                                                            `-----\n"
        "                    -:---------`                                                      .-----.     \n"
        "              `-----`         `-:::---:                                      `--------`           \n"
        "          `-:-.         `              /---`                       `----:----.                    \n"
        "  `.-------           ` ..          -  .  `-:-                `-::--`    `-..`                    \n"
        "---`              `-.`-:-/:      `o+--+.o-:: -:    .-:-:------.        `..   ``                   \n"
        "                `.. .:.   `::::-.  :o:-+-+ .s:`o //--:://---  `:.    ..  `-::--://::-..           \n"
        "                  `/-           `-:: `/+-/: +`--- .-`      -y::`...    .:`------.``````           \n"
        "                .::                `+/:+../:.:           /// .-/-::.-::.:--     `..--...----::----\n"
        "            `:::`                   -...  .:-`           //:/+-`o`.:-::-                          \n"
        "          :/-                                             / / .:+ +`                              \n"
        "       `::`                                               +`/`/ +`+                               \n"
        "  `-:::.                                                  --/-` `-                                \n"
        "--.`                                                                                              \n");
}

static int start_test()
{
    for (int x = 0; x < 10; x++)
    {
        printf("%d: Pre start: %d\n", x, esp_get_free_heap_size());
        start_modem();

        printf("%d: started: %d\n", x, esp_get_free_heap_size());
        stop_modem();
        printf("----------------------------\n");
    }
    return 0;
}

/****************************************************************/
/** @brief start - start something                             */

static struct
{
    struct arg_str *suffix;
    struct arg_end *end;
} start_args;

static int start_command(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&start_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, start_args.end, argv[0]);
        return 1;
    }
    if (start_args.suffix->count)
    {

        if (strstr(start_args.suffix->sval[0], "modem"))
        {
            start_modem();
        }

        if (strstr(start_args.suffix->sval[0], "ppp"))
        {
            init_ppp(dce);
        }

        if (strstr(start_args.suffix->sval[0], "mqtt"))
        {
            start_mqtt_connection();
        }

        if (strstr(start_args.suffix->sval[0], "life"))
        {
            start_life();
        }

        if (strstr(start_args.suffix->sval[0], "test"))
        {
            start_test();
        }
    }
    return 0;
}

static void register_start()
{
    start_args.suffix = arg_str1(NULL, NULL, "<command>", "modem command");
    start_args.end = arg_end(2);
    const esp_console_cmd_t cmd = {
        .command = "start",
        .help = "Start or stop the Something (DCE)",
        .hint = "[modem|ppp]",
        .func = &start_command,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

/****************************************************************/
/** @brief stop - stop something                               */
static struct
{
    struct arg_str *suffix;
    struct arg_end *end;
} stop_args;

static int stop_command(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&stop_args);
    if (nerrors != 0)
    {
        arg_print_errors(stderr, stop_args.end, argv[0]);
        return 1;
    }
    if (stop_args.suffix->count)
    {
        if (strstr(stop_args.suffix->sval[0], "modem"))
        {
            stop_modem();
        }

        if (strstr(stop_args.suffix->sval[0], "ppp"))
        {
            de_init_ppp(dce);
        }
    }
    return 0;
}

static void register_stop()
{
    stop_args.suffix = arg_str1(NULL, NULL, "<command>", "modem command");
    stop_args.end = arg_end(2);
    const esp_console_cmd_t cmd = {
        .command = "stop",
        .help = "Start or stop the Something (DCE)",
        .hint = "[modem|ppp]",
        .func = &stop_command,
    };

    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void console_send_raw(const char *line)
{
    sim800_send_raw(dce, line, 3000);
}