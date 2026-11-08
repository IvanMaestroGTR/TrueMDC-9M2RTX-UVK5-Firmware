/* Copyright 2023 Dual Tachyon
 * https://github.com/DualTachyon
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */
#include "app/app.h"
#include "ui/ui.h"
#include "ui/main.h"
#include <stdio.h>
#include "string.h"
#include "settings.h"
#include "../audio.h"
#include "../bsp/dp32g030/gpio.h"
#include "../bsp/dp32g030/portcon.h"
#include "bk4819.h"
#include "gpio.h"
#include "system.h"
#include "systick.h"
#include "ARMCM0.h"

#ifdef ENABLE_MDC1200
#include "app/mdc1200.h"
#ifdef ENABLE_FLEETSYNC
#include "app/fleetsync.h"
#endif
#endif

#include "misc.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))
#endif
BK4819_FilterBandwidth_t m_bandwidth = BK4819_FILTER_BW_NARROW;


static const uint8_t DTMF_TONE1_GAIN = 65;
static const uint8_t DTMF_TONE2_GAIN = 93;
static const uint8_t MDC_FSK_TX_GAIN = 96;  // FSK gain for MDC1200 TX (configurable: 0-127, default 96)

// SysTick ticks per 1200-baud symbol. Nominal is 40000 @ 48 MHz.
// Measured +0.5% slow on one radio, so start at 40000 * 1200/1194 ~= 39800.
// Retune: new = old * 1200 / measured_baud
#define MDC_TX_BIT_TICKS   39870u

#ifdef ENABLE_MDC1200
static bool BK4819_IsFleetSyncPTTIDMode(void)
{
    return gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_PRE ||
           gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_POST ||
           gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_BOTH;
}

static bool BK4819_HasPostIDPTTID(void)
{
    return gEeprom.PTT_ID == PTT_ID_MODE_MDC_POST ||
           gEeprom.PTT_ID == PTT_ID_MODE_MDC_BOTH ||
           (BK4819_IsFleetSyncPTTIDMode() &&
            (gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_POST ||
             gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_BOTH));
}

static uint8_t BK4819_GetMDCPostPreamble(void)
{
    return gEeprom.MDC1200_PREAMBLE_WHEN == MDC_PREAMBLE_WHEN_PRE
         ? 0
         : gEeprom.MDC1200_PREAMBLE_DURATION;
}

static bool BK4819_SendPostPTTID(void)
{
#ifdef ENABLE_FLEETSYNC
    if (BK4819_IsFleetSyncPTTIDMode()) {
        BK4819_send_FleetSync(gEeprom.FLEETSYNC_FLEET,
                              gEeprom.FLEETSYNC_UNIT,
                              true);
        return true;
    }
#endif
    BK4819_send_MDC1200(MDC1200_OP_CODE_POST_ID,
                        0x00,
                        gEeprom.MDC1200_ID,
                        BK4819_GetMDCPostPreamble());
    return false;
}
#endif

static uint16_t gBK4819_GpioOutState;

bool gRxIdleMode;
static bool gKenwSecondaryTone;

__inline uint16_t scale_freq(const uint16_t freq) {
//	return (((uint32_t)freq * 1032444u) + 50000u) / 100000u;   // with rounding
    return (((uint32_t) freq * 1353245u) + (1u << 16)) >> 17;   // with rounding
}

void BK4819_Init(void) {
    GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCN);
    GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCL);
    GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SDA);

    BK4819_WriteRegister(BK4819_REG_00, 0x8000);
    BK4819_WriteRegister(BK4819_REG_00, 0x0000);

    BK4819_WriteRegister(BK4819_REG_37, 0x1D0F);
    BK4819_WriteRegister(BK4819_REG_36, 0x0022);

    BK4819_InitAGC(false);
    BK4819_SetAGC(true);

    BK4819_WriteRegister(BK4819_REG_19, 0b0001000001000001);   // <15> MIC AGC  1 = disable  0 = enable

    BK4819_WriteRegister(BK4819_REG_7D, 0xE940);

    // REG_48 .. RX AF level
    //
    // <15:12> 11  ???  0 to 15
    //
    // <11:10> 0 AF Rx Gain-1
    //         0 =   0dB
    //         1 =  -6dB
    //         2 = -12dB
    //         3 = -18dB
    //
    // <9:4>   60 AF Rx Gain-2  -26dB ~ 5.5dB   0.5dB/step
    //         63 = max
    //          0 = mute
    //
    // <3:0>   15 AF DAC Gain (after Gain-1 and Gain-2) approx 2dB/step
    //         15 = max
    //          0 = min
    //
    BK4819_WriteRegister(BK4819_REG_48,    //  0xB3A8);     // 1011 00 111010 1000
                         (11u << 12) |     // ??? 0..15
                         (0u << 10) |     // AF Rx Gain-1
                         (58u << 4) |     // AF Rx Gain-2
                         (8u << 0));     // AF DAC Gain (after Gain-1 and Gain-2)

#if 1
    const uint8_t dtmf_coeffs[] = {111, 107, 103, 98, 80, 71, 58, 44, 65, 55, 37, 23, 228, 203, 181, 159};
    for (unsigned int i = 0; i < ARRAY_SIZE(dtmf_coeffs); i++)
        BK4819_WriteRegister(BK4819_REG_09, (i << 12) | dtmf_coeffs[i]);
#else
    // original code
    BK4819_WriteRegister(BK4819_REG_09, 0x006F);  // 6F
    BK4819_WriteRegister(BK4819_REG_09, 0x106B);  // 6B
    BK4819_WriteRegister(BK4819_REG_09, 0x2067);  // 67
    BK4819_WriteRegister(BK4819_REG_09, 0x3062);  // 62
    BK4819_WriteRegister(BK4819_REG_09, 0x4050);  // 50
    BK4819_WriteRegister(BK4819_REG_09, 0x5047);  // 47
    BK4819_WriteRegister(BK4819_REG_09, 0x603A);  // 3A
    BK4819_WriteRegister(BK4819_REG_09, 0x702C);  // 2C
    BK4819_WriteRegister(BK4819_REG_09, 0x8041);  // 41
    BK4819_WriteRegister(BK4819_REG_09, 0x9037);  // 37
    BK4819_WriteRegister(BK4819_REG_09, 0xA025);  // 25
    BK4819_WriteRegister(BK4819_REG_09, 0xB017);  // 17
    BK4819_WriteRegister(BK4819_REG_09, 0xC0E4);  // E4
    BK4819_WriteRegister(BK4819_REG_09, 0xD0CB);  // CB
    BK4819_WriteRegister(BK4819_REG_09, 0xE0B5);  // B5
    BK4819_WriteRegister(BK4819_REG_09, 0xF09F);  // 9F
#endif

    BK4819_WriteRegister(BK4819_REG_1F, 0x5454);
    BK4819_WriteRegister(BK4819_REG_3E, 0xA037);

    gBK4819_GpioOutState = 0x9000;

    BK4819_WriteRegister(BK4819_REG_33, 0x9000);
    BK4819_WriteRegister(BK4819_REG_3F, 0);
}

static uint16_t BK4819_ReadU16(void) {
    unsigned int i;
    uint16_t Value;

    PORTCON_PORTC_IE = (PORTCON_PORTC_IE & ~PORTCON_PORTC_IE_C2_MASK) | PORTCON_PORTC_IE_C2_BITS_ENABLE;
    GPIOC->DIR = (GPIOC->DIR & ~GPIO_DIR_2_MASK) | GPIO_DIR_2_BITS_INPUT;
    //SYSTICK_DelayUs(1);
    SYSTICK_Delay250ns(1);
    Value = 0;
    for (i = 0; i < 16; i++) {
        Value <<= 1;
        Value |= GPIO_CheckBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SDA);
        GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCL);
        SYSTICK_Delay250ns(1);
        GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCL);
        SYSTICK_Delay250ns(1);
    }
    PORTCON_PORTC_IE = (PORTCON_PORTC_IE & ~PORTCON_PORTC_IE_C2_MASK) | PORTCON_PORTC_IE_C2_BITS_DISABLE;
    GPIOC->DIR = (GPIOC->DIR & ~GPIO_DIR_2_MASK) | GPIO_DIR_2_BITS_OUTPUT;

    return Value;
}

uint16_t BK4819_ReadRegister(BK4819_REGISTER_t Register) {
    uint16_t Value;

    GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCN);
    GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCL);

    SYSTICK_Delay250ns(1);

    GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCN);
    BK4819_WriteU8(Register | 0x80);
    Value = BK4819_ReadU16();
    GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCN);

    SYSTICK_Delay250ns(1);

    GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCL);
    GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SDA);

    return Value;

}

void BK4819_WriteRegister(BK4819_REGISTER_t Register, uint16_t Data) {
    GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCN);
    GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCL);

    SYSTICK_Delay250ns(1);

    GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCN);
    BK4819_WriteU8(Register);

    //SYSTICK_Delay250ns(1);

    BK4819_WriteU16(Data);

    //SYSTICK_Delay250ns(1);

    GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCN);

    SYSTICK_Delay250ns(1);

    GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCL);
    GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SDA);
}

void BK4819_WriteU8(uint8_t Data) {
    unsigned int i;

    GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCL);
    for (i = 0; i < 8; i++) {
        if ((Data & 0x80) == 0)
            GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SDA);
        else
            GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SDA);

        SYSTICK_Delay250ns(1);
        GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCL);
        SYSTICK_Delay250ns(1);

        Data <<= 1;

        GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCL);
        SYSTICK_Delay250ns(1);
    }
}

void BK4819_WriteU16(uint16_t Data) {
    unsigned int i;

    GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCL);
    for (i = 0; i < 16; i++) {
        if ((Data & 0x8000) == 0)
            GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SDA);
        else
            GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SDA);

        SYSTICK_Delay250ns(1);
        GPIO_SetBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCL);

        Data <<= 1;

        SYSTICK_Delay250ns(1);
        GPIO_ClearBit(&GPIOC->DATA, GPIOC_PIN_BK4819_SCL);
        SYSTICK_Delay250ns(1);
    }
}

void BK4819_SetAGC(bool enable) {
    uint16_t regVal = BK4819_ReadRegister(BK4819_REG_7E);
    if (!(regVal & (1 << 15)) == enable)
        return;

    BK4819_WriteRegister(BK4819_REG_7E, (regVal & ~(1 << 15) & ~(0b111 << 12))
                                        | (!enable << 15)   // 0  AGC fix mode
                                        | (3u << 12)       // 3  AGC fix index
    );

    // if(enable) {
    // 	BK4819_WriteRegister(BK4819_REG_7B, 0x8420);
    // }
    // else {
    // 	BK4819_WriteRegister(BK4819_REG_7B, 0x318C);

    // 	BK4819_WriteRegister(BK4819_REG_7C, 0x595E);
    // 	BK4819_WriteRegister(BK4819_REG_20, 0x8DEF);

    // 	for (uint8_t i = 0; i < 8; i++) {
    // 		//BK4819_WriteRegister(BK4819_REG_06, ((i << 13) | 0x2500u) + 0x036u);
    // 		BK4819_WriteRegister(BK4819_REG_06, (i & 7) << 13 | 0x4A << 7 | 0x36);
    // 	}
    // }
}

void BK4819_InitAGC(bool amModulation) {
    // REG_10, REG_11, REG_12 REG_13, REG_14
    //
    // Rx AGC Gain Table[]. (Index Max->Min is 3,2,1,0,-1)
    //
    // <15:10> ???
    //
    // <9:8>   LNA Gain Short
    //         3 =   0dB  <<<		1o11				read from spectrum			reference manual
    //         2 = 					-24dB  				-19     					 -11
    //         1 = 					-30dB  				-24     					 -16
    //         0 = 					-33dB  				-28     					 -19
    //
    // <7:5>   LNA Gain
    //         7 =   0dB
    //         6 =  -2dB
    //         5 =  -4dB
    //         4 =  -6dB
    //         3 =  -9dB
    //         2 = -14dB <<<
    //         1 = -19dB
    //         0 = -24dB
    //
    // <4:3>   MIXER Gain
    //         3 =   0dB <<<
    //         2 =  -3dB
    //         1 =  -6dB
    //         0 =  -8dB
    //
    // <2:0>   PGA Gain
    //         7 =   0dB
    //         6 =  -3dB <<<
    //         5 =  -6dB
    //         4 =  -9dB
    //         3 = -15dB
    //         2 = -21dB
    //         1 = -27dB
    //         0 = -33dB
    //

    BK4819_WriteRegister(BK4819_REG_13, 0x03BE);  // 0x03BE / 000000 11 101 11 110 /  -7dB
    BK4819_WriteRegister(BK4819_REG_12, 0x037B);  // 0x037B / 000000 11 011 11 011 / -24dB
    BK4819_WriteRegister(BK4819_REG_11, 0x027B);  // 0x027B / 000000 10 011 11 011 / -43dB
    BK4819_WriteRegister(BK4819_REG_10, 0x007A);  // 0x007A / 000000 00 011 11 010 / -58dB

    if (amModulation) {
        BK4819_WriteRegister(BK4819_REG_14, 0x0000);
        BK4819_WriteRegister(BK4819_REG_49, (0 << 14) | (50 << 7) | (32 << 0));
    } else {
        BK4819_WriteRegister(BK4819_REG_14, 0x0019);  // 0x0019 / 000000 00 000 11 001 / -79dB
        BK4819_WriteRegister(BK4819_REG_49, (0 << 14) | (84 << 7) | (56 << 0)); //0x2A38 / 00 1010100 0111000 / 84, 56
    }
    BK4819_WriteRegister(BK4819_REG_7B, 0x8420);

}


/*
 * TrueMDC one-way callsign beacon.
 * 1200-baud AFSK: 1200 Hz = 1, 2200 Hz = 0.
 * Packet: D3 91 D3 91 sync, six ASCII callsign bytes, one ASCII SSID digit,
 * then CRC-16/ARC (little-endian). No AX.25, HDLC flags, NRZI or bit stuffing.
 * The existing 2200/1200-Hz warble preamble is intentionally preserved.
 */
