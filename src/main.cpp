#include <Arduino.h>
#include <Adafruit_Sensor.h>
#include <DHT.h>
#include <LiquidCrystal.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <esp_bt.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <esp_arduino_version.h>
#include <time.h>
#include <stdio.h>

// ============================ HARDWARE ============================
static constexpr uint8_t DHT_SENSOR_PIN = 32;
static constexpr uint8_t DHT_SENSOR_TYPE = DHT11;

static constexpr uint8_t BUTTON4 = 15;
static constexpr uint8_t BUTTON3 = 2;
static constexpr uint8_t BUTTON2 = 0;
static constexpr uint8_t BUTTON1 = 4;

static constexpr uint8_t RGB_R = 23;
static constexpr uint8_t RGB_G = 19;
static constexpr uint8_t RGB_B = 18;
static constexpr uint8_t STATUS_LED = 22;
static constexpr uint8_t BUZZER_PIN = 13;

static constexpr uint8_t LCD_COLS = 20;
static constexpr uint8_t LCD_ROWS = 4;

DHT dht_sensor(DHT_SENSOR_PIN, DHT_SENSOR_TYPE);
LiquidCrystal lcd(12, 27, 14, 26, 25, 33);

// ============================ SERVICES ============================
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;
WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);

static const char *DEFAULT_DEVICE_NAME = "stazionemeteo";
static const char *FW_VERSION = "2.5.1";
String deviceName = DEFAULT_DEVICE_NAME;
// Public build: no private bootstrap credentials. Configure Wi-Fi via the AP.
// Optional bootstrap values are inserted only once into the 3-slot store.
static const char *BOOTSTRAP_WIFI_SSID = "";
static const char *BOOTSTRAP_WIFI_PASS = "";
static const char *NTP_SERVER = "pool.ntp.org";
// Europe/Rome, including automatic CET/CEST daylight-saving changes.
static const char *TZ_ITALY = "CET-1CEST,M3.5.0,M10.5.0/3";

static constexpr uint32_t DHT_INTERVAL_MS = 2500;
static constexpr uint32_t LCD_SERVICE_MS = 90;
static constexpr uint32_t SCREEN_REFRESH_MS = 250;
static constexpr uint32_t MQTT_PUBLISH_MS = 30000;
static constexpr uint32_t MQTT_RETRY_MS = 10000;
static constexpr uint32_t HISTORY_INTERVAL_MS = 300000;
static constexpr uint16_t HISTORY_POINTS = 288;
static constexpr uint8_t LOG_CAPACITY = 60;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 9000;
static constexpr uint32_t WIFI_RETRY_AFTER_DROP_MS = 15000;
static constexpr uint32_t LCD_SCROLL_WAIT_MS = 1800;
static constexpr uint32_t LCD_SCROLL_STEP_MS = 350;
static constexpr uint32_t HOME_WEATHER_MS = 5000;
static constexpr uint32_t HOME_CLOCK_MS = 30000;
static constexpr uint32_t BPM_SAVE_DEFER_MS = 1800;
static constexpr uint8_t MAX_WIFI_NETWORKS = 3;
static constexpr uint8_t MAX_SCAN_RESULTS = 20;

// ============================ STATE ============================
enum class UiMode : uint8_t {
  HOME,
  SONG_LIST,
  SONG_PLAYING,
  TIMER_MENU,
  STOPWATCH_VIEW,
  COUNTDOWN_SET_H,
  COUNTDOWN_SET_M,
  COUNTDOWN_SET_S,
  COUNTDOWN_VIEW,
  NCC_SET_M,
  NCC_SET_S,
  NCC_READY,
  NCC_VIEW,
  AP_MODE,
  OTA
};

enum class HomePage : uint8_t {
  WEATHER,
  CLOCK
};

UiMode uiMode = UiMode::HOME;

struct WifiCredential {
  String ssid;
  String pass;
};

WifiCredential savedWifi[MAX_WIFI_NETWORKS];
uint8_t savedWifiCount = 0;

struct ScanItem {
  String ssid;
  int32_t rssi = -127;
  wifi_auth_mode_t auth = WIFI_AUTH_OPEN;
};

ScanItem scanItems[MAX_SCAN_RESULTS];
uint8_t scanCount = 0;
bool scanRunning = false;
int8_t wifiCandidateOrder[MAX_WIFI_NETWORKS] = {-1, -1, -1};
uint8_t wifiCandidateCount = 0;
uint8_t wifiCandidatePos = 0;
bool wifiConnecting = false;
uint32_t wifiConnectStarted = 0;
uint32_t wifiLastLostAt = 0;
bool apActive = false;
bool bootConnectionSequence = false;
bool savePendingWifiOnSuccess = false;
String pendingWifiSsid;
String pendingWifiPass;

float humidity = NAN;
float temperatureC = NAN;
uint32_t lastDhtRead = 0;

String desiredLine[LCD_ROWS];
String visibleLine[LCD_ROWS];
// Logical LCD framebuffer. Codes 0..7 are CGRAM custom glyphs; ordinary
// screens use printable ASCII codes. The web UI renders this same 20x4
// framebuffer dot-by-dot instead of approximating it with proportional text.
uint8_t lcdCellCodes[LCD_ROWS][LCD_COLS];
uint8_t lcdCustomGlyphs[8][8];
bool lcdGraphicsMode = false;
bool lcdForceRedraw = true;
int8_t loadedClockStyle = -1;
String lastClockHm = "";
uint16_t scrollOffset[LCD_ROWS] = {0, 0, 0, 0};
uint32_t scrollEpoch[LCD_ROWS] = {0, 0, 0, 0};
uint32_t lastLcdService = 0;
uint32_t lastScreenRefresh = 0;

bool statusLedState = false;
uint32_t lastStatusLedToggle = 0;

int selectedIndex = 0;
HomePage homePage = HomePage::WEATHER;
uint32_t homePageSince = 0;
uint8_t clockStyle = 0; // 0 Morbido, 1 Classico, 2 Sottile, 3 Punti, 4 Tech

struct ButtonDebounce {
  uint8_t pin;
  bool stable;
  bool lastRaw;
  uint32_t changedAt;
};

ButtonDebounce buttons[4] = {
  {BUTTON1, HIGH, HIGH, 0},
  {BUTTON2, HIGH, HIGH, 0},
  {BUTTON3, HIGH, HIGH, 0},
  {BUTTON4, HIGH, HIGH, 0}
};

bool colorFlashActive = false;
uint32_t colorFlashUntil = 0;
uint8_t rgbStateR = 0;
uint8_t rgbStateG = 0;
uint8_t rgbStateB = 0;

// MQTT configuration.
String mqttHost;
uint16_t mqttPort = 1883;
String mqttUser;
String mqttPass;
String mqttTopic = "stazionemeteo/sensor";
bool mqttEnabled = false;
uint32_t lastMqttAttempt = 0;
uint32_t lastMqttPublish = 0;

// Non-blocking player state. Each song keeps its own BPM in NVS.
static constexpr uint8_t SONG_COUNT = 37;
int tempo = 120;
uint16_t songBpm[SONG_COUNT];
bool bpmDirty = false;
uint32_t bpmDirtySince = 0;
bool songPlaying = false;
int currentSong = -1;
uint16_t currentNoteIndex = 0;
uint32_t noteEndsAt = 0;
uint32_t toneStopsAt = 0;
bool toneIsOn = false;

// Timer state. Stopwatch and countdown continue in background when the user
// returns to another screen.
bool stopwatchRunning = false;
uint32_t stopwatchStartedAt = 0;
uint32_t stopwatchAccumulatedMs = 0;
uint32_t stopwatchLapMs = 0;

uint8_t countdownSetHours = 0;
uint8_t countdownSetMinutes = 5;
uint8_t countdownSetSeconds = 0;
uint32_t countdownInitialMs = 5UL * 60UL * 1000UL;
uint32_t countdownRemainingMs = 5UL * 60UL * 1000UL;
uint32_t countdownEndsAt = 0;
bool countdownRunning = false;
bool countdownAlarm = false;

// Shared finite timer alarm: exactly three beep-beep pairs, then silence.
// The alarm state remains visible after the sound has finished.
uint8_t timerAlarmPhase = 0;
uint8_t timerAlarmPairsCompleted = 0;
uint32_t timerAlarmPhaseAt = 0;
bool timerAlarmSoundFinished = false;

// Distinct short warning tone at 3, 2 and 1 seconds before expiry.
int8_t countdownLastPreBeepSecond = -1;
int8_t nccLastPreBeepSecond = -1;
uint32_t timerPreBeepUntil = 0;

// Nomi, Cose e Citta game. Duration is configured once per game session;
// used letters are reset only when leaving and entering the game again.
uint8_t nccSetMinutes = 2;
uint8_t nccSetSeconds = 0;
uint32_t nccDefaultMs = 2UL * 60UL * 1000UL;
uint32_t nccRemainingMs = 2UL * 60UL * 1000UL;
uint32_t nccEndsAt = 0;
bool nccRunning = false;
bool nccAlarm = false;
bool nccSessionActive = false;
char nccLetter = '-';
uint32_t nccUsedLettersMask = 0; // bits A..Z
bool nccUrgentLedOn = false;
uint32_t nccUrgentLedAt = 0;

// mDNS can only be started once per active station session.
bool mdnsStarted = false;

// Alarm configuration.
bool alarmsEnabled = false;
float alarmTempMin = 5.0f;
float alarmTempMax = 35.0f;
float alarmHumidityMin = 20.0f;
float alarmHumidityMax = 80.0f;
String alarmTopic = "stazionemeteo/alarm";
bool alarmTempLow = false;
bool alarmTempHigh = false;
bool alarmHumidityLow = false;
bool alarmHumidityHigh = false;
bool alarmPublishPending = false;

// 24h local history: 288 samples x 5 minutes. It intentionally lives in RAM;
// a reboot starts a new history and does not wear flash with periodic writes.
struct HistoryPoint {
  uint32_t timestamp;
  float temperature;
  float humidity;
};
HistoryPoint history[HISTORY_POINTS];
uint16_t historyHead = 0;
uint16_t historyCount = 0;
uint32_t lastHistorySample = 0;

// Small in-RAM circular event log.
struct LogEntry {
  uint32_t uptimeMs;
  String text;
};
LogEntry eventLog[LOG_CAPACITY];
uint8_t logHead = 0;
uint8_t logCount = 0;

esp_reset_reason_t bootResetReason = ESP_RST_UNKNOWN;
esp_sleep_wakeup_cause_t bootWakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
bool watchdogRegistered = false;

// Forward declaration needed by handleButton() with the C++11 toolchain used by
// Arduino-ESP32 2.0.14 / PlatformIO Espressif32 6.5.0.
void goToDeepSleep();
void buzzerTone(uint16_t frequency);
void stopSong();
uint32_t nccCurrentRemainingMs();

// ============================ HELPERS ============================
String jsonEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    if (c == '\\' || c == '"') { out += '\\'; out += c; }
    else if (c == '\n') out += "\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if ((uint8_t)c >= 0x80) out += '#'; // keep JSON valid when LCD uses 0xFF block glyphs
    else out += c;
  }
  return out;
}

String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else out += c;
  }
  return out;
}

String uiModeName() {
  switch (uiMode) {
    case UiMode::HOME: return homePage == HomePage::CLOCK ? "home_clock" : "home_weather";
    case UiMode::SONG_LIST: return "song_list";
    case UiMode::SONG_PLAYING: return "song_playing";
    case UiMode::TIMER_MENU: return "timer_menu";
    case UiMode::STOPWATCH_VIEW: return "stopwatch";
    case UiMode::COUNTDOWN_SET_H: return "countdown_set_hours";
    case UiMode::COUNTDOWN_SET_M: return "countdown_set_minutes";
    case UiMode::COUNTDOWN_SET_S: return "countdown_set_seconds";
    case UiMode::COUNTDOWN_VIEW: return "countdown";
    case UiMode::NCC_SET_M: return "ncc_set_minutes";
    case UiMode::NCC_SET_S: return "ncc_set_seconds";
    case UiMode::NCC_READY: return "ncc_ready";
    case UiMode::NCC_VIEW: return "ncc_game";
    case UiMode::AP_MODE: return "ap_mode";
    case UiMode::OTA: return "ota";
  }
  return "unknown";
}

String sanitizeDeviceName(String name) {
  name.trim();
  name.toLowerCase();
  String out;
  out.reserve(32);
  bool lastDash = false;
  for (size_t i = 0; i < name.length() && out.length() < 31; ++i) {
    char c = name.charAt(i);
    bool valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (valid) {
      out += c;
      lastDash = false;
    } else if ((c == '-' || c == '_' || c == ' ') && out.length() && !lastDash) {
      out += '-';
      lastDash = true;
    }
  }
  while (out.endsWith("-")) out.remove(out.length() - 1);
  if (!out.length()) out = DEFAULT_DEVICE_NAME;
  return out;
}

String resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "other_wdt";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

String wakeCauseName(esp_sleep_wakeup_cause_t c) {
  switch (c) {
    case ESP_SLEEP_WAKEUP_EXT0: return "ext0_button";
    case ESP_SLEEP_WAKEUP_EXT1: return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return "touch";
    case ESP_SLEEP_WAKEUP_ULP: return "ulp";
    default: return "undefined";
  }
}

void addLog(const String &message) {
  eventLog[logHead].uptimeMs = millis();
  eventLog[logHead].text = message;
  logHead = (logHead + 1) % LOG_CAPACITY;
  if (logCount < LOG_CAPACITY) ++logCount;
  Serial.println("[LOG] " + message);
}

uint32_t historyTimestamp() {
  time_t now = time(nullptr);
  if (now > 1600000000) return (uint32_t)now;
  return millis() / 1000;
}

void recordHistoryIfDue(bool force = false) {
  if (isnan(temperatureC) || isnan(humidity)) return;
  uint32_t now = millis();
  if (!force && historyCount && now - lastHistorySample < HISTORY_INTERVAL_MS) return;
  lastHistorySample = now;
  history[historyHead].timestamp = historyTimestamp();
  history[historyHead].temperature = temperatureC;
  history[historyHead].humidity = humidity;
  historyHead = (historyHead + 1) % HISTORY_POINTS;
  if (historyCount < HISTORY_POINTS) ++historyCount;
}

String alarmSummary() {
  String s;
  if (alarmTempLow) s += "temperatura_bassa,";
  if (alarmTempHigh) s += "temperatura_alta,";
  if (alarmHumidityLow) s += "umidita_bassa,";
  if (alarmHumidityHigh) s += "umidita_alta,";
  if (s.endsWith(",")) s.remove(s.length() - 1);
  return s.length() ? s : "ok";
}

void evaluateAlarms() {
  bool oldAny = alarmTempLow || alarmTempHigh || alarmHumidityLow || alarmHumidityHigh;
  bool oldTL = alarmTempLow, oldTH = alarmTempHigh, oldHL = alarmHumidityLow, oldHH = alarmHumidityHigh;

  if (!alarmsEnabled || isnan(temperatureC) || isnan(humidity)) {
    alarmTempLow = alarmTempHigh = alarmHumidityLow = alarmHumidityHigh = false;
  } else {
    alarmTempLow = temperatureC < alarmTempMin;
    alarmTempHigh = temperatureC > alarmTempMax;
    alarmHumidityLow = humidity < alarmHumidityMin;
    alarmHumidityHigh = humidity > alarmHumidityMax;
  }

  bool newAny = alarmTempLow || alarmTempHigh || alarmHumidityLow || alarmHumidityHigh;
  if (oldTL != alarmTempLow || oldTH != alarmTempHigh || oldHL != alarmHumidityLow || oldHH != alarmHumidityHigh) {
    alarmPublishPending = true;
    addLog(String("Allarmi: ") + alarmSummary());
  } else if (oldAny != newAny) {
    alarmPublishPending = true;
  }
}

String buttonTitle(uint8_t id) {
  if (uiMode == UiMode::OTA) return "OTA in corso";

  if (songPlaying) {
    if (id == 1) return "-20 BPM";
    if (id == 2) return "+20 BPM";
    if (id == 3) return "—";
    return "Stop";
  }

  if (uiMode == UiMode::SONG_LIST) {
    if (id == 1) return "Scendi";
    if (id == 2) return "Sali";
    if (id == 3) return "Riproduci";
    return "Indietro";
  }

  if (uiMode == UiMode::TIMER_MENU) {
    if (id == 1) return "Cronometro";
    if (id == 2) return "Countdown";
    if (id == 3) return "Nomi, Cose e Citta";
    return "Indietro";
  }

  if (uiMode == UiMode::STOPWATCH_VIEW) {
    if (id == 1) return stopwatchRunning ? "Pausa" : "Avvia";
    if (id == 2) return "Azzera";
    if (id == 3) return "Giro";
    return "Indietro";
  }

  if (uiMode == UiMode::COUNTDOWN_SET_H) return id == 1 ? "+1 ora" : id == 2 ? "-1 ora" : id == 3 ? "Minuti" : "Indietro";
  if (uiMode == UiMode::COUNTDOWN_SET_M) return id == 1 ? "+1 minuto" : id == 2 ? "-1 minuto" : id == 3 ? "Secondi" : "Ore";
  if (uiMode == UiMode::COUNTDOWN_SET_S) return id == 1 ? "+1 secondo" : id == 2 ? "-1 secondo" : id == 3 ? "Avvia" : "Minuti";

  if (uiMode == UiMode::COUNTDOWN_VIEW) {
    if (countdownAlarm) return id == 1 ? "Chiudi allarme" : id == 2 ? "Ripeti" : id == 3 ? "Nuovo countdown" : "Indietro";
    return id == 1 ? (countdownRunning ? "Pausa" : "Riprendi") : id == 2 ? "+1 minuto" : id == 3 ? "Reimposta" : "Indietro";
  }

  if (uiMode == UiMode::NCC_SET_M) return id == 1 ? "+1 minuto" : id == 2 ? "-1 minuto" : id == 3 ? "Secondi" : "Esci";
  if (uiMode == UiMode::NCC_SET_S) return id == 1 ? "+1 secondo" : id == 2 ? "-1 secondo" : id == 3 ? "Conferma" : "Minuti";
  if (uiMode == UiMode::NCC_READY) return id == 1 ? "Avvia manche" : id == 2 ? "Durata" : id == 3 ? "Avvia manche" : "Esci";
  if (uiMode == UiMode::NCC_VIEW) {
    if (nccAlarm) return id == 1 ? "Nuova manche" : id == 2 ? "Chiudi avviso" : id == 3 ? "Nuova manche" : "Esci";
    return id == 1 ? (nccRunning ? "Pausa" : "Riprendi") : id == 2 ? "Ricomincia" : id == 3 ? "Nuova manche" : "Esci";
  }

  if (id == 1) return homePage == HomePage::CLOCK ? "Mostra meteo" : "Mostra orologio";
  if (id == 2) return "Menu musica";
  if (id == 3) return "Menu timer";
  return "Deep sleep";
}

String buttonHint(uint8_t id) {
  if (uiMode == UiMode::OTA) return "Aggiornamento OTA in corso";
  if (songPlaying) return id == 1 ? "Riduce di 20 BPM il brano e memorizza il valore" : id == 2 ? "Aumenta di 20 BPM il brano e memorizza il valore" : id == 3 ? "Non assegnato durante la riproduzione" : "Interrompe il brano e torna all'elenco";
  if (uiMode == UiMode::SONG_LIST) return id == 1 ? "Seleziona il brano seguente" : id == 2 ? "Seleziona il brano precedente" : id == 3 ? "Avvia il brano con i BPM memorizzati" : "Torna alla schermata principale";
  if (uiMode == UiMode::TIMER_MENU) return id == 1 ? "Apre il cronometro, che può continuare in background" : id == 2 ? "Apre il countdown con ore, minuti e secondi" : id == 3 ? "Avvia una sessione di Nomi, Cose e Città con lettere casuali non ripetute" : "Torna alla schermata principale";
  if (uiMode == UiMode::STOPWATCH_VIEW) return id == 1 ? "Avvia o mette in pausa il cronometro" : id == 2 ? "Azzera il cronometro" : id == 3 ? "Memorizza il giro corrente" : "Torna al menu Timer senza fermare il cronometro";
  if (uiMode == UiMode::COUNTDOWN_SET_H || uiMode == UiMode::COUNTDOWN_SET_M || uiMode == UiMode::COUNTDOWN_SET_S) return id == 1 ? "Incrementa il valore corrente" : id == 2 ? "Decrementa il valore corrente" : id == 3 ? (uiMode == UiMode::COUNTDOWN_SET_S ? "Avvia il countdown" : "Passa al campo successivo") : "Torna al passaggio precedente";
  if (uiMode == UiMode::COUNTDOWN_VIEW) {
    if (countdownAlarm) return id == 1 ? "Ferma l'allarme sonoro se è ancora nei tre cicli" : id == 2 ? "Riparte con la stessa durata" : id == 3 ? "Torna all'impostazione del countdown" : "Torna al menu Timer";
    return id == 1 ? "Pausa o riprende mantenendo il residuo" : id == 2 ? "Aggiunge un minuto" : id == 3 ? "Riporta alla durata iniziale" : "Torna al menu Timer; il countdown continua in background";
  }
  if (uiMode == UiMode::NCC_SET_M || uiMode == UiMode::NCC_SET_S) return id == 1 ? "Aumenta la durata standard delle manche" : id == 2 ? "Riduce la durata standard delle manche" : id == 3 ? (uiMode == UiMode::NCC_SET_S ? "Salva la durata per questa e le future sessioni" : "Passa ai secondi") : "Torna indietro; uscendo dal gioco si azzera l'elenco delle lettere usate";
  if (uiMode == UiMode::NCC_READY) return id == 1 || id == 3 ? "Estrae una lettera casuale non ancora usata e avvia contemporaneamente il timer" : id == 2 ? "Modifica la durata predefinita delle manche" : "Esce dal gioco e rende di nuovo disponibili tutte le lettere";
  if (uiMode == UiMode::NCC_VIEW) {
    if (nccAlarm) return id == 1 || id == 3 ? "Avvia subito una nuova manche con una nuova lettera" : id == 2 ? "Chiude l'avviso sonoro e mantiene la manche terminata" : "Esce dal gioco e azzera lo storico lettere";
    return id == 1 ? "Pausa o riprende il timer della manche" : id == 2 ? "Riparte dall'inizio con la stessa lettera" : id == 3 ? "Avvia subito una nuova manche con una lettera non ancora usata" : "Esce dal gioco e azzera lo storico lettere";
  }
  return id == 1 ? "Salta fra dati meteo e orologio HH:MM" : id == 2 ? "Apre l'elenco delle melodie" : id == 3 ? "Apre Cronometro, Countdown e Nomi Cose e Città" : "Mette l'ESP32 in deep sleep; il tasto rosso lo risveglia";
}

String pad20(const String &s) {
  String out = s.substring(0, LCD_COLS);
  while (out.length() < LCD_COLS) out += ' ';
  return out;
}

void colora(int r, int g, int b) {
  rgbStateR = r ? 1 : 0;
  rgbStateG = g ? 1 : 0;
  rgbStateB = b ? 1 : 0;
  digitalWrite(RGB_R, rgbStateR ? HIGH : LOW);
  digitalWrite(RGB_G, rgbStateG ? HIGH : LOW);
  digitalWrite(RGB_B, rgbStateB ? HIGH : LOW);
}

String rgbMeaning() {
  if (countdownAlarm) return "countdown terminato";
  if (nccAlarm) return "Nomi Cose Citta: tempo scaduto";
  if (nccRunning && nccCurrentRemainingMs() <= 10000UL) return "Nomi Cose Citta: ultimi 10 secondi";
  if (nccRunning) return "Nomi Cose Citta: manche attiva";
  if (uiMode == UiMode::NCC_VIEW && !nccRunning) return "Nomi Cose Citta: pausa";
  if (uiMode == UiMode::NCC_SET_M || uiMode == UiMode::NCC_SET_S || uiMode == UiMode::NCC_READY) return "Nomi Cose Citta: pronto";
  if (songPlaying) return "musica";
  if (countdownRunning) return "countdown attivo";
  if (uiMode == UiMode::COUNTDOWN_VIEW && !countdownRunning) return "countdown in pausa";
  if (stopwatchRunning) return "cronometro attivo";
  if (uiMode == UiMode::TIMER_MENU || uiMode == UiMode::STOPWATCH_VIEW || uiMode == UiMode::COUNTDOWN_SET_H || uiMode == UiMode::COUNTDOWN_SET_M || uiMode == UiMode::COUNTDOWN_SET_S) return "menu timer";
  if (wifiConnecting || scanRunning) return "attività Wi-Fi";
  if (apActive && WiFi.status() != WL_CONNECTED) return "access point";
  if (WiFi.status() == WL_CONNECTED) return "connesso";
  return "non connesso";
}

void restoreStatusColor() {
  if (countdownAlarm || nccAlarm) colora(1, 0, 0);
  else if (nccRunning) colora(0, 1, 0);              // green; serviceTimers flashes yellow in last 10 s
  else if (uiMode == UiMode::NCC_VIEW && !nccRunning) colora(1, 1, 0); // yellow pause
  else if (uiMode == UiMode::NCC_SET_M || uiMode == UiMode::NCC_SET_S || uiMode == UiMode::NCC_READY) colora(0, 0, 1); // blue ready/setup
  else if (countdownRunning) colora(1, 0, 1);         // magenta
  else if (uiMode == UiMode::COUNTDOWN_VIEW && !countdownRunning) colora(1, 1, 0);
  else if (stopwatchRunning) colora(0, 1, 1);         // cyan
  else if (uiMode == UiMode::TIMER_MENU || uiMode == UiMode::STOPWATCH_VIEW || uiMode == UiMode::COUNTDOWN_SET_H || uiMode == UiMode::COUNTDOWN_SET_M || uiMode == UiMode::COUNTDOWN_SET_S) colora(0, 0, 1);
  else if (wifiConnecting || scanRunning) colora(1, 1, 0);
  else if (apActive && WiFi.status() != WL_CONNECTED) colora(0, 0, 1);
  else if (WiFi.status() == WL_CONNECTED) colora(0, 1, 0);
  else colora(1, 0, 0);
}

void flashButtonColor(uint8_t id) {
  if (id == 1) colora(1, 0, 1);
  else if (id == 2) colora(1, 1, 0);
  else if (id == 3) colora(0, 0, 1);
  else colora(1, 0, 0);
  colorFlashActive = true;
  colorFlashUntil = millis() + 180;
}

// ============================ LCD + SCROLL ============================
void invalidateTextDisplay() {
  for (uint8_t row = 0; row < LCD_ROWS; ++row) visibleLine[row] = "";
  lcdForceRedraw = true;
}

void enterTextDisplayMode() {
  if (lcdGraphicsMode) {
    lcdGraphicsMode = false;
    lastClockHm = "";
    invalidateTextDisplay();
  }
}

void setScreenLine(uint8_t row, const String &text) {
  if (row >= LCD_ROWS) return;
  enterTextDisplayMode();
  if (desiredLine[row] != text) {
    desiredLine[row] = text;
    scrollOffset[row] = 0;
    scrollEpoch[row] = millis();
  }
}

void writeTrackedCell(uint8_t row, uint8_t col, uint8_t code, bool force = false) {
  if (row >= LCD_ROWS || col >= LCD_COLS) return;
  if (force || lcdForceRedraw || lcdCellCodes[row][col] != code) {
    lcd.setCursor(col, row);
    lcd.write(code);
    lcdCellCodes[row][col] = code;
  }
}

void serviceDisplay() {
  if (lcdGraphicsMode) return;

  const uint32_t now = millis();
  if (now - lastLcdService < LCD_SERVICE_MS && !lcdForceRedraw) return;
  lastLcdService = now;

  for (uint8_t row = 0; row < LCD_ROWS; ++row) {
    String rendered;
    const String &src = desiredLine[row];

    if (src.length() <= LCD_COLS) {
      rendered = pad20(src);
    } else {
      const uint16_t maxOffset = src.length() - LCD_COLS;
      const uint32_t elapsed = now - scrollEpoch[row];

      if (elapsed < LCD_SCROLL_WAIT_MS) {
        scrollOffset[row] = 0;
      } else {
        const uint32_t step = (elapsed - LCD_SCROLL_WAIT_MS) / LCD_SCROLL_STEP_MS;
        const uint32_t cycle = maxOffset + 7;
        const uint32_t pos = step % cycle;
        scrollOffset[row] = (pos <= maxOffset) ? pos : maxOffset;
        if (pos == cycle - 1) scrollEpoch[row] = now;
      }

      rendered = src.substring(scrollOffset[row], scrollOffset[row] + LCD_COLS);
      rendered = pad20(rendered);
    }

    bool changed = visibleLine[row] != rendered;
    if (changed || lcdForceRedraw) {
      visibleLine[row] = rendered;
      for (uint8_t col = 0; col < LCD_COLS; ++col) {
        uint8_t code = (uint8_t)rendered.charAt(col);
        if (code < 32 || code > 126) code = '?';
        writeTrackedCell(row, col, code, lcdForceRedraw);
      }
    }
  }
  lcdForceRedraw = false;
}

String localTimeString() {
  struct tm timeinfo;
  char buffer[21];
  if (!getLocalTime(&timeinfo, 10)) return "Ora non sincronizzata";
  strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}

String localHmString() {
  struct tm timeinfo;
  char buffer[6];
  if (!getLocalTime(&timeinfo, 10)) return "--:--";
  strftime(buffer, sizeof(buffer), "%H:%M", &timeinfo);
  return String(buffer);
}

const char *clockStyleName(uint8_t style) {
  switch (style) {
    case 0: return "Morbido";
    case 1: return "Arrotondato";
    case 2: return "Sottile";
    case 3: return "Punti";
    case 4: return "Tech";
  }
  return "Morbido";
}

// True pixel fonts for the HD44780 CGRAM. The eight glyphs form a
// 7-segment-like font at 3 cells x 4 rows per digit. They are not printable
// ASCII pieces: every stroke is a real 5x8 pixel bitmap stored in CGRAM.
//
// Glyph semantics, identical in every style:
// 0 horizontal at bottom, 1 horizontal at top,
// 2 left vertical, 3 right vertical,
// 4 left vertical + bottom horizontal, 5 right vertical + bottom horizontal,
// 6 left vertical + top horizontal,    7 right vertical + top horizontal.
static const uint8_t CLOCK_GLYPHS[5][8][8] = {
  { // 0 - Morbido
    {0x00,0x00,0x00,0x00,0x00,0x0E,0x1F,0x1F},
    {0x1F,0x1F,0x0E,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18},
    {0x03,0x03,0x03,0x03,0x03,0x03,0x03,0x03},
    {0x18,0x18,0x18,0x18,0x18,0x1E,0x1F,0x1F},
    {0x03,0x03,0x03,0x03,0x03,0x0F,0x1F,0x1F},
    {0x1F,0x1F,0x1E,0x18,0x18,0x18,0x18,0x18},
    {0x1F,0x1F,0x0F,0x03,0x03,0x03,0x03,0x03}
  },
  { // 1 - Arrotondato
    {0x00,0x00,0x00,0x00,0x00,0x04,0x0E,0x1F},
    {0x1F,0x0E,0x04,0x00,0x00,0x00,0x00,0x00},
    {0x10,0x18,0x18,0x18,0x18,0x18,0x18,0x10},
    {0x01,0x03,0x03,0x03,0x03,0x03,0x03,0x01},
    {0x10,0x18,0x18,0x18,0x18,0x1C,0x1E,0x1F},
    {0x01,0x03,0x03,0x03,0x03,0x07,0x0F,0x1F},
    {0x1F,0x1E,0x1C,0x18,0x18,0x18,0x18,0x10},
    {0x1F,0x0F,0x07,0x03,0x03,0x03,0x03,0x01}
  },
  { // 2 - Sottile
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    {0x1F,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x10},
    {0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x01},
    {0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    {0x01,0x01,0x01,0x01,0x01,0x01,0x01,0x1F},
    {0x1F,0x10,0x10,0x10,0x10,0x10,0x10,0x10},
    {0x1F,0x01,0x01,0x01,0x01,0x01,0x01,0x01}
  },
  { // 3 - Punti
    {0x00,0x00,0x00,0x00,0x00,0x15,0x00,0x15},
    {0x15,0x00,0x15,0x00,0x00,0x00,0x00,0x00},
    {0x10,0x00,0x10,0x00,0x10,0x00,0x10,0x00},
    {0x01,0x00,0x01,0x00,0x01,0x00,0x01,0x00},
    {0x10,0x00,0x10,0x00,0x10,0x15,0x00,0x15},
    {0x01,0x00,0x01,0x00,0x01,0x15,0x00,0x15},
    {0x15,0x00,0x15,0x00,0x10,0x00,0x10,0x00},
    {0x15,0x00,0x15,0x00,0x01,0x00,0x01,0x00}
  },
  { // 4 - Tech
    {0x00,0x00,0x00,0x00,0x00,0x1F,0x00,0x1F},
    {0x1F,0x00,0x1F,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x18,0x00,0x18,0x18,0x00,0x18,0x18},
    {0x03,0x03,0x00,0x03,0x03,0x00,0x03,0x03},
    {0x18,0x18,0x00,0x18,0x18,0x1F,0x00,0x1F},
    {0x03,0x03,0x00,0x03,0x03,0x1F,0x00,0x1F},
    {0x1F,0x00,0x1F,0x18,0x18,0x00,0x18,0x18},
    {0x1F,0x00,0x1F,0x03,0x03,0x00,0x03,0x03}
  }
};

static constexpr uint8_t LCD_SPACE = 32;
enum ClockSegment : uint8_t {
  SEG_A = 1 << 0, SEG_B = 1 << 1, SEG_C = 1 << 2, SEG_D = 1 << 3,
  SEG_E = 1 << 4, SEG_F = 1 << 5, SEG_G = 1 << 6
};
static const uint8_t CLOCK_SEGMENTS[10] = {
  SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F,
  SEG_B|SEG_C,
  SEG_A|SEG_B|SEG_D|SEG_E|SEG_G,
  SEG_A|SEG_B|SEG_C|SEG_D|SEG_G,
  SEG_B|SEG_C|SEG_F|SEG_G,
  SEG_A|SEG_C|SEG_D|SEG_F|SEG_G,
  SEG_A|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G,
  SEG_A|SEG_B|SEG_C,
  SEG_A|SEG_B|SEG_C|SEG_D|SEG_E|SEG_F|SEG_G,
  SEG_A|SEG_B|SEG_C|SEG_D|SEG_F|SEG_G
};

uint8_t clockDigitCell(uint8_t digit, uint8_t row, uint8_t col) {
  if (digit > 9 || row > 3 || col > 2) return LCD_SPACE;
  uint8_t seg = CLOCK_SEGMENTS[digit];
  bool a = seg & SEG_A, b = seg & SEG_B, c = seg & SEG_C, d = seg & SEG_D;
  bool e = seg & SEG_E, f = seg & SEG_F, g = seg & SEG_G;

  if (row == 0) return a ? 0 : LCD_SPACE;
  if (row == 3) return d ? 1 : LCD_SPACE;
  if (row == 1) {
    if (col == 0) return f && g ? 4 : f ? 2 : g ? 0 : LCD_SPACE;
    if (col == 1) return g ? 0 : LCD_SPACE;
    return b && g ? 5 : b ? 3 : g ? 0 : LCD_SPACE;
  }
  if (col == 0) return e && g ? 6 : e ? 2 : g ? 1 : LCD_SPACE;
  if (col == 1) return g ? 1 : LCD_SPACE;
  return c && g ? 7 : c ? 3 : g ? 1 : LCD_SPACE;
}

void loadClockGlyphStyle(uint8_t style) {
  style = style <= 4 ? style : 0;
  if (loadedClockStyle == (int8_t)style) return;
  for (uint8_t glyph = 0; glyph < 8; ++glyph) {
    uint8_t bitmap[8];
    for (uint8_t y = 0; y < 8; ++y) {
      bitmap[y] = CLOCK_GLYPHS[style][glyph][y] & 0x1F;
      lcdCustomGlyphs[glyph][y] = bitmap[y];
    }
    lcd.createChar(glyph, bitmap);
  }
  loadedClockStyle = (int8_t)style;
  lcdForceRedraw = true;
}

void clearGraphicsFrame() {
  for (uint8_t row = 0; row < LCD_ROWS; ++row) {
    for (uint8_t col = 0; col < LCD_COLS; ++col) {
      writeTrackedCell(row, col, LCD_SPACE, true);
    }
  }
}

void drawBigClockDigit(uint8_t digit, uint8_t startCol) {
  if (digit > 9 || startCol + 2 >= LCD_COLS) return;
  for (uint8_t row = 0; row < 4; ++row) {
    for (uint8_t x = 0; x < 3; ++x) {
      writeTrackedCell(row, startCol + x, clockDigitCell(digit, row, x), true);
    }
  }
}

void drawBigClockColon(uint8_t col) {
  if (col >= LCD_COLS) return;
  for (uint8_t row = 0; row < 4; ++row) writeTrackedCell(row, col, LCD_SPACE, true);
  // A compact two-dot separator using the normal ROM colon character on the
  // two central LCD rows. Digits themselves are entirely CGRAM pixel glyphs.
  writeTrackedCell(1, col, ':', true);
  writeTrackedCell(2, col, ':', true);
}

void setBigClockScreen() {
  String hm = localHmString();
  if (hm == "--:--") {
    enterTextDisplayMode();
    setScreenLine(0, "");
    setScreenLine(1, " Ora non sincronizzata ");
    setScreenLine(2, "");
    setScreenLine(3, "");
    return;
  }

  uint8_t style = clockStyle <= 4 ? clockStyle : 0;
  bool styleChanged = loadedClockStyle != (int8_t)style;
  loadClockGlyphStyle(style);

  if (!lcdGraphicsMode || styleChanged || lastClockHm != hm || lcdForceRedraw) {
    lcdGraphicsMode = true;
    // Layout: margin, HH, gap, colon, gap, MM, margin = 20 cells.
    clearGraphicsFrame();
    drawBigClockDigit((uint8_t)(hm.charAt(0) - '0'), 1);
    drawBigClockDigit((uint8_t)(hm.charAt(1) - '0'), 5);
    drawBigClockColon(9);
    drawBigClockDigit((uint8_t)(hm.charAt(3) - '0'), 11);
    drawBigClockDigit((uint8_t)(hm.charAt(4) - '0'), 15);
    lastClockHm = hm;
    lcdForceRedraw = false;
  }

  // Keep legacy text fields meaningful for API clients, while the web replica
  // uses the exact framebuffer/custom glyph matrix.
  visibleLine[0] = "   " + hm + " PIXEL     ";
  visibleLine[1] = "                    ";
  visibleLine[2] = "                    ";
  visibleLine[3] = "                    ";
}

static const char *NCC_LETTERS[26][4] = {
  {" ## ","#  #","####","#  #"}, {"### ","#  #","### ","### "},
  {" ###","#   ","#   "," ###"}, {"### ","#  #","#  #","### "},
  {"####","#   ","### ","####"}, {"####","#   ","### ","#   "},
  {" ###","#   ","# ##"," ###"}, {"#  #","#  #","####","#  #"},
  {"####"," ## "," ## ","####"}, {"  ##","   #","#  #"," ## "},
  {"#  #","# # ","##  ","#  #"}, {"#   ","#   ","#   ","####"},
  {"#  #","####","####","#  #"}, {"#  #","## #","# ##","#  #"},
  {" ## ","#  #","#  #"," ## "}, {"### ","#  #","### ","#   "},
  {" ## ","#  #","# ##"," ###"}, {"### ","#  #","### ","#  #"},
  {" ###","#   ","   #","### "}, {"####"," ## "," ## "," ## "},
  {"#  #","#  #","#  #"," ## "}, {"#  #","#  #"," ## "," ## "},
  {"#  #","#  #","####"," ## "}, {"#  #"," ## "," ## ","#  #"},
  {"#  #"," ## "," ## "," ## "}, {"####","  # "," #  ","####"}
};

String bigGameLetterRow(char letter, uint8_t row) {
  if (letter < 'A' || letter > 'Z') return "    ";
  return String(NCC_LETTERS[letter - 'A'][row]);
}

String formatMmSs(uint32_t ms) {
  uint32_t total = (ms + 999UL) / 1000UL;
  uint32_t minutes = total / 60UL;
  uint32_t seconds = total % 60UL;
  char buf[8];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)minutes, (unsigned long)seconds);
  return String(buf);
}

