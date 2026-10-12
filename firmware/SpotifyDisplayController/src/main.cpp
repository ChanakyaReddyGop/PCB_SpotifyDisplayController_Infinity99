#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>
#include <WebSocketsClient.h>

const char *WIFI_SSID = "WIFI_NAME";
const char *WIFI_PASSWORD = "WIFI_PASSWORD";

const char *BRIDGE_HOST = "IP";

constexpr uint16_t BRIDGE_PORT = 8765;
const char *BRIDGE_PATH = "/";

WebSocketsClient webSocket;

bool socketConnected = false;
uint32_t lastWifiRetryAt = 0;

constexpr int TFT_SCK = 8;
constexpr int TFT_MOSI = 10; 
constexpr int TFT_CS = 20;
constexpr int TFT_DC = 21;
constexpr int TFT_RST = 7;
constexpr int TFT_BL = 6;

constexpr int BUTTON_PREVIOUS = 2;
constexpr int BUTTON_PAUSE = 3;
constexpr int BUTTON_NEXT = 4;

constexpr int POT_PIN = 5;

constexpr uint8_t TFT_INIT = INITR_BLACKTAB;
constexpr uint8_t TFT_ROTATION = 1;

constexpr uint32_t DEBOUNCE_MS = 30;
constexpr uint32_t POT_SAMPLE_MS = 10;
constexpr uint32_t VOLUME_SEND_MS = 100;
constexpr uint32_t BRIDGE_TIMEOUT_MS = 6000;
constexpr uint32_t DISPLAY_INTERVAL_MS = 100;

constexpr int VOLUME_CHANGE_THRESHOLD = 2;
constexpr bool REVERSE_POT = false;

constexpr int POT_RAW_MIN = 0;
constexpr int POT_RAW_MAX = 4095;

Adafruit_ST7735 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);

struct Button {
  int pin;
  const char *command;
  bool lastRaw;
  bool stable;
  uint32_t changedAt;
};

Button buttons[] = {
  {BUTTON_PREVIOUS, "previous", HIGH, HIGH, 0},
  {BUTTON_PAUSE, "toggle", HIGH, HIGH, 0},
  {BUTTON_NEXT, "next", HIGH, HIGH, 0}
};

String songTitle = "No song information";
String songArtist = "";

bool playing = false;
bool bridgeConnected = false;
bool displayDirty = true;

int displayedVolume = 0;

float filteredPot = 0.0f;
int lastSentVolume = -1;
bool volumeBaselineSet = false;

uint32_t lastPotSampleAt = 0;
uint32_t lastVolumeSentAt = 0;
uint32_t lastBridgeMessageAt = 0;
uint32_t lastDisplayAt = 0;

constexpr size_t RX_CAPACITY = 2048;


void sendCommand(const char *command) {
  if(!socketConnected || !bridgeConnected) {
    return;
  }

  StaticJsonDocument<128> doc;
  doc["command"] = command;

  String message;
  serializeJson(doc, message);
  webSocket.sendTXT(message);
}

void sendVolume(int value) {
  if(!socketConnected || !bridgeConnected) {
    return;
  }

  StaticJsonDocument<128> doc;
  doc["command"] = "volume";
  doc["value"] = value;

  String message;
  serializeJson(doc, message);
  webSocket.sendTXT(message);
}

void updateButtons(uint32_t now) {
  for(Button &button : buttons) {
    bool raw = digitalRead(button.pin);

    if(raw != button.lastRaw) {
      button.lastRaw = raw;
      button.changedAt = now;
    }

    if(now - button.changedAt >= DEBOUNCE_MS && raw != button.stable) {
      button.stable = raw;

      if(button.stable == LOW) {
        sendCommand(button.command);
      }
    }
  }
}

int potToPercent(float raw) {
  float percent = (raw - POT_RAW_MIN) * 100.0f / (POT_RAW_MAX - POT_RAW_MIN);

  int value = constrain(static_cast<int>(lroundf(percent)), 0, 100);

  if(REVERSE_POT) {
    value -= 100;
  }

  return value;
}

