#include <WiFiS3.h>
#include <ArduinoJson.h>
#include <MIDI.h>

// Replace with your Wi-Fi credentials
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const uint16_t MCP_PORT = 80;
const uint8_t BUTTON_PIN = 4;       // D4 on Arduino UNO R4 WiFi
const uint8_t DEFAULT_MIDI_CHANNEL = 1;
const size_t MAX_EVENTS = 64;

WiFiServer server(MCP_PORT);

MIDI_CREATE_INSTANCE(HardwareSerial, Serial1, MIDI);

struct MusicEvent {
  bool isNote;
  uint8_t note;
  uint8_t velocity;
  uint16_t ticks;
};

struct MusicSequence {
  MusicEvent events[MAX_EVENTS];
  size_t count;
  uint8_t channel;
};

MusicSequence sequence;
bool sequenceLoaded = false;

bool playing = false;
bool startPending = false;
bool midiRunning = false;

size_t currentEventIndex = 0;
uint16_t ticksRemaining = 0;
bool noteActive = false;
uint8_t activeNote = 0;
uint8_t activeVelocity = 0;

bool buttonState = HIGH;
bool lastButtonReading = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 30;

const uint8_t STATUS_OK = 200;
const uint8_t STATUS_BAD_REQUEST = 400;
const uint8_t STATUS_METHOD_NOT_ALLOWED = 405;
const uint8_t STATUS_NOT_FOUND = 404;

String handleMcpRequest(const String &body, int &statusCode);
void beginPlayback();
void stopPlayback(bool keepPending);
void startCurrentEvent();
void finishCurrentEvent();
void allNotesOff();
void handleButton();

void handleMidiClock();
void handleMidiStart();
void handleMidiStop();
void handleMidiContinue();

const char *statusText(int code) {
  switch (code) {
    case STATUS_OK:
      return "OK";
    case STATUS_BAD_REQUEST:
      return "Bad Request";
    case STATUS_METHOD_NOT_ALLOWED:
      return "Method Not Allowed";
    case STATUS_NOT_FOUND:
      return "Not Found";
    default:
      return "Internal Server Error";
  }
}

void sendHttpResponse(WiFiClient &client, int statusCode, const String &body) {
  client.print(F("HTTP/1.1 "));
  client.print(statusCode);
  client.print(' ');
  client.println(statusText(statusCode));
  client.println(F("Content-Type: application/json"));
  client.print(F("Content-Length: "));
  client.println(body.length());
  client.println(F("Connection: close"));
  client.println();
  client.print(body);
}

void processClient(WiFiClient &client) {
  client.setTimeout(2000);

  String requestLine = client.readStringUntil('\r');
  client.read();  // consume '\n'
  if (requestLine.length() == 0) {
    client.stop();
    return;
  }

  int firstSpace = requestLine.indexOf(' ');
  int secondSpace = requestLine.indexOf(' ', firstSpace + 1);
  String method = requestLine.substring(0, firstSpace);
  String path = requestLine.substring(firstSpace + 1, secondSpace);

  bool isPost = method == "POST";
  if (!isPost) {
    sendHttpResponse(client, STATUS_METHOD_NOT_ALLOWED,
                     F("{\"error\":\"only_post_supported\"}"));
    client.stop();
    return;
  }

  if (path != "/") {
    sendHttpResponse(client, STATUS_NOT_FOUND, F("{\"error\":\"not_found\"}"));
    client.stop();
    return;
  }

  int contentLength = 0;
  bool isJson = false;
  while (client.connected()) {
    String line = client.readStringUntil('\r');
    client.read();
    if (line.length() == 0) {
      break;
    }
    line.trim();
    if (line.startsWith(F("Content-Length:"))) {
      contentLength = line.substring(15).toInt();
    } else if (line.startsWith(F("Content-Type:"))) {
      if (line.indexOf(F("application/json")) >= 0) {
        isJson = true;
      }
    }
  }

  if (!isJson) {
    sendHttpResponse(client, STATUS_BAD_REQUEST,
                     F("{\"error\":\"content_type_json_required\"}"));
    client.stop();
    return;
  }

  String body;
  body.reserve(contentLength);
  while (body.length() < (unsigned)contentLength && client.connected()) {
    if (client.available()) {
      body += static_cast<char>(client.read());
    }
  }

  int statusCode = STATUS_OK;
  String response = handleMcpRequest(body, statusCode);
  sendHttpResponse(client, statusCode, response);
  client.stop();
}