String formatDurationMs(uint32_t ms, bool withCentiseconds = false) {
  uint32_t totalSec = ms / 1000UL;
  uint32_t h = totalSec / 3600UL;
  uint32_t m = (totalSec % 3600UL) / 60UL;
  uint32_t sec = totalSec % 60UL;
  char buf[24];
  if (withCentiseconds && h == 0) {
    uint32_t cs = (ms % 1000UL) / 10UL;
    snprintf(buf, sizeof(buf), "%02lu:%02lu.%02lu", (unsigned long)m, (unsigned long)sec, (unsigned long)cs);
  } else {
    snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m, (unsigned long)sec);
  }
  return String(buf);
}

uint32_t stopwatchElapsedMs() {
  return stopwatchAccumulatedMs + (stopwatchRunning ? (millis() - stopwatchStartedAt) : 0);
}

uint32_t countdownCurrentRemainingMs() {
  if (!countdownRunning) return countdownRemainingMs;
  int32_t delta = (int32_t)(countdownEndsAt - millis());
  return delta > 0 ? (uint32_t)delta : 0;
}

void syncCountdownSetFromInitial() {
  uint32_t total = countdownInitialMs / 1000UL;
  countdownSetHours = (total / 3600UL > 99UL) ? 99 : (uint8_t)(total / 3600UL);
  countdownSetMinutes = (total % 3600UL) / 60UL;
  countdownSetSeconds = total % 60UL;
}

void resetTimerAlarmPattern() {
  timerAlarmPhase = 0;
  timerAlarmPairsCompleted = 0;
  timerAlarmPhaseAt = 0;
  timerAlarmSoundFinished = false;
  timerPreBeepUntil = 0;
  if (!songPlaying) buzzerTone(0);
}

void stopCountdownAlarm() {
  countdownAlarm = false;
  resetTimerAlarmPattern();
  restoreStatusColor();
}

void stopNccAlarm() {
  nccAlarm = false;
  resetTimerAlarmPattern();
  restoreStatusColor();
}

void startCountdownFromSetup() {
  uint32_t totalSec = (uint32_t)countdownSetHours * 3600UL +
                      (uint32_t)countdownSetMinutes * 60UL +
                      (uint32_t)countdownSetSeconds;
  if (totalSec == 0) totalSec = 1;
  countdownInitialMs = totalSec * 1000UL;
  countdownRemainingMs = countdownInitialMs;
  countdownEndsAt = millis() + countdownRemainingMs;
  countdownRunning = true;
  countdownLastPreBeepSecond = -1;
  stopCountdownAlarm();
  uiMode = UiMode::COUNTDOWN_VIEW;
  addLog("Countdown avviato: " + formatDurationMs(countdownInitialMs));
  restoreStatusColor();
}

void toggleCountdownPause() {
  if (countdownAlarm) return;
  if (countdownRunning) {
    countdownRemainingMs = countdownCurrentRemainingMs();
    countdownRunning = false;
    addLog("Countdown in pausa");
  } else if (countdownRemainingMs > 0) {
    countdownEndsAt = millis() + countdownRemainingMs;
    countdownRunning = true;
    addLog("Countdown ripreso");
  }
  restoreStatusColor();
}

void resetCountdown() {
  stopCountdownAlarm();
  countdownRunning = false;
  countdownLastPreBeepSecond = -1;
  countdownRemainingMs = countdownInitialMs;
  syncCountdownSetFromInitial();
  addLog("Countdown reimpostato");
  restoreStatusColor();
}

uint32_t nccCurrentRemainingMs() {
  if (!nccRunning) return nccRemainingMs;
  int32_t delta = (int32_t)(nccEndsAt - millis());
  return delta > 0 ? (uint32_t)delta : 0;
}

void syncNccSetFromDefault() {
  uint32_t totalSec = nccDefaultMs / 1000UL;
  nccSetMinutes = (uint8_t)min((uint32_t)99, (uint32_t)(totalSec / 60UL));
  nccSetSeconds = totalSec % 60UL;
}

void saveNccDefault() {
  uint32_t totalSec = (uint32_t)nccSetMinutes * 60UL + (uint32_t)nccSetSeconds;
  if (totalSec == 0) totalSec = 1;
  nccDefaultMs = totalSec * 1000UL;
  nccRemainingMs = nccDefaultMs;
  preferences.begin("game", false);
  preferences.putUShort("seconds", (uint16_t)(nccDefaultMs / 1000UL));
  preferences.end();
  addLog("Nomi Cose Citta: durata " + formatMmSs(nccDefaultMs));
}

void enterNccSession() {
  stopNccAlarm();
  nccRunning = false;
  nccSessionActive = true;
  nccLetter = '-';
  nccUsedLettersMask = 0;
  nccLastPreBeepSecond = -1;
  nccRemainingMs = nccDefaultMs;
  syncNccSetFromDefault();
  uiMode = UiMode::NCC_SET_M;
  addLog("Nomi Cose Citta: nuova sessione");
  restoreStatusColor();
}

void exitNccSession() {
  stopNccAlarm();
  nccRunning = false;
  nccSessionActive = false;
  nccLetter = '-';
  nccUsedLettersMask = 0;
  nccLastPreBeepSecond = -1;
  nccRemainingMs = nccDefaultMs;
  uiMode = UiMode::TIMER_MENU;
  addLog("Nomi Cose Citta: sessione chiusa");
  restoreStatusColor();
}

bool chooseNewNccLetter() {
  static constexpr uint32_t ALL_LETTERS = (1UL << 26) - 1UL;
  uint32_t available = ALL_LETTERS & ~nccUsedLettersMask;
  if (available == 0) return false;
  uint8_t count = 0;
  for (uint8_t i = 0; i < 26; ++i) if (available & (1UL << i)) ++count;
  uint8_t pick = (uint8_t)(esp_random() % count);
  for (uint8_t i = 0; i < 26; ++i) {
    if (!(available & (1UL << i))) continue;
    if (pick == 0) {
      nccLetter = 'A' + i;
      nccUsedLettersMask |= (1UL << i);
      return true;
    }
    --pick;
  }
  return false;
}

void startNewNccRound() {
  stopNccAlarm();
  if (!chooseNewNccLetter()) {
    nccRunning = false;
    nccRemainingMs = 0;
    uiMode = UiMode::NCC_READY;
    addLog("Nomi Cose Citta: tutte le 26 lettere utilizzate");
    restoreStatusColor();
    return;
  }
  nccRemainingMs = nccDefaultMs;
  nccEndsAt = millis() + nccRemainingMs;
  nccRunning = true;
  nccLastPreBeepSecond = -1;
  nccUrgentLedAt = 0;
  nccUrgentLedOn = false;
  uiMode = UiMode::NCC_VIEW;
  addLog("Nomi Cose Citta: manche lettera " + String(nccLetter));
  restoreStatusColor();
}

void toggleNccPause() {
  if (nccAlarm || nccLetter == '-') return;
  if (nccRunning) {
    nccRemainingMs = nccCurrentRemainingMs();
    nccRunning = false;
    addLog("Nomi Cose Citta: pausa");
  } else if (nccRemainingMs > 0) {
    nccEndsAt = millis() + nccRemainingMs;
    nccRunning = true;
    addLog("Nomi Cose Citta: ripresa");
  }
  nccUrgentLedAt = 0;
  restoreStatusColor();
}

void restartNccSameLetter() {
  if (nccLetter == '-') return;
  stopNccAlarm();
  nccRemainingMs = nccDefaultMs;
  nccEndsAt = millis() + nccRemainingMs;
  nccRunning = true;
  nccLastPreBeepSecond = -1;
  nccUrgentLedAt = 0;
  addLog("Nomi Cose Citta: ripetuta lettera " + String(nccLetter));
  restoreStatusColor();
}

void servicePreExpiryTone(uint32_t now) {
  if (timerPreBeepUntil != 0 && (int32_t)(now - timerPreBeepUntil) >= 0) {
    timerPreBeepUntil = 0;
    buzzerTone(0);
    if (songPlaying) {
      toneStopsAt = 0;
      noteEndsAt = 0;
    }
  }
}

void maybePreExpiryBeep(uint32_t now, uint32_t remainingMs, bool game) {
  if (remainingMs == 0 || remainingMs > 3000UL) return;
  int8_t second = (int8_t)((remainingMs + 999UL) / 1000UL);
  int8_t &lastSecond = game ? nccLastPreBeepSecond : countdownLastPreBeepSecond;
  if (second < 1 || second > 3 || second == lastSecond) return;

  lastSecond = second;
  // Deliberately different from the final alarm: short high tick.
  buzzerTone(game ? 2850 : 2550);
  timerPreBeepUntil = now + 95UL;
}

void serviceTimerAlarmBeep(uint32_t now) {
  bool alarm = countdownAlarm || nccAlarm;
  if (!alarm) {
    if (timerAlarmPhaseAt != 0 || timerAlarmSoundFinished) resetTimerAlarmPattern();
    return;
  }
  if (timerAlarmSoundFinished) return;

  // One pair = beep 160 ms, gap 120 ms, beep 160 ms, pair gap 420 ms.
  // Exactly three pairs are emitted; the alarm screen then stays silent.
  static const uint16_t phaseMs[4] = {160, 120, 160, 420};

  if (timerAlarmPhaseAt == 0) {
    timerAlarmPhase = 0;
    timerAlarmPairsCompleted = 0;
    timerAlarmPhaseAt = now + phaseMs[0];
  } else if ((int32_t)(now - timerAlarmPhaseAt) >= 0) {
    if (timerAlarmPhase == 2) {
      ++timerAlarmPairsCompleted;
      buzzerTone(0);
      if (timerAlarmPairsCompleted >= 3) {
        timerAlarmSoundFinished = true;
        timerAlarmPhaseAt = 0;
        restoreStatusColor();
        return;
      }
    }
    timerAlarmPhase = (timerAlarmPhase + 1) % 4;
    timerAlarmPhaseAt = now + phaseMs[timerAlarmPhase];
  }

  bool beepOn = timerAlarmPhase == 0 || timerAlarmPhase == 2;
  if (beepOn) {
    buzzerTone(nccAlarm ? 2050 : 1750);
    colora(1, 0, 0);
  } else {
    buzzerTone(0);
    if (timerAlarmPhase == 3) colora(0, 0, 0);
  }
}

void serviceTimers() {
  uint32_t now = millis();
  servicePreExpiryTone(now);

  if (countdownRunning) {
    uint32_t rem = countdownCurrentRemainingMs();
    if (rem == 0) {
      countdownRunning = false;
      countdownRemainingMs = 0;
      countdownAlarm = true;
      countdownLastPreBeepSecond = -1;
      timerPreBeepUntil = 0;
      resetTimerAlarmPattern();
      if (songPlaying) stopSong();
      addLog("Countdown terminato: 3 x beep-beep");
      restoreStatusColor();
    } else {
      maybePreExpiryBeep(now, rem, false);
    }
  }

  if (nccRunning) {
    uint32_t rem = nccCurrentRemainingMs();
    if (rem == 0) {
      nccRunning = false;
      nccRemainingMs = 0;
      nccAlarm = true;
      nccLastPreBeepSecond = -1;
      timerPreBeepUntil = 0;
      resetTimerAlarmPattern();
      nccUrgentLedAt = 0;
      if (songPlaying) stopSong();
      addLog("Nomi Cose Citta: tempo scaduto, lettera " + String(nccLetter) + " - 3 x beep-beep");
      restoreStatusColor();
    } else {
      maybePreExpiryBeep(now, rem, true);
      if (rem <= 10000UL) {
        if (nccUrgentLedAt == 0 || (int32_t)(now - nccUrgentLedAt) >= 0) {
          nccUrgentLedAt = now + 250;
          nccUrgentLedOn = !nccUrgentLedOn;
          if (nccUrgentLedOn) colora(1, 1, 0); else colora(0, 0, 0);
        }
      } else if (nccUrgentLedAt != 0) {
        nccUrgentLedAt = 0;
        nccUrgentLedOn = false;
        restoreStatusColor();
      }
    }
  }

  serviceTimerAlarmBeep(now);
}

// ============================ PREFERENCES ============================
void persistWifiCredentials();

void loadWifiCredentials() {
  bool changed = false;

  preferences.begin("wifi3", true);
  bool bootstrapProvisioned = preferences.getBool("bootdone", false);
  savedWifiCount = preferences.getUChar("count", 0);
  if (savedWifiCount > MAX_WIFI_NETWORKS) {
    savedWifiCount = MAX_WIFI_NETWORKS;
    changed = true;
  }

  for (uint8_t i = 0; i < savedWifiCount; ++i) {
    savedWifi[i].ssid = preferences.getString(("s" + String(i)).c_str(), "");
    savedWifi[i].pass = preferences.getString(("p" + String(i)).c_str(), "");
  }
  preferences.end();

  // One-time migration from the original single-network namespace.
  if (savedWifiCount == 0) {
    preferences.begin("credentials", true);
    String oldSsid = preferences.getString("ssid", "");
    String oldPass = preferences.getString("psw", "");
    preferences.end();
    if (oldSsid.length()) {
      savedWifi[0].ssid = oldSsid;
      savedWifi[0].pass = oldPass;
      savedWifiCount = 1;
      changed = true;
    }
  }

  // The original Wi-Fi is injected only once on first migration/fresh install.
  // If the user later presses "Dimentica", it must stay forgotten.
  if (!bootstrapProvisioned) {
    bool bootstrapPresent = false;
    for (uint8_t i = 0; i < savedWifiCount; ++i) {
      if (savedWifi[i].ssid == BOOTSTRAP_WIFI_SSID) {
        bootstrapPresent = true;
        break;
      }
    }
    if (!bootstrapPresent && strlen(BOOTSTRAP_WIFI_SSID) > 0 && savedWifiCount < MAX_WIFI_NETWORKS) {
      savedWifi[savedWifiCount].ssid = BOOTSTRAP_WIFI_SSID;
      savedWifi[savedWifiCount].pass = BOOTSTRAP_WIFI_PASS;
      ++savedWifiCount;
    }
    changed = true; // also persists the one-time marker
  }

  // Do not rewrite NVS at every reboot; write only when migration/bootstrap
  // actually changed the credential store.
  if (changed) persistWifiCredentials();
}

void persistWifiCredentials() {
  preferences.begin("wifi3", false);
  preferences.clear();
  preferences.putBool("bootdone", true);
  preferences.putUChar("count", savedWifiCount);
  for (uint8_t i = 0; i < savedWifiCount; ++i) {
    preferences.putString(("s" + String(i)).c_str(), savedWifi[i].ssid);
    preferences.putString(("p" + String(i)).c_str(), savedWifi[i].pass);
  }
  preferences.end();
}

void saveWifiCredential(const String &ssid, const String &pass) {
  if (!ssid.length()) return;

  // Already the most-recent network with the same credential: avoid an NVS
  // write on every ordinary reconnect.
  if (savedWifiCount > 0 && savedWifi[0].ssid == ssid && savedWifi[0].pass == pass) return;

  WifiCredential next[MAX_WIFI_NETWORKS];
  uint8_t out = 0;
  next[out].ssid = ssid;
  next[out].pass = pass;
  ++out;

  for (uint8_t i = 0; i < savedWifiCount && out < MAX_WIFI_NETWORKS; ++i) {
    if (savedWifi[i].ssid != ssid) {
      next[out] = savedWifi[i];
      ++out;
    }
  }

  savedWifiCount = out;
  for (uint8_t i = 0; i < savedWifiCount; ++i) savedWifi[i] = next[i];
  persistWifiCredentials();
}

void loadMqttSettings() {
  preferences.begin("mqtt", true);
  mqttEnabled = preferences.getBool("enabled", false);
  mqttHost = preferences.getString("host", "");
  mqttPort = preferences.getUShort("port", 1883);
  mqttUser = preferences.getString("user", "");
  mqttPass = preferences.getString("pass", "");
  mqttTopic = preferences.getString("topic", "stazionemeteo/sensor");
  preferences.end();
  if (mqttPort == 0) mqttPort = 1883;
  if (!mqttTopic.length()) mqttTopic = "stazionemeteo/sensor";
}

void saveMqttSettings() {
  preferences.begin("mqtt", false);
  preferences.putBool("enabled", mqttEnabled);
  preferences.putString("host", mqttHost);
  preferences.putUShort("port", mqttPort);
  preferences.putString("user", mqttUser);
  preferences.putString("pass", mqttPass);
  preferences.putString("topic", mqttTopic);
  preferences.end();
  mqtt.disconnect();
}

void loadDeviceSettings() {
  preferences.begin("device", true);
  deviceName = sanitizeDeviceName(preferences.getString("name", DEFAULT_DEVICE_NAME));
  clockStyle = preferences.getUChar("clock", 0);
  if (clockStyle > 4) clockStyle = 0;
  preferences.end();

  preferences.begin("game", true);
  nccDefaultMs = (uint32_t)preferences.getUShort("seconds", 120) * 1000UL;
  preferences.end();
  if (nccDefaultMs < 1000UL || nccDefaultMs > 5999000UL) nccDefaultMs = 2UL * 60UL * 1000UL;
  nccRemainingMs = nccDefaultMs;
  syncNccSetFromDefault();

  preferences.begin("alarms", true);
  alarmsEnabled = preferences.getBool("enabled", false);
  alarmTempMin = preferences.getFloat("tmin", 5.0f);
  alarmTempMax = preferences.getFloat("tmax", 35.0f);
  alarmHumidityMin = preferences.getFloat("hmin", 20.0f);
  alarmHumidityMax = preferences.getFloat("hmax", 80.0f);
  alarmTopic = preferences.getString("topic", "stazionemeteo/alarm");
  preferences.end();
  if (!alarmTopic.length()) alarmTopic = deviceName + "/alarm";
}

void saveDeviceName(const String &name) {
  deviceName = sanitizeDeviceName(name);
  preferences.begin("device", false);
  preferences.putString("name", deviceName);
  preferences.end();
}

void saveClockStyle(uint8_t style) {
  clockStyle = style <= 4 ? style : 0;
  loadedClockStyle = -1;
  lcdForceRedraw = true;
  preferences.begin("device", false);
  preferences.putUChar("clock", clockStyle);
  preferences.end();
  addLog("Stile orologio: " + String(clockStyleName(clockStyle)));
}

void saveAlarmSettings() {
  preferences.begin("alarms", false);
  preferences.putBool("enabled", alarmsEnabled);
  preferences.putFloat("tmin", alarmTempMin);
  preferences.putFloat("tmax", alarmTempMax);
  preferences.putFloat("hmin", alarmHumidityMin);
  preferences.putFloat("hmax", alarmHumidityMax);
  preferences.putString("topic", alarmTopic);
  preferences.end();
  evaluateAlarms();
}

void loadMusicSettings() {
  preferences.begin("music", true);
  for (uint8_t i = 0; i < SONG_COUNT; ++i) {
    uint16_t bpm = preferences.getUShort(("b" + String(i)).c_str(), 120);
    songBpm[i] = constrain((int)bpm, 40, 300);
  }
  preferences.end();
}

void saveAllMusicSettings() {
  preferences.begin("music", false);
  for (uint8_t i = 0; i < SONG_COUNT; ++i) {
    preferences.putUShort(("b" + String(i)).c_str(), songBpm[i]);
  }
  preferences.end();
  bpmDirty = false;
}

void markCurrentBpmDirty() {
  if (currentSong < 0 || currentSong >= SONG_COUNT) return;
  songBpm[currentSong] = constrain(tempo, 40, 300);
  bpmDirty = true;
  bpmDirtySince = millis();
}

void saveCurrentBpmNow() {
  if (!bpmDirty || currentSong < 0 || currentSong >= SONG_COUNT) return;
  preferences.begin("music", false);
  preferences.putUShort(("b" + String(currentSong)).c_str(), songBpm[currentSong]);
  preferences.end();
  bpmDirty = false;
  addLog("BPM salvati: brano " + String(currentSong + 1) + " = " + String(songBpm[currentSong]));
}

void serviceBpmPersistence() {
  if (bpmDirty && millis() - bpmDirtySince >= BPM_SAVE_DEFER_MS) saveCurrentBpmNow();
}

