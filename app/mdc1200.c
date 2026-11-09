#include "driver/bk4819.h"
#include "app/app.h"
#include "driver/crc.h"
#include "driver/uart.h"
#include "mdc1200.h"
#include "misc.h"
#include <string.h>
#include "driver/eeprom.h"
#include "settings.h"

// Global variables required by the firmware and UI
uint8_t      mdc1200_op;
uint8_t      mdc1200_arg;
uint16_t     mdc1200_unit_id;
uint16_t     mdc1200_fleet_id;
uint8_t      mdc1200_rx_ready_tick_500ms;
uint8_t      mdc1200_rx_buffer[5 + (MDC1200_FEC_K * 2)];
unsigned int mdc1200_rx_buffer_index = 0;

// Keep all original constants
const uint8_t mdc1200_pre_amble[] = {0x00, 0xFF};
const uint8_t mdc1200_sync[5] = {0x07, 0x09, 0x2a, 0x44, 0x6f};
const uint8_t mdc1200_sync_suc_xor[5] = {0xfb, 0x72, 0x40, 0x99, 0xa7};

/** 
 * Keep your original Error Correction
 */
void error_correction(void *data) {
    int i;
    uint8_t shift_reg;
    uint8_t syn;
    uint8_t *data8 = (uint8_t *) data;

    for (i = 0, shift_reg = 0, syn = 0; i < MDC1200_FEC_K; i++) {
        const uint8_t bi = data8[i];
        int bit_num;
        for (bit_num = 0; bit_num < 8; bit_num++) {
            uint8_t b;
            unsigned int k = 0;

            shift_reg = (shift_reg << 1) | ((bi >> bit_num) & 1u);
            b = ((shift_reg >> 6) ^ (shift_reg >> 5) ^ (shift_reg >> 2) ^ (shift_reg >> 0)) & 1u;
            syn = (syn << 1) | (((b ^ (data8[i + MDC1200_FEC_K] >> bit_num)) & 1u) ? 1u : 0u);

            if (syn & 0x80) k++;
            if (syn & 0x20) k++;
            if (syn & 0x04) k++;
            if (syn & 0x02) k++;

            if (k >= 3) { 
                int ii = i;
                int bn = bit_num - 7;
                if (bn < 0) {
                    bn += 8;
                    ii--;
                }
                if (ii >= 0)
                    data8[ii] ^= 1u << bn;
                syn ^= 0xA6;
            }
        }
    }
}

/** 
 * Keep your original De-interleaving logic
 */
bool decode_data(void *data) {
    uint16_t crc1;
    uint16_t crc2;
    uint8_t *data8 = (uint8_t *) data;
    uint8_t encoded[MDC1200_FEC_K * 2];

    {
        unsigned int i, k, m;
        uint8_t deinterleaved[(MDC1200_FEC_K * 2) * 8];

        for (i = 0, k = 0; i < 16; i++) {
            for (m = 0; m < MDC1200_FEC_K; m++) {
                const unsigned int n = (m * 16) + i;
                deinterleaved[k++] = (data8[n >> 3] >> ((7 - n) & 7u)) & 1u;
            }
        }

        for (i = 0, m = 0; i < (MDC1200_FEC_K * 2); i++) {
            unsigned int k;
            uint8_t b = 0;
            for (k = 0; k < 8; k++)
                if (deinterleaved[m++])
                    b |= 1u << k;
            data8[i] = b;
        }
    }

    memcpy(encoded, data8, sizeof(encoded));
    error_correction(data);

    crc1 = compute_crc(data, 4);
    crc2 = ((uint16_t) data8[5] << 8) | (data8[4] << 0);

    if (crc1 == crc2)
        return true;

    // Retry one coded-bit correction for residual errors left by the greedy FEC pass.
    for (unsigned int bit = 0; bit < sizeof(encoded) * 8; bit++) {
        memcpy(data8, encoded, sizeof(encoded));
        data8[bit >> 3] ^= 1u << (bit & 7u);
        error_correction(data);

        crc1 = compute_crc(data, 4);
        crc2 = ((uint16_t) data8[5] << 8) | (data8[4] << 0);
        if (crc1 == crc2)
            return true;
    }

    memcpy(data8, encoded, sizeof(encoded));
    return false;
}

// *** TX FUNCTIONS (KEEP VERBATIM) ***

