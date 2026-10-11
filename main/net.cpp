// Two ways to meet the phone, both always on:
//  - Access point "MiniNav": the phone joins it; the address DHCP hands the phone is where its
//    Android Auto headunit server listens. Some phones (Samsung) then lose mobile internet.
//  - Phone hotspot: the board joins the phone's hotspot "MiniNavHotspot" as a station and talks
//    to the headunit server at the hotspot's gateway address. The phone keeps its internet.
// The hotspot wins when both are up.
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "lwip/ip4_addr.h"
#include "app.h"

static const char *TAG = "net";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_phone_ip[16];      // phone on our access point
static char s_hotspot_ip[16];    // phone's hotspot gateway, when joined
static volatile bool s_sta_connected;
static int s_ap_clients;

static void show_ip(void) {
    char ip[16] = "";
    net_phone_ip(ip, sizeof ip);
    strlcpy(g_app.phone_ip, ip, sizeof g_app.phone_ip);
}

bool net_phone_ip(char *out, int len) {
    portENTER_CRITICAL(&s_lock);
    const char *src = s_hotspot_ip[0] ? s_hotspot_ip : s_phone_ip;
    bool ok = src[0] != 0;
    if (ok) strlcpy(out, src, len);
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

static void on_event(void *, esp_event_base_t base, int32_t id, void *data) {
    if (base == IP_EVENT && id == IP_EVENT_AP_STAIPASSIGNED) {
        auto *e = (ip_event_ap_staipassigned_t *)data;
        char ip[16];
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip));
        portENTER_CRITICAL(&s_lock);
        strlcpy(s_phone_ip, ip, sizeof s_phone_ip);
        portEXIT_CRITICAL(&s_lock);
        show_ip();
        ESP_LOGI(TAG, "phone joined, IP %s", ip);
        if (g_app.stage < STAGE_PHONE) app_set_stage(STAGE_PHONE);
        else display_request_status();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_sta_list_t list;
        esp_wifi_ap_get_sta_list(&list);
        s_ap_clients = list.num;
        ESP_LOGI(TAG, "a station left, %d still connected", list.num);
        if (list.num == 0) {
            portENTER_CRITICAL(&s_lock);
            s_phone_ip[0] = 0;
            portEXIT_CRITICAL(&s_lock);
            show_ip();
            if (g_app.stage >= STAGE_PHONE && g_app.stage < STAGE_TCP) app_set_stage(STAGE_AP);
            else display_request_status();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        auto *e = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "station " MACSTR " associated", MAC2STR(e->mac));
        s_ap_clients++;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *e = (ip_event_got_ip_t *)data;
        char ip[16];
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.gw));
        portENTER_CRITICAL(&s_lock);
        strlcpy(s_hotspot_ip, ip, sizeof s_hotspot_ip);
        portEXIT_CRITICAL(&s_lock);
        show_ip();
        ESP_LOGI(TAG, "joined phone hotspot \"%s\", phone at %s", CONFIG_AAM_HOTSPOT_SSID, ip);
        if (g_app.stage < STAGE_PHONE) app_set_stage(STAGE_PHONE);
        else display_request_status();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_sta_connected = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool had = s_hotspot_ip[0] != 0;
        s_sta_connected = false;
        portENTER_CRITICAL(&s_lock);
        s_hotspot_ip[0] = 0;
        portEXIT_CRITICAL(&s_lock);
        if (had) {
            ESP_LOGI(TAG, "left phone hotspot");
            show_ip();
            display_request_status();
        }
    }
}

// Looks for the phone's hotspot every 10 s, but only while nobody is on our access point:
// station scans pause the AP radio and would make the map stutter.
static void hotspot_timer_cb(void *) {
    if (!s_sta_connected && s_ap_clients == 0) esp_wifi_connect();
}

void net_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    // Hand out an address only: no gateway, no DNS. With a gateway the phone sends its
    // internet traffic (and DNS) to the board, which has no internet, and Maps breaks with
    // DNS_PROBE errors. Without one the phone keeps mobile data as its default network and
    // only reaches 192.168.4.x (the board) over WiFi.
    uint8_t offer = 0;
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_ip_info_t ipi;
    esp_netif_get_ip_info(ap_netif, &ipi);
    ipi.gw.addr = 0;  // belt and braces: nothing to offer as a router
    esp_netif_set_ip_info(ap_netif, &ipi);
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &offer, sizeof offer);
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof offer);
    esp_netif_dhcps_start(ap_netif);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));

    wifi_config_t ap = {};
    strlcpy((char *)ap.ap.ssid, CONFIG_AAM_AP_SSID, sizeof ap.ap.ssid);
    ap.ap.ssid_len = strlen(CONFIG_AAM_AP_SSID);
    strlcpy((char *)ap.ap.password, CONFIG_AAM_AP_PASSWORD, sizeof ap.ap.password);
    ap.ap.channel = CONFIG_AAM_AP_CHANNEL;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;
    wifi_config_t sta = {};
    strlcpy((char *)sta.sta.ssid, CONFIG_AAM_HOTSPOT_SSID, sizeof sta.sta.ssid);
    strlcpy((char *)sta.sta.password, CONFIG_AAM_HOTSPOT_PASSWORD, sizeof sta.sta.password);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta.sta.failure_retry_cnt = 1;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "access point \"%s\" up on channel %d; also looking for hotspot \"%s\"", CONFIG_AAM_AP_SSID,
             CONFIG_AAM_AP_CHANNEL, CONFIG_AAM_HOTSPOT_SSID);
    esp_wifi_connect();
    esp_timer_create_args_t ta = {};
    ta.callback = hotspot_timer_cb;
    ta.name = "hotspot";
    esp_timer_handle_t t;
    esp_timer_create(&ta, &t);
    esp_timer_start_periodic(t, 10 * 1000000);
    app_set_stage(STAGE_AP);
}