// ============================ WIFI ============================
void startAccessPoint() {
  if (apActive) {
    if (WiFi.status() != WL_CONNECTED && uiMode == UiMode::HOME) uiMode = UiMode::AP_MODE;
    restoreStatusColor();
    return;
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(deviceName.c_str()); // intentionally open: the local panel itself has no admin login
  WiFi.softAPsetHostname(deviceName.c_str());
  delay(50);
  dnsServer.start(53, "*", WiFi.softAPIP());
  apActive = true;
  addLog("AP attivo: " + deviceName + " @ " + WiFi.softAPIP().toString());

  if (WiFi.status() != WL_CONNECTED && uiMode == UiMode::HOME) uiMode = UiMode::AP_MODE;
  restoreStatusColor();
}

void stopAccessPoint() {
  if (!apActive) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(false);
  apActive = false;
  addLog("AP disattivato");
  WiFi.mode(WIFI_STA);
}

void startWifiConnection(const String &ssid, const String &pass, bool saveOnSuccess = false, bool fromBootSequence = false) {
  if (!ssid.length()) return;

  // Never overwrite one of the three known-good networks before authentication
  // has actually succeeded. A mistyped password must not poison the saved list.
  pendingWifiSsid = ssid;
  pendingWifiPass = pass;
  addLog("Connessione Wi-Fi: " + ssid);
  savePendingWifiOnSuccess = saveOnSuccess;
  bootConnectionSequence = fromBootSequence;

  WiFi.mode(apActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.disconnect(false, false);
  delay(20);
  WiFi.begin(ssid.c_str(), pass.c_str());
  WiFi.setAutoReconnect(true);

  wifiConnecting = true;
  wifiConnectStarted = millis();
  restoreStatusColor();
}

void prepareSavedWifiCandidates() {
  wifiCandidateCount = savedWifiCount;
  wifiCandidatePos = 0;
  for (uint8_t i = 0; i < MAX_WIFI_NETWORKS; ++i) wifiCandidateOrder[i] = -1;
  for (uint8_t i = 0; i < savedWifiCount; ++i) wifiCandidateOrder[i] = i;
}

void tryNextBootCandidate() {
  if (wifiCandidatePos >= wifiCandidateCount) {
    wifiConnecting = false;
    bootConnectionSequence = false;
    startAccessPoint();
    return;
  }

  int idx = wifiCandidateOrder[wifiCandidatePos++];
  if (idx >= 0 && idx < savedWifiCount) {
    addLog("Tentativo Wi-Fi: " + savedWifi[idx].ssid);
    startWifiConnection(savedWifi[idx].ssid, savedWifi[idx].pass, false, true);
  }
}

void startSavedWifiSequence() {
  if (!savedWifiCount) {
    startAccessPoint();
    return;
  }
  prepareSavedWifiCandidates();
  tryNextBootCandidate();
}

void startWifiScan() {
  if (scanRunning || wifiConnecting) return;
  scanRunning = true;
  scanCount = 0;

  WiFi.mode(apActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.scanDelete();
  int16_t result = WiFi.scanNetworks(true, true);
  if (result == WIFI_SCAN_FAILED) {
    scanRunning = false;
    addLog("Scansione Wi-Fi non avviata");
  } else {
    addLog("Scansione Wi-Fi richiesta");
  }
  restoreStatusColor();
}

void cancelWifiScan() {
  if (!scanRunning) return;
  esp_wifi_scan_stop();
  WiFi.scanDelete();
  scanRunning = false;
  scanCount = 0;
  addLog("Scansione Wi-Fi annullata");
  restoreStatusColor();
}

void harvestWifiScan() {
  if (!scanRunning) return;
  int found = WiFi.scanComplete();
  if (found == WIFI_SCAN_RUNNING) return;

  scanRunning = false;
  scanCount = 0;

  if (found > 0) {
    for (int i = 0; i < found && scanCount < MAX_SCAN_RESULTS; ++i) {
      String ssidFound = WiFi.SSID(i);
      if (!ssidFound.length()) continue;

      bool duplicate = false;
      for (uint8_t j = 0; j < scanCount; ++j) {
        if (scanItems[j].ssid == ssidFound) {
          duplicate = true;
          if (WiFi.RSSI(i) > scanItems[j].rssi) {
            scanItems[j].rssi = WiFi.RSSI(i);
            scanItems[j].auth = WiFi.encryptionType(i);
          }
          break;
        }
      }
      if (!duplicate) {
        scanItems[scanCount].ssid = ssidFound;
        scanItems[scanCount].rssi = WiFi.RSSI(i);
        scanItems[scanCount].auth = WiFi.encryptionType(i);
        ++scanCount;
      }
    }

    for (uint8_t i = 0; i < scanCount; ++i) {
      for (uint8_t j = i + 1; j < scanCount; ++j) {
        if (scanItems[j].rssi > scanItems[i].rssi) {
          ScanItem tmp = scanItems[i];
          scanItems[i] = scanItems[j];
          scanItems[j] = tmp;
        }
      }
    }
  }
  WiFi.scanDelete();
  addLog("Scansione Wi-Fi completata: " + String(scanCount) + " reti");
  restoreStatusColor();
}

void onWifiConnected() {
  wifiConnecting = false;
  bootConnectionSequence = false;

  // Save a newly entered network only after a verified connection. Otherwise
  // simply move an already-known successful network to MRU position.
  const String connected = WiFi.SSID();
  bool stored = false;
  if (savePendingWifiOnSuccess && connected == pendingWifiSsid) {
    saveWifiCredential(pendingWifiSsid, pendingWifiPass);
    stored = true;
  }
  if (!stored) {
    for (uint8_t i = 0; i < savedWifiCount; ++i) {
      if (savedWifi[i].ssid == connected) {
        saveWifiCredential(savedWifi[i].ssid, savedWifi[i].pass);
        break;
      }
    }
  }
  savePendingWifiOnSuccess = false;
  pendingWifiSsid = "";
  pendingWifiPass = "";

  configTzTime(TZ_ITALY, NTP_SERVER);

  if (!mdnsStarted) {
    mdnsStarted = MDNS.begin(deviceName.c_str());
    if (mdnsStarted) MDNS.addService("http", "tcp", 80);
  }

  addLog("Wi-Fi connesso: " + connected + " IP " + WiFi.localIP().toString());
  if (apActive) stopAccessPoint();
  if (uiMode == UiMode::AP_MODE) {
    uiMode = UiMode::HOME;
    homePage = HomePage::WEATHER;
    homePageSince = millis();
  }
  restoreStatusColor();
}

void processWifi() {
  harvestWifiScan();

  if (wifiConnecting) {
    if (WiFi.status() == WL_CONNECTED) {
      onWifiConnected();
      return;
    }

    if (millis() - wifiConnectStarted >= WIFI_CONNECT_TIMEOUT_MS) {
      wifiConnecting = false;
      WiFi.disconnect(false, false);
      addLog("Connessione Wi-Fi fallita: " + pendingWifiSsid);
      savePendingWifiOnSuccess = false;
      pendingWifiSsid = "";
      pendingWifiPass = "";

      // Only the boot/recovery sequence is allowed to advance through the
      // saved candidates. A failed manual web/LCD connection returns to AP.
      if (bootConnectionSequence && wifiCandidatePos < wifiCandidateCount) {
        tryNextBootCandidate();
      } else {
        bootConnectionSequence = false;
        startAccessPoint();
      }
    }
    return;
  }

  if (WiFi.status() != WL_CONNECTED && !apActive && !scanRunning) {
    if (wifiLastLostAt == 0) wifiLastLostAt = millis();
    if (millis() - wifiLastLostAt >= WIFI_RETRY_AFTER_DROP_MS) {
      wifiLastLostAt = millis();
      if (savedWifiCount) startSavedWifiSequence();
      else startAccessPoint();
    }
  } else if (WiFi.status() == WL_CONNECTED) {
    wifiLastLostAt = 0;
  }
}

// ============================ DHT ============================
void serviceSensor() {
  if (millis() - lastDhtRead < DHT_INTERVAL_MS) return;
  lastDhtRead = millis();

  float h = dht_sensor.readHumidity();
  float t = dht_sensor.readTemperature();

  // Keep the last good measurement rather than replacing it with NaN.
  bool valid = false;
  if (!isnan(h)) { humidity = h; valid = true; }
  if (!isnan(t)) { temperatureC = t; valid = true; }
  if (valid) {
    recordHistoryIfDue(historyCount == 0);
    evaluateAlarms();
  }
}

#define NOTE_B0  31
#define NOTE_C1  33
#define NOTE_CS1 35
#define NOTE_D1  37
#define NOTE_DS1 39
#define NOTE_E1  41
#define NOTE_F1  44
#define NOTE_FS1 46
#define NOTE_G1  49
#define NOTE_GS1 52
#define NOTE_A1  55
#define NOTE_AS1 58
#define NOTE_B1  62
#define NOTE_C2  65
#define NOTE_CS2 69
#define NOTE_D2  73
#define NOTE_DS2 78
#define NOTE_E2  82
#define NOTE_F2  87
#define NOTE_FS2 93
#define NOTE_G2  98
#define NOTE_GS2 104
#define NOTE_A2  110
#define NOTE_AS2 117
#define NOTE_B2  123
#define NOTE_C3  131
#define NOTE_CS3 139
#define NOTE_D3  147
#define NOTE_DS3 156
#define NOTE_E3  165
#define NOTE_F3  175
#define NOTE_FS3 185
#define NOTE_G3  196
#define NOTE_GS3 208
#define NOTE_A3  220
#define NOTE_AS3 233
#define NOTE_B3  247
#define NOTE_C4  262
#define NOTE_CS4 277
#define NOTE_D4  294
#define NOTE_DS4 311
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_FS4 370
#define NOTE_G4  392
#define NOTE_GS4 415
#define NOTE_A4  440
#define NOTE_AS4 466
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_CS5 554
#define NOTE_D5  587
#define NOTE_DS5 622
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_FS5 740
#define NOTE_G5  784
#define NOTE_GS5 831
#define NOTE_A5  880
#define NOTE_AS5 932
#define NOTE_B5  988
#define NOTE_C6  1047
#define NOTE_CS6 1109
#define NOTE_D6  1175
#define NOTE_DS6 1245
#define NOTE_E6  1319
#define NOTE_F6  1397
#define NOTE_FS6 1480
#define NOTE_G6  1568
#define NOTE_GS6 1661
#define NOTE_A6  1760
#define NOTE_AS6 1865
#define NOTE_B6  1976
#define NOTE_C7  2093
#define NOTE_CS7 2217
#define NOTE_D7  2349
#define NOTE_DS7 2489
#define NOTE_E7  2637
#define NOTE_F7  2794
#define NOTE_FS7 2960
#define NOTE_G7  3136
#define NOTE_GS7 3322
#define NOTE_A7  3520
#define NOTE_AS7 3729
#define NOTE_B7  3951
#define NOTE_C8  4186
#define NOTE_CS8 4435
#define NOTE_D8  4699
#define NOTE_DS8 4978
#define REST      0

#define NUMELEMENTS(x) (sizeof(x) / sizeof(x[0]))

int melody0[] = {
  NOTE_B4,-4, NOTE_E5,-4, NOTE_B4,-4, NOTE_E5,-4, 
  NOTE_B4,8,  NOTE_E5,-4, NOTE_B4,8, REST,8,  NOTE_AS4,8, NOTE_B4,8, 
  NOTE_B4,8,  NOTE_AS4,8, NOTE_B4,8, NOTE_A4,8, REST,8, NOTE_GS4,8, NOTE_A4,8, NOTE_G4,8,
  NOTE_G4,4,  NOTE_E4,-2, 
  NOTE_B4,-4, NOTE_E5,-4, NOTE_B4,-4, NOTE_E5,-4, 
  NOTE_B4,8,  NOTE_E5,-4, NOTE_B4,8, REST,8,  NOTE_AS4,8, NOTE_B4,8,
  NOTE_A4,-4, NOTE_A4,-4, NOTE_GS4,8, NOTE_A4,-4,
  NOTE_D5,8,  NOTE_C5,-4, NOTE_B4,-4, NOTE_A4,-4,
  NOTE_B4,-4, NOTE_E5,-4, NOTE_B4,-4, NOTE_E5,-4, 
  NOTE_B4,8,  NOTE_E5,-4, NOTE_B4,8, REST,8,  NOTE_AS4,8, NOTE_B4,8,
  NOTE_D5,4, NOTE_D5,-4, NOTE_B4,8, NOTE_A4,-4,
  NOTE_G4,-4, NOTE_E4,-2,
  NOTE_E4, 2, NOTE_G4,2,
  NOTE_B4, 2, NOTE_D5,2,
  NOTE_F5, -4, NOTE_E5,-4, NOTE_AS4,8, NOTE_AS4,8, NOTE_B4,4, NOTE_G4,4, 
};
int melody1[] = {
  NOTE_A4,-4, NOTE_A4,-4, NOTE_A4,16, NOTE_A4,16, NOTE_A4,16, NOTE_A4,16, NOTE_F4,8, REST,8,
  NOTE_A4,-4, NOTE_A4,-4, NOTE_A4,16, NOTE_A4,16, NOTE_A4,16, NOTE_A4,16, NOTE_F4,8, REST,8,
  NOTE_A4,4, NOTE_A4,4, NOTE_A4,4, NOTE_F4,-8, NOTE_C5,16,
  NOTE_A4,4, NOTE_F4,-8, NOTE_C5,16, NOTE_A4,2,//4
  NOTE_E5,4, NOTE_E5,4, NOTE_E5,4, NOTE_F5,-8, NOTE_C5,16,
  NOTE_A4,4, NOTE_F4,-8, NOTE_C5,16, NOTE_A4,2,
  NOTE_A5,4, NOTE_A4,-8, NOTE_A4,16, NOTE_A5,4, NOTE_GS5,-8, NOTE_G5,16, //7 
  NOTE_DS5,16, NOTE_D5,16, NOTE_DS5,8, REST,8, NOTE_A4,8, NOTE_DS5,4, NOTE_D5,-8, NOTE_CS5,16,
  NOTE_C5,16, NOTE_B4,16, NOTE_C5,16, REST,8, NOTE_F4,8, NOTE_GS4,4, NOTE_F4,-8, NOTE_A4,-16,//9
  NOTE_C5,4, NOTE_A4,-8, NOTE_C5,16, NOTE_E5,2,
  NOTE_A5,4, NOTE_A4,-8, NOTE_A4,16, NOTE_A5,4, NOTE_GS5,-8, NOTE_G5,16, //7 
  NOTE_DS5,16, NOTE_D5,16, NOTE_DS5,8, REST,8, NOTE_A4,8, NOTE_DS5,4, NOTE_D5,-8, NOTE_CS5,16,
  NOTE_C5,16, NOTE_B4,16, NOTE_C5,16, REST,8, NOTE_F4,8, NOTE_GS4,4, NOTE_F4,-8, NOTE_A4,-16,//9
  NOTE_A4,4, NOTE_F4,-8, NOTE_C5,16, NOTE_A4,2,
};
int melody2[] = {  REST, 2, NOTE_D4, 4,
  NOTE_G4, -4, NOTE_AS4, 8, NOTE_A4, 4,
  NOTE_G4, 2, NOTE_D5, 4,
  NOTE_C5, -2, 
  NOTE_A4, -2,
  NOTE_G4, -4, NOTE_AS4, 8, NOTE_A4, 4,
  NOTE_F4, 2, NOTE_GS4, 4,
  NOTE_D4, -1, 
  NOTE_D4, 4,
  NOTE_G4, -4, NOTE_AS4, 8, NOTE_A4, 4, //10
  NOTE_G4, 2, NOTE_D5, 4,
  NOTE_F5, 2, NOTE_E5, 4,
  NOTE_DS5, 2, NOTE_B4, 4,
  NOTE_DS5, -4, NOTE_D5, 8, NOTE_CS5, 4,
  NOTE_CS4, 2, NOTE_B4, 4,
  NOTE_G4, -1,
  NOTE_AS4, 4,  
  NOTE_D5, 2, NOTE_AS4, 4,//18
  NOTE_D5, 2, NOTE_AS4, 4,
  NOTE_DS5, 2, NOTE_D5, 4,
  NOTE_CS5, 2, NOTE_A4, 4,
  NOTE_AS4, -4, NOTE_D5, 8, NOTE_CS5, 4,
  NOTE_CS4, 2, NOTE_D4, 4,
  NOTE_D5, -1, 
  REST,4, NOTE_AS4,4,  
  NOTE_D5, 2, NOTE_AS4, 4,//26
  NOTE_D5, 2, NOTE_AS4, 4,
  NOTE_F5, 2, NOTE_E5, 4,
  NOTE_DS5, 2, NOTE_B4, 4,
  NOTE_DS5, -4, NOTE_D5, 8, NOTE_CS5, 4,
  NOTE_CS4, 2, NOTE_AS4, 4,
  NOTE_G4, -1,  
};
int melody3[] = {  NOTE_AS4,8, NOTE_AS4,8, NOTE_AS4,8,//1
  NOTE_F5,2, NOTE_C6,2,
  NOTE_AS5,8, NOTE_A5,8, NOTE_G5,8, NOTE_F6,2, NOTE_C6,4,  
  NOTE_AS5,8, NOTE_A5,8, NOTE_G5,8, NOTE_F6,2, NOTE_C6,4,  
  NOTE_AS5,8, NOTE_A5,8, NOTE_AS5,8, NOTE_G5,2, NOTE_C5,8, NOTE_C5,8, NOTE_C5,8,
  NOTE_F5,2, NOTE_C6,2,
  NOTE_AS5,8, NOTE_A5,8, NOTE_G5,8, NOTE_F6,2, NOTE_C6,4,  
  
  NOTE_AS5,8, NOTE_A5,8, NOTE_G5,8, NOTE_F6,2, NOTE_C6,4, //8  
  NOTE_AS5,8, NOTE_A5,8, NOTE_AS5,8, NOTE_G5,2, NOTE_C5,-8, NOTE_C5,16, 
  NOTE_D5,-4, NOTE_D5,8, NOTE_AS5,8, NOTE_A5,8, NOTE_G5,8, NOTE_F5,8,
  NOTE_F5,8, NOTE_G5,8, NOTE_A5,8, NOTE_G5,4, NOTE_D5,8, NOTE_E5,4,NOTE_C5,-8, NOTE_C5,16,
  NOTE_D5,-4, NOTE_D5,8, NOTE_AS5,8, NOTE_A5,8, NOTE_G5,8, NOTE_F5,8,
  
  NOTE_C6,-8, NOTE_G5,16, NOTE_G5,2, REST,8, NOTE_C5,8,//13
  NOTE_D5,-4, NOTE_D5,8, NOTE_AS5,8, NOTE_A5,8, NOTE_G5,8, NOTE_F5,8,
  NOTE_F5,8, NOTE_G5,8, NOTE_A5,8, NOTE_G5,4, NOTE_D5,8, NOTE_E5,4,NOTE_C6,-8, NOTE_C6,16,
  NOTE_F6,4, NOTE_DS6,8, NOTE_CS6,4, NOTE_C6,8, NOTE_AS5,4, NOTE_GS5,8, NOTE_G5,4, NOTE_F5,8,
  NOTE_C6,1
  
};
int melody4[] = {  
  NOTE_C5,4, NOTE_G4,8, NOTE_AS4,4, NOTE_A4,8,
  NOTE_G4,16, NOTE_C4,8, NOTE_C4,16, NOTE_G4,16, NOTE_G4,8, NOTE_G4,16,
  NOTE_C5,4, NOTE_G4,8, NOTE_AS4,4, NOTE_A4,8,
  NOTE_G4,2,  
  NOTE_C5,4, NOTE_G4,8, NOTE_AS4,4, NOTE_A4,8,
  NOTE_G4,16, NOTE_C4,8, NOTE_C4,16, NOTE_G4,16, NOTE_G4,8, NOTE_G4,16,
  NOTE_F4,8, NOTE_E4,8, NOTE_D4,8, NOTE_C4,8,
  NOTE_C4,2,
  NOTE_C5,4, NOTE_G4,8, NOTE_AS4,4, NOTE_A4,8,
  NOTE_G4,16, NOTE_C4,8, NOTE_C4,16, NOTE_G4,16, NOTE_G4,8, NOTE_G4,16,
  NOTE_C5,4, NOTE_G4,8, NOTE_AS4,4, NOTE_A4,8,
  NOTE_G4,2,
  NOTE_C5,4, NOTE_G4,8, NOTE_AS4,4, NOTE_A4,8,
  NOTE_G4,16, NOTE_C4,8, NOTE_C4,16, NOTE_G4,16, NOTE_G4,8, NOTE_G4,16,
  NOTE_F4,8, NOTE_E4,8, NOTE_D4,8, NOTE_C4,8,
  NOTE_C4,16, NOTE_D5,8, NOTE_D5,16, NOTE_D5,16, NOTE_D5,8, NOTE_D5,16,
  NOTE_D5,16, NOTE_D5,8, NOTE_D5,16, NOTE_C5,8, NOTE_E5,-8,
  NOTE_C5,8, NOTE_C5,16, NOTE_E5,16, NOTE_E5,8, NOTE_C5,16,
  NOTE_F5,8, NOTE_D5,8, NOTE_D5,8, NOTE_E5,-8,
  NOTE_C5,8, NOTE_D5,16, NOTE_E5,16, NOTE_D5,8, NOTE_C5,16,
  NOTE_F5,8, NOTE_F5,8, NOTE_A5,8, NOTE_G5,-8,//21
  NOTE_G5,8, NOTE_C5,16, NOTE_C5,16, NOTE_C5,8, NOTE_C5,16,
  NOTE_F5,-8, NOTE_E5,16, NOTE_D5,8, NOTE_C5,4,
  NOTE_C5,16, NOTE_C5,16, NOTE_C5,16, NOTE_C5,16,
  NOTE_F5,8, NOTE_F5,16, NOTE_A5,8, NOTE_G5,-8,//25
  NOTE_G5,8, NOTE_C5,16, NOTE_C5,16, NOTE_C5,8, NOTE_C5,16,
  NOTE_F5,16, NOTE_E5,8, NOTE_D5,16, NOTE_C5,8, NOTE_E5,-8,
  NOTE_C5,8, NOTE_D5,16, NOTE_E5,16, NOTE_D5,8, NOTE_C5,16, 
  NOTE_F5,8, NOTE_F5,16, NOTE_A5,8, NOTE_G5,-8,//29
  NOTE_G5,8, NOTE_C5,16, NOTE_C5,16, NOTE_C5,8, NOTE_C5,16,
  NOTE_F5,8, NOTE_E5,16, NOTE_D5,8, NOTE_C5,8,
  NOTE_C5,4, NOTE_G4,8, NOTE_AS4,4, NOTE_A4,8,
  NOTE_G4,16, NOTE_C4,8, NOTE_C4,16, NOTE_G4,16, NOTE_G4,8, NOTE_G4,16,
  NOTE_C5,4, NOTE_G4,8, NOTE_AS4,4, NOTE_A4,8,
  NOTE_G4,2,
  NOTE_C5,4, NOTE_G4,8, NOTE_AS4,4, NOTE_A4,8,
  NOTE_G4,16, NOTE_C4,8, NOTE_C4,16, NOTE_G4,16, NOTE_G4,8, NOTE_G4,16,
  NOTE_F4,8, NOTE_E4,8, NOTE_D4,8, NOTE_C4,-2,
  NOTE_C5,4, NOTE_G4,8, NOTE_AS4,4, NOTE_A4,8,
  NOTE_G4,16, NOTE_C4,8, NOTE_C4,16, NOTE_G4,16, NOTE_G4,8, NOTE_G4,16,
  NOTE_C5,4, NOTE_G4,8, NOTE_AS4,4, NOTE_A4,8,
  NOTE_G4,2,
  NOTE_C5,4, NOTE_G4,8, NOTE_AS4,4, NOTE_A4,8,
  NOTE_G4,16, NOTE_C4,8, NOTE_C4,16, NOTE_G4,16, NOTE_G4,8, NOTE_G4,16,
  NOTE_F4,8, NOTE_E4,8, NOTE_D4,8, NOTE_C4,-2,
  NOTE_C4,16, NOTE_C4,8, NOTE_C4,16, NOTE_E4,16, NOTE_E4,8, NOTE_E4,16,
  NOTE_F4,16, NOTE_F4,8, NOTE_F4,16, NOTE_FS4,16, NOTE_FS4,8, NOTE_FS4,16,
  NOTE_G4,8, REST,8, NOTE_AS4,8, NOTE_C5,1,
};
int melody5[] = {
  NOTE_D4, -8, NOTE_G4, 16, NOTE_C5, -4, 
  NOTE_B4, 8, NOTE_G4, -16, NOTE_E4, -16, NOTE_A4, -16,
  NOTE_D5, 2,
};
int melody6[] = { 
  NOTE_G4,8, NOTE_C4,8, NOTE_DS4,16, NOTE_F4,16, NOTE_G4,8, NOTE_C4,8, NOTE_DS4,16, NOTE_F4,16, //1
  NOTE_G4,8, NOTE_C4,8, NOTE_DS4,16, NOTE_F4,16, NOTE_G4,8, NOTE_C4,8, NOTE_DS4,16, NOTE_F4,16,
  NOTE_G4,8, NOTE_C4,8, NOTE_E4,16, NOTE_F4,16, NOTE_G4,8, NOTE_C4,8, NOTE_E4,16, NOTE_F4,16,
  NOTE_G4,8, NOTE_C4,8, NOTE_E4,16, NOTE_F4,16, NOTE_G4,8, NOTE_C4,8, NOTE_E4,16, NOTE_F4,16,
  NOTE_G4,-4, NOTE_C4,-4,//5

  NOTE_DS4,16, NOTE_F4,16, NOTE_G4,4, NOTE_C4,4, NOTE_DS4,16, NOTE_F4,16, //6
  NOTE_D4,-1, //7 and 8
  NOTE_F4,-4, NOTE_AS3,-4,
  NOTE_DS4,16, NOTE_D4,16, NOTE_F4,4, NOTE_AS3,-4,
  NOTE_DS4,16, NOTE_D4,16, NOTE_C4,-1, //11 and 12
  NOTE_G4,-4, NOTE_C4,-4,//5
  NOTE_DS4,16, NOTE_F4,16, NOTE_G4,4, NOTE_C4,4, NOTE_DS4,16, NOTE_F4,16, //6
  NOTE_D4,-1, //7 and 8
  NOTE_F4,-4, NOTE_AS3,-4,
  NOTE_DS4,16, NOTE_D4,16, NOTE_F4,4, NOTE_AS3,-4,
  NOTE_DS4,16, NOTE_D4,16, NOTE_C4,-1, //11 and 12
  NOTE_G4,-4, NOTE_C4,-4,
  NOTE_DS4,16, NOTE_F4,16, NOTE_G4,4,  NOTE_C4,4, NOTE_DS4,16, NOTE_F4,16,
  NOTE_D4,-2,//15
  NOTE_F4,-4, NOTE_AS3,-4,
  NOTE_D4,-8, NOTE_DS4,-8, NOTE_D4,-8, NOTE_AS3,-8,
  NOTE_C4,-1,
  NOTE_C5,-2,
  NOTE_AS4,-2,
  NOTE_C4,-2,
  NOTE_G4,-2,
  NOTE_DS4,-2,
  NOTE_DS4,-4, NOTE_F4,-4, 
  NOTE_G4,-1,
  NOTE_C5,-2,//28
  NOTE_AS4,-2,
  NOTE_C4,-2,
  NOTE_G4,-2, 
  NOTE_DS4,-2,
  NOTE_DS4,-4, NOTE_D4,-4,
  NOTE_C5,8, NOTE_G4,8, NOTE_GS4,16, NOTE_AS4,16, NOTE_C5,8, NOTE_G4,8, NOTE_GS4,16, NOTE_AS4,16,
  NOTE_C5,8, NOTE_G4,8, NOTE_GS4,16, NOTE_AS4,16, NOTE_C5,8, NOTE_G4,8, NOTE_GS4,16, NOTE_AS4,16,
  REST,4, NOTE_GS5,16, NOTE_AS5,16, NOTE_C6,8, NOTE_G5,8, NOTE_GS5,16, NOTE_AS5,16,
  NOTE_C6,8, NOTE_G5,16, NOTE_GS5,16, NOTE_AS5,16, NOTE_C6,8, NOTE_G5,8, NOTE_GS5,16, NOTE_AS5,16,  
};
int melody7[] = {  REST, 4, REST, 8, REST, 8, REST, 8, NOTE_E4, 8, NOTE_A4, 8, NOTE_C5, 8, //1
  NOTE_B4, 8, NOTE_A4, 8, NOTE_C5, 8, NOTE_A4, 8, NOTE_B4, 8, NOTE_A4, 8, NOTE_F4, 8, NOTE_G4, 8,
  NOTE_E4, 2, NOTE_E4, 8, NOTE_A4, 8, NOTE_C5, 8,
  NOTE_B4, 8, NOTE_A4, 8, NOTE_C5, 8, NOTE_A4, 8, NOTE_C5, 8, NOTE_A4, 8, NOTE_E4, 8, NOTE_DS4, 8,
  
  NOTE_D4, 2, NOTE_D4, 8, NOTE_F4, 8, NOTE_GS4, 8, //5
  NOTE_B4, 2, NOTE_D4, 8, NOTE_F4, 8, NOTE_GS4, 8,
  NOTE_A4, 2, NOTE_C4, 8, NOTE_C4, 8, NOTE_G4, 8, 
  NOTE_F4, 8, NOTE_E4, 8, NOTE_G4, 8, NOTE_F4, 8, NOTE_F4, 8, NOTE_E4, 8, NOTE_E4, 8, NOTE_GS4, 8,

  NOTE_A4, 2, REST,8, NOTE_A4, 8, NOTE_A4, 8, NOTE_GS4, 8, //9
  NOTE_G4, 2, NOTE_B4, 8, NOTE_A4, 8, NOTE_F4, 8, 
  NOTE_E4, 2, NOTE_E4, 8, NOTE_G4, 8, NOTE_E4, 8,
  NOTE_D4, 2, NOTE_D4, 8, NOTE_D4, 8, NOTE_F4, 8, NOTE_DS4, 8, 
   
  NOTE_E4, 2, REST, 8, NOTE_E4, 8, NOTE_A4, 8, NOTE_C5, 8, //13
  NOTE_B4, 8, NOTE_A4, 8, NOTE_C5, 8, NOTE_A4, 8, NOTE_B4, 8, NOTE_A4, 8, NOTE_F4, 8, NOTE_G4, 8, //2
  NOTE_E4, 2, NOTE_E4, 8, NOTE_A4, 8, NOTE_C5, 8,
  NOTE_B4, 8, NOTE_A4, 8, NOTE_C5, 8, NOTE_A4, 8, NOTE_C5, 8, NOTE_A4, 8, NOTE_E4, 8, NOTE_DS4, 8,
  
  NOTE_D4, 2, NOTE_D4, 8, NOTE_F4, 8, NOTE_GS4, 8, //5
  NOTE_B4, 2, NOTE_D4, 8, NOTE_F4, 8, NOTE_GS4, 8,
  NOTE_A4, 2, NOTE_C4, 8, NOTE_C4, 8, NOTE_G4, 8, 
  NOTE_F4, 8, NOTE_E4, 8, NOTE_G4, 8, NOTE_F4, 8, NOTE_F4, 8, NOTE_E4, 8, NOTE_E4, 8, NOTE_GS4, 8,

  NOTE_A4, 2, REST,8, NOTE_A4, 8, NOTE_A4, 8, NOTE_GS4, 8, //9
  NOTE_G4, 2, NOTE_B4, 8, NOTE_A4, 8, NOTE_F4, 8, 
  NOTE_E4, 2, NOTE_E4, 8, NOTE_G4, 8, NOTE_E4, 8,
  NOTE_D4, 2, NOTE_D4, 8, NOTE_D4, 8, NOTE_F4, 8, NOTE_DS4, 8, 
   
  NOTE_E4, 2 //13
};
int melody8[] = { REST, 4, NOTE_G5, 4,
  NOTE_A5, 4, NOTE_AS5, 4,
  NOTE_A5, 4, NOTE_F5, 4,
  NOTE_A5, 4, NOTE_G5, 4,
  REST, 4, NOTE_G5, 4,
  NOTE_A5, 4, NOTE_AS5, 4,
  NOTE_C6, 4, NOTE_AS5, 4,

  NOTE_A5, 4, NOTE_G5, 4, //8
  REST, 4, NOTE_G5, 4,
  NOTE_A5, 4, NOTE_AS5, 4,
  NOTE_A5, 4, NOTE_F5, 4,
  NOTE_A5, 4, NOTE_G5, 4,
  NOTE_D6, 4, REST, 8, NOTE_C6, 8,
  REST, 4, NOTE_AS5, 4,

  NOTE_A5, 4, NOTE_AS5, 8, NOTE_C6, 8, //15
  NOTE_F6, 8, REST, 8, REST, 4,
  NOTE_G5, 16, NOTE_D5, 16, NOTE_D6, 16, NOTE_D5, 16, NOTE_C6, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16,
  NOTE_A5, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16, NOTE_A5, 16, NOTE_D5, 16, NOTE_G5, 16, NOTE_D5, 16,
  NOTE_A5, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16, NOTE_C6, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16,

  NOTE_A5, 16, NOTE_D5, 16, NOTE_F5, 16, NOTE_D5, 16, NOTE_A5, 16, NOTE_D5, 16, NOTE_G5, 16, NOTE_D5, 16, //20
  NOTE_G5, 16, NOTE_D5, 16, NOTE_D6, 16, NOTE_D5, 16, NOTE_C6, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16,
  NOTE_A5, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16, NOTE_A5, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16,
  NOTE_A5, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16, NOTE_C6, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16,
  NOTE_A5, 16, NOTE_D5, 16, NOTE_F5, 16, NOTE_D5, 16, NOTE_A5, 16, NOTE_D5, 16, NOTE_G5, 16, NOTE_D5, 16,

  NOTE_G5, 16, NOTE_D5, 16, NOTE_D6, 16, NOTE_D5, 16, NOTE_C6, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16, //25
  NOTE_A5, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16, NOTE_A5, 16, NOTE_D5, 16, NOTE_G5, 16, NOTE_D5, 16,
  NOTE_A5, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16, NOTE_C6, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16,
  NOTE_A5, 16, NOTE_D5, 16, NOTE_F5, 16, NOTE_D5, 16, NOTE_A5, 16, NOTE_D5, 16, NOTE_G5, 16, NOTE_D5, 16,
  NOTE_AS5, 16, NOTE_D5, 16, NOTE_D6, 16, NOTE_D5, 16, NOTE_C6, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16,

  NOTE_A5, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16, NOTE_A5, 16, NOTE_D5, 16, NOTE_G5, 16, NOTE_D5, 16,
  NOTE_A5, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16, NOTE_C6, 16, NOTE_D5, 16, NOTE_AS5, 16, NOTE_D5, 16,
  NOTE_A5, 16, NOTE_D5, 16, NOTE_F5, 16, NOTE_D5, 16, NOTE_A5, 16, NOTE_D5, 16, NOTE_G5, 16, NOTE_D5, 16,
  NOTE_C6, 16, NOTE_C6, 16, NOTE_F6, 16, NOTE_D6, 8, REST, 16, REST, 8,
  REST, 4, NOTE_C6, 16, NOTE_AS5, 16,

  NOTE_C6, -8,  NOTE_F6, -8, NOTE_D6, -4, //35
  NOTE_C6, 8, NOTE_AS5, 8,
  NOTE_C6, 8, NOTE_F6, 16, NOTE_D6, 8, REST, 16, REST, 8,
  REST, 4, NOTE_C6, 8, NOTE_D6, 8,
  NOTE_DS6, -8, NOTE_F6, -8,

  NOTE_D6, -8, REST, 16, NOTE_DS6, 8, REST, 8, //40
  NOTE_C6, 8, NOTE_F6, 16, NOTE_D6, 8, REST, 16, REST, 8,
  REST, 4, NOTE_C6, 8, NOTE_AS5, 8,
  NOTE_C6, -8,  NOTE_F6, -8, NOTE_D6, -4,
  NOTE_C6, 8, NOTE_AS5, 8,

  NOTE_C6, 8, NOTE_F6, 16, NOTE_D6, 8, REST, 16, REST, 8, //45
  REST, 4, NOTE_C6, 8, NOTE_D6, 8,
  NOTE_DS6, -8, NOTE_F6, -8,
  NOTE_D5, 8, NOTE_FS5, 8, NOTE_F5, 8, NOTE_A5, 8,
  NOTE_A5, -8, NOTE_G5, -4,

  NOTE_A5, -8, NOTE_G5, -4, //50
  NOTE_A5, -8, NOTE_G5, -4,
  NOTE_AS5, 8, NOTE_A5, 8, NOTE_G5, 8, NOTE_F5, 8,
  NOTE_A5, -8, NOTE_G5, -8, NOTE_D5, 8,
  NOTE_A5, -8, NOTE_G5, -8, NOTE_D5, 8,
  NOTE_A5, -8, NOTE_G5, -8, NOTE_D5, 8,

  NOTE_AS5, 4, NOTE_C6, 4, NOTE_A5, 4, NOTE_AS5, 4,
  NOTE_G5,16, NOTE_D5,16, NOTE_D6,16, NOTE_D5,16, NOTE_C6,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16,//56 //r
  NOTE_A5,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16, NOTE_A5,16, NOTE_D5,16, NOTE_G5,16, NOTE_D5,16,
  NOTE_A5,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16, NOTE_C6,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16,
  NOTE_A5,16, NOTE_D5,16, NOTE_F5,16, NOTE_D5,16, NOTE_A5,16, NOTE_D5,16, NOTE_G5,16, NOTE_D5,16,

  NOTE_G5,16, NOTE_D5,16, NOTE_D6,16, NOTE_D5,16, NOTE_C6,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16,//61
  NOTE_A5,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16, NOTE_A5,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16,
  NOTE_A5,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16, NOTE_C6,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16,
  NOTE_A5,16, NOTE_D5,16, NOTE_F5,16, NOTE_D5,16, NOTE_A5,16, NOTE_D5,16, NOTE_G5,16, NOTE_D5,16,
  NOTE_G5,16, NOTE_D5,16, NOTE_D6,16, NOTE_D5,16, NOTE_C6,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16,

  NOTE_A5,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16, NOTE_A5,16, NOTE_D5,16, NOTE_G5,16, NOTE_D5,16,//66
  NOTE_A5,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16, NOTE_C6,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16,
  NOTE_A5,16, NOTE_D5,16, NOTE_F5,16, NOTE_D5,16, NOTE_A5,16, NOTE_D5,16, NOTE_G5,16, NOTE_D5,16,
  NOTE_AS5,16, NOTE_D5,16, NOTE_D6,16, NOTE_D5,16, NOTE_C6,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16,
  NOTE_A5,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16, NOTE_A5,16, NOTE_D5,16, NOTE_G5,16, NOTE_D5,16,

  NOTE_A5,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16, NOTE_C6,16, NOTE_D5,16, NOTE_AS5,16, NOTE_D5,16,//71 //
  NOTE_A5, 16, NOTE_D5, 16, NOTE_F5, 16, NOTE_D5, 16, NOTE_A5, 8, NOTE_G5, 32, NOTE_A5, 32, NOTE_AS5, 32, NOTE_C6, 32,
  NOTE_D6, 16, NOTE_G5, 16, NOTE_AS5, 16, NOTE_G5, 16, NOTE_C6, 16, NOTE_G5, 16, NOTE_D6, 16, NOTE_G5, 16,
  NOTE_C6, 16, NOTE_G5, 16, NOTE_A5, 16, NOTE_G5, 16, NOTE_F6, 16, NOTE_G5, 16, NOTE_D6, 16, NOTE_DS5, 16,
  NOTE_D6, 4, REST, 4,

  NOTE_C5, 8, REST, 8, NOTE_A4, -16, NOTE_AS4, -16, NOTE_C5, 16, //76
  NOTE_D6, 16, NOTE_G4, 16, NOTE_AS4, 16, NOTE_G4, 16, NOTE_C5, 16, NOTE_G4, 16, NOTE_D6, 16, NOTE_G4, 16,
  NOTE_C6, 16, NOTE_F4, 16, NOTE_A4, 16, NOTE_F4, 16, NOTE_F5, 16, NOTE_F4, 16, NOTE_D6, 16, NOTE_DS4, 16,
  NOTE_D6, 16, REST, 8, NOTE_E4, 16, NOTE_F4, 16,
  
  //change of key B Major A# C# D# F# G#
  NOTE_GS4, 8, REST, 8, NOTE_AS4, 8, REST, 8,

  NOTE_DS5, 16, NOTE_GS4, 16, NOTE_B4, 16, NOTE_GS4, 16, NOTE_CS5, 16, NOTE_GS4, 16, NOTE_DS5, 16, NOTE_GS4, 16, //81
  NOTE_CS5, 16, NOTE_FS4, 16, NOTE_AS4, 16, NOTE_FS4, 16, NOTE_FS5, 16, NOTE_FS4, 16, NOTE_DS5, 16, NOTE_E5, 16,
  NOTE_D5, 4, REST, 4,
  NOTE_CS5, 8, REST, 8, NOTE_AS4, -16,  NOTE_B4, -16, NOTE_CS5, 16,
  NOTE_DS5, 16, NOTE_GS4, 16, NOTE_B4, 16, NOTE_GS4, 16, NOTE_CS5, 16, NOTE_GS4, 16, NOTE_DS5, 16, NOTE_GS4, 16,
  
  NOTE_CS5, 16, NOTE_FS4, 16, NOTE_AS4, 16, NOTE_FS4, 16, NOTE_FS5, 16, NOTE_FS4, 16, NOTE_DS5, 16, NOTE_E5, 16,
  NOTE_DS5, 4, REST, 8, NOTE_DS5, 16,  NOTE_E5, 16,
  NOTE_FS5, 16, NOTE_CS5, 16, NOTE_E5, 16, NOTE_CS4, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_G5, 16, NOTE_AS5, 16,
  NOTE_GS5, 16, NOTE_DS5, 16, NOTE_DS6, 16, NOTE_DS5, 16, NOTE_CS6, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16,

  NOTE_AS5, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16, NOTE_AS5, 16, NOTE_DS5, 16, NOTE_GS5, 16, NOTE_DS5, 16, //90
  NOTE_AS5, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16, NOTE_CS6, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16,
  NOTE_AS5, 16, NOTE_DS5, 16, NOTE_FS5, 16, NOTE_DS5, 16, NOTE_AS5, 16, NOTE_DS5, 16, NOTE_GS5, 16, NOTE_DS5, 16,
  NOTE_GS5, 16, NOTE_DS5, 16, NOTE_DS6, 16, NOTE_DS5, 16, NOTE_CS6, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16,

  NOTE_AS5, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16, NOTE_AS5, 16, NOTE_DS5, 16, NOTE_GS5, 16, NOTE_DS5, 16,//94
  NOTE_AS5, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16, NOTE_CS6, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16,
  NOTE_AS5, 16, NOTE_DS5, 16, NOTE_FS5, 16, NOTE_DS5, 16, NOTE_AS5, 16, NOTE_DS5, 16, NOTE_GS5, 16, NOTE_DS5, 16,
  NOTE_GS5, 16, NOTE_DS5, 16, NOTE_DS6, 16, NOTE_DS5, 16, NOTE_CS6, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16,

  NOTE_AS5, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16, NOTE_AS5, 16, NOTE_DS5, 16, NOTE_GS5, 16, NOTE_DS5, 16,//98
  NOTE_AS5, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16, NOTE_CS6, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16,
  NOTE_AS5, 16, NOTE_DS5, 16, NOTE_FS5, 16, NOTE_DS5, 16, NOTE_AS5, 16, NOTE_DS5, 16, NOTE_GS5, 16, NOTE_DS5, 16,
  NOTE_GS5, 16, NOTE_DS5, 16, NOTE_DS6, 16, NOTE_DS5, 16, NOTE_CS6, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16,

  NOTE_AS5, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16, NOTE_AS5, 16, NOTE_DS5, 16, NOTE_GS5, 16, NOTE_DS5, 16,//102
  NOTE_AS5, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16, NOTE_CS6, 16, NOTE_DS5, 16, NOTE_B5, 16, NOTE_DS5, 16,
  NOTE_AS5, 16, NOTE_DS5, 16, NOTE_FS5, 16, NOTE_DS5, 16, NOTE_AS5, 16, NOTE_DS5, 16, NOTE_GS5, 16, NOTE_DS5, 16,
  
  NOTE_CS6, 8, NOTE_FS6, 16, NOTE_DS6, 8, REST,16, REST,8, //107
  REST,4, NOTE_CS6, 8, NOTE_B5, 8,
  NOTE_CS6,-8, NOTE_FS6, -8, NOTE_DS6, -4,
  NOTE_CS6, 8, NOTE_B5, 8,
  NOTE_CS6, 8, NOTE_FS6, 16, NOTE_DS6, 8, REST,16, REST,8,
  REST,4, NOTE_CS6, 8, NOTE_B5, 8,
  NOTE_E6, -8, NOTE_F6, -8,
  
  NOTE_DS6,-8, REST,16, NOTE_E6,8, REST,16, REST,16, //112
  NOTE_CS6, 8, NOTE_FS6, 16, NOTE_DS6, 8, REST,16, REST,8,
  REST,4, NOTE_CS6, 8, NOTE_B5, 8,
  NOTE_CS6,-8, NOTE_FS6, -8, NOTE_DS6, -4,
  NOTE_CS6, 8, NOTE_B5, 8,
  
  NOTE_CS6, 8, NOTE_FS6, 16, NOTE_DS6, 8, REST,16, REST,8, //117
  REST,4, NOTE_CS5, 8, NOTE_DS5, 8,
  NOTE_E5, -8, NOTE_F5, -8,
  NOTE_DS5, 8, NOTE_G5, 8, NOTE_GS5, 8, NOTE_AS5, 8,
  NOTE_AS5, -8, NOTE_GS5, -8,

  NOTE_AS5, -8, NOTE_GS5, -8,//122
  NOTE_AS5, -8, NOTE_GS5, -8,
  NOTE_B6, 8, NOTE_AS5, 8, NOTE_GS5, 8, NOTE_FS5, 8,
  NOTE_AS5,-8, NOTE_GS6, -8, NOTE_DS5, 8,
  NOTE_AS5,-8, NOTE_GS6, -8, NOTE_DS5, 8,
  NOTE_AS5,-8, NOTE_GS6, -8, NOTE_DS5, 8,

  NOTE_B5,8, NOTE_CS6, 8, NOTE_AS5, 8, NOTE_B5, 8,//128
  NOTE_GS5,8, REST,8, REST, 16
  
};
int melody9[] = {REST,2, NOTE_D5,8, NOTE_B4,4, NOTE_D5,8, //1
  NOTE_CS5,4, NOTE_D5,8, NOTE_CS5,4, NOTE_A4,2, 
  REST,8, NOTE_A4,8, NOTE_FS5,8, NOTE_E5,4, NOTE_D5,8,
  NOTE_CS5,4, NOTE_D5,8, NOTE_CS5,4, NOTE_A4,2, 
  REST,4, NOTE_D5,8, NOTE_B4,4, NOTE_D5,8,
  NOTE_CS5,4, NOTE_D5,8, NOTE_CS5,4, NOTE_A4,2, 

  REST,8, NOTE_B4,8, NOTE_B4,8, NOTE_G4,4, NOTE_B4,8, //7
  NOTE_A4,4, NOTE_B4,8, NOTE_A4,4, NOTE_D4,2,
  REST,4, NOTE_D5,8, NOTE_B4,4, NOTE_D5,8,
  NOTE_CS5,4, NOTE_D5,8, NOTE_CS5,4, NOTE_A4,2, 
  REST,8, NOTE_A4,8, NOTE_FS5,8, NOTE_E5,4, NOTE_D5,8,
  NOTE_CS5,4, NOTE_D5,8, NOTE_CS5,4, NOTE_A4,2, 

  REST,4, NOTE_D5,8, NOTE_B4,4, NOTE_D5,8, //13
  NOTE_CS5,4, NOTE_D5,8, NOTE_CS5,4, NOTE_A4,2, 
  REST,8, NOTE_B4,8, NOTE_B4,8, NOTE_G4,4, NOTE_B4,8,
  NOTE_A4,4, NOTE_B4,8, NOTE_A4,4, NOTE_D4,8, NOTE_D4,8, NOTE_FS4,8,
  NOTE_E4,-1,
  REST,8, NOTE_D4,8, NOTE_E4,8, NOTE_FS4,-1,

  REST,8, NOTE_D4,8, NOTE_D4,8, NOTE_FS4,8, NOTE_F4,-1, //20
  REST,8, NOTE_D4,8, NOTE_F4,8, NOTE_E4,-1, //end 1

  //repeats from 1

  REST,2, NOTE_D5,8, NOTE_B4,4, NOTE_D5,8, //1
  NOTE_CS5,4, NOTE_D5,8, NOTE_CS5,4, NOTE_A4,2, 
  REST,8, NOTE_A4,8, NOTE_FS5,8, NOTE_E5,4, NOTE_D5,8,
  NOTE_CS5,4, NOTE_D5,8, NOTE_CS5,4, NOTE_A4,2, 
  REST,4, NOTE_D5,8, NOTE_B4,4, NOTE_D5,8,
  NOTE_CS5,4, NOTE_D5,8, NOTE_CS5,4, NOTE_A4,2, 

  REST,8, NOTE_B4,8, NOTE_B4,8, NOTE_G4,4, NOTE_B4,8, //7
  NOTE_A4,4, NOTE_B4,8, NOTE_A4,4, NOTE_D4,2,
  REST,4, NOTE_D5,8, NOTE_B4,4, NOTE_D5,8,
  NOTE_CS5,4, NOTE_D5,8, NOTE_CS5,4, NOTE_A4,2, 
  REST,8, NOTE_A4,8, NOTE_FS5,8, NOTE_E5,4, NOTE_D5,8,
  NOTE_CS5,4, NOTE_D5,8, NOTE_CS5,4, NOTE_A4,2, 

  REST,4, NOTE_D5,8, NOTE_B4,4, NOTE_D5,8, //13
  NOTE_CS5,4, NOTE_D5,8, NOTE_CS5,4, NOTE_A4,2, 
  REST,8, NOTE_B4,8, NOTE_B4,8, NOTE_G4,4, NOTE_B4,8,
  NOTE_A4,4, NOTE_B4,8, NOTE_A4,4, NOTE_D4,8, NOTE_D4,8, NOTE_FS4,8,
  NOTE_E4,-1,
  REST,8, NOTE_D4,8, NOTE_E4,8, NOTE_FS4,-1,

  REST,8, NOTE_D4,8, NOTE_D4,8, NOTE_FS4,8, NOTE_F4,-1, //20
  REST,8, NOTE_D4,8, NOTE_F4,8, NOTE_E4,8, //end 2
  NOTE_E4,-2, NOTE_A4,8, NOTE_CS5,8, 
  NOTE_FS5,8, NOTE_E5,4, NOTE_D5,8, NOTE_A5,-4,

};
int melody10[] = { NOTE_FS4,8, REST,8, NOTE_A4,8, NOTE_CS5,8, REST,8,NOTE_A4,8, REST,8, NOTE_FS4,8, //1
  NOTE_D4,8, NOTE_D4,8, NOTE_D4,8, REST,8, REST,4, REST,8, NOTE_CS4,8,
  NOTE_D4,8, NOTE_FS4,8, NOTE_A4,8, NOTE_CS5,8, REST,8, NOTE_A4,8, REST,8, NOTE_F4,8,
  NOTE_E5,-4, NOTE_DS5,8, NOTE_D5,8, REST,8, REST,4,
  
  NOTE_GS4,8, REST,8, NOTE_CS5,8, NOTE_FS4,8, REST,8,NOTE_CS5,8, REST,8, NOTE_GS4,8, //5
  REST,8, NOTE_CS5,8, NOTE_G4,8, NOTE_FS4,8, REST,8, NOTE_E4,8, REST,8,
  NOTE_E4,8, NOTE_E4,8, NOTE_E4,8, REST,8, REST,4, NOTE_E4,8, NOTE_E4,8,
  NOTE_E4,8, REST,8, REST,4, NOTE_DS4,8, NOTE_D4,8, 

  NOTE_CS4,8, REST,8, NOTE_A4,8, NOTE_CS5,8, REST,8,NOTE_A4,8, REST,8, NOTE_FS4,8, //9
  NOTE_D4,8, NOTE_D4,8, NOTE_D4,8, REST,8, NOTE_E5,8, NOTE_E5,8, NOTE_E5,8, REST,8,
  REST,8, NOTE_FS4,8, NOTE_A4,8, NOTE_CS5,8, REST,8, NOTE_A4,8, REST,8, NOTE_F4,8,
  NOTE_E5,2, NOTE_D5,8, REST,8, REST,4,

  NOTE_B4,8, NOTE_G4,8, NOTE_D4,8, NOTE_CS4,4, NOTE_B4,8, NOTE_G4,8, NOTE_CS4,8, //13
  NOTE_A4,8, NOTE_FS4,8, NOTE_C4,8, NOTE_B3,4, NOTE_F4,8, NOTE_D4,8, NOTE_B3,8,
  NOTE_E4,8, NOTE_E4,8, NOTE_E4,8, REST,4, REST,4, NOTE_AS4,4,
  NOTE_CS5,8, NOTE_D5,8, NOTE_FS5,8, NOTE_A5,8, REST,8, REST,4, 

  REST,2, NOTE_A3,4, NOTE_AS3,4, //17 
  NOTE_A3,-4, NOTE_A3,8, NOTE_A3,2,
  REST,4, NOTE_A3,8, NOTE_AS3,8, NOTE_A3,8, NOTE_F4,4, NOTE_C4,8,
  NOTE_A3,-4, NOTE_A3,8, NOTE_A3,2,

  REST,2, NOTE_B3,4, NOTE_C4,4, //21
  NOTE_CS4,-4, NOTE_C4,8, NOTE_CS4,2,
  REST,4, NOTE_CS4,8, NOTE_C4,8, NOTE_CS4,8, NOTE_GS4,4, NOTE_DS4,8,
  NOTE_CS4,-4, NOTE_DS4,8, NOTE_B3,1,
  
  NOTE_E4,4, NOTE_E4,4, NOTE_E4,4, REST,8,//25

  //repeats 1-25

  NOTE_FS4,8, REST,8, NOTE_A4,8, NOTE_CS5,8, REST,8,NOTE_A4,8, REST,8, NOTE_FS4,8, //1
  NOTE_D4,8, NOTE_D4,8, NOTE_D4,8, REST,8, REST,4, REST,8, NOTE_CS4,8,
  NOTE_D4,8, NOTE_FS4,8, NOTE_A4,8, NOTE_CS5,8, REST,8, NOTE_A4,8, REST,8, NOTE_F4,8,
  NOTE_E5,-4, NOTE_DS5,8, NOTE_D5,8, REST,8, REST,4,
  
  NOTE_GS4,8, REST,8, NOTE_CS5,8, NOTE_FS4,8, REST,8,NOTE_CS5,8, REST,8, NOTE_GS4,8, //5
  REST,8, NOTE_CS5,8, NOTE_G4,8, NOTE_FS4,8, REST,8, NOTE_E4,8, REST,8,
  NOTE_E4,8, NOTE_E4,8, NOTE_E4,8, REST,8, REST,4, NOTE_E4,8, NOTE_E4,8,
  NOTE_E4,8, REST,8, REST,4, NOTE_DS4,8, NOTE_D4,8, 

  NOTE_CS4,8, REST,8, NOTE_A4,8, NOTE_CS5,8, REST,8,NOTE_A4,8, REST,8, NOTE_FS4,8, //9
  NOTE_D4,8, NOTE_D4,8, NOTE_D4,8, REST,8, NOTE_E5,8, NOTE_E5,8, NOTE_E5,8, REST,8,
  REST,8, NOTE_FS4,8, NOTE_A4,8, NOTE_CS5,8, REST,8, NOTE_A4,8, REST,8, NOTE_F4,8,
  NOTE_E5,2, NOTE_D5,8, REST,8, REST,4,

  NOTE_B4,8, NOTE_G4,8, NOTE_D4,8, NOTE_CS4,4, NOTE_B4,8, NOTE_G4,8, NOTE_CS4,8, //13
  NOTE_A4,8, NOTE_FS4,8, NOTE_C4,8, NOTE_B3,4, NOTE_F4,8, NOTE_D4,8, NOTE_B3,8,
  NOTE_E4,8, NOTE_E4,8, NOTE_E4,8, REST,4, REST,4, NOTE_AS4,4,
  NOTE_CS5,8, NOTE_D5,8, NOTE_FS5,8, NOTE_A5,8, REST,8, REST,4, 

  REST,2, NOTE_A3,4, NOTE_AS3,4, //17 
  NOTE_A3,-4, NOTE_A3,8, NOTE_A3,2,
  REST,4, NOTE_A3,8, NOTE_AS3,8, NOTE_A3,8, NOTE_F4,4, NOTE_C4,8,
  NOTE_A3,-4, NOTE_A3,8, NOTE_A3,2,

  REST,2, NOTE_B3,4, NOTE_C4,4, //21
  NOTE_CS4,-4, NOTE_C4,8, NOTE_CS4,2,
  REST,4, NOTE_CS4,8, NOTE_C4,8, NOTE_CS4,8, NOTE_GS4,4, NOTE_DS4,8,
  NOTE_CS4,-4, NOTE_DS4,8, NOTE_B3,1,
  
  NOTE_E4,4, NOTE_E4,4, NOTE_E4,4, REST,8,//25

  //finishes with 26
  //NOTE_FS4,8, REST,8, NOTE_A4,8, NOTE_CS5,8, REST,8, NOTE_A4,8, REST,8, NOTE_FS4,8
   
};
int melody11[] = {  NOTE_D5,1, 
  NOTE_DS5,1,
  
  NOTE_F5,1, //7
  REST,4,  NOTE_F5,-4, NOTE_DS5,8,  NOTE_D5,8, NOTE_F5,1, NOTE_AS4,8, 
  NOTE_G4,-2, NOTE_F4,1, 
  NOTE_F4,1,
   
  REST,4, //12
  REST,8,
  NOTE_F4,8, NOTE_G4,8, NOTE_GS4,8, NOTE_AS4,8, NOTE_C5,8, 
  NOTE_D5,1, 
  NOTE_DS5,1,
  NOTE_F5,1,
  NOTE_F5,-4,  NOTE_DS5,8, NOTE_D5,8, NOTE_CS5,8,
  NOTE_C5,-2, NOTE_AS4,8,

  NOTE_G4,1, //18
  NOTE_F4,-1,
  REST,4,
  NOTE_D5,-4, REST,16, NOTE_D5,16, NOTE_D5,2, 
  REST,4, NOTE_D5,8, NOTE_DS5,8, NOTE_F5,8, NOTE_G5,8, NOTE_F5,8, NOTE_DS5,8, NOTE_D5,8,  

 
  NOTE_D5,-4, NOTE_DS5,16, NOTE_DS5,2, //23
  REST,4, NOTE_G4,8, NOTE_C5,8, NOTE_D5,8, NOTE_DS5,8, NOTE_F5,8, NOTE_DS5,8, NOTE_D5,8,
  NOTE_C5,-4, REST,16, NOTE_G4,2,
  REST,4, NOTE_G4,8, NOTE_GS4,8, NOTE_AS4,8, NOTE_C5,8, NOTE_AS4,8, NOTE_GS4,8, NOTE_G5,8,
  
  NOTE_F4,-4,  NOTE_AS4,-4, NOTE_G4,2, //27
  REST,8, NOTE_C4,8, NOTE_D4,8, NOTE_DS4,8, NOTE_G4,8, NOTE_C5,8,
  NOTE_D5,-4, REST,16, NOTE_D5,-16, NOTE_D5,2,
  REST,4, NOTE_D5,8, NOTE_DS5,8, NOTE_F5,8, NOTE_G5,8, NOTE_F5,8, NOTE_DS5,8, NOTE_D5,8,  
  NOTE_D5,-4, NOTE_DS5,-16, NOTE_DS5,2,
  
  REST,4, NOTE_C5,8, NOTE_D5,8, NOTE_DS5,8, NOTE_F5,8, NOTE_DS5,8, NOTE_D5,8, NOTE_AS4,8,//32
  NOTE_AS4,-4, NOTE_C5,-4, NOTE_C5,-4,
  NOTE_F4,-4, REST,8, NOTE_G4,4, NOTE_D5,4, NOTE_DS5,4,
  NOTE_D5,-4, REST,16, NOTE_C5,16, NOTE_C5,2, 
  
  REST,4, NOTE_D5,4, NOTE_DS5,4, NOTE_F5,4, //36 
  NOTE_G5,-4, REST,16, NOTE_F5,2,
  NOTE_AS5,-4, NOTE_G5,-4, NOTE_DS5,4,
  
  NOTE_D5,-4, REST,16, NOTE_DS5,2, //39
  REST,4, NOTE_C5,8, NOTE_D5,8, NOTE_DS5,8, NOTE_E5,8, NOTE_F5,8, NOTE_FS5,8,
  NOTE_G5,-4, NOTE_F5,-4, REST,4,  NOTE_AS5,2,

  NOTE_G5,4, NOTE_F5,8,  NOTE_G5,8,  REST,8, NOTE_E5,8,//42
  REST,8, NOTE_D5,8, NOTE_C5,-2, 
  REST,8, NOTE_G4,8, NOTE_A4,8, NOTE_AS4,8, NOTE_C5,8, NOTE_D5,8, NOTE_DS5,8, 

  NOTE_DS5,-4,  NOTE_D5,-4,  NOTE_AS4,4, //45
  REST,4, NOTE_DS5,8, NOTE_E5,8,  NOTE_F5,4, NOTE_E5,8, NOTE_DS5,8, NOTE_D5,8, NOTE_AS5,8,
  NOTE_C5,4, NOTE_G4,8, NOTE_D5,4, NOTE_G4,8, NOTE_D5,4, 
  REST,8, NOTE_FS5,8, NOTE_G5,8, NOTE_FS5,8, NOTE_F5,8, NOTE_DS5,8, NOTE_D5,8, NOTE_DS5,8,

  REST, 8, NOTE_AS5,8, NOTE_G5,8, NOTE_DS5,8, NOTE_F5,8, REST,8, NOTE_G5,8, //49
  REST,8, NOTE_FS5,8,  NOTE_F5,8, NOTE_DS5,8, NOTE_F5,8, NOTE_DS5,8, NOTE_D5,8, NOTE_DS5,8,
  NOTE_D5,-4,  NOTE_C5,-4, REST,4, 
  REST,4, NOTE_C5,8, NOTE_D5,8, NOTE_DS5,8, NOTE_D5,8, NOTE_C5,8, NOTE_AS4,8,

  NOTE_D5,8,  NOTE_DS5,8,  NOTE_F5,8, NOTE_G5,8, NOTE_D5,8, NOTE_C5,8, NOTE_D5,8, NOTE_DS5,8,//53
  NOTE_F5,8,  NOTE_G5,8,  NOTE_AS5,8, NOTE_GS5,8, NOTE_G5,8, NOTE_F5,8, NOTE_DS5,8, NOTE_F5,8,
  NOTE_DS5,8,  NOTE_D5,16, NOTE_DS5,16, NOTE_D5,16, NOTE_AS4,8, NOTE_C5,8, NOTE_D5,8, NOTE_DS5,8, NOTE_F5,8,
  NOTE_G5,8, NOTE_AS5,8, NOTE_GS5,8, NOTE_G5,8, NOTE_F5,8, NOTE_DS5,8, NOTE_D5,8, NOTE_DS5,8,
  
  NOTE_C5,8,  NOTE_D5,8,  NOTE_DS5,8, NOTE_F5,8, NOTE_C5,8, NOTE_G4,8, NOTE_C5,8, NOTE_D5,8,//57
  NOTE_DS5,8,  NOTE_F5,8,  NOTE_AS5,8, NOTE_F5,8, NOTE_DS5,8, NOTE_D5,8, NOTE_AS4,8, NOTE_DS5,8,
  NOTE_D5,8,  NOTE_D5,16, NOTE_DS5,16, NOTE_D5,16, NOTE_G4,8, NOTE_C5,8, NOTE_D5,8, NOTE_DS5,8, NOTE_F5,8,
  NOTE_F5,8, NOTE_AS5,8, NOTE_F5,8, NOTE_DS5,8, NOTE_D5,8, NOTE_DS5,8, NOTE_D5,8, NOTE_AS4,8,
    
  NOTE_D5,8,  NOTE_DS5,8,  NOTE_F5,8, NOTE_G5,8, NOTE_AS4,8, NOTE_G4,8, NOTE_AS4,8, NOTE_DS5,8,//61
  NOTE_AS5,8,  NOTE_DS5,8,  NOTE_AS5,8, NOTE_GS5,8, NOTE_G5,8, NOTE_GS5,8, NOTE_G5,8, NOTE_F5,8,
  NOTE_DS5,8,  NOTE_D5,16, NOTE_DS5,16, NOTE_D5,16, NOTE_AS4,8, NOTE_C5,8, NOTE_D5,8, NOTE_DS5,8, NOTE_F5,8,
  
  NOTE_C6,8, NOTE_D5,8, NOTE_AS5,8, NOTE_D5,8, NOTE_C5,8, NOTE_D5,8, NOTE_B5,8, NOTE_G4,8, //64
  NOTE_C4,8, NOTE_DS4,8, NOTE_G4,8, NOTE_C5,8, NOTE_DS5,8, NOTE_G5,8, REST,8, NOTE_C5,8,
  NOTE_D5,8, NOTE_DS5,8, NOTE_D5,16, NOTE_DS5,16, NOTE_D5,16, NOTE_C5,8, NOTE_G4,8, NOTE_C5,8, NOTE_G5,8,
  NOTE_D5,-4, NOTE_C5,8, NOTE_C5,1,

  REST,4,  //68
  NOTE_DS4,8, NOTE_C4,-4, NOTE_DS4,2,
  NOTE_D6,2, NOTE_B3,2,
  NOTE_DS4,8, NOTE_C4,-4, NOTE_G3,2,
  NOTE_D6,2, NOTE_B3,2,
  NOTE_DS4,8, NOTE_C4,-4, NOTE_G4,2,
  NOTE_FS4,2, NOTE_D4,2,
  NOTE_F4,2, NOTE_D4,2,
  NOTE_D4,2, NOTE_G4,2,
  
  NOTE_G4,1, //77 these shold be tied together :(  
  NOTE_G4,1,
  NOTE_G4,1,
  NOTE_G4,1,
  REST,1,
  REST,1,
  NOTE_G4,1,
  NOTE_G4,1,
  NOTE_DS4,2, NOTE_G4,2, //repeat from here
  NOTE_G4,2, NOTE_C4,4, NOTE_D4,8, NOTE_DS4,8,

  NOTE_F4,2, NOTE_AS4,2, //87
  NOTE_AS4,2, NOTE_C4,4, NOTE_D4,8, NOTE_DS4,8,
  NOTE_DS4,2, NOTE_G4,-2,
  NOTE_F4,2, NOTE_G4,8, NOTE_F4,8, 
  NOTE_G4,-2, NOTE_D4,-1,
  NOTE_C4,2, NOTE_G4,-2,
  NOTE_F4,2,  NOTE_D4,8, NOTE_DS4,8,
  NOTE_F4,2, NOTE_AS3,2,
   
  NOTE_AS4,2, NOTE_C4,4, NOTE_D4,8, NOTE_DS4,8,
  NOTE_DS4,2, NOTE_AS4,-2,
  NOTE_GS4,2, NOTE_G4,8, NOTE_F4,8, NOTE_F4,8, 
  NOTE_G4,-1, 
  
  
  NOTE_DS4,2, NOTE_G4,2, //repeat from here
  NOTE_G4,2, NOTE_C4,4, NOTE_D4,8, NOTE_DS4,8,

  NOTE_F4,2, NOTE_AS4,2, //87
  NOTE_AS4,2, NOTE_C4,4, NOTE_D4,8, NOTE_DS4,8,
  NOTE_DS4,2, NOTE_G4,-2,
  NOTE_F4,2, NOTE_G4,8, NOTE_F4,8, 
  NOTE_G4,-2, NOTE_D4,-1,
  NOTE_C4,2, NOTE_G4,-2,
  NOTE_F4,2,  NOTE_D4,8, NOTE_DS4,8,
  NOTE_F4,2, NOTE_AS3,2,
   
  NOTE_AS4,2, NOTE_C4,4, NOTE_D4,8, NOTE_DS4,8,
  NOTE_DS4,2, NOTE_AS4,-2,
  NOTE_GS4,2, NOTE_G4,8, NOTE_F4,8, NOTE_F4,8, 
  NOTE_G4,-1,   
  
};
int melody12[] = {  NOTE_D4,4, NOTE_A4,4, NOTE_A4,4,
  REST,8, NOTE_E4,8, NOTE_B4,2,
  NOTE_F4,4, NOTE_C5,4, NOTE_C5,4,
  REST,8, NOTE_E4,8, NOTE_B4,2,
  NOTE_D4,4, NOTE_A4,4, NOTE_A4,4,
  REST,8, NOTE_E4,8, NOTE_B4,2,
  NOTE_F4,4, NOTE_C5,4, NOTE_C5,4,
  REST,8, NOTE_E4,8, NOTE_B4,2,
  NOTE_D4,8, NOTE_F4,8, NOTE_D5,2,
  
  NOTE_D4,8, NOTE_F4,8, NOTE_D5,2,
  NOTE_E5,-4, NOTE_F5,8, NOTE_E5,8, NOTE_E5,8,
  NOTE_E5,8, NOTE_C5,8, NOTE_A4,2,
  NOTE_A4,4, NOTE_D4,4, NOTE_F4,8, NOTE_G4,8,
  NOTE_A4,-2,
  NOTE_A4,4, NOTE_D4,4, NOTE_F4,8, NOTE_G4,8,
  NOTE_E4,-2,
  NOTE_D4,8, NOTE_F4,8, NOTE_D5,2,
  NOTE_D4,8, NOTE_F4,8, NOTE_D5,2,

  NOTE_E5,-4, NOTE_F5,8, NOTE_E5,8, NOTE_E5,8,
  NOTE_E5,8, NOTE_C5,8, NOTE_A4,2,
  NOTE_A4,4, NOTE_D4,4, NOTE_F4,8, NOTE_G4,8,
  NOTE_A4,2, NOTE_A4,4,
  NOTE_D4,1,
};
int melody13[] = { NOTE_E5,8, NOTE_E5,8, REST,8, NOTE_E5,8, REST,8, NOTE_C5,8, NOTE_E5,8, //1
  NOTE_G5,4, REST,4, NOTE_G4,8, REST,4, 
  NOTE_C5,-4, NOTE_G4,8, REST,4, NOTE_E4,-4, // 3
  NOTE_A4,4, NOTE_B4,4, NOTE_AS4,8, NOTE_A4,4,
  NOTE_G4,-8, NOTE_E5,-8, NOTE_G5,-8, NOTE_A5,4, NOTE_F5,8, NOTE_G5,8,
  REST,8, NOTE_E5,4,NOTE_C5,8, NOTE_D5,8, NOTE_B4,-4,
  NOTE_C5,-4, NOTE_G4,8, REST,4, NOTE_E4,-4, // repeats from 3
  NOTE_A4,4, NOTE_B4,4, NOTE_AS4,8, NOTE_A4,4,
  NOTE_G4,-8, NOTE_E5,-8, NOTE_G5,-8, NOTE_A5,4, NOTE_F5,8, NOTE_G5,8,
  REST,8, NOTE_E5,4,NOTE_C5,8, NOTE_D5,8, NOTE_B4,-4,

  
  REST,4, NOTE_G5,8, NOTE_FS5,8, NOTE_F5,8, NOTE_DS5,4, NOTE_E5,8,//7
  REST,8, NOTE_GS4,8, NOTE_A4,8, NOTE_C4,8, REST,8, NOTE_A4,8, NOTE_C5,8, NOTE_D5,8,
  REST,4, NOTE_DS5,4, REST,8, NOTE_D5,-4,
  NOTE_C5,2, REST,2,

  REST,4, NOTE_G5,8, NOTE_FS5,8, NOTE_F5,8, NOTE_DS5,4, NOTE_E5,8,//repeats from 7
  REST,8, NOTE_GS4,8, NOTE_A4,8, NOTE_C4,8, REST,8, NOTE_A4,8, NOTE_C5,8, NOTE_D5,8,
  REST,4, NOTE_DS5,4, REST,8, NOTE_D5,-4,
  NOTE_C5,2, REST,2,

  NOTE_C5,8, NOTE_C5,4, NOTE_C5,8, REST,8, NOTE_C5,8, NOTE_D5,4,//11
  NOTE_E5,8, NOTE_C5,4, NOTE_A4,8, NOTE_G4,2,

  NOTE_C5,8, NOTE_C5,4, NOTE_C5,8, REST,8, NOTE_C5,8, NOTE_D5,8, NOTE_E5,8,//13
  REST,1, 
  NOTE_C5,8, NOTE_C5,4, NOTE_C5,8, REST,8, NOTE_C5,8, NOTE_D5,4,
  NOTE_E5,8, NOTE_C5,4, NOTE_A4,8, NOTE_G4,2,
  NOTE_E5,8, NOTE_E5,8, REST,8, NOTE_E5,8, REST,8, NOTE_C5,8, NOTE_E5,4,
  NOTE_G5,4, REST,4, NOTE_G4,4, REST,4, 
  NOTE_C5,-4, NOTE_G4,8, REST,4, NOTE_E4,-4, // 19
  
  NOTE_A4,4, NOTE_B4,4, NOTE_AS4,8, NOTE_A4,4,
  NOTE_G4,-8, NOTE_E5,-8, NOTE_G5,-8, NOTE_A5,4, NOTE_F5,8, NOTE_G5,8,
  REST,8, NOTE_E5,4, NOTE_C5,8, NOTE_D5,8, NOTE_B4,-4,

  NOTE_C5,-4, NOTE_G4,8, REST,4, NOTE_E4,-4, // repeats from 19
  NOTE_A4,4, NOTE_B4,4, NOTE_AS4,8, NOTE_A4,4,
  NOTE_G4,-8, NOTE_E5,-8, NOTE_G5,-8, NOTE_A5,4, NOTE_F5,8, NOTE_G5,8,
  REST,8, NOTE_E5,4, NOTE_C5,8, NOTE_D5,8, NOTE_B4,-4,

  NOTE_E5,8, NOTE_C5,4, NOTE_G4,8, REST,4, NOTE_GS4,4,//23
  NOTE_A4,8, NOTE_F5,4, NOTE_F5,8, NOTE_A4,2,
  NOTE_D5,-8, NOTE_A5,-8, NOTE_A5,-8, NOTE_A5,-8, NOTE_G5,-8, NOTE_F5,-8,
  
  NOTE_E5,8, NOTE_C5,4, NOTE_A4,8, NOTE_G4,2, //26
  NOTE_E5,8, NOTE_C5,4, NOTE_G4,8, REST,4, NOTE_GS4,4,
  NOTE_A4,8, NOTE_F5,4, NOTE_F5,8, NOTE_A4,2,
  NOTE_B4,8, NOTE_F5,4, NOTE_F5,8, NOTE_F5,-8, NOTE_E5,-8, NOTE_D5,-8,
  NOTE_C5,8, NOTE_E4,4, NOTE_E4,8, NOTE_C4,2,

  NOTE_E5,8, NOTE_C5,4, NOTE_G4,8, REST,4, NOTE_GS4,4,//repeats from 23
  NOTE_A4,8, NOTE_F5,4, NOTE_F5,8, NOTE_A4,2,
  NOTE_D5,-8, NOTE_A5,-8, NOTE_A5,-8, NOTE_A5,-8, NOTE_G5,-8, NOTE_F5,-8,
  
  NOTE_E5,8, NOTE_C5,4, NOTE_A4,8, NOTE_G4,2, //26
  NOTE_E5,8, NOTE_C5,4, NOTE_G4,8, REST,4, NOTE_GS4,4,
  NOTE_A4,8, NOTE_F5,4, NOTE_F5,8, NOTE_A4,2,
  NOTE_B4,8, NOTE_F5,4, NOTE_F5,8, NOTE_F5,-8, NOTE_E5,-8, NOTE_D5,-8,
  NOTE_C5,8, NOTE_E4,4, NOTE_E4,8, NOTE_C4,2,
  NOTE_C5,8, NOTE_C5,4, NOTE_C5,8, REST,8, NOTE_C5,8, NOTE_D5,8, NOTE_E5,8,
  REST,1,

  NOTE_C5,8, NOTE_C5,4, NOTE_C5,8, REST,8, NOTE_C5,8, NOTE_D5,4, //33
  NOTE_E5,8, NOTE_C5,4, NOTE_A4,8, NOTE_G4,2,
  NOTE_E5,8, NOTE_E5,8, REST,8, NOTE_E5,8, REST,8, NOTE_C5,8, NOTE_E5,4,
  NOTE_G5,4, REST,4, NOTE_G4,4, REST,4, 
  NOTE_E5,8, NOTE_C5,4, NOTE_G4,8, REST,4, NOTE_GS4,4,
  NOTE_A4,8, NOTE_F5,4, NOTE_F5,8, NOTE_A4,2,
  NOTE_D5,-8, NOTE_A5,-8, NOTE_A5,-8, NOTE_A5,-8, NOTE_G5,-8, NOTE_F5,-8,
  
  NOTE_E5,8, NOTE_C5,4, NOTE_A4,8, NOTE_G4,2, //40
  NOTE_E5,8, NOTE_C5,4, NOTE_G4,8, REST,4, NOTE_GS4,4,
  NOTE_A4,8, NOTE_F5,4, NOTE_F5,8, NOTE_A4,2,
  NOTE_B4,8, NOTE_F5,4, NOTE_F5,8, NOTE_F5,-8, NOTE_E5,-8, NOTE_D5,-8,
  NOTE_C5,8, NOTE_E4,4, NOTE_E4,8, NOTE_C4,2,
  
  //game over sound
  NOTE_C5,-4, NOTE_G4,-4, NOTE_E4,4, //45
  NOTE_A4,-8, NOTE_B4,-8, NOTE_A4,-8, NOTE_GS4,-8, NOTE_AS4,-8, NOTE_GS4,-8,
  NOTE_G4,8, NOTE_D4,8, NOTE_E4,-2,  

};
int melody14[] = {  NOTE_E5, 4,  NOTE_B4,8,  NOTE_C5,8,  NOTE_D5,4,  NOTE_C5,8,  NOTE_B4,8,
  NOTE_A4, 4,  NOTE_A4,8,  NOTE_C5,8,  NOTE_E5,4,  NOTE_D5,8,  NOTE_C5,8,
  NOTE_B4, -4,  NOTE_C5,8,  NOTE_D5,4,  NOTE_E5,4,
  NOTE_C5, 4,  NOTE_A4,4,  NOTE_A4,8,  NOTE_A4,4,  NOTE_B4,8,  NOTE_C5,8,

  NOTE_D5, -4,  NOTE_F5,8,  NOTE_A5,4,  NOTE_G5,8,  NOTE_F5,8,
  NOTE_E5, -4,  NOTE_C5,8,  NOTE_E5,4,  NOTE_D5,8,  NOTE_C5,8,
  NOTE_B4, 4,  NOTE_B4,8,  NOTE_C5,8,  NOTE_D5,4,  NOTE_E5,4,
  NOTE_C5, 4,  NOTE_A4,4,  NOTE_A4,4, REST, 4,

  NOTE_E5, 4,  NOTE_B4,8,  NOTE_C5,8,  NOTE_D5,4,  NOTE_C5,8,  NOTE_B4,8,
  NOTE_A4, 4,  NOTE_A4,8,  NOTE_C5,8,  NOTE_E5,4,  NOTE_D5,8,  NOTE_C5,8,
  NOTE_B4, -4,  NOTE_C5,8,  NOTE_D5,4,  NOTE_E5,4,
  NOTE_C5, 4,  NOTE_A4,4,  NOTE_A4,8,  NOTE_A4,4,  NOTE_B4,8,  NOTE_C5,8,

  NOTE_D5, -4,  NOTE_F5,8,  NOTE_A5,4,  NOTE_G5,8,  NOTE_F5,8,
  NOTE_E5, -4,  NOTE_C5,8,  NOTE_E5,4,  NOTE_D5,8,  NOTE_C5,8,
  NOTE_B4, 4,  NOTE_B4,8,  NOTE_C5,8,  NOTE_D5,4,  NOTE_E5,4,
  NOTE_C5, 4,  NOTE_A4,4,  NOTE_A4,4, REST, 4,
  

  NOTE_E5,2,  NOTE_C5,2,
  NOTE_D5,2,   NOTE_B4,2,
  NOTE_C5,2,   NOTE_A4,2,
  NOTE_GS4,2,  NOTE_B4,4,  REST,8, 
  NOTE_E5,2,   NOTE_C5,2,
  NOTE_D5,2,   NOTE_B4,2,
  NOTE_C5,4,   NOTE_E5,4,  NOTE_A5,2,
  NOTE_GS5,2,

};
int melody15[] = {  NOTE_E4,2, NOTE_G4,4,
  NOTE_D4,2, NOTE_C4,8, NOTE_D4,8, 
  NOTE_E4,2, NOTE_G4,4,
  NOTE_D4,-2,
  NOTE_E4,2, NOTE_G4,4,
  NOTE_D5,2, NOTE_C5,4,
  NOTE_G4,2, NOTE_F4,8, NOTE_E4,8, 
  NOTE_D4,-2,
  NOTE_E4,2, NOTE_G4,4,
  NOTE_D4,2, NOTE_C4,8, NOTE_D4,8, 
  NOTE_E4,2, NOTE_G4,4,
  NOTE_D4,-2,
  NOTE_E4,2, NOTE_G4,4,

  NOTE_D5,2, NOTE_C5,4,
  NOTE_G4,2, NOTE_F4,8, NOTE_E4,8, 
  NOTE_F4,8, NOTE_E4,8, NOTE_C4,2,
  NOTE_F4,2, NOTE_E4,8, NOTE_D4,8, 
  NOTE_E4,8, NOTE_D4,8, NOTE_A3,2,
  NOTE_G4,2, NOTE_F4,8, NOTE_E4,8, 
  NOTE_F4,8, NOTE_E4,8, NOTE_C4,4, NOTE_F4,4,
  NOTE_C5,-2, 
  
};
int melody16[] = {  NOTE_AS4,-2,  NOTE_F4,8,  NOTE_F4,8,  NOTE_AS4,8,//1
  NOTE_GS4,16,  NOTE_FS4,16,  NOTE_GS4,-2,
  NOTE_AS4,-2,  NOTE_FS4,8,  NOTE_FS4,8,  NOTE_AS4,8,
  NOTE_A4,16,  NOTE_G4,16,  NOTE_A4,-2,
  REST,1, 

  NOTE_AS4,4,  NOTE_F4,-4,  NOTE_AS4,8,  NOTE_AS4,16,  NOTE_C5,16, NOTE_D5,16, NOTE_DS5,16,//7
  NOTE_F5,2,  NOTE_F5,8,  NOTE_F5,8,  NOTE_F5,8,  NOTE_FS5,16, NOTE_GS5,16,
  NOTE_AS5,-2,  NOTE_AS5,8,  NOTE_AS5,8,  NOTE_GS5,8,  NOTE_FS5,16,
  NOTE_GS5,-8,  NOTE_FS5,16,  NOTE_F5,2,  NOTE_F5,4, 

  NOTE_DS5,-8, NOTE_F5,16, NOTE_FS5,2, NOTE_F5,8, NOTE_DS5,8, //11
  NOTE_CS5,-8, NOTE_DS5,16, NOTE_F5,2, NOTE_DS5,8, NOTE_CS5,8,
  NOTE_C5,-8, NOTE_D5,16, NOTE_E5,2, NOTE_G5,8, 
  NOTE_F5,16, NOTE_F4,16, NOTE_F4,16, NOTE_F4,16,NOTE_F4,16,NOTE_F4,16,NOTE_F4,16,NOTE_F4,16,NOTE_F4,8, NOTE_F4,16,NOTE_F4,8,

  NOTE_AS4,4,  NOTE_F4,-4,  NOTE_AS4,8,  NOTE_AS4,16,  NOTE_C5,16, NOTE_D5,16, NOTE_DS5,16,//15
  NOTE_F5,2,  NOTE_F5,8,  NOTE_F5,8,  NOTE_F5,8,  NOTE_FS5,16, NOTE_GS5,16,
  NOTE_AS5,-2, NOTE_CS6,4,
  NOTE_C6,4, NOTE_A5,2, NOTE_F5,4,
  NOTE_FS5,-2, NOTE_AS5,4,
  NOTE_A5,4, NOTE_F5,2, NOTE_F5,4,

  NOTE_FS5,-2, NOTE_AS5,4,
  NOTE_A5,4, NOTE_F5,2, NOTE_D5,4,
  NOTE_DS5,-2, NOTE_FS5,4,
  NOTE_F5,4, NOTE_CS5,2, NOTE_AS4,4,
  NOTE_C5,-8, NOTE_D5,16, NOTE_E5,2, NOTE_G5,8, 
  NOTE_F5,16, NOTE_F4,16, NOTE_F4,16, NOTE_F4,16,NOTE_F4,16,NOTE_F4,16,NOTE_F4,16,NOTE_F4,16,NOTE_F4,8, NOTE_F4,16,NOTE_F4,8
  
};
int melody17[] = { NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //1
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, -2,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //5
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, -2,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //9
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, -2,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //13
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_FS3, -16, NOTE_D3, -16, NOTE_B2, -16, NOTE_A3, -16, NOTE_FS3, -16, NOTE_B2, -16, NOTE_D3, -16, NOTE_FS3, -16, NOTE_A3, -16, NOTE_FS3, -16, NOTE_D3, -16, NOTE_B2, -16,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //17
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, -2,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //21
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_B3, -16, NOTE_G3, -16, NOTE_E3, -16, NOTE_G3, -16, NOTE_B3, -16, NOTE_E4, -16, NOTE_G3, -16, NOTE_B3, -16, NOTE_E4, -16, NOTE_B3, -16, NOTE_G4, -16, NOTE_B4, -16,

  NOTE_A2, 8, NOTE_A2, 8, NOTE_A3, 8, NOTE_A2, 8, NOTE_A2, 8, NOTE_G3, 8, NOTE_A2, 8, NOTE_A2, 8, //25
  NOTE_F3, 8, NOTE_A2, 8, NOTE_A2, 8, NOTE_DS3, 8, NOTE_A2, 8, NOTE_A2, 8, NOTE_E3, 8, NOTE_F3, 8,
  NOTE_A2, 8, NOTE_A2, 8, NOTE_A3, 8, NOTE_A2, 8, NOTE_A2, 8, NOTE_G3, 8, NOTE_A2, 8, NOTE_A2, 8,
  NOTE_F3, 8, NOTE_A2, 8, NOTE_A2, 8, NOTE_DS3, -2,

  NOTE_A2, 8, NOTE_A2, 8, NOTE_A3, 8, NOTE_A2, 8, NOTE_A2, 8, NOTE_G3, 8, NOTE_A2, 8, NOTE_A2, 8, //29
  NOTE_F3, 8, NOTE_A2, 8, NOTE_A2, 8, NOTE_DS3, 8, NOTE_A2, 8, NOTE_A2, 8, NOTE_E3, 8, NOTE_F3, 8,
  NOTE_A2, 8, NOTE_A2, 8, NOTE_A3, 8, NOTE_A2, 8, NOTE_A2, 8, NOTE_G3, 8, NOTE_A2, 8, NOTE_A2, 8,
  NOTE_A3, -16, NOTE_F3, -16, NOTE_D3, -16, NOTE_A3, -16, NOTE_F3, -16, NOTE_D3, -16, NOTE_C4, -16, NOTE_A3, -16, NOTE_F3, -16, NOTE_A3, -16, NOTE_F3, -16, NOTE_D3, -16,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //33
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, -2,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //37
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, -2,

  NOTE_CS3, 8, NOTE_CS3, 8, NOTE_CS4, 8, NOTE_CS3, 8, NOTE_CS3, 8, NOTE_B3, 8, NOTE_CS3, 8, NOTE_CS3, 8, //41
  NOTE_A3, 8, NOTE_CS3, 8, NOTE_CS3, 8, NOTE_G3, 8, NOTE_CS3, 8, NOTE_CS3, 8, NOTE_GS3, 8, NOTE_A3, 8,
  NOTE_B2, 8, NOTE_B2, 8, NOTE_B3, 8, NOTE_B2, 8, NOTE_B2, 8, NOTE_A3, 8, NOTE_B2, 8, NOTE_B2, 8,
  NOTE_G3, 8, NOTE_B2, 8, NOTE_B2, 8, NOTE_F3, -2,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //45
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_B3, -16, NOTE_G3, -16, NOTE_E3, -16, NOTE_G3, -16, NOTE_B3, -16, NOTE_E4, -16, NOTE_G3, -16, NOTE_B3, -16, NOTE_E4, -16, NOTE_B3, -16, NOTE_G4, -16, NOTE_B4, -16,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //49
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, -2,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //53
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_FS3, -16, NOTE_DS3, -16, NOTE_B2, -16, NOTE_FS3, -16, NOTE_DS3, -16, NOTE_B2, -16, NOTE_G3, -16, NOTE_D3, -16, NOTE_B2, -16, NOTE_DS4, -16, NOTE_DS3, -16, NOTE_B2, -16,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //57
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, -2,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //61
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_E4, -16, NOTE_B3, -16, NOTE_G3, -16, NOTE_G4, -16, NOTE_E4, -16, NOTE_G3, -16, NOTE_B3, -16, NOTE_D4, -16, NOTE_E4, -16, NOTE_G4, -16, NOTE_E4, -16, NOTE_G3, -16,  

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //65
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, -2,

  NOTE_A2, 8, NOTE_A2, 8, NOTE_A3, 8, NOTE_A2, 8, NOTE_A2, 8, NOTE_G3, 8, NOTE_A2, 8, NOTE_A2, 8, //69
  NOTE_F3, 8, NOTE_A2, 8, NOTE_A2, 8, NOTE_DS3, 8, NOTE_A2, 8, NOTE_A2, 8, NOTE_E3, 8, NOTE_F3, 8,
  NOTE_A2, 8, NOTE_A2, 8, NOTE_A3, 8, NOTE_A2, 8, NOTE_A2, 8, NOTE_G3, 8, NOTE_A2, 8, NOTE_A2, 8,
  NOTE_A3, -16, NOTE_F3, -16, NOTE_D3, -16, NOTE_A3, -16, NOTE_F3, -16, NOTE_D3, -16, NOTE_C4, -16, NOTE_A3, -16, NOTE_F3, -16, NOTE_A3, -16, NOTE_F3, -16, NOTE_D3, -16,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //73
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, -2,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //77
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, -2,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //81
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, -2,

  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8, //73
  NOTE_C3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_AS2, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_B2, 8, NOTE_C3, 8,
  NOTE_E2, 8, NOTE_E2, 8, NOTE_E3, 8, NOTE_E2, 8, NOTE_E2, 8, NOTE_D3, 8, NOTE_E2, 8, NOTE_E2, 8,
  NOTE_B3, -16, NOTE_G3, -16, NOTE_E3, -16, NOTE_B2, -16, NOTE_E3, -16, NOTE_G3, -16, NOTE_C4, -16, NOTE_B3, -16, NOTE_G3, -16, NOTE_B3, -16, NOTE_G3, -16, NOTE_E3, -16,  
};
int melody18[] = {  NOTE_D5,-4, NOTE_A5,8, NOTE_FS5,8, NOTE_D5,8,
  NOTE_E5,-4, NOTE_FS5,8, NOTE_G5,4,
  NOTE_FS5,-4, NOTE_E5,8, NOTE_FS5,4,
  NOTE_D5,-2,
  NOTE_D5,-4, NOTE_A5,8, NOTE_FS5,8, NOTE_D5,8,
  NOTE_E5,-4, NOTE_FS5,8, NOTE_G5,4,
  NOTE_FS5,-1,
  NOTE_D5,-4, NOTE_A5,8, NOTE_FS5,8, NOTE_D5,8,
  NOTE_E5,-4, NOTE_FS5,8, NOTE_G5,4,
  
  NOTE_FS5,-4, NOTE_E5,8, NOTE_FS5,4,
  NOTE_D5,-2,
  NOTE_D5,-4, NOTE_A5,8, NOTE_FS5,8, NOTE_D5,8,
  NOTE_E5,-4, NOTE_FS5,8, NOTE_G5,4,
  NOTE_FS5,-1,
  
};
int melody19[] = { NOTE_E5,16, NOTE_E5,8, NOTE_D5,16, REST,16, NOTE_CS5,-4, NOTE_E4,8, NOTE_FS4,16, NOTE_G4,16, NOTE_A4,16,
  
  NOTE_B4,-8, NOTE_E4,-8, NOTE_B4,8, NOTE_A4,16, NOTE_D5,-4, //7
  NOTE_E5,16, NOTE_E5,8, NOTE_D5,16, REST,16, NOTE_CS5,-4, NOTE_E4,8, NOTE_FS4,16, NOTE_G4,16, NOTE_A4,16,
  NOTE_B4,-8, NOTE_E4,-8, NOTE_B4,8, NOTE_A4,16, NOTE_D4,-4,
  REST,8, NOTE_E5,8, REST,16, NOTE_B5,16, REST,8, NOTE_AS5,16, NOTE_B5,16, NOTE_AS5,16, NOTE_G5,16, REST,4,

  NOTE_B5,8, NOTE_B5,16, NOTE_AS5,16, REST,16, NOTE_AS5,16, NOTE_A5,16, REST,16, NOTE_B5,16, NOTE_G5,16, NOTE_B5,16, NOTE_AS5,16, REST,16, NOTE_B5,16, NOTE_A5,16, NOTE_G5,16,//11
  REST,8, NOTE_E5,8, REST,16, NOTE_B5,16, REST,8, NOTE_AS5,16, NOTE_B5,16, NOTE_AS5,16, NOTE_G5,16, REST,4,
  NOTE_B5,8, NOTE_B5,16, NOTE_AS5,16, REST,16, NOTE_AS5,16, NOTE_A5,16, REST,16, NOTE_B5,16, NOTE_G5,16, NOTE_B5,16, NOTE_AS5,16, REST,16, NOTE_B5,16, NOTE_A5,16, NOTE_G5,16,

  NOTE_DS4,-8, NOTE_FS4,-8, NOTE_C5,8, NOTE_B4,-8, NOTE_G4,-8, NOTE_E4,8, //14
  NOTE_DS4,-8, NOTE_FS4,-8, NOTE_C5,8, NOTE_B4,-8, NOTE_G4,-8, REST,8,
  NOTE_DS4,-8, NOTE_FS4,-8, NOTE_C5,8, NOTE_B4,-8, NOTE_G4,-8, NOTE_E4,8,
  NOTE_DS4,-8, NOTE_FS4,-8, NOTE_C5,8, NOTE_B4,-8, NOTE_CS5,-8, NOTE_DS5,8,

  NOTE_E5,16, NOTE_E5,16, NOTE_E4,16, NOTE_E4,-2,//18
  NOTE_C4,8, NOTE_C4,8, NOTE_E4,16, NOTE_G4,-8, NOTE_D4,8, NOTE_D4,8, NOTE_FS4,16, NOTE_A4,-8,
  NOTE_E5,16, NOTE_E5,16, NOTE_E4,16, NOTE_E4,-2,
  NOTE_C4,8, NOTE_C4,8, NOTE_E4,16, NOTE_G4,-8, NOTE_D4,8, NOTE_D4,8, NOTE_B3,16, NOTE_D4,-8,

  //repeats a second time

  NOTE_E5,16, NOTE_E5,8, NOTE_D5,16, REST,16, NOTE_CS5,-4, NOTE_E4,8, NOTE_FS4,16, NOTE_G4,16, NOTE_A4,16,
  
  NOTE_B4,-8, NOTE_E4,-8, NOTE_B4,8, NOTE_A4,16, NOTE_D5,-4, //7
  NOTE_E5,16, NOTE_E5,8, NOTE_D5,16, REST,16, NOTE_CS5,-4, NOTE_E4,8, NOTE_FS4,16, NOTE_G4,16, NOTE_A4,16,
  NOTE_B4,-8, NOTE_E4,-8, NOTE_B4,8, NOTE_A4,16, NOTE_D4,-4,
  REST,8, NOTE_E5,8, REST,16, NOTE_B5,16, REST,8, NOTE_AS5,16, NOTE_B5,16, NOTE_AS5,16, NOTE_G5,16, REST,4,

  NOTE_B5,8, NOTE_B5,16, NOTE_AS5,16, REST,16, NOTE_AS5,16, NOTE_A5,16, REST,16, NOTE_B5,16, NOTE_G5,16, NOTE_B5,16, NOTE_AS5,16, REST,16, NOTE_B5,16, NOTE_A5,16, NOTE_G5,16,//11
  REST,8, NOTE_E5,8, REST,16, NOTE_B5,16, REST,8, NOTE_AS5,16, NOTE_B5,16, NOTE_AS5,16, NOTE_G5,16, REST,4,
  NOTE_B5,8, NOTE_B5,16, NOTE_AS5,16, REST,16, NOTE_AS5,16, NOTE_A5,16, REST,16, NOTE_B5,16, NOTE_G5,16, NOTE_B5,16, NOTE_AS5,16, REST,16, NOTE_B5,16, NOTE_A5,16, NOTE_G5,16,

  NOTE_DS4,-8, NOTE_FS4,-8, NOTE_C5,8, NOTE_B4,-8, NOTE_G4,-8, NOTE_E4,8, //14
  NOTE_DS4,-8, NOTE_FS4,-8, NOTE_C5,8, NOTE_B4,-8, NOTE_G4,-8, REST,8,
  NOTE_DS4,-8, NOTE_FS4,-8, NOTE_C5,8, NOTE_B4,-8, NOTE_G4,-8, NOTE_E4,8,
  NOTE_DS4,-8, NOTE_FS4,-8, NOTE_C5,8, NOTE_B4,-8, NOTE_CS5,-8, NOTE_DS5,8,

  NOTE_E5,16, NOTE_E5,16, NOTE_E4,16, NOTE_E4,-2,//18
  NOTE_C4,8, NOTE_C4,8, NOTE_E4,16, NOTE_G4,-8, NOTE_D4,8, NOTE_D4,8, NOTE_FS4,16, NOTE_A4,-8,
  NOTE_E5,16, NOTE_E5,16, NOTE_E4,16, NOTE_E4,-2,
  NOTE_C4,8, NOTE_C4,8, NOTE_E4,16, NOTE_G4,-8, NOTE_D4,8, NOTE_D4,8, NOTE_B3,16, NOTE_D4,-8,
  
};
int melody20[] = {  NOTE_FS4,2, NOTE_E4,2,
  NOTE_D4,2, NOTE_CS4,2,
  NOTE_B3,2, NOTE_A3,2,
  NOTE_B3,2, NOTE_CS4,2,
  NOTE_FS4,2, NOTE_E4,2,
  NOTE_D4,2, NOTE_CS4,2,
  NOTE_B3,2, NOTE_A3,2,
  NOTE_B3,2, NOTE_CS4,2,
  NOTE_D4,2, NOTE_CS4,2,
  NOTE_B3,2, NOTE_A3,2,
  NOTE_G3,2, NOTE_FS3,2,
  NOTE_G3,2, NOTE_A3,2,

  NOTE_D4,4, NOTE_FS4,8, NOTE_G4,8, NOTE_A4,4, NOTE_FS4,8, NOTE_G4,8, 
  NOTE_A4,4, NOTE_B3,8, NOTE_CS4,8, NOTE_D4,8, NOTE_E4,8, NOTE_FS4,8, NOTE_G4,8, 
  NOTE_FS4,4, NOTE_D4,8, NOTE_E4,8, NOTE_FS4,4, NOTE_FS3,8, NOTE_G3,8,
  NOTE_A3,8, NOTE_G3,8, NOTE_FS3,8, NOTE_G3,8, NOTE_A3,2,
  NOTE_G3,4, NOTE_B3,8, NOTE_A3,8, NOTE_G3,4, NOTE_FS3,8, NOTE_E3,8, 
  NOTE_FS3,4, NOTE_D3,8, NOTE_E3,8, NOTE_FS3,8, NOTE_G3,8, NOTE_A3,8, NOTE_B3,8,

  NOTE_G3,4, NOTE_B3,8, NOTE_A3,8, NOTE_B3,4, NOTE_CS4,8, NOTE_D4,8,
  NOTE_A3,8, NOTE_B3,8, NOTE_CS4,8, NOTE_D4,8, NOTE_E4,8, NOTE_FS4,8, NOTE_G4,8, NOTE_A4,2,
  NOTE_A4,4, NOTE_FS4,8, NOTE_G4,8, NOTE_A4,4,
  NOTE_FS4,8, NOTE_G4,8, NOTE_A4,8, NOTE_A3,8, NOTE_B3,8, NOTE_CS4,8,
  NOTE_D4,8, NOTE_E4,8, NOTE_FS4,8, NOTE_G4,8, NOTE_FS4,4, NOTE_D4,8, NOTE_E4,8,
  NOTE_FS4,8, NOTE_CS4,8, NOTE_A3,8, NOTE_A3,8,

  NOTE_CS4,4, NOTE_B3,4, NOTE_D4,8, NOTE_CS4,8, NOTE_B3,4,
  NOTE_A3,8, NOTE_G3,8, NOTE_A3,4, NOTE_D3,8, NOTE_E3,8, NOTE_FS3,8, NOTE_G3,8,
  NOTE_A3,8, NOTE_B3,4, NOTE_G3,4, NOTE_B3,8, NOTE_A3,8, NOTE_B3,4,
  NOTE_CS4,8, NOTE_D4,8, NOTE_A3,8, NOTE_B3,8, NOTE_CS4,8, NOTE_D4,8, NOTE_E4,8,
  NOTE_FS4,8, NOTE_G4,8, NOTE_A4,2,  
   
  
};
int melody21[] = { NOTE_G4,8,//1
  NOTE_AS4,4, NOTE_C5,8, NOTE_D5,-8, NOTE_DS5,16, NOTE_D5,8,
  NOTE_C5,4, NOTE_A4,8, NOTE_F4,-8, NOTE_G4,16, NOTE_A4,8,
  NOTE_AS4,4, NOTE_G4,8, NOTE_G4,-8, NOTE_FS4,16, NOTE_G4,8,
  NOTE_A4,4, NOTE_FS4,8, NOTE_D4,4, NOTE_G4,8,
  
  NOTE_AS4,4, NOTE_C5,8, NOTE_D5,-8, NOTE_DS5,16, NOTE_D5,8,//6
  NOTE_C5,4, NOTE_A4,8, NOTE_F4,-8, NOTE_G4,16, NOTE_A4,8,
  NOTE_AS4,-8, NOTE_A4,16, NOTE_G4,8, NOTE_FS4,-8, NOTE_E4,16, NOTE_FS4,8, 
  NOTE_G4,-2,
  NOTE_F5,2, NOTE_E5,16, NOTE_D5,8,

  NOTE_C5,4, NOTE_A4,8, NOTE_F4,-8, NOTE_G4,16, NOTE_A4,8,//11
  NOTE_AS4,4, NOTE_G4,8, NOTE_G4,-8, NOTE_FS4,16, NOTE_G4,8,
  NOTE_A4,4, NOTE_FS4,8, NOTE_D4,04,
  NOTE_F5,2, NOTE_E5,16, NOTE_D5,8,
  NOTE_C5,4, NOTE_A4,8, NOTE_F4,-8, NOTE_G4,16, NOTE_A4,8,

  NOTE_AS4,-8, NOTE_A4,16, NOTE_G4,8, NOTE_FS4,-8, NOTE_E4,16, NOTE_FS4,8,//16
  NOTE_G4,-2,

  //repeats from the beginning

  NOTE_G4,8,//1
  NOTE_AS4,4, NOTE_C5,8, NOTE_D5,-8, NOTE_DS5,16, NOTE_D5,8,
  NOTE_C5,4, NOTE_A4,8, NOTE_F4,-8, NOTE_G4,16, NOTE_A4,8,
  NOTE_AS4,4, NOTE_G4,8, NOTE_G4,-8, NOTE_FS4,16, NOTE_G4,8,
  NOTE_A4,4, NOTE_FS4,8, NOTE_D4,4, NOTE_G4,8,
  
  NOTE_AS4,4, NOTE_C5,8, NOTE_D5,-8, NOTE_DS5,16, NOTE_D5,8,//6
  NOTE_C5,4, NOTE_A4,8, NOTE_F4,-8, NOTE_G4,16, NOTE_A4,8,
  NOTE_AS4,-8, NOTE_A4,16, NOTE_G4,8, NOTE_FS4,-8, NOTE_E4,16, NOTE_FS4,8, 
  NOTE_G4,-2,
  NOTE_F5,2, NOTE_E5,16, NOTE_D5,8,

  NOTE_C5,4, NOTE_A4,8, NOTE_F4,-8, NOTE_G4,16, NOTE_A4,8,//11
  NOTE_AS4,4, NOTE_G4,8, NOTE_G4,-8, NOTE_FS4,16, NOTE_G4,8,
  NOTE_A4,4, NOTE_FS4,8, NOTE_D4,04,
  NOTE_F5,2, NOTE_E5,16, NOTE_D5,8,
  NOTE_C5,4, NOTE_A4,8, NOTE_F4,-8, NOTE_G4,16, NOTE_A4,8,

  NOTE_AS4,-8, NOTE_A4,16, NOTE_G4,8, NOTE_FS4,-8, NOTE_E4,16, NOTE_FS4,8,//16
  NOTE_G4,-2
  
  
};
int melody22[] = {  NOTE_E4,4,  NOTE_E4,4,  NOTE_F4,4,  NOTE_G4,4,//1
  NOTE_G4,4,  NOTE_F4,4,  NOTE_E4,4,  NOTE_D4,4,
  NOTE_C4,4,  NOTE_C4,4,  NOTE_D4,4,  NOTE_E4,4,
  NOTE_E4,-4, NOTE_D4,8,  NOTE_D4,2,

  NOTE_E4,4,  NOTE_E4,4,  NOTE_F4,4,  NOTE_G4,4,//4
  NOTE_G4,4,  NOTE_F4,4,  NOTE_E4,4,  NOTE_D4,4,
  NOTE_C4,4,  NOTE_C4,4,  NOTE_D4,4,  NOTE_E4,4,
  NOTE_D4,-4,  NOTE_C4,8,  NOTE_C4,2,

  NOTE_D4,4,  NOTE_D4,4,  NOTE_E4,4,  NOTE_C4,4,//8
  NOTE_D4,4,  NOTE_E4,8,  NOTE_F4,8,  NOTE_E4,4, NOTE_C4,4,
  NOTE_D4,4,  NOTE_E4,8,  NOTE_F4,8,  NOTE_E4,4, NOTE_D4,4,
  NOTE_C4,4,  NOTE_D4,4,  NOTE_G3,2,

  NOTE_E4,4,  NOTE_E4,4,  NOTE_F4,4,  NOTE_G4,4,//12
  NOTE_G4,4,  NOTE_F4,4,  NOTE_E4,4,  NOTE_D4,4,
  NOTE_C4,4,  NOTE_C4,4,  NOTE_D4,4,  NOTE_E4,4,
  NOTE_D4,-4,  NOTE_C4,8,  NOTE_C4,2
  
};
int melody23[] = {  NOTE_G4, 4, NOTE_G4, 4, NOTE_D5, -2,
  NOTE_C5, 8, NOTE_D5, 8, NOTE_AS4, 4, NOTE_A4, 8, NOTE_G4, 8,
  NOTE_A4, 8, NOTE_AS4, 8, NOTE_C5, 1,
  
  NOTE_D5, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_F4, 8,
  NOTE_D4, 4, NOTE_D4, 4, NOTE_G4, -2,
  NOTE_A4, 4, NOTE_G4, 4, NOTE_F4, 8, NOTE_E4, 8,
  
  NOTE_F4, 4, NOTE_E4, 4, NOTE_D4, 1,
  NOTE_E4, 4, NOTE_F4, 4, NOTE_A4, 4,
  NOTE_G4, 4, NOTE_G4, 4, NOTE_AS4, -2,

  NOTE_C5, 4, NOTE_AS4, 4, NOTE_A4, 8, NOTE_G4, 8,
  NOTE_A4, 4, NOTE_AS4, 4, NOTE_C5, -2,
  NOTE_CS5, 4, NOTE_C5, 4, NOTE_A4, 4,
  
  NOTE_CS5, 4, NOTE_CS4, 4, NOTE_F5, -2,
  NOTE_G5, 4, NOTE_F5, 4, NOTE_DS4, 8, NOTE_CS4, 8,
  NOTE_F5, 2, NOTE_C5, -2, 
  
  NOTE_AS4, 4, NOTE_C5, 4, NOTE_AS4, 8, NOTE_A4, 8,
  NOTE_G4, 4, NOTE_G4, 4, NOTE_AS4, 1,
  NOTE_C5, 4, NOTE_AS4, 4, NOTE_A4, 8, NOTE_G4, 8,  

  NOTE_F4, 4, NOTE_G4, 4, NOTE_A4, 1,
  NOTE_AS4, 4, NOTE_A4, 4, NOTE_F4, 4,
  NOTE_G4, 4, NOTE_G4, 4, NOTE_D5, -2,

  NOTE_C5, 4, NOTE_AS4, 4, NOTE_A4, 8, NOTE_G4, 8,
  NOTE_A4,-1, NOTE_A4,-1, REST,2,
  NOTE_G4, 4, NOTE_G4, 4, NOTE_D5, -2,
  
  NOTE_C5, 8, NOTE_D5, 8, NOTE_AS4, 4, NOTE_A4, 8, NOTE_G4, 8,
  NOTE_A4, 8, NOTE_AS4, 8, NOTE_C5, 1,
  NOTE_C5, 8, NOTE_D5, 8, NOTE_C5, 4, NOTE_AS4, 8, NOTE_A4, 8,
  
  
  NOTE_D4, 4, NOTE_D4, 4, NOTE_G4, -2,
  NOTE_G4, 8, NOTE_A4, 8, NOTE_G4, 4, NOTE_F4, 8, NOTE_E4, 8,
  NOTE_F4, 8, NOTE_E4, 8, NOTE_D4, -2,//1
  
  REST,4, NOTE_C5, 8, NOTE_D5, 8, NOTE_C5, 4, NOTE_AS4, 8, NOTE_A4, 8,
  NOTE_G4, 4, NOTE_G4, 4, NOTE_B4, -2,
  NOTE_C5, 4, NOTE_AS4, 4, NOTE_A4, 8, NOTE_G4, 8,  

  NOTE_A4, 4, NOTE_G4, 4, NOTE_F4, -1, REST,4,
  NOTE_G4, 4, NOTE_G4, 4, NOTE_D5, -2,
  NOTE_C5, 8, NOTE_D5, 8, NOTE_AS4, 4, NOTE_A4, 8, NOTE_G4, 8,
  
  NOTE_A4, 4, NOTE_G4, 4, NOTE_F4, -2,
  NOTE_A4, 4, NOTE_G4, 4, NOTE_F4, -2,
  NOTE_A4, 4, NOTE_G4, 4, NOTE_F4, -1,

};
int melody24[] = {  NOTE_D5,4, NOTE_G4,8, NOTE_A4,8, NOTE_B4,8, NOTE_C5,8, //1
  NOTE_D5,4, NOTE_G4,4, NOTE_G4,4,
  NOTE_E5,4, NOTE_C5,8, NOTE_D5,8, NOTE_E5,8, NOTE_FS5,8,
  NOTE_G5,4, NOTE_G4,4, NOTE_G4,4,
  NOTE_C5,4, NOTE_D5,8, NOTE_C5,8, NOTE_B4,8, NOTE_A4,8,
  
  NOTE_B4,4, NOTE_C5,8, NOTE_B4,8, NOTE_A4,8, NOTE_G4,8,//6
  NOTE_FS4,4, NOTE_G4,8, NOTE_A4,8, NOTE_B4,8, NOTE_G4,8,
  NOTE_A4,-2,
  NOTE_D5,4, NOTE_G4,8, NOTE_A4,8, NOTE_B4,8, NOTE_C5,8, 
  NOTE_D5,4, NOTE_G4,4, NOTE_G4,4,
  NOTE_E5,4, NOTE_C5,8, NOTE_D5,8, NOTE_E5,8, NOTE_FS5,8,
  
  NOTE_G5,4, NOTE_G4,4, NOTE_G4,4,
  NOTE_C5,4, NOTE_D5,8, NOTE_C5,8, NOTE_B4,8, NOTE_A4,8, //12
  NOTE_B4,4, NOTE_C5,8, NOTE_B4,8, NOTE_A4,8, NOTE_G4,8,
  NOTE_A4,4, NOTE_B4,8, NOTE_A4,8, NOTE_G4,8, NOTE_FS4,8,
  NOTE_G4,-2,

  //repeats from 1

  NOTE_D5,4, NOTE_G4,8, NOTE_A4,8, NOTE_B4,8, NOTE_C5,8, //1
  NOTE_D5,4, NOTE_G4,4, NOTE_G4,4,
  NOTE_E5,4, NOTE_C5,8, NOTE_D5,8, NOTE_E5,8, NOTE_FS5,8,
  NOTE_G5,4, NOTE_G4,4, NOTE_G4,4,
  NOTE_C5,4, NOTE_D5,8, NOTE_C5,8, NOTE_B4,8, NOTE_A4,8,
  
  NOTE_B4,4, NOTE_C5,8, NOTE_B4,8, NOTE_A4,8, NOTE_G4,8,//6
  NOTE_FS4,4, NOTE_G4,8, NOTE_A4,8, NOTE_B4,8, NOTE_G4,8,
  NOTE_A4,-2,
  NOTE_D5,4, NOTE_G4,8, NOTE_A4,8, NOTE_B4,8, NOTE_C5,8, 
  NOTE_D5,4, NOTE_G4,4, NOTE_G4,4,
  NOTE_E5,4, NOTE_C5,8, NOTE_D5,8, NOTE_E5,8, NOTE_FS5,8,
  
  NOTE_G5,4, NOTE_G4,4, NOTE_G4,4,
  NOTE_C5,4, NOTE_D5,8, NOTE_C5,8, NOTE_B4,8, NOTE_A4,8, //12
  NOTE_B4,4, NOTE_C5,8, NOTE_B4,8, NOTE_A4,8, NOTE_G4,8,
  NOTE_A4,4, NOTE_B4,8, NOTE_A4,8, NOTE_G4,8, NOTE_FS4,8,
  NOTE_G4,-2,

  //continues from 17

  NOTE_B5,4, NOTE_G5,8, NOTE_A5,8, NOTE_B5,8, NOTE_G5,8,//17
  NOTE_A5,4, NOTE_D5,8, NOTE_E5,8, NOTE_FS5,8, NOTE_D5,8,
  NOTE_G5,4, NOTE_E5,8, NOTE_FS5,8, NOTE_G5,8, NOTE_D5,8,
  NOTE_CS5,4, NOTE_B4,8, NOTE_CS5,8, NOTE_A4,4,
  NOTE_A4,8, NOTE_B4,8, NOTE_CS5,8, NOTE_D5,8, NOTE_E5,8, NOTE_FS5,8,

  NOTE_G5,4, NOTE_FS5,4, NOTE_E5,4, //22
  NOTE_FS5,4, NOTE_A4,4, NOTE_CS5,4,
  NOTE_D5,-2,
  NOTE_D5,4, NOTE_G4,8, NOTE_FS5,8, NOTE_G4,4,
  NOTE_E5,4,  NOTE_G4,8, NOTE_FS4,8, NOTE_G4,4,
  NOTE_D5,4, NOTE_C5,4, NOTE_B4,4,

  NOTE_A4,8, NOTE_G4,8, NOTE_FS4,8, NOTE_G4,8, NOTE_A4,4, //28
  NOTE_D4,8, NOTE_E4,8, NOTE_FS4,8, NOTE_G4,8, NOTE_A4,8, NOTE_B4,8,
  NOTE_C5,4, NOTE_B4,4, NOTE_A4,4,
  NOTE_B4,8, NOTE_D5,8, NOTE_G4,4, NOTE_FS4,4,
  NOTE_G4,-2,
  };
int melody25[] = {  NOTE_G4, 4, NOTE_G4, 4, //1
  NOTE_AS4, -4, NOTE_G4, 8, NOTE_G4, 4,
  NOTE_AS4, 4, REST, 4, NOTE_G4, 8, NOTE_AS4, 8,
  NOTE_DS5, 4, NOTE_D5, -4, NOTE_C5, 8,
  NOTE_C5, 4, NOTE_AS4, 4, NOTE_F4, 8, NOTE_G4, 8,
  NOTE_GS4, 4, NOTE_F4, 4, NOTE_F4, 8, NOTE_G4, 8,
  NOTE_GS4, 4, REST, 4, NOTE_F4, 8, NOTE_GS4, 8,
  NOTE_D5, 8, NOTE_C5, 8, NOTE_AS4, 4, NOTE_D5, 4,

  NOTE_DS5, 4, REST, 4, NOTE_DS4, 8, NOTE_DS4, 8, //8
  NOTE_DS5, 2, NOTE_C5, 8, NOTE_GS4, 8,
  NOTE_AS4, 2, NOTE_G4, 8, NOTE_DS4, 8,
  NOTE_GS4, 4, NOTE_AS4, 4, NOTE_C5, 4,
  NOTE_AS4, 2, NOTE_DS4, 8, NOTE_DS4, 8,
  NOTE_DS5, 2, NOTE_C5, 8, NOTE_GS4, 8,
  NOTE_AS4, 2, NOTE_G4, 8, NOTE_DS4, 8,
  NOTE_AS4, 4, NOTE_G4, 4, NOTE_DS4, 4,
  NOTE_DS4, 2

};
int melody26[] = {  NOTE_E5, 16, NOTE_DS5, 16, //1
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_A4, -8, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16,
  NOTE_B4, -8, NOTE_E4, 16, NOTE_GS4, 16, NOTE_B4, 16,
  NOTE_C5, 8,  REST, 16, NOTE_E4, 16, NOTE_E5, 16,  NOTE_DS5, 16,
  
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,//6
  NOTE_A4, -8, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16, 
  NOTE_B4, -8, NOTE_E4, 16, NOTE_C5, 16, NOTE_B4, 16, 
  NOTE_A4 , 4, REST, 8, //9 - 1st ending

  //repaets from 1 ending on 10
  NOTE_E5, 16, NOTE_DS5, 16, //1
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_A4, -8, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16,
  NOTE_B4, -8, NOTE_E4, 16, NOTE_GS4, 16, NOTE_B4, 16,
  NOTE_C5, 8,  REST, 16, NOTE_E4, 16, NOTE_E5, 16,  NOTE_DS5, 16,
  
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,//6
  NOTE_A4, -8, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16, 
  NOTE_B4, -8, NOTE_E4, 16, NOTE_C5, 16, NOTE_B4, 16, 
  NOTE_A4, 8, REST, 16, NOTE_B4, 16, NOTE_C5, 16, NOTE_D5, 16, //10 - 2nd ending
  //continues from 11
  NOTE_E5, -8, NOTE_G4, 16, NOTE_F5, 16, NOTE_E5, 16, 
  NOTE_D5, -8, NOTE_F4, 16, NOTE_E5, 16, NOTE_D5, 16, //12
  
  NOTE_C5, -8, NOTE_E4, 16, NOTE_D5, 16, NOTE_C5, 16, //13
  NOTE_B4, 8, REST, 16, NOTE_E4, 16, NOTE_E5, 16, REST, 16,
  REST, 16, NOTE_E5, 16, NOTE_E6, 16, REST, 16, REST, 16, NOTE_DS5, 16,
  NOTE_E5, 16, REST, 16, REST, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_DS5, 16,
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_A4, 8, REST, 16, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16,
  
  NOTE_B4, 8, REST, 16, NOTE_E4, 16, NOTE_GS4, 16, NOTE_B4, 16, //19
  NOTE_C5, 8, REST, 16, NOTE_E4, 16, NOTE_E5, 16,  NOTE_DS5, 16,
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_A4, 8, REST, 16, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16,
  NOTE_B4, 8, REST, 16, NOTE_E4, 16, NOTE_C5, 16, NOTE_B4, 16,
  NOTE_A4, 8, REST, 16, NOTE_B4, 16, NOTE_C5, 16, NOTE_D5, 16, //24 (1st ending)
  
  //repeats from 11
  NOTE_E5, -8, NOTE_G4, 16, NOTE_F5, 16, NOTE_E5, 16, 
  NOTE_D5, -8, NOTE_F4, 16, NOTE_E5, 16, NOTE_D5, 16, //12
  
  NOTE_C5, -8, NOTE_E4, 16, NOTE_D5, 16, NOTE_C5, 16, //13
  NOTE_B4, 8, REST, 16, NOTE_E4, 16, NOTE_E5, 16, REST, 16,
  REST, 16, NOTE_E5, 16, NOTE_E6, 16, REST, 16, REST, 16, NOTE_DS5, 16,
  NOTE_E5, 16, REST, 16, REST, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_DS5, 16,
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_A4, 8, REST, 16, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16,
  
  NOTE_B4, 8, REST, 16, NOTE_E4, 16, NOTE_GS4, 16, NOTE_B4, 16, //19
  NOTE_C5, 8, REST, 16, NOTE_E4, 16, NOTE_E5, 16,  NOTE_DS5, 16,
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_A4, 8, REST, 16, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16,
  NOTE_B4, 8, REST, 16, NOTE_E4, 16, NOTE_C5, 16, NOTE_B4, 16,
  NOTE_A4, 8, REST, 16, NOTE_C5, 16, NOTE_C5, 16, NOTE_C5, 16, //25 - 2nd ending

  //continues from 26
  NOTE_C5 , 4, NOTE_F5, -16, NOTE_E5, 32, //26
  NOTE_E5, 8, NOTE_D5, 8, NOTE_AS5, -16, NOTE_A5, 32,
  NOTE_A5, 16, NOTE_G5, 16, NOTE_F5, 16, NOTE_E5, 16, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_AS4, 8, NOTE_A4, 8, NOTE_A4, 32, NOTE_G4, 32, NOTE_A4, 32, NOTE_B4, 32,
  NOTE_C5 , 4, NOTE_D5, 16, NOTE_DS5, 16,
  NOTE_E5, -8, NOTE_E5, 16, NOTE_F5, 16, NOTE_A4, 16,
  NOTE_C5 , 4,  NOTE_D5, -16, NOTE_B4, 32,
  
  
  NOTE_C5, 32, NOTE_G5, 32, NOTE_G4, 32, NOTE_G5, 32, NOTE_A4, 32, NOTE_G5, 32, NOTE_B4, 32, NOTE_G5, 32, NOTE_C5, 32, NOTE_G5, 32, NOTE_D5, 32, NOTE_G5, 32, //33
  NOTE_E5, 32, NOTE_G5, 32, NOTE_C6, 32, NOTE_B5, 32, NOTE_A5, 32, NOTE_G5, 32, NOTE_F5, 32, NOTE_E5, 32, NOTE_D5, 32, NOTE_G5, 32, NOTE_F5, 32, NOTE_D5, 32,
  NOTE_C5, 32, NOTE_G5, 32, NOTE_G4, 32, NOTE_G5, 32, NOTE_A4, 32, NOTE_G5, 32, NOTE_B4, 32, NOTE_G5, 32, NOTE_C5, 32, NOTE_G5, 32, NOTE_D5, 32, NOTE_G5, 32,

  NOTE_E5, 32, NOTE_G5, 32, NOTE_C6, 32, NOTE_B5, 32, NOTE_A5, 32, NOTE_G5, 32, NOTE_F5, 32, NOTE_E5, 32, NOTE_D5, 32, NOTE_G5, 32, NOTE_F5, 32, NOTE_D5, 32, //36
  NOTE_E5, 32, NOTE_F5, 32, NOTE_E5, 32, NOTE_DS5, 32, NOTE_E5, 32, NOTE_B4, 32, NOTE_E5, 32, NOTE_DS5, 32, NOTE_E5, 32, NOTE_B4, 32, NOTE_E5, 32, NOTE_DS5, 32,
  NOTE_E5, -8, NOTE_B4, 16, NOTE_E5, 16, NOTE_DS5, 16,
  NOTE_E5, -8, NOTE_B4, 16, NOTE_E5, 16, REST, 16,

  REST, 16, NOTE_DS5, 16, NOTE_E5, 16, REST, 16, REST, 16, NOTE_DS5, 16, //40
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_A4, 8, REST, 16, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16,
  NOTE_B4, 8, REST, 16, NOTE_E4, 16, NOTE_GS4, 16, NOTE_B4, 16,
  NOTE_C5, 8, REST, 16, NOTE_E4, 16, NOTE_E5, 16, NOTE_DS5, 16,
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,

  NOTE_A4, 8, REST, 16, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16, //46
  NOTE_B4, 8, REST, 16, NOTE_E4, 16, NOTE_C5, 16, NOTE_B4, 16,
  NOTE_A4, 8, REST, 16, NOTE_B4, 16, NOTE_C5, 16, NOTE_D5, 16,
  NOTE_E5, -8, NOTE_G4, 16, NOTE_F5, 16, NOTE_E5, 16,
  NOTE_D5, -8, NOTE_F4, 16, NOTE_E5, 16, NOTE_D5, 16,
  NOTE_C5, -8, NOTE_E4, 16, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_B4, 8, REST, 16, NOTE_E4, 16, NOTE_E5, 16, REST, 16,
  REST, 16, NOTE_E5, 16, NOTE_E6, 16, REST, 16, REST, 16, NOTE_DS5, 16,

  NOTE_E5, 16, REST, 16, REST, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_D5, 16, //54
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_A4, 8, REST, 16, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16,
  NOTE_B4, 8, REST, 16, NOTE_E4, 16, NOTE_GS4, 16, NOTE_B4, 16,
  NOTE_C5, 8, REST, 16, NOTE_E4, 16, NOTE_E5, 16, NOTE_DS5, 16,
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,
  
  NOTE_A4, 8, REST, 16, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16, //60
  NOTE_B4, 8, REST, 16, NOTE_E4, 16, NOTE_C5, 16, NOTE_B4, 16,
  NOTE_A4, 8, REST, 16, REST, 16, REST, 8, 
  NOTE_CS5 , -4, 
  NOTE_D5 , 4, NOTE_E5, 16, NOTE_F5, 16,
  NOTE_F5 , 4, NOTE_F5, 8, 
  NOTE_E5 , -4,
  NOTE_D5 , 4, NOTE_C5, 16, NOTE_B4, 16,
  NOTE_A4 , 4, NOTE_A4, 8,
  NOTE_A4, 8, NOTE_C5, 8, NOTE_B4, 8,
  NOTE_A4 , -4,
  NOTE_CS5 , -4,

  NOTE_D5 , 4, NOTE_E5, 16, NOTE_F5, 16, //72
  NOTE_F5 , 4, NOTE_F5, 8,
  NOTE_F5 , -4,
  NOTE_DS5 , 4, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_AS4 , 4, NOTE_A4, 8,
  NOTE_GS4 , 4, NOTE_G4, 8,
  NOTE_A4 , -4,
  NOTE_B4 , 4, REST, 8,
  NOTE_A3, -32, NOTE_C4, -32, NOTE_E4, -32, NOTE_A4, -32, NOTE_C5, -32, NOTE_E5, -32, NOTE_D5, -32, NOTE_C5, -32, NOTE_B4, -32,

  NOTE_A4, -32, NOTE_C5, -32, NOTE_E5, -32, NOTE_A5, -32, NOTE_C6, -32, NOTE_E6, -32, NOTE_D6, -32, NOTE_C6, -32, NOTE_B5, -32, //80
  NOTE_A4, -32, NOTE_C5, -32, NOTE_E5, -32, NOTE_A5, -32, NOTE_C6, -32, NOTE_E6, -32, NOTE_D6, -32, NOTE_C6, -32, NOTE_B5, -32,
  NOTE_AS5, -32, NOTE_A5, -32, NOTE_GS5, -32, NOTE_G5, -32, NOTE_FS5, -32, NOTE_F5, -32, NOTE_E5, -32, NOTE_DS5, -32, NOTE_D5, -32,

  NOTE_CS5, -32, NOTE_C5, -32, NOTE_B4, -32, NOTE_AS4, -32, NOTE_A4, -32, NOTE_GS4, -32, NOTE_G4, -32, NOTE_FS4, -32, NOTE_F4, -32, //84
  NOTE_E4, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_A4, -8, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16,
  NOTE_B4, -8, NOTE_E4, 16, NOTE_GS4, 16, NOTE_B4, 16,

  NOTE_C5, 8, REST, 16, NOTE_E4, 16, NOTE_E5, 16, NOTE_DS5, 16, //88
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16, 
  NOTE_A4, -8, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16, 
  NOTE_B4, -8, NOTE_E4, 16, NOTE_C5, 16, NOTE_B4, 16, 
  NOTE_A4, -8, REST, -8,
  REST, -8, NOTE_G4, 16, NOTE_F5, 16, NOTE_E5, 16,
  NOTE_D5 , 4, REST, 8,
  REST, -8, NOTE_E4, 16, NOTE_D5, 16, NOTE_C5, 16,
  
  NOTE_B4, -8, NOTE_E4, 16, NOTE_E5, 8, //96
  NOTE_E5, 8, NOTE_E6, -8, NOTE_DS5, 16,
  NOTE_E5, 16, REST, 16, REST, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_DS5, 16,
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_A4, -8, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16,
  NOTE_B4, -8, NOTE_E4, 16, NOTE_GS4, 16, NOTE_B4, 16,

  NOTE_C5, 8, REST, 16, NOTE_E4, 16, NOTE_E5, 16, NOTE_DS5, 16, //102
  NOTE_E5, 16, NOTE_DS5, 16, NOTE_E5, 16, NOTE_B4, 16, NOTE_D5, 16, NOTE_C5, 16,
  NOTE_A4, -8, NOTE_C4, 16, NOTE_E4, 16, NOTE_A4, 16,
  NOTE_B4, -8, NOTE_E4, 16, NOTE_C5, 16, NOTE_B4, 16,
  NOTE_A4 , -4,
};
int melody27[] = {  NOTE_G4,8, NOTE_A4,8, NOTE_B4,4, NOTE_D5,4, NOTE_D5,4, NOTE_B4,4, 
  NOTE_C5,4, NOTE_C5,2, NOTE_G4,8, NOTE_A4,8,
  NOTE_B4,4, NOTE_D5,4, NOTE_D5,4, NOTE_C5,4,

  NOTE_B4,2, REST,8, NOTE_G4,8, NOTE_G4,8, NOTE_A4,8,
  NOTE_B4,4, NOTE_D5,4, REST,8, NOTE_D5,8, NOTE_C5,8, NOTE_B4,8,
  NOTE_G4,4, NOTE_C5,4, REST,8, NOTE_C5,8, NOTE_B4,8, NOTE_A4,8,

  NOTE_A4,4, NOTE_B4,4, REST,8, NOTE_B4,8, NOTE_A4,8, NOTE_G4,8,
  NOTE_G4,2, REST,8, NOTE_G4,8, NOTE_G4,8, NOTE_A4,8,
  NOTE_B4,4, NOTE_D5,4, REST,8, NOTE_D5,8, NOTE_C5,8, NOTE_B4,8,

  NOTE_G4,4, NOTE_C5,4, REST,8, NOTE_C5,8, NOTE_B4,8, NOTE_A4,8,
  NOTE_A4,4, NOTE_B4,4, REST,8, NOTE_B4,8, NOTE_A4,8, NOTE_G4,8,
  NOTE_G4,4, NOTE_F5,8, NOTE_D5,8, NOTE_E5,8, NOTE_C5,8, NOTE_D5,8, NOTE_B4,8,

  NOTE_C5,8, NOTE_A4,8, NOTE_B4,8, NOTE_G4,8, NOTE_A4,8, NOTE_G4,8, NOTE_E4,8, NOTE_G4,8,
  NOTE_G4,4, NOTE_F5,8, NOTE_D5,8, NOTE_E5,8, NOTE_C5,8, NOTE_D5,8, NOTE_B4,8,
  NOTE_C5,8, NOTE_A4,8, NOTE_B4,8, NOTE_G4,8, NOTE_A4,8, NOTE_G4,8, NOTE_E4,8, NOTE_G4,8,
  NOTE_G4,-2, REST,4
  
};
int melody28[] = {  REST,2, REST,4, REST,8, NOTE_DS4,8, 
  NOTE_E4,-4, REST,8, NOTE_FS4,8, NOTE_G4,-4, REST,8, NOTE_DS4,8,
  NOTE_E4,-8, NOTE_FS4,8,  NOTE_G4,-8, NOTE_C5,8, NOTE_B4,-8, NOTE_E4,8, NOTE_G4,-8, NOTE_B4,8,   
  NOTE_AS4,2, NOTE_A4,-16, NOTE_G4,-16, NOTE_E4,-16, NOTE_D4,-16, 
  NOTE_E4,2, REST,4, REST,8, NOTE_DS4,4,

  NOTE_E4,-4, REST,8, NOTE_FS4,8, NOTE_G4,-4, REST,8, NOTE_DS4,8,
  NOTE_E4,-8, NOTE_FS4,8,  NOTE_G4,-8, NOTE_C5,8, NOTE_B4,-8, NOTE_G4,8, NOTE_B4,-8, NOTE_E5,8,
  NOTE_DS5,1,   
  NOTE_D5,2, REST,4, REST,8, NOTE_DS4,8, 
  NOTE_E4,-4, REST,8, NOTE_FS4,8, NOTE_G4,-4, REST,8, NOTE_DS4,8,
  NOTE_E4,-8, NOTE_FS4,8,  NOTE_G4,-8, NOTE_C5,8, NOTE_B4,-8, NOTE_E4,8, NOTE_G4,-8, NOTE_B4,8,   
  
  NOTE_AS4,2, NOTE_A4,-16, NOTE_G4,-16, NOTE_E4,-16, NOTE_D4,-16, 
  NOTE_E4,-4, REST,4,
  REST,4, NOTE_E5,-8, NOTE_D5,8, NOTE_B4,-8, NOTE_A4,8, NOTE_G4,-8, NOTE_E4,-8,
  NOTE_AS4,16, NOTE_A4,-8, NOTE_AS4,16, NOTE_A4,-8, NOTE_AS4,16, NOTE_A4,-8, NOTE_AS4,16, NOTE_A4,-8,   
  NOTE_G4,-16, NOTE_E4,-16, NOTE_D4,-16, NOTE_E4,16, NOTE_E4,16, NOTE_E4,2,
 
};
int melody29[] = {  NOTE_FS5,8, NOTE_FS5,8,NOTE_D5,8, NOTE_B4,8, REST,8, NOTE_B4,8, REST,8, NOTE_E5,8, 
  REST,8, NOTE_E5,8, REST,8, NOTE_E5,8, NOTE_GS5,8, NOTE_GS5,8, NOTE_A5,8, NOTE_B5,8,
  NOTE_A5,8, NOTE_A5,8, NOTE_A5,8, NOTE_E5,8, REST,8, NOTE_D5,8, REST,8, NOTE_FS5,8, 
  REST,8, NOTE_FS5,8, REST,8, NOTE_FS5,8, NOTE_E5,8, NOTE_E5,8, NOTE_FS5,8, NOTE_E5,8,
  NOTE_FS5,8, NOTE_FS5,8,NOTE_D5,8, NOTE_B4,8, REST,8, NOTE_B4,8, REST,8, NOTE_E5,8, 
  
  REST,8, NOTE_E5,8, REST,8, NOTE_E5,8, NOTE_GS5,8, NOTE_GS5,8, NOTE_A5,8, NOTE_B5,8,
  NOTE_A5,8, NOTE_A5,8, NOTE_A5,8, NOTE_E5,8, REST,8, NOTE_D5,8, REST,8, NOTE_FS5,8, 
  REST,8, NOTE_FS5,8, REST,8, NOTE_FS5,8, NOTE_E5,8, NOTE_E5,8, NOTE_FS5,8, NOTE_E5,8,
  NOTE_FS5,8, NOTE_FS5,8,NOTE_D5,8, NOTE_B4,8, REST,8, NOTE_B4,8, REST,8, NOTE_E5,8, 
  REST,8, NOTE_E5,8, REST,8, NOTE_E5,8, NOTE_GS5,8, NOTE_GS5,8, NOTE_A5,8, NOTE_B5,8,
  
  NOTE_A5,8, NOTE_A5,8, NOTE_A5,8, NOTE_E5,8, REST,8, NOTE_D5,8, REST,8, NOTE_FS5,8, 
  REST,8, NOTE_FS5,8, REST,8, NOTE_FS5,8, NOTE_E5,8, NOTE_E5,8, NOTE_FS5,8, NOTE_E5,8,
  
};
int melody30[] = { NOTE_D4,8, NOTE_E4,8, NOTE_F4,8, NOTE_G4,8, NOTE_E4,4, NOTE_C4,8, NOTE_D4,1, 
};
int melody31[] = {  NOTE_F4, 4, NOTE_G4, 4, NOTE_A4, 8, NOTE_G4, 4, NOTE_A4, 8, //1
  NOTE_AS4, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_F4, 4, NOTE_G4, 8,
  NOTE_A4, 4, NOTE_C4, 8, NOTE_C4, 4, NOTE_C4, 8, NOTE_C4, 4,
  NOTE_C4, 1, //1st ending

  NOTE_F4, 4, NOTE_G4, 4, NOTE_A4, 8, NOTE_G4, 4, NOTE_A4, 8, //repeats from 1
  NOTE_AS4, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_F4, 4, NOTE_G4, 8,
  NOTE_A4, 4, NOTE_C4, 8, NOTE_C4, 4, NOTE_C4, 8, NOTE_C4, 4,
  NOTE_C4, -2,  REST, -8, NOTE_A4, 16, //2nd ending

  NOTE_A4, -8, NOTE_A4, 16, NOTE_A4, -8, NOTE_A4, 16, NOTE_A4, -8, NOTE_A4, 16, NOTE_A4, -8, NOTE_A4, 16, //6
  NOTE_AS4, -8, NOTE_AS4, 16, NOTE_AS4, -8, NOTE_AS4, 16, NOTE_AS4, -8, NOTE_AS4, 16, NOTE_AS4, -8, NOTE_AS4, 16,
  NOTE_A4, -8, NOTE_A4, 16, NOTE_A4, -8, NOTE_A4, 16, NOTE_A4, -8, NOTE_A4, 16, NOTE_A4, -8, NOTE_A4, 16,
  NOTE_G4, -8, NOTE_G4, 16, NOTE_G4, -8, NOTE_G4, 16, NOTE_G4, -8, NOTE_G4, 16, NOTE_G4, -8, NOTE_G4, 16,

  NOTE_A4, -8, NOTE_A4, 16, NOTE_A4, -8, NOTE_A4, 16, NOTE_A4, -8, NOTE_A4, 16, NOTE_A4, -8, NOTE_A4, 16, //10
  NOTE_AS4, -8, NOTE_AS4, 16, NOTE_AS4, -8, NOTE_AS4, 16, NOTE_AS4, -8, NOTE_AS4, 16, NOTE_AS4, -8, NOTE_AS4, 16,
  NOTE_A4, -8, NOTE_A4, 16, NOTE_A4, -8, NOTE_A4, 16, NOTE_A4, -8, NOTE_A4, 16, NOTE_A4, -8, NOTE_A4, 16,
  NOTE_G4, -8, NOTE_G4, 16, NOTE_G4, -8, NOTE_G4, 16, NOTE_G4, -8, NOTE_G4, 16, NOTE_G4, -8, NOTE_G4, 16,

  NOTE_F4, 4, NOTE_G4, 4, NOTE_A4, 8, NOTE_G4, 4, NOTE_A4, 8, //14
  NOTE_AS4, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_F4, 4, NOTE_G4, 8,
  NOTE_A4, 4, NOTE_G4, 4, NOTE_F4, 4, NOTE_A4, 4,
  NOTE_G4, 1,
  NOTE_C5, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_A4, 4, NOTE_C5, 8,
  NOTE_AS4, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_F4, 4, NOTE_G4, 8,
  NOTE_A4, 4, NOTE_G4, 4, NOTE_F4, 4, NOTE_A4, 4,
  NOTE_G4, 1,

  NOTE_C5, 1, //22
  NOTE_C5, 4, NOTE_AS4, 8, NOTE_C5, 8, NOTE_AS4, 2,
  NOTE_A4, 4, NOTE_C4, 8, NOTE_C4, 4, NOTE_C4, 8, NOTE_C4, 4,
  NOTE_C4, 1,

  REST, 4, NOTE_A4, 8, NOTE_G4, 8, NOTE_F4, 8, NOTE_E4, 8, NOTE_D4, 8, NOTE_C4, 8, 
  NOTE_D4, 1,
  REST, 4, NOTE_A4, 8, NOTE_G4, 8, NOTE_F4, 8, NOTE_E4, 8, NOTE_D4, 8, NOTE_C4, 8, 
  NOTE_D4, 1,

  NOTE_F4, 4, NOTE_G4, 4, NOTE_A4, 8, NOTE_G4, 4, NOTE_A4, 8, //repeats from 14
  NOTE_AS4, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_F4, 4, NOTE_G4, 8,
  NOTE_A4, 4, NOTE_G4, 4, NOTE_F4, 4, NOTE_A4, 4,
  NOTE_G4, 1,
  NOTE_C5, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_A4, 4, NOTE_C5, 8,
  NOTE_AS4, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_F4, 4, NOTE_G4, 8,
  NOTE_A4, 4, NOTE_G4, 4, NOTE_F4, 4, NOTE_A4, 4,
  NOTE_G4, 1,

  NOTE_C5, 1, //22
  NOTE_C5, 4, NOTE_AS4, 8, NOTE_C5, 8, NOTE_AS4, 2,
  NOTE_A4, 4, NOTE_C4, 8, NOTE_C4, 4, NOTE_C4, 8, NOTE_C4, 4,
  NOTE_C4, 1,

  REST, 4, NOTE_A4, 8, NOTE_G4, 8, NOTE_F4, 8, NOTE_E4, 8, NOTE_D4, 8, NOTE_C4, 8, 
  NOTE_D4, 1,
  REST, 4, NOTE_A4, 8, NOTE_G4, 8, NOTE_F4, 8, NOTE_E4, 8, NOTE_D4, 8, NOTE_C4, 8, 
  NOTE_D4, 1,

  NOTE_F4, 4, NOTE_G4, 4, NOTE_A4, 8, NOTE_G4, 4, NOTE_A4, 8, //30
  NOTE_AS4, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_F4, 4, NOTE_G4, 8,
  NOTE_A4, 4, NOTE_C4, 8, NOTE_C4, 4, NOTE_C4, 8, NOTE_C4, 4,
  NOTE_C4, 1, 

  NOTE_F4, 4, NOTE_G4, 4, NOTE_A4, 8, NOTE_G4, 4, NOTE_A4, 8, //repeats from 14 (again)
  NOTE_AS4, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_F4, 4, NOTE_G4, 8,
  NOTE_A4, 4, NOTE_G4, 4, NOTE_F4, 4, NOTE_A4, 4,
  NOTE_G4, 1,
  NOTE_C5, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_A4, 4, NOTE_C5, 8,
  NOTE_AS4, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_F4, 4, NOTE_G4, 8,
  NOTE_A4, 4, NOTE_G4, 4, NOTE_F4, 4, NOTE_A4, 4,
  NOTE_G4, 1,

  NOTE_C5, 1, //22
  NOTE_C5, 4, NOTE_AS4, 8, NOTE_C5, 8, NOTE_AS4, 2,
  NOTE_A4, 4, NOTE_C4, 8, NOTE_C4, 4, NOTE_C4, 8, NOTE_C4, 4,
  NOTE_C4, 1,

  REST, 4, NOTE_A4, 8, NOTE_G4, 8, NOTE_F4, 8, NOTE_E4, 8, NOTE_D4, 8, NOTE_C4, 8, 
  NOTE_D4, 1,
  REST, 4, NOTE_A4, 8, NOTE_G4, 8, NOTE_F4, 8, NOTE_E4, 8, NOTE_D4, 8, NOTE_C4, 8, 
  NOTE_D4, 1,

  NOTE_F4, 4, NOTE_G4, 4, NOTE_A4, 8, NOTE_G4, 4, NOTE_A4, 8, //30
  NOTE_AS4, 4, NOTE_A4, 4, NOTE_G4, 8, NOTE_F4, 4, NOTE_G4, 8,
  NOTE_A4, 4, NOTE_C4, 8, NOTE_C4, 4, NOTE_C4, 8, NOTE_C4, 4,
  NOTE_C4, 1, 
  
};
int melody32[] = {    REST,1,
    REST,1,
    NOTE_C4,4, NOTE_E4,4, NOTE_G4,4, NOTE_E4,4, 
    NOTE_C4,4, NOTE_E4,8, NOTE_G4,-4, NOTE_E4,4,
    NOTE_A3,4, NOTE_C4,4, NOTE_E4,4, NOTE_C4,4,
    NOTE_A3,4, NOTE_C4,8, NOTE_E4,-4, NOTE_C4,4,
    NOTE_G3,4, NOTE_B3,4, NOTE_D4,4, NOTE_B3,4,
    NOTE_G3,4, NOTE_B3,8, NOTE_D4,-4, NOTE_B3,4,

    NOTE_G3,4, NOTE_G3,8, NOTE_G3,-4, NOTE_G3,8, NOTE_G3,4, 
    NOTE_G3,4, NOTE_G3,4, NOTE_G3,8, NOTE_G3,4,
    NOTE_C4,4, NOTE_E4,4, NOTE_G4,4, NOTE_E4,4, 
    NOTE_C4,4, NOTE_E4,8, NOTE_G4,-4, NOTE_E4,4,
    NOTE_A3,4, NOTE_C4,4, NOTE_E4,4, NOTE_C4,4,
    NOTE_A3,4, NOTE_C4,8, NOTE_E4,-4, NOTE_C4,4,
    NOTE_G3,4, NOTE_B3,4, NOTE_D4,4, NOTE_B3,4,
    NOTE_G3,4, NOTE_B3,8, NOTE_D4,-4, NOTE_B3,4,

    NOTE_G3,-1, 
  
};
int melody33[] = {  NOTE_C4,-8, NOTE_E4,16, NOTE_G4,8, NOTE_C5,8, NOTE_E5,8, NOTE_D5,8, NOTE_C5,8, NOTE_A4,8,
  NOTE_FS4,8, NOTE_G4,8, REST,4, REST,2,
  NOTE_C4,-8, NOTE_E4,16, NOTE_G4,8, NOTE_C5,8, NOTE_E5,8, NOTE_D5,8, NOTE_C5,8, NOTE_A4,8,
  NOTE_G4,-2, NOTE_A4,8, NOTE_DS4,1,
  
  NOTE_A4,8,
  NOTE_E4,8, NOTE_C4,8, REST,4, REST,2,
  NOTE_C4,-8, NOTE_E4,16, NOTE_G4,8, NOTE_C5,8, NOTE_E5,8, NOTE_D5,8, NOTE_C5,8, NOTE_A4,8,
  NOTE_FS4,8, NOTE_G4,8, REST,4, REST,4, REST,8, NOTE_G4,8,
  NOTE_D5,4, NOTE_D5,4, NOTE_B4,8, NOTE_G4,8, REST,8, NOTE_G4,8,
   
  NOTE_C5,4, NOTE_C5,4, NOTE_AS4,16, NOTE_C5,16, NOTE_AS4,16, NOTE_G4,16, NOTE_F4,8, NOTE_DS4,8,
  NOTE_FS4,4, NOTE_FS4,4, NOTE_F4,16, NOTE_G4,16, NOTE_F4,16, NOTE_DS4,16, NOTE_C4,8, NOTE_G4,8,
  NOTE_AS4,8, NOTE_C5,8, REST,4, REST,2,
};
int melody34[] = {  NOTE_C4,4, NOTE_C4,8, 
  NOTE_D4,-4, NOTE_C4,-4, NOTE_F4,-4,
  NOTE_E4,-2, NOTE_C4,4, NOTE_C4,8, 
  NOTE_D4,-4, NOTE_C4,-4, NOTE_G4,-4,
  NOTE_F4,-2, NOTE_C4,4, NOTE_C4,8,

  NOTE_C5,-4, NOTE_A4,-4, NOTE_F4,-4, 
  NOTE_E4,-4, NOTE_D4,-4, NOTE_AS4,4, NOTE_AS4,8,
  NOTE_A4,-4, NOTE_F4,-4, NOTE_G4,-4,
  NOTE_F4,-2,
 
};
int melody35[] = { NOTE_C5,4, //1
  NOTE_F5,4, NOTE_F5,8, NOTE_G5,8, NOTE_F5,8, NOTE_E5,8,
  NOTE_D5,4, NOTE_D5,4, NOTE_D5,4,
  NOTE_G5,4, NOTE_G5,8, NOTE_A5,8, NOTE_G5,8, NOTE_F5,8,
  NOTE_E5,4, NOTE_C5,4, NOTE_C5,4,
  NOTE_A5,4, NOTE_A5,8, NOTE_AS5,8, NOTE_A5,8, NOTE_G5,8,
  NOTE_F5,4, NOTE_D5,4, NOTE_C5,8, NOTE_C5,8,
  NOTE_D5,4, NOTE_G5,4, NOTE_E5,4,

  NOTE_F5,2, NOTE_C5,4, //8 
  NOTE_F5,4, NOTE_F5,8, NOTE_G5,8, NOTE_F5,8, NOTE_E5,8,
  NOTE_D5,4, NOTE_D5,4, NOTE_D5,4,
  NOTE_G5,4, NOTE_G5,8, NOTE_A5,8, NOTE_G5,8, NOTE_F5,8,
  NOTE_E5,4, NOTE_C5,4, NOTE_C5,4,
  NOTE_A5,4, NOTE_A5,8, NOTE_AS5,8, NOTE_A5,8, NOTE_G5,8,
  NOTE_F5,4, NOTE_D5,4, NOTE_C5,8, NOTE_C5,8,
  NOTE_D5,4, NOTE_G5,4, NOTE_E5,4,
  NOTE_F5,2, NOTE_C5,4,

  NOTE_F5,4, NOTE_F5,4, NOTE_F5,4,//17
  NOTE_E5,2, NOTE_E5,4,
  NOTE_F5,4, NOTE_E5,4, NOTE_D5,4,
  NOTE_C5,2, NOTE_A5,4,
  NOTE_AS5,4, NOTE_A5,4, NOTE_G5,4,
  NOTE_C6,4, NOTE_C5,4, NOTE_C5,8, NOTE_C5,8,
  NOTE_D5,4, NOTE_G5,4, NOTE_E5,4,
  NOTE_F5,2, NOTE_C5,4, 
  NOTE_F5,4, NOTE_F5,8, NOTE_G5,8, NOTE_F5,8, NOTE_E5,8,
  NOTE_D5,4, NOTE_D5,4, NOTE_D5,4,
  
  NOTE_G5,4, NOTE_G5,8, NOTE_A5,8, NOTE_G5,8, NOTE_F5,8, //27
  NOTE_E5,4, NOTE_C5,4, NOTE_C5,4,
  NOTE_A5,4, NOTE_A5,8, NOTE_AS5,8, NOTE_A5,8, NOTE_G5,8,
  NOTE_F5,4, NOTE_D5,4, NOTE_C5,8, NOTE_C5,8,
  NOTE_D5,4, NOTE_G5,4, NOTE_E5,4,
  NOTE_F5,2, NOTE_C5,4,
  NOTE_F5,4, NOTE_F5,4, NOTE_F5,4,
  NOTE_E5,2, NOTE_E5,4,
  NOTE_F5,4, NOTE_E5,4, NOTE_D5,4,
  
  NOTE_C5,2, NOTE_A5,4,//36
  NOTE_AS5,4, NOTE_A5,4, NOTE_G5,4,
  NOTE_C6,4, NOTE_C5,4, NOTE_C5,8, NOTE_C5,8,
  NOTE_D5,4, NOTE_G5,4, NOTE_E5,4,
  NOTE_F5,2, NOTE_C5,4, 
  NOTE_F5,4, NOTE_F5,8, NOTE_G5,8, NOTE_F5,8, NOTE_E5,8,
  NOTE_D5,4, NOTE_D5,4, NOTE_D5,4,
  NOTE_G5,4, NOTE_G5,8, NOTE_A5,8, NOTE_G5,8, NOTE_F5,8, 
  NOTE_E5,4, NOTE_C5,4, NOTE_C5,4,
  
  NOTE_A5,4, NOTE_A5,8, NOTE_AS5,8, NOTE_A5,8, NOTE_G5,8,//45
  NOTE_F5,4, NOTE_D5,4, NOTE_C5,8, NOTE_C5,8,
  NOTE_D5,4, NOTE_G5,4, NOTE_E5,4,
  NOTE_F5,2, NOTE_C5,4,
  NOTE_F5,4, NOTE_F5,8, NOTE_G5,8, NOTE_F5,8, NOTE_E5,8,
  NOTE_D5,4, NOTE_D5,4, NOTE_D5,4,
  NOTE_G5,4, NOTE_G5,8, NOTE_A5,8, NOTE_G5,8, NOTE_F5,8,
  NOTE_E5,4, NOTE_C5,4, NOTE_C5,4,
  
  NOTE_A5,4, NOTE_A5,8, NOTE_AS5,8, NOTE_A5,8, NOTE_G5,8, //53
  NOTE_F5,4, NOTE_D5,4, NOTE_C5,8, NOTE_C5,8,
  NOTE_D5,4, NOTE_G5,4, NOTE_E5,4,
  NOTE_F5,2, REST,4
};
int melody36[] = {  NOTE_G4,-4, NOTE_A4,8, NOTE_G4,4,
  NOTE_E4,-2, 
  NOTE_G4,-4, NOTE_A4,8, NOTE_G4,4,
  NOTE_E4,-2, 
  NOTE_D5,2, NOTE_D5,4,
  NOTE_B4,-2,
  NOTE_C5,2, NOTE_C5,4,
  NOTE_G4,-2,

  NOTE_A4,2, NOTE_A4,4,
  NOTE_C5,-4, NOTE_B4,8, NOTE_A4,4,
  NOTE_G4,-4, NOTE_A4,8, NOTE_G4,4,
  NOTE_E4,-2, 
  NOTE_A4,2, NOTE_A4,4,
  NOTE_C5,-4, NOTE_B4,8, NOTE_A4,4,
  NOTE_G4,-4, NOTE_A4,8, NOTE_G4,4,
  NOTE_E4,-2, 
  
  NOTE_D5,2, NOTE_D5,4,
  NOTE_F5,-4, NOTE_D5,8, NOTE_B4,4,
  NOTE_C5,-2,
  NOTE_E5,-2,
  NOTE_C5,4, NOTE_G4,4, NOTE_E4,4,
  NOTE_G4,-4, NOTE_F4,8, NOTE_D4,4,
  NOTE_C4,-2,
  NOTE_C4,-1,};





String nomi[37] = {"Cantina Band from Star Wars","Imperial March from Star Wars","Hedwig’s theme from Harry Potter","Star Wars theme","Pulo da gaita from the Brazilian Movie O Auto da Compadecida","Star Trek fanfare","Game of Thrones","The Godfather","Bloody Tears from Castlevania II","Green Hill Zone from Sonic the Hedgehog","Mii channel theme","Professor Layton’s theme from Professor Layton and the Curious Village","Song of stomrs from The Legend of Zelda Ocarina of time","Super Mario Bros overworld theme","Tetris theme (Korobeiniki)","Zelda’s Lullaby from The Legend of Zelda Ocarina of time","The Legend of Zelda for the NES","DOOM","Jigglypuff’s Song from Pokemon","Vampire Killer from Castlevania","Cannon in D – Pachelbel","Greensleeves","Ode to Joy – Beethoven’s Symphony No. 9","Prince Igor – Borodin’s Polovtsian Dances","Minuet in G – Christian Petzold","Brahms’ Lullaby (Wiegenlied)","Fur Elise – Beethoven","Asa Branca – Luiz Gonzaga","Pink Panther Theme","Take on me A-ha","The lick","The Lion sleeps tonight (A-weema-weh)","Keyboard cat","Elephant Walk","Happy Birthday","We Wish You a Merry Christmas","Silent Night"};

struct NOTE
{
  String title;
  int *note;
  uint16_t numElements;
};

NOTE songs[] =
{
{nomi[0],melody0, NUMELEMENTS(melody0)},
{nomi[1],melody1, NUMELEMENTS(melody1)},
{nomi[2],melody2, NUMELEMENTS(melody2)},
{nomi[3],melody3, NUMELEMENTS(melody3)},
{nomi[4],melody4, NUMELEMENTS(melody4)},
{nomi[5],melody5, NUMELEMENTS(melody5)},
{nomi[6],melody6, NUMELEMENTS(melody6)},
{nomi[7],melody7, NUMELEMENTS(melody7)},
{nomi[8],melody8, NUMELEMENTS(melody8)},
{nomi[9],melody9, NUMELEMENTS(melody9)},
{nomi[10],melody10, NUMELEMENTS(melody10)},
{nomi[11],melody11, NUMELEMENTS(melody11)},
{nomi[12],melody12, NUMELEMENTS(melody12)},
{nomi[13],melody13, NUMELEMENTS(melody13)},
{nomi[14],melody14, NUMELEMENTS(melody14)},
{nomi[15],melody15, NUMELEMENTS(melody15)},
{nomi[16],melody16, NUMELEMENTS(melody16)},
{nomi[17],melody17, NUMELEMENTS(melody17)},
{nomi[18],melody18, NUMELEMENTS(melody18)},
{nomi[19],melody19, NUMELEMENTS(melody19)},
{nomi[20],melody20, NUMELEMENTS(melody20)},
{nomi[21],melody21, NUMELEMENTS(melody21)},
{nomi[22],melody22, NUMELEMENTS(melody22)},
{nomi[23],melody23, NUMELEMENTS(melody23)},
{nomi[24],melody24, NUMELEMENTS(melody24)},
{nomi[25],melody25, NUMELEMENTS(melody25)},
{nomi[26],melody26, NUMELEMENTS(melody26)},
{nomi[27],melody27, NUMELEMENTS(melody27)},
{nomi[28],melody28, NUMELEMENTS(melody28)},
{nomi[29],melody29, NUMELEMENTS(melody29)},
{nomi[30],melody30, NUMELEMENTS(melody30)},
{nomi[31],melody31, NUMELEMENTS(melody31)},
{nomi[32],melody32, NUMELEMENTS(melody32)},
{nomi[33],melody33, NUMELEMENTS(melody33)},
{nomi[34],melody34, NUMELEMENTS(melody34)},
{nomi[35],melody35, NUMELEMENTS(melody35)},
{nomi[36],melody36, NUMELEMENTS(melody36)},
};



// ============================ BUZZER / MUSIC ============================
static constexpr uint8_t BUZZER_CHANNEL = 0;

void buzzerBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(BUZZER_PIN, 2000, 10);
#else
  ledcSetup(BUZZER_CHANNEL, 2000, 10);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
#endif
}

void buzzerTone(uint16_t frequency) {
  if (frequency == 0) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWriteTone(BUZZER_PIN, 0);
#else
    ledcWriteTone(BUZZER_CHANNEL, 0);
#endif
    toneIsOn = false;
    return;
  }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWriteTone(BUZZER_PIN, frequency);
#else
  ledcWriteTone(BUZZER_CHANNEL, frequency);
#endif
  toneIsOn = true;
}

void stopSong() {
  saveCurrentBpmNow();
  buzzerTone(0);
  songPlaying = false;
  currentSong = -1;
  currentNoteIndex = 0;
  uiMode = UiMode::SONG_LIST;
  restoreStatusColor();
}

void startSong(int song) {
  if (song < 0 || song >= (int)NUMELEMENTS(songs) || song >= SONG_COUNT) return;
  currentSong = song;
  tempo = songBpm[song];
  currentNoteIndex = 0;
  songPlaying = true;
  noteEndsAt = 0;
  toneStopsAt = 0;
  uiMode = UiMode::SONG_PLAYING;
}

void serviceSongPlayer() {
  if (!songPlaying || currentSong < 0 || countdownAlarm || nccAlarm || timerPreBeepUntil != 0) return;
  const uint32_t now = millis();

  if (toneIsOn && (int32_t)(now - toneStopsAt) >= 0) {
    buzzerTone(0);
  }

  if (noteEndsAt != 0 && (int32_t)(now - noteEndsAt) < 0) return;

  if (currentNoteIndex >= songs[currentSong].numElements) {
    stopSong();
    return;
  }

  int frequency = songs[currentSong].note[currentNoteIndex];
  int divider = songs[currentSong].note[currentNoteIndex + 1];
  currentNoteIndex += 2;

  tempo = constrain(tempo, 40, 300);
  const int wholenote = (60000 * 2) / tempo;
  int noteDuration;

  if (divider > 0) noteDuration = wholenote / divider;
  else {
    noteDuration = wholenote / abs(divider);
    noteDuration = (int)(noteDuration * 1.5f);
  }

  if (frequency > 0) {
    // Preserve the original music LED behaviour: yellow at note start, then
    // cyan while the note is sounding.
    colora(1, 1, 0);
    buzzerTone((uint16_t)frequency);
    colora(0, 1, 1);
    toneStopsAt = now + (uint32_t)(noteDuration * 0.90f);
  } else {
    buzzerTone(0);
    toneStopsAt = now;
  }

  noteEndsAt = now + noteDuration;
}

// ============================ BUTTONS ============================
void handleButton(uint8_t id) {
  flashButtonColor(id);

  if (uiMode == UiMode::OTA) return;

  if (songPlaying) {
    if (id == 1) {
      tempo = max(40, tempo - 20);
      markCurrentBpmDirty();
    } else if (id == 2) {
      tempo = min(300, tempo + 20);
      markCurrentBpmDirty();
    } else if (id == 4) {
      stopSong();
    }
    return;
  }

  if (uiMode == UiMode::SONG_LIST) {
    const int count = NUMELEMENTS(songs);
    if (id == 1 && selectedIndex + 1 < count) ++selectedIndex;
    else if (id == 2 && selectedIndex > 0) --selectedIndex;
    else if (id == 3) startSong(selectedIndex);
    else if (id == 4) {
      uiMode = UiMode::HOME;
      selectedIndex = 0;
      homePage = HomePage::WEATHER;
      homePageSince = millis();
      restoreStatusColor();
    }
    return;
  }

  if (uiMode == UiMode::TIMER_MENU) {
    if (id == 1) {
      uiMode = UiMode::STOPWATCH_VIEW;
    } else if (id == 2) {
      if (countdownRunning || countdownAlarm || countdownRemainingMs != countdownInitialMs) {
        uiMode = UiMode::COUNTDOWN_VIEW;
      } else {
        syncCountdownSetFromInitial();
        uiMode = UiMode::COUNTDOWN_SET_H;
      }
    } else if (id == 3) {
      enterNccSession();
      return;
    } else if (id == 4) {
      uiMode = UiMode::HOME;
      homePage = HomePage::WEATHER;
      homePageSince = millis();
    }
    restoreStatusColor();
    return;
  }

  if (uiMode == UiMode::STOPWATCH_VIEW) {
    if (id == 1) {
      if (stopwatchRunning) {
        stopwatchAccumulatedMs = stopwatchElapsedMs();
        stopwatchRunning = false;
        addLog("Cronometro in pausa: " + formatDurationMs(stopwatchAccumulatedMs, true));
      } else {
        stopwatchStartedAt = millis();
        stopwatchRunning = true;
        addLog("Cronometro avviato");
      }
    } else if (id == 2) {
      stopwatchRunning = false;
      stopwatchAccumulatedMs = 0;
      stopwatchLapMs = 0;
      addLog("Cronometro azzerato");
    } else if (id == 3) {
      stopwatchLapMs = stopwatchElapsedMs();
      addLog("Giro cronometro: " + formatDurationMs(stopwatchLapMs, true));
    } else if (id == 4) {
      uiMode = UiMode::TIMER_MENU;
    }
    restoreStatusColor();
    return;
  }

  if (uiMode == UiMode::COUNTDOWN_SET_H) {
    if (id == 1) countdownSetHours = (countdownSetHours + 1) % 100;
    else if (id == 2) countdownSetHours = countdownSetHours == 0 ? 99 : countdownSetHours - 1;
    else if (id == 3) uiMode = UiMode::COUNTDOWN_SET_M;
    else if (id == 4) uiMode = UiMode::TIMER_MENU;
    restoreStatusColor();
    return;
  }

  if (uiMode == UiMode::COUNTDOWN_SET_M) {
    if (id == 1) countdownSetMinutes = (countdownSetMinutes + 1) % 60;
    else if (id == 2) countdownSetMinutes = countdownSetMinutes == 0 ? 59 : countdownSetMinutes - 1;
    else if (id == 3) uiMode = UiMode::COUNTDOWN_SET_S;
    else if (id == 4) uiMode = UiMode::COUNTDOWN_SET_H;
    restoreStatusColor();
    return;
  }

  if (uiMode == UiMode::COUNTDOWN_SET_S) {
    if (id == 1) countdownSetSeconds = (countdownSetSeconds + 1) % 60;
    else if (id == 2) countdownSetSeconds = countdownSetSeconds == 0 ? 59 : countdownSetSeconds - 1;
    else if (id == 3) startCountdownFromSetup();
    else if (id == 4) uiMode = UiMode::COUNTDOWN_SET_M;
    restoreStatusColor();
    return;
  }

  if (uiMode == UiMode::COUNTDOWN_VIEW) {
    if (countdownAlarm) {
      if (id == 1) {
        stopCountdownAlarm();
      } else if (id == 2) {
        stopCountdownAlarm();
        countdownRemainingMs = countdownInitialMs;
        countdownEndsAt = millis() + countdownRemainingMs;
        countdownRunning = true;
        addLog("Countdown ripetuto");
      } else if (id == 3) {
        stopCountdownAlarm();
        resetCountdown();
        uiMode = UiMode::COUNTDOWN_SET_H;
      } else if (id == 4) {
        stopCountdownAlarm();
        uiMode = UiMode::TIMER_MENU;
      }
    } else {
      if (id == 1) {
        toggleCountdownPause();
      } else if (id == 2) {
        uint32_t rem = countdownCurrentRemainingMs();
        rem = (rem > 359939000UL) ? 359999000UL : rem + 60000UL;
        countdownRemainingMs = rem;
        if (countdownRunning) countdownEndsAt = millis() + rem;
        addLog("Countdown +1 minuto");
      } else if (id == 3) {
        resetCountdown();
      } else if (id == 4) {
        uiMode = UiMode::TIMER_MENU;
      }
    }
    restoreStatusColor();
    return;
  }

  if (uiMode == UiMode::NCC_SET_M) {
    if (id == 1) nccSetMinutes = (nccSetMinutes + 1) % 100;
    else if (id == 2) nccSetMinutes = nccSetMinutes == 0 ? 99 : nccSetMinutes - 1;
    else if (id == 3) uiMode = UiMode::NCC_SET_S;
    else if (id == 4) exitNccSession();
    restoreStatusColor();
    return;
  }

  if (uiMode == UiMode::NCC_SET_S) {
    if (id == 1) nccSetSeconds = (nccSetSeconds + 1) % 60;
    else if (id == 2) nccSetSeconds = nccSetSeconds == 0 ? 59 : nccSetSeconds - 1;
    else if (id == 3) {
      saveNccDefault();
      uiMode = UiMode::NCC_READY;
    } else if (id == 4) uiMode = UiMode::NCC_SET_M;
    restoreStatusColor();
    return;
  }

  if (uiMode == UiMode::NCC_READY) {
    if (id == 1 || id == 3) startNewNccRound();
    else if (id == 2) { syncNccSetFromDefault(); uiMode = UiMode::NCC_SET_M; restoreStatusColor(); }
    else if (id == 4) exitNccSession();
    return;
  }

  if (uiMode == UiMode::NCC_VIEW) {
    if (nccAlarm) {
      if (id == 1 || id == 3) startNewNccRound();
      else if (id == 2) { stopNccAlarm(); uiMode = UiMode::NCC_READY; }
      else if (id == 4) exitNccSession();
    } else {
      if (id == 1) toggleNccPause();
      else if (id == 2) restartNccSameLetter();
      else if (id == 3) startNewNccRound();
      else if (id == 4) exitNccSession();
    }
    restoreStatusColor();
    return;
  }

  // AP mode keeps the connection instructions visible, but music/timer remain
  // directly reachable. Wi-Fi configuration itself is web-only.
  if (uiMode == UiMode::AP_MODE) {
    if (id == 2) {
      selectedIndex = 0;
      uiMode = UiMode::SONG_LIST;
    } else if (id == 3) {
      uiMode = UiMode::TIMER_MENU;
    } else if (id == 4) {
      goToDeepSleep();
    }
    restoreStatusColor();
    return;
  }

  // HOME: 1 switches the 5s weather / 30s clock cycle manually,
  // 2 opens music, 3 opens timer, 4 enters deep sleep.
  if (id == 1) {
    homePage = (homePage == HomePage::WEATHER) ? HomePage::CLOCK : HomePage::WEATHER;
    homePageSince = millis();
  } else if (id == 2) {
    selectedIndex = 0;
    uiMode = UiMode::SONG_LIST;
  } else if (id == 3) {
    uiMode = UiMode::TIMER_MENU;
  } else if (id == 4) {
    goToDeepSleep();
  }
  restoreStatusColor();
}

void initializeButtonStates() {
  for (uint8_t i = 0; i < 4; ++i) {
    bool level = digitalRead(buttons[i].pin);
    buttons[i].stable = level;
    buttons[i].lastRaw = level;
    buttons[i].changedAt = millis();
  }
}

void serviceButtons() {
  const uint32_t now = millis();

  for (uint8_t i = 0; i < 4; ++i) {
    bool raw = digitalRead(buttons[i].pin);
    if (raw != buttons[i].lastRaw) {
      buttons[i].lastRaw = raw;
      buttons[i].changedAt = now;
    }

    if (now - buttons[i].changedAt >= 35 && raw != buttons[i].stable) {
      buttons[i].stable = raw;
      if (raw == LOW) handleButton(i + 1);
    }
  }

  if (colorFlashActive && (int32_t)(now - colorFlashUntil) >= 0) {
    colorFlashActive = false;
    restoreStatusColor();
  }
}

// ============================ SCREEN MODEL ============================
void updateScreenModel() {
  if (millis() - lastScreenRefresh < SCREEN_REFRESH_MS) return;
  lastScreenRefresh = millis();

  if (uiMode == UiMode::OTA) return;

  if (uiMode == UiMode::SONG_PLAYING && currentSong >= 0) {
    setScreenLine(0, "Riproduzione");
    setScreenLine(1, songs[currentSong].title);
    setScreenLine(2, "Tempo: " + String(tempo) + " BPM");
    setScreenLine(3, "1- / 2+ / 4 Stop");
    return;
  }

  if (uiMode == UiMode::SONG_LIST) {
    const int count = NUMELEMENTS(songs);
    setScreenLine(0, String(count) + " canzoni trovate");
    for (uint8_t row = 1; row < 4; ++row) {
      int idx = selectedIndex + row - 1;
      if (idx < count) setScreenLine(row, String(idx == selectedIndex ? ">" : " ") + songs[idx].title);
      else setScreenLine(row, "");
    }
    return;
  }

  if (uiMode == UiMode::TIMER_MENU) {
    setScreenLine(0, "TIMER   4 Indietro");
    setScreenLine(1, "1 Cronometro");
    setScreenLine(2, "2 Countdown");
    setScreenLine(3, "3 Nomi Cose Citta");
    return;
  }

  if (uiMode == UiMode::STOPWATCH_VIEW) {
    setScreenLine(0, stopwatchRunning ? "CRONOMETRO >" : "CRONOMETRO ||");
    setScreenLine(1, formatDurationMs(stopwatchElapsedMs(), true));
    setScreenLine(2, stopwatchLapMs ? "Giro " + formatDurationMs(stopwatchLapMs, true) : "Giro --:--.--");
    setScreenLine(3, "1 Start 2 Reset 3 Giro");
    return;
  }

  if (uiMode == UiMode::COUNTDOWN_SET_H || uiMode == UiMode::COUNTDOWN_SET_M || uiMode == UiMode::COUNTDOWN_SET_S) {
    String field = uiMode == UiMode::COUNTDOWN_SET_H ? "ORE" : (uiMode == UiMode::COUNTDOWN_SET_M ? "MINUTI" : "SECONDI");
    char buf[20];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", countdownSetHours, countdownSetMinutes, countdownSetSeconds);
    setScreenLine(0, "COUNTDOWN - " + field);
    setScreenLine(1, String(buf));
    setScreenLine(2, "1 +   2 -");
    setScreenLine(3, uiMode == UiMode::COUNTDOWN_SET_S ? "3 AVVIA  4 Indietro" : "3 Avanti 4 Indietro");
    return;
  }

  if (uiMode == UiMode::COUNTDOWN_VIEW) {
    if (countdownAlarm) {
      setScreenLine(0, "COUNTDOWN TERMINATO");
      setScreenLine(1, "00:00:00");
      setScreenLine(2, "1 Stop 2 Ripeti");
      setScreenLine(3, "3 Nuovo 4 Indietro");
    } else {
      uint32_t rem = countdownCurrentRemainingMs();
      setScreenLine(0, countdownRunning ? "COUNTDOWN >" : "COUNTDOWN ||");
      setScreenLine(1, formatDurationMs(rem));
      setScreenLine(2, "1 Pausa 2 +1 min");
      setScreenLine(3, "3 Reset 4 Indietro");
    }
    return;
  }

  if (uiMode == UiMode::NCC_SET_M || uiMode == UiMode::NCC_SET_S) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%02u:%02u", nccSetMinutes, nccSetSeconds);
    setScreenLine(0, "NOMI COSE CITTA");
    setScreenLine(1, String("Durata ") + buf);
    setScreenLine(2, uiMode == UiMode::NCC_SET_M ? "MINUTI: 1+  2-" : "SECONDI: 1+ 2-");
    setScreenLine(3, uiMode == UiMode::NCC_SET_S ? "3 CONFERMA 4 Ind." : "3 Avanti  4 Esci");
    return;
  }

  if (uiMode == UiMode::NCC_READY) {
    setScreenLine(0, "NOMI COSE CITTA");
    setScreenLine(1, "Durata " + formatMmSs(nccDefaultMs));
    if (nccUsedLettersMask == ((1UL << 26) - 1UL)) setScreenLine(2, "LETTERE ESAURITE");
    else setScreenLine(2, "1 AVVIA LA MANCHE");
    setScreenLine(3, "2 Durata  4 Esci");
    return;
  }

  if (uiMode == UiMode::NCC_VIEW) {
    uint32_t rem = nccCurrentRemainingMs();
    String right0 = "  NOMI COSE";
    String right1 = "  CITTA " + String(nccLetter);
    String right2 = "  " + formatMmSs(rem);
    String right3;
    if (nccAlarm) right3 = "  1/3 NUOVA";
    else if (nccRunning) right3 = rem <= 10000UL ? "  ULTIMI 10!" : "  1PAUSA 3N";
    else right3 = "  1START 3N";
    setScreenLine(0, bigGameLetterRow(nccLetter, 0) + right0);
    setScreenLine(1, bigGameLetterRow(nccLetter, 1) + right1);
    setScreenLine(2, bigGameLetterRow(nccLetter, 2) + right2);
    setScreenLine(3, bigGameLetterRow(nccLetter, 3) + right3);
    return;
  }

  if (uiMode == UiMode::AP_MODE && apActive && WiFi.status() != WL_CONNECTED) {
    setScreenLine(0, "AP: " + deviceName);
    setScreenLine(1, "Apri 192.168.4.1");
    setScreenLine(2, isnan(temperatureC) ? "Temperatura: --" : "Temperatura: " + String(temperatureC, 1) + " C");
    setScreenLine(3, isnan(humidity) ? "Umidita: --" : "Umidita: " + String(humidity, 1) + " %");
    return;
  }

  if (uiMode == UiMode::HOME) {
    if (homePageSince == 0) homePageSince = millis();
    uint32_t elapsed = millis() - homePageSince;

    if (homePage == HomePage::WEATHER && elapsed >= HOME_WEATHER_MS) {
      homePage = HomePage::CLOCK;
      homePageSince = millis();
    } else if (homePage == HomePage::CLOCK && elapsed >= HOME_CLOCK_MS) {
      homePage = HomePage::WEATHER;
      homePageSince = millis();
    }

    if (homePage == HomePage::CLOCK) {
      setBigClockScreen();
      return;
    }

    if (WiFi.status() == WL_CONNECTED) {
      setScreenLine(0, localTimeString());
      setScreenLine(1, "IP: " + WiFi.localIP().toString());
    } else {
      setScreenLine(0, "WiFi non collegato");
      setScreenLine(1, apActive ? "Web: 192.168.4.1" : "Riconnessione...");
    }
    setScreenLine(2, isnan(humidity) ? "Umidita: --" : "Umidita: " + String(humidity, 1) + " %");
    setScreenLine(3, isnan(temperatureC) ? "Temperatura: --" : "Temperatura: " + String(temperatureC, 1) + " C");
    return;
  }
}

