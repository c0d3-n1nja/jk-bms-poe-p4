#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "sdkconfig.h"
#include "lwip/inet.h"
#include "app_eth.h"
#include "app_config.h"

static const char *TAG = "app_eth";

#define ETH_CHECK(x, ...) ESP_RETURN_ON_ERROR(x, TAG, __VA_ARGS__)

static app_eth_cb_t s_cb = NULL;
static esp_eth_handle_t s_eth_handle = NULL;

/**
 * Applies the configured static IPv4 settings (config validated at save
 * time). Must run before the link is started: stopping the DHCP client and
 * setting the addresses up front means the netif never announces a DHCP
 * lease. static_dns also becomes the main DNS server in static mode.
 */
static void apply_static_ip(esp_netif_t *netif)
{
    const app_config_t *cfg = app_config_get();
    esp_netif_ip_info_t info;
    esp_ip4_addr_t ip4;
    esp_netif_dns_info_t dns;
    esp_err_t err = esp_netif_dhcpc_stop(netif);

    /* DHCP has not run yet at this point - "already stopped" is expected. */
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "dhcpc_stop failed: %s", esp_err_to_name(err));
    }
    memset(&info, 0, sizeof(info));
    if (esp_netif_str_to_ip4(cfg->static_ip, &ip4) == ESP_OK) {
        info.ip.addr = ip4.addr;
    }
    if (esp_netif_str_to_ip4(cfg->static_netmask, &ip4) == ESP_OK) {
        info.netmask.addr = ip4.addr;
    }
    if (esp_netif_str_to_ip4(cfg->static_gateway, &ip4) == ESP_OK) {
        info.gw.addr = ip4.addr;
    }
    if (esp_netif_set_ip_info(netif, &info) != ESP_OK) {
        /* Saved configs are validated -> this only happens with a corrupted
         * NVS. Keep going without an IP; DHCP stays stopped (no lease). */
        ESP_LOGE(TAG, "static ip_info failed");
    }

    if (esp_netif_str_to_ip4(cfg->static_dns, &ip4) == ESP_OK) {
        memset(&dns, 0, sizeof(dns));
        dns.ip.type = IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = ip4.addr;
        ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));
    }
    ESP_LOGI(TAG, "Static IP " IPSTR ", netmask " IPSTR ", gateway " IPSTR " prepared",
             IP2STR(&info.ip), IP2STR(&info.netmask), IP2STR(&info.gw));
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet started");
            break;
        case ETHERNET_EVENT_CONNECTED: {
            uint8_t mac[6] = { 0 };
            esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac);
            ESP_LOGI(TAG, "Ethernet link up, HW addr " MACSTR, MAC2STR(mac));
            break;
        }
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet link down");
            if (s_cb) {
                s_cb(APP_ETH_EVENT_DISCONNECTED);
            }
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet stopped");
            break;
        default:
            break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const app_config_t *cfg = app_config_get();
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "%s: IP " IPSTR ", netmask " IPSTR ", gateway " IPSTR,
             cfg->use_dhcp ? "DHCP" : "Static IP",
             IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask), IP2STR(&ev->ip_info.gw));
    if (cfg->use_dhcp && cfg->static_dns[0] != '\0') {
        /* DHCP module: optional secondary DNS (fills the gap if the lease
         * has no or a useless resolver). */
        esp_ip4_addr_t ip4;
        esp_netif_dns_info_t dns;
        esp_netif_t *netif = ev->esp_netif ? ev->esp_netif
                                           : esp_netif_get_handle_from_ifkey("ETH_DEF");

        if (esp_netif_str_to_ip4(cfg->static_dns, &ip4) == ESP_OK) {
            memset(&dns, 0, sizeof(dns));
            dns.ip.type = IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = ip4.addr;
            if (esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK) {
                ESP_LOGI(TAG, "Fallback DNS " IPSTR " registered", IP2STR(&ip4));
            }
        }
    }
    if (s_cb) {
        s_cb(APP_ETH_EVENT_GOT_IP);
    }
}

esp_err_t app_eth_init(app_eth_cb_t cb)
{
    s_cb = cb;

    /* TCP/IP stack + system event loop */
    ETH_CHECK(esp_netif_init(), "esp_netif_init failed");
    ETH_CHECK(esp_event_loop_create_default(), "event loop failed");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);   /* default name "eth", DHCP client */

    /* Internal EMAC (RMII) + IP101GRI PHY, pinmap of the Unit PoE-P4 */
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = CONFIG_JKBMS_ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = CONFIG_JKBMS_ETH_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num = CONFIG_JKBMS_ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = CONFIG_JKBMS_ETH_MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);

    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ETH_CHECK(esp_eth_driver_install(&eth_cfg, &s_eth_handle), "eth driver install failed");

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    ETH_CHECK(esp_netif_attach(netif, glue), "esp_netif_attach failed");

    /* Static mode: stop the DHCP client and pin the addresses BEFORE the
     * link starts. Defaults (DHCP) need no preparation. */
    if (!app_config_get()->use_dhcp) {
        apply_static_ip(netif);
    }

    ETH_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event, NULL), "eth handler failed");
    ETH_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_got_ip, NULL), "ip handler failed");

    ETH_CHECK(esp_eth_start(s_eth_handle), "esp_eth_start failed");
    return ESP_OK;
}
