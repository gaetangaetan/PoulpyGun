// PoulpyGun — tube slot on ESP-NOW, three button edges back to the hub.
#include <ESP8266WiFi.h>
#include <espnow.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
extern "C" {
#include "user_interface.h"
}

#define PIN_PIXELS D1
#define PIN_A D3
#define PIN_B D4
#define PIN_TRIGGER D5
#define PIN_GND1 D6
#define PIN_GND2 D7
#define PIN_GND3 D8

#define DEFAULT_LEDS 130
#define MAX_LEDS 300
#define SLOT 10

#define SIGNATURE_A 0x3C
#define SIGNATURE_B 0x61
#define SIGNATURE_C 0x05
#define REQUEST_MAC_CMD 0xFB
#define RESPONSE_MAC_CMD 0xFC
#define SET_GROUP_NUMBER_CMD 0xFF
#define GET_INFO_CMD 0xE0
#define RESPONSE_INFO_CMD 0xD0
#define SET_LED_COUNT_CMD 0xE4
#define SET_DMX_ADDRESS_CMD 0xED
#define GET_DMX_ADDRESS_CMD 0xEF
#define RESPONSE_DMX_ADDRESS_CMD 0xD7
#define SET_CHANNEL_CMD 0xE3
#define IDENTIFY_CMD 0xEA
#define BUTTON_EVENT_CMD 0xB0
#define STREAM_KIND 5
#define STREAM_MULTIFIXTURE 0x01
#define STREAM_MARK 6
#define STREAM_MAGIC 0xA5

#define EEPROM_MAGIC 0xA5
#define FW_NAME "PoulpyGun"

struct __attribute__((packed)) DmxPacket {
  uint8_t blockNumber;
  uint8_t dmxvalues[128];
  uint8_t data[100];
};

static Adafruit_NeoPixel strip(DEFAULT_LEDS, PIN_PIXELS, NEO_GRB + NEO_KHZ800);
static uint8_t dmx[512];
static volatile bool packetPending;
static DmxPacket pending;
static uint8_t senderMac[6];
static bool multiStream;
static bool identify;
static uint32_t identifyUntil;
static int address = 1;
static int ledCount = DEFAULT_LEDS;
static uint8_t groupNumber = 0;
static uint8_t wifiChannel = 1;
static uint8_t myMac[6];
static uint8_t buttonSeq;
static const uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint8_t hue;
static uint16_t head;
static uint8_t phase;
static uint8_t frac;

struct Button {
  uint8_t pin;
  uint8_t index;
  bool stable;
  bool lastRead;
  uint32_t changedAt;
};

static Button buttons[3] = {
  {PIN_TRIGGER, 0, true, true, 0},
  {PIN_A, 1, true, true, 0},
  {PIN_B, 2, true, true, 0},
};

static void saveConfig() {
  EEPROM.write(0, address & 0xFF);
  EEPROM.write(1, (address >> 8) & 0xFF);
  EEPROM.write(2, ledCount & 0xFF);
  EEPROM.write(3, (ledCount >> 8) & 0xFF);
  EEPROM.write(4, groupNumber);
  EEPROM.write(5, wifiChannel);
  EEPROM.write(6, EEPROM_MAGIC);
  EEPROM.commit();
}

static void loadConfig() {
  EEPROM.begin(16);
  if (EEPROM.read(6) != EEPROM_MAGIC) {
    address = 1;
    ledCount = DEFAULT_LEDS;
    groupNumber = 0;
    wifiChannel = 1;
    saveConfig();
    return;
  }
  address = EEPROM.read(0) | (EEPROM.read(1) << 8);
  ledCount = EEPROM.read(2) | (EEPROM.read(3) << 8);
  groupNumber = EEPROM.read(4);
  wifiChannel = EEPROM.read(5);
  if (address < 1 || address > 512 - SLOT + 1) address = 1;
  if (ledCount < 1 || ledCount > MAX_LEDS) ledCount = DEFAULT_LEDS;
  if (wifiChannel < 1 || wifiChannel > 13) wifiChannel = 1;
  if (groupNumber > 31) groupNumber = 0;
}

