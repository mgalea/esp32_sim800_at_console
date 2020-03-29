#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "tcpip_adapter.h"
#include "mqtt_client.h"
#include "esp_modem.h"
#include "esp_log.h"
#include "sim800.h"
#include "bg96.h"
#include "driver/gpio.h"

#include "esp_console.h"
#include "argtable3/argtable3.h"
#include "sdkconfig.h"

#define BROKER_URL "mqtt://mqtt.eclipse.org"

#define SIM800_PWKEY CONFIG_EXAMPLE_UART_MODEM_PWKEY
#define set_sim800_pwkey() gpio_set_level(SIM800_PWKEY, 1)
#define clear_sim800_pwkey() gpio_set_level(SIM800_PWKEY, 0)

#define SIM800_RST CONFIG_EXAMPLE_UART_MODEM_RST
#define set_sim800_rst() gpio_set_level(SIM800_RST, 1)
#define clear_sim800_rst() gpio_set_level(SIM800_RST, 0)

#define SIM800_POWER 23
#define set_sim800_pwrsrc() gpio_set_level(SIM800_POWER, 1)
#define clear_sim800_pwrsrc() gpio_set_level(SIM800_POWER, 0)

static EventGroupHandle_t event_group = NULL;
static const int CONNECT_BIT = BIT0;
static const int STOP_BIT = BIT1;
static const int GOT_DATA_BIT = BIT2;

static const char *TAG = "modem_cmd";

modem_dce_t *dce;

static void register_start_modem();
static void register_stop_modem();
static void register_get_operator();
static void register_at_command();

void register_modem()
{
    register_start_modem();
    register_stop_modem();
    register_get_operator();
    register_at_command();
}

static void modem_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case MODEM_EVENT_PPP_START:
        ESP_LOGI(TAG, "Modem PPP Started");
        break;
    case MODEM_EVENT_PPP_CONNECT:
        ESP_LOGI(TAG, "Modem Connect to PPP Server");
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
        break;
    case MODEM_EVENT_PPP_STOP:
        ESP_LOGI(TAG, "Modem PPP Stopped");
        xEventGroupSetBits(event_group, STOP_BIT);
        break;
    case MODEM_EVENT_UNKNOWN:
        ESP_LOGW(TAG, "Unknow line received: %s", (char *)event_data);
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

static int initialize_modem()
{
    gpio_config_t io_conf;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1 << SIM800_PWKEY) + (1 << SIM800_RST) + (1 << SIM800_POWER);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    set_sim800_pwrsrc();
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    ESP_LOGI(TAG, "Resetting SIM800...");
    set_sim800_rst();
    set_sim800_pwkey();

    vTaskDelay(1000 / portTICK_PERIOD_MS);

    ESP_LOGI(TAG, "Initializing SIM800...");
    clear_sim800_pwkey();
    vTaskDelay(1100 / portTICK_PERIOD_MS);
    set_sim800_pwkey();

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    clear_sim800_rst();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    set_sim800_rst();

    /* create dte object */
    esp_modem_dte_config_t config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    modem_dte_t *dte = esp_modem_dte_init(&config);

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
    ESP_ERROR_CHECK(dce->set_flow_ctrl(dce, MODEM_FLOW_CONTROL_NONE));
    ESP_ERROR_CHECK(dce->store_profile(dce));

    /* Print Module ID, Operator, IMEI, IMSI */
    ESP_LOGI(TAG, "Module: %s", dce->name);
    ESP_LOGI(TAG, "IMEI: %s", dce->imei);
    ESP_LOGI(TAG, "IMSI: %s", dce->imsi);

    sim800_set_default_line_handler(dce);
    return 0;
}

static void register_start_modem()
{
    const esp_console_cmd_t cmd = {
        .command = "poweron",
        .help = "Start and Initialize the modem (DCE)",
        .hint = "no arguments",
        .func = &initialize_modem,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
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

int de_initialize_modem()
{
    modem_dte_t *dte = dce->dte;
    /* Power down module */
    ESP_ERROR_CHECK(dce->power_down(dce));
    ESP_LOGI(TAG, "Power down");
    ESP_ERROR_CHECK(dce->deinit(dce));
    ESP_ERROR_CHECK(dte->deinit(dte));
    clear_sim800_pwrsrc();
    return 0;
}

static void register_stop_modem()
{
    const esp_console_cmd_t cmd = {
        .command = "poweroff",
        .help = "Stop the modem (DCE)",
        .hint = "no aguments",
        .func = &de_initialize_modem,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

static struct {
    struct arg_str *suffix;
    struct arg_end *end;
} at_args;

static int at_command(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&at_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, at_args.end, argv[0]);
        return 1;
    }
        if (at_args.suffix->count) {

        ESP_LOGI(TAG, "AT Command %s", at_args.suffix->sval[0]);
        const char *at_command=at_args.suffix->sval[0];
        sim800_at(dce, at_command);
    }
    return 0;
}

static void register_at_command()
{
    at_args.suffix = arg_str1(NULL, NULL, "<command>", "at command");
    at_args.end = arg_end(2);

    const esp_console_cmd_t cmd = {
        .command = "AT",
        .help = "the Hayes modem AT command",
        .hint = "no spaces between commands.",
        .func = &at_command,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}

void initialize_ppp(modem_dte_t *dte)
{
    tcpip_adapter_init();
    event_group = xEventGroupCreate();

    esp_modem_setup_ppp(dte);
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
    /* Exit PPP mode */
    ESP_ERROR_CHECK(esp_modem_exit_ppp(dte));
    xEventGroupWaitBits(event_group, STOP_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
}
