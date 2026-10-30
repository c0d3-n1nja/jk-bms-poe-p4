#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    char ip[16];
    char netmask[16];
    char gateway[16];
    char dns[16];         /* main DNS (DHCP-leased or the static one) */
    char dns_backup[16];  /* optional user fallback DNS (DHCP mode only) */
    bool have_ip;             /* an address is (being) used at all */
    bool use_dhcp;            /* configured mode */
} app_eth_netinfo_t;

typedef enum {
    APP_ETH_EVENT_GOT_IP = 0,     /* new/first IP via DHCP or static */
    APP_ETH_EVENT_DISCONNECTED,   /* Ethernet link lost */
} app_eth_event_t;

typedef void (*app_eth_cb_t)(app_eth_event_t event);

/**
 * Initializes esp_netif, event loop and the internal EMAC + IP101GRI PHY of
 * the Unit PoE-P4 and starts the driver (DHCP client runs automatically).
 * cb is called from the event loop task.
 */
esp_err_t app_eth_init(app_eth_cb_t cb);

/**
 * Effective IP/DNS settings (DHCP-leased parameters or the applied static
 * ones). All strings are dotted-quad or "" if the value does not apply /
 * is not assigned yet. Returns a pointer into static storage; the cache is
 * refreshed on every GOT_IP event (static IPs raise it with the link-up
 * action of esp_netif, leased ones via the DHCP client).
 */
const app_eth_netinfo_t *app_eth_get_netinfo(void);