// ============================ MQTT ============================
void serviceMqtt() {
  if (!mqttEnabled || !mqttHost.length() || WiFi.status() != WL_CONNECTED) {
    if (mqtt.connected()) mqtt.disconnect();
    return;
  }

  mqtt.loop();

  if (!mqtt.connected()) {
    if (millis() - lastMqttAttempt < MQTT_RETRY_MS) return;
    lastMqttAttempt = millis();

    mqtt.setServer(mqttHost.c_str(), mqttPort);
    String clientId = deviceName + "-" + String((unsigned long)(ESP.getEfuseMac() & 0xFFFFFF), (unsigned char)HEX);

    bool ok;
    if (mqttUser.length()) ok = mqtt.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str());
    else ok = mqtt.connect(clientId.c_str());

    if (!ok) return;
    addLog("MQTT connesso: " + mqttHost);
  }

  if (alarmPublishPending && mqtt.connected()) {
    String alarmPayload = "{";
    alarmPayload += "\"device\":\"" + jsonEscape(deviceName) + "\",";
    alarmPayload += "\"state\":\"" + jsonEscape(alarmSummary()) + "\",";
    alarmPayload += "\"temperature_c\":" + String(temperatureC, 1) + ",";
    alarmPayload += "\"humidity_pct\":" + String(humidity, 1) + "}";
    mqtt.publish(alarmTopic.c_str(), alarmPayload.c_str(), true);
    alarmPublishPending = false;
  }

  if (millis() - lastMqttPublish < MQTT_PUBLISH_MS) return;
  if (isnan(temperatureC) || isnan(humidity)) return;
  lastMqttPublish = millis();

  String payload = "{";
  payload += "\"temperature_c\":" + String(temperatureC, 1) + ",";
  payload += "\"humidity_pct\":" + String(humidity, 1) + ",";
  payload += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"uptime_s\":" + String(millis() / 1000) + ",";
  payload += "\"alarm\":\"" + jsonEscape(alarmSummary()) + "\"";
  payload += "}";

  mqtt.publish(mqttTopic.c_str(), payload.c_str(), true);
}

