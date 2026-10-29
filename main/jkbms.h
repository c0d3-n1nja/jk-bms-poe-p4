#pragma once

/*
 * JK-BMS decoder for the JK-PB series ("JK BMS RS485 Modbus V1.0", protocol 001
 * on the RS485-1 / UART1 RJ45 port, also valid on RS485-2 in slave mode).
 *
 * - 115200 8N1, standard Modbus RTU framing, CRC16-MODBUS (poly 0xA001)
 * - Function 0x03 (read holding registers); writes are not used
 * - Register map (byte-address blocks): configuration @0x1000,
 *   cell data @0x1200, live status @0x128A, identity @0x1400
 *
 * Multi-byte values are big-endian (Modbus standard: high word first,
 * high byte first). Temperatures are 0.1 deg C signed, voltages mV,
 * currents mA, power mW.
 */

/*
 * JK addressing: the FC 0x03 start address is a BYTE address in the BMS data
 * area (not a 16-bit register index); a request returns 2 * quantity bytes
 * starting at that byte address ("The register addresses of the JK BMS are
 * byte offsets ... The response of the BMS contains the values without any
 * gaps", see syssi/esphome-jk-bms esp32-jk-pb-modbus-example.yaml). The
 * *_START values below therefore match the offsets of the official register
 * map one-to-one, and payload offsets inside the decoders are byte offsets
 * relative to the block start.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Register regions (byte addresses). There are no address limits: earlier
 * "silent boundaries" were an artifact of a missing bus-quiet gap between
 * consecutive reads (the BMS drops requests that arrive too soon after its
 * own response). Regions are read in chunks of up to 37 words. */
#define JKBM_REG_CELL_START      0x1200u
#define JKBM_REG_CELL_WORDS      37u      /* 74 bytes: 0x1200..0x1249 */
#define JKBM_REG_CELL_BYTES      (JKBM_REG_CELL_WORDS * 2u)

#define JKBM_REGION_CONFIG_START 0x1000u
#define JKBM_REGION_CONFIG_BYTES 136u     /* 0x1000..0x1087 protections + switches */
#define JKBM_REGION_STATUS_START 0x128Au
#define JKBM_REGION_STATUS_BYTES 116u     /* 0x128A..0x12FD power, temps, SOC */
#define JKBM_REGION_IDENT_START  0x1400u
#define JKBM_REGION_IDENT_BYTES  32u      /* 0x1400..0x141F model, hw/sw version */
#define JKBM_REG_PROTO_START     0x14B2u  /* UART1/CAN protocol number (2 bytes) */

#define JKBM_MAX_CELLS 24