void BK4819_SendCallsignBeacon(const bool with_preamble)
{
    uint8_t frame[13];
    uint16_t crc = 0xFFFFu;
    uint16_t saved_reg30;
    uint8_t tone_mark = 2u;
    const uint16_t mark = scale_freq(1200u);
    const uint16_t space = scale_freq(2200u);
    const uint32_t bit_ticks_q8 = 39870u << 8;
    const uint32_t reload = SysTick->LOAD + 1u;
    uint32_t acc_q8 = 0u, last;
    const uint32_t primask = __get_PRIMASK();
    static const uint8_t sync[4] = {0xD3u, 0x91u, 0xD3u, 0x91u};

    for (unsigned int i = 0; i < 4u; ++i)
        frame[i] = sync[i];
    for (unsigned int i = 0; i < 6u; ++i)
        frame[4u + i] = (uint8_t)gEeprom.AX25_CALLSIGN[i];
    frame[10] = (uint8_t)('0' + (gEeprom.AX25_SSID % 10u));

    /* CRC-16/ARC over the seven-byte callsign + SSID payload. */
    for (unsigned int i = 4u; i < 11u; ++i) {
        crc ^= frame[i];
        for (unsigned int b = 0; b < 8u; ++b)
            crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0xA001u)
                             : (uint16_t)(crc >> 1);
    }
    frame[11] = (uint8_t)(crc & 0xFFu);
    frame[12] = (uint8_t)(crc >> 8);

    saved_reg30 = BK4819_ReadRegister(BK4819_REG_30);
    BK4819_EnterTxMute();
    BK4819_WriteRegister(BK4819_REG_70,
                         BK4819_REG_70_ENABLE_TONE1 |
                         (96u << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));
    BK4819_WriteRegister(BK4819_REG_71, mark);
    BK4819_EnableTXLink();
    BK4819_ExitTxMute();

    __disable_irq();
    last = SysTick->VAL;

#define BEACON_WAIT_SYMBOL() do { \
        for (;;) { \
            const uint32_t now = SysTick->VAL; \
            const uint32_t d = (last >= now) ? (last - now) : (last + reload - now); \
            last = now; acc_q8 += d << 8; \
            if (acc_q8 >= bit_ticks_q8) { acc_q8 -= bit_ticks_q8; break; } \
        } \
    } while (0)
#define BEACON_SEND_SYMBOL(v) do { \
        const uint8_t symbol_ = (uint8_t)(v); \
        if (symbol_ != tone_mark) { \
            BK4819_WriteRegister(BK4819_REG_71, symbol_ ? mark : space); \
            tone_mark = symbol_; \
        } \
        BEACON_WAIT_SYMBOL(); \
    } while (0)
#define BEACON_SEND_BYTE(v) do { \
        const uint8_t byte_ = (uint8_t)(v); \
        for (unsigned int bit_ = 0; bit_ < 8u; ++bit_) \
            BEACON_SEND_SYMBOL((byte_ >> bit_) & 1u); \
    } while (0)

    /* Keep the existing warble pattern and configured cycle count unchanged. */
    tone_mark = 2u;
    if (with_preamble && gEeprom.MDC1200_PREAMBLE_DURATION > 1u) {
        for (unsigned int cycle = 0; cycle < gEeprom.MDC1200_PREAMBLE_DURATION; ++cycle) {
            for (unsigned int p = 0; p < 33u; ++p) BEACON_SEND_SYMBOL(0u);
            for (unsigned int p = 0; p < 20u; ++p) BEACON_SEND_SYMBOL(1u);
        }
    } else {
        for (unsigned int p = 0; p < 20u; ++p) BEACON_SEND_SYMBOL(0u);
        for (unsigned int p = 0; p < 33u; ++p) BEACON_SEND_SYMBOL(1u);
    }

    for (unsigned int i = 0; i < sizeof(frame); ++i)
        BEACON_SEND_BYTE(frame[i]);

#undef BEACON_SEND_BYTE
#undef BEACON_SEND_SYMBOL
#undef BEACON_WAIT_SYMBOL

    if (!primask) __enable_irq();

    BK4819_EnterTxMute();
    BK4819_WriteRegister(BK4819_REG_70, 0u);
    BK4819_WriteRegister(BK4819_REG_30, saved_reg30);
    BK4819_ExitTxMute();
}

void BK4819_PlayRoger(void) {
    if (gEeprom.PTT_ID == PTT_ID_MODE_AX25_POST ||
        gEeprom.PTT_ID == PTT_ID_MODE_AX25_BOTH) {
        BK4819_MuteMic();
        SYSTEM_DelayMs(50);
#ifdef ENABLE_MDC1200
        BK4819_SendCallsignBeacon(gEeprom.MDC1200_PREAMBLE_WHEN != MDC_PREAMBLE_WHEN_PRE);
#else
        BK4819_SendCallsignBeacon(false);
#endif
    }
#ifdef ENABLE_MDC1200
    if (BK4819_HasPostIDPTTID()) {
        BK4819_MuteMic();
        SYSTEM_DelayMs(50);
        SYSTEM_DelayMs(BK4819_SendPostPTTID() ? 40 : 20);
    }
#endif
}

int8_t BK4819_GetRxGain_dB(void) {
    union {
        struct {
            uint16_t pga: 3;
            uint16_t mixer: 2;
            uint16_t lna: 3;
            uint16_t lnaS: 2;
        };
        uint16_t __raw;
    } agcGainReg;

    union {
        struct {
            uint16_t _: 5;
            uint16_t agcSigStrength: 7;
            int16_t gainIdx: 3;
            uint16_t agcEnab: 1;
        };
        uint16_t __raw;
    } reg7e;

    reg7e.__raw = BK4819_ReadRegister(BK4819_REG_7E);
    uint8_t gainAddr = reg7e.gainIdx < 0 ? BK4819_REG_14 : BK4819_REG_10 + reg7e.gainIdx;
    agcGainReg.__raw = BK4819_ReadRegister(gainAddr);
    int8_t lnaShortTab[] = {-28, -24, -19, 0};
    int8_t lnaTab[] = {-24, -19, -14, -9, -6, -4, -2, 0};
    int8_t mixerTab[] = {-8, -6, -3, 0};
    int8_t pgaTab[] = {-33, -27, -21, -15, -9, -6, -3, 0};
    return lnaShortTab[agcGainReg.lnaS] + lnaTab[agcGainReg.lna] + mixerTab[agcGainReg.mixer] + pgaTab[agcGainReg.pga];
}

int16_t BK4819_GetRSSI_dBm(void) {
    uint16_t rssi = BK4819_GetRSSI();
    return (rssi / 2) - 160;// - BK4819_GetRxGain_dB();
}

void BK4819_ToggleGpioOut(BK4819_GPIO_PIN_t Pin, bool bSet) {
    if (bSet)
        gBK4819_GpioOutState |= (0x40u >> Pin);
    else
        gBK4819_GpioOutState &= ~(0x40u >> Pin);

    BK4819_WriteRegister(BK4819_REG_33, gBK4819_GpioOutState);
}

void BK4819_SetCDCSSCodeWord(uint32_t CodeWord) {
    // REG_51
    //
    // <15>  0
    //       1 = Enable TxCTCSS/CDCSS
    //       0 = Disable
    //
    // <14>  0
    //       1 = GPIO0Input for CDCSS
    //       0 = Normal Mode (for BK4819 v3)
    //
    // <13>  0
    //       1 = Transmit negative CDCSS code
    //       0 = Transmit positive CDCSS code
    //
    // <12>  0 CTCSS/CDCSS mode selection
    //       1 = CTCSS
    //       0 = CDCSS
    //
    // <11>  0 CDCSS 24/23bit selection
    //       1 = 24bit
    //       0 = 23bit
    //
    // <10>  0 1050HzDetectionMode
    //       1 = 1050/4 Detect Enable, CTC1 should be set to 1050/4 Hz
    //
    // <9>   0 Auto CDCSS Bw Mode
    //       1 = Disable
    //       0 = Enable
    //
    // <8>   0 Auto CTCSS Bw Mode
    //       0 = Enable
    //       1 = Disable
    //
    // <6:0> 0 CTCSS/CDCSS Tx Gain1 Tuning
    //       0   = min
    //       127 = max

    // Enable CDCSS
    // Transmit positive CDCSS code
    // CDCSS Mode
    // CDCSS 23bit
    // Enable Auto CDCSS Bw Mode
    // Enable Auto CTCSS Bw Mode
    // CTCSS/CDCSS Tx Gain1 Tuning = 51
    //
    BK4819_WriteRegister(BK4819_REG_51,
                         BK4819_REG_51_ENABLE_CxCSS |
                         BK4819_REG_51_GPIO6_PIN2_NORMAL |
                         BK4819_REG_51_TX_CDCSS_POSITIVE |
                         BK4819_REG_51_MODE_CDCSS |
                         BK4819_REG_51_CDCSS_23_BIT |
                         BK4819_REG_51_1050HZ_NO_DETECTION |
                         BK4819_REG_51_AUTO_CDCSS_BW_ENABLE |
                         BK4819_REG_51_AUTO_CTCSS_BW_ENABLE |
                         (51u << BK4819_REG_51_SHIFT_CxCSS_TX_GAIN1));

    // REG_07 <15:0>
    //
    // When <13> = 0 for CTC1
    // <12:0> = CTC1 frequency control word =
    //                          freq(Hz) * 20.64888 for XTAL 13M/26M or
    //                          freq(Hz) * 20.97152 for XTAL 12.8M/19.2M/25.6M/38.4M
    //
    // When <13> = 1 for CTC2 (Tail 55Hz Rx detection)
    // <12:0> = CTC2 (should below 100Hz) frequency control word =
    //                          25391 / freq(Hz) for XTAL 13M/26M or
    //                          25000 / freq(Hz) for XTAL 12.8M/19.2M/25.6M/38.4M
    //
    // When <13> = 2 for CDCSS 134.4Hz
    // <12:0> = CDCSS baud rate frequency (134.4Hz) control word =
    //                          freq(Hz) * 20.64888 for XTAL 13M/26M or
    //                          freq(Hz) * 20.97152 for XTAL 12.8M/19.2M/25.6M/38.4M
    //
    BK4819_WriteRegister(BK4819_REG_07, BK4819_REG_07_MODE_CTC1 | 2775u);

    // REG_08 <15:0> <15> = 1 for CDCSS high 12bit
    //               <15> = 0 for CDCSS low  12bit
    // <11:0> = CDCSShigh/low 12bit code
    //
    BK4819_WriteRegister(BK4819_REG_08, (0u << 15) | ((CodeWord >> 0) & 0x0FFF)); // LS 12-bits
    BK4819_WriteRegister(BK4819_REG_08, (1u << 15) | ((CodeWord >> 12) & 0x0FFF)); // MS 12-bits
}

void BK4819_SetCTCSSFrequency(uint32_t FreqControlWord) {
    // REG_51 <15>  0                                 1 = Enable TxCTCSS/CDCSS           0 = Disable
    // REG_51 <14>  0                                 1 = GPIO0Input for CDCSS           0 = Normal Mode.(for BK4819v3)
    // REG_51 <13>  0                                 1 = Transmit negative CDCSS code   0 = Transmit positive CDCSScode
    // REG_51 <12>  0 CTCSS/CDCSS mode selection      1 = CTCSS                          0 = CDCSS
    // REG_51 <11>  0 CDCSS 24/23bit selection        1 = 24bit                          0 = 23bit
    // REG_51 <10>  0 1050HzDetectionMode             1 = 1050/4 Detect Enable, CTC1 should be set to 1050/4 Hz
    // REG_51 <9>   0 Auto CDCSS Bw Mode              1 = Disable                        0 = Enable.
    // REG_51 <8>   0 Auto CTCSS Bw Mode              0 = Enable                         1 = Disable
    // REG_51 <6:0> 0 CTCSS/CDCSS Tx Gain1 Tuning     0 = min                            127 = max

    uint16_t Config;
    if (FreqControlWord == 2625) {    // Enables 1050Hz detection mode
        // Enable TxCTCSS
        // CTCSS Mode
        // 1050/4 Detect Enable
        // Enable Auto CDCSS Bw Mode
        // Enable Auto CTCSS Bw Mode
        // CTCSS/CDCSS Tx Gain1 Tuning = 74
        //
        Config = 0x944A;   // 1 0 0 1 0 1 0 0 0 1001010
    } else {    // Enable TxCTCSS
        // CTCSS Mode
        // Enable Auto CDCSS Bw Mode
        // Enable Auto CTCSS Bw Mode
        // CTCSS/CDCSS Tx Gain1 Tuning = 74
        //亚音
//        Config = 0x904A;   // 1 0 0 1 0 0 0 0 0 1001010
        Config = 0x9033;
    }
    BK4819_WriteRegister(BK4819_REG_51, Config);

    // REG_07 <15:0>
    //
    // When <13> = 0 for CTC1
    // <12:0> = CTC1 frequency control word =
    //                          freq(Hz) * 20.64888 for XTAL 13M/26M or
    //                          freq(Hz) * 20.97152 for XTAL 12.8M/19.2M/25.6M/38.4M
    //
    // When <13> = 1 for CTC2 (Tail RX detection)
    // <12:0> = CTC2 (should below 100Hz) frequency control word =
    //                          25391 / freq(Hz) for XTAL 13M/26M or
    //                          25000 / freq(Hz) for XTAL 12.8M/19.2M/25.6M/38.4M
    //
    // When <13> = 2 for CDCSS 134.4Hz
    // <12:0> = CDCSS baud rate frequency (134.4Hz) control word =
    //                          freq(Hz) * 20.64888 for XTAL 13M/26M or
    //                          freq(Hz) * 20.97152 for XTAL 12.8M/19.2M/25.6M/38.4M
    //
    BK4819_WriteRegister(BK4819_REG_07, BK4819_REG_07_MODE_CTC1 |
                                        (((FreqControlWord * 206488u) + 50000u) / 100000u));   // with rounding
}