// ============================ WEB ============================
const char WEB_PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="it"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><title>stazionemeteo</title>
<style>
:root{color-scheme:light dark;--bg:#0b1118;--panel:#111a24;--panel2:#16212d;--line:#263646;--txt:#edf4fb;--muted:#9db0c3;--accent:#4ea1ff;--ok:#38c77a;--warn:#ffb84d;--bad:#ff6b6b;--shadow:0 8px 28px #0005}
@media(prefers-color-scheme:light){:root{--bg:#eef3f8;--panel:#fff;--panel2:#f7f9fc;--line:#d8e1ea;--txt:#15202b;--muted:#617386;--accent:#006edc;--ok:#17854f;--warn:#a56200;--bad:#c72d2d;--shadow:0 6px 24px #22334418}}
*{box-sizing:border-box}html{scroll-behavior:smooth}body{margin:0;background:var(--bg);color:var(--txt);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}button,input,select,summary{font:inherit}
header{position:sticky;top:0;z-index:10;background:var(--bg);backdrop-filter:blur(12px);border-bottom:1px solid var(--line)}.head{max-width:1180px;margin:auto;padding:12px clamp(12px,3vw,24px);display:flex;align-items:center;gap:12px}.brand{font-weight:800;font-size:clamp(18px,3vw,24px);min-width:0;overflow:hidden;text-overflow:ellipsis}.pill{margin-left:auto;border:1px solid var(--line);padding:5px 9px;border-radius:99px;color:var(--muted);white-space:nowrap}
main{max-width:1180px;margin:auto;padding:clamp(12px,3vw,24px);display:grid;grid-template-columns:repeat(12,minmax(0,1fr));gap:clamp(10px,2vw,16px)}.card{grid-column:span 6;background:var(--panel);border:1px solid var(--line);border-radius:16px;padding:clamp(13px,2.5vw,19px);box-shadow:var(--shadow);min-width:0}.wide{grid-column:1/-1}.third{grid-column:span 4}h2{margin:0 0 12px;font-size:17px}.muted{color:var(--muted)}.statusline{word-break:break-word}.metrics{display:grid;grid-template-columns:repeat(2,1fr);gap:10px}.metric{background:var(--panel2);border:1px solid var(--line);border-radius:12px;padding:12px}.value{font-size:clamp(28px,6vw,42px);font-weight:800;letter-spacing:-1px}.unit{color:var(--muted)}
.lcdbezel{background:linear-gradient(145deg,#333,#111);border:1px solid #444;border-radius:14px;padding:clamp(8px,2vw,14px);box-shadow:inset 0 1px #ffffff15,0 8px 22px #0005}.lcdglass{background:linear-gradient(135deg,#b8d77f,#9dbc68);border:1px solid #6e8747;border-radius:7px;padding:clamp(6px,1.5vw,10px);box-shadow:inset 0 0 22px #49612755}.lcdcanvas{display:block;width:100%;height:auto;aspect-ratio:2.5/1;image-rendering:pixelated}.keygrid{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;margin-top:12px}.key{min-height:88px;text-align:left;display:flex;flex-direction:column;justify-content:space-between}.key b{font-size:15px}.key small{display:block;color:var(--muted);line-height:1.25;margin-top:7px}.key:disabled{opacity:.45;cursor:not-allowed}
button,.button{border:1px solid var(--line);border-radius:10px;padding:10px 12px;background:var(--panel2);color:var(--txt);cursor:pointer;transition:.15s}button:hover:not(:disabled){border-color:var(--accent);transform:translateY(-1px)}button.primary{background:var(--accent);color:#fff;border-color:transparent}.danger{color:var(--bad)}input,select{width:100%;padding:10px 11px;border:1px solid var(--line);border-radius:9px;background:var(--bg);color:var(--txt)}input[type=checkbox]{width:auto}.clockpreview{margin-top:10px;background:#a9c875;color:#152108;border-radius:10px;padding:12px;text-align:center;font:800 clamp(28px,8vw,50px)/1 ui-monospace,monospace;letter-spacing:.08em}.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}.field{display:block;margin:7px 0;color:var(--muted);font-size:13px}.field input{margin-top:4px;color:var(--txt)}.net{display:flex;align-items:center;gap:10px;padding:9px 0;border-bottom:1px solid var(--line)}.net .grow{flex:1;min-width:0}.net b{word-break:break-word}.actions{display:flex;flex-wrap:wrap;gap:8px;align-items:center}details{border-top:1px solid var(--line);margin-top:12px;padding-top:10px}summary{cursor:pointer;font-weight:650}.diag{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:6px 12px}.diag div{min-width:0;overflow-wrap:anywhere}.log{max-height:280px;overflow:auto;background:var(--bg);border:1px solid var(--line);border-radius:9px;padding:8px;font:12px/1.5 ui-monospace,monospace}.log div{padding:3px 0;border-bottom:1px dotted var(--line)}canvas{width:100%;height:230px;background:var(--panel2);border:1px solid var(--line);border-radius:10px}.alarm{color:var(--bad);font-weight:700}.ok{color:var(--ok)}.ledline{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-top:11px}.rgbdot{width:18px;height:18px;border-radius:50%;border:1px solid var(--line);box-shadow:0 0 12px #0006;display:inline-block}.timerstate{color:var(--muted);font-size:13px}
@media(max-width:900px){.card,.third{grid-column:1/-1}.keygrid{grid-template-columns:repeat(2,1fr)}}
@media(max-width:540px){main{padding:10px;gap:10px}.head{padding:10px}.pill{font-size:12px}.card{padding:12px;border-radius:13px}.metrics,.row,.diag{grid-template-columns:1fr}.keygrid{grid-template-columns:1fr 1fr}.key{min-height:96px}.actions>button{flex:1 1 140px}.net{align-items:flex-start;flex-wrap:wrap}.net button{width:100%}canvas{height:190px}}
</style></head><body>
<header><div class="head"><div id="brand" class="brand">stazionemeteo</div><div id="fw" class="pill">firmware</div></div></header>
<main>
<section class="card third"><h2>Meteo</h2><div class="metrics"><div class="metric"><div id="temp" class="value">--</div><div class="unit">Temperatura °C</div></div><div class="metric"><div id="hum" class="value">--</div><div class="unit">Umidità %</div></div></div><p id="alarmState" class="muted"></p><p id="status" class="muted statusline"></p></section>
<section class="card"><h2>Replica LCD 20×4</h2><div class="lcdbezel"><div class="lcdglass"><canvas id="lcdCanvas" class="lcdcanvas" width="1000" height="400" aria-label="Replica dot-matrix del display LCD 20 per 4"></canvas></div></div><div class="ledline"><span id="rgbLed" class="rgbdot"></span><span>LED RGB: <b id="rgbText">--</b></span><span id="timerState" class="timerstate"></span></div><div id="keys" class="keygrid">
<button class="key" onclick="press(1)"><b>Tasto 1 · Schermata</b><small>Passa tra meteo e orologio fullscreen</small></button>
<button class="key" onclick="press(2)"><b>Tasto 2 · Musica</b><small>Apre il menu delle melodie</small></button>
<button class="key" onclick="press(3)"><b>Tasto 3 · Timer</b><small>Apre cronometro, countdown e Nomi Cose e Città</small></button>
<button class="key" onclick="press(4)"><b>Tasto 4 · Indietro / Sleep</b><small>Torna indietro; dalla home entra in deep sleep</small></button>
</div></section>
<section class="card wide"><h2>Storico locale ~24 ore</h2><canvas id="chart"></canvas><div class="muted">Campionamento ogni 5 minuti; lo storico è in RAM e riparte dopo un riavvio.</div></section>
<section class="card"><h2>Wi‑Fi</h2><div class="actions"><button class="primary" onclick="scan()">Scansiona adesso</button><span id="scanstate" class="muted">Nessuna scansione automatica</span></div><div id="nets"></div><details><summary>Ultime 3 reti memorizzate</summary><div id="saved"></div></details></section>
<section class="card"><h2>MQTT</h2><form id="mqttForm"><label><input id="mqen" type="checkbox"> Abilita MQTT</label><label class="field">Server<input id="mqhost" placeholder="192.168.1.10"></label><div class="row"><label class="field">Porta<input id="mqport" type="number" value="1883"></label><label class="field">Topic sensore<input id="mqtopic"></label></div><div class="row"><label class="field">Username<input id="mquser"></label><label class="field">Password<input id="mqpass" type="password" placeholder="vuoto = non modificare"></label></div><button type="submit">Salva MQTT</button> <span id="mqmsg" class="muted"></span></form></section>
<section class="card"><h2>Allarmi</h2><form id="alarmForm"><label><input id="alen" type="checkbox"> Abilita allarmi</label><div class="row"><label class="field">Temperatura minima °C<input id="tmin" type="number" step="0.1"></label><label class="field">Temperatura massima °C<input id="tmax" type="number" step="0.1"></label></div><div class="row"><label class="field">Umidità minima %<input id="hmin" type="number" step="0.1"></label><label class="field">Umidità massima %<input id="hmax" type="number" step="0.1"></label></div><label class="field">Topic MQTT allarmi<input id="altopic"></label><button type="submit">Salva allarmi</button> <span id="almsg" class="muted"></span></form></section>
<section class="card"><h2>Nome dispositivo</h2><p class="muted">Determina hostname, mDNS e nome della rete AP. Il default resta <b>stazionemeteo</b>.</p><form id="nameForm"><label class="field">Nome<input id="devname" maxlength="31"></label><button type="submit">Salva e riavvia</button></form></section>
<section class="card"><h2>Orologio fullscreen</h2><p class="muted">Scegli il disegno delle cifre sul display 20×4. La modifica è immediata e viene memorizzata.</p><form id="clockForm"><label class="field">Stile<select id="clockstyle"><option value="0">Morbido</option><option value="1">Arrotondato</option><option value="2">Sottile</option><option value="3">Punti</option><option value="4">Tech</option></select></label><button type="submit">Applica stile</button> <span id="clockmsg" class="muted"></span></form><div class="lcdbezel" style="margin-top:10px"><div class="lcdglass"><canvas id="clockPreview" class="lcdcanvas" width="1000" height="400"></canvas></div></div></section>
<section class="card"><h2>Diagnostica</h2><div id="diag" class="diag"></div><p><a class="button" href="/metrics" target="_blank">Apri /metrics Prometheus</a></p></section>
<section class="card"><h2>Backup e ripristino</h2><p class="muted">Il backup contiene anche password Wi‑Fi e MQTT: trattalo come file riservato.</p><div class="actions"><button onclick="backupConfig()">Scarica backup JSON</button><input id="restoreFile" type="file" accept="application/json,.json"><button onclick="restoreConfig()">Ripristina e riavvia</button></div><p id="restoreMsg" class="muted"></p></section>
<section class="card"><h2>Aggiornamento firmware OTA</h2><p class="muted">Versione corrente: <span id="fw2"></span>. Il firmware viene verificato da Update prima dell'attivazione.</p><form id="otaForm"><input id="fwfile" type="file" accept=".bin" required><button type="submit">Carica firmware</button></form><p id="otaMsg" class="muted"></p></section>
<section class="card wide"><h2>Log eventi</h2><div class="actions"><button onclick="loadLog()">Aggiorna log</button><button class="danger" onclick="clearLog()">Cancella log</button></div><div id="log" class="log"></div></section>
</main>
<script>
const $=id=>document.getElementById(id);const esc=s=>(s??'').toString().replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));let settingsLoaded=false,lastHistory=0;
async function api(url,opt){opt=opt||{};if(!opt.cache)opt.cache='no-store';let r=await fetch(url,opt);if(!r.ok)throw new Error(await r.text());return r}
function keyHtml(k){return `<button class="key" ${k.enabled?'':'disabled'} onclick="press(${k.id})"><b>Tasto ${k.id} · ${esc(k.title)}</b><small>${esc(k.hint)}</small></button>`}
const ROM5={
' ':[0,0,0,0,0,0,0,0],':':[0,4,4,0,0,4,4,0],'.':[0,0,0,0,0,0,6,6],'-':[0,0,0,31,0,0,0,0],
'/':[1,2,4,8,16,0,0,0],'%':[17,2,4,8,17,0,0,0],'+':[0,4,4,31,4,4,0,0],
'>':[16,8,4,2,4,8,16,0],'<':[1,2,4,8,4,2,1,0],'=':[0,31,0,31,0,0,0,0],
'!':[4,4,4,4,4,0,4,0],'?':[14,17,1,2,4,0,4,0],'_':[0,0,0,0,0,0,31,0],
'0':[14,17,19,21,25,17,14,0],'1':[4,12,4,4,4,4,14,0],'2':[14,17,1,2,4,8,31,0],
'3':[30,1,1,14,1,1,30,0],'4':[2,6,10,18,31,2,2,0],'5':[31,16,16,30,1,1,30,0],
'6':[14,16,16,30,17,17,14,0],'7':[31,1,2,4,8,8,8,0],'8':[14,17,17,14,17,17,14,0],
'9':[14,17,17,15,1,1,14,0],
'A':[14,17,17,31,17,17,17,0],'B':[30,17,17,30,17,17,30,0],'C':[14,17,16,16,16,17,14,0],
'D':[30,17,17,17,17,17,30,0],'E':[31,16,16,30,16,16,31,0],'F':[31,16,16,30,16,16,16,0],
'G':[14,17,16,23,17,17,15,0],'H':[17,17,17,31,17,17,17,0],'I':[14,4,4,4,4,4,14,0],
'J':[7,2,2,2,18,18,12,0],'K':[17,18,20,24,20,18,17,0],'L':[16,16,16,16,16,16,31,0],
'M':[17,27,21,21,17,17,17,0],'N':[17,25,21,19,17,17,17,0],'O':[14,17,17,17,17,17,14,0],
'P':[30,17,17,30,16,16,16,0],'Q':[14,17,17,17,21,18,13,0],'R':[30,17,17,30,20,18,17,0],
'S':[15,16,16,14,1,1,30,0],'T':[31,4,4,4,4,4,4,0],'U':[17,17,17,17,17,17,14,0],
'V':[17,17,17,17,17,10,4,0],'W':[17,17,17,17,21,21,10,0],'X':[17,17,10,4,10,17,17,0],
'Y':[17,17,10,4,4,4,4,0],'Z':[31,1,2,4,8,16,31,0],
'a':[0,0,14,1,15,17,15,0],'b':[16,16,22,25,17,17,30,0],'c':[0,0,14,17,16,17,14,0],
'd':[1,1,13,19,17,17,15,0],'e':[0,0,14,17,31,16,14,0],'f':[6,9,8,28,8,8,8,0],
'g':[0,0,15,17,15,1,14,0],'h':[16,16,22,25,17,17,17,0],'i':[4,0,12,4,4,4,14,0],
'j':[2,0,6,2,2,18,12,0],'k':[16,16,18,20,24,20,18,0],'l':[12,4,4,4,4,4,14,0],
'm':[0,0,26,21,21,21,21,0],'n':[0,0,22,25,17,17,17,0],'o':[0,0,14,17,17,17,14,0],
'p':[0,0,30,17,30,16,16,0],'q':[0,0,15,17,15,1,1,0],'r':[0,0,22,25,16,16,16,0],
's':[0,0,15,16,14,1,30,0],'t':[8,8,28,8,8,9,6,0],'u':[0,0,17,17,17,19,13,0],
'v':[0,0,17,17,17,10,4,0],'w':[0,0,17,17,21,21,10,0],'x':[0,0,17,10,4,10,17,0],
'y':[0,0,17,17,15,1,14,0],'z':[0,0,31,2,4,8,31,0]
 };
function drawLcd(canvas,cells,custom){
 if(!canvas)return;
 const ctx=canvas.getContext('2d');
 if(!ctx)return;
 const cols=20,rows=4;
 if(!Array.isArray(cells)||cells.length<cols*rows)cells=Array(cols*rows).fill(32);
 if(!Array.isArray(custom))custom=[];
 // Keep an exact 20x4 geometry while using a high-resolution internal canvas.
 const W=1000,H=400,cw=W/cols,ch=H/rows;
 if(canvas.width!==W)canvas.width=W;if(canvas.height!==H)canvas.height=H;
 ctx.clearRect(0,0,W,H);
 ctx.fillStyle='#a9c978';ctx.fillRect(0,0,W,H);
 // Subtle per-character blocks like a physical HD44780 glass/module.
 for(let r=0;r<rows;r++)for(let c=0;c<cols;c++){
   const x=c*cw,y=r*ch;
   ctx.fillStyle=(r+c)%2?'rgba(70,95,43,.018)':'rgba(255,255,255,.012)';
   ctx.fillRect(x+1,y+1,cw-2,ch-2);
 }
 const charPadX=7,charPadY=7;
 const gridW=cw-charPadX*2,gridH=ch-charPadY*2;
 const stepX=gridW/5,stepY=gridH/8;
 const dotR=Math.max(1.7,Math.min(stepX,stepY)*0.31);
 const on='#1c2e13',off='rgba(35,55,23,.105)';
 function bitmapFor(code){
   code=Number(code)||0;
   if(code>=0&&code<=7&&Array.isArray(custom[code]))return custom[code];
   const ch=String.fromCharCode(code);
   return ROM5[ch]||ROM5[ch.toUpperCase()]||ROM5['?'];
 }
 for(let r=0;r<rows;r++)for(let c=0;c<cols;c++){
   const bmp=bitmapFor(cells[r*cols+c]);
   const ox=c*cw+charPadX,oy=r*ch+charPadY;
   for(let py=0;py<8;py++){
     const bits=Number(bmp[py]||0);
     for(let px=0;px<5;px++){
       const active=!!(bits&(1<<(4-px)));
       const cx=ox+(px+.5)*stepX,cy=oy+(py+.5)*stepY;
       ctx.beginPath();ctx.arc(cx,cy,dotR,0,Math.PI*2);
       ctx.fillStyle=active?on:off;ctx.fill();
     }
   }
 }
}
const CLOCK_SEG=[63,6,91,79,102,109,125,7,127,111];
const CLOCK_GLYPHS_JS=[
 [[0,0,0,0,0,14,31,31],[31,31,14,0,0,0,0,0],[24,24,24,24,24,24,24,24],[3,3,3,3,3,3,3,3],[24,24,24,24,24,30,31,31],[3,3,3,3,3,15,31,31],[31,31,30,24,24,24,24,24],[31,31,15,3,3,3,3,3]],
 [[0,0,0,0,0,4,14,31],[31,14,4,0,0,0,0,0],[16,24,24,24,24,24,24,16],[1,3,3,3,3,3,3,1],[16,24,24,24,24,28,30,31],[1,3,3,3,3,7,15,31],[31,30,28,24,24,24,24,16],[31,15,7,3,3,3,3,1]],
 [[0,0,0,0,0,0,0,31],[31,0,0,0,0,0,0,0],[16,16,16,16,16,16,16,16],[1,1,1,1,1,1,1,1],[16,16,16,16,16,16,16,31],[1,1,1,1,1,1,1,31],[31,16,16,16,16,16,16,16],[31,1,1,1,1,1,1,1]],
 [[0,0,0,0,0,21,0,21],[21,0,21,0,0,0,0,0],[16,0,16,0,16,0,16,0],[1,0,1,0,1,0,1,0],[16,0,16,0,16,21,0,21],[1,0,1,0,1,21,0,21],[21,0,21,0,16,0,16,0],[21,0,21,0,1,0,1,0]],
 [[0,0,0,0,0,31,0,31],[31,0,31,0,0,0,0,0],[24,24,0,24,24,0,24,24],[3,3,0,3,3,0,3,3],[24,24,0,24,24,31,0,31],[3,3,0,3,3,31,0,31],[31,0,31,24,24,0,24,24],[31,0,31,3,3,0,3,3]]
];
function clockCell(d,r,c){
 let seg=CLOCK_SEG[d]||0,a=seg&1,b=seg&2,cc=seg&4,dd=seg&8,e=seg&16,f=seg&32,g=seg&64;
 if(r===0)return a?0:32;if(r===3)return dd?1:32;
 if(r===1){if(c===0)return f&&g?4:f?2:g?0:32;if(c===1)return g?0:32;return b&&g?5:b?3:g?0:32}
 if(c===0)return e&&g?6:e?2:g?1:32;if(c===1)return g?1:32;return cc&&g?7:cc?3:g?1:32
}
function previewCells(style,hm='12:34'){
 let cells=Array(80).fill(32);
 function digit(d,start){for(let r=0;r<4;r++)for(let x=0;x<3;x++)cells[r*20+start+x]=clockCell(d,r,x)}
 digit(Number(hm[0]),1);digit(Number(hm[1]),5);cells[1*20+9]=58;cells[2*20+9]=58;digit(Number(hm[3]),11);digit(Number(hm[4]),15);
 return {cells,custom:CLOCK_GLYPHS_JS[style]||CLOCK_GLYPHS_JS[0]};
}
function updateClockPreview(){let st=Number($('clockstyle').value),p=previewCells(st);drawLcd($('clockPreview'),p.cells,p.custom)}
async function getState(){try{const s=await api('/api/state').then(r=>r.json());$('brand').textContent=s.device.name;document.title=s.device.name;$('fw').textContent='v'+s.firmware;$('fw2').textContent=s.firmware;$('temp').textContent=s.temperature_c==null?'--':s.temperature_c.toFixed(1);$('hum').textContent=s.humidity_pct==null?'--':s.humidity_pct.toFixed(1);$('alarmState').innerHTML=s.alarms.active?'<span class="alarm">ALLARME: '+esc(s.alarms.summary)+'</span>':'<span class="ok">Allarmi: '+(s.alarms.enabled?'OK':'disabilitati')+'</span>';$('status').textContent=(s.wifi.connected?'Wi‑Fi '+s.wifi.ssid+' · '+s.wifi.ip+' · '+s.wifi.rssi+' dBm':'Wi‑Fi non collegato')+(s.ap?' · AP '+s.device.name+' attivo':'')+' · MQTT '+(s.mqtt.connected?'connesso':'non connesso');drawLcd($('lcdCanvas'),s.lcd_matrix.cells,s.lcd_matrix.custom);let rr=s.rgb.r?255:0,gg=s.rgb.g?255:0,bb=s.rgb.b?255:0;$('rgbLed').style.background=`rgb(${rr},${gg},${bb})`;$('rgbText').textContent=s.rgb.meaning;$('timerState').textContent=s.timers.ncc_alarm?'Nomi Cose Città: tempo scaduto':s.timers.ncc_running?'Nomi Cose Città '+s.timers.ncc_letter+' · '+fmtMs(s.timers.ncc_remaining_ms):s.timers.countdown_alarm?'Countdown terminato':s.timers.countdown_running?'Countdown '+fmtMs(s.timers.countdown_remaining_ms):s.timers.stopwatch_running?'Cronometro '+fmtMs(s.timers.stopwatch_ms):'';$('keys').innerHTML=s.buttons.map(keyHtml).join('');$('scanstate').textContent=s.scan_running?'Scansione richiesta in corso…':'Scansione solo su richiesta';$('saved').innerHTML=s.saved.map((x,i)=>`<div class="net"><div class="grow">${esc(x)}</div><button class="danger" onclick="forgetWifi(${i})">Dimentica</button></div>`).join('')||'<p class="muted">Nessuna rete salvata</p>';
$('diag').innerHTML=[['Uptime',fmtUptime(s.diag.uptime_s)],['Heap libero',s.diag.free_heap+' B'],['Heap minimo',s.diag.min_free_heap+' B'],['Reset',s.diag.reset_reason],['Wake-up',s.diag.wake_reason],['RSSI',s.wifi.connected?s.wifi.rssi+' dBm':'—'],['AP client',s.diag.ap_clients],['Sensore letto',s.diag.sensor_age_s+' s fa'],['mDNS',s.device.name+'.local'],['Modalità',s.mode],['Watchdog loop',s.diag.watchdog?'attivo':'non registrato']].map(x=>`<div class="muted">${esc(x[0])}</div><div>${esc(x[1])}</div>`).join('');
if(!settingsLoaded){$('mqen').checked=s.mqtt.enabled;$('mqhost').value=s.mqtt.host;$('mqport').value=s.mqtt.port;$('mqtopic').value=s.mqtt.topic;$('mquser').value=s.mqtt.user;$('alen').checked=s.alarms.enabled;$('tmin').value=s.alarms.temp_min;$('tmax').value=s.alarms.temp_max;$('hmin').value=s.alarms.humidity_min;$('hmax').value=s.alarms.humidity_max;$('altopic').value=s.alarms.topic;$('devname').value=s.device.name;$('clockstyle').value=s.clock.style;updateClockPreview();settingsLoaded=true}if(Date.now()-lastHistory>60000){loadHistory();lastHistory=Date.now()}}catch(e){$('status').textContent='Pannello non raggiungibile: '+e.message}}
function fmtUptime(v){let d=Math.floor(v/86400),h=Math.floor(v%86400/3600),m=Math.floor(v%3600/60);return `${d}g ${h}h ${m}m`}function fmtMs(ms){let t=Math.max(0,Math.floor(ms/1000)),h=Math.floor(t/3600),m=Math.floor(t%3600/60),s=t%60;return [h,m,s].map(x=>String(x).padStart(2,"0")).join(":")}
async function press(n){await api('/api/button?id='+n,{method:'POST'});setTimeout(getState,100)}
async function scan(){await api('/api/wifi/scan',{method:'POST'});$('nets').innerHTML='';pollScan()}
async function pollScan(){try{const d=await api('/api/wifi/networks').then(r=>r.json());$('scanstate').textContent=d.running?'Scansione richiesta in corso…':'Scansione completata';$('nets').innerHTML=d.networks.map((n,i)=>`<div class="net"><div class="grow"><b>${esc(n.ssid)}</b><br><span class="muted">${n.rssi} dBm · ${n.open?'aperta':'protetta'}</span></div><button onclick="joinWifi(${i},${n.open})">Collega</button></div>`).join('');if(d.running)setTimeout(pollScan,700)}catch(e){$('scanstate').textContent=e.message}}
async function joinWifi(i,open){let p=open?'':prompt('Password Wi‑Fi:','');if(p===null)return;let body=new URLSearchParams({index:i,password:p});alert(await api('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body}).then(r=>r.text()))}
async function forgetWifi(i){if(!confirm('Dimenticare questa rete?'))return;await api('/api/wifi/forget?index='+i,{method:'POST'});settingsLoaded=false;getState()}
$('mqttForm').onsubmit=async e=>{e.preventDefault();let body=new URLSearchParams({enabled:$('mqen').checked?'1':'0',host:$('mqhost').value,port:$('mqport').value,topic:$('mqtopic').value,user:$('mquser').value,pass:$('mqpass').value});$('mqmsg').textContent=await api('/api/mqtt',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body}).then(r=>r.text());$('mqpass').value='';settingsLoaded=false;getState()}
$('alarmForm').onsubmit=async e=>{e.preventDefault();let body=new URLSearchParams({enabled:$('alen').checked?'1':'0',tmin:$('tmin').value,tmax:$('tmax').value,hmin:$('hmin').value,hmax:$('hmax').value,topic:$('altopic').value});$('almsg').textContent=await api('/api/alarms',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body}).then(r=>r.text());settingsLoaded=false;getState()}
$('nameForm').onsubmit=async e=>{e.preventDefault();if(!confirm('Salvare il nuovo nome e riavviare la stazione?'))return;let body=new URLSearchParams({name:$('devname').value});await api('/api/device',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});setTimeout(()=>location.reload(),5000)}
$('clockstyle').onchange=updateClockPreview;
$('clockForm').onsubmit=async e=>{e.preventDefault();let body=new URLSearchParams({style:$('clockstyle').value});$('clockmsg').textContent=await api('/api/clockstyle',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body}).then(r=>r.text());settingsLoaded=false;getState()}
async function backupConfig(){location.href='/api/config/backup'}
async function restoreConfig(){let f=$('restoreFile').files[0];if(!f){$('restoreMsg').textContent='Seleziona un file JSON';return}if(!confirm('Il ripristino sostituirà configurazione e reti salvate e riavvierà il dispositivo. Continuare?'))return;let txt=await f.text();try{$('restoreMsg').textContent=await api('/api/config/restore',{method:'POST',headers:{'Content-Type':'application/json'},body:txt}).then(r=>r.text());setTimeout(()=>location.reload(),5000)}catch(e){$('restoreMsg').textContent=e.message}}
async function loadLog(){try{let d=await api('/api/log').then(r=>r.json());$('log').innerHTML=d.entries.map(x=>`<div><span class="muted">+${Math.floor(x.ms/1000)}s</span> ${esc(x.text)}</div>`).join('')||'<div>Nessun evento</div>'}catch(e){$('log').textContent=e.message}}
async function clearLog(){if(!confirm('Cancellare il log eventi in RAM?'))return;await api('/api/log/clear',{method:'POST'});loadLog()}
async function loadHistory(){try{let d=await api('/api/history').then(r=>r.json());drawChart(d.points)}catch(e){}}
function drawChart(p){let c=$('chart'),r=c.getBoundingClientRect(),dpr=devicePixelRatio||1;c.width=Math.max(1,r.width*dpr);c.height=Math.max(1,r.height*dpr);let x=c.getContext('2d');x.scale(dpr,dpr);let w=r.width,h=r.height;x.clearRect(0,0,w,h);if(p.length<2){x.fillStyle=getComputedStyle(document.body).color;x.fillText('Storico non ancora sufficiente',12,24);return}let ts=p.map(a=>a.t),hs=p.map(a=>a.h),all=ts.concat(hs),mn=Math.min(...all),mx=Math.max(...all);if(mx-mn<5){mx+=2.5;mn-=2.5}let py=v=>h-20-(v-mn)/(mx-mn)*(h-40),px=i=>10+i/(p.length-1)*(w-20);let cs=getComputedStyle(document.documentElement);x.strokeStyle=cs.getPropertyValue('--line');x.beginPath();for(let i=0;i<5;i++){let y=20+i*(h-40)/4;x.moveTo(10,y);x.lineTo(w-10,y)}x.stroke();function line(vals,col){x.strokeStyle=col;x.lineWidth=2;x.beginPath();vals.forEach((v,i)=>i?x.lineTo(px(i),py(v)):x.moveTo(px(i),py(v)));x.stroke()}line(ts,cs.getPropertyValue('--bad'));line(hs,cs.getPropertyValue('--accent'));x.fillStyle=cs.getPropertyValue('--muted');x.fillText('Temperatura',12,14);x.fillText('Umidità',100,14)}
window.addEventListener('resize',()=>loadHistory());
$('otaForm').onsubmit=e=>{e.preventDefault();let f=$('fwfile').files[0];if(!f)return;let fd=new FormData();fd.append('update',f);$('otaMsg').textContent='Caricamento…';let x=new XMLHttpRequest();x.open('POST','/update');x.upload.onprogress=v=>{if(v.lengthComputable)$('otaMsg').textContent='Caricamento '+Math.round(v.loaded/v.total*100)+'%'};x.onload=()=>$('otaMsg').textContent=x.responseText;x.send(fd)};
(()=>{const blank=Array(80).fill(32);const hello="stazionemeteo";for(let i=0;i<hello.length&&i<20;i++)blank[i]=hello.charCodeAt(i);drawLcd($("lcdCanvas"),blank,[]);updateClockPreview();})();
getState();loadLog();loadHistory();setInterval(getState,1000);
</script></body></html>
)HTML";

String backupConfigJson() {
  DynamicJsonDocument doc(8192);
  doc["schema"] = 1;
  doc["firmware"] = FW_VERSION;
  doc["device"]["name"] = deviceName;
  doc["device"]["clock_style"] = clockStyle;
  doc["game"]["duration_ms"] = nccDefaultMs;
  JsonArray nets = doc.createNestedArray("wifi");
  for (uint8_t i = 0; i < savedWifiCount; ++i) {
    JsonObject n = nets.createNestedObject();
    n["ssid"] = savedWifi[i].ssid;
    n["password"] = savedWifi[i].pass;
  }
  JsonObject mq = doc.createNestedObject("mqtt");
  mq["enabled"] = mqttEnabled; mq["host"] = mqttHost; mq["port"] = mqttPort; mq["user"] = mqttUser; mq["password"] = mqttPass; mq["topic"] = mqttTopic;
  JsonObject al = doc.createNestedObject("alarms");
  al["enabled"] = alarmsEnabled; al["temp_min"] = alarmTempMin; al["temp_max"] = alarmTempMax; al["humidity_min"] = alarmHumidityMin; al["humidity_max"] = alarmHumidityMax; al["topic"] = alarmTopic;
  JsonArray bpms = doc.createNestedArray("music_bpm");
  for (uint8_t i = 0; i < SONG_COUNT; ++i) bpms.add(songBpm[i]);
  String out; serializeJsonPretty(doc, out); return out;
}

bool restoreConfigJson(const String &body, String &error) {
  DynamicJsonDocument doc(8192);
  DeserializationError e = deserializeJson(doc, body);
  if (e) { error = String("JSON non valido: ") + e.c_str(); return false; }
  if ((int)(doc["schema"] | 0) != 1) { error = "Schema backup non supportato"; return false; }

  // Parse and validate EVERYTHING first. Nothing is written to NVS until the
  // complete backup has passed validation, avoiding half-restored settings.
  String newName = sanitizeDeviceName(doc["device"]["name"] | DEFAULT_DEVICE_NAME);
  int newClockStyle = doc["device"]["clock_style"] | (int)clockStyle;
  if (newClockStyle < 0 || newClockStyle > 4) { error = "Stile orologio non valido nel backup"; return false; }
  uint32_t newNccDefaultMs = doc["game"]["duration_ms"] | nccDefaultMs;
  if (newNccDefaultMs < 1000UL || newNccDefaultMs > 5999000UL) { error = "Durata Nomi Cose Citta non valida nel backup"; return false; }

  JsonArray nets = doc["wifi"].as<JsonArray>();
  if (nets.size() > MAX_WIFI_NETWORKS) { error = "Il backup contiene più di 3 reti"; return false; }

  WifiCredential restored[MAX_WIFI_NETWORKS];
  uint8_t restoredCount = 0;
  for (JsonObject n : nets) {
    String ssid = String((const char *)(n["ssid"] | ""));
    String pass = String((const char *)(n["password"] | ""));
    ssid.trim();
    if (!ssid.length()) continue;
    if (ssid.length() > 32) { error = "SSID troppo lungo nel backup"; return false; }
    if (pass.length() > 63) { error = "Password Wi-Fi troppo lunga nel backup"; return false; }

    bool duplicate = false;
    for (uint8_t i = 0; i < restoredCount; ++i) {
      if (restored[i].ssid == ssid) { duplicate = true; break; }
    }
    if (duplicate) continue;
    restored[restoredCount].ssid = ssid;
    restored[restoredCount].pass = pass;
    ++restoredCount;
  }

  bool newMqttEnabled = doc["mqtt"]["enabled"] | false;
  String newMqttHost = String((const char *)(doc["mqtt"]["host"] | ""));
  uint32_t newMqttPortRaw = doc["mqtt"]["port"] | 1883;
  if (newMqttPortRaw < 1 || newMqttPortRaw > 65535) { error = "Porta MQTT non valida nel backup"; return false; }
  uint16_t newMqttPort = (uint16_t)newMqttPortRaw;
  String newMqttUser = String((const char *)(doc["mqtt"]["user"] | ""));
  String newMqttPass = String((const char *)(doc["mqtt"]["password"] | ""));
  String newMqttTopic = String((const char *)(doc["mqtt"]["topic"] | ""));
  if (!newMqttTopic.length()) newMqttTopic = newName + "/sensor";

  bool newAlarmsEnabled = doc["alarms"]["enabled"] | false;
  float newTempMin = doc["alarms"]["temp_min"] | 5.0f;
  float newTempMax = doc["alarms"]["temp_max"] | 35.0f;
  float newHumidityMin = doc["alarms"]["humidity_min"] | 20.0f;
  float newHumidityMax = doc["alarms"]["humidity_max"] | 80.0f;
  if (newTempMin >= newTempMax || newHumidityMin >= newHumidityMax || newHumidityMin < 0 || newHumidityMax > 100) {
    error = "Soglie allarme non valide nel backup";
    return false;
  }
  String newAlarmTopic = String((const char *)(doc["alarms"]["topic"] | ""));
  if (!newAlarmTopic.length()) newAlarmTopic = newName + "/alarm";

  uint16_t restoredBpm[SONG_COUNT];
  for (uint8_t i = 0; i < SONG_COUNT; ++i) restoredBpm[i] = songBpm[i];
  if (doc.containsKey("music_bpm")) {
    JsonArray bpmArray = doc["music_bpm"].as<JsonArray>();
    if (bpmArray.size() != SONG_COUNT) { error = "Numero BPM nel backup non valido"; return false; }
    for (uint8_t i = 0; i < SONG_COUNT; ++i) {
      int value = bpmArray[i] | 120;
      if (value < 40 || value > 300) { error = "BPM fuori intervallo nel backup"; return false; }
      restoredBpm[i] = value;
    }
  }

  // Commit only after every field has been validated.
  saveDeviceName(newName);
  saveClockStyle((uint8_t)newClockStyle);
  nccDefaultMs = newNccDefaultMs;
  nccRemainingMs = nccDefaultMs;
  syncNccSetFromDefault();
  preferences.begin("game", false); preferences.putUShort("seconds", (uint16_t)(nccDefaultMs / 1000UL)); preferences.end();

  savedWifiCount = restoredCount;
  for (uint8_t i = 0; i < restoredCount; ++i) savedWifi[i] = restored[i];
  persistWifiCredentials();

  mqttEnabled = newMqttEnabled;
  mqttHost = newMqttHost;
  mqttPort = newMqttPort;
  mqttUser = newMqttUser;
  mqttPass = newMqttPass;
  mqttTopic = newMqttTopic;
  saveMqttSettings();

  alarmsEnabled = newAlarmsEnabled;
  alarmTempMin = newTempMin;
  alarmTempMax = newTempMax;
  alarmHumidityMin = newHumidityMin;
  alarmHumidityMax = newHumidityMax;
  alarmTopic = newAlarmTopic;
  saveAlarmSettings();

  for (uint8_t i = 0; i < SONG_COUNT; ++i) songBpm[i] = restoredBpm[i];
  saveAllMusicSettings();

  addLog("Configurazione ripristinata da backup");
  return true;
}

String prometheusMetrics() {
  String m; m.reserve(1500);
  m += "stazionemeteo_info{device=\"" + deviceName + "\",firmware=\"" + String(FW_VERSION) + "\"} 1\n";
  m += "# HELP stazionemeteo_temperature_celsius Temperature in Celsius\n# TYPE stazionemeteo_temperature_celsius gauge\n";
  if (!isnan(temperatureC)) m += "stazionemeteo_temperature_celsius " + String(temperatureC, 2) + "\n";
  m += "# HELP stazionemeteo_humidity_percent Relative humidity\n# TYPE stazionemeteo_humidity_percent gauge\n";
  if (!isnan(humidity)) m += "stazionemeteo_humidity_percent " + String(humidity, 2) + "\n";
  m += "stazionemeteo_wifi_connected " + String(WiFi.status() == WL_CONNECTED ? 1 : 0) + "\n";
  m += "stazionemeteo_clock_style " + String(clockStyle) + "\n";
  m += "stazionemeteo_ncc_running " + String(nccRunning ? 1 : 0) + "\n";
  m += "stazionemeteo_ncc_remaining_seconds " + String(nccCurrentRemainingMs() / 1000UL) + "\n";
  m += "stazionemeteo_ncc_used_letters " + String(__builtin_popcount((unsigned int)nccUsedLettersMask)) + "\n";
  m += "stazionemeteo_wifi_rssi_dbm " + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + "\n";
  m += "stazionemeteo_mqtt_connected " + String(mqtt.connected() ? 1 : 0) + "\n";
  m += "stazionemeteo_ap_active " + String(apActive ? 1 : 0) + "\n";
  m += "stazionemeteo_alarm_active " + String((alarmTempLow || alarmTempHigh || alarmHumidityLow || alarmHumidityHigh) ? 1 : 0) + "\n";
  m += "stazionemeteo_uptime_seconds " + String(millis() / 1000) + "\n";
  m += "stazionemeteo_heap_free_bytes " + String(ESP.getFreeHeap()) + "\n";
  m += "stazionemeteo_heap_min_free_bytes " + String(ESP.getMinFreeHeap()) + "\n";
  m += "stazionemeteo_stopwatch_running " + String(stopwatchRunning ? 1 : 0) + "\n";
  m += "stazionemeteo_stopwatch_seconds " + String(stopwatchElapsedMs() / 1000.0f, 2) + "\n";
  m += "stazionemeteo_countdown_running " + String(countdownRunning ? 1 : 0) + "\n";
  m += "stazionemeteo_countdown_remaining_seconds " + String(countdownCurrentRemainingMs() / 1000.0f, 2) + "\n";
  m += "stazionemeteo_countdown_alarm " + String(countdownAlarm ? 1 : 0) + "\n";
  return m;
}

String historyJson() {
  String j; j.reserve(9000); j = "{\"points\":[";
  uint16_t start = (historyHead + HISTORY_POINTS - historyCount) % HISTORY_POINTS;
  for (uint16_t i = 0; i < historyCount; ++i) {
    uint16_t idx = (start + i) % HISTORY_POINTS;
    if (i) j += ",";
    j += "{\"ts\":" + String(history[idx].timestamp) + ",\"t\":" + String(history[idx].temperature, 2) + ",\"h\":" + String(history[idx].humidity, 2) + "}";
  }
  j += "]}"; return j;
}

String logJson() {
  String j; j.reserve(5000); j = "{\"entries\":[";
  uint8_t start = (logHead + LOG_CAPACITY - logCount) % LOG_CAPACITY;
  for (uint8_t i = 0; i < logCount; ++i) {
    uint8_t idx = (start + i) % LOG_CAPACITY;
    if (i) j += ",";
    j += "{\"ms\":" + String(eventLog[idx].uptimeMs) + ",\"text\":\"" + jsonEscape(eventLog[idx].text) + "\"}";
  }
  j += "]}"; return j;
}

void sendNoCacheHeaders() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "0");
}

