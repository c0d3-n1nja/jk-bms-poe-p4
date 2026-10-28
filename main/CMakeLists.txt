idf_component_register(
    SRCS "main.c" "jkbms.c" "app_eth.c" "app_mqtt.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "ca_cert.pem"
)