// freq_10Hz is CTCSS Hz * 10
void BK4819_SetTailDetection(const uint32_t freq_10Hz) {
    // REG_07 <15:0>
    //
    // When <13> = 0 for CTC1
    // <12:0> = CTC1 frequency control word =
    //                          freq(Hz) * 20.64888 for XTAL 13M/26M or
    //                          freq(Hz) * 20.97152 for XTAL 12.8M/19.2M/25.6M/38.4M
    //
    // When <13> = 1 for CTC2 (Tail RX detection)
    // <12:0> = CTC2 (should below 100Hz) frequency control word =
    //                          25391 / freq(Hz) for XTAL 13M/26M or
    //                          25000 / freq(Hz) for XTAL 12.8M/19.2M/25.6M/38.4M
    //
    // When <13> = 2 for CDCSS 134.4Hz
    // <12:0> = CDCSS baud rate frequency (134.4Hz) control word =
    //                          freq(Hz) * 20.64888 for XTAL 13M/26M or
    //                          freq(Hz) * 20.97152 for XTAL 12.8M/19.2M/25.6M/38.4M
    //
    BK4819_WriteRegister(BK4819_REG_07,
                         BK4819_REG_07_MODE_CTC2 | ((253910 + (freq_10Hz / 2)) / freq_10Hz));  // with rounding
}

void BK4819_EnableVox(uint16_t VoxEnableThreshold, uint16_t VoxDisableThreshold) {
    //VOX Algorithm
    //if (voxamp>VoxEnableThreshold)                VOX = 1;
    //else
    //if (voxamp<VoxDisableThreshold) (After Delay) VOX = 0;

    const uint16_t REG_31_Value = BK4819_ReadRegister(BK4819_REG_31);

    // 0xA000 is undocumented?
    BK4819_WriteRegister(BK4819_REG_46, 0xA000 | (VoxEnableThreshold & 0x07FF));

    // 0x1800 is undocumented?
    BK4819_WriteRegister(BK4819_REG_79, 0x1800 | (VoxDisableThreshold & 0x07FF));

    // Bottom 12 bits are undocumented, 15:12 vox disable delay *128ms
    BK4819_WriteRegister(BK4819_REG_7A, 0x289A); // vox disable delay = 128*5 = 640ms

    // Enable VOX
    BK4819_WriteRegister(BK4819_REG_31, REG_31_Value | (1u << 2));    // VOX Enable
}

void BK4819_SetFilterBandwidth(const BK4819_FilterBandwidth_t Bandwidth, const bool weak_no_different) {
    // REG_43
    // <15>    0 ???
    //
    // <14:12> 4 RF filter bandwidth
    //         0 = 1.7  kHz
    //         1 = 2.0  kHz
    //         2 = 2.5  kHz
    //         3 = 3.0  kHz
    //         4 = 3.75 kHz
    //         5 = 4.0  kHz
    //         6 = 4.25 kHz
    //         7 = 4.5  kHz
    // if <5> == 1, RF filter bandwidth * 2
    //
    // <11:9>  0 RF filter bandwidth when signal is weak
    //         0 = 1.7  kHz
    //         1 = 2.0  kHz
    //         2 = 2.5  kHz
    //         3 = 3.0  kHz
    //         4 = 3.75 kHz
    //         5 = 4.0  kHz
    //         6 = 4.25 kHz
    //         7 = 4.5  kHz
    // if <5> == 1, RF filter bandwidth * 2
    //
    // <8:6>   1 AFTxLPF2 filter Band Width
    //         1 = 2.5  kHz (for 12.5k channel space)
    //         2 = 2.75 kHz
    //         0 = 3.0  kHz (for 25k   channel space)
    //         3 = 3.5  kHz
    //         4 = 4.5  kHz
    //         5 = 4.25 kHz
    //         6 = 4.0  kHz
    //         7 = 3.75 kHz
    //
    // <5:4>   0 BW Mode Selection
    //         0 = 12.5k
    //         1 =  6.25k
    //         2 = 25k/20k
    //
    // <3>     1 ???
    //
    // <2>     0 Gain after FM Demodulation
    //         0 = 0dB
    //         1 = 6dB
    //
    // <1:0>   0 ???

    uint16_t val;
    m_bandwidth = Bandwidth;

    switch (Bandwidth) {
        default:
        case BK4819_FILTER_BW_WIDE:    // 25kHz
            if (weak_no_different) {    // make the RX bandwidth the same with weak signals
                val =
                        (0u << 15) |     //  0
                        (4u << 12) |     // *3 RF filter bandwidth
                        (4u << 9) |     // *0 RF filter bandwidth when signal is weak
                        (6u << 6) |     // *0 AFTxLPF2 filter Band Width
                        (2u << 4) |     //  2 BW Mode Selection
                        (1u << 3) |     //  1
                        (0u << 2) |     //  0 Gain after FM Demodulation
                        (0u << 0);      //  0
            } else {    // with weak RX signals the RX bandwidth is reduced
                val =                // 0x3028);         // 0 011 000 000 10 1 0 00
                        (0u << 15) |     //  0
                        (4u << 12) |     // *3 RF filter bandwidth
                        (2u << 9) |     // *0 RF filter bandwidth when signal is weak
                        (6u << 6) |     // *0 AFTxLPF2 filter Band Width
                        (2u << 4) |     //  2 BW Mode Selection
                        (1u << 3) |     //  1
                        (0u << 2) |     //  0 Gain after FM Demodulation
                        (0u << 0);      //  0
            }
            break;

        case BK4819_FILTER_BW_NARROW:    // 12.5kHz
            if (weak_no_different) {
                val =
                        (0u << 15) |     //  0
                        (4u << 12) |     // *4 RF filter bandwidth
                        (4u << 9) |     // *0 RF filter bandwidth when signal is weak
                        (0u << 6) |     // *1 AFTxLPF2 filter Band Width
                        (0u << 4) |     //  0 BW Mode Selection
                        (1u << 3) |     //  1
                        (0u << 2) |     //  0 Gain after FM Demodulation
                        (0u << 0);      //  0
            } else {
                val =                // 0x4048);        // 0 100 000 001 00 1 0 00
                        (0u << 15) |     //  0
                        (4u << 12) |     // *4 RF filter bandwidth
                        (2u << 9) |     // *0 RF filter bandwidth when signal is weak
                        (0u << 6) |     // *1 AFTxLPF2 filter Band Width
                        (0u << 4) |     //  0 BW Mode Selection
                        (1u << 3) |     //  1
                        (0u << 2) |     //  0 Gain after FM Demodulation
                        (0u << 0);      //  0
            }
            break;

        case BK4819_FILTER_BW_NARROWER:    // 6.25kHz
            if (weak_no_different) {
                val =
                        (0u << 15) |     //  0
                        (3u << 12) |     //  3 RF filter bandwidth
                        (3u << 9) |     // *0 RF filter bandwidth when signal is weak
                        (1u << 6) |     //  1 AFTxLPF2 filter Band Width
                        (1u << 4) |     //  1 BW Mode Selection
                        (1u << 3) |     //  1
                        (0u << 2) |     //  0 Gain after FM Demodulation
                        (0u << 0);      //  0
            } else {
                val =
                        (0u << 15) |     //  0
                        (3u << 12) |     //  3 RF filter bandwidth
                        (0u << 9) |     //  0 RF filter bandwidth when signal is weak
                        (1u << 6) |     //  1 AFTxLPF2 filter Band Width
                        (1u << 4) |     //  1 BW Mode Selection
                        (1u << 3) |     //  1
                        (0u << 2) |     //  1 Gain after FM Demodulation
                        (0u << 0);      //  0
            }
            break;

        case BK4819_FILTER_BW_AM:
            // Use the 2.0 kHz AM receive filter for both normal and weak signals.
            val =
                    (0u << 15) |
                    (1u << 12) |
                    (1u << 9) |
                    (3u << 6) |
                    (0u << 4) |
                    (1u << 3) |
                    (0u << 2) |
                    (0u << 0);
            break;
    }

    BK4819_WriteRegister(BK4819_REG_43, val);
}

void BK4819_SetupPowerAmplifier(const uint8_t bias, const uint32_t frequency) {
    // REG_36 <15:8> 0 PA Bias output 0 ~ 3.2V
    //               255 = 3.2V
    //                 0 = 0V
    //
    // REG_36 <7>    0
    //               1 = Enable PA-CTL output
    //               0 = Disable (Output 0 V)
    //
    // REG_36 <5:3>  7 PA gain 1 tuning
    //               7 = max
    //               0 = min
    //
    // REG_36 <2:0>  7 PA gain 2 tuning
    //               7 = max
    //               0 = min
    //
    //                                  280MHz       gain 1 = 1  gain 2 = 0  gain 1 = 4  gain 2 = 2
    const uint8_t gain = (frequency < 28000000) ? (1u << 3) | (0u << 0) : (4u << 3) | (2u << 0);
    const uint8_t enable = 1;
    BK4819_WriteRegister(BK4819_REG_36, (bias << 8) | (enable << 7) | (gain << 0));
}

void BK4819_SetFrequency(uint32_t Frequency) {
    BK4819_WriteRegister(BK4819_REG_38, (Frequency >> 0) & 0xFFFF);
    BK4819_WriteRegister(BK4819_REG_39, (Frequency >> 16) & 0xFFFF);
}

void BK4819_SetupSquelch(
        uint8_t SquelchOpenRSSIThresh,
        uint8_t SquelchCloseRSSIThresh,
        uint8_t SquelchOpenNoiseThresh,
        uint8_t SquelchCloseNoiseThresh,
        uint8_t SquelchCloseGlitchThresh,
        uint8_t SquelchOpenGlitchThresh) {
    // REG_70
    //
    // <15>   0 Enable TONE1
    //        1 = Enable
    //        0 = Disable
    //
    // <14:8> 0 TONE1 tuning gain
    //        0 ~ 127
    //
    // <7>    0 Enable TONE2
    //        1 = Enable
    //        0 = Disable
    //
    // <6:0>  0 TONE2/FSK tuning gain
    //        0 ~ 127
    //
    BK4819_WriteRegister(BK4819_REG_70, 0);

    // Glitch threshold for Squelch = close
    //
    // 0 ~ 255
    //
    BK4819_WriteRegister(BK4819_REG_4D, 0xA000 | SquelchCloseGlitchThresh);

    // REG_4E
    //
    // <15:14> 1 ???
    //
    // <13:11> Squelch = open  Delay Setting
    //         0 ~ 7  (0 = fastest open)
    //
    // <10:9>  Squelch = close Delay Setting
    //         0 ~ 3  (0 = shortest tail)
    //
    // <8>     0 ???
    //
    // <7:0>   Glitch threshold for Squelch = open
    //         0 ~ 255
    //
    BK4819_WriteRegister(BK4819_REG_4E,
                         (1u << 14) |
                         (5u << 11) | //opendelay
                         (6u << 9) | //closedelay
                         SquelchOpenGlitchThresh);

    // REG_4F
    //
    // <14:8> 47 Ex-noise threshold for Squelch = close
    //        0 ~ 127
    //
    // <7>    ???
    //
    // <6:0>  46 Ex-noise threshold for Squelch = open
    //        0 ~ 127
    //
    BK4819_WriteRegister(BK4819_REG_4F, ((uint16_t) SquelchCloseNoiseThresh << 8) | SquelchOpenNoiseThresh);

    // REG_78
    //
    // <15:8> 72 RSSI threshold for Squelch = open    0.5dB/step
    //
    // <7:0>  70 RSSI threshold for Squelch = close   0.5dB/step
    //
    BK4819_WriteRegister(BK4819_REG_78, ((uint16_t) SquelchOpenRSSIThresh << 8) | SquelchCloseRSSIThresh);

    BK4819_SetAF(BK4819_AF_MUTE);

    BK4819_RX_TurnOn();
}

void BK4819_SetAF(BK4819_AF_Type_t AF) {
    // AF Output Inverse Mode = Inverse
    // Undocumented bits 0x2040
    //
//	BK4819_WriteRegister(BK4819_REG_47, 0x6040 | (AF << 8));
    BK4819_WriteRegister(BK4819_REG_47, (6u << 12) | (AF << 8) | (1u << 6));
}

void BK4819_SetRegValue(RegisterSpec s, uint16_t v) {
    uint16_t reg = BK4819_ReadRegister(s.num);
    reg &= ~(s.mask << s.offset);
    BK4819_WriteRegister(s.num, reg | (v << s.offset));
}

void BK4819_RX_TurnOn(void) {
    // DSP Voltage Setting = 1
    // ANA LDO = 2.7v
    // VCO LDO = 2.7v
    // RF LDO  = 2.7v
    // PLL LDO = 2.7v
    // ANA LDO bypass
    // VCO LDO bypass
    // RF LDO  bypass
    // PLL LDO bypass
    // Reserved bit is 1 instead of 0
    // Enable  DSP
    // Enable  XTAL
    // Enable  Band Gap
    //
    BK4819_WriteRegister(BK4819_REG_37, 0x1F0F);  // 0001111100001111

    // Turn off everything
    BK4819_WriteRegister(BK4819_REG_30, 0);


    BK4819_WriteRegister(BK4819_REG_30,
                         BK4819_REG_30_ENABLE_VCO_CALIB |
                         BK4819_REG_30_DISABLE_UNKNOWN |
                         BK4819_REG_30_ENABLE_RX_LINK |
                         BK4819_REG_30_ENABLE_AF_DAC |
                         BK4819_REG_30_ENABLE_DISC_MODE |
                         BK4819_REG_30_ENABLE_PLL_VCO |
                         BK4819_REG_30_DISABLE_PA_GAIN |
                         BK4819_REG_30_DISABLE_MIC_ADC |
                         BK4819_REG_30_DISABLE_TX_DSP |
                         BK4819_REG_30_ENABLE_RX_DSP);
}

void BK4819_PickRXFilterPathBasedOnFrequency(uint32_t Frequency) {
    if (Frequency < 28000000) {    // VHF
        BK4819_ToggleGpioOut(BK4819_GPIO4_PIN32_VHF_LNA, true);
        BK4819_ToggleGpioOut(BK4819_GPIO3_PIN31_UHF_LNA, false);
    } else if (Frequency == 0xFFFFFFFF) {    // OFF
        BK4819_ToggleGpioOut(BK4819_GPIO4_PIN32_VHF_LNA, false);
        BK4819_ToggleGpioOut(BK4819_GPIO3_PIN31_UHF_LNA, false);
    } else {    // UHF
        BK4819_ToggleGpioOut(BK4819_GPIO4_PIN32_VHF_LNA, false);
        BK4819_ToggleGpioOut(BK4819_GPIO3_PIN31_UHF_LNA, true);
    }
}


