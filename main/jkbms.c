#include <string.h>
#include <stdio.h>
#include "jkbms.h"

/* ------------------------------------------------------------------ */
/* Modbus RTU framing                                                   */
/* ------------------------------------------------------------------ */

uint16_t jkbms_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    uint16_t pos;
    int i;

    for (pos = 0; pos < len; pos++) {
        crc ^= (uint16_t)buf[pos];
        for (i = 0; i < 8; i++) {
            if ((crc & 0x0001u) != 0u) {
                crc >>= 1;
                crc ^= 0xA001u;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

uint16_t jkbms_build_read_request(uint8_t addr, uint16_t start_reg, uint16_t count, uint8_t *out)
{
    uint16_t crc;

    out[0] = addr;
    out[1] = 0x03;                       /* function: read holding registers */
    out[2] = (uint8_t)(start_reg >> 8);
    out[3] = (uint8_t)(start_reg & 0xFFu);
    out[4] = (uint8_t)(count >> 8);
    out[5] = (uint8_t)(count & 0xFFu);
    crc = jkbms_crc16(out, 6);
    out[6] = (uint8_t)(crc & 0xFFu);     /* wire order: low byte first */
    out[7] = (uint8_t)(crc >> 8);
    return 8;
}

bool jkbms_parse_response(const uint8_t *raw, uint16_t len, uint8_t addr, uint16_t count,
                          const uint8_t **data)
{
    uint16_t need, crc, remote;

    if (raw == NULL || count == 0u || count > 0x7Du) {
        return false;
    }
    need = (uint16_t)(5u + 2u * count);  /* addr + fc + byte count + data + crc16 */
    if (len < need) {
        return false;                    /* response not yet complete */
    }
    if (raw[0] != addr || raw[1] != 0x03u) {
        return false;
    }
    if (raw[2] != (uint8_t)(2u * count)) {
        return false;
    }
    crc = jkbms_crc16(raw, (uint16_t)(need - 2u));
    remote = (uint16_t)((uint16_t)raw[need - 2u] | ((uint16_t)raw[need - 1u] << 8));
    if (crc != remote) {
        return false;                    /* CRC error */
    }
    if (data != NULL) {
        *data = raw + 3;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Big-endian accessors (Modbus standard order)                         */
/* ------------------------------------------------------------------ */

static inline uint16_t rd16(const uint8_t *d)
{
    return (uint16_t)(((uint16_t)d[0] << 8) | d[1]);
}

static inline uint32_t rd32(const uint8_t *d)
{
    return ((uint32_t)rd16(d) << 16) | rd16(d + 2);
}

static inline int16_t rds16(const uint8_t *d)
{
    return (int16_t)rd16(d);
}

static inline int32_t rds32(const uint8_t *d)
{
    return (int32_t)rd32(d);
}

static inline int popcount32(uint32_t v)
{
    int n = 0;
    while (v != 0u) {
        v &= (v - 1u);
        n++;
    }
    return n;
}

/* Copies an ASCII field, skips leading NULs (the firmware right-pads e.g.
 * the software version: 00 "15.38" 00 00), then stops at the next NUL and
 * filters non-printable bytes. */
static void copy_ascii(char *dst, size_t dst_size, const uint8_t *src, size_t src_len)
{
    size_t n = 0, i = 0;

    while (i < src_len && src[i] == 0u) {
        i++;
    }
    for (; i < src_len && src[i] != 0u; i++) {
        char c = (char)src[i];
        if (c < 0x20 || c > 0x7E) {
            continue;
        }
        if (n + 1 >= dst_size) {
            break;
        }
        dst[n++] = c;
    }
    dst[n] = '\0';
}

/* ------------------------------------------------------------------ */
/* Cell block @0x1200 (payload = bytes 0x1200..0x1249)                  */
/* ------------------------------------------------------------------ */

bool jkbms_decode_cell_block(const uint8_t *data, uint16_t byte_count, jkbms_data_t *o)
{
    int cells, i;
    float mn, mx;

    if (data == NULL || o == NULL || byte_count < JKBM_REG_CELL_BYTES) {
        return false;
    }

    /* Byte address 0x1240 (payload 64): CellSta bitmask, BIT[n] = cell n present */
    cells = popcount32(rd32(data + 64));
    if (cells == 0) {
        /* fallback: assume the first 16 transmitted cell voltages are valid */
        cells = 16;
    }
    if (cells > JKBM_MAX_CELLS) {
        cells = JKBM_MAX_CELLS;
    }
    o->cell_count = (uint8_t)cells;
    o->battery_strings = o->cell_count;

    mn = 1000000.0f;
    mx = -1000000.0f;
    for (i = 0; i < cells; i++) {
        float v = (float)rd16(data + (size_t)i * 2u) * 0.001f;
        o->cell_voltage[i] = v;
        if (v < mn) { mn = v; o->min_voltage_cell = (uint8_t)(i + 1); }
        if (v > mx) { mx = v; o->max_voltage_cell = (uint8_t)(i + 1); }
    }
    o->min_cell_voltage = mn;
    o->max_cell_voltage = mx;

    /* Byte address 0x1244/0x1246 (payload 68/70): CellVolAve / CellVdifMax (mV) */
    o->average_cell_voltage = (float)rd16(data + 68) * 0.001f;
    o->delta_cell_voltage   = (float)rd16(data + 70) * 0.001f;
    return true;
}

/* ------------------------------------------------------------------ */
/* Status region @0x128A..0x12FD - chunk decoder (arbitrary window)     */
/* ------------------------------------------------------------------ */

bool jkbms_decode_status_chunk(uint16_t base, const uint8_t *data, uint16_t len, jkbms_data_t *o)
{
    uint32_t end = (uint32_t)base + len;

    if (data == NULL || o == NULL || len == 0u) {
        return false;
    }

#define ST_WIN(ADDR, SIZE) ((uint32_t)(ADDR) >= base && (uint32_t)(ADDR) + (SIZE) <= end)
#define ST_PTR(ADDR) (data + ((ADDR) - base))

    if (ST_WIN(0x128A, 2)) {
        o->mosfet_temperature = (float)rds16(ST_PTR(0x128A)) * 0.1f;   /* TempMos */
    }
    if (ST_WIN(0x1290, 4)) {
        o->total_voltage = (float)rd32(ST_PTR(0x1290)) * 0.001f;       /* BatVol, mV */
    }
    if (ST_WIN(0x1298, 4)) {
        o->current = (float)rds32(ST_PTR(0x1298)) * 0.001f;            /* BatCurrent, mA */
    }
    if (ST_WIN(0x1294, 4)) {
        o->total_power = (float)rd32(ST_PTR(0x1294)) * 0.001f;         /* BatWatt, mW */
        o->charge_power    = o->current > 0.0f ? o->total_power : 0.0f;
        o->discharge_power = o->current < 0.0f ? o->total_power : 0.0f;
    }
    if (ST_WIN(0x129C, 2)) {
        o->temperature_1 = (float)rds16(ST_PTR(0x129C)) * 0.1f;        /* TempBat1 */
    }
    if (ST_WIN(0x129E, 2)) {
        o->temperature_2 = (float)rds16(ST_PTR(0x129E)) * 0.1f;        /* TempBat2 */
    }
    if (ST_WIN(0x12A0, 4)) {
        o->errors_bitmask = rd32(ST_PTR(0x12A0));                      /* Alarm */
    }
    if (ST_WIN(0x12A4, 2)) {
        o->balancing_current = (float)rds16(ST_PTR(0x12A4)) * 0.001f;  /* BalanCurrent, mA */
    }
    if (ST_WIN(0x12A6, 2)) {
        uint8_t balan_sta = ST_PTR(0x12A6)[0];                         /* hi: BalanSta */
        o->balance_state = balan_sta > 2u ? 0u : balan_sta;
        o->state_of_charge = ST_PTR(0x12A6)[1];                        /* lo: SOC */
        if (balan_sta != 0u) {
            o->operation_mode_bitmask |= (uint16_t)(1u << 2);
        }
    }
    if (ST_WIN(0x12A8, 4)) {
        o->capacity_remaining = (float)rds32(ST_PTR(0x12A8)) * 0.001f; /* SOCCapRemain, mAh */
    }
    if (ST_WIN(0x12AC, 4)) {
        o->full_charge_capacity = (float)rd32(ST_PTR(0x12AC)) * 0.001f;
        o->actual_battery_capacity = o->full_charge_capacity;
    }
    if (ST_WIN(0x12B0, 4)) {
        uint32_t cycle_count = rd32(ST_PTR(0x12B0));                   /* SOCCycleCount */
        o->charging_cycles = cycle_count > 0xFFFFu ? 0xFFFFu : (uint16_t)cycle_count;
    }
    if (ST_WIN(0x12B4, 4)) {
        o->total_charging_cycle_capacity = rd32(ST_PTR(0x12B4)) / 1000u;
    }
    if (ST_WIN(0x12B8, 2)) {
        o->state_of_health = ST_PTR(0x12B8)[0];                        /* hi: SOH */
        if (ST_PTR(0x12B8)[1] != 0u) {                                 /* lo: Precharge */
            o->operation_mode_bitmask |= (uint16_t)(1u << 3);
        }
    }
    if (ST_WIN(0x12BC, 4)) {
        o->total_runtime_hours = (float)rd32(ST_PTR(0x12BC)) / 3600.0f;
    }
    if (ST_WIN(0x12C0, 2)) {
        if (ST_PTR(0x12C0)[0] != 0u) {                                 /* hi: Charge */
            o->operation_mode_bitmask |= (uint16_t)(1u << 0);
        }
        if (ST_PTR(0x12C0)[1] != 0u) {                                 /* lo: Discharge */
            o->operation_mode_bitmask |= (uint16_t)(1u << 1);
        }
    }
    if (ST_WIN(0x12D0, 2)) {
        /* BIT0 = MOS, BIT1..5 = battery sensor 1..5 ("1: Normal; 0: Missing") */
        int sensor_bits = (int)((ST_PTR(0x12D0)[0] >> 1) & 0x1Fu);
        if (sensor_bits != 0) {
            o->temperature_sensor_count = (uint8_t)popcount32((uint32_t)sensor_bits);
        }
    }
    if (ST_WIN(0x12F8, 2)) {
        o->temperature_3 = (float)rds16(ST_PTR(0x12F8)) * 0.1f;        /* TempBat3 */
    }
    if (ST_WIN(0x12FA, 2)) {
        o->temperature_4 = (float)rds16(ST_PTR(0x12FA)) * 0.1f;        /* TempBat4 */
    }
    if (ST_WIN(0x12FC, 2)) {
        o->temperature_5 = (float)rds16(ST_PTR(0x12FC)) * 0.1f;        /* TempBat5 */
    }

#undef ST_WIN
#undef ST_PTR
    return true;
}

/* ------------------------------------------------------------------ */
/* Config region @0x1000..0x1087 - chunk decoder (arbitrary window)     */
/* ------------------------------------------------------------------ */

bool jkbms_decode_config_chunk(uint16_t base, const uint8_t *data, uint16_t len, jkbms_data_t *o)
{
    uint32_t end = (uint32_t)base + len;

    if (data == NULL || o == NULL || len == 0u) {
        return false;
    }

#define CFG_WIN(ADDR, SIZE) ((uint32_t)(ADDR) >= base && (uint32_t)(ADDR) + (SIZE) <= end)
#define CFG_PTR(ADDR) (data + ((ADDR) - base))

    if (CFG_WIN(0x1004, 4)) o->cell_uvp              = (float)rd32(CFG_PTR(0x1004)) * 0.001f;
    if (CFG_WIN(0x1008, 4)) o->cell_uvp_recovery     = (float)rd32(CFG_PTR(0x1008)) * 0.001f;
    if (CFG_WIN(0x100C, 4)) o->cell_ovp              = (float)rd32(CFG_PTR(0x100C)) * 0.001f;
    if (CFG_WIN(0x1010, 4)) o->cell_ovp_recovery     = (float)rd32(CFG_PTR(0x1010)) * 0.001f;
    if (CFG_WIN(0x1014, 4)) o->balance_delta_voltage = (float)rd32(CFG_PTR(0x1014)) * 0.001f;

    if (CFG_WIN(0x102C, 4)) o->charging_ocp          = (float)rd32(CFG_PTR(0x102C)) * 0.001f;
    if (CFG_WIN(0x1030, 4)) o->charging_ocp_delay    = (float)rd32(CFG_PTR(0x1030));
    if (CFG_WIN(0x1038, 4)) o->discharging_ocp       = (float)rd32(CFG_PTR(0x1038)) * 0.001f;
    if (CFG_WIN(0x103C, 4)) o->discharging_ocp_delay = (float)rd32(CFG_PTR(0x103C));

    if (CFG_WIN(0x104C, 4)) o->charge_ot_protect     = (float)rds32(CFG_PTR(0x104C)) * 0.1f;
    if (CFG_WIN(0x1054, 4)) o->battery_ot_protect    = (float)rds32(CFG_PTR(0x1054)) * 0.1f;
    if (CFG_WIN(0x1058, 4)) o->battery_ot_recover    = (float)rds32(CFG_PTR(0x1058)) * 0.1f;
    if (CFG_WIN(0x1054, 4)) o->discharge_ot_protect  = o->battery_ot_protect;
    if (CFG_WIN(0x105C, 4)) o->charge_ut_protect     = (float)rds32(CFG_PTR(0x105C)) * 0.1f;
    if (CFG_WIN(0x1060, 4)) o->charge_ut_recover     = (float)rds32(CFG_PTR(0x1060)) * 0.1f;
    if (CFG_WIN(0x1064, 4)) o->mosfet_ot_protect     = (float)rds32(CFG_PTR(0x1064)) * 0.1f;
    if (CFG_WIN(0x1068, 4)) o->mosfet_ot_recover     = (float)rds32(CFG_PTR(0x1068)) * 0.1f;

    if (CFG_WIN(0x1070, 4)) o->charging_switch_on    = rd32(CFG_PTR(0x1070)) != 0u;
    if (CFG_WIN(0x1074, 4)) o->discharging_switch_on = rd32(CFG_PTR(0x1074)) != 0u;
    if (CFG_WIN(0x1078, 4)) o->balancing_switch_on   = rd32(CFG_PTR(0x1078)) != 0u;
    if (CFG_WIN(0x1084, 4)) o->balance_start_voltage = (float)rd32(CFG_PTR(0x1084)) * 0.001f;

#undef CFG_WIN
#undef CFG_PTR
    return true;
}

/* ------------------------------------------------------------------ */
/* Identity region @0x1400..0x141F - chunk decoder (arbitrary window)   */
/* ------------------------------------------------------------------ */

bool jkbms_decode_identity_chunk(uint16_t base, const uint8_t *data, uint16_t len, jkbms_data_t *o)
{
    uint32_t end = (uint32_t)base + len;

    if (data == NULL || o == NULL || len == 0u) {
        return false;
    }

    if (base <= 0x1400u && 0x1400u + 16u <= end) {
        copy_ascii(o->device_type, sizeof(o->device_type), data + (0x1400u - base), 16);
    }
    if (base <= 0x1410u && 0x1410u + 8u <= end) {
        copy_ascii(o->hardware_version, sizeof(o->hardware_version), data + (0x1410u - base), 8);
    }
    if (base <= 0x1418u && 0x1418u + 8u <= end) {
        copy_ascii(o->software_version, sizeof(o->software_version), data + (0x1418u - base), 8);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Derived/fallback values after a complete poll cycle                  */
/* ------------------------------------------------------------------ */

void jkbms_finalize(jkbms_data_t *o)
{
    if (o == NULL) {
        return;
    }
    if (o->manufacturer[0] == '\0') {
        strncpy(o->manufacturer, "Jikong", sizeof(o->manufacturer) - 1);
        o->manufacturer[sizeof(o->manufacturer) - 1] = '\0';
    }
    if (o->battery_type[0] == '\0') {
        strncpy(o->battery_type, "Unknown", sizeof(o->battery_type) - 1);
        o->battery_type[sizeof(o->battery_type) - 1] = '\0';
    }
    if (o->temperature_sensor_count == 0) {
        /* fallback: count non-zero battery temperatures (0 deg C is treated
           as "missing"; a real 0.0 deg C reading is rare) */
        if (o->temperature_1 != 0.0f) o->temperature_sensor_count++;
        if (o->temperature_2 != 0.0f) o->temperature_sensor_count++;
        if (o->temperature_3 != 0.0f) o->temperature_sensor_count++;
        if (o->temperature_4 != 0.0f) o->temperature_sensor_count++;
        if (o->temperature_5 != 0.0f) o->temperature_sensor_count++;
    }
}

/* ------------------------------------------------------------------ */
/* Alarm/mode bits as text                                              */
/* ------------------------------------------------------------------ */

void jkbms_errors_to_text(uint32_t bitmask, char *buf, size_t buf_len)
{
    static const char *const labels[24] = {
        "Balance wire resistance",      /* BIT0  AlarmWireRes        */
        "MOS overtemperature",          /* BIT1  AlarmMosOTP         */
        "Cell count mismatch",          /* BIT2  AlarmCellQuantity   */
        "Current sensor fault",         /* BIT3  AlarmCurSensorErr   */
        "Cell overvoltage",             /* BIT4  AlarmCellOVP        */
        "Battery overvoltage",          /* BIT5  AlarmBatOVP         */
        "Charge overcurrent",           /* BIT6  AlarmChOCP          */
        "Charge short circuit",         /* BIT7  AlarmChSCP          */
        "Charge overtemperature",       /* BIT8  AlarmChOTP          */
        "Charge undertemperature",      /* BIT9  AlarmChUTP          */
        "Internal communication fault", /* BIT10 AlarmCPUAuxCommuErr */
        "Cell undervoltage",            /* BIT11 AlarmCellUVP        */
        "Battery undervoltage",         /* BIT12 AlarmBatUVP         */
        "Discharge overcurrent",        /* BIT13 AlarmDchOCP         */
        "Discharge short circuit",      /* BIT14 AlarmDchSCP         */
        "Discharge overtemperature",    /* BIT15 AlarmDchOTP         */
        "Charge MOS fault",             /* BIT16 AlarmChargeMOS      */
        "Discharge MOS fault",          /* BIT17 AlarmDischargeMOS   */
        "GPS disconnected",             /* BIT18 GPSDisconneted      */
        "Password warning",             /* BIT19 ModifyPWDinTime     */
        "Discharge start failure",      /* BIT20 DischargeOnFailed   */
        "Battery overtemperature",      /* BIT21 BatteryOverTempAlarm*/
        "Temperature sensor fault",     /* BIT22                     */
        "Parallel module fault",        /* BIT23                     */
    };
    size_t used = 0;
    int i;

    buf[0] = '\0';
    for (i = 0; i < 24; i++) {
        if ((bitmask & (1UL << i)) == 0u) {
            continue;
        }
        if (labels[i][0] == '\0') {
            continue;
        }
        if (used > 0 && used + 1 < buf_len) {
            buf[used++] = ';';
        }
        {
            size_t l = strlen(labels[i]);
            if (used + l + 1 > buf_len) {
                l = buf_len - used - 1;
            }
            memcpy(buf + used, labels[i], l);
            used += l;
            buf[used] = '\0';
        }
    }
}

void jkbms_modes_to_text(uint16_t bitmask, char *buf, size_t buf_len)
{
    static const char *const labels[4] = {
        "Charging enabled",
        "Discharging enabled",
        "Balancer active",
        "Precharge active",
    };
    size_t used = 0;
    int i;

    buf[0] = '\0';
    if (bitmask == 0) {
        return;
    }
    for (i = 0; i < 4; i++) {
        if ((bitmask & (1 << i)) == 0) {
            continue;
        }
        if (used > 0 && used + 1 < buf_len) {
            buf[used++] = ';';
        }
        {
            size_t l = strlen(labels[i]);
            if (used + l + 1 > buf_len) {
                l = buf_len - used - 1;
            }
            memcpy(buf + used, labels[i], l);
            used += l;
            buf[used] = '\0';
        }
    }
}