void sendPortalRedirect() {
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
  server.send(302, "text/plain", "");
}

String stateJson() {
  String j; j.reserve(6500); j = "{";
  j += "\"firmware\":\"" + String(FW_VERSION) + "\",";
  j += "\"temperature_c\":"; j += isnan(temperatureC) ? "null" : String(temperatureC, 2); j += ",";
  j += "\"humidity_pct\":"; j += isnan(humidity) ? "null" : String(humidity, 2); j += ",";
  j += "\"mode\":\"" + uiModeName() + "\",";
  j += "\"home_page\":\"" + String(homePage == HomePage::CLOCK ? "clock" : "weather") + "\",";
  j += "\"ap\":" + String(apActive ? "true" : "false") + ",";
  j += "\"scan_running\":" + String(scanRunning ? "true" : "false") + ",";
  j += "\"device\":{\"name\":\"" + jsonEscape(deviceName) + "\"},";
  j += "\"clock\":{\"style\":" + String(clockStyle) + ",\"name\":\"" + String(clockStyleName(clockStyle)) + "\"},";
  j += "\"rgb\":{\"r\":" + String(rgbStateR) + ",\"g\":" + String(rgbStateG) + ",\"b\":" + String(rgbStateB) + ",\"meaning\":\"" + jsonEscape(rgbMeaning()) + "\"},";
  j += "\"timers\":{";
  j += "\"stopwatch_running\":" + String(stopwatchRunning ? "true" : "false") + ",";
  j += "\"stopwatch_ms\":" + String(stopwatchElapsedMs()) + ",";
  j += "\"stopwatch_lap_ms\":" + String(stopwatchLapMs) + ",";
  j += "\"countdown_running\":" + String(countdownRunning ? "true" : "false") + ",";
  j += "\"countdown_alarm\":" + String(countdownAlarm ? "true" : "false") + ",";
  j += "\"countdown_remaining_ms\":" + String(countdownCurrentRemainingMs()) + ",";
  j += "\"countdown_initial_ms\":" + String(countdownInitialMs) + ",";
  j += "\"ncc_running\":" + String(nccRunning ? "true" : "false") + ",";
  j += "\"ncc_alarm\":" + String(nccAlarm ? "true" : "false") + ",";
  j += "\"ncc_remaining_ms\":" + String(nccCurrentRemainingMs()) + ",";
  j += "\"ncc_default_ms\":" + String(nccDefaultMs) + ",";
  j += "\"ncc_letter\":\"" + String(nccLetter) + "\",";
  j += "\"ncc_used_count\":" + String(__builtin_popcount((unsigned int)nccUsedLettersMask)) + "},";
  j += "\"music\":{\"playing\":" + String(songPlaying ? "true" : "false") + ",\"song\":" + String(currentSong) + ",\"bpm\":" + String(tempo) + "},";
  j += "\"lcd\":[";
  for (uint8_t i = 0; i < LCD_ROWS; ++i) { if (i) j += ","; j += "\"" + jsonEscape(visibleLine[i]) + "\""; }
  j += "],\"lcd_matrix\":{\"graphics\":" + String(lcdGraphicsMode ? "true" : "false") + ",\"cells\":[";
  for (uint8_t row = 0; row < LCD_ROWS; ++row) {
    for (uint8_t col = 0; col < LCD_COLS; ++col) {
      if (row || col) j += ",";
      j += String(lcdCellCodes[row][col]);
    }
  }
  j += "],\"custom\":[";
  for (uint8_t g = 0; g < 8; ++g) {
    if (g) j += ",";
    j += "[";
    for (uint8_t y = 0; y < 8; ++y) { if (y) j += ","; j += String(lcdCustomGlyphs[g][y]); }
    j += "]";
  }
  j += "]},\"buttons\":[";
  for (uint8_t i = 1; i <= 4; ++i) {
    if (i > 1) j += ",";
    bool enabled = uiMode != UiMode::OTA;
    if (songPlaying && i == 3) enabled = false;
    if (uiMode == UiMode::AP_MODE && i == 1) enabled = false;
    j += "{\"id\":" + String(i) + ",\"title\":\"" + jsonEscape(buttonTitle(i)) + "\",\"hint\":\"" + jsonEscape(buttonHint(i)) + "\",\"enabled\":" + String(enabled ? "true" : "false") + "}";
  }
  j += "],";
  j += "\"wifi\":{\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",\"ssid\":\"" + jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "") + "\",\"ip\":\"" + String(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "") + "\",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + "},";
  j += "\"saved\":["; for (uint8_t i = 0; i < savedWifiCount; ++i) { if (i) j += ","; j += "\"" + jsonEscape(savedWifi[i].ssid) + "\""; } j += "],";
  j += "\"mqtt\":{\"enabled\":" + String(mqttEnabled ? "true" : "false") + ",\"connected\":" + String(mqtt.connected() ? "true" : "false") + ",\"host\":\"" + jsonEscape(mqttHost) + "\",\"port\":" + String(mqttPort) + ",\"topic\":\"" + jsonEscape(mqttTopic) + "\",\"user\":\"" + jsonEscape(mqttUser) + "\"},";
  bool alarmActive = alarmTempLow || alarmTempHigh || alarmHumidityLow || alarmHumidityHigh;
  j += "\"alarms\":{\"enabled\":" + String(alarmsEnabled ? "true" : "false") + ",\"active\":" + String(alarmActive ? "true" : "false") + ",\"summary\":\"" + jsonEscape(alarmSummary()) + "\",\"temp_min\":" + String(alarmTempMin,1) + ",\"temp_max\":" + String(alarmTempMax,1) + ",\"humidity_min\":" + String(alarmHumidityMin,1) + ",\"humidity_max\":" + String(alarmHumidityMax,1) + ",\"topic\":\"" + jsonEscape(alarmTopic) + "\"},";
  uint32_t sensorAge = lastDhtRead ? (millis() - lastDhtRead) / 1000 : 0;
  j += "\"diag\":{\"uptime_s\":" + String(millis()/1000) + ",\"free_heap\":" + String(ESP.getFreeHeap()) + ",\"min_free_heap\":" + String(ESP.getMinFreeHeap()) + ",\"reset_reason\":\"" + resetReasonName(bootResetReason) + "\",\"wake_reason\":\"" + wakeCauseName(bootWakeCause) + "\",\"ap_clients\":" + String(apActive ? WiFi.softAPgetStationNum() : 0) + ",\"sensor_age_s\":" + String(sensorAge) + ",\"watchdog\":" + String(watchdogRegistered ? "true" : "false") + "}";
  j += "}"; return j;
}
String networksJson() {
  String j;
  j.reserve(2048);
  j = "{\"running\":" + String(scanRunning ? "true" : "false") + ",\"networks\":[";
  for (uint8_t i = 0; i < scanCount; ++i) {
    if (i) j += ",";
    j += "{\"ssid\":\"" + jsonEscape(scanItems[i].ssid) + "\",";
    j += "\"rssi\":" + String(scanItems[i].rssi) + ",";
    j += "\"open\":" + String(scanItems[i].auth == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  j += "]}";
  return j;
}

void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    sendNoCacheHeaders();
    server.send_P(200, "text/html; charset=utf-8", WEB_PAGE);
  });

  server.on("/api/state", HTTP_GET, []() {
    sendNoCacheHeaders();
    server.send(200, "application/json", stateJson());
  });

  server.on("/metrics", HTTP_GET, []() {
    sendNoCacheHeaders();
    server.send(200, "text/plain; version=0.0.4; charset=utf-8", prometheusMetrics());
  });

  server.on("/api/history", HTTP_GET, []() {
    sendNoCacheHeaders();
    server.send(200, "application/json", historyJson());
  });

  server.on("/api/log", HTTP_GET, []() {
    sendNoCacheHeaders();
    server.send(200, "application/json", logJson());
  });

  server.on("/api/log/clear", HTTP_POST, []() {
    logHead = 0; logCount = 0;
    server.send(200, "text/plain", "Log cancellato");
  });

  server.on("/api/config/backup", HTTP_GET, []() {
    sendNoCacheHeaders();
    server.sendHeader("Content-Disposition", "attachment; filename=stazionemeteo-backup.json");
    server.send(200, "application/json", backupConfigJson());
  });

  server.on("/api/config/restore", HTTP_POST, []() {
    String error;
    if (!restoreConfigJson(server.arg("plain"), error)) {
      server.send(400, "text/plain", error);
      return;
    }
    server.send(200, "text/plain", "Configurazione ripristinata. Riavvio...");
    delay(250);
    ESP.restart();
  });

  server.on("/api/button", HTTP_POST, []() {
    int id = server.arg("id").toInt();
    if (id < 1 || id > 4) {
      server.send(400, "text/plain", "Tasto non valido");
      return;
    }
    handleButton((uint8_t)id);
    server.send(200, "text/plain", "OK");
  });

  server.on("/api/wifi/scan", HTTP_POST, []() {
    if (scanRunning) { server.send(409, "text/plain", "Scansione già in corso"); return; }
    if (wifiConnecting) { server.send(409, "text/plain", "Connessione Wi-Fi in corso"); return; }
    startWifiScan();
    server.send(202, "text/plain", "Scansione avviata su richiesta");
  });

  server.on("/api/wifi/networks", HTTP_GET, []() {
    sendNoCacheHeaders();
    server.send(200, "application/json", networksJson());
  });

  server.on("/api/wifi/forget", HTTP_POST, []() {
    int idx = server.arg("index").toInt();
    if (idx < 0 || idx >= savedWifiCount) { server.send(400, "text/plain", "Indice rete non valido"); return; }
    String forgotten = savedWifi[idx].ssid;
    for (uint8_t i = idx; i + 1 < savedWifiCount; ++i) savedWifi[i] = savedWifi[i + 1];
    --savedWifiCount;
    persistWifiCredentials();
    addLog("Rete dimenticata: " + forgotten);
    server.send(200, "text/plain", "Rete rimossa");
  });

  server.on("/api/wifi/connect", HTTP_POST, []() {
    int idx = server.arg("index").toInt();
    if (idx < 0 || idx >= scanCount) {
      server.send(400, "text/plain", "Rete non valida");
      return;
    }
    String pass = server.arg("password");
    if (scanItems[idx].auth != WIFI_AUTH_OPEN && pass.length() < 8) {
      server.send(400, "text/plain", "Password troppo corta");
      return;
    }
    startWifiConnection(scanItems[idx].ssid, pass, true, false);
    server.send(202, "text/plain", "Connessione avviata. Se riesce, l'AP verra disattivato.");
  });

  server.on("/api/mqtt", HTTP_POST, []() {
    mqttEnabled = server.arg("enabled") == "1";
    mqttHost = server.arg("host");
    uint32_t port = server.arg("port").toInt();
    mqttPort = (port > 0 && port <= 65535) ? (uint16_t)port : 1883;
    mqttTopic = server.arg("topic");
    mqttUser = server.arg("user");
    String newPass = server.arg("pass");
    if (newPass.length()) mqttPass = newPass;
    if (!mqttTopic.length()) mqttTopic = "stazionemeteo/sensor";
    saveMqttSettings();
    addLog("Configurazione MQTT aggiornata");
    server.send(200, "text/plain", "Configurazione MQTT salvata");
  });

  server.on("/api/alarms", HTTP_POST, []() {
    float tmin = server.arg("tmin").toFloat(), tmax = server.arg("tmax").toFloat();
    float hmin = server.arg("hmin").toFloat(), hmax = server.arg("hmax").toFloat();
    if (tmin >= tmax || hmin >= hmax || hmin < 0 || hmax > 100) { server.send(400, "text/plain", "Soglie non valide"); return; }
    alarmsEnabled = server.arg("enabled") == "1";
    alarmTempMin = tmin; alarmTempMax = tmax; alarmHumidityMin = hmin; alarmHumidityMax = hmax;
    alarmTopic = server.arg("topic"); if (!alarmTopic.length()) alarmTopic = deviceName + "/alarm";
    saveAlarmSettings(); addLog("Configurazione allarmi aggiornata");
    server.send(200, "text/plain", "Allarmi salvati");
  });

  server.on("/api/device", HTTP_POST, []() {
    String name = sanitizeDeviceName(server.arg("name"));
    if (name.length() < 1) { server.send(400, "text/plain", "Nome non valido"); return; }
    saveDeviceName(name); addLog("Nome dispositivo modificato: " + deviceName);
    server.send(200, "text/plain", "Nome salvato. Riavvio...");
    delay(250); ESP.restart();
  });

  server.on("/api/clockstyle", HTTP_POST, []() {
    int style = server.arg("style").toInt();
    if (style < 0 || style > 4) { server.send(400, "text/plain", "Stile non valido"); return; }
    saveClockStyle((uint8_t)style);
    if (uiMode == UiMode::HOME && homePage == HomePage::CLOCK) lastScreenRefresh = 0;
    server.send(200, "text/plain", String("Stile applicato: ") + clockStyleName(clockStyle));
  });

  server.on("/update", HTTP_POST,
    []() {
      bool failed = Update.hasError();
      server.sendHeader("Connection", "close");
      server.send(200, "text/plain", failed ? "Aggiornamento FALLITO" : "Aggiornamento riuscito. Riavvio...");
      if (!failed) {
        addLog("Aggiornamento OTA completato");
        delay(250);
        ESP.restart();
      } else {
        uiMode = WiFi.status() == WL_CONNECTED ? UiMode::HOME : UiMode::AP_MODE;
      }
    },
    []() {
      HTTPUpload &upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        uiMode = UiMode::OTA;
        stopSong();
        uiMode = UiMode::OTA;
        setScreenLine(0, "Aggiornamento OTA");
        setScreenLine(1, upload.filename);
        setScreenLine(2, "Preparazione...");
        setScreenLine(3, "");
        serviceDisplay();
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
        setScreenLine(2, "Ricevuti: " + String(upload.totalSize + upload.currentSize) + " B");
      } else if (upload.status == UPLOAD_FILE_END) {
        if (!Update.end(true)) Update.printError(Serial);
        setScreenLine(2, Update.hasError() ? "Errore OTA" : "OTA completato");
        setScreenLine(3, Update.hasError() ? "Controlla seriale" : "Riavvio...");
      }
    }
  );

  // Common captive-portal probes.
  server.on("/generate_204", HTTP_ANY, sendPortalRedirect);
  server.on("/hotspot-detect.html", HTTP_ANY, sendPortalRedirect);
  server.on("/connecttest.txt", HTTP_ANY, sendPortalRedirect);
  server.on("/ncsi.txt", HTTP_ANY, sendPortalRedirect);
  server.onNotFound([]() {
    if (apActive) sendPortalRedirect();
    else server.send(404, "text/plain", "Not found");
  });

  server.begin();
}

