#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>

const char* WIFI_NAME = "Kcell_Auleti_C2CC";
const char* WIFI_PASSWORD = "ipdqr6ft399";
const char* BOT_TOKEN = "9128502390:AAHiysbbOKLKfbOyR2qoga1jwpniBfesaao";
const char* JOIN_CODE = "Hopkins";

#define TRIG_PIN 19
#define ECHO_PIN 18
// LEDs from the ACEBOTT kit. Each LED requires its own 220-ohm series resistor.
// ESP32 GPIO -> resistor -> LED long leg (+); LED short leg (-) -> GND.
#define LED_GREEN_PIN 25
#define LED_RED_PIN   26
#define LED_BLUE_PIN  27

// Make a public, anonymous ThingSpeak channel (field1..field6).
// Keep the Write API Key ONLY in your local ESP32 sketch, never in website files.
const char* THINGSPEAK_WRITE_KEY = "Y2STBG0EQOU8009J";
const char* DEVICE_LABEL = "DRAINIQ LAB"; // Do not put your home address in public cloud data.
const unsigned long CLOUD_INTERVAL_MS = 20000UL;


// Distance from sensor to the bottom of an empty container.
// Set using DRAINIQ LIVE over USB and store in ESP32 Preferences.
const float DEFAULT_EMPTY_DISTANCE_CM = 13.0f;
float emptyDistanceCM = DEFAULT_EMPTY_DISTANCE_CM;
const float YELLOW_PERCENT = 60.0f;
const float RED_PERCENT = 80.0f;
const int MAX_USERS = 5;

const unsigned long ALERT_COOLDOWN_MS = 60000UL;
const unsigned long SENSOR_TIMEOUT_MS = 20000UL;
const unsigned long WIFI_RETRY_MS = 30000UL;
const unsigned long TELEGRAM_POLL_MS = 3000UL;
const int CONFIRM_COUNT = 3;

Preferences prefs;
String users[MAX_USERS];
long updateOffset = 0;

float distanceCM = -1.0f;
float suspectedDistance = -1.0f;
unsigned long lastGoodReading = 0;
unsigned long firstRejectedReading = 0;
unsigned long lastMeasure = 0;
unsigned long lastTelegram = 0;
unsigned long lastCloudAttempt = 0;
unsigned long lastCloudSuccess = 0;
int lastCloudHttpCode = 0;
unsigned long lastWiFiRetry = 0;
unsigned long lastAlertSent = 0;
unsigned long lastNoticeAttempt = 0;
bool wifiWasConnected = false;
bool hasGoodReading = false;
bool hasSentAlert = false;
bool sensorLost = false;
bool acknowledgedRed[MAX_USERS] = {false};
const int EVENT_COUNT = 14;
String eventMessages[EVENT_COUNT];
int eventNext = 0;
int eventCount = 0;
int recoveryCount = 0;
int activeLevel = -1;
int pendingLevel = -1;
int pendingCount = 0;

const int HISTORY_SIZE = 50;
float levelHistory[HISTORY_SIZE];
unsigned long timeHistory[HISTORY_SIZE];
int historyNext = 0;
int historyCount = 0;

enum Notice {
  NOTICE_NONE,
  NOTICE_YELLOW,
  NOTICE_GREEN,
  NOTICE_RED,
  NOTICE_RED_REMINDER,
  NOTICE_RED_ENDED,
  NOTICE_SENSOR_LOST,
  NOTICE_SENSOR_RECOVERED
};

Notice pendingNotice = NOTICE_NONE;
bool pendingUrgent = false;

String keyForUser(int i) {
  if (i == 0) return "owner";
  return "user" + String(i);
}

bool registered(const String &id) {
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i] == id) return true;
  }
  return false;
}

bool registerUser(const String &id) {
  if (registered(id)) return true;
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i] == "") {
      users[i] = id;
      String key = keyForUser(i);
      prefs.putString(key.c_str(), id);
      return true;
    }
  }
  return false;
}

