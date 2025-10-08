#include <WiFiS3.h>
#include <ArduinoJson.h>
#include <MIDI.h>

// Replace with your Wi-Fi credentials
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const uint16_t CONTROL_PORT = 80;
const uint8_t BUTTON_PIN = 4;       // D4 on Arduino UNO R4 WiFi
const uint8_t TRIGGER_BUTTON_PIN = 3;  // D3 on Arduino UNO R4 WiFi
const uint8_t TRIGGER_NOTE = 60;    // Middle C
const uint8_t TRIGGER_VELOCITY = 100;
const uint8_t DEFAULT_MIDI_CHANNEL = 1;
const size_t MAX_EVENTS = 64;

WiFiServer server(CONTROL_PORT);

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
  uint16_t sourceTicksPerQuarter;
  uint16_t appliedTicksPerQuarter;
  bool ticksPerQuarterProvided;
  bool autoScaledTicks;
};

MusicSequence sequence;
bool sequenceLoaded = false;

bool playing = false;
bool startPending = false;
bool midiRunning = false;

size_t currentEventIndex = 0;
uint32_t clockPosition = 0;
uint32_t eventEndTick = 0;
bool noteActive = false;
uint8_t activeNote = 0;
uint8_t activeVelocity = 0;

bool buttonState = HIGH;
bool lastButtonReading = HIGH;
unsigned long lastDebounceTime = 0;

bool triggerButtonState = HIGH;
bool lastTriggerButtonReading = HIGH;
unsigned long lastTriggerDebounceTime = 0;

const unsigned long debounceDelay = 30;

const uint8_t STATUS_OK = 200;
const uint8_t STATUS_BAD_REQUEST = 400;
const uint8_t STATUS_METHOD_NOT_ALLOWED = 405;
const uint8_t STATUS_NOT_FOUND = 404;

uint32_t gcd32(uint32_t a, uint32_t b) {
  while (b != 0) {
    uint32_t temp = a % b;
    a = b;
    b = temp;
  }
  return a;
}