// ============================ SLEEP ============================
void goToDeepSleep() {
  addLog("Richiesto deep sleep");
  stopCountdownAlarm();
  stopSong();
  mqtt.disconnect();

  setScreenLine(0, "Sospensione...");
  setScreenLine(1, "Rilascia il tasto");
  setScreenLine(2, "Rosso per riattivare");
  setScreenLine(3, "");
  serviceDisplay();

  // GPIO15 is the original wake button. Wait for release before sleeping,
  // otherwise ext0 level-low wake would immediately wake the ESP32 again.
  uint32_t releaseWait = millis();
  while (digitalRead(BUTTON4) == LOW && millis() - releaseWait < 5000) {
    server.handleClient();
    if (apActive) dnsServer.processNextRequest();
    delay(10);
  }
  if (digitalRead(BUTTON4) == LOW) {
    // A stuck/held wake pin would cause an immediate wake loop. Abort sleep
    // instead and leave the station operational.
    setScreenLine(0, "Sleep annullato");
    setScreenLine(1, "Tasto rosso premuto");
    setScreenLine(2, "Rilascia e riprova");
    setScreenLine(3, "");
    uiMode = (WiFi.status() == WL_CONNECTED) ? UiMode::HOME : UiMode::AP_MODE;
    restoreStatusColor();
    return;
  }

  if (apActive) dnsServer.stop();
  server.stop();
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  btStop();

  digitalWrite(STATUS_LED, LOW);
  colora(0, 0, 0);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Deep sleep");
  delay(100);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_15, 0);
  esp_deep_sleep_start();
}