float measureDistance() {
  float values[5];
  int count = 0;

  for (int i = 0; i < 5; i++) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);

    unsigned long duration = pulseIn(ECHO_PIN, HIGH, 30000UL);
    if (duration > 0) {
      float d = duration * 0.0343f / 2.0f;
      if (d >= 2.0f && d <= 400.0f) values[count++] = d;
    }
    delay(70);
  }

  if (count < 3) return -1.0f;

  for (int i = 0; i < count - 1; i++) {
    for (int j = i + 1; j < count; j++) {
      if (values[j] < values[i]) {
        float temp = values[i];
        values[i] = values[j];
        values[j] = temp;
      }
    }
  }

  if (count % 2 == 1) return values[count / 2];
  return (values[count / 2 - 1] + values[count / 2]) / 2.0f;
}

float fillPercent(float distance) {
  if (emptyDistanceCM <= 0.0f) return 0.0f;
  return constrain((emptyDistanceCM - distance) * 100.0f / emptyDistanceCM, 0.0f, 100.0f);
}

void rememberLevel(float level) {
  levelHistory[historyNext] = level;
  timeHistory[historyNext] = millis();
  historyNext = (historyNext + 1) % HISTORY_SIZE;
  if (historyCount < HISTORY_SIZE) historyCount++;
}

bool risingRate(float &rate) {
  if (historyCount < 2) return false;
  unsigned long now = millis();
  int chosen = -1;

  for (int i = 0; i < historyCount; i++) {
    int pos = (historyNext - 1 - i + HISTORY_SIZE) % HISTORY_SIZE;
    unsigned long age = now - timeHistory[pos];
    if (age >= 30000UL) {
      chosen = pos;
      break;
    }
    if (age >= 15000UL) chosen = pos;
  }

  if (chosen < 0) return false;
  unsigned long elapsed = now - timeHistory[chosen];
  if (elapsed < 15000UL) return false;
  float currentLevel = emptyDistanceCM - distanceCM;
  rate = (currentLevel - levelHistory[chosen]) * 60000.0f / (float)elapsed;
  return true;
}

int levelFor(float percent) {
  int desired = percent >= RED_PERCENT ? 2 : (percent >= YELLOW_PERCENT ? 1 : 0);
  if (activeLevel == 2 && percent >= RED_PERCENT - 3.0f) return 2;
  if (activeLevel == 1 && desired == 0 && percent >= YELLOW_PERCENT - 3.0f) return 1;
  return desired;
}

String levelText(int value) {
  if (value == 2) return "КРАСНЫЙ - ОПАСНОСТЬ";
  if (value == 1) return "ЖЕЛТЫЙ - ВНИМАНИЕ";
  if (value == 0) return "ЗЕЛЕНЫЙ - НОРМА";
  return "ОПРЕДЕЛЯЕМ УРОВЕНЬ";
}

String localClock() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "(часы ещё не синхронизированы)";
  struct tm stamp;
  localtime_r(&now, &stamp);
  char buffer[24];
  strftime(buffer, sizeof(buffer), "%d.%m %H:%M:%S", &stamp);
  return String(buffer);
}

void rememberEvent(const String &message) {
  eventMessages[eventNext] = localClock() + " | " + message;
  eventNext = (eventNext + 1) % EVENT_COUNT;
  if (eventCount < EVENT_COUNT) eventCount++;
  Serial.println("DRAINIQ EVENT: " + message);
}

String recentEvents() {
  String out = "DRAINIQ | ЖУРНАЛ СОБЫТИЙ\n";
  if (eventCount == 0) return out + "Пока нет событий после включения ESP32.";
  int amount = eventCount < 7 ? eventCount : 7;
  for (int j = 0; j < amount; j++) {
    int pos = (eventNext - 1 - j + EVENT_COUNT) % EVENT_COUNT;
    out += String(j + 1) + ". " + eventMessages[pos] + "\n";
  }
  out += "События хранятся в RAM и исчезнут после перезапуска.";
  return out;
}

int userIndex(const String &chatId) {
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i] != "" && users[i] == chatId) return i;
  }
  return -1;
}

