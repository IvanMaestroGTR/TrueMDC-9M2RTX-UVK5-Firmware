# TMID / Multi-pair FSK Analyzer

A small desktop WAV analyzer for debugging the audio-side TMID FSK signal. It uses only the Python standard library and Tkinter (normally included with desktop Python installations).

## Run

```bash
python tmid_fsk_analyzer.py
```

On Windows, use `py tmid_fsk_analyzer.py`.

## Features

- Load PCM WAV recordings (8/16/24/32-bit, mono or stereo).
- Analyze multiple mark/space pairs in one pass. Defaults include TMID 1200/2200 Hz and MDC 1200/1800 Hz.
- Configure baud rate, initial bit offset, analysis-window width, and mark/space bit polarity.
- Inspect every bit decision, timestamp, selected tone, confidence, and per-tone score.
- View raw LSB-first bytes, UART framing candidates, ASCII, and a TMID payload hypothesis (6 ASCII callsign bytes + 1 ASCII SSID byte + little-endian CRC-16/MODBUS).
- Export bit decisions to CSV.

## Important limitation

This analyzes a recorded audio waveform. It does **not** model BK4819 FSK register configuration, hardware sync recognition, FIFO packing/bit order, interrupt timing, or RF demodulation. Use it to establish whether the captured audio contains a stable and CRC-valid TMID packet, then compare that result with the bytes delivered by the radio's RX FIFO.

The first version uses a fixed symbol clock and user-adjustable start offset; it does not perform automatic clock recovery. Sweep the offset in small steps when diagnosing bit alignment.
