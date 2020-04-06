#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <esp_log.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "bluetooth_service.h"
#include <esp_ota_ops.h>
#include "board.h"
#include <esp_event_loop.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_http_server.h>
#include <esp_bt_device.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_gap_bt_api.h>
#include <esp_spiffs.h>
#include <sys/param.h>
#include <string>
#include <memory>
#include "utils.hpp"
#include "netLogger.hpp"
#include "httpFile.hpp"
#include "ota.hpp"
#include "audioPlayer.hpp"

static constexpr gpio_num_t kPinButton = GPIO_NUM_27;
static constexpr gpio_num_t kPinRollbackButton = GPIO_NUM_32;
static constexpr gpio_num_t kPinLed = GPIO_NUM_2;

static const char *TAG = "netplay";
static const char* kStreamUrls[] = {
    "http://stream01048.westreamradio.com:80/wsm-am-mp3",
    "http://94.23.252.14:8067/player"
};

        // "http://icestreaming.rai.it/12.mp3");
        // "http://94.23.252.14:8067/player");
        // "https://dl.espressif.com/dl/audio/ff-16b-2c-44100hz.mp3");
        // BBC m4a "http://a.files.bbci.co.uk/media/live/manifesto/audio/simulcast/hls/nonuk/sbr_low/llnw/bbc_radio_one.m3u8"

httpd_handle_t gHttpServer = nullptr;
AudioPlayer player(AudioPlayer::kOutputI2s);

void reconfigDhcpServer();
void startWifiSoftAp();
void startWebserver(bool isAp=false);

esp_periph_set_handle_t periphSet;

const char* getNextStreamUrl() {
    static int currStreamIdx = -1;
    currStreamIdx++;
    if (currStreamIdx >= (sizeof(kStreamUrls) / sizeof(kStreamUrls[0]))) {
        currStreamIdx = 0;
    }
    return kStreamUrls[currStreamIdx];
}

void configGpios()
{
    gpio_pad_select_gpio(kPinButton);
    gpio_set_direction(kPinButton, GPIO_MODE_INPUT);
    gpio_pullup_en(kPinButton);

    gpio_pad_select_gpio(kPinRollbackButton);
    gpio_set_direction(kPinRollbackButton, GPIO_MODE_INPUT);
    gpio_pullup_en(kPinRollbackButton);

    gpio_pad_select_gpio(kPinLed);
    gpio_set_direction(kPinLed, GPIO_MODE_OUTPUT);
}

NetLogger netLogger(false);

void mountSpiffs()
{
    ESP_LOGI(TAG, "Initializing SPIFFS");

     esp_vfs_spiffs_conf_t conf = {
       .base_path = "/spiffs",
       .partition_label = "storage",
       .max_files = 5,
       .format_if_mount_failed = true
     };

     // Use settings defined above to initialize and mount SPIFFS filesystem.
     // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
     esp_err_t ret = esp_vfs_spiffs_register(&conf);

     if (ret != ESP_OK) {
         if (ret == ESP_FAIL) {
             ESP_LOGE(TAG, "Failed to mount or format filesystem");
         } else if (ret == ESP_ERR_NOT_FOUND) {
             ESP_LOGE(TAG, "Failed to find SPIFFS partition");
         } else {
             ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
         }
         return;
     }

     size_t total = 0, used = 0;
     ret = esp_spiffs_info(conf.partition_label, &total, &used);
     if (ret != ESP_OK) {
         ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
     } else {
         ESP_LOGW(TAG, "Partition size: total: %d, used: %d", total, used);
     }
}
bool rollbackCheckUserForced()
{
    if (gpio_get_level(kPinRollbackButton)) {
        return false;
    }

    static constexpr const char* RB = "ROLLBACK";
    ESP_LOGW("RB", "Rollback button press detected, waiting for 4 second to confirm...");
    vTaskDelay(4000 / portTICK_PERIOD_MS);
    if (gpio_get_level(kPinRollbackButton)) {
        ESP_LOGW("RB", "Rollback not pressed after 1 second, rollback canceled");
        return false;
    }
    ESP_LOGW(RB, "App rollback requested by user button, rolling back and rebooting...");
    setOtherPartitionBootableAndRestart();
    return true;
}

extern "C" void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    player.setLogLevel(ESP_LOG_DEBUG);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    configGpios();

    mountSpiffs();
    tcpip_adapter_init();
    rollbackCheckUserForced();
    rollbackConfirmAppIsWorking();
    if (!gpio_get_level(kPinButton)) {
        ESP_LOGW(TAG, "Button pressed at boot, start as access point for configuration");
        startWifiSoftAp();
        startWebserver(true);
        return;
    }
//== WIFI
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    periphSet = esp_periph_set_init(&periph_cfg);

    ESP_LOGI(TAG, "Start and wait for Wi-Fi network");
    periph_wifi_cfg_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    wifi_cfg.ssid = CONFIG_WIFI_SSID;
    wifi_cfg.password = CONFIG_WIFI_PASSWORD;
    esp_periph_handle_t wifi_handle = periph_wifi_init(&wifi_cfg);
    esp_periph_start(periphSet, wifi_handle);
    periph_wifi_wait_for_connected(wifi_handle, portMAX_DELAY);