bool BK4819_CompanderEnabled(void) {
    return (BK4819_ReadRegister(BK4819_REG_31) & (1u << 3)) ? true : false;
}

void BK4819_SetCompander(const unsigned int mode) {
    // mode 0 .. OFF
    // mode 1 .. TX
    // mode 2 .. RX
    // mode 3 .. TX and RX

    const uint16_t r31 = BK4819_ReadRegister(BK4819_REG_31);

    if (mode == 0) {    // disable
        BK4819_WriteRegister(BK4819_REG_31, r31 & ~(1u << 3));
        return;
    }

    // REG_29
    //
    // <15:14> 10 Compress (AF Tx) Ratio
    //         00 = Disable
    //         01 = 1.333:1
    //         10 = 2:1
    //         11 = 4:1
    //
    // <13:7>  86 Compress (AF Tx) 0 dB point (dB)
    //
    // <6:0>   64 Compress (AF Tx) noise point (dB)
    //
    const uint16_t compress_ratio = (mode == 1 || mode >= 3) ? 2 : 0;  // 2:1
    const uint16_t compress_0dB = 86;
    const uint16_t compress_noise_dB = 64;
//	AB40  10 1010110 1000000
    BK4819_WriteRegister(BK4819_REG_29, // (BK4819_ReadRegister(BK4819_REG_29) & ~(3u << 14)) | (compress_ratio << 14));
                         (compress_ratio << 14) |
                         (compress_0dB << 7) |
                         (compress_noise_dB << 0));

    // REG_28
    //
    // <15:14> 01 Expander (AF Rx) Ratio
    //         00 = Disable
    //         01 = 1:2
    //         10 = 1:3
    //         11 = 1:4
    //
    // <13:7>  86 Expander (AF Rx) 0 dB point (dB)
    //
    // <6:0>   56 Expander (AF Rx) noise point (dB)
    //
    const uint16_t expand_ratio = (mode >= 2) ? 1 : 0;   // 1:2
    const uint16_t expand_0dB = 86;
    const uint16_t expand_noise_dB = 56;
//	6B38  01 1010110 0111000
    BK4819_WriteRegister(BK4819_REG_28, // (BK4819_ReadRegister(BK4819_REG_28) & ~(3u << 14)) | (expand_ratio << 14));
                         (expand_ratio << 14) |
                         (expand_0dB << 7) |
                         (expand_noise_dB << 0));

    // enable
    BK4819_WriteRegister(BK4819_REG_31, r31 | (1u << 3));
}

void BK4819_DisableVox(void) {
    const uint16_t Value = BK4819_ReadRegister(BK4819_REG_31);
    BK4819_WriteRegister(BK4819_REG_31, Value & 0xFFFB);
}

void BK4819_DisableDTMF(void) {
    BK4819_WriteRegister(BK4819_REG_24, 0);
}

void BK4819_EnableDTMF(void) {
    // no idea what this does
    BK4819_WriteRegister(BK4819_REG_21, 0x06D8);        // 0000 0110 1101 1000

    // REG_24
    //
    // <15>   1  ???
    //
    // <14:7> 24 Threshold
    //
    // <6>    1  ???
    //
    // <5>    0  DTMF/SelCall enable
    //        1 = Enable
    //        0 = Disable
    //
    // <4>    1  DTMF or SelCall detection mode
    //        1 = for DTMF
    //        0 = for SelCall
    //
    // <3:0>  14 Max symbol number for SelCall detection
    //
//	const uint16_t threshold = 24;    // default, but doesn't decode non-QS radios
    const uint16_t threshold = 130;   // but 128 ~ 247 does
    BK4819_WriteRegister(BK4819_REG_24,                      // 1 00011000 1 1 1 1110
                         (1u << BK4819_REG_24_SHIFT_UNKNOWN_15) |
                         (threshold << BK4819_REG_24_SHIFT_THRESHOLD) |      // 0 ~ 255
                         (1u << BK4819_REG_24_SHIFT_UNKNOWN_6) |
                         BK4819_REG_24_ENABLE |
                         BK4819_REG_24_SELECT_DTMF |
                         (15u << BK4819_REG_24_SHIFT_MAX_SYMBOLS));     // 0 ~ 15
}

void BK4819_PlayTone(uint16_t Frequency, bool bTuningGainSwitch) {
    uint16_t ToneConfig = BK4819_REG_70_ENABLE_TONE1;

    BK4819_EnterTxMute();
    BK4819_SetAF(BK4819_AF_BEEP);

    if (bTuningGainSwitch == 0)
        ToneConfig |= 96u << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN;
    else
        ToneConfig |= 28u << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN;
    BK4819_WriteRegister(BK4819_REG_70, ToneConfig);

    BK4819_WriteRegister(BK4819_REG_30, 0);
    BK4819_WriteRegister(BK4819_REG_30,
                         BK4819_REG_30_ENABLE_AF_DAC | BK4819_REG_30_ENABLE_DISC_MODE | BK4819_REG_30_ENABLE_TX_DSP);

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(Frequency));
}

// level 0 ~ 127
void BK4819_PlaySingleTone(const unsigned int tone_Hz, const unsigned int delay, const unsigned int level,
                           const bool play_speaker) {
    BK4819_EnterTxMute();

    if (play_speaker) {
        AUDIO_AudioPathOn();
        BK4819_SetAF(BK4819_AF_BEEP);
    } else
        BK4819_SetAF(BK4819_AF_MUTE);


    BK4819_WriteRegister(BK4819_REG_70,
                         BK4819_REG_70_ENABLE_TONE1 | ((level & 0x7f) << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));

    if (play_speaker)
        BK4819_Enable_AfDac_DiscMode_TxDsp();
    else
        BK4819_EnableTXLink();
    SYSTEM_DelayMs(50);

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone_Hz));

    BK4819_ExitTxMute();
    SYSTEM_DelayMs(delay);
    BK4819_EnterTxMute();

    if (play_speaker) {
        AUDIO_AudioPathOff();
        BK4819_SetAF(BK4819_AF_MUTE);
    }

    BK4819_WriteRegister(BK4819_REG_70, 0x0000);
    if (play_speaker)
        BK4819_TurnsOffTones_TurnsOnRX();
    else {
        BK4819_WriteRegister(BK4819_REG_30, 0xC1FE);
        BK4819_ExitTxMute();
    }
}

void BK4819_PlayToneSequence(const uint16_t *frequencies, const uint16_t *durations, unsigned int count,
                             uint8_t toneGain) {
    BK4819_EnterTxMute();
    AUDIO_AudioPathOn();
    BK4819_SetAF(BK4819_AF_BEEP);
    BK4819_Enable_AfDac_DiscMode_TxDsp();

    for (unsigned int i = 0; i < count; i++) {
        if (frequencies[i] != 0) {
            BK4819_WriteRegister(BK4819_REG_70,
                                 BK4819_REG_70_ENABLE_TONE1 |
                                     (toneGain << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));
            BK4819_WriteRegister(BK4819_REG_71, scale_freq(frequencies[i]));
            BK4819_ExitTxMute();
        } else {
            BK4819_WriteRegister(BK4819_REG_70, 0);
        }

        SYSTEM_DelayMs(durations[i]);
        BK4819_EnterTxMute();
    }

    AUDIO_AudioPathOff();
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_WriteRegister(BK4819_REG_70, 0);
    BK4819_TurnsOffTones_TurnsOnRX();
}

//void BK4819_PlaySquelchTailBeep(void) {
//    BK4819_EnterTxMute();
//    AUDIO_AudioPathOn();
//    BK4819_SetAF(BK4819_AF_BEEP);
//    BK4819_Enable_AfDac_DiscMode_TxDsp();
//    BK4819_WriteRegister(BK4819_REG_70,
//                         BK4819_REG_70_ENABLE_TONE1 |
//                         (55u << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));
//    BK4819_WriteRegister(BK4819_REG_71, scale_freq(1397));
//    BK4819_ExitTxMute();
//    SYSTEM_DelayMs(75);
//    BK4819_EnterTxMute();
//    AUDIO_AudioPathOff();
//    BK4819_SetAF(BK4819_AF_MUTE);
//    BK4819_WriteRegister(BK4819_REG_70, 0);
//    BK4819_TurnsOffTones_TurnsOnRX();
//}


void BK4819_ResetTalkPermitToneState(void) {
    gKenwSecondaryTone = false;
}

void BK4819_MarkTalkPermitToneRx(void) {
    gKenwSecondaryTone = true;
}

void BK4819_PlayTalkPermitTone(uint8_t mode) {
    static const uint16_t xtsFrequencies[] = {910, 0, 910, 0, 910};
    static const uint16_t xtsDurations[] = {100, 20, 30, 20, 60};
    static const uint16_t trboFrequencies[] = {1570, 1050, 1570, 1317};
    static const uint16_t trboDurations[] = {100, 40, 40, 40};
    static const uint16_t fleetSyncFrequencies[] = {1215, 1629, 1215, 1629};
    static const uint16_t fleetSyncDurations[] = {100, 30, 30, 30};
    const uint16_t *frequencies;
    const uint16_t *durations;
    unsigned int count;
    uint8_t selectedMode = mode;

    if (selectedMode == TALK_PERMIT_TONE_AUTO) {
        const bool hasSignalling = (gEeprom.PTT_ID == PTT_ID_MODE_MDC_PRE ||
                                   gEeprom.PTT_ID == PTT_ID_MODE_MDC_POST ||
                                   gEeprom.PTT_ID == PTT_ID_MODE_MDC_BOTH ||
                                   gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_PRE ||
                                   gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_POST ||
                                   gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_BOTH);

        if (!hasSignalling) {
            selectedMode = TALK_PERMIT_TONE_XTS;
        } else if (gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_PRE ||
                   gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_POST ||
                   gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_BOTH) {
            selectedMode = TALK_PERMIT_TONE_KENW;
        } else {
            selectedMode = TALK_PERMIT_TONE_TRBO;
        }
    }

    if (selectedMode == TALK_PERMIT_TONE_XTS) {
        frequencies = xtsFrequencies;
        durations = xtsDurations;
        count = 5;
    } else if (selectedMode == TALK_PERMIT_TONE_TRBO) {
        frequencies = trboFrequencies;
        durations = trboDurations;
        count = 4;
    } else if (selectedMode == TALK_PERMIT_TONE_KENW) {
        frequencies = fleetSyncFrequencies;
        durations = fleetSyncDurations;
        count = 4;
    } else {
        return;
    }

    BK4819_PlayToneSequence(frequencies, durations, count, 55);

    if (mode == TALK_PERMIT_TONE_KENW)
        gKenwSecondaryTone = true;
}

void BK4819_PlayTalkPermitToneTx(uint8_t mode, bool delay_speaker_open) {
    static const uint16_t xtsFrequencies[] = {910, 0, 910, 0, 910};
    static const uint16_t xtsDurations[] = {100, 20, 30, 20, 60};
    static const uint16_t trboFrequencies[] = {1570, 1050, 1570, 1317};
    static const uint16_t trboDurations[] = {100, 40, 40, 40};
    static const uint16_t fleetSyncFrequencies[] = {1215, 1629, 1215, 1629};
    static const uint16_t fleetSyncDurations[] = {100, 30, 30, 30};
    const uint16_t *frequencies;
    const uint16_t *durations;
    uint8_t toneGain = 55;
    unsigned int count;
    uint8_t selectedMode = mode;

    if (selectedMode == TALK_PERMIT_TONE_AUTO) {
        const bool hasSignalling = (gEeprom.PTT_ID == PTT_ID_MODE_MDC_PRE ||
                                   gEeprom.PTT_ID == PTT_ID_MODE_MDC_POST ||
                                   gEeprom.PTT_ID == PTT_ID_MODE_MDC_BOTH ||
                                   gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_PRE ||
                                   gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_POST ||
                                   gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_BOTH);

        if (!hasSignalling) {
            selectedMode = TALK_PERMIT_TONE_XTS;
        } else if (gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_PRE ||
                   gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_POST ||
                   gEeprom.PTT_ID == PTT_ID_MODE_FLEETSYNC_BOTH) {
            selectedMode = TALK_PERMIT_TONE_KENW;
        } else {
            selectedMode = TALK_PERMIT_TONE_TRBO;
        }
    }

    if (selectedMode == TALK_PERMIT_TONE_XTS) {
        frequencies = xtsFrequencies;
        durations = xtsDurations;
        count = 5;
    } else if (selectedMode == TALK_PERMIT_TONE_TRBO) {
        frequencies = trboFrequencies;
        durations = trboDurations;
        count = 4;
    } else if (selectedMode == TALK_PERMIT_TONE_KENW) {
        frequencies = fleetSyncFrequencies;
        durations = fleetSyncDurations;
        count = 4;
    } else {
        return;
    }

    SYSTEM_DelayMs(1);
    GPIO_ClearBit(&GPIOC->DATA, 4);
    SYSTEM_DelayMs(1);
    BK4819_EnterTxMute();
    if (delay_speaker_open)
        AUDIO_AudioPathOff();
    else
        AUDIO_AudioPathOn();
    BK4819_SetAF(2);
    BK4819_EnableTXLink();
    GPIO_SetBit(&GPIOC->DATA, 4);
    SYSTEM_DelayMs(1);

    for (unsigned int i = 0; i < count; i++) {
        if (frequencies[i] != 0) {
            BK4819_WriteRegister(BK4819_REG_70, BK4819_REG_70_ENABLE_TONE1 | (toneGain << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));
            BK4819_WriteRegister(BK4819_REG_71, scale_freq(frequencies[i]));
        } else {
            BK4819_WriteRegister(BK4819_REG_70, 0);
        }

        if (delay_speaker_open && i == 0 && durations[i] > 60) {
            SYSTEM_DelayMs(60);
            AUDIO_AudioPathOn();
            SYSTEM_DelayMs(durations[i] - 60);
        } else {
            SYSTEM_DelayMs(durations[i]);
        }
        BK4819_WriteRegister(BK4819_REG_70, 0);
    }

    SYSTEM_DelayMs(1);
    GPIO_ClearBit(&GPIOC->DATA, 4);
    SYSTEM_DelayMs(1);
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_TxOn_Beep();
    SYSTEM_DelayMs(1);
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(1);

    UI_SetTalkPermitToast(TALK_PERMIT_TOAST_SEND, 0);
}

