# TMID / FSK Bit & Preamble Analyzer

A small desktop WAV analyzer for investigating TMID FSK audio and the warble preamble. Python 3 + Tkinter; no third-party packages are required.

## Run

```bash
py tools/tmid_fsk_analyzer.py
```

## Signal settings

- **Frequency mode: Manual** — enter separate **Mark (Hz)** and **Space (Hz)** values.
- **Frequency mode: Auto** — estimates the two strongest separated tones in the beginning of the recording. Check the detected values; auto mode can be confused by silence, speech, noise, or a recording that starts before the FSK signal.
- Set baud rate (default 1200), start offset, analysis window, and bit polarity.
- Start offset 0 ms analyzes from the beginning of the WAV, so include the warble in the recording if you want to inspect it.

## Preamble / warble inspection

The report includes the chronological bit stream and a run-length view such as `0:3 1:3 0:3 1:3`, plus run lengths alone. This makes alternating warble patterns visible instead of only presenting the reconstructed packet bytes.

## Exporting a log for review

After analyzing a recording, click **Export FULL LOG (.txt)…** and send the resulting TXT file with the original WAV. The log contains WAV metadata, selected frequencies, baud/timing settings, every detected symbol and confidence score, the full bit stream, run-length sequence, raw LSB-first bytes, and UART/CRC hypotheses. **Export bit CSV…** is also available for spreadsheet inspection.

## Limitations

This is an audio-side analyzer. It does not emulate BK4819 sync detection, FIFO packing/bit order, interrupt timing, or the radio's internal demodulator. The first version uses a fixed symbol clock and a user-set start offset; it does not yet recover timing automatically. A CRC failure alone does not prove that the transmitted packet is malformed.