static bool macIsMe(const uint8_t* mac) {
  bool allFf = true;
  bool same = true;
  for (int i = 0; i < 6; i++) {
    if (mac[i] != 0xFF) allFf = false;
    if (mac[i] != myMac[i]) same = false;
  }
  return allFf || same;
}

static void stamp(DmxPacket* pkt) {
  pkt->data[0] = SIGNATURE_A;
  pkt->data[1] = SIGNATURE_B;
  pkt->data[2] = SIGNATURE_C;
}

static void sendPacket(DmxPacket* pkt) {
  esp_now_send((u8*)broadcastMac, (u8*)pkt, sizeof(DmxPacket));
}

static void replyMac() {
  DmxPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.blockNumber = 255;
  stamp(&pkt);
  pkt.data[3] = RESPONSE_MAC_CMD;
  memcpy(pkt.data + 4, myMac, 6);
  pkt.data[10] = groupNumber;
  sendPacket(&pkt);
}

static void replyInfo() {
  DmxPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.blockNumber = 255;
  stamp(&pkt);
  pkt.data[3] = RESPONSE_INFO_CMD;
  memcpy(pkt.data + 4, myMac, 6);
  pkt.data[10] = groupNumber;
  pkt.data[11] = 1;
  pkt.data[12] = wifiChannel;
  pkt.data[13] = ledCount & 0xFF;
  pkt.data[14] = (ledCount >> 8) & 0xFF;
  const uint8_t n = sizeof(FW_NAME) - 1;
  pkt.data[28] = n;
  memcpy(pkt.data + 29, FW_NAME, n);
  pkt.data[53] = address & 0xFF;
  pkt.data[54] = (address >> 8) & 0xFF;
  sendPacket(&pkt);
}

static void replyAddress() {
  DmxPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.blockNumber = 255;
  stamp(&pkt);
  pkt.data[3] = RESPONSE_DMX_ADDRESS_CMD;
  memcpy(pkt.data + 4, myMac, 6);
  pkt.data[10] = address & 0xFF;
  pkt.data[11] = (address >> 8) & 0xFF;
  sendPacket(&pkt);
}

static void sendButton(uint8_t index, bool down) {
  DmxPacket pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.blockNumber = 255;
  stamp(&pkt);
  pkt.data[3] = BUTTON_EVENT_CMD;
  memcpy(pkt.data + 4, myMac, 6);
  pkt.data[10] = index;
  pkt.data[11] = down ? 1 : 0;
  pkt.data[12] = buttonSeq++;
  sendPacket(&pkt);
}

static void applyCommand(const DmxPacket* pkt) {
  const uint8_t cmd = pkt->data[3];
  if (cmd == REQUEST_MAC_CMD) {
    replyMac();
    return;
  }
  if (!macIsMe(pkt->data + 4)) return;
  if (cmd == GET_INFO_CMD) {
    replyInfo();
    return;
  }
  if (cmd == GET_DMX_ADDRESS_CMD) {
    replyAddress();
    return;
  }
  if (cmd == SET_GROUP_NUMBER_CMD) {
    groupNumber = pkt->data[10];
    if (groupNumber > 31) groupNumber = 0;
    saveConfig();
    replyInfo();
    return;
  }
  if (cmd == SET_LED_COUNT_CMD) {
    int n = pkt->data[10] | (pkt->data[11] << 8);
    if (n < 1) n = 1;
    if (n > MAX_LEDS) n = MAX_LEDS;
    ledCount = n;
    strip.updateLength(ledCount);
    saveConfig();
    replyInfo();
    return;
  }
  if (cmd == SET_DMX_ADDRESS_CMD) {
    int a = pkt->data[10] | (pkt->data[11] << 8);
    if (a < 1) a = 1;
    if (a > 512 - SLOT + 1) a = 512 - SLOT + 1;
    address = a;
    saveConfig();
    replyAddress();
    return;
  }
  if (cmd == SET_CHANNEL_CMD) {
    uint8_t ch = pkt->data[10];
    if (ch >= 1 && ch <= 13) {
      wifiChannel = ch;
      saveConfig();
      wifi_set_channel(wifiChannel);
    }
    return;
  }
  if (cmd == IDENTIFY_CMD) {
    identify = true;
    identifyUntil = millis() + 1600;
  }
}

static void onSent(uint8_t*, uint8_t) {}

