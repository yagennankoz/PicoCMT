# PicoCmt RAW File Format

[日本語](RAW_FORMAT.md) | **English**

A `.RAW` file holds the waveform recorded by PicoCmt as is, without converting it into a tape image. It is created when you record with `Rec Fmt` in the settings menu set to `RAW`. The file `RAW_TEMP.BIN` (the raw data of the last recording), which is created in the root of the SD card each time you record, uses the same format.

This document is for people who read or write RAW files with tools on a PC or similar.

## 1. Overall structure

*   There is no header. From the start to the end of the file, the file is a sequence of **16-bit words (little-endian)**.
*   Each word represents one half-period of the wave (the time the input stayed High or Low). Some words have a special meaning (see the table below).
*   The file size is always a multiple of 2.

## 2. Word types

| Word | Meaning |
|---|---|
| `0x0000` | A word with no meaning. Skip it when reading |
| `0xFFFF` | Silence. The **next word** is the length of the silence (milliseconds, 1 to 65534). The two words form a pair |
| Anything else | A half-period. Bit 15 is the level, bits 14 to 0 are the length |

### 2-1. Half-period words

| Bits | Content |
|---|---|
| bit 15 | Level. `1` = High, `0` = Low |
| bits 14-0 | Length (microseconds, 1 to 32766). The value measured by the recording circuit, which is slightly shorter than the actual length (see below) |

*   The **level** is the output level of PicoCmt's input circuit (comparator). It may be inverted relative to the polarity of the tape signal. FSK signals such as those of the PC-8801 are read by frequency, so you do not need to care about the polarity.
*   **Length correction**: The recording circuit (the PIO of the Raspberry Pi Pico) records a value shorter than the actual length, by the instructions not included in the measured time (about 1.5 microseconds for High and about 2.0 microseconds for Low). When reading, PicoCmt **adds 2 microseconds** for both levels.
    ```
    actual length (us) = (word & 0x7FFF) + 2
    ```
*   High and Low normally alternate. If values were dropped during recording, the same level may appear twice in a row (at the end of a recording, PicoCmt prints the number of dropped values to the serial log).
*   Half-periods of 10 milliseconds or longer are normally written as silence words (below). However, the last few half-periods of a recording may be written as half-period words (up to `0x7FFE`) even if they are 10 milliseconds or longer.

### 2-2. Silence words

```
0xFFFF, length (milliseconds)
```

*   A place where the input wave did not change for 10 milliseconds or longer (silence, or a very low tone).
*   The length is in milliseconds (fractions are truncated).
*   The level during the silence is not recorded. During playback, PicoCmt holds the level opposite to the previous half-period for the length of the silence.

## 3. Start and end of a recording

*   After recording starts, nothing is written until 50 tape-signal half-periods (80 to 1500 microseconds) arrive in a row. The first word is the half-period after those 50.
*   The silence after the end of a recording (STOP, REMOTE terminal OFF, or about 8 seconds of silence) is not written.

## 4. Examples

| Bytes (order in the file) | Word | Meaning |
|---|---|---|
| `CE 00` | `0x00CE` | Low for 206 + 2 = 208 microseconds (half-period of 2400 Hz) |
| `9F 81` | `0x819F` | High for 415 + 2 = 417 microseconds (half-period of 1200 Hz) |
| `FF FF F4 01` | `0xFFFF`, `0x01F4` | Silence for 500 milliseconds |

## 5. Reading example (Python)

```python
import struct

def read_raw(path):
    """Return a list of (level, length[us]). Silence has level None"""
    data = open(path, 'rb').read()
    words = struct.unpack('<%dH' % (len(data) // 2), data[:len(data) // 2 * 2])
    halves = []
    i = 0
    while i < len(words):
        w = words[i]
        i += 1
        if w == 0x0000:
            continue
        if w == 0xFFFF:
            if i < len(words):
                halves.append((None, words[i] * 1000))  # silence (ms -> us)
                i += 1
            continue
        level = 1 if (w & 0x8000) else 0
        halves.append((level, (w & 0x7FFF) + 2))  # correct for the recording circuit
    return halves
```

## 6. Notes on creating RAW files

If you create RAW files with a tool on a PC or similar, follow these rules so that PicoCmt can play and convert them.

*   Write the half-period length **minus 2 microseconds** to bits 14 to 0 (PicoCmt adds 2 when reading).
*   Write half-periods of 10 milliseconds or longer as silence words (`0xFFFF` and milliseconds). For silence longer than 65534 milliseconds, write several silence words in a row.
*   Alternate High and Low.

PicoCmt's conversion from WAV (the conversion menu) also follows these rules: it first turns the WAV file into RAW, then converts it into each format.

## 7. Size estimate

A 1200 baud tape has about 2400 to 4800 half-periods per second, so it takes about 5 to 10 KB per second, or about 3 to 6 MB for 10 minutes.