String statusText(int code) {
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

void sendJsonResponse(WiFiClient &client, int statusCode, const JsonDocument &doc) {
  client.print(F("HTTP/1.1 "));
  client.print(statusCode);
  client.print(' ');
  client.println(statusText(statusCode));
  client.println(F("Content-Type: application/json"));
  client.print(F("Content-Length: "));
  client.println(measureJson(doc));
  client.println(F("Connection: close"));
  client.println();
  serializeJson(doc, client);
}

void sendJsonError(WiFiClient &client, int statusCode, const String &code,
                   const String &message) {
  StaticJsonDocument<256> doc;
  JsonObject error = doc.createNestedObject("error");
  error["code"] = code;
  error["message"] = message;
  sendJsonResponse(client, statusCode, doc);
}

void processClient(WiFiClient &client);
void handleStatusRequest(WiFiClient &client);
void handleSequencePost(WiFiClient &client, const String &body);
void printWifiStatus();

void beginPlayback();
void stopPlayback(bool keepPending);
void startCurrentEvent();
void finishCurrentEvent();
void allNotesOff();
void handleButton();
void handleTriggerButton();

void handleMidiClock();
void handleMidiStart();
void handleMidiStop();
void handleMidiContinue();

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
  if (firstSpace < 0 || secondSpace < 0) {
    sendJsonError(client, STATUS_BAD_REQUEST, "malformed_request",
                  "Unable to parse HTTP request line.");
    client.stop();
    return;
  }

  String method = requestLine.substring(0, firstSpace);
  String path = requestLine.substring(firstSpace + 1, secondSpace);

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

  if (method == "POST" && path == "/sequence") {
    if (!isJson) {
      sendJsonError(client, STATUS_BAD_REQUEST, "content_type_json_required",
                    "POST /sequence expects application/json.");
      client.stop();
      return;
    }

    if (contentLength <= 0) {
      sendJsonError(client, STATUS_BAD_REQUEST, "content_length_required",
                    "Missing Content-Length header.");
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

    handleSequencePost(client, body);
  } else if (method == "GET" && path == "/status") {
    handleStatusRequest(client);
  } else if (method == "POST") {
    sendJsonError(client, STATUS_NOT_FOUND, "unknown_endpoint",
                  String("No handler for ") + path + ".");
  } else {
    sendJsonError(client, STATUS_METHOD_NOT_ALLOWED, "method_not_allowed",
                  "Only POST /sequence and GET /status are supported.");
  }

  client.stop();
}

void handleStatusRequest(WiFiClient &client) {
  StaticJsonDocument<256> doc;
  doc["sequenceLoaded"] = sequenceLoaded;
  doc["eventCount"] = sequence.count;
  doc["channel"] = sequence.channel == 0 ? DEFAULT_MIDI_CHANNEL : sequence.channel;
  doc["playing"] = playing;
  doc["startPending"] = startPending;
  doc["midiClockRunning"] = midiRunning;

  JsonObject timing = doc.createNestedObject("timing");
  timing["sourceTicksPerQuarter"] = sequence.sourceTicksPerQuarter;
  timing["appliedTicksPerQuarter"] = sequence.appliedTicksPerQuarter;
  timing["ticksPerQuarterProvided"] = sequence.ticksPerQuarterProvided;
  timing["autoScaled"] = sequence.autoScaledTicks;
  if (sequence.appliedTicksPerQuarter > 0) {
    timing["scaleFactor"] = static_cast<float>(sequence.sourceTicksPerQuarter) /
                             static_cast<float>(sequence.appliedTicksPerQuarter);
  } else {
    timing["scaleFactor"] = 1.0f;
  }

  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["ip"] = WiFi.localIP().toString();
  wifi["rssi"] = WiFi.RSSI();

  sendJsonResponse(client, STATUS_OK, doc);
}

void handleSequencePost(WiFiClient &client, const String &body) {
  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    sendJsonError(client, STATUS_BAD_REQUEST, "invalid_json",
                  String("Unable to parse JSON body: ") + err.c_str());
    return;
  }

  JsonObject payload = doc.as<JsonObject>();
  JsonArray seqArray = payload["sequence"].as<JsonArray>();
  if (seqArray.isNull() || seqArray.size() == 0) {
    sendJsonError(client, STATUS_BAD_REQUEST, "sequence_required",
                  "Payload must include a non-empty 'sequence' array.");
    return;
  }

  uint8_t channel = payload["channel"].as<uint8_t>();
  if (channel < 1 || channel > 16) {
    channel = DEFAULT_MIDI_CHANNEL;
  }

  uint32_t requestedTicksPerQuarter = payload["ticksPerQuarter"].as<uint32_t>();
  if (requestedTicksPerQuarter == 0) {
    requestedTicksPerQuarter = payload["ppqn"].as<uint32_t>();
  }
  bool ticksPerQuarterProvided = requestedTicksPerQuarter > 0;
  if (requestedTicksPerQuarter == 0) {
    requestedTicksPerQuarter = 24;
  }
  if (ticksPerQuarterProvided && requestedTicksPerQuarter > 65535) {
    sendJsonError(client, STATUS_BAD_REQUEST, "ticks_per_quarter_out_of_range",
                  "ticksPerQuarter must be 65535 or less.");
    return;
  }

  MusicEvent tempEvents[MAX_EVENTS];
  uint32_t rawTicks[MAX_EVENTS];

  size_t count = 0;
  uint32_t gcdTicks = 0;
  for (JsonObject obj : seqArray) {
    if (count >= MAX_EVENTS) {
      sendJsonError(client, STATUS_BAD_REQUEST, "sequence_too_long",
                    String("Sequence can contain at most ") + MAX_EVENTS + " events.");
      return;
    }

    const char *type = obj["type"] | "";
    uint32_t ticks = obj["ticks"].as<uint32_t>();
    if (ticks == 0) {
      sendJsonError(client, STATUS_BAD_REQUEST, "ticks_must_be_positive",
                    "Each event must specify 'ticks' greater than zero.");
      return;
    }

    rawTicks[count] = ticks;
    if (gcdTicks == 0) {
      gcdTicks = ticks;
    } else {
      gcdTicks = gcd32(gcdTicks, ticks);
    }

    MusicEvent &event = tempEvents[count];
    event.ticks = 0;

    if (strcmp(type, "note") == 0) {
      int note = obj["note"].as<int>();
      if (note < 0 || note > 127) {
        sendJsonError(client, STATUS_BAD_REQUEST, "note_out_of_range",
                      "Note values must be between 0 and 127.");
        return;
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
      sendJsonError(client, STATUS_BAD_REQUEST, "invalid_event_type",
                    "Each event 'type' must be 'note' or 'rest'.");
      return;
    }
    count++;
  }

  uint32_t scaleFactor = 1;
  uint16_t appliedTicksPerQuarter = 24;
  uint16_t sourceTicksPerQuarter =
      requestedTicksPerQuarter > 65535
          ? static_cast<uint16_t>(65535)
          : static_cast<uint16_t>(requestedTicksPerQuarter);
  sequence.autoScaledTicks = false;

  if (ticksPerQuarterProvided) {
    for (size_t i = 0; i < count; ++i) {
      uint32_t ticks = rawTicks[i];
      uint32_t scaled = static_cast<uint32_t>(
          (static_cast<uint64_t>(ticks) * appliedTicksPerQuarter +
           (requestedTicksPerQuarter / 2)) /
          requestedTicksPerQuarter);
      if (scaled == 0) {
        scaled = 1;
      }
      if (scaled > 65535) {
        sendJsonError(
            client, STATUS_BAD_REQUEST, "ticks_after_scaling_out_of_range",
            "Event duration exceeds supported range after scaling.");
        return;
      }
      tempEvents[i].ticks = static_cast<uint16_t>(scaled);
    }
    sequence.autoScaledTicks = false;
  } else {
    if (gcdTicks >= appliedTicksPerQuarter &&
        gcdTicks % appliedTicksPerQuarter == 0) {
      scaleFactor = gcdTicks / appliedTicksPerQuarter;
    }

    if (scaleFactor > 1) {
      for (size_t i = 0; i < count; ++i) {
        uint32_t scaled = rawTicks[i] / scaleFactor;
        if (scaled == 0) {
          scaled = 1;
        }
        if (scaled > 65535) {
          sendJsonError(client, STATUS_BAD_REQUEST, "ticks_too_large",
                        "Scaled event duration exceeds supported range.");
          return;
        }
        tempEvents[i].ticks = static_cast<uint16_t>(scaled);
      }
      sequence.autoScaledTicks = true;
      uint32_t computedSource = scaleFactor * appliedTicksPerQuarter;
      if (computedSource > 65535) {
        computedSource = 65535;
      }
      sourceTicksPerQuarter = static_cast<uint16_t>(computedSource);
    } else {
      for (size_t i = 0; i < count; ++i) {
        if (rawTicks[i] > 65535) {
          sendJsonError(client, STATUS_BAD_REQUEST, "ticks_too_large",
                        "Event duration exceeds supported range.");
          return;
        }
        tempEvents[i].ticks = static_cast<uint16_t>(rawTicks[i]);
      }
      sequence.autoScaledTicks = false;
      sourceTicksPerQuarter = appliedTicksPerQuarter;
    }
  }

  if (sequence.autoScaledTicks) {
    Serial.print(F("Auto-scaled sequence ticks by factor "));
    Serial.println(scaleFactor);
  }
  if (ticksPerQuarterProvided && requestedTicksPerQuarter != appliedTicksPerQuarter) {
    Serial.print(F("Scaled sequence from "));
    Serial.print(requestedTicksPerQuarter);
    Serial.println(F(" ticks per quarter to 24."));
  }

  for (size_t i = 0; i < count; ++i) {
    sequence.events[i] = tempEvents[i];
  }

  sequence.count = count;
  sequence.channel = channel;
  sequence.sourceTicksPerQuarter = sourceTicksPerQuarter;
  sequence.appliedTicksPerQuarter = appliedTicksPerQuarter;
  sequence.ticksPerQuarterProvided = ticksPerQuarterProvided;
  sequenceLoaded = count > 0;
  startPending = false;
  if (playing) {
    stopPlayback(false);
  }

  Serial.print(F("Loaded sequence with "));
  Serial.print(count);
  Serial.print(F(" events on channel "));
  Serial.println(channel);

  StaticJsonDocument<256> response;
  response["status"] = "ok";
  response["eventsLoaded"] = count;
  response["channel"] = channel;
  response["sequenceLoaded"] = sequenceLoaded;
  response["midiClockRunning"] = midiRunning;
  response["ticksPerQuarter"] = sequence.sourceTicksPerQuarter;
  response["autoScaled"] = sequence.autoScaledTicks;

  sendJsonResponse(client, STATUS_OK, response);
}

void beginPlayback() {
  if (!sequenceLoaded || sequence.count == 0) {
    startPending = false;
    return;
  }

  currentEventIndex = 0;
  clockPosition = 0;
  eventEndTick = 0;
  noteActive = false;
  playing = true;
  startPending = false;
  startCurrentEvent();
  Serial.println(F("Playback started."));
}

void stopPlayback(bool keepPending) {
  if (noteActive) {
    MIDI.sendNoteOff(activeNote, 0,
                     sequence.channel == 0 ? DEFAULT_MIDI_CHANNEL : sequence.channel);
    noteActive = false;
  }
  playing = false;
  clockPosition = 0;
  eventEndTick = 0;
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
  eventEndTick = clockPosition + event.ticks;
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
  if (!playing) {
    return;
  }

  currentEventIndex++;
  if (currentEventIndex >= sequence.count) {
    currentEventIndex = 0;
    if (sequence.count == 0) {
      stopPlayback(false);
      return;
    }
    Serial.println(F("Looping sequence."));
  }

  if (sequence.count > 0) {
    startCurrentEvent();
  } else {
    stopPlayback(false);
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

void handleTriggerButton() {
  bool reading = digitalRead(TRIGGER_BUTTON_PIN);
  if (reading != lastTriggerButtonReading) {
    lastTriggerDebounceTime = millis();
  }

  if ((millis() - lastTriggerDebounceTime) > debounceDelay) {
    if (reading != triggerButtonState) {
      triggerButtonState = reading;
      uint8_t channel =
          sequence.channel == 0 ? DEFAULT_MIDI_CHANNEL : sequence.channel;
      if (triggerButtonState == LOW) {
        MIDI.sendNoteOn(TRIGGER_NOTE, TRIGGER_VELOCITY, channel);
      } else {
        MIDI.sendNoteOff(TRIGGER_NOTE, 0, channel);
      }
    }
  }

  lastTriggerButtonReading = reading;
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

  clockPosition++;

  while (playing && clockPosition >= eventEndTick) {
    finishCurrentEvent();
  }
}

void handleMidiStart() {
  midiRunning = true;
  Serial.println(F("MIDI Start received."));
  clockPosition = 0;
  eventEndTick = 0;
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
  clockPosition = 0;
  eventEndTick = 0;
  if (startPending && sequenceLoaded) {
    beginPlayback();
  }
}

void setup() {
  sequence.count = 0;
  sequence.channel = DEFAULT_MIDI_CHANNEL;
  sequence.sourceTicksPerQuarter = 24;
  sequence.appliedTicksPerQuarter = 24;
  sequence.ticksPerQuarterProvided = false;
  sequence.autoScaledTicks = false;

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(TRIGGER_BUTTON_PIN, INPUT_PULLUP);

  Serial.begin(115200);
  while (!Serial) {
  }

  Serial.println(F("Booting UNO R4 MIDI player"));

  int status = WL_IDLE_STATUS;
  while (status != WL_CONNECTED) {
    Serial.print(F("Attempting to connect to Network named: "));
    Serial.println(WIFI_SSID);
    status = WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    delay(10000);
  }
  server.begin();
  printWifiStatus();
  Serial.println(F("Sequence control server listening on port 80"));

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
  handleTriggerButton();
  MIDI.read();

  WiFiClient client = server.available();
  if (client) {
    processClient(client);
  }
}

void printWifiStatus() {
  Serial.print(F("SSID: "));
  Serial.println(WiFi.SSID());

  IPAddress ip = WiFi.localIP();
  Serial.print(F("IP Address: "));
  Serial.println(ip);

  long rssi = WiFi.RSSI();
  Serial.print(F("signal strength (RSSI):"));
  Serial.print(rssi);
  Serial.println(F(" dBm"));
  Serial.println(ip);
}