void BK4819_EnterTxMute(void) {
    BK4819_WriteRegister(BK4819_REG_50, 0xBB20);
}

void BK4819_ExitTxMute(void) {
    BK4819_WriteRegister(BK4819_REG_50, 0x3B20);
}

void BK4819_Sleep(void) {
    BK4819_WriteRegister(BK4819_REG_30, 0);
    BK4819_WriteRegister(BK4819_REG_37, 0x1D00);
}

void BK4819_TurnsOffTones_TurnsOnRX(void) {
    BK4819_WriteRegister(BK4819_REG_70, 0);
    BK4819_SetAF(BK4819_AF_MUTE);

    BK4819_ExitTxMute();

    BK4819_WriteRegister(BK4819_REG_30, 0);
    BK4819_WriteRegister(BK4819_REG_30,
                         BK4819_REG_30_ENABLE_VCO_CALIB |
                         BK4819_REG_30_ENABLE_RX_LINK |
                         BK4819_REG_30_ENABLE_AF_DAC |
                         BK4819_REG_30_ENABLE_DISC_MODE |
                         BK4819_REG_30_ENABLE_PLL_VCO |
                         BK4819_REG_30_ENABLE_RX_DSP);
}

#ifdef ENABLE_AIRCOPY
void BK4819_SetupAircopy(void)
    {
        BK4819_WriteRegister(BK4819_REG_70, 0x00E0);    // Enable Tone2, tuning gain 48
        BK4819_WriteRegister(BK4819_REG_72, 0x3065);    // Tone2 baudrate 1200
        BK4819_WriteRegister(BK4819_REG_58, 0x00C1);    // FSK Enable, FSK 1.2K RX Bandwidth, Preamble 0xAA or 0x55, RX Gain 0, RX Mode
                                                        // (FSK1.2K, FSK2.4K Rx and NOAA SAME Rx), TX Mode FSK 1.2K and FSK 2.4K Tx
        BK4819_WriteRegister(BK4819_REG_5C, 0x5665);    // Enable CRC among other things we don't know yet
        BK4819_WriteRegister(BK4819_REG_5D, 0x4700);    // FSK Data Length 72 Bytes (0xabcd + 2 byte length + 64 byte payload + 2 byte CRC + 0xdcba)
    }
#endif

void BK4819_ResetFSK(void) {
    BK4819_WriteRegister(BK4819_REG_3F, 0x0000);        // Disable interrupts
    BK4819_WriteRegister(BK4819_REG_59, 0x0068);        // Sync length 4 bytes, 7 byte preamble

    SYSTEM_DelayMs(30);

    BK4819_Disable();
}

void BK4819_ExitBypass(void) {
    BK4819_SetAF(BK4819_AF_MUTE);

    // REG_7E
    //
    // <15>    0 AGC fix mode
    //         1 = fix
    //         0 = auto
    //
    // <14:12> 3 AGC fix index
    //         3 ( 3) = max
    //         2 ( 2)
    //         1 ( 1)
    //         0 ( 0)
    //         7 (-1)
    //         6 (-2)
    //         5 (-3)
    //         4 (-4) = min
    //
    // <11:6>  0 ???
    //
    // <5:3>   5 DC filter band width for Tx (MIC In)
    //         0 ~ 7
    //         0 = bypass DC filter
    //
    // <2:0>   6 DC filter band width for Rx (I.F In)
    //         0 ~ 7
    //         0 = bypass DC filter
    //

    uint16_t regVal = BK4819_ReadRegister(BK4819_REG_7E);

    // 0x302E / 0 011 000000 101 110
    BK4819_WriteRegister(BK4819_REG_7E, (regVal & ~(0b111 << 3))

                                        | (5u << 3)       // 5  DC Filter band width for Tx (MIC In)

    );
}

void BK4819_PrepareTransmit(bool muteMic) {
    BK4819_ExitBypass();
    BK4819_ExitTxMute();
    BK4819_TxOn_Beep();
    if(muteMic)
	{
		BK4819_MuteMic();
	}
}

void BK4819_MuteMic(void)
{
	const uint16_t reg30 = BK4819_ReadRegister(BK4819_REG_30);
	BK4819_WriteRegister(BK4819_REG_30, reg30 & ~(1u << 2));
}

void BK4819_UnmuteMic(void)
{
	const uint16_t reg30 = BK4819_ReadRegister(BK4819_REG_30);
	BK4819_WriteRegister(BK4819_REG_30, reg30 | (1u << 2));
}

void BK4819_TxOn_Beep(void) {
    BK4819_WriteRegister(BK4819_REG_37, 0x1D0F);
    BK4819_WriteRegister(BK4819_REG_52, 0x028F);
    BK4819_WriteRegister(BK4819_REG_30, 0x0000);
    BK4819_WriteRegister(BK4819_REG_30, 0xC1FE);
}

void BK4819_ExitSubAu(void) {
    // REG_51
    //
    // <15>  0
    //       1 = Enable TxCTCSS/CDCSS
    //       0 = Disable
    //
    // <14>  0
    //       1 = GPIO0Input for CDCSS
    //       0 = Normal Mode (for BK4819 v3)
    //
    // <13>  0
    //       1 = Transmit negative CDCSS code
    //       0 = Transmit positive CDCSS code
    //
    // <12>  0 CTCSS/CDCSS mode selection
    //       1 = CTCSS
    //       0 = CDCSS
    //
    // <11>  0 CDCSS 24/23bit selection
    //       1 = 24bit
    //       0 = 23bit
    //
    // <10>  0 1050HzDetectionMode
    //       1 = 1050/4 Detect Enable, CTC1 should be set to 1050/4 Hz
    //
    // <9>   0 Auto CDCSS Bw Mode
    //       1 = Disable
    //       0 = Enable
    //
    // <8>   0 Auto CTCSS Bw Mode
    //       0 = Enable
    //       1 = Disable
    //
    // <6:0> 0 CTCSS/CDCSS Tx Gain1 Tuning
    //       0   = min
    //       127 = max
    //
    BK4819_WriteRegister(BK4819_REG_51, 0x0000);
}

void BK4819_Conditional_RX_TurnOn_and_GPIO6_Enable(void) {
    if (gRxIdleMode) {
        BK4819_ToggleGpioOut(BK4819_GPIO0_PIN28_RX_ENABLE, true);
        BK4819_RX_TurnOn();
    }
}

void BK4819_EnterDTMF_TX(bool bLocalLoopback) {
    BK4819_EnableDTMF();
    BK4819_EnterTxMute();
    BK4819_SetAF(bLocalLoopback ? BK4819_AF_BEEP : BK4819_AF_MUTE);

    BK4819_WriteRegister(BK4819_REG_70,
                         BK4819_REG_70_MASK_ENABLE_TONE1 |
                         (DTMF_TONE1_GAIN << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN) |
                         BK4819_REG_70_MASK_ENABLE_TONE2 |
                         (DTMF_TONE2_GAIN << BK4819_REG_70_SHIFT_TONE2_TUNING_GAIN));

    BK4819_EnableTXLink();
}

void BK4819_ExitDTMF_TX(bool bKeep) {
    BK4819_EnterTxMute();
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_WriteRegister(BK4819_REG_70, 0x0000);
    BK4819_DisableDTMF();
    BK4819_WriteRegister(BK4819_REG_30, 0xC1FE);
    if (!bKeep)
        BK4819_ExitTxMute();
}

void BK4819_EnableTXLink(void) {
    BK4819_WriteRegister(BK4819_REG_30,
                         BK4819_REG_30_ENABLE_VCO_CALIB |
                         BK4819_REG_30_ENABLE_UNKNOWN |
                         BK4819_REG_30_DISABLE_RX_LINK |
                         BK4819_REG_30_ENABLE_AF_DAC |
                         BK4819_REG_30_ENABLE_DISC_MODE |
                         BK4819_REG_30_ENABLE_PLL_VCO |
                         BK4819_REG_30_ENABLE_PA_GAIN |
                         BK4819_REG_30_DISABLE_MIC_ADC |
                         BK4819_REG_30_ENABLE_TX_DSP |
                         BK4819_REG_30_DISABLE_RX_DSP);
}

void BK4819_PlayDTMF(char Code) {

    struct DTMF_TonePair {
        uint16_t tone1;
        uint16_t tone2;
    };

    const struct DTMF_TonePair tones[] = {
            {941, 1336},
            {697, 1209},
            {697, 1336},
            {697, 1477},
            {770, 1209},
            {770, 1336},
            {770, 1477},
            {852, 1209},
            {852, 1336},
            {852, 1477},
            {697, 1633},
            {770, 1633},
            {852, 1633},
            {941, 1633},
            {941, 1209},
            {941, 1477},
    };


    const struct DTMF_TonePair *pSelectedTone = NULL;
    switch (Code) {
        case '0'...'9':
            pSelectedTone = &tones[0 + Code - '0'];
            break;
        case 'A'...'D':
            pSelectedTone = &tones[10 + Code - 'A'];
            break;
        case '*':
            pSelectedTone = &tones[14];
            break;
        case '#':
            pSelectedTone = &tones[15];
            break;
        default:
            pSelectedTone = NULL;
    }

    if (pSelectedTone) {
        BK4819_WriteRegister(BK4819_REG_71,
                             (((uint32_t) pSelectedTone->tone1 * 103244) + 5000) / 10000);   // with rounding
        BK4819_WriteRegister(BK4819_REG_72,
                             (((uint32_t) pSelectedTone->tone2 * 103244) + 5000) / 10000);   // with rounding
    }
}

void BK4819_PlayDTMFString(const char *pString, bool bDelayFirst, uint16_t FirstCodePersistTime,
                           uint16_t HashCodePersistTime, uint16_t CodePersistTime, uint16_t CodeInternalTime) {
    unsigned int i;

    if (pString == NULL)
        return;

    for (i = 0; pString[i]; i++) {
        uint16_t Delay;
        BK4819_PlayDTMF(pString[i]);
        BK4819_ExitTxMute();
        if (bDelayFirst && i == 0)
            Delay = FirstCodePersistTime;
        else if (pString[i] == '*' || pString[i] == '#')
            Delay = HashCodePersistTime;
        else
            Delay = CodePersistTime;
        SYSTEM_DelayMs(Delay);
        BK4819_EnterTxMute();
        SYSTEM_DelayMs(CodeInternalTime);
    }
}

