# UNO R4 WiFi MIDI MCP Bridge

This project pairs an Arduino UNO R4 WiFi sketch with a Python-based
[FastMCP](https://github.com/jlowin/fastmcp) server. The Arduino stays on your
network listening for HTTP commands, while the Python program exposes a
Model Context Protocol (MCP) interface that tools such as Claude Desktop can
use to load MIDI sequences and monitor playback.

```
LLM / Claude ──(MCP over stdio)──▶ `uno_r4_mcp_bridge.py`
                             │
                             └──(HTTP)──▶ Arduino UNO R4 WiFi
```

The Arduino receives monophonic MIDI phrases, waits for an external MIDI clock
(start/continue/stop), and performs the uploaded material in sync. Transport
is handled with a push button on D4 that toggles between play and stop, while a
second button on D3 can be tapped to fire a middle C over MIDI for quick
testing.

## Components

- **Arduino sketch (`MidiMcpPlayer/MidiMcpPlayer.ino`)** – connects to Wi-Fi,
  exposes a tiny REST API, buffers MIDI events, and performs them according to
  incoming MIDI clock messages. The hardware UART (`Serial1`) is used for the
  MIDI shield.
- **Python bridge (`uno_r4_mcp_bridge.py`)** – implements a FastMCP server with
  two tools: `load_sequence` uploads note data to the Arduino and `get_status`
  reads back transport information. The bridge is designed to run on the same
  machine as the LLM client.

## Arduino setup

1. Install the **WiFiS3**, **ArduinoJson**, and **MIDI Library** packages in the
   Arduino IDE.
2. Open `MidiMcpPlayer/MidiMcpPlayer.ino` and set `WIFI_SSID` / `WIFI_PASSWORD`
   near the top of the file.
3. Wire the MIDI shield to the hardware UART, a normally-open push button from
   D4 to ground (transport control), and another normally-open push button from
   D3 to ground (manual C trigger). The sketch enables the pull-up resistors.
4. Select **Arduino UNO R4 WiFi** as the target board and upload the sketch.
5. Open the serial monitor at 115200 baud to confirm the assigned IP address and
   REST server status.

### REST endpoints exposed by the Arduino

- `POST /sequence`
  ```json
  {
    "channel": 1,
    "ticksPerQuarter": 48,
    "sequence": [
      { "type": "note", "note": 60, "velocity": 96, "ticks": 24 },
      { "type": "rest", "ticks": 12 },
      { "type": "note", "note": 64, "velocity": 96, "ticks": 24 }
    ]
  }
  ```
  Loads up to 64 events. Each event requires a positive `ticks` duration. Note
  events must include `note` (0–127) and optionally `velocity` (defaults to 100).
  Use the optional `ticksPerQuarter` (alias `ppqn`) to tell the sketch what
  resolution the payload uses; it is scaled to 24 ticks per quarter internally.
  The board immediately replaces the previous buffer and arms playback.

- `GET /status`
  ```json
  {
    "sequenceLoaded": true,
    "eventCount": 3,
    "channel": 1,
    "playing": false,
    "startPending": true,
    "midiClockRunning": false,
    "timing": {
      "sourceTicksPerQuarter": 48,
      "appliedTicksPerQuarter": 24,
      "ticksPerQuarterProvided": true,
      "autoScaled": false,
      "scaleFactor": 2.0
    },
    "wifi": {
      "ip": "192.168.1.42",
      "rssi": -54
    }
  }
  ```
  Useful for debugging connectivity from the Python side.
  The `timing` block reflects whether the sketch auto-scaled the incoming data
  and which ticks-per-quarter resolution it ultimately stored.

## Python MCP bridge

Create a virtual environment (recommended) and install the dependencies:

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

The bridge reads its configuration from CLI flags or environment variables. The
most convenient approach is to create a `.env` file next to the script:

```env
UNO_R4_BASE_URL=http://192.168.1.42
UNO_R4_TIMEOUT=5
MCP_TRANSPORT=stdio
```

Then start the server (which defaults to stdio transport):

```bash
python uno_r4_mcp_bridge.py
```

Two MCP tools become available:

- `load_sequence(sequence, channel=1, ticks_per_quarter=None)` – validates the
  payload and forwards it to `POST /sequence` on the Arduino. On success the
  Arduino confirms how many events were buffered, the ticks-per-quarter it is
  using internally, and whether the MIDI clock is currently running.
- `get_status()` – simply returns the JSON document from `GET /status`.

Both tools surface Arduino-side validation errors (e.g. bad note numbers) back
through MCP so the calling agent can correct its request.

## Music data format

- **ticks** – durations are specified in MIDI clock ticks (24 ticks per quarter
  note when the external device conforms to the MIDI spec). If your data is in a
  different resolution (e.g. 48 ticks per quarter from a DAW export) provide the
  optional `ticksPerQuarter` (or `ppqn`) value and the Arduino will scale the
  durations accordingly. When omitted the sketch automatically reduces common
  multiples (48, 96, …) down to 24 ticks per quarter so playback matches the
  external MIDI clock.
- **note events** – require `type: "note"`, `note` 0–127, optional `velocity`
  1–127 (defaults to 100).
- **rests** – `type: "rest"` and `ticks`; any `note` or `velocity` fields are
  ignored.
- **channel** – optional; defaults to channel 1 when omitted or out of range.

Sequences longer than 64 events are rejected to conserve memory on the UNO R4.

## Using with Claude Desktop

Update `claude_desktop_config.json` so Claude launches the bridge via stdio. An
example entry is provided below (adjust the paths for your system):

```json
{
  "mcpServers": {
    "uno-r4-midi": {
      "command": "python3",
      "args": [
        "/absolute/path/to/uno_r4_mcp_bridge.py",
        "--transport",
        "stdio"
      ],
      "env": {
        "UNO_R4_BASE_URL": "http://192.168.1.42"
      },
      "description": "Arduino UNO R4 WiFi MIDI player (FastMCP bridge)"
    }
  }
}
```

Claude requires the `command` field even when using stdio. After saving the
configuration, restart Claude Desktop. The MCP panel should list
`uno-r4-midi` with the `load_sequence` and `get_status` tools available.

## Typical workflow

1. Flash the Arduino sketch and confirm it reports the correct IP address.
2. Start `python uno_r4_mcp_bridge.py` and verify that it can reach the board by
   calling `get_status` (either from Claude or via `mcp` CLI tooling).
3. Have the AI agent invoke `load_sequence` with the desired phrase.
4. Press the D4 button to arm playback. Once external MIDI clock pulses arrive,
   the UNO will perform the uploaded notes in sync. Press D4 again to stop. Tap
   the D3 button at any time to send a momentary middle C for monitoring.
5. Send a new `load_sequence` request whenever you want to change the material.

## Troubleshooting

- Ensure the UNO and the machine running the Python bridge are on the same
  network; firewalls blocking port 80 will prevent uploads.
- If the bridge reports connection errors, double-check `UNO_R4_BASE_URL` and
  the Arduino's serial console output.
- MIDI playback only starts after the board sees a MIDI `Start`/`Continue`
  message and the D4 button has been pressed to arm playback.