void xor_modulation(void *data, const unsigned int size) {
    unsigned int i;
    uint8_t *data8 = (uint8_t *) data;
    uint8_t prev_bit = 0;
    for (i = 0; i < size; i++) {
        int bit_num;
        uint8_t in = data8[i];
        uint8_t out = 0;
        for (bit_num = 7; bit_num >= 0; bit_num--) {
            const uint8_t new_bit = (in >> bit_num) & 1u;
            if (new_bit != prev_bit)
                out |= 1u << bit_num;
            prev_bit = new_bit;
        }
        data8[i] = out ^ 0xff;
    }
}

uint8_t *encode_data(void *data) {
    uint8_t *data8 = (uint8_t *) data;
    {
        unsigned int i;
        uint8_t shift_reg = 0;
        for (i = 0; i < MDC1200_FEC_K; i++) {
            unsigned int bit_num;
            const uint8_t bi = data8[i];
            uint8_t bo = 0;
            for (bit_num = 0; bit_num < 8; bit_num++) {
                shift_reg = (shift_reg << 1) | ((bi >> bit_num) & 1u);
                bo |= (((shift_reg >> 6) ^ (shift_reg >> 5) ^ (shift_reg >> 2) ^ (shift_reg >> 0)) & 1u) << bit_num;
            }
            data8[MDC1200_FEC_K + i] = bo;
        }
    }

    {
        unsigned int i, k;
        uint8_t interleaved[(MDC1200_FEC_K * 2) * 8];
        for (i = 0, k = 0; i < (MDC1200_FEC_K * 2); i++) {
            unsigned int bit_num;
            const uint8_t b = data8[i];
            for (bit_num = 0; bit_num < 8; bit_num++) {
                interleaved[k] = (b >> bit_num) & 1u;
                k += 16;
                if (k >= sizeof(interleaved))
                    k -= sizeof(interleaved) - 1;
            }
        }
        for (i = 0, k = 0; i < (MDC1200_FEC_K * 2); i++) {
            int bit_num;
            uint8_t b = 0;
            for (bit_num = 7; bit_num >= 0; bit_num--)
                if (interleaved[k++])
                    b |= 1u << bit_num;
            data8[i] = b;
        }
    }
    return data8 + (MDC1200_FEC_K * 2);
}

unsigned int MDC1200_encode_single_packet(void *data, const uint8_t op, const uint8_t arg, const uint16_t unit_id) {
    unsigned int size;
    uint16_t crc;
    uint8_t *p = (uint8_t *) data;

    // Motorola-reference TX framing is assembled in BK4819_send_MDC1200().
    // Keep the encoder output starting at sync; do not prepend the legacy 0x00,0xFF bytes.
    memcpy(p, mdc1200_sync, sizeof(mdc1200_sync));
    p += sizeof(mdc1200_sync);

    p[0] = op;
    p[1] = arg;
    p[2] = (unit_id >> 8) & 0x00ff;
    p[3] = (unit_id >> 0) & 0x00ff;
    crc = compute_crc(p, 4);
    p[4] = (crc >> 0) & 0x00ff;
    p[5] = (crc >> 8) & 0x00ff;
    p[6] = 0;

    p = encode_data(p);
    size = (unsigned int) (p - (uint8_t *) data);
    xor_modulation(data, size);
    return size;
}

// *** RX PERSISTENT STATE[cite: 3] ***

struct {
    uint8_t bit;
    uint8_t prev_bit;
    uint8_t xor_bit;
    uint64_t shift_reg;
    unsigned int bit_count;
    unsigned int stage;
    bool inverted_sync;
    unsigned int data_index;
    uint8_t data[40];
} rx;

void MDC1200_reset_rx(void) {
    memset(&rx, 0, sizeof(rx));
}

/**
 * FIXED: process_rx_data[cite: 3]
 * Removed the memset at the start. Now properly tracks bits across 
 * multiple function calls, essential for weak/intermittent signals.
 */