void BK4819_TransmitTone(bool bLocalLoopback, uint32_t Frequency) {
    BK4819_EnterTxMute();

    // REG_70
    //
    // <15>   0 Enable TONE1
    //        1 = Enable
    //        0 = Disable
    //
    // <14:8> 0 TONE1 tuning gain
    //        0 ~ 127
    //
    // <7>    0 Enable TONE2
    //        1 = Enable
    //        0 = Disable
    //
    // <6:0>  0 TONE2/FSK amplitude
    //        0 ~ 127
    //
    // set the tone amplitude
    //
    BK4819_WriteRegister(BK4819_REG_70,
                         BK4819_REG_70_MASK_ENABLE_TONE1 | (55u << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(Frequency));

    BK4819_SetAF(bLocalLoopback ? BK4819_AF_BEEP : BK4819_AF_MUTE);

    BK4819_EnableTXLink();

    SYSTEM_DelayMs(50);

    BK4819_ExitTxMute();
}

void BK4819_GenTail(uint8_t Tail) {
    // REG_52
    //
    // <15>    0 Enable 120/180/240 degree shift CTCSS or 134.4Hz Tail when CDCSS mode
    //         0 = Normal
    //         1 = Enable
    //
    // <14:13> 0 CTCSS tail mode selection (only valid when REG_52 <15> = 1)
    //         00 = for 134.4Hz CTCSS Tail when CDCSS mode
    //         01 = CTCSS0 120° phase shift
    //         10 = CTCSS0 180° phase shift
    //         11 = CTCSS0 240° phase shift
    //
    // <12>    0 CTCSSDetectionThreshold Mode
    //         1 = ~0.1%
    //         0 =  0.1 Hz
    //
    // <11:6>  0x0A CTCSS found detect threshold
    //
    // <5:0>   0x0F CTCSS lost  detect threshold

    // REG_07 <15:0>
    //
    // When <13> = 0 for CTC1
    // <12:0> = CTC1 frequency control word =
    //                          freq(Hz) * 20.64888 for XTAL 13M/26M or
    //                          freq(Hz) * 20.97152 for XTAL 12.8M/19.2M/25.6M/38.4M
    //
    // When <13> = 1 for CTC2 (Tail 55Hz Rx detection)
    // <12:0> = CTC2 (should below 100Hz) frequency control word =
    //                          25391 / freq(Hz) for XTAL 13M/26M or
    //                          25000 / freq(Hz) for XTAL 12.8M/19.2M/25.6M/38.4M
    //
    // When <13> = 2 for CDCSS 134.4Hz
    // <12:0> = CDCSS baud rate frequency (134.4Hz) control word =
    //                          freq(Hz) * 20.64888 for XTAL 13M/26M or
    //                          freq(Hz)*20.97152 for XTAL 12.8M/19.2M/25.6M/38.4M

    switch (Tail) {
        case 0: // 134.4Hz CTCSS Tail
            BK4819_WriteRegister(BK4819_REG_52, 0x828F);   // 1 00 0 001010 001111
            break;
        case 1: // 120° phase shift
            BK4819_WriteRegister(BK4819_REG_52, 0xA28F);   // 1 01 0 001010 001111
            break;
        case 2: // 180° phase shift
            BK4819_WriteRegister(BK4819_REG_52, 0xC28F);   // 1 10 0 001010 001111
            break;
        case 3: // 240° phase shift
            BK4819_WriteRegister(BK4819_REG_52, 0xE28F);   // 1 11 0 001010 001111
            break;
        case 4: // 55Hz tone freq
            BK4819_WriteRegister(BK4819_REG_07, 0x046f);   // 0 00 0 010001 101111
            break;
    }
}

void BK4819_PlayCDCSSTail(void) {
    BK4819_GenTail(0);     // CTC134
    //亚音
//    BK4819_WriteRegister(BK4819_REG_51, 0x804A);
    BK4819_WriteRegister(BK4819_REG_51, 0x8033);

}

void BK4819_PlayCTCSSTail(void) {
#ifdef ENABLE_CTCSS_TAIL_PHASE_SHIFT
    //BK4819_GenTail(1);     // 120° phase shift
        BK4819_GenTail(2);       // 180° phase shift
        //BK4819_GenTail(3);     // 240° phase shift
#else
    BK4819_GenTail(4);       // 55Hz tone freq
#endif

    // REG_51
    //
    // <15>  0
    //       1 = Enable TxCTCSS/CDCSS
    //       0 = Disable
    //
    // <14>  0
    //       1 = GPIO0Input for CDCSS
    //       0 = Normal Mode (for BK4819 v3)
    //
    // <13>  0
    //       1 = Transmit negative CDCSS code
    //       0 = Transmit positive CDCSS code
    //
    // <12>  0 CTCSS/CDCSS mode selection
    //       1 = CTCSS
    //       0 = CDCSS
    //
    // <11>  0 CDCSS 24/23bit selection
    //       1 = 24bit
    //       0 = 23bit
    //
    // <10>  0 1050HzDetectionMode
    //       1 = 1050/4 Detect Enable, CTC1 should be set to 1050/4 Hz
    //
    // <9>   0 Auto CDCSS Bw Mode
    //       1 = Disable
    //       0 = Enable
    //
    // <8>   0 Auto CTCSS Bw Mode
    //       0 = Enable
    //       1 = Disable
    //
    // <6:0> 0 CTCSS/CDCSS Tx Gain1 Tuning
    //       0   = min
    //       127 = max
//亚音
//    BK4819_WriteRegister(BK4819_REG_51, 0x904A); // 1 0 0 1 0 0 0 0 0 1001010
    BK4819_WriteRegister(BK4819_REG_51, 0x9033); // 1 0 0 1 0 0 0 0 0 1001010

}

uint16_t BK4819_GetRSSI(void) {
    return BK4819_ReadRegister(BK4819_REG_67) & 0x01FF;
}

uint8_t BK4819_GetGlitchIndicator(void) {
    return BK4819_ReadRegister(BK4819_REG_63) & 0x00FF;
}

uint8_t BK4819_GetExNoiceIndicator(void) {
    return BK4819_ReadRegister(BK4819_REG_65) & 0x007F;
}

// ============================================================================
// Software Squelch - Faster close than hardware squelch
// ============================================================================
// 
// Hardware squelch is slower because it combines RSSI + noise + glitch.
// Software squelch closes instantly when either RSSI OR noise degrades,
// providing quicker muting of weak/noisy signals.

static bool g_SquelchOpen = false;

// Main squelch tick - call this periodically (e.g., every 10-100ms in your main loop)
//
// Parameters:
//   openRssi:    RSSI threshold to OPEN squelch (higher = stronger signal needed)
//                Typical: 100-200 depending on sensitivity desired
//   closeRssi:   RSSI threshold to CLOSE squelch (hysteresis)
//                Typical: 20-30% lower than openRssi to prevent chatter
//   openNoise:   Noise threshold to OPEN (lower = cleaner signal)
//                Range: 0-127, typical: 20-50 dB
//   closeNoise:  Noise threshold to CLOSE (hysteresis)
//                Typical: 20% higher than openNoise
//
// Logic:
//   OPEN:  requires BOTH rssi >= openRssi AND noise <= openNoise
//   CLOSE: requires EITHER rssi <= closeRssi OR noise >= closeNoise (faster!)
//
void BK4819_SoftSquelch_Tick(uint16_t openRssi, uint16_t closeRssi,
                              uint8_t openNoise, uint8_t closeNoise) {
    uint16_t rssi  = BK4819_GetRSSI();
    uint8_t  noise = BK4819_GetExNoiceIndicator();
    
    // Both RSSI AND noise must meet thresholds to open squelch
    if (!g_SquelchOpen && rssi >= openRssi && noise <= openNoise) {
        g_SquelchOpen = true;
        BK4819_SetAF(BK4819_AF_FM);
    }
    // Either RSSI OR noise degradation triggers immediate close (fast muting)
    else if (g_SquelchOpen && (rssi <= closeRssi || noise >= closeNoise)) {
        g_SquelchOpen = false;
        BK4819_SetAF(BK4819_AF_MUTE);
    }
}

// Get current software squelch state
bool BK4819_SoftSquelch_IsOpen(void) {
    return g_SquelchOpen;
}

// Manually force squelch open/closed (overrides automatic control)
void BK4819_SoftSquelch_Set(bool bOpen) {
    if (bOpen && !g_SquelchOpen) {
        g_SquelchOpen = true;
        BK4819_SetAF(BK4819_AF_FM);
    } else if (!bOpen && g_SquelchOpen) {
        g_SquelchOpen = false;
        BK4819_SetAF(BK4819_AF_MUTE);
    }
}

uint16_t BK4819_GetVoiceAmplitudeOut(void) {
    return BK4819_ReadRegister(BK4819_REG_64);
}

uint8_t BK4819_GetAfTxRx(void) {
    return BK4819_ReadRegister(BK4819_REG_6F) & 0x003F;
}

bool BK4819_GetFrequencyScanResult(uint32_t *pFrequency) {
    const uint16_t High = BK4819_ReadRegister(BK4819_REG_0D);
    const bool Finished = (High & 0x8000) == 0;
    if (Finished) {
        const uint16_t Low = BK4819_ReadRegister(BK4819_REG_0E);
        *pFrequency = (uint32_t) ((High & 0x7FF) << 16) | Low;
    }
    return Finished;
}

BK4819_CssScanResult_t BK4819_GetCxCSSScanResult(uint32_t *pCdcssFreq, uint16_t *pCtcssFreq) {
    uint16_t Low;
    uint16_t High = BK4819_ReadRegister(BK4819_REG_69);

    if ((High & 0x8000) == 0) {
        Low = BK4819_ReadRegister(BK4819_REG_6A);
        *pCdcssFreq = ((High & 0xFFF) << 12) | (Low & 0xFFF);
        return BK4819_CSS_RESULT_CDCSS;
    }

    Low = BK4819_ReadRegister(BK4819_REG_68);

    if ((Low & 0x8000) == 0) {
        *pCtcssFreq = ((Low & 0x1FFF) * 4843) / 10000;
        return BK4819_CSS_RESULT_CTCSS;
    }

    return BK4819_CSS_RESULT_NOT_FOUND;
}

void BK4819_DisableFrequencyScan(void) {
    // REG_32
    //
    // <15:14> 0 frequency scan time
    //         0 = 0.2 sec
    //         1 = 0.4 sec
    //         2 = 0.8 sec
    //         3 = 1.6 sec
    //
    // <13:1>  ???
    //
    // <0>     0 frequency scan enable
    //         1 = enable
    //         0 = disable
    //
    BK4819_WriteRegister(BK4819_REG_32, // 0x0244);    // 00 0000100100010 0
                         (0u << 14) |          // 0 frequency scan Time
                         (290u << 1) |          // ???
                         (0u << 0));          // 0 frequency scan enable
}

void BK4819_EnableFrequencyScan(void) {
    // REG_32
    //
    // <15:14> 0 frequency scan time
    //         0 = 0.2 sec
    //         1 = 0.4 sec
    //         2 = 0.8 sec
    //         3 = 1.6 sec
    //
    // <13:1>  ???
    //
    // <0>     0 frequency scan enable
    //         1 = enable
    //         0 = disable
    //
    BK4819_WriteRegister(BK4819_REG_32, // 0x0245);   // 00 0000100100010 1
                         (0u << 14) |          // 0 frequency scan time
                         (290u << 1) |          // ???
                         (1u << 0));          // 1 frequency scan enable
}

void BK4819_SetScanFrequency(uint32_t Frequency) {
    BK4819_SetFrequency(Frequency);

    // REG_51
    //
    // <15>  0
    //       1 = Enable TxCTCSS/CDCSS
    //       0 = Disable
    //
    // <14>  0
    //       1 = GPIO0Input for CDCSS
    //       0 = Normal Mode (for BK4819 v3)
    //
    // <13>  0
    //       1 = Transmit negative CDCSS code
    //       0 = Transmit positive CDCSS code
    //
    // <12>  0 CTCSS/CDCSS mode selection
    //       1 = CTCSS
    //       0 = CDCSS
    //
    // <11>  0 CDCSS 24/23bit selection
    //       1 = 24bit
    //       0 = 23bit
    //
    // <10>  0 1050HzDetectionMode
    //       1 = 1050/4 Detect Enable, CTC1 should be set to 1050/4 Hz
    //
    // <9>   0 Auto CDCSS Bw Mode
    //       1 = Disable
    //       0 = Enable
    //
    // <8>   0 Auto CTCSS Bw Mode
    //       0 = Enable
    //       1 = Disable
    //
    // <6:0> 0 CTCSS/CDCSS Tx Gain1 Tuning
    //       0   = min
    //       127 = max
    //
    BK4819_WriteRegister(BK4819_REG_51,
                         BK4819_REG_51_DISABLE_CxCSS |
                         BK4819_REG_51_GPIO6_PIN2_NORMAL |
                         BK4819_REG_51_TX_CDCSS_POSITIVE |
                         BK4819_REG_51_MODE_CDCSS |
                         BK4819_REG_51_CDCSS_23_BIT |
                         BK4819_REG_51_1050HZ_NO_DETECTION |
                         BK4819_REG_51_AUTO_CDCSS_BW_DISABLE |
                         BK4819_REG_51_AUTO_CTCSS_BW_DISABLE);

    BK4819_RX_TurnOn();
}

void BK4819_Disable(void) {
    BK4819_WriteRegister(BK4819_REG_30, 0);

}

void BK4819_StopScan(void) {
    BK4819_DisableFrequencyScan();
    BK4819_Disable();
}

uint8_t BK4819_GetDTMF_5TONE_Code(void) {
    return (BK4819_ReadRegister(BK4819_REG_0B) >> 8) & 0x0F;
}

uint8_t BK4819_GetCDCSSCodeType(void) {
    return (BK4819_ReadRegister(BK4819_REG_0C) >> 14) & 3u;
}

uint8_t BK4819_GetCTCShift(void) {
    return (BK4819_ReadRegister(BK4819_REG_0C) >> 12) & 3u;
}

uint8_t BK4819_GetCTCType(void) {
    return (BK4819_ReadRegister(BK4819_REG_0C) >> 10) & 3u;
}

void BK4819_SendFSKData(uint16_t *pData) {
    unsigned int i;
    uint8_t Timeout = 200;

    SYSTEM_DelayMs(20);

    BK4819_WriteRegister(BK4819_REG_3F, BK4819_REG_3F_FSK_TX_FINISHED);
    BK4819_WriteRegister(BK4819_REG_59, 0x8068);
    BK4819_WriteRegister(BK4819_REG_59, 0x0068);

    for (i = 0; i < 36; i++)
        BK4819_WriteRegister(BK4819_REG_5F, pData[i]);

    SYSTEM_DelayMs(20);

    BK4819_WriteRegister(BK4819_REG_59, 0x2868);

    while (Timeout-- && (BK4819_ReadRegister(BK4819_REG_0C) & 1u) == 0)
        SYSTEM_DelayMs(5);

    BK4819_WriteRegister(BK4819_REG_02, 0);

    SYSTEM_DelayMs(20);

    BK4819_ResetFSK();
}

void BK4819_PrepareFSKReceive(void) {
    BK4819_ResetFSK();
    BK4819_WriteRegister(BK4819_REG_02, 0);
    BK4819_WriteRegister(BK4819_REG_3F, 0);
    BK4819_RX_TurnOn();
    BK4819_WriteRegister(BK4819_REG_3F, 0 | BK4819_REG_3F_FSK_RX_FINISHED | BK4819_REG_3F_FSK_FIFO_ALMOST_FULL);

    // Clear RX FIFO
    // FSK Preamble Length 7 bytes
    // FSK SyncLength Selection
    BK4819_WriteRegister(BK4819_REG_59, 0x4068);

    // Enable FSK Scramble
    // Enable FSK RX
    // FSK Preamble Length 7 bytes
    // FSK SyncLength Selection
    BK4819_WriteRegister(BK4819_REG_59, 0x3068);
}


/* Conventional audible Roger tones are intentionally excluded to save flash. */
#ifdef ENABLE_LEGACY_ROGER_BEEPS
void BK4819_PlayRogerOne(void) {
    const uint32_t tone1_Hz = 1001;  

    BK4819_EnterTxMute();
    AUDIO_AudioPathOn();
    BK4819_SetAF(BK4819_AF_BEEP);

    BK4819_WriteRegister(BK4819_REG_70, BK4819_REG_70_ENABLE_TONE1 | (55u << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));

    BK4819_EnableTXLink();
    SYSTEM_DelayMs(50);


    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone1_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(25);
    BK4819_EnterTxMute();

    SYSTEM_DelayMs(25);

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone1_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(25);
    BK4819_EnterTxMute();

    SYSTEM_DelayMs(25);

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone1_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(50);
    BK4819_EnterTxMute();

    BK4819_WriteRegister(BK4819_REG_70, 0x0000);
    SYSTEM_DelayMs(50);
    AUDIO_AudioPathOff();
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_WriteRegister(BK4819_REG_30, 0xC1FE);   // 1 1 0000 0 1 1111 1 1 1 0
}

void BK4819_PlayRogerTwo(void) {
    const uint32_t tone1_Hz = 627;
    const uint32_t tone2_Hz = 959;

    BK4819_EnterTxMute();
    AUDIO_AudioPathOn();
    BK4819_SetAF(BK4819_AF_BEEP);

    BK4819_WriteRegister(BK4819_REG_70, BK4819_REG_70_ENABLE_TONE1 | (55u << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));

    BK4819_EnableTXLink();
    SYSTEM_DelayMs(50);

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone1_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(100);
    BK4819_EnterTxMute();

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone2_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(40);
    BK4819_EnterTxMute();

    BK4819_WriteRegister(BK4819_REG_70, 0x0000);
    SYSTEM_DelayMs(50);
    AUDIO_AudioPathOff();
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_WriteRegister(BK4819_REG_30, 0xC1FE);
}

void BK4819_PlayRogerThree(void) {
    const uint32_t tone1_Hz = 785;
    const uint32_t tone2_Hz = 526;

    BK4819_EnterTxMute();
    AUDIO_AudioPathOn();
    BK4819_SetAF(BK4819_AF_BEEP);

    BK4819_WriteRegister(BK4819_REG_70, BK4819_REG_70_ENABLE_TONE1 | (55u << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));

    BK4819_EnableTXLink();
    SYSTEM_DelayMs(50);

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone1_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(300);
    BK4819_EnterTxMute();

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone2_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(250);
    BK4819_EnterTxMute();

    BK4819_WriteRegister(BK4819_REG_70, 0x0000);
    SYSTEM_DelayMs(50);
    AUDIO_AudioPathOff();
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_WriteRegister(BK4819_REG_30, 0xC1FE);
}

void BK4819_PlayRogerFour(void) {
    const uint32_t tone1_Hz = 1538;
    const uint32_t tone2_Hz = 1938;
    const uint32_t tone3_Hz = 1838;
    const uint32_t tone4_Hz = 487;
    const uint32_t tone5_Hz = 788;
    const uint32_t tone6_Hz = 938;
    const uint32_t tone7_Hz = 1238;

    BK4819_EnterTxMute();
    AUDIO_AudioPathOn();
    BK4819_SetAF(BK4819_AF_BEEP);

    BK4819_WriteRegister(BK4819_REG_70, BK4819_REG_70_ENABLE_TONE1 | (55u << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));

    BK4819_EnableTXLink();
    SYSTEM_DelayMs(50);

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone1_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(60);
    BK4819_EnterTxMute();
//    SYSTEM_DelayMs(10);

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone2_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(50);
    BK4819_EnterTxMute();
//    SYSTEM_DelayMs(10);

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone3_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(50);
    BK4819_EnterTxMute();
//    SYSTEM_DelayMs(10);

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone4_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(50);
    BK4819_EnterTxMute();

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone5_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(50);
    BK4819_EnterTxMute();

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone6_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(50);
    BK4819_EnterTxMute();

    BK4819_WriteRegister(BK4819_REG_71, scale_freq(tone7_Hz));
    BK4819_ExitTxMute();
    SYSTEM_DelayMs(50);
    BK4819_EnterTxMute();

    BK4819_WriteRegister(BK4819_REG_70, 0x0000);
    SYSTEM_DelayMs(50);
    AUDIO_AudioPathOff();
    BK4819_SetAF(BK4819_AF_MUTE);
    BK4819_WriteRegister(BK4819_REG_30, 0xC1FE);
}
#endif

void BK4819_Enable_AfDac_DiscMode_TxDsp(void) {
    BK4819_WriteRegister(BK4819_REG_30, 0x0000);
    BK4819_WriteRegister(BK4819_REG_30, 0x0302);
}

void BK4819_GetVoxAmp(uint16_t *pResult) {
    *pResult = BK4819_ReadRegister(BK4819_REG_64) & 0x7FFF;
}

void BK4819_SetScrambleFrequencyControlWord(uint32_t Frequency) {
    BK4819_WriteRegister(BK4819_REG_71, scale_freq(Frequency));
}

void BK4819_PlayDTMFEx(bool bLocalLoopback, char Code) {
    BK4819_EnableDTMF();
    BK4819_EnterTxMute();

    BK4819_SetAF(bLocalLoopback ? BK4819_AF_BEEP : BK4819_AF_MUTE);

    BK4819_WriteRegister(BK4819_REG_70,
                         BK4819_REG_70_MASK_ENABLE_TONE1 |
                         (DTMF_TONE1_GAIN << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN) |
                         BK4819_REG_70_MASK_ENABLE_TONE2 |
                         (DTMF_TONE2_GAIN << BK4819_REG_70_SHIFT_TONE2_TUNING_GAIN));

    BK4819_EnableTXLink();

    SYSTEM_DelayMs(50);

    BK4819_PlayDTMF(Code);

    BK4819_ExitTxMute();
}

#ifdef ENABLE_MDC1200
void BK4819_start_tone(const uint16_t frequency, const unsigned int level, const bool tx, const bool tx_mute)
{
    SYSTEM_DelayMs(1);

    GPIO_ClearBit(&GPIOC->DATA, 4);

    SYSTEM_DelayMs(1);

    // mute TX
    BK4819_WriteRegister(0x50, (1u << 15) | 0x3B20);

    BK4819_WriteRegister(0x70, BK4819_REG_70_ENABLE_TONE1 | ((level & 0x7f) << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));

    BK4819_WriteRegister(0x30, 0);

    if (!tx)
    {
        BK4819_WriteRegister(0x30,
//			BK4819_REG_30_ENABLE_VCO_CALIB |
//			BK4819_REG_30_ENABLE_UNKNOWN   |
//			BK4819_REG_30_ENABLE_RX_LINK   |
            BK4819_REG_30_ENABLE_AF_DAC    |
            BK4819_REG_30_ENABLE_DISC_MODE |
//			BK4819_REG_30_ENABLE_PLL_VCO   |
//			BK4819_REG_30_ENABLE_PA_GAIN   |
//			BK4819_REG_30_ENABLE_MIC_ADC   |
            BK4819_REG_30_ENABLE_TX_DSP    |
//			BK4819_REG_30_ENABLE_RX_DSP    |
        0);
    }
    else
    {
        BK4819_WriteRegister(0x30,
            BK4819_REG_30_ENABLE_VCO_CALIB |
            BK4819_REG_30_ENABLE_UNKNOWN   |
//			BK4819_REG_30_ENABLE_RX_LINK   |
            BK4819_REG_30_ENABLE_AF_DAC    |
            BK4819_REG_30_ENABLE_DISC_MODE |
            BK4819_REG_30_ENABLE_PLL_VCO   |
            BK4819_REG_30_ENABLE_PA_GAIN   |
//			BK4819_REG_30_ENABLE_MIC_ADC   |
            BK4819_REG_30_ENABLE_TX_DSP    |
//			BK4819_REG_30_ENABLE_RX_DSP    |
        0);
    }

    BK4819_WriteRegister(0x71, scale_freq(frequency));

    SYSTEM_DelayMs(1);

//	BK4819_SetAF(tx ? BK4819_AF_BEEP : 2);
    BK4819_SetAF(2);  // RX
//	BK4819_SetAF(BK4819_AF_BEEP);  // TX

    if (!tx_mute)
        BK4819_WriteRegister(0x50, 0x3B20);   // 0011 1011 0010 0000

    GPIO_SetBit(&GPIOC->DATA, 4);

    SYSTEM_DelayMs(1);
}

void BK4819_stop_tones(const bool tx)
{
    SYSTEM_DelayMs(1);

    GPIO_ClearBit(&GPIOC->DATA, 4);

    SYSTEM_DelayMs(1);

    BK4819_SetAF(BK4819_AF_MUTE);

//	BK4819_EnterTxMute();
    BK4819_WriteRegister(0x70, 0);
    BK4819_WriteRegister(0x30, 0);
    if (!tx)
    {
        BK4819_WriteRegister(0x30,
            BK4819_REG_30_ENABLE_VCO_CALIB |
//			BK4819_REG_30_ENABLE_UNKNOWN   |
            BK4819_REG_30_ENABLE_RX_LINK   |
            BK4819_REG_30_ENABLE_AF_DAC    |
            BK4819_REG_30_ENABLE_DISC_MODE |
            BK4819_REG_30_ENABLE_PLL_VCO   |
//			BK4819_REG_30_ENABLE_PA_GAIN   |
//			BK4819_REG_30_ENABLE_MIC_ADC   |
//			BK4819_REG_30_ENABLE_TX_DSP    |
            BK4819_REG_30_ENABLE_RX_DSP    |
        0);
    }
    else
    {
        BK4819_WriteRegister(0x30,
            BK4819_REG_30_ENABLE_VCO_CALIB |
            BK4819_REG_30_ENABLE_UNKNOWN   |
//			BK4819_REG_30_ENABLE_RX_LINK   |
            BK4819_REG_30_ENABLE_AF_DAC    |
            BK4819_REG_30_ENABLE_DISC_MODE |
            BK4819_REG_30_ENABLE_PLL_VCO   |
            BK4819_REG_30_ENABLE_PA_GAIN   |
            BK4819_REG_30_ENABLE_MIC_ADC   |
            BK4819_REG_30_ENABLE_TX_DSP    |
//			BK4819_REG_30_ENABLE_RX_DSP    |
        0);
    }

    SYSTEM_DelayMs(1);

    BK4819_ExitTxMute();

    SYSTEM_DelayMs(1);
}

// Helper function to send raw FSK packet data (for preamble tones)
static void BK4819_send_FSK_packet(const uint8_t *packet, unsigned int size)
{
    uint16_t fsk_reg59;

    // setup FSK modem
    BK4819_WriteRegister(0x58,
        (1u << 13) |  // FSK TX mode: FFSK 1200/1800
        (7u << 10) |  // FSK RX mode: FFSK 1200/1800
        (0u << 8) |   // FSK RX gain
        (0u << 6) |   // ???
        (0u << 4) |   // FSK preamble type
        (1u << 1) |   // FSK RX bandwidth
        (1u << 0));   // FSK enable

    // Set tone frequencies
    BK4819_WriteRegister(0x72, scale_freq(1200));  // tone-2 = 1200Hz
    BK4819_WriteRegister(0x70,
        (0u << 15) |  // TONE-1 disable
        (0u << 8) |   // TONE-1 tuning
        (1u << 7) |   // TONE-2 enable
        (96u << 0));  // TONE-2 gain

    // Setup FSK register 0x59
    fsk_reg59 = (0u << 15) |  // clear TX FIFO
                (0u << 14) |  // clear RX FIFO
                (0u << 13) |  // scramble
                (0u << 12) |  // enable RX
                (0u << 11) |  // enable TX
                (0u << 10) |  // invert RX
                (0u << 9) |   // invert TX
                (0u << 8) |   // ???
                (0u << 4) |   // preamble length (minimum)
                (1u << 3) |   // sync length
                (0u << 0);    // ???

    // Set packet length
    BK4819_WriteRegister(0x5D, ((size - 1) << 8));

    // Sync bytes
    BK4819_WriteRegister(0x5A, 0x0000);
    BK4819_WriteRegister(0x5B, 0x0000);

    // CRC setting
    BK4819_WriteRegister(0x5C, 0x5625);

    // Clear and release FIFO
    BK4819_WriteRegister(0x59, (1u << 15) | (1u << 14) | fsk_reg59);
    BK4819_WriteRegister(0x59, fsk_reg59);

    // Load packet data
    {
        unsigned int i;
        const uint16_t *p = (const uint16_t *)packet;
        for (i = 0; i < (size / sizeof(p[0])); i++)
            BK4819_WriteRegister(0x5F, p[i]);
    }

    // Enable TX interrupt
    BK4819_WriteRegister(0x3F, BK4819_REG_3F_FSK_TX_FINISHED);

    // Enable FSK TX
    BK4819_WriteRegister(0x59, (1u << 11) | fsk_reg59);

    // Wait for TX to complete
    {
        unsigned int timeout = 300 / 4;  // allow up to 300ms
        while (timeout-- > 0) {
            SYSTEM_DelayMs(4);
            if (BK4819_ReadRegister(0x0C) & (1u << 0)) {
                BK4819_WriteRegister(0x02, 0);
                if (BK4819_ReadRegister(0x02) & BK4819_REG_02_FSK_TX_FINISHED)
                    timeout = 0;
            }
        }
    }

    // Disable FSK
    BK4819_WriteRegister(0x59, fsk_reg59);
    BK4819_WriteRegister(0x3F, 0);
    BK4819_WriteRegister(0x70, 0);
    BK4819_WriteRegister(0x58, 0);
}

void BK4819_send_MDC1200(const uint8_t op, const uint8_t arg, const uint16_t id, const uint8_t preamble_duration)
{
    uint16_t saved_reg30;
    uint8_t packet[42];
    const bool use_extended_preamble = (preamble_duration != 0u);
    const unsigned int preamble_cycles = use_extended_preamble ? preamble_duration : 1u;

    // Keep the existing MDC1200 encoder and its output byte-for-byte unchanged.
    const unsigned int mdc_size = MDC1200_encode_single_packet(packet, op, arg, id);

    /*
     * Raw FFSK transmit using the BK4819 Tone-1 generator.
     *
     * This deliberately does NOT enable the BK4819 FSK packet engine
     * (REG_58/REG_59/REG_5A/REG_5B/REG_5F). Therefore the chip cannot
     * prepend its own hardware preamble or sync bytes.
     *
     * Existing MDC byte stream is already differentially encoded by
     * xor_modulation(). Transmit the resulting bits MSB-first, using the
     * same tone convention as the software preamble:
     *   bit 1 -> 1200 Hz (0xFF / mark)
     *   bit 0 -> 1800 Hz (0x00 / space).
     */
    saved_reg30 = BK4819_ReadRegister(BK4819_REG_30);
    BK4819_EnterTxMute();

    BK4819_WriteRegister(BK4819_REG_70,
                         BK4819_REG_70_ENABLE_TONE1 |
                         ((MDC_FSK_TX_GAIN & 0x7Fu) << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));

    /*
     * Both preamble modes start at 1800 Hz. The symbol counts below select
     * the short-1800/long-1200 normal pattern or the long-1800/short-1200
     * extended pattern.
     */
    BK4819_WriteRegister(BK4819_REG_71, scale_freq(1800u));

    BK4819_EnableTXLink();
    BK4819_ExitTxMute();

    /*
     * SysTick runs at 48 MHz. 40,000 ticks = 833.333... us,
     * i.e. the standard 1200-baud bit period. Timing each bit from
     * immediately before its frequency-register write keeps the
     * register-write overhead inside the symbol interval.
     */
    {
        const uint32_t bit_ticks_q8 = MDC_TX_BIT_TICKS << 8;
        const uint32_t reload       = SysTick->LOAD + 1u;
        const uint16_t reg71_1200   = scale_freq(1200u);
        const uint16_t reg71_1800   = scale_freq(1800u);
        const uint32_t primask      = __get_PRIMASK();
        uint32_t acc_q8 = 0;
        uint32_t last;
        uint8_t  cur_bit = 0;            // preloaded tone is 1800 Hz (bit 0)
        unsigned int byte_index;

        __disable_irq();                 // ~190 ms, no ISR jitter
        last = SysTick->VAL;

        /*
         * No additional preamble: one normal MDC preamble with a short 1800-Hz
         * section (20 symbols) followed by a long 1200-Hz section (33 symbols).
         *
         * Additional preamble +1..+7: repeat the extended pattern for the
         * selected count, with a long 1800-Hz section (33 symbols) followed
         * by a short 1200-Hz section (20 symbols). This extended pattern is
         * also the final preamble immediately before the packet.
         *
         * Emit at symbol granularity; neither run is byte-aligned. The encoded
         * sync/payload follows immediately after the selected preamble pattern.
         */
#define MDC_TX_SEND_SYMBOL(symbol_value) do { \
            const uint8_t bit = (uint8_t)(symbol_value); \
            if (bit != cur_bit) { \
                BK4819_WriteRegister(BK4819_REG_71, bit ? reg71_1200 : reg71_1800); \
                cur_bit = bit; \
            } \
            for (;;) { \
                const uint32_t now = SysTick->VAL; \
                const uint32_t d = (last >= now) ? (last - now) : (last + reload - now); \
                last = now; \
                acc_q8 += d << 8; \
                if (acc_q8 >= bit_ticks_q8) { \
                    acc_q8 -= bit_ticks_q8; \
                    break; \
                } \
            } \
        } while (0)

        if (use_extended_preamble) {
            for (unsigned int cycle = 0; cycle < preamble_cycles; cycle++) {
                for (unsigned int symbol = 0; symbol < 33u; symbol++)
                    MDC_TX_SEND_SYMBOL(0u);
                for (unsigned int symbol = 0; symbol < 20u; symbol++)
                    MDC_TX_SEND_SYMBOL(1u);
            }
        } else {
            for (unsigned int symbol = 0; symbol < 20u; symbol++)
                MDC_TX_SEND_SYMBOL(0u);
            for (unsigned int symbol = 0; symbol < 33u; symbol++)
                MDC_TX_SEND_SYMBOL(1u);
        }

        for (byte_index = 0; byte_index < mdc_size; byte_index++) {
            for (int bit_index = 7; bit_index >= 0; bit_index--)
                MDC_TX_SEND_SYMBOL((packet[byte_index] >> bit_index) & 1u);
        }
#undef MDC_TX_SEND_SYMBOL

        if (!primask)
            __enable_irq();
    }

    // Stop the tone generator and restore the normal PTT transmit audio path.
    BK4819_EnterTxMute();
    BK4819_WriteRegister(BK4819_REG_70, 0);
    BK4819_WriteRegister(BK4819_REG_30, saved_reg30);
    BK4819_ExitTxMute();
}