void indicateStatus() {
  bool net = (WiFi.status() == WL_CONNECTED);
  bool good = hasGoodReading && !sensorLost && millis() - lastGoodReading <= SENSOR_TIMEOUT_MS;
  // Blue = Wi-Fi disconnected; green = normal; red = warning or danger.
  // In warning state red blinks, in danger state red stays on.
  digitalWrite(LED_BLUE_PIN, net ? LOW : HIGH);
  digitalWrite(LED_GREEN_PIN, good && activeLevel == 0 ? HIGH : LOW);
  bool blink = (millis() / 450UL) % 2 == 0;
  bool red = !good ? blink : (activeLevel == 2 || (activeLevel == 1 && blink));
  digitalWrite(LED_RED_PIN, red ? HIGH : LOW);
}

String report() {
  String message = "DRAINIQ | " + String(DEVICE_LABEL) + "\n";
  message += "Время устройства: " + localClock() + "\n";
  if (!hasGoodReading || sensorLost || millis() - lastGoodReading > SENSOR_TIMEOUT_MS) {
    message += "НЕТ СВЕЖИХ ДАННЫХ ОТ ДАТЧИКА";
    if (hasGoodReading) {
      message += "\nПоследнее измерение: ";
      message += String((millis() - lastGoodReading) / 1000UL);
      message += " сек назад";
    }
    return message;
  }

  message += "Статус: ";
  message += levelText(activeLevel);
  message += "\nРасстояние: ";
  message += String(distanceCM, 2);
  message += " см\nЗаполнение: ";
  message += String(fillPercent(distanceCM), 1);
  message += "%\nПустой контейнер: ";
  message += String(emptyDistanceCM, 2);
  message += " см\nСкорость подъема: ";

  float rate = 0.0f;
  if (risingRate(rate)) {
    if (rate > 0.02f) message += "+";
    message += String(rate, 2);
    message += " см/мин";
  } else {
    message += "собираем данные (15-30 сек)";
  }

  message += "\nПоследний сигнал: ";
  message += String((millis() - lastGoodReading) / 1000UL);
  message += " сек назад";
  message += "\nПричина: ";
  if (activeLevel == 2) message += "заполнение достигло красного порога 80%";
  else if (activeLevel == 1) message += "заполнение достигло жёлтого порога 60%";
  else message += "уровень ниже порога 60%";
  message += "\nОблако: ";
  if (lastCloudSuccess == 0) message += "ещё не подтверждено";
  else message += String((millis() - lastCloudSuccess) / 1000UL) + " сек с успешной отправки";
  return message;
}

bool sendTelegram(const String &chatId, const String &message) {
  if (WiFi.status() != WL_CONNECTED || chatId == "") return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN) + "/sendMessage";
  if (!http.begin(client, url)) return false;
  http.setTimeout(5000);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["chat_id"] = chatId;
  doc["text"] = message;
  String payload;
  serializeJson(doc, payload);

  int code = http.POST(payload);
  http.end();
  Serial.print("Telegram send HTTP: ");
  Serial.println(code);
  return code == 200;
}

bool sendEveryone(const String &message) {
  bool anySent = false;
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i] != "" && sendTelegram(users[i], message)) anySent = true;
  }
  return anySent;
}

bool hasUnacknowledgedRedUser() {
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i] != "" && !acknowledgedRed[i]) return true;
  }
  return false;
}

bool sendRedReminderToUnacked(const String &message) {
  bool any = false;
  for (int i = 0; i < MAX_USERS; i++) {
    if (users[i] != "" && !acknowledgedRed[i] && sendTelegram(users[i], message)) any = true;
  }
  return any;
}

void queueNotice(Notice code, bool urgent) {
  pendingNotice = code;
  pendingUrgent = urgent;
  if (urgent) lastNoticeAttempt = millis() - 5000UL;
}