bool MDC1200_process_rx_data(
        const void *buffer,
        const unsigned int size,
        uint8_t *op,
        uint8_t *arg,
        uint16_t *unit_id) {
    const uint8_t *buffer8 = (const uint8_t *) buffer;
    unsigned int index;

    // DO NOT CLEAR rx HERE! Clearing here breaks fragmented reception.[cite: 3]

    for (index = 0; index < size; index++) {
        int bit;
        const uint8_t rx_byte = buffer8[index];

        for (bit = 7; bit >= 0; bit--) {
            unsigned int i;
            rx.prev_bit = rx.bit;
            rx.bit = (rx_byte >> bit) & 1u;
            rx.xor_bit = (rx.xor_bit ^ rx.bit) & 1u; 
            rx.shift_reg = (rx.shift_reg << 1) | rx.xor_bit;
            rx.bit_count++;

            if (rx.stage == 0) { // Looking for sync
                // We check every bit shift for a potential sync match
                const unsigned int sync_bit_ok_threshold = 32;

                if (rx.bit_count >= 40) {
                    uint64_t sync_nor = 0x07092a446fu;
                    uint64_t sync_inv = 0xffffffffffu ^ sync_nor;

                    sync_nor ^= rx.shift_reg;
                    sync_inv ^= rx.shift_reg;

                    unsigned int nor_count = 0;
                    unsigned int inv_count = 0;
                    for (i = 40; i > 0; i--, sync_nor >>= 1, sync_inv >>= 1) {
                        nor_count += sync_nor & 1u;
                        inv_count += sync_inv & 1u;
                    }
                    nor_count = 40 - nor_count;
                    inv_count = 40 - inv_count;

                    if (nor_count >= sync_bit_ok_threshold || inv_count >= sync_bit_ok_threshold) {
                        rx.inverted_sync = (inv_count > nor_count) ? true : false;
                        rx.data_index = 0;
                        rx.bit_count = 0; // Reset for payload collection
                        rx.stage = 1;
                    }
                }
                continue;
            }

            // Payload collection stage
            if (rx.bit_count < 8)
                continue;

            rx.bit_count = 0;
            rx.data[rx.data_index++] = rx.shift_reg & 0xff;

            if (rx.data_index < (MDC1200_FEC_K * 2))
                continue;

            if (!decode_data(rx.data)) {
                MDC1200_reset_rx();
                continue;
            }

            *op = rx.data[0];
            *arg = rx.data[1];
            *unit_id = ((uint16_t) rx.data[2] << 8) | (rx.data[3] << 0);

            MDC1200_reset_rx();
            return true;
        }
    }
    return false;
}


/* -------------------------------------------------------------------------
 * TMID independent raw FFSK packet receiver.
 *
 * Air format:
 *   1200 baud, 1200 Hz mark / 2200 Hz space
 *   A5 A5 raw sync word (16 bits, LSB first)
 *   9 asynchronous octets: start(0), data(8 LSB first), stop(1)
 *   identity = 6 callsign ASCII bytes + 1 ASCII SSID digit
 *   CRC-16/MODBUS, low byte first
 *
 * No convolutional code, interleaver, HDLC or legacy MDC decoder is used.
 * ------------------------------------------------------------------------- */
char tmid_rx_callsign[7];
uint8_t tmid_rx_ssid;

static uint16_t tmid_crc16(const uint8_t *p, unsigned int n)
{
    uint16_t crc = 0xFFFFu;
    while (n--) {
        crc ^= *p++;
        for (unsigned int b = 0; b < 8u; ++b)
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u)
                             : (uint16_t)(crc >> 1);
    }
    return crc;
}

void TMID_EncodePayload(const uint8_t input[TMID_ID_BYTES],
                        uint8_t output[TMID_PACKET_BYTES])
{
    const uint16_t crc = tmid_crc16(input, TMID_ID_BYTES);
    memcpy(output, input, TMID_ID_BYTES);
    output[7] = (uint8_t)crc;
    output[8] = (uint8_t)(crc >> 8);
}

static bool tmid_payload_valid(const uint8_t *p)
{
    bool padding = false;
    if (tmid_crc16(p, TMID_ID_BYTES) !=
        (uint16_t)(p[7] | ((uint16_t)p[8] << 8)))
        return false;

    for (unsigned int i = 0; i < 6u; ++i) {
        const char ch = (char)p[i];
        if (ch == ' ') {
            padding = true;
            continue;
        }
        if (padding || !((ch >= 'A' && ch <= 'Z') ||
                         (ch >= '0' && ch <= '9')))
            return false;
    }
    return p[0] != ' ' && p[6] >= '0' && p[6] <= '9';
}

static uint8_t tmid_get_raw_bit(const uint8_t *raw, unsigned int bit,
                                unsigned int reverse)
{
    const unsigned int shift = reverse ? (7u - (bit & 7u)) : (bit & 7u);
    return (uint8_t)((raw[bit >> 3] >> shift) & 1u);
}