void updatePotentiometer(uint32_t now) {
  if(now - lastPotSampleAt < POT_SAMPLE_MS) {
    return;
  }

  lastPotSampleAt = now;

  int raw = analogRead(POT_PIN);

  filteredPot += 0.15f * (raw - filteredPot);

  int volume = potToPercent(filteredPot);

  if(!bridgeConnected) {
    volumeBaselineSet = false;
    return;
  }

  if(!volumeBaselineSet) {
    lastSentVolume = volume;
    volumeBaselineSet = true;
    return;
  }

  bool changedEnough = abs(volume - lastSentVolume) >= VOLUME_CHANGE_THRESHOLD;

  bool reachedEndpoint = (volume == 0 || volume == 100) && volume != lastSentVolume;

  if ((changedEnough || reachedEndpoint) && now - lastVolumeSentAt >= VOLUME_SEND_MS && socketConnected) {
    sendVolume(volume);

    lastSentVolume = volume;
    lastVolumeSentAt = now;

    displayedVolume = volume;
    displayDirty = true;
  }
}

void handleIncomingLine(const char *line) {
  StaticJsonDocument<4096> doc;

  DeserializationError error = deserializeJson(doc, line);

  if(error || !doc.is<JsonObject>()) {
    return;
  }

  bool isStatus = doc.containsKey("title") || doc.containsKey("artist") || doc.containsKey("playing") || doc.containsKey("volume") || doc.containsKey("connected");

  if(!isStatus) {
    return;
  }

  lastBridgeMessageAt = millis();

  bool connected = true;

  if(doc["connected"].is<bool>()) {
    connected = doc["connected"].as<bool>();
  }

  if(connected != bridgeConnected) {
    bridgeConnected = connected;
    volumeBaselineSet = false;
    displayDirty = true;
  }

  if(doc["title"].is<const char *>()) {
    String newTitle = doc["title"].as<const char *>();

    if(newTitle != songTitle) {
      songTitle = newTitle;
      displayDirty = true;
    }
  }

  if(doc["artist"].is<const char *>()) {
    String newArtist = doc["artist"].as<const char *>();

    if(newArtist != songArtist) {
      songArtist = newArtist;
      displayDirty = true;
    }
  }

  if(doc["playing"].is<bool>()) {
    bool newPlaying = doc["playing"].as<bool>();

    if(newPlaying != playing) {
      playing = newPlaying;
      displayDirty = true;
    }
  }

  if(doc["volume"].is<int>()) {
    int newVolume = constrain(doc["volume"].as<int>(), 0, 100);

    if(newVolume != displayedVolume) {
      displayedVolume = newVolume;
      displayDirty = true;
    }
  }

}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch(type) {
    case WStype_CONNECTED:
      socketConnected = true;
      bridgeConnected = false;
      volumeBaselineSet = false;
      displayDirty = true;

      Serial.println("WebSocket connected");
      break;
    
    case WStype_DISCONNECTED:
      socketConnected = false;
      bridgeConnected = false;
      volumeBaselineSet = false;
      displayDirty = true;

      Serial.println("WebSocket disconnected");
      break;
    
    case WStype_TEXT: {
      if(length == 0 || length >= RX_CAPACITY) {
        return;
      }

      char message[RX_CAPACITY];
      memcpy(message, payload, length);
      message[length] = '\0';
      handleIncomingLine(message);
      break;
    }

    default:
      break;
  }
}


String displayText(const String &input) {
  String result;
  result.reserve(input.length());

  for (size_t i = 0; i < input.length(); i++) {
    uint8_t c = static_cast<uint8_t>(input[i]);

    if(c >=32 && c <= 126) {
      result += static_cast<char>(c);
    }
    else if(c < 128) {
      result += ' ';
    }
    else if ((c & 0xC0) != 0x80) {
      result += '?';
    }
  }
  return result;
}

void drawTextBlock(const String &input, int x, int y, uint8_t textSize, int maxLines, uint16_t color) {
  String text = displayText(input);

  int charsPerLine = (tft.width() - x) / (6 * textSize);

  if (charsPerLine < 1) {
    return;
  }

  int capacity = charsPerLine * maxLines;

  if(static_cast<int>(text.length()) > capacity) {
    text = text.substring(0, capacity);

    if(capacity >= 3) {
      text.setCharAt(capacity - 3, '.');
      text.setCharAt(capacity - 2, '.');
      text.setCharAt(capacity - 1, '.');
    }
  }

  tft.setTextSize(textSize);
  tft.setTextColor(color);
  tft.setTextWrap(false);

  for(int line = 0; line < maxLines; ++line) {
    int start = line * charsPerLine;

    if(start >= static_cast<int>(text.length())) {
      break;
    }
    int end = start + charsPerLine;

    tft.setCursor(x, y + line * 8 * textSize);
    tft.print(text.substring(start, end));
  }
}