String handleMcpRequest(const String &body, int &statusCode) {
  static const size_t REQ_DOC_CAPACITY = 4096;
  static const size_t RES_DOC_CAPACITY = 4096;
  StaticJsonDocument<REQ_DOC_CAPACITY> req;
  DeserializationError err = deserializeJson(req, body);
  if (err) {
    statusCode = STATUS_BAD_REQUEST;
    return F("{\"error\":\"invalid_json\"}");
  }

  StaticJsonDocument<RES_DOC_CAPACITY> res;
  long id = req["id"].as<long>();
  const char *method = req["method"] | "";

  res["id"] = id;

  if (strcmp(method, "initialize") == 0) {
    JsonObject result = res.createNestedObject("result");
    result["protocolVersion"] = "2025-03-26";
    JsonObject capabilities = result.createNestedObject("capabilities");
    capabilities["tools"]["listChanged"] = false;
    JsonObject serverInfo = result.createNestedObject("serverInfo");
    serverInfo["name"] = "UNO R4 MIDI MCP";
    serverInfo["version"] = "0.1.0";
  } else if (strcmp(method, "tools/list") == 0) {
    JsonObject result = res.createNestedObject("result");
    JsonArray tools = result.createNestedArray("tools");
    JsonObject tool = tools.createNestedObject();
    tool["name"] = "load_sequence";
    tool["description"] =
        "Load a monophonic sequence of notes and rests expressed in MIDI clock ticks.";
    JsonObject schema = tool.createNestedObject("inputSchema");
    schema["type"] = "object";
    schema["additionalProperties"] = false;

    JsonObject properties = schema.createNestedObject("properties");
    JsonObject channelProp = properties.createNestedObject("channel");
    channelProp["type"] = "integer";
    channelProp["minimum"] = 1;
    channelProp["maximum"] = 16;
    channelProp["default"] = DEFAULT_MIDI_CHANNEL;
    channelProp["description"] =
        "MIDI channel (1-16) to send NoteOn/NoteOff messages on.";

    JsonObject sequenceProp = properties.createNestedObject("sequence");
    sequenceProp["type"] = "array";
    sequenceProp["description"] =
        "Ordered events. Each item has a `type` of 'note' or 'rest' and a `ticks` length.";
    JsonObject items = sequenceProp.createNestedObject("items");
    items["type"] = "object";
    items["additionalProperties"] = false;

    JsonObject itemProps = items.createNestedObject("properties");
    itemProps.createNestedObject("type")["type"] = "string";
    itemProps.createNestedObject("ticks")["type"] = "integer";
    itemProps["ticks"]["minimum"] = 1;
    itemProps.createNestedObject("note")["type"] = "integer";
    itemProps["note"]["minimum"] = 0;
    itemProps["note"]["maximum"] = 127;
    itemProps.createNestedObject("velocity")["type"] = "integer";
    itemProps["velocity"]["minimum"] = 1;
    itemProps["velocity"]["maximum"] = 127;

    JsonArray required = items.createNestedArray("required");
    required.add("type");
    required.add("ticks");

    JsonArray schemaRequired = schema.createNestedArray("required");
    schemaRequired.add("sequence");

    JsonObject annotations = tool.createNestedObject("annotations");
    annotations["title"] = "Load MIDI Sequence";
  } else if (strcmp(method, "tools/call") == 0) {
    JsonObject params = req["params"].as<JsonObject>();
    const char *toolName = params["name"] | "";
    if (strcmp(toolName, "load_sequence") != 0) {
      statusCode = STATUS_BAD_REQUEST;
      return F("{\"error\":\"unknown_tool\"}");
    }

    JsonObject args = params["arguments"].as<JsonObject>();
    if (args.isNull()) {
      statusCode = STATUS_BAD_REQUEST;
      return F("{\"error\":\"missing_arguments\"}");
    }

    JsonArray seqArray = args["sequence"].as<JsonArray>();
    if (seqArray.isNull()) {
      statusCode = STATUS_BAD_REQUEST;
      return F("{\"error\":\"sequence_array_required\"}");
    }

    uint8_t channel = args["channel"].as<uint8_t>();
    if (channel < 1 || channel > 16) {
      channel = DEFAULT_MIDI_CHANNEL;
    }

    size_t count = 0;
    for (JsonObject obj : seqArray) {
      if (count >= MAX_EVENTS) {
        statusCode = STATUS_BAD_REQUEST;
        return F("{\"error\":\"sequence_too_long\"}");
      }

      const char *type = obj["type"] | "";
      uint16_t ticks = obj["ticks"].as<uint16_t>();
      if (ticks == 0) {
        statusCode = STATUS_BAD_REQUEST;
        return F("{\"error\":\"ticks_must_be_positive\"}");
      }

      MusicEvent &event = sequence.events[count];
      event.ticks = ticks;

      if (strcmp(type, "note") == 0) {
        int note = obj["note"].as<int>();
        if (note < 0 || note > 127) {
          statusCode = STATUS_BAD_REQUEST;
          return F("{\"error\":\"note_out_of_range\"}");
        }
        int velocity = obj["velocity"].as<int>();
        if (velocity < 1 || velocity > 127) {
          velocity = 100;
        }
        event.isNote = true;
        event.note = static_cast<uint8_t>(note);
        event.velocity = static_cast<uint8_t>(velocity);
      } else if (strcmp(type, "rest") == 0) {
        event.isNote = false;
        event.note = 0;
        event.velocity = 0;
      } else {
        statusCode = STATUS_BAD_REQUEST;
        return F("{\"error\":\"invalid_event_type\"}");
      }
      count++;
    }

    sequence.count = count;
    sequence.channel = channel;
    sequenceLoaded = count > 0;
    startPending = false;
    if (playing) {
      stopPlayback(false);
    }

    JsonObject result = res.createNestedObject("result");
    JsonArray content = result.createNestedArray("content");
    JsonObject message = content.createNestedObject();
    message["type"] = "text";
    String responseText = String("Loaded ") + count + " events on channel " + channel + ".";
    message["text"] = responseText;
  } else {
    statusCode = STATUS_BAD_REQUEST;
    return F("{\"error\":\"unknown_method\"}");
  }

  String output;
  serializeJson(res, output);
  return output;
}