/*
 * Decode framed octets from the raw post-sync FIFO bitstream. We explicitly
 * check every start and stop bit. A candidate is accepted only after CRC and
 * callsign/SSID validation, so a false sync or a framing slip cannot display
 * a random ID.
 */
static bool tmid_decode_serial(const uint8_t *raw,
                               uint8_t output[TMID_ID_BYTES])
{
    uint8_t packet[TMID_PACKET_BYTES];

    for (unsigned int reverse = 0; reverse < 2u; ++reverse) {
        for (unsigned int invert = 0; invert < 2u; ++invert) {
            for (unsigned int offset = 0; offset < 7u; ++offset) {
                bool framing_ok = true;
                unsigned int bit = offset;

                for (unsigned int octet = 0; octet < TMID_PACKET_BYTES; ++octet) {
                    uint8_t value = 0;
                    if ((tmid_get_raw_bit(raw, bit++, reverse) ^ invert) != 0u) {
                        framing_ok = false; /* start bit must be space / 0 */
                        break;
                    }
                    for (unsigned int data_bit = 0; data_bit < 8u; ++data_bit) {
                        const uint8_t data = tmid_get_raw_bit(raw, bit++, reverse) ^ invert;
                        value |= (uint8_t)(data << data_bit);
                    }
                    if ((tmid_get_raw_bit(raw, bit++, reverse) ^ invert) != 1u) {
                        framing_ok = false; /* stop bit must be mark / 1 */
                        break;
                    }
                    packet[octet] = value;
                }

                if (framing_ok && tmid_payload_valid(packet)) {
                    memcpy(output, packet, TMID_ID_BYTES);
                    return true;
                }
            }
        }
    }
    return false;
}

static uint8_t tmid_rx_buffer[TMID_SERIAL_BYTES];
static uint8_t tmid_rx_count;
static uint8_t tmid_rx_idle_ticks;
static bool tmid_rx_armed;
static bool tmid_rx_capturing;
static bool tmid_irq_mask_saved;
static uint16_t tmid_saved_irq_mask;

static bool tmid_mode_selected(void)
{
    return gEeprom.PTT_ID == PTT_ID_MODE_TMID_PRE ||
           gEeprom.PTT_ID == PTT_ID_MODE_TMID_POST ||
           gEeprom.PTT_ID == PTT_ID_MODE_TMID_BOTH;
}

static void tmid_rx_restart_capture(void)
{
    BK4819_WriteRegister(BK4819_REG_59, 0x4000u);
    BK4819_WriteRegister(BK4819_REG_59, 0x1000u);
    tmid_rx_count = 0;
    tmid_rx_capturing = false;
}

static void tmid_rx_arm(void)
{
    BK4819_WriteRegister(BK4819_REG_02, 0u);
    BK4819_WriteRegister(BK4819_REG_3F, 0u);
    BK4819_RX_TurnOn();

    /* 2200-Hz space / 1200-Hz mark FFSK receive pair. */
    BK4819_WriteRegister(BK4819_REG_70, 0x80E0u);
    BK4819_WriteRegister(BK4819_REG_71, 22714u);
    BK4819_WriteRegister(BK4819_REG_72, 0x3065u);
    BK4819_WriteRegister(BK4819_REG_58, 0x3FC3u);
    BK4819_WriteRegister(BK4819_REG_5A, 0xA5A5u);
    BK4819_WriteRegister(BK4819_REG_5C, 0xAA30u);
    BK4819_WriteRegister(BK4819_REG_5D,
                         (uint16_t)((TMID_SERIAL_BYTES - 1u) << 8));
    BK4819_WriteRegister(BK4819_REG_5E, (64u << 3) | 4u);

    {
        const uint16_t mask = tmid_irq_mask_saved ? tmid_saved_irq_mask : 0u;
        BK4819_WriteRegister(BK4819_REG_3F,
            mask | BK4819_REG_3F_FSK_RX_SYNC |
                   BK4819_REG_3F_FSK_RX_FINISHED |
                   BK4819_REG_3F_FSK_FIFO_ALMOST_FULL);
    }

    tmid_rx_restart_capture();
    tmid_rx_armed = true;
}