//====

    startWebserver();
    netLogger.waitForLogConnection();
    ESP_LOGI(TAG, "Log connection accepted, continuing");
    player.setSourceUrl(getNextStreamUrl(), AudioPlayer::kCodecMp3);
    player.play();

//  esp_periph_set_destroy(periphSet);
}

static esp_err_t indexUrlHandler(httpd_req_t *req)
{
    static const char indexHtml[] =
        "<html><head /><body><h>NetPlayer HTTP server</h><br/>Free heap memory: ";
    httpd_resp_send_chunk(req, indexHtml, sizeof(indexHtml));
    char buf[32];
    snprintf(buf, sizeof(buf)-1, "%d", xPortGetFreeHeapSize());
    httpd_resp_send_chunk(req, buf, strlen(buf));
    static const char indexHtmlEnd[] = "</body></html>";
    httpd_resp_send_chunk(req, indexHtmlEnd, sizeof(indexHtmlEnd));
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}
static const httpd_uri_t indexUrl = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = indexUrlHandler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = nullptr
};

/* An HTTP GET handler */
static esp_err_t playUrlHandler(httpd_req_t *req)
{
    UrlParams params(req);
    for (auto& param: params.keyVals()) {
        ESP_LOGI("URL", "'%s' = '%s'", param.key.str, param.val.str);
    }
    auto url = params.strParam("url");
    if (!url) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "URL parameter not found");
        ESP_LOGE("http", "Url param not found, query:'%s'", params.ptr());
        return ESP_OK;
    }
    ESP_LOGW("HTTP", "Http req url: %s", url.str);
    auto strUrl = getNextStreamUrl();
    player.setSourceUrl(strUrl, AudioPlayer::kCodecMp3);
    std::string msg("Changing stream url to '");
    msg.append(strUrl).append("'");
    httpd_resp_send(req, msg.c_str(), msg.size());
    return ESP_OK;
}

static const httpd_uri_t play = {
    .uri       = "/play",
    .method    = HTTP_GET,
    .handler   = playUrlHandler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = nullptr
};

void stopWebserver() {
    /* Stop the web server */
    if (!gHttpServer) {
        return;
    }
    netLogger.unregisterWithHttpServer("/log");
    httpd_stop(gHttpServer);
    gHttpServer = nullptr;
}

void startWebserver(bool isAp)
{
    if (gHttpServer) {
        stopWebserver();
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&gHttpServer, &config) != ESP_OK) {
        ESP_LOGI(TAG, "Error starting server!");
        return;
    }
    // Set URI handlers
    ESP_LOGI(TAG, "Registering URI handlers");
    netLogger.registerWithHttpServer(gHttpServer, "/log");
    httpd_register_uri_handler(gHttpServer, &otaUrlHandler);
    httpd_register_uri_handler(gHttpServer, &indexUrl);
    httpd_register_uri_handler(gHttpServer, &httpFsPut);
    httpd_register_uri_handler(gHttpServer, &httpFsGet);

    if (!isAp) {
        httpd_register_uri_handler(gHttpServer, &play);
    }
}

void reconfigDhcpServer()
{
    // stop DHCP server
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP));
    // assign a static IP to the network interface
    tcpip_adapter_ip_info_t info;
    memset(&info, 0, sizeof(info));

    IP4_ADDR(&info.ip, 192, 168, 0, 1);
    IP4_ADDR(&info.gw, 192, 168, 0, 1); //ESP acts as router, so gw addr will be its own addr
    IP4_ADDR(&info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(tcpip_adapter_set_ip_info(TCPIP_ADAPTER_IF_AP, &info));
    // start the DHCP server
    ESP_ERROR_CHECK(tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP));
    printf("DHCP server started \n");
}

static esp_err_t apEventHandler(void *ctx, system_event_t *event)
{
    switch(event->event_id) {
    case SYSTEM_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "station:" MACSTR " join, AID=%d",
                 MAC2STR(event->event_info.sta_connected.mac),
                 event->event_info.sta_connected.aid);
        break;
    case SYSTEM_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "station:" MACSTR "leave, AID=%d",
                 MAC2STR(event->event_info.sta_disconnected.mac),
                 event->event_info.sta_disconnected.aid);
        break;
    default:
        break;
    }
    return ESP_OK;
}
void startWifiSoftAp()
{
    ESP_ERROR_CHECK(esp_event_loop_init(apEventHandler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    //ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

//  reconfigDhcpServer();

    // configure the wifi connection and start the interface
    wifi_config_t ap_config;
    auto& ap = ap_config.ap;
    strcpy((char*)ap.ssid, "NetPlayer");
    strcpy((char*)ap.password, "net12player");
    ap.ssid_len = 0;
    ap.channel = 4;
    ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ssid_hidden = 0;
    ap.max_connection = 8;
    ap.beacon_interval = 400;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    printf("ESP WiFi started in AP mode \n");
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(20));
}