void beginPlayback() {
  if (!sequenceLoaded || sequence.count == 0) {
    startPending = false;
    return;
  }

  currentEventIndex = 0;
  ticksRemaining = 0;
  noteActive = false;
  playing = true;
  startPending = false;
  startCurrentEvent();
  Serial.println(F("Playback started."));
}

void stopPlayback(bool keepPending) {
  if (noteActive) {
    MIDI.sendNoteOff(activeNote, 0, sequence.channel == 0 ? DEFAULT_MIDI_CHANNEL : sequence.channel);
    noteActive = false;
  }
  playing = false;
  ticksRemaining = 0;
  currentEventIndex = 0;
  if (!keepPending) {
    startPending = false;
  }
  Serial.println(F("Playback stopped."));
}

void startCurrentEvent() {
  if (!playing || currentEventIndex >= sequence.count) {
    stopPlayback(false);
    return;
  }

  MusicEvent &event = sequence.events[currentEventIndex];
  ticksRemaining = event.ticks;
  if (event.isNote) {
    uint8_t channel = sequence.channel == 0 ? DEFAULT_MIDI_CHANNEL : sequence.channel;
    MIDI.sendNoteOn(event.note, event.velocity, channel);
    noteActive = true;
    activeNote = event.note;
    activeVelocity = event.velocity;
  } else {
    noteActive = false;
  }
}

void finishCurrentEvent() {
  if (noteActive) {
    uint8_t channel = sequence.channel == 0 ? DEFAULT_MIDI_CHANNEL : sequence.channel;
    MIDI.sendNoteOff(activeNote, 0, channel);
    noteActive = false;
  }
  currentEventIndex++;
  if (currentEventIndex >= sequence.count) {
    stopPlayback(false);
  } else {
    startCurrentEvent();
  }
}

void allNotesOff() {
  uint8_t channel = sequence.channel == 0 ? DEFAULT_MIDI_CHANNEL : sequence.channel;
  for (uint8_t note = 0; note < 128; ++note) {
    MIDI.sendNoteOff(note, 0, channel);
  }
}

void handleButton() {
  bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        if (playing || startPending) {
          stopPlayback(false);
        } else if (sequenceLoaded) {
          startPending = true;
          if (midiRunning) {
            beginPlayback();
          }
        } else {
          Serial.println(F("No sequence loaded."));
        }
      }
    }
  }

  lastButtonReading = reading;
}

void handleMidiClock() {
  if (!midiRunning) {
    return;
  }

  if (startPending && !playing && sequenceLoaded) {
    beginPlayback();
  }

  if (!playing) {
    return;
  }

  if (ticksRemaining > 0) {
    ticksRemaining--;
    if (ticksRemaining == 0) {
      finishCurrentEvent();
    }
  }
}

void handleMidiStart() {
  midiRunning = true;
  Serial.println(F("MIDI Start received."));
  bool wasPlaying = playing;
  if (playing) {
    stopPlayback(true);
  }
  if (wasPlaying) {
    startPending = true;
  }
  if (startPending && sequenceLoaded) {
    beginPlayback();
  }
}

void handleMidiStop() {
  midiRunning = false;
  Serial.println(F("MIDI Stop received."));
  if (playing) {
    stopPlayback(true);
  }
}

void handleMidiContinue() {
  midiRunning = true;
  Serial.println(F("MIDI Continue received."));
  if (startPending && sequenceLoaded) {
    beginPlayback();
  }
}

void setup() {
  sequence.count = 0;
  sequence.channel = DEFAULT_MIDI_CHANNEL;

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  while (!Serial) {
  }

  Serial.println(F("Booting UNO R4 MIDI MCP player"));

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print(F("Connecting to Wi-Fi"));
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  Serial.print(F("Connected. IP: "));
  Serial.println(WiFi.localIP());

  server.begin();
  Serial.println(F("MCP HTTP server listening on port 80"));

  Serial1.begin(31250);
  MIDI.begin(MIDI_CHANNEL_OMNI);
  MIDI.turnThruOff();

  MIDI.setHandleClock(handleMidiClock);
  MIDI.setHandleStart(handleMidiStart);
  MIDI.setHandleStop(handleMidiStop);
  MIDI.setHandleContinue(handleMidiContinue);
}

void loop() {
  handleButton();
  MIDI.read();

  WiFiClient client = server.available();
  if (client) {
    processClient(client);
  }
}
