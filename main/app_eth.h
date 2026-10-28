#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    APP_ETH_EVENT_GOT_IP = 0,     /* new/first IP via DHCP */
    APP_ETH_EVENT_DISCONNECTED,   /* Ethernet link lost */
} app_eth_event_t;

typedef void (*app_eth_cb_t)(app_eth_event_t event);

/**
 * Initializes esp_netif, event loop and the internal EMAC + IP101GRI PHY of
 * the Unit PoE-P4 and starts the driver (DHCP client runs automatically).
 * cb is called from the event loop task.
 */
esp_err_t app_eth_init(app_eth_cb_t cb);