static void onRecv(uint8_t* mac, uint8_t* data, uint8_t len) {
  if (!data || len < 14) return;
  const bool command = data[0] == 255;
  if (packetPending && !command) return;
  memcpy(&pending, data, len < sizeof(pending) ? len : sizeof(pending));
  if (mac) memcpy(senderMac, mac, 6);
  packetPending = true;
}

static uint8_t scale(uint8_t v, uint8_t bri) {
  return (uint8_t)((v * bri) / 255);
}

static void paintSolid(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < ledCount; i++)
    strip.setPixelColor(i, strip.Color(r, g, b));
}

static int variedLen(int baseLen, int key, uint8_t rnd) {
  if (rnd == 0 || baseLen <= 1) return baseLen;
  int span = max(1, (baseLen * (int)rnd) / 255);
  uint8_t h = (uint8_t)(key * 47 + 19);
  int delta = map(h, 0, 255, -span, span);
  return max(1, baseLen + delta);
}

static bool onTrain(int pos, int headPos, int baseLen, int period, uint8_t rnd) {
  int rel = pos - headPos;
  int cycle = rel / period;
  int within = rel % period;
  if (rel < 0) {
    cycle = (rel - (period - 1)) / period;
    within = rel - cycle * period;
  }
  int len = variedLen(baseLen, cycle, rnd);
  return within >= 0 && within < len;
}

static void render() {
  if (!multiStream) return;
  const int base = address - 1;
  if (base < 0 || base + SLOT > 512) return;
  const uint8_t r = dmx[base + 0];
  const uint8_t g = dmx[base + 1];
  const uint8_t b = dmx[base + 2];
  const uint8_t look = dmx[base + 3];
  const uint8_t speed = dmx[base + 4];
  const uint8_t width = dmx[base + 5];
  const uint8_t extra = dmx[base + 6];
  const uint8_t rnd = dmx[base + 7];
  strip.clear();

  if (look <= 31) {
    paintSolid(r, g, b);
  } else if (look <= 63) {
    uint8_t delta = map(width, 0, 255, 1, 40);
    int step = map((int)speed, 0, 255, -6, 6);
    hue = (uint8_t)(hue + step);
    for (int i = 0; i < ledCount; i++) {
      uint8_t h = hue + i * delta;
      uint8_t rr, gg, bb;
      if (h < 85) { rr = 255 - h * 3; gg = h * 3; bb = 0; }
      else if (h < 170) { h -= 85; rr = 0; gg = 255 - h * 3; bb = h * 3; }
      else { h -= 170; rr = h * 3; gg = 0; bb = 255 - h * 3; }
      strip.setPixelColor(i, strip.Color(scale(rr, r ? r : 255), scale(gg, g ? g : 255), scale(bb, b ? b : 255)));
    }
  } else if (look <= 95) {
    int density = map(width, 0, 255, 1, max(1, ledCount / 2));
    uint8_t thresh = (uint8_t)map(speed, 0, 255, 4, 40);
    for (int i = 0; i < density; i++) {
      if (random(256) < thresh + extra / 4)
        strip.setPixelColor(random(ledCount), strip.Color(r, g, b));
    }
  } else if (look <= 127) {
    bool on = random(256) < (extra < 8 ? 40 : extra);
    uint8_t dim = on ? 255 : (uint8_t)map(width, 0, 255, 0, 80);
    uint8_t rr = scale(r, dim), gg = scale(g, dim), bb = scale(b, dim);
    if (on && random(256) < map(speed, 0, 255, 10, 180)) { rr = gg = bb = 255; }
    paintSolid(rr, gg, bb);
  } else if (look <= 159) {
    int w = max(1, (int)map((int)width, 0, 255, 1, ledCount));
    w = variedLen(w, (int)(head / max(1, ledCount)), rnd);
    int step = (int)map((int)speed, 0, 255, 1, 8);
    head = (uint16_t)((head + step) % max(1, ledCount));
    for (int i = 0; i < w; i++)
      strip.setPixelColor((head + i) % ledCount, strip.Color(r, g, b));
  } else if (look <= 191) {
    phase = (uint8_t)(phase + map(speed, 0, 255, 1, 8));
    uint8_t wave = (uint8_t)((sin((phase + extra) * 0.0245f) + 1.f) * 127.f);
    uint8_t lo = (uint8_t)map(width, 0, 255, 0, 180);
    uint8_t bri = lo + (uint8_t)((wave * (255 - lo)) / 255);
    paintSolid(scale(r, bri), scale(g, bri), scale(b, bri));
  } else if (look <= 223) {
    int len = max(1, (int)map((int)width, 0, 255, 1, ledCount));
    int period = max(1, len + max(1, len / 2));
    frac = (uint8_t)(frac + map((int)speed, 0, 255, 1, 8));
    head = (uint16_t)(head + frac / 3);
    frac = (uint8_t)(frac % 3);
    for (int i = 0; i < ledCount; i++) {
      if (onTrain(i, (int)head, len, period, rnd))
        strip.setPixelColor(i, strip.Color(r, g, b));
      else if (onTrain(ledCount - 1 - i, (int)head + period / 2, len, period, rnd))
        strip.setPixelColor(i, strip.Color(255, 255, 255));
    }
  } else {
    int len = max(1, (int)map((int)width, 0, 255, 1, ledCount));
    int gap = (int)map((int)extra, 0, 255, 0, ledCount);
    int period = max(1, len + gap);
    int step = (int)map((int)speed, 0, 255, 1, 8);
    head = (uint16_t)(head + step);
    for (int i = 0; i < ledCount; i++) {
      if (onTrain(i, (int)head, len, period, rnd))
        strip.setPixelColor(i, strip.Color(r, g, b));
    }
  }
  strip.show();
}