#ifdef ENABLE_FLEETSYNC
    void BK4819_send_FleetSync(const uint16_t fleet, const uint16_t unit, const bool end_of_transmission)
    {
        uint16_t saved_reg30;
        uint8_t packet[FLEETSYNC_PACKET_SIZE];
        const unsigned int packet_size = FleetSync_encode_ani(packet, fleet, unit, end_of_transmission);

        /*
         * FleetSync II: 24 alternating preamble bits, followed by the complete
         * 32-bit sync word and the 64-bit ANI block produced by the encoder.
         * The encoder and its packet bytes remain unchanged.
         *
         * Use the same software-timed BK4819 Tone-1 FFSK engine as MDC1200.
         * FleetSync bytes are transmitted MSB-first as encoded by
         * FleetSync_encode_ani(); unlike MDC1200, no MDC xor_modulation()
         * transform is applied here.
         */
        saved_reg30 = BK4819_ReadRegister(BK4819_REG_30);
        BK4819_EnterTxMute();

        BK4819_WriteRegister(BK4819_REG_70,
                             BK4819_REG_70_ENABLE_TONE1 |
                             ((MDC_FSK_TX_GAIN & 0x7Fu) << BK4819_REG_70_SHIFT_TONE1_TUNING_GAIN));

        // The 0xAA alternating preamble starts with a 1 (1200 Hz).
        BK4819_WriteRegister(BK4819_REG_71, scale_freq(1200u));

        BK4819_EnableTXLink();
        BK4819_ExitTxMute();

        {
            const uint32_t bit_ticks_q8 = MDC_TX_BIT_TICKS << 8;
            const uint32_t reload       = SysTick->LOAD + 1u;
            const uint16_t reg71_1200   = scale_freq(1200u);
            const uint16_t reg71_1800   = scale_freq(1800u);
            const uint32_t primask      = __get_PRIMASK();
            uint32_t acc_q8 = 0;
            uint32_t last;
            uint8_t cur_bit = 1u;
            unsigned int byte_index;

            __disable_irq();
            last = SysTick->VAL;

#define FLEETSYNC_TX_SEND_SYMBOL(symbol_value) do { \
                const uint8_t bit = (uint8_t)(symbol_value); \
                if (bit != cur_bit) { \
                    BK4819_WriteRegister(BK4819_REG_71, bit ? reg71_1200 : reg71_1800); \
                    cur_bit = bit; \
                } \
                for (;;) { \
                    const uint32_t now = SysTick->VAL; \
                    const uint32_t d = (last >= now) ? (last - now) : (last + reload - now); \
                    last = now; \
                    acc_q8 += d << 8; \
                    if (acc_q8 >= bit_ticks_q8) { \
                        acc_q8 -= bit_ticks_q8; \
                        break; \
                    } \
                } \
            } while (0)

            // Three bytes of alternating 0xAA preamble (24 symbols).
            for (unsigned int symbol = 0; symbol < 24u; symbol++)
                FLEETSYNC_TX_SEND_SYMBOL((symbol & 1u) == 0u ? 1u : 0u);

            // Send sync + ANI block, all 12 bytes, MSB-first.
            for (byte_index = 0; byte_index < packet_size; byte_index++) {
                for (int bit_index = 7; bit_index >= 0; bit_index--)
                    FLEETSYNC_TX_SEND_SYMBOL((packet[byte_index] >> bit_index) & 1u);
            }

#undef FLEETSYNC_TX_SEND_SYMBOL

            if (!primask)
                __enable_irq();
        }

        // Stop Tone-1 and restore the normal PTT transmit audio path.
        BK4819_EnterTxMute();
        BK4819_WriteRegister(BK4819_REG_70, 0);
        BK4819_WriteRegister(BK4819_REG_30, saved_reg30);
        BK4819_ExitTxMute();
    }