bool noticeIsRelevant(Notice code) {
  if (code == NOTICE_SENSOR_LOST) return sensorLost;
  if (code == NOTICE_RED || code == NOTICE_RED_REMINDER) return !sensorLost && activeLevel == 2;
  if (code == NOTICE_YELLOW) return !sensorLost && activeLevel == 1;
  if (code == NOTICE_GREEN || code == NOTICE_RED_ENDED) return !sensorLost && activeLevel != 2;
  if (code == NOTICE_SENSOR_RECOVERED) return !sensorLost;
  return false;
}

String noticeText(Notice code) {
  if (code == NOTICE_SENSOR_LOST) return "DRAINIQ: ПОТЕРЯН СИГНАЛ ДАТЧИКА!\n" + report();
  if (code == NOTICE_SENSOR_RECOVERED) return "DRAINIQ: сигнал датчика ВОССТАНОВЛЕН.\n" + report();
  if (code == NOTICE_RED) return "DRAINIQ: СРОЧНАЯ ТРЕВОГА!\n" + report();
  if (code == NOTICE_RED_REMINDER) return "DRAINIQ: ОПАСНОСТЬ СОХРАНЯЕТСЯ.\n" + report();
  if (code == NOTICE_RED_ENDED) return "DRAINIQ: опасный уровень снизился.\n" + report();
  if (code == NOTICE_YELLOW) return "DRAINIQ: повышенный уровень воды.\n" + report();
  return "DRAINIQ: уровень в норме.\n" + report();
}

void flushNotice() {
  if (pendingNotice == NOTICE_NONE) return;
  if (!noticeIsRelevant(pendingNotice)) {
    pendingNotice = NOTICE_NONE;
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;
  if (!pendingUrgent && hasSentAlert && millis() - lastAlertSent < ALERT_COOLDOWN_MS) return;
  if (millis() - lastNoticeAttempt < 5000UL) return;

  lastNoticeAttempt = millis();
  // Each /ack pauses repeated RED reminders only for that Telegram user.
  if (pendingNotice == NOTICE_RED_REMINDER && !hasUnacknowledgedRedUser()) {
    pendingNotice = NOTICE_NONE;
    pendingUrgent = false;
    return;
  }
  bool didSend = pendingNotice == NOTICE_RED_REMINDER
    ? sendRedReminderToUnacked(noticeText(pendingNotice))
    : sendEveryone(noticeText(pendingNotice));
  if (didSend) {
    lastAlertSent = millis();
    hasSentAlert = true;
    Serial.println("DRAINIQ automatic notification sent");
    pendingNotice = NOTICE_NONE;
    pendingUrgent = false;
  }
}

void updateStatus(float percent) {
  int desired = levelFor(percent);
  if (desired == activeLevel) {
    pendingLevel = -1;
    pendingCount = 0;
    return;
  }

  if (pendingLevel == desired) pendingCount++;
  else {
    pendingLevel = desired;
    pendingCount = 1;
  }

  if (pendingCount < CONFIRM_COUNT) return;

  int previous = activeLevel;
  activeLevel = desired;
  pendingLevel = -1;
  pendingCount = 0;

  if (desired == 2) {
    for (int i = 0; i < MAX_USERS; i++) acknowledgedRed[i] = false;
    rememberEvent("КРАСНАЯ ТРЕВОГА: заполнение " + String(percent, 1) + "%");
    queueNotice(NOTICE_RED, true);
  }
  else if (previous == 2) {
    rememberEvent("Критический уровень закончился: " + String(percent, 1) + "%");
    queueNotice(NOTICE_RED_ENDED, true);
  }
  else if (desired == 1) {
    rememberEvent("ВНИМАНИЕ: заполнение " + String(percent, 1) + "%");
    queueNotice(NOTICE_YELLOW, false);
  }
  else if (previous >= 0) {
    rememberEvent("Восстановилась НОРМА: " + String(percent, 1) + "%");
    queueNotice(NOTICE_GREEN, false);
  }
}

// USB protocol shared with DRAINIQ LIVE:
//   GET_DEPTH       -> DEPTH_CM:9.00
//   SET_DEPTH:9.00  -> DEPTH_CM:9.00 (after persisting to NVS)
// Invalid settings -> ERR_DEPTH:INVALID
String serialCommand = "";

void reportDepth() {
  Serial.print("DEPTH_CM:");
  Serial.println(emptyDistanceCM, 2);
}

void processSerialCommand(String command) {
  command.trim();
  if (command == "GET_DEPTH") {
    reportDepth();
    return;
  }
  if (!command.startsWith("SET_DEPTH:")) return;

  String input = command.substring(10);
  input.trim();
  char *endptr = nullptr;
  float candidate = strtof(input.c_str(), &endptr);
  if (input.length() == 0 || endptr == input.c_str() || *endptr != '\0' ||
      !isfinite(candidate) || candidate < 2.0f || candidate > 400.0f) {
    Serial.println("ERR_DEPTH:INVALID");
    return;
  }
  if (!prefs.putFloat("depth_cm", candidate)) {
    Serial.println("ERR_DEPTH:SAVE");
    return;
  }
  int oldLevel = activeLevel;
  emptyDistanceCM = candidate;
  // Level-history stores water height. Old data are no longer comparable.
  historyCount = 0;
  historyNext = 0;
  pendingLevel = -1;
  pendingCount = 0;
  pendingNotice = NOTICE_NONE;
  pendingUrgent = false;
  if (hasGoodReading && !sensorLost) {
    // Recalculate immediately so /status cannot show a stale level.
    activeLevel = -1;
    activeLevel = levelFor(fillPercent(distanceCM));
    if (oldLevel == 2 && activeLevel != 2) {
      queueNotice(NOTICE_RED_ENDED, true);
    } else if (oldLevel != 2 && activeLevel == 2) {
      queueNotice(NOTICE_RED, true);
    }
  }
  rememberEvent("Калибровка изменена: " + String(emptyDistanceCM, 2) + " см");
  reportDepth();
}

void handleSerialCommands() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r' || c == '\n') {
      if (serialCommand.length() > 0) {
        processSerialCommand(serialCommand);
        serialCommand = "";
      }
    } else if (c >= 32 && c <= 126 && serialCommand.length() < 64) {
      serialCommand += c;
    }
  }
}

