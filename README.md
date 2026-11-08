# Note: This firmware is currently compatible with v1 hardware only. Support for V2 is now available in another repository, see: https://github.com/IvanMaestroGTR/TrueMDC-9M2RTX-UVK5-V2-Firmware.

This firmware is based on the LOSEHU132E firmware (see: https://github.com/losehu/uv-k5-firmware-custom), with several changes made to differentiate it from the original 132E build.
Please refer to the releases for the changelog.

CHIRP support is included; the CHIRP module for this repository can be found in the CHIRP Module folder.

SNS Group for this firmware, for discussion and bug reports:
https://chat.whatsapp.com/ILSZVD0Unua4ATllDHVcUB
Feel free to join us!

## Custom Features

### MDC1200

MDC1200 signalling includes:

- User-configurable MDC1200 ID.
- Contact list and contact aliases.
- Motorola-style extended wobble preamble.
- Configurable additional preamble duration.
- Configurable preamble placement: Pre, Post, or Both.
- Signalling-based Roger modes.

MDC ID notes:

- Personal IDs: `0001-D999`
- `E001-E999` is reserved for group calling. Avoid using this range for a personal ID.
- `FFFF` is used for All Call. Avoid using `FFFF` for a personal ID.

Selective calling is not included due to firmware size limitations.

### FleetSync

FleetSync signalling includes:

- Separate Fleet ID and Unit ID.
- Contact list and contact aliases.
- Configurable FleetSync PTT-ID delay.
- Signalling-based Roger modes.
- Decoder and transmit path follow the active FleetSync signalling mode.

Valid FleetSync ranges:

- Fleet ID: `100-349`
- Unit ID: `1000-4999`

Values outside these limits are clamped to the nearest supported value.

Selective calling is not included due to firmware size limitations.

### DTMF PTT-ID

DTMF PTT-ID follows the stock firmware behaviour for the supported features.

Supported PTT-ID modes:

- Pre
- Post
- Both

DTMF preload timing can be configured.

Selective calling is not included due to firmware size limitations.

When DTMF Pre-ID with side tone is enabled, the DTMF side tone acts as the talk-permit indication and the normal TPT is suppressed to avoid duplicate audio. If the side tone is disabled, the normal Talk Permit Tone is used instead.

### Talk Permit Tone (TPT)

TPT includes:

- Off
- XTS
- TRBO
- Kenwood
- Auto

Auto mode selects the appropriate talk-permit indication according to the active signalling configuration.

### Signalling-Based Roger

Standalone Roger beeps are no longer used. Roger indication is tied to the selected signalling system.

Supported signalling Roger modes:

- MDC1200: Off, Pre, Post, Both
- FleetSync: Off, Pre, Post, Both
- DTMF: Off, Pre, Post, Both

### Signalling Decoder

The decoder follows the selected signalling configuration:

- No signalling selected: MDC1200 decoder is used by default.
- MDC1200 selected: MDC1200 decoder.
- FleetSync selected: FleetSync decoder.

## Current Signalling Menu

The current firmware uses individual menu items rather than a nested Signalling menu.

| Menu | Item | Function |
|---:|---|---|
| **25** | `MDCID` | Configure MDC1200 ID |
| **26** | `FScID` | Configure FleetSync Fleet ID and Unit ID |
| **27** | `ID Dly` | Signalling-dependent ID delay |
| **28** | `MDCPre` | Configure additional MDC1200 preamble duration |
| **29** | `MDCWhn` | Select MDC1200 preamble placement |
| **30** | `PTT ID` | Select PTT-ID signalling mode |
| **40** | `D ST` | DTMF side tone control |
| **44** | `TPT` | Talk Permit Tone selection |

> Menu numbers above correspond to the current firmware build configuration. TPT (Menu 44) is currently accessible on the firmware and is documented as-is.

### Menu 27 — ID Dly

`ID Dly` changes its function according to the selected signalling mode.

- DTMF Pre/Post/Both: DTMF preload timing.
- FleetSync Pre/Post/Both: FleetSync PTT-ID delay.
- Other modes: repeater tail-tone elimination.

### Menu 28 — MDCPre

Controls the additional MDC1200 extended preamble duration:

- Off
- +1
- +2
- +3
- +4
- +5
- +6
- +7

### Menu 29 — MDCWhn

Controls where the additional MDC1200 preamble is transmitted:

- Pre
- Post
- Both

### Menu 30 — PTT ID

Available modes:

- Off
- Pre MDC
- Post MDC
- Both MDC
- Pre FSync
- Post FSync
- Both FSync
- Pre DTMF
- Post DTMF
- Both DTMF

### Menu 40 — D ST

Controls the DTMF side tone:

- Off
- On

### Menu 44 — TPT

Talk Permit Tone selection:

- Off
- XTS
- TRBO
- Kenw
- Auto

A nested Signalling menu is planned for version 9.

## UI Tone Control

### F + Down — UI Tone

F + Down toggles the overall UI tone / boot beep control.

When disabled, UI tones such as the Talk Permit Tone, call-end tone, and power-on beep are disabled. This does not affect channel audio or voice operation.

The setting persists until disabled again.

The `C.End` menu option independently enables or disables the three-note call-end tone. When enabled, it plays after two seconds without receive activity. Turning UI Tone off with F + Down suppresses the tone without changing the `C.End` preference; turning UI Tone back on restores it.

The main screen displays:

- `<<UI Tone: On>>`
- `<<UI Tone: Off>>`

### F + Up — Key Beep

F + Up controls the key beep separately from the general UI tone setting.

The main screen displays:

- `<<Key Beep: On>>`
- `<<Key Beep: Off>>`

## Power & Calibration

### Ultra Low Power

Ultra Low Power has been added for applications such as hotspot operation where lower RF output power is preferred.

### Power Calibration

Power calibration can be adjusted for each power level.

1. Select the desired power level.
2. Open the power calibration menu.
3. The calibration value shown corresponds to the selected power level.
4. Adjust the value as required.

Each power level has its own calibration value.

## CHIRP Support

The custom CHIRP module can configure the custom signalling features, including:

- MDC1200 ID.
- MDC1200 preamble settings.
- FleetSync Fleet ID and Unit ID.
- FleetSync PTT-ID delay.
- DTMF timing.
- Signalling contact lists and aliases.

This allows signalling configuration to be managed from CHIRP instead of entering every value manually on the radio.

## Thanks

I would like to thank everyone who trusts my work and those who have tested my builds. Special thanks to Sara Sinn for early testing, 9W2BIL for extensive beta testing of new features, 9W3MIG, 9W3KKW, and 9W3JJJ for supporting this firmware, and 9M2DSL and 9W2ESR for their ideas and feedback. Special thanks to BI7CZK on his professional advice on Hytera Radio audio UI. Thanks also to everyone else who has used my firmware, and to those who have helped keep the spirit of this project lively and forward-moving!

I hope this work makes your K5 a little better. Enjoy the firmware!

73 DE 9M2RTX
