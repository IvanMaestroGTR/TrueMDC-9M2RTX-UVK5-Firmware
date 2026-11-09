#ifndef FLEETSYNC_H
#define FLEETSYNC_H

#include <stdbool.h>
#include <stdint.h>

/* FleetSync II ANI is one 64-bit block after the 32-bit sync word. */
#define FLEETSYNC_PACKET_SIZE 12u

/* Wire IDs use Fleet = displayed ID - 99 and Unit = displayed ID - 999. */
#define FLEETSYNC_FLEET_MIN 100u
#define FLEETSYNC_FLEET_MAX 349u
#define FLEETSYNC_UNIT_MIN  1000u
#define FLEETSYNC_UNIT_MAX  4999u

/*
 * Builds a FleetSync II ANI/PTT-ID burst.
 * end_of_transmission selects the EOT form; false selects BOT.
 */
unsigned int FleetSync_encode_ani(void *data, uint16_t fleet, uint16_t unit,
                                  bool end_of_transmission);

/*
 * Decode a FleetSync II ANI burst.
 * Returns true if successfully decoded, false otherwise.
 */
bool FleetSync_decode_ani(const uint8_t *data, uint16_t *fleet, uint16_t *unit);

/*
 * Attempts a MDC-style byte-level correction pass on a FleetSync payload block.
 * This is a compatibility helper for code paths that want the same greedy
 * forward-error repair style as MDC1200 without changing the on-air payload.
 */
bool FleetSync_mdc_style_correct(uint8_t *block);

#endif