void checkTelegram() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = "https://api.telegram.org/bot" + String(BOT_TOKEN)
    + "/getUpdates?offset=" + String(updateOffset) + "&limit=10";
  if (!http.begin(client, url)) return;
  http.setTimeout(5000);

  int code = http.GET();
  if (code != 200) {
    Serial.print("Telegram read HTTP: ");
    Serial.println(code);
    http.end();
    return;
  }

  String response = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, response)) {
    Serial.println("Telegram JSON error");
    return;
  }

  JsonArray updates = doc["result"].as<JsonArray>();
  long newestOffset = updateOffset;

  for (JsonObject update : updates) {
    long id = update["update_id"].as<long>();
    if (id >= newestOffset) newestOffset = id + 1;
    JsonObject msg = update["message"].as<JsonObject>();
    if (msg.isNull()) continue;

    String type = msg["chat"]["type"] | "";
    if (type != "private") continue;
    String chatId;
    serializeJson(msg["chat"]["id"], chatId);
    String command = msg["text"] | "";
    command.trim();

    if (command == "/start") {
      if (registered(chatId)) sendTelegram(chatId, "DRAINIQ подключен! Команды: /status /events /ack /help");
      else sendTelegram(chatId, "Для подключения: /join СЕКРЕТНЫЙ_КОД");
    } else if (command.startsWith("/join ")) {
      String entered = command.substring(6);
      entered.trim();
      if (entered == JOIN_CODE) {
        if (registerUser(chatId)) sendTelegram(chatId, "DRAINIQ подключен. Ты получаешь предупреждения! /status");
        else sendTelegram(chatId, "Все 5 мест уже заняты.");
      } else sendTelegram(chatId, "Неверный код подключения.");
    } else if (command == "/status" || command == "/level") {
      if (registered(chatId)) sendTelegram(chatId, report());
      else sendTelegram(chatId, "Сначала подключись: /join СЕКРЕТНЫЙ_КОД");
    } else if (command == "/ack" && registered(chatId)) {
      int idx = userIndex(chatId);
      if (idx >= 0 && activeLevel == 2 && !sensorLost) {
        acknowledgedRed[idx] = true;
        rememberEvent("Тревога подтверждена пользователем №" + String(idx + 1));
        sendTelegram(chatId, "✅ Красная тревога подтверждена. Повторные красные напоминания для тебя остановлены до новой тревоги. Общий контроль датчика продолжается.");
      } else {
        sendTelegram(chatId, "Сейчас нет активной красной тревоги для подтверждения.");
      }
    } else if (command == "/events" && registered(chatId)) {
      sendTelegram(chatId, recentEvents());
    } else if (command == "/help" && registered(chatId)) {
      sendTelegram(chatId, "/status — уровень, скорость, калибровка, последнее измерение\n/events — последние события\n/ack — подтвердить красную тревогу и остановить её повторы для себя\n/level — то же, что /status");
    }
  }

  if (newestOffset != updateOffset) {
    updateOffset = newestOffset;
    prefs.putLong("offset", updateOffset);
  }
}

