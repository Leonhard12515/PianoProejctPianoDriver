# Piano solenoid driver

Open [PianoDriver.ino](PianoDriver/PianoDriver.ino) in the Arduino IDE, select **Arduino Uno**, and upload. The sketch uses the Wire library bundled with Arduino AVR Boards.

## First bench test

The sketch assumes active-high MOSFET inputs. The defaults enable ports 1 and 2 on mux channels 0 and 1. Ports 3–8 are disabled. Set `POPULATED_PORTS` to match the boards you connected (`0b00000001` for port 1 alone). The channel map, 7-bit addresses (mux `0x70`, PCA9685 `0x40`), nominal 1 kHz PWM frequency, and 100 ms pulse limit are at the top of the sketch.

For the first logic check, leave coil power disconnected and probe physical output 0 on each PCA9685 with a scope or logic analyzer. Open Serial Monitor at **115200 baud** with **Newline** selected. With the two default boards connected, send each line separately.

```text
STOP
HIT 1 0 10 50
HIT 2 0 25 50
HIT 3 0 10 50
STOP
```

Expect `OK`, `OK`, `OK`, `ERR`, `OK` in order. The first pulse is a roughly 1 kHz, 50% PWM burst lasting about 10 ms on port 1. The second lasts about 25 ms on port 2. Port 3 is disabled. For key discovery with the coils connected, repeat the port 1 command, changing only the output from 0 through 15, and record which key moves.

## Serial protocol

- `HIT <port> <output> <duration_ms> <duty_percent>` takes decimal integers. Ports are 1–8, outputs 0–15, durations 1–`MAX_HIT_MS`, and duty 1–100%. A duty of 100% holds the output high for the pulse.
- Each LF-terminated line (CRLF also works) gets one reply. `OK` for a HIT means its PWM write succeeded and its timer started. A busy output returns `ERR` and keeps its original deadline. Malformed commands, disabled or uninitialized ports, and failed command writes also return `ERR`.
- Send chord lines together. Each line starts its strike as soon as the Uno processes its newline. Each output has an independent deadline measured from its own successful write. Physical edges are subject to the millisecond clock, I²C write time and PWM cycle.
- `STOP` writes full-off to every populated board, even when idle. If a write fails, the sketch retries full-off on the affected board and rejects new strikes there until it succeeds. If initialization failed at startup, correct the connection and reset the Uno.

## Timing check without hardware

The sketch was compiled for `arduino:avr:uno` and exercised with a simulated clock, serial input and I²C register writes. Two HIT lines arrived in one serial buffer. Both addressed output 0, so their overlap also checked mux isolation.

| Simulated time | Port 1, output 0 | Port 2, output 0 |
| --- | --- | --- |
| 100 ms | Starts, 50% | Off |
| 101 ms | 50% | Starts, 50% |
| 110 ms | Off | 50% |
| 126 ms | Off | Off |
