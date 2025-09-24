# UNO R4 WiFi MIDI MCP Player

This sketch turns an Arduino UNO R4 WiFi into a Wi-Fi connected MIDI player that can be remote-controlled by AI agents through the [Model Context Protocol (MCP)](https://github.com/modelcontextprotocol/). Music phrases are delivered over HTTP, parsed on the board, and performed in sync with an incoming MIDI clock.

## Features

- Connects to Wi-Fi using the built-in ESP32-S3 co-processor (`WiFiS3` library).
- Hosts a lightweight MCP-compatible HTTP endpoint at `POST /`.
- Implements the `load_sequence` tool so an AI agent can upload a sequence of notes and rests.
- Uses the [FortySevenEffects Arduino MIDI Library](https://github.com/FortySevenEffects/arduino_midi_library) over the hardware UART (`Serial1`) for MIDI I/O.
- Locks playback to an external MIDI clock, start, stop, and continue messages.
- Start/stop control via the D4 push button (configured with the internal pull-up resistor).

## Hardware setup

- **Board:** Arduino UNO R4 WiFi.
- **MIDI shield:** Connected to the hardware UART. The sketch assumes the standard MIDI baud rate (31,250 bps).
- **Start/Stop button:** Momentary switch on D4 to ground. Internal pull-up is enabled in software.

## MCP tool schema

The MCP server exposes a single tool named `load_sequence`. It accepts a JSON body with this shape:

```json
{
  "id": 1,
  "method": "tools/call",
  "params": {
    "name": "load_sequence",
    "arguments": {
      "channel": 1,
      "sequence": [
        { "type": "note", "note": 60, "velocity": 96, "ticks": 24 },
        { "type": "rest", "ticks": 12 },
        { "type": "note", "note": 64, "velocity": 96, "ticks": 24 }
      ]
    }
  }
}
```

### Music data format

- `channel` (optional): MIDI channel (1-16). Defaults to channel 1 when omitted or invalid.
- `sequence`: Ordered list of musical events. Each item contains:
  - `type`: Either `"note"` or `"rest"`.
  - `ticks`: Duration in MIDI clock ticks (24 ticks = quarter note when the external MIDI clock follows the MIDI specification).
  - `note` and `velocity`: Required when `type` is `"note"`. `note` must be 0-127, `velocity` 1-127 (defaults to 100 if omitted).

The uploaded sequence is stored locally on the board. Pressing the D4 button toggles playback. When playback is armed and MIDI clock pulses are received (after a MIDI `Start` or `Continue`), the board performs the sequence tightly aligned with the sync. Playback stops automatically at the end of the sequence or when D4 is pressed again.

## Running the sketch

1. Install the **WiFiS3**, **ArduinoJson**, and **MIDI Library** packages in the Arduino IDE.
2. Update `WIFI_SSID` and `WIFI_PASSWORD` near the top of `MidiMcpPlayer.ino`.
3. Select **Arduino UNO R4 WiFi** as the target board.
4. Upload the sketch.
5. After boot, the serial monitor prints the assigned IP address and MCP server status.

## Example MCP workflow

1. AI agent calls `initialize` and `tools/list` following the MCP spec to discover the tool.
2. Agent invokes `tools/call` with the `load_sequence` arguments shown above.
3. User presses the D4 button. The sketch waits for MIDI clock pulses and begins playing notes in sync.
4. Press D4 again to stop. External MIDI `Stop` messages also halt playback while keeping the sequence loaded.

## Notes

- The MCP server only supports HTTP `POST` requests with the `Content-Type: application/json` header.
- The sketch keeps up to 64 events in memory. Larger payloads are rejected with a `sequence_too_long` error.
- If Wi-Fi disconnects, the sketch does not attempt to reconnect automatically. Add reconnection logic if needed for your environment.
- Use the `Serial` monitor (115200 baud) to observe diagnostic messages such as MIDI transport events, playback state, and MCP errors.