void sendCloudReading() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!hasGoodReading) return;
  if (strlen(THINGSPEAK_WRITE_KEY) < 12 ||
      String(THINGSPEAK_WRITE_KEY) == "YOUR_THINGSPEAK_WRITE_KEY") return;

  WiFiClientSecure cloudClient;
  // Prototype: encrypted HTTPS but without a pinned/trusted CA verification.
  // Do not use for high-stakes unattended deployments. Use a CA certificate in production.
  cloudClient.setInsecure();
  HTTPClient request;
  request.setTimeout(4500);
  if (!request.begin(cloudClient, "https://api.thingspeak.com/update")) return;
  request.addHeader("Content-Type", "application/x-www-form-urlencoded");
  String body = "api_key=" + String(THINGSPEAK_WRITE_KEY);
  int cloudStatus = sensorLost || millis() - lastGoodReading > SENSOR_TIMEOUT_MS ? 3 : activeLevel;
  float rate = 0.0f;
  risingRate(rate);
  body += "&field1=" + String(cloudStatus == 3 ? -1.0f : distanceCM, 2);
  body += "&field2=" + String(cloudStatus == 3 ? -1.0f : fillPercent(distanceCM), 1);
  body += "&field3=" + String(rate, 2);
  body += "&field4=" + String(emptyDistanceCM, 2);
  body += "&field5=" + String(cloudStatus);
  body += "&field6=" + String(WiFi.RSSI());
  int code = request.POST(body);
  lastCloudHttpCode = code;
  String response = code == 200 ? request.getString() : "";
  request.end();
  // ThingSpeak returns entry_id (non-zero) or 0 on rejected updates.
  if (code == 200 && response.toInt() > 0) {
    lastCloudSuccess = millis();
    Serial.println("CLOUD: upload OK, entry " + response);
  } else {
    Serial.println("CLOUD: upload failed, HTTP " + String(code));
  }
}

void serviceWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiWasConnected) {
      wifiWasConnected = true;
      Serial.println("WiFi CONNECTED!");
      Serial.print("ESP32 IP: ");
      Serial.println(WiFi.localIP());
    }
    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    Serial.println("WiFi connection lost!");
  }

  if (millis() - lastWiFiRetry < WIFI_RETRY_MS) return;

  lastWiFiRetry = millis();
  Serial.println("WiFi retrying...");
  WiFi.disconnect(false, false);
  delay(200);
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD);
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_BLUE_PIN, OUTPUT);
  digitalWrite(TRIG_PIN, LOW);

  prefs.begin("drainiq", false);
  for (int i = 0; i < MAX_USERS; i++) {
    String key = keyForUser(i);
    users[i] = prefs.getString(key.c_str(), "");
    if (i > 0 && users[i] == "") {
      String legacyKey = "sub" + String(i);
      users[i] = prefs.getString(legacyKey.c_str(), "");
    }
  }
  updateOffset = prefs.getLong("offset", 0);
  emptyDistanceCM = prefs.getFloat("depth_cm", DEFAULT_EMPTY_DISTANCE_CM);
  if (!isfinite(emptyDistanceCM) || emptyDistanceCM < 2.0f || emptyDistanceCM > 400.0f) {
    emptyDistanceCM = DEFAULT_EMPTY_DISTANCE_CM;
  }
  reportDepth();

  Serial.println("DRAINIQ 3.2 STARTING...");
  // Kazakhstan / Astana currently UTC+05. For other locations change offset.
  configTime(5 * 3600, 0, "pool.ntp.org", "time.google.com");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.begin(WIFI_NAME, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 20000UL) {
    Serial.print(".");
    delay(500);
  }

  wifiWasConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiWasConnected) {
    Serial.println("\nWiFi CONNECTED!");
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed! Will retry every 30s.");
    Serial.print("WiFi status code: ");
    Serial.println((int)WiFi.status());
  }
  lastWiFiRetry = millis();
  lastGoodReading = millis();
  rememberEvent("Устройство запущено");
  Serial.println("DRAINIQ READY!");
}

void loop() {
  handleSerialCommands();
  indicateStatus();
  serviceWiFi();

  if (millis() - lastMeasure >= 1200UL) {
    lastMeasure = millis();
    float candidate = measureDistance();
    bool accepted = false;

    if (candidate < 0) {
      Serial.println("No signal");
    } else if (distanceCM >= 0 && fabsf(candidate - distanceCM) > 2.5f
               && !(suspectedDistance >= 0 && fabsf(candidate - suspectedDistance) < 1.5f)) {
      suspectedDistance = candidate;
      Serial.println("Filtered spike");
    } else {
      suspectedDistance = -1;
      distanceCM = candidate;
      lastGoodReading = millis();
      firstRejectedReading = 0;
      hasGoodReading = true;
      accepted = true;
      rememberLevel(emptyDistanceCM - distanceCM);
      Serial.print("Distance: ");
      Serial.print(distanceCM, 2);
      Serial.println(" cm");
    }

    if (sensorLost) {
      if (accepted) {
        recoveryCount++;
        if (recoveryCount >= CONFIRM_COUNT) {
          sensorLost = false;
          recoveryCount = 0;
          rememberEvent("Сигнал датчика восстановлен");
          queueNotice(NOTICE_SENSOR_RECOVERED, true);
          pendingLevel = -1;
          pendingCount = 0;
          updateStatus(fillPercent(distanceCM));
        }
      } else recoveryCount = 0;
    } else if (accepted) {
      updateStatus(fillPercent(distanceCM));
    }

    if (!sensorLost && !accepted) {
      if (firstRejectedReading == 0) firstRejectedReading = millis();
      if (millis() - firstRejectedReading > SENSOR_TIMEOUT_MS) {
        sensorLost = true;
        recoveryCount = 0;
        pendingLevel = -1;
        pendingCount = 0;
        historyCount = 0;
        historyNext = 0;
        Serial.println("DRAINIQ: SENSOR LOST");
        rememberEvent("Сигнал датчика потерян");
        queueNotice(NOTICE_SENSOR_LOST, true);
      }
    }
  }

  if (millis() - lastTelegram >= TELEGRAM_POLL_MS) {
    lastTelegram = millis();
    checkTelegram();
  }

  if (pendingNotice == NOTICE_NONE && !sensorLost && activeLevel == 2 &&
      hasSentAlert && millis() - lastAlertSent >= ALERT_COOLDOWN_MS &&
      hasGoodReading && millis() - lastGoodReading <= SENSOR_TIMEOUT_MS) {
    queueNotice(NOTICE_RED_REMINDER, false);
  }

  flushNotice();
  if (millis() - lastCloudAttempt >= CLOUD_INTERVAL_MS) {
    lastCloudAttempt = millis();
    sendCloudReading();
  }
}