typedef struct {
    /* Cell data */
    uint8_t  cell_count;
    float    cell_voltage[JKBM_MAX_CELLS];   /* V */
    float    min_cell_voltage;               /* V */
    float    max_cell_voltage;               /* V */
    float    delta_cell_voltage;             /* V */
    float    average_cell_voltage;           /* V */
    uint8_t  min_voltage_cell;               /* 1-based cell number */
    uint8_t  max_voltage_cell;               /* 1-based cell number */

    /* Temperatures */
    float    mosfet_temperature;             /* deg C */
    float    temperature_1;                  /* deg C (TempBat1) */
    float    temperature_2;                  /* deg C (TempBat2) */
    float    temperature_3;                  /* deg C (TempBat3) */
    float    temperature_4;                  /* deg C (TempBat4) */
    float    temperature_5;                  /* deg C (TempBat5) */
    uint8_t  temperature_sensor_count;

    /* Electrical */
    float    total_voltage;                  /* V */
    float    current;                        /* A, sign as reported by the BMS
                                                (orientation to be verified on
                                                first live data) */
    float    total_power;                    /* W */
    float    charge_power;                   /* W (>= 0) */
    float    discharge_power;                /* W (>= 0) */

    /* Status */
    uint8_t  state_of_charge;                /* % */
    uint8_t  state_of_health;                /* % */
    uint8_t  balance_state;                  /* 0 = off, 1 = charge, 2 = discharge */
    float    balancing_current;              /* A */
    float    capacity_remaining;             /* Ah */
    float    full_charge_capacity;           /* Ah */
    float    actual_battery_capacity;        /* Ah */
    uint16_t charging_cycles;
    uint32_t total_charging_cycle_capacity;  /* Ah */
    uint16_t battery_strings;

    /* Bitmasks */
    uint32_t errors_bitmask;                 /* alarm bits 0..23, see below */
    uint16_t operation_mode_bitmask;         /* bit0 charging, bit1 discharging,
                                                bit2 balancer, bit3 precharge */

    /* Switch states (user settings, config block) */
    bool     balancing_switch_on;
    bool     charging_switch_on;
    bool     discharging_switch_on;

    /* Protection/balance configuration parameters */
    float    total_voltage_ovp;              /* V (not in PB map, always 0) */
    float    total_voltage_uvp;              /* V (not in PB map, always 0) */
    float    cell_ovp;                       /* V */
    float    cell_ovp_recovery;              /* V */
    float    cell_ovp_delay;                 /* s (not in PB map, always 0) */
    float    cell_uvp;                       /* V */
    float    cell_uvp_recovery;              /* V */
    float    cell_uvp_delay;                 /* s (not in PB map, always 0) */
    float    cell_diff_protection;           /* V (not in PB map, always 0) */
    float    discharging_ocp;                /* A */
    float    discharging_ocp_delay;          /* s */
    float    charging_ocp;                   /* A */
    float    charging_ocp_delay;             /* s */
    float    balance_start_voltage;          /* V */
    float    balance_delta_voltage;          /* V */
    float    mosfet_ot_protect;              /* deg C */
    float    mosfet_ot_recover;              /* deg C */
    float    battery_ot_protect;             /* deg C */
    float    battery_ot_recover;             /* deg C */
    float    battery_temp_diff_protect;      /* deg C (not in PB map, always 0) */
    float    charge_ot_protect;              /* deg C */
    float    discharge_ot_protect;           /* deg C (same source as battery OT) */
    float    charge_ut_protect;              /* deg C */
    float    charge_ut_recover;              /* deg C */
    float    discharge_ut_protect;           /* deg C (not in PB map, always 0) */
    float    discharge_ut_recover;           /* deg C (not in PB map, always 0) */

    /* Miscellaneous */
    float    current_calibration;            /* A (not in PB map, always 0) */
    float    total_runtime_hours;
    uint8_t  low_soc_alarm;                  /* % (not in PB map, always 0) */
    uint8_t  protocol_version;               /* UART1 protocol number */
    char     battery_type[24];
    char     device_type[12];                /* manufacturer device ID */
    char     hardware_version[12];
    char     software_version[24];
    char     manufacturer[28];
} jkbms_data_t;

/**
 * Modbus RTU CRC16 (poly 0xA001, init 0xFFFF), wire order: low byte first.
 */
uint16_t jkbms_crc16(const uint8_t *buf, uint16_t len);

/**
 * Assembles a Modbus RTU read request (function 0x03) into out (8 bytes).
 */
uint16_t jkbms_build_read_request(uint8_t addr, uint16_t start_reg, uint16_t count, uint8_t *out);

/**
 * Validates a Modbus RTU read response (addr, function 0x03, byte count and
 * CRC16) for the given register count. On success, *data points to the
 * register payload (2 * count bytes, big-endian).
 */
bool jkbms_parse_response(const uint8_t *raw, uint16_t len, uint8_t addr, uint16_t count,
                          const uint8_t **data);

/**
 * Block decoders for arbitrary windows (base = byte address of the window,
 * data/len = payload). Each call fills only the fields that lie completely
 * inside the window; repeated calls for consecutive chunks accumulate into
 * the same jkbms_data_t (mode bits are OR-ed in).
 */
bool jkbms_decode_cell_block(const uint8_t *data, uint16_t byte_count, jkbms_data_t *out);
bool jkbms_decode_status_chunk(uint16_t base, const uint8_t *data, uint16_t len, jkbms_data_t *out);
bool jkbms_decode_config_chunk(uint16_t base, const uint8_t *data, uint16_t len, jkbms_data_t *out);
bool jkbms_decode_identity_chunk(uint16_t base, const uint8_t *data, uint16_t len, jkbms_data_t *out);

/**
 * Derived/fallback values after a complete poll cycle (sensor count fallback,
 * manufacturer default).
 */
void jkbms_finalize(jkbms_data_t *out);

void jkbms_errors_to_text(uint32_t bitmask, char *buf, size_t buf_len);
void jkbms_modes_to_text(uint16_t bitmask, char *buf, size_t buf_len);