static void tmid_rx_stop(void)
{
    if (!tmid_rx_armed)
        return;
    BK4819_WriteRegister(BK4819_REG_59, 0u);
    BK4819_WriteRegister(BK4819_REG_58, 0u);
    BK4819_WriteRegister(BK4819_REG_70, 0u);
    if (tmid_irq_mask_saved) {
        BK4819_WriteRegister(BK4819_REG_3F, tmid_saved_irq_mask);
        tmid_irq_mask_saved = false;
    }
    tmid_rx_armed = false;
    tmid_rx_capturing = false;
    tmid_rx_count = 0;
}

void TMID_RxService(void)
{
    if (!tmid_mode_selected() || gCurrentFunction == FUNCTION_TRANSMIT ||
        gCurrentFunction == FUNCTION_POWER_SAVE) {
        tmid_rx_stop();
        return;
    }
    if (!tmid_rx_armed) {
        if (!tmid_irq_mask_saved) {
            tmid_saved_irq_mask = BK4819_ReadRegister(BK4819_REG_3F);
            tmid_irq_mask_saved = true;
        }
        tmid_rx_arm();
    }
}

static void tmid_rx_finish(void);
static void tmid_rx_read_fifo(uint8_t words);

/* Called at the firmware's 10 ms cadence, not from the tight main-loop spin. */
void TMID_RxTimeSlice10ms(void)
{
    if (!tmid_rx_armed || !tmid_mode_selected() ||
        gCurrentFunction == FUNCTION_TRANSMIT) {
        tmid_rx_idle_ticks = 0;
        return;
    }

    {
        const uint16_t required = BK4819_REG_3F_FSK_RX_SYNC |
                                 BK4819_REG_3F_FSK_RX_FINISHED |
                                 BK4819_REG_3F_FSK_FIFO_ALMOST_FULL;
        const uint16_t mask = BK4819_ReadRegister(BK4819_REG_3F);
        if ((mask & required) != required)
            BK4819_WriteRegister(BK4819_REG_3F, mask | required);
    }

    if (!tmid_rx_capturing) {
        tmid_rx_idle_ticks = 0;
        return;
    }
    if (tmid_rx_idle_ticks < 150u)
        ++tmid_rx_idle_ticks;
    if (tmid_rx_idle_ticks >= 150u) {
        tmid_rx_read_fifo(8u);
        tmid_rx_finish();
        tmid_rx_restart_capture();
        tmid_rx_idle_ticks = 0;
    }
}

static void tmid_rx_finish(void)
{
    uint8_t decoded[TMID_ID_BYTES];
    const bool valid = (tmid_rx_count == TMID_SERIAL_BYTES) &&
                       tmid_decode_serial(tmid_rx_buffer, decoded);

    if (valid) {
        memcpy(tmid_rx_callsign, decoded, 6u);
        tmid_rx_callsign[6] = '\0';
        tmid_rx_ssid = (uint8_t)(decoded[6] - '0');
        mdc1200_rx_ready_tick_500ms = 10u;
        gUpdateDisplay = true;
    }

    tmid_rx_count = 0;
    tmid_rx_capturing = false;
}

static void tmid_rx_read_fifo(uint8_t words)
{
    while (words-- && tmid_rx_count < TMID_SERIAL_BYTES) {
        const uint16_t word = BK4819_ReadRegister(BK4819_REG_5F);
        tmid_rx_buffer[tmid_rx_count++] = (uint8_t)word;
        if (tmid_rx_count < TMID_SERIAL_BYTES)
            tmid_rx_buffer[tmid_rx_count++] = (uint8_t)(word >> 8);
    }
}

void TMID_HandleRxInterrupts(uint16_t interrupt_bits)
{
    if (!tmid_rx_armed)
        return;

    if (interrupt_bits & BK4819_REG_02_FSK_RX_SYNC) {
        tmid_rx_count = 0;
        tmid_rx_idle_ticks = 0;
        tmid_rx_capturing = true;
    }

    if ((interrupt_bits & BK4819_REG_02_FSK_FIFO_ALMOST_FULL) &&
        tmid_rx_capturing) {
        tmid_rx_idle_ticks = 0;
        tmid_rx_read_fifo(4u);
    }

    if (interrupt_bits & BK4819_REG_02_FSK_RX_FINISHED) {
        if (tmid_rx_capturing) {
            tmid_rx_read_fifo(8u);
            tmid_rx_finish();
        }
        tmid_rx_restart_capture();
    }
}

#ifdef ENABLE_FLEETSYNC
#include "app/fleetsync.h"
static uint8_t fleetsync_rx_buffer[FLEETSYNC_PACKET_SIZE];
static unsigned int fleetsync_rx_buffer_index;
#endif