static void pollButtons() {
  const uint32_t now = millis();
  for (auto& button : buttons) {
    bool raw = digitalRead(button.pin) == LOW;
    if (raw != button.lastRead) {
      button.lastRead = raw;
      button.changedAt = now;
    }
    if (now - button.changedAt < 30) continue;
    if (raw == button.stable) continue;
    button.stable = raw;
    sendButton(button.index, raw);
  }
}

static void servicePacket() {
  if (!packetPending) return;
  DmxPacket pkt = pending;
  packetPending = false;
  if (pkt.data[0] != SIGNATURE_A || pkt.data[1] != SIGNATURE_B || pkt.data[2] != SIGNATURE_C)
    return;
  if (pkt.blockNumber == 255) {
    applyCommand(&pkt);
    return;
  }
  if (pkt.blockNumber > 3) return;
  memcpy(dmx + pkt.blockNumber * 128, pkt.dmxvalues, 128);
  multiStream = pkt.data[STREAM_KIND] == STREAM_MULTIFIXTURE && pkt.data[STREAM_MARK] == STREAM_MAGIC;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_GND1, OUTPUT); digitalWrite(PIN_GND1, LOW);
  pinMode(PIN_GND2, OUTPUT); digitalWrite(PIN_GND2, LOW);
  pinMode(PIN_GND3, OUTPUT); digitalWrite(PIN_GND3, LOW);
  pinMode(PIN_A, INPUT_PULLUP);
  pinMode(PIN_B, INPUT_PULLUP);
  pinMode(PIN_TRIGGER, INPUT_PULLUP);
  loadConfig();
  strip.updateLength(ledCount);
  strip.begin();
  strip.setBrightness(80);
  strip.clear();
  strip.show();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  wifi_set_channel(wifiChannel);
  if (esp_now_init() != 0) {
    Serial.println("ESP-NOW init failed");
    return;
  }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(onSent);
  esp_now_register_recv_cb(onRecv);
  esp_now_add_peer((u8*)broadcastMac, ESP_NOW_ROLE_COMBO, wifiChannel, NULL, 0);
  WiFi.macAddress(myMac);
  Serial.print("PoulpyGun ");
  Serial.println(WiFi.macAddress());
  replyInfo();
}

void loop() {
  servicePacket();
  pollButtons();
  if (identify) {
    bool on = ((millis() / 120) % 2) == 0;
    paintSolid(on ? 255 : 0, on ? 255 : 0, on ? 255 : 0);
    strip.show();
    if ((int32_t)(millis() - identifyUntil) > 0) identify = false;
    return;
  }
  static uint32_t lastRender;
  if (millis() - lastRender > 30) {
    lastRender = millis();
    render();
  }
}