// ============================ SETUP / LOOP ============================
void setup() {
  Serial.begin(115200);
  Serial.println();
  bootResetReason = esp_reset_reason();
  bootWakeCause = esp_sleep_get_wakeup_cause();
  Serial.println("stazionemeteo avvio...");

  lcd.begin(LCD_COLS, LCD_ROWS);
  lcd.clear();
  for (uint8_t row = 0; row < LCD_ROWS; ++row) {
    for (uint8_t col = 0; col < LCD_COLS; ++col) lcdCellCodes[row][col] = LCD_SPACE;
  }
  for (uint8_t g = 0; g < 8; ++g) for (uint8_t y = 0; y < 8; ++y) lcdCustomGlyphs[g][y] = 0;

  pinMode(BUTTON1, INPUT_PULLUP);
  pinMode(BUTTON2, INPUT_PULLUP);
  pinMode(BUTTON3, INPUT_PULLUP);
  pinMode(BUTTON4, INPUT_PULLUP);
  initializeButtonStates();

  pinMode(RGB_R, OUTPUT);
  pinMode(RGB_G, OUTPUT);
  pinMode(RGB_B, OUTPUT);
  pinMode(STATUS_LED, OUTPUT);
  colora(1, 1, 0);

  dht_sensor.begin();
  buzzerBegin();

  loadDeviceSettings();
  loadWifiCredentials();
  loadMqttSettings();
  loadMusicSettings();
  homePage = HomePage::WEATHER;
  homePageSince = millis();

  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  WiFi.setHostname(deviceName.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  setupWebServer();

  addLog("Boot firmware " + String(FW_VERSION) + " reset=" + resetReasonName(bootResetReason) + " wake=" + wakeCauseName(bootWakeCause));
  setScreenLine(0, deviceName);
  setScreenLine(1, "Avvio...");
  setScreenLine(2, String(savedWifiCount) + " WiFi memorizzate");
  setScreenLine(3, "");
  serviceDisplay();

  // No automatic Wi-Fi scan: try the known networks directly in MRU order.
  if (savedWifiCount == 0) startAccessPoint();
  else startSavedWifiSequence();

  // Arduino-ESP32 2.0.14 uses the ESP-IDF 4.x TWDT API.
  esp_err_t wdtInit = esp_task_wdt_init(15, true);
  if (wdtInit == ESP_OK || wdtInit == ESP_ERR_INVALID_STATE) {
    watchdogRegistered = (esp_task_wdt_add(NULL) == ESP_OK);
  }
}

void loop() {
  if (watchdogRegistered) esp_task_wdt_reset();
  server.handleClient();
  if (apActive) dnsServer.processNextRequest();

  serviceButtons();
  serviceSensor();
  processWifi();
  serviceTimers();
  serviceSongPlayer();
  serviceBpmPersistence();
  serviceMqtt();
  updateScreenModel();
  serviceDisplay();

  if (WiFi.status() == WL_CONNECTED && millis() - lastStatusLedToggle >= 1000) {
    lastStatusLedToggle = millis();
    statusLedState = !statusLedState;
    digitalWrite(STATUS_LED, statusLedState);
  } else if (WiFi.status() != WL_CONNECTED) {
    digitalWrite(STATUS_LED, LOW);
  }

  delay(1);
}