#endif

#endif

void enable_msg_rx(const bool enable) {
    // REG_70
    //
    // <15>    0 TONE-1
    //         1 = enable
    //         0 = disable
    //
    // <14:8>  0 TONE-1 gain
    //
    // <7>     0 TONE-2
    //         1 = enable
    //         0 = disable
    //
    // <6:0>   0 TONE-2 / FSK gain
    //         0 ~ 127
    //
    // enable tone-2, set gain

    // REG_72
    //
    // <15:0>  0x2854 TONE-2 / FSK frequency control word
    //         = freq(Hz) * 10.32444 for XTAL 13M / 26M or
    //         = freq(Hz) * 10.48576 for XTAL 12.8M / 19.2M / 25.6M / 38.4M
    //
    // tone-2 = 1200Hz

    // REG_58
    //
    // <15:13> 1 FSK TX mode selection
    //         0 = FSK 1.2K and FSK 2.4K TX .. no tones, direct FM
    //         1 = FFSK 1200 / 1800 TX
    //         2 = ???
    //         3 = FFSK 1200 / 2400 TX
    //         4 = ???
    //         5 = NOAA SAME TX
    //         6 = ???
    //         7 = ???
    //
    // <12:10> 0 FSK RX mode selection
    //         0 = FSK 1.2K, FSK 2.4K RX and NOAA SAME RX .. no tones, direct FM
    //         1 = ???
    //         2 = ???
    //         3 = ???
    //         4 = FFSK 1200 / 2400 RX
    //         5 = ???
    //         6 = ???
    //         7 = FFSK 1200 / 1800 RX
    //
    // <9:8>   0 FSK RX gain
    //         0 ~ 3
    //
    // <7:6>   0 ???
    //         0 ~ 3
    //
    // <5:4>   0 FSK preamble type selection
    //         0 = 0xAA or 0x55 due to the MSB of FSK sync byte 0
    //         1 = ???
    //         2 = 0x55
    //         3 = 0xAA
    //
    // <3:1>   1 FSK RX bandwidth setting
    //         0 = FSK 1.2K .. no tones, direct FM
    //         1 = FFSK 1200 / 1800
    //         2 = NOAA SAME RX
    //         3 = ???
    //         4 = FSK 2.4K and FFSK 1200 / 2400
    //         5 = ???
    //         6 = ???
    //         7 = ???
    //
    // <0>     1 FSK enable
    //         0 = disable
    //         1 = enable

    // REG_5C
    //
    // <15:7>  ???
    //
    // <6>     1 CRC option enable
    //         0 = disable
    //         1 = enable
    //
    // <5:0>   ???
    //
    // disable CRC

    // REG_5D
    //
    // set the packet size

    if (enable) {
        const bool useFleetSyncRoger = BK4819_IsFleetSyncPTTIDMode();
        const uint16_t fsk_reg59 =
                (0u << 15) |   // 1 = clear TX FIFO
                (0u << 14) |   // 1 = clear RX FIFO
                (0u << 13) |   // 1 = scramble
                (0u << 12) |   // 1 = enable RX
                (0u << 11) |   // 1 = enable TX
                (0u << 10) |   // 1 = invert data when RX
                (0u << 9) |   // 1 = invert data when TX
                (0u << 8) |   // ???
                (0u << 4) |   // 0 ~ 15 preamble length selection .. mdc1200 does not send bit reversals :(
                (1u << 3) |   // 0/1 sync length selection
                (0u << 0);    // 0 ~ 7  ???

        BK4819_WriteRegister(0x70,
                             (0u << 15) |    // 0
                             (0u << 8) |    // 0
                             (1u << 7) |    // 1
                             (96u << 0));    // 96

        BK4819_WriteRegister(0x72, scale_freq(1200));

        BK4819_WriteRegister(0x58,
                             (1u << 13) |        // 1 FSK TX mode selection
                             //   0 = FSK 1.2K and FSK 2.4K TX .. no tones, direct FM
                             //   1 = FFSK 1200 / 1800 TX
                             //   2 = ???
                             //   3 = FFSK 1200 / 2400 TX
                             //   4 = ???
                             //   5 = NOAA SAME TX
                             //   6 = ???
                             //   7 = ???
                             //
                             (7u << 10) |        // 0 FSK RX mode selection
                             //   0 = FSK 1.2K, FSK 2.4K RX and NOAA SAME RX .. no tones, direct FM
                             //   1 = ???
                             //   2 = ???
                             //   3 = ???
                             //   4 = FFSK 1200 / 2400 RX
                             //   5 = ???
                             //   6 = ???
                             //   7 = FFSK 1200 / 1800 RX
                             //
                             (3u << 8) |            // 0 FSK RX gain
                             //   0 ~ 3
                             //
                             (0u << 6) |            // 0 ???
                             //   0 ~ 3
                             //
                             (0u << 4) |            // 0 FSK preamble type selection
                             //   0 = 0xAA or 0x55 due to the MSB of FSK sync byte 0
                             //   1 = ???
                             //   2 = 0x55
                             //   3 = 0xAA
                             //
                             (1u << 1) |            // 1 FSK RX bandwidth setting
                             //   0 = FSK 1.2K .. no tones, direct FM
                             //   1 = FFSK 1200 / 1800
                             //   2 = NOAA SAME RX
                             //   3 = ???
                             //   4 = FSK 2.4K and FFSK 1200 / 2400
                             //   5 = ???
                             //   6 = ???
                             //   7 = ???
                             //
                             (1u << 0));            // 1 FSK enable
        //   0 = disable
        //   1 = enable

        // REG_5A .. bytes 0 & 1 sync pattern
        //
        // <15:8> sync byte 0
        // < 7:0> sync byte 1
//			BK4819_WriteRegister(0x5A, ((uint16_t)mdc1200_sync_suc_xor[0] << 8) | (mdc1200_sync_suc_xor[1] << 0));
#ifdef ENABLE_FLEETSYNC
        BK4819_WriteRegister(0x5A, useFleetSyncRoger ? 0xAAAA : 0x7240);
#else
        BK4819_WriteRegister(0x5A, 0x7240);
#endif

        // REG_5B .. bytes 2 & 3 sync pattern
        //
        // <15:8> sync byte 2
        // < 7:0> sync byte 3
//			BK4819_WriteRegister(0x5B, ((uint16_t)mdc1200_sync_suc_xor[2] << 8) | (mdc1200_sync_suc_xor[3] << 0));
#ifdef ENABLE_FLEETSYNC
        BK4819_WriteRegister(0x5B, useFleetSyncRoger ? 0x23EB : 0x99a7);
#else
        BK4819_WriteRegister(0x5B, 0x99a7);
#endif

        // disable CRC
        BK4819_WriteRegister(0x5C, 0x5625);   // 01010110 0 0 100101
//			BK4819_WriteRegister(0x5C, 0xAA30);   // 10101010 0 0 110000

        // set the almost full threshold
        BK4819_WriteRegister(0x5E, (64u << 3) | (1u << 0));  // 0 ~ 127, 0 ~ 7

        {
#ifdef ENABLE_FLEETSYNC
            BK4819_WriteRegister(
                0x5D,
                ((useFleetSyncRoger ? FLEETSYNC_PACKET_SIZE : MDC1200_FEC_K * 2u) - 1u) << 8);
#else
            BK4819_WriteRegister(0x5D, (MDC1200_FEC_K * 2u - 1u) << 8);
#endif
        }

        // clear FIFO's then enable RX
        BK4819_WriteRegister(0x59, (1u << 15) | (1u << 14) | fsk_reg59);
        BK4819_WriteRegister(0x59, (1u << 12) | fsk_reg59);

        // clear interrupt flags
        BK4819_WriteRegister(0x02, 0);

//			BK4819_RX_TurnOn();

        // enable interrupts
//			BK4819_WriteRegister(0x3F, BK4819_ReadRegister(0x3F) | BK4819_REG_3F_FSK_RX_SYNC | BK4819_REG_3F_FSK_RX_FINISHED | BK4819_REG_3F_FSK_FIFO_ALMOST_FULL);
    } else {
        BK4819_WriteRegister(0x70, 0);
        BK4819_WriteRegister(0x58, 0);
    }
}
