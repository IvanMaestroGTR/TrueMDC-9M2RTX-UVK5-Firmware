#include "fleetsync.h"

#include <stdint.h>
#include <string.h>

/* FleetSync II's 32-bit over-the-air sync. */
static const uint8_t fleetsync_sync[] = {0xaa, 0xaa, 0x23, 0xeb};

/*
 * This is the CRC-15 calculation used by the supplied decoder.  The returned
 * low bit is reserved for the block's even-parity check bit.
 */
static uint16_t FleetSync_crc15(const uint8_t *data)
{
    uint16_t crc = 0;

    for (unsigned int byte = 0; byte < 6; byte++) {
        for (int bit = 7; bit >= 0; bit--) {
            const uint16_t input = (data[byte] >> bit) & 1u;
            const uint16_t feedback = input ^ ((crc >> 15) & 1u);

            if (feedback)
                crc ^= 0x6815u;
            crc <<= 1;
        }
    }

    return crc ^ 0x0002u;
}

static uint8_t FleetSync_even_parity(const uint8_t *data, unsigned int size)
{
    uint8_t parity = 0;

    for (unsigned int i = 0; i < size; i++) {
        uint8_t value = data[i];
        while (value) {
            parity ^= 1u;
            value &= value - 1u;
        }
    }

    return parity;
}

static bool FleetSync_validate_block(const uint8_t *block, uint16_t *fleet, uint16_t *unit)
{
    const uint16_t crc_calculated = FleetSync_crc15(block);
    const uint16_t crc_received = ((uint16_t)block[6] << 8) | block[7];

    if ((crc_calculated >> 1) != (crc_received >> 1))
        return false;
    if (FleetSync_even_parity(block, 8) != 0)
        return false;

    /* FleetSync wire values are offset from displayed IDs by 99 and 999. */
    *fleet = (uint16_t)block[2] + (FLEETSYNC_FLEET_MIN - 1u);
    *unit = (uint16_t)(((uint16_t)block[3] << 4) | ((uint16_t)block[4] >> 4)) +
            (FLEETSYNC_UNIT_MIN - 1u);

    return *fleet <= FLEETSYNC_FLEET_MAX && *unit <= FLEETSYNC_UNIT_MAX;
}

static bool FleetSync_mdc_style_correct_block(uint8_t *block, uint16_t *fleet, uint16_t *unit)
{
    uint8_t corrected[8];
    memcpy(corrected, block, sizeof(corrected));

    /*
     * MDC1200 uses a K=7 convolutional-style correction loop.  FleetSync's
     * payload is only 8 bytes, so apply the same syndrome pattern with a
     * wrap-around pass across the block to recover single-bit errors while
     * preserving the native packet format.
     */
    uint8_t shift_reg = 0;
    uint8_t syn = 0;

    for (unsigned int i = 0; i < 8u; i++) {
        const uint8_t bi = corrected[i];

        for (int bit_num = 7; bit_num >= 0; bit_num--) {
            unsigned int k = 0;
            shift_reg = (uint8_t)((shift_reg << 1) | ((bi >> bit_num) & 1u));
            const uint8_t b = (uint8_t)(((shift_reg >> 6) ^ (shift_reg >> 5) ^
                                         (shift_reg >> 2) ^ (shift_reg >> 0)) & 1u);
            const uint8_t paired = corrected[(i + 1u) % 8u];
            syn = (uint8_t)((syn << 1) | (((b ^ ((paired >> bit_num) & 1u)) & 1u) ? 1u : 0u));

            if (syn & 0x80u) k++;
            if (syn & 0x20u) k++;
            if (syn & 0x04u) k++;
            if (syn & 0x02u) k++;

            if (k >= 3u) {
                int ii = (int)i;
                int bn = bit_num - 7;

                if (bn < 0) {
                    bn += 8;
                    ii--;
                }

                if (ii >= 0)
                    corrected[(unsigned int)ii] ^= (uint8_t)(1u << (bn & 7u));

                syn ^= 0xA6u;
            }
        }
    }

    if (FleetSync_validate_block(corrected, fleet, unit)) {
        memcpy(block, corrected, sizeof(corrected));
        return true;
    }

    return false;
}