void solve_sign(const uint16_t interrupt_bits)
{
    if (tmid_mode_selected()) {
        TMID_HandleRxInterrupts(interrupt_bits);
        return;
    }
    const bool rx_sync = (interrupt_bits & BK4819_REG_02_FSK_RX_SYNC) != 0;
    const bool rx_fifo_almost_full =
        (interrupt_bits & BK4819_REG_02_FSK_FIFO_ALMOST_FULL) != 0;
    const bool rx_finished =
        (interrupt_bits & BK4819_REG_02_FSK_RX_FINISHED) != 0;
    const uint16_t rx_sync_flags = BK4819_ReadRegister(BK4819_REG_0B);
    const bool rx_sync_neg = (rx_sync_flags & (1u << 7)) != 0;
#ifdef ENABLE_FLEETSYNC
    const bool useFleetSyncDecode =
        gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_PRE ||
        gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_POST ||
        gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_BOTH;
#endif

    if (rx_sync) {
#ifdef ENABLE_FLEETSYNC
        if (useFleetSyncDecode) {
            fleetsync_rx_buffer_index = 0;
            fleetsync_rx_buffer[fleetsync_rx_buffer_index++] = 0xAA;
            fleetsync_rx_buffer[fleetsync_rx_buffer_index++] = 0xAA;
            fleetsync_rx_buffer[fleetsync_rx_buffer_index++] = 0x23;
            fleetsync_rx_buffer[fleetsync_rx_buffer_index++] = 0xEB;
        } else
#endif
        {
            mdc1200_rx_buffer_index = 0;
            for (unsigned int i = 0; i < sizeof(mdc1200_sync_suc_xor); i++)
                mdc1200_rx_buffer[mdc1200_rx_buffer_index++] =
                    mdc1200_sync_suc_xor[i] ^ (rx_sync_neg ? 0xFF : 0x00);
        }
    }

    if (rx_fifo_almost_full) {
        const uint16_t count = BK4819_ReadRegister(BK4819_REG_5E) & 7u;
        for (uint16_t i = 0; i < count; i++) {
            const uint16_t word = BK4819_ReadRegister(BK4819_REG_5F);
#ifdef ENABLE_FLEETSYNC
            if (useFleetSyncDecode) {
                if (fleetsync_rx_buffer_index < sizeof(fleetsync_rx_buffer))
                    fleetsync_rx_buffer[fleetsync_rx_buffer_index++] = word & 0xFF;
                if (fleetsync_rx_buffer_index < sizeof(fleetsync_rx_buffer))
                    fleetsync_rx_buffer[fleetsync_rx_buffer_index++] = word >> 8;
                continue;
            }
#endif
            {
                const uint16_t data = word ^ (rx_sync_neg ? 0xFFFF : 0x0000);
                if (mdc1200_rx_buffer_index < sizeof(mdc1200_rx_buffer))
                    mdc1200_rx_buffer[mdc1200_rx_buffer_index++] = data & 0xFF;
                if (mdc1200_rx_buffer_index < sizeof(mdc1200_rx_buffer))
                    mdc1200_rx_buffer[mdc1200_rx_buffer_index++] = data >> 8;
            }
        }

#ifdef ENABLE_FLEETSYNC
        if (useFleetSyncDecode &&
            fleetsync_rx_buffer_index >= sizeof(fleetsync_rx_buffer)) {
            uint16_t fleet, unit;
            if (FleetSync_decode_ani(fleetsync_rx_buffer, &fleet, &unit)) {
                mdc1200_fleet_id = fleet;
                mdc1200_unit_id = unit;
                mdc1200_rx_ready_tick_500ms = 10;
                gUpdateDisplay = true;
            }
            fleetsync_rx_buffer_index = 0;
        }
#endif

        if (mdc1200_rx_buffer_index >= sizeof(mdc1200_rx_buffer)) {
#ifdef ENABLE_FLEETSYNC
            if (!useFleetSyncDecode)
#endif
            {
                if (MDC1200_process_rx_data(mdc1200_rx_buffer,
                                             mdc1200_rx_buffer_index,
                                             &mdc1200_op, &mdc1200_arg,
                                             &mdc1200_unit_id)) {
                    mdc1200_rx_ready_tick_500ms = 10;
                    gUpdateDisplay = true;
                }
            }
            mdc1200_rx_buffer_index = 0;
        }
    }

    if (rx_finished) {
#ifdef ENABLE_FLEETSYNC
        if (useFleetSyncDecode)
            fleetsync_rx_buffer_index = 0;
#endif
        const uint16_t fsk_reg59 =
            BK4819_ReadRegister(BK4819_REG_59) &
            ~((1u << 15) | (1u << 14) | (1u << 12) | (1u << 11));
        BK4819_WriteRegister(BK4819_REG_59, (1u << 15) | (1u << 14) | fsk_reg59);
        BK4819_WriteRegister(BK4819_REG_59, (1u << 12) | fsk_reg59);
    }
}

