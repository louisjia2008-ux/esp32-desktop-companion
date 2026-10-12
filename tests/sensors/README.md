# Offline LD2410C parser checks

Run `python3 scripts/run_sensor_tests.py`, optionally with `--sanitize` for
AddressSanitizer and UndefinedBehaviorSanitizer. Tests compile the production
parser as C++11 without Arduino, hardware, serial ports or configuration commands.

The two literal byte fixtures in `test_ld2410c_parser.cpp` are the **published
examples**, not captured sensor readings, from HLK's *LD2410C UART protocol
V1.09*, pages 19–20, tables 10–15:
<https://h.hlktech.com/download/HLK-LD2410C-24G/1/LD2410C%20%E4%B8%B2%E5%8F%A3%E9%80%9A%E4%BF%A1%E5%8D%8F%E8%AE%AE%20V1.09.pdf>.
Mutated fixtures and deterministic noise exercise parsing and recovery. A pass
does not prove the GPIO mapping, electrical connection, radar operation or human
presence detection on the device.

The parser accepts normal 13-byte payloads and engineering payloads whose gate
counts and length match table 15. Fixed envelope bytes are checked; `55 00` is
not a calculated CRC. Target states 4–6 are valid protocol data with unknown
presence, and other states/layouts are counted as unsupported without replacing
the last sample. Samples retain their timestamp; callers must use `isFresh()`
before treating an old sample as a current reading.