void drawScreen() {
  tft.fillScreen(ST77XX_BLACK);

  tft.setTextWrap(false);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN);
  tft.setCursor(4,4);
  tft.print("SPOTIFY CONTROLLER");

  tft.drawFastHLine(4, 16, tft.width() - 8, ST77XX_BLUE);

  if (!bridgeConnected) {
    drawTextBlock("Waited for bridge", 4, 28, 1, 2, ST77XX_WHITE);
    drawTextBlock("Connect Wi-Fi and run the computer bridge", 4, 54, 1, 3, ST77XX_YELLOW);
    return;
  }

  drawTextBlock(songTitle.length() ? songTitle : "No active track", 4, 24, 2, 2, ST77XX_WHITE);
  drawTextBlock(songArtist, 4, 62, 1, 2, ST77XX_CYAN);

  int statusY = tft.height() - 34;

  tft.setTextSize(1);
  tft.setTextColor(playing ? ST77XX_GREEN : ST77XX_YELLOW);
  tft.setCursor(4, statusY);
  tft.print(playing ? "Playing" : "Paused");

  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(4, statusY + 12);
  tft.print("Volume: ");
  tft.print(displayedVolume);
  tft.print('%');

  int barWidth = tft.width() - 8;
  int barY = tft.height() - 9;

  tft.drawRect(4, barY, barWidth, 7, ST77XX_WHITE);

  int fillWidth = (barWidth - 2) * displayedVolume / 100;

  if(fillWidth > 0) {
    tft.fillRect(5, barY + 1, fillWidth, 5, ST77XX_GREEN);
  }
}

void updateBridgeConnection(uint32_t now) {
  if(bridgeConnected && (!socketConnected || WiFi.status() != WL_CONNECTED || now - lastBridgeMessageAt > BRIDGE_TIMEOUT_MS)) {
    bridgeConnected = false;
    volumeBaselineSet = false;
    displayDirty = true;
  }
}

void updateDisplay(uint32_t now) {
  if(!displayDirty || now - lastDisplayAt < DISPLAY_INTERVAL_MS) {
    return;
  }

  lastDisplayAt = now;
  drawScreen();
  displayDirty = false;
}

void updateNetwork(uint32_t now) {
  if(WiFi.status() != WL_CONNECTED) {
    if (socketConnected || bridgeConnected) {
      socketConnected = false;
      bridgeConnected = false;
      volumeBaselineSet = false;
      displayDirty = true;
    }

    if(now - lastWifiRetryAt >= 10000) {
      lastWifiRetryAt = now;
      WiFi.reconnect();
    }
    return;
  }
  webSocket.loop();
}


void setup() {
  Serial.begin(115200);

  for(Button &button : buttons) {
    pinMode(button.pin, INPUT_PULLUP);
    button.lastRaw = digitalRead(button.pin);
    button.stable = button.lastRaw;
    button.changedAt = millis();
  }

  pinMode(POT_PIN, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(POT_PIN, ADC_11db);

  filteredPot = analogRead(POT_PIN);
  displayedVolume = potToPercent(filteredPot);

  SPI.begin(TFT_SCK, -1, TFT_MOSI, TFT_CS);

  tft.initR(TFT_INIT);
  tft.setRotation(TFT_ROTATION);
  tft.fillScreen(ST77XX_BLACK);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  drawScreen();
  displayDirty = false;

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  webSocket.begin(BRIDGE_HOST, BRIDGE_PORT, BRIDGE_PATH);

  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
}

void loop() {
  uint32_t now = millis();

  updateNetwork(now);

  now = millis();

  updateBridgeConnection(now);
  updateButtons(now);
  updatePotentiometer(now);
  updateDisplay(now);

  delay(1);
}