// *** UTILITY FUNCTIONS (KEEP VERBATIM)[cite: 3] ***

void MDC1200_init(void) {
    MDC1200_reset_rx();
}

uint16_t extractHex(const char *str) {
    uint16_t result = 0;
    while (*str) {
        char c = *str++;
        if (c >= '0' && c <= '9') { result = (result << 4) | (c - '0'); } 
        else if (c >= 'A' && c <= 'F') { result = (result << 4) | (c - 'A' + 10); }
    }
    return result;
}

#ifdef  ENABLE_MDC1200_CONTACT
uint8_t contact_num=0;
void mdc1200_update_contact_num() {
    EEPROM_ReadBuffer(MDC_NUM_ADD, (uint8_t *)&contact_num, 1);
    if(contact_num>MAX_CONTACT_NUM)contact_num=0;
}

bool mdc1200_contact_find(uint16_t mdc_id, char *contact) {
    mdc1200_update_contact_num();
    uint16_t add = 0x1D00;
    for (uint8_t i = 0; i < contact_num; i++) {
        uint8_t read_once[16]={0};
        EEPROM_ReadBuffer(add , read_once, 16);
        if (mdc_id == (uint16_t) (read_once[1] | (read_once[0] << 8))) {
            int valid_len = 0;
            for (int j = 0; j < 14; ++j) {
                uint8_t c = read_once[2 + j];
                if (c == '\0') break;
                if (c < ' ' || c > '~') return false;
                valid_len++;
            }
            memcpy(contact, read_once + 2, 14);
            contact[14] = '\0';
            return true;
        }
        add += 16;
        if(add == 0x1E00) add = 0x1F90;
    }
    return false;
}

bool fleetsync_contact_find(uint16_t fleet_id, uint16_t unit_id, char *contact) {
    char stored_fleet[4];
    char stored_unit[5];
    const uint16_t fleet_digits = fleet_id;
    const uint16_t unit_digits = unit_id;

    for (unsigned int i = 0; i < 16; i++) {
        uint8_t read_once[16] = {0};
        EEPROM_ReadBuffer(0x1C00 + (i * 16), read_once, 16);

        if (read_once[0] == 0xFF || read_once[0] == 0)
            continue;

        memset(stored_fleet, 0, sizeof(stored_fleet));
        memset(stored_unit, 0, sizeof(stored_unit));
        memcpy(stored_fleet, &read_once[8], 3);
        memcpy(stored_unit, &read_once[11], 4);

        const uint16_t stored_fleet_id = (uint16_t)(
            ((read_once[8] >= '0' && read_once[8] <= '9') ? (read_once[8] - '0') : 0) * 100u +
            ((read_once[9] >= '0' && read_once[9] <= '9') ? (read_once[9] - '0') : 0) * 10u +
            ((read_once[10] >= '0' && read_once[10] <= '9') ? (read_once[10] - '0') : 0));
        const uint16_t stored_unit_id = (uint16_t)(
            ((read_once[11] >= '0' && read_once[11] <= '9') ? (read_once[11] - '0') : 0) * 1000u +
            ((read_once[12] >= '0' && read_once[12] <= '9') ? (read_once[12] - '0') : 0) * 100u +
            ((read_once[13] >= '0' && read_once[13] <= '9') ? (read_once[13] - '0') : 0) * 10u +
            ((read_once[14] >= '0' && read_once[14] <= '9') ? (read_once[14] - '0') : 0));

        if (stored_fleet_id == fleet_digits && stored_unit_id == unit_digits) {
            memcpy(contact, read_once, 8);
            contact[8] = '\0';
            while (contact[0] != '\0' && contact[strlen(contact) - 1] == ' ')
                contact[strlen(contact) - 1] = '\0';
            return true;
        }
    }

    return false;
}
#endif