bool FleetSync_mdc_style_correct(uint8_t *block)
{
    uint16_t fleet = 0;
    uint16_t unit = 0;

    return FleetSync_mdc_style_correct_block(block, &fleet, &unit);
}

static inline bool FleetSync_decode_block(const uint8_t *block, uint16_t *fleet, uint16_t *unit)
{
    if (FleetSync_validate_block(block, fleet, unit))
        return true;

    /*
     * FleetSync has CRC and parity redundancy, so one-bit errors can often be
     * corrected without changing the over-the-air packet format.  Try a single
     * bit flip across the 64-bit payload block and accept the first valid decode.
     */
    uint8_t corrected[8];
    memcpy(corrected, block, sizeof(corrected));

    for (unsigned int bit = 0; bit < (sizeof(corrected) * 8u); bit++) {
        const unsigned int byte_index = bit / 8u;
        const unsigned int bit_index = bit % 8u;

        corrected[byte_index] ^= (uint8_t)(1u << bit_index);
        if (FleetSync_validate_block(corrected, fleet, unit))
            return true;
        corrected[byte_index] ^= (uint8_t)(1u << bit_index);
    }

    if (FleetSync_mdc_style_correct_block(corrected, fleet, unit))
        return true;

    return false;
}

unsigned int FleetSync_encode_ani(void *data, uint16_t fleet, uint16_t unit,
                                  bool end_of_transmission)
{
    uint8_t *packet = data;
    uint8_t *block = packet + sizeof(fleetsync_sync);
    /* Match the FleetSync reference: displayed ID = wire value + 99/999. */
    uint16_t fleet_raw = fleet - (FLEETSYNC_FLEET_MIN - 1u);
    uint16_t unit_raw = unit - (FLEETSYNC_UNIT_MIN - 1u);
    uint16_t crc;

    /* Callers normally validate these.  Clamping also makes this safe alone. */
    if (fleet < FLEETSYNC_FLEET_MIN || fleet > FLEETSYNC_FLEET_MAX)
        fleet_raw = 1;
    if (unit < FLEETSYNC_UNIT_MIN || unit > FLEETSYNC_UNIT_MAX)
        unit_raw = 1;

    for (unsigned int i = 0; i < sizeof(fleetsync_sync); i++)
        packet[i] = fleetsync_sync[i];

    /*
     * ANI, no Fleet-extension: the destination is the unassigned unit/fleet.
     * Bit 1 is the inverted EOT flag (1 = BOT, 0 = EOT).
     */
    block[0] = end_of_transmission ? 0x00 : 0x02;
    block[1] = 0x80;                    /* ANI flag */
    block[2] = (uint8_t)fleet_raw;
    block[3] = (uint8_t)(unit_raw >> 4);
    block[4] = (uint8_t)(unit_raw << 4);
    block[5] = 0x00;

    crc = FleetSync_crc15(block);
    block[6] = (uint8_t)(crc >> 8);
    block[7] = (uint8_t)crc;

    /* The least-significant bit of the final byte is the even parity bit. */
    block[7] &= 0xfeu;
    if (FleetSync_even_parity(block, 8) != 0)
        block[7] |= 1u;

    return FLEETSYNC_PACKET_SIZE;
}

/*
 * Decode a FleetSync II ANI burst.
 * Returns true if successfully decoded, false otherwise.
 */
bool FleetSync_decode_ani(const uint8_t *data, uint16_t *fleet, uint16_t *unit)
{
    if (!data || !fleet || !unit)
        return false;
    
    // Check sync word
    if (data[0] != 0xaa || data[1] != 0xaa || data[2] != 0x23 || data[3] != 0xeb)
        return false;
    
    const uint8_t *block = data + sizeof(fleetsync_sync);

    return FleetSync_decode_block(block, fleet, unit);
}
