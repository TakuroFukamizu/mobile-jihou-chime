/**
 * Jihou
 * 
 * @Hardwares: M5AtomS3R
 * @Platform Version: Arduino ESP32 Board Manager v2.1.3
 * @Libraries: M5Unified, EspEasyUtils
 */

#include <WiFi.h>

// Different versions of the framework have different SNTP header file names and availability.
#if __has_include (<esp_sntp.h>)
  #include <esp_sntp.h>
  #define SNTP_ENABLED 1
#elif __has_include (<sntp.h>)
  #include <sntp.h>
  #define SNTP_ENABLED 1
#endif

#ifndef SNTP_ENABLED
#define SNTP_ENABLED 0
#endif

#include <M5Unified.h>
#include <SD.h>
#include <EspEasyTask.h>
#include "env.h"

//-------------------------------

struct __attribute__((packed)) wav_header_t
{
  char RIFF[4];
  uint32_t chunk_size;
  char WAVEfmt[8];
  uint32_t fmt_chunk_size;
  uint16_t audiofmt;
  uint16_t channel;
  uint32_t sample_rate;
  uint32_t byte_per_sec;
  uint16_t block_size;
  uint16_t bit_per_sample;
};

struct __attribute__((packed)) sub_chunk_t
{
  char identifier[4];
  uint32_t chunk_size;
  uint8_t data[1];
};

//-------------------------------

static constexpr const size_t buf_num = 3;
static constexpr const size_t buf_size = 1024;
static uint8_t wav_data[buf_num][buf_size];

uint8_t prev_hour = 99;

bool ntp_synced = false;
unsigned long ntp_last_synced = 0;

EspEasyTask jihouTask;
EspEasyTask ntpSyncTask;

//-------------------------------

unsigned long getTime();

void setupRtcByWifi();
bool playFile(const char* filename);

void jihouTaskFunc();
void ntpSyncTaskFunc();

//-------------------------------

void setup() {
  auto cfg = M5.config();
  cfg.external_rtc  = true;  // default=false. use Unit RTC.
  cfg.external_speaker.atomic_spk = true;
  M5.begin(cfg);

  M5.Display.setEpdMode(m5gfx::epd_fastest);
  M5.setLogDisplayIndex(0);
  
  if (!M5.Rtc.isEnabled()) {
    M5.Log.println("RTC not found.");
    for (;;) { M5.delay(500); }
  }
  M5.Log.println("RTC found.");

  // setup RTC ( NTP auto setting )
  configTzTime(NTP_TIMEZONE, NTP_SERVER1, NTP_SERVER2, NTP_SERVER3);
  
  M5.Log.print("WiFi: ");
  for (int i = 20; i && WiFi.status() != WL_CONNECTED; --i) {
    M5.Log.print(".");
    M5.delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    M5.Log.println("Connected.");
  } else {
    //Init WiFi as Station, start SmartConfig
    WiFi.mode(WIFI_AP_STA);
    WiFi.beginSmartConfig();
    M5.Log.println("Not connected.\r\nSmartConfig ON.");
  }

  setupRtcByWifi();
  
  // setup SPK module
  auto spkCfg = M5.Speaker.config();
  M5.Speaker.config(spkCfg);
  M5.Speaker.begin();

  M5.Log.print("SD:");
  auto pin_sck = M5.getPin(m5::pin_name_t::sd_spi_sclk);
  auto pin_miso = M5.getPin(m5::pin_name_t::sd_spi_miso);
  auto pin_mosi = M5.getPin(m5::pin_name_t::sd_spi_mosi);
  auto pin_ss = M5.getPin(m5::pin_name_t::sd_spi_ss);
  SPI.begin(pin_sck, pin_miso, pin_mosi, pin_ss);
  if (!SD.begin(pin_ss, SPI, 24000000)) {
    M5.Log.println("Card Mount Failed");
    return;
  }

  M5.Log.println("Check SPK");
  M5.Speaker.setVolume(128);
  /// ピポッ
  M5.Speaker.tone(2000, 100, 0, true);
  M5.Speaker.tone(1000, 100, 0, false);

  jihouTask.begin(jihouTaskFunc, 10, ESP_EASY_TASK_CPU_NUM);
  ntpSyncTask.begin(ntpSyncTaskFunc, 2, ESP_EASY_TASK_CPU_NUM);

  M5.Display.clear();
}

void loop(void) {
  static constexpr const char* const wd[7] = {"日","月","火","水","木","金","土"};

  M5.delay(10);
  M5.update();

  M5.Display.setTextSize(1);

  // show date/time
  // auto dt = M5.Rtc.getDateTime();
  // uint16_t year = dt.date.year;
  // uint8_t month = dt.date.month, date = dt.date.date, weekDay = dt.date.weekDay;
  // uint8_t hour = dt.time.hours, min = dt.time.minutes, sec = dt.time.seconds;
  auto t = time(nullptr);
  auto tm = localtime(&t); // for local timezone.
  uint16_t year = tm->tm_year+1900;
  uint8_t month = tm->tm_mon+1, date = tm->tm_mday, weekDay = tm->tm_wday;
  uint8_t hour = tm->tm_hour, min = tm->tm_min, sec = tm->tm_sec;
  M5.Display.setCursor(4,8);
  M5.Display.setFont(&fonts::lgfxJapanMincho_16);
  M5.Log.printf("%04d/%02d/%02d (%s)", year, month, date, wd[weekDay]);

  M5.Display.setCursor(4,44);
  M5.Display.setFont(&fonts::Font7);
  M5.Display.setTextSize(0.8);
  M5.Log.printf("%02d:%02d", hour, min);
  M5.Display.setCursor(116,70);
  M5.Display.setTextSize(0.2);
  M5.Log.printf("%02d", sec);
  
  // show status
  M5.Display.setFont(&fonts::lgfxJapanGothic_12);
  M5.Display.setCursor(0,100);
  M5.Display.setTextSize(1);
  M5.Log.printf("NTP: %s\r\n", ntp_synced ? "同期済" : "未同期");
  M5.Log.printf("時報: %s\r\n", M5.Speaker.isPlaying() ? "実行" : "停止");

  if (M5.BtnA.wasDoubleClicked()) { // force NTP sync
    setupRtcByWifi();
  }
}

//-------------------------------

void jihouTaskFunc() {
  while(true) {
    auto dt = M5.Rtc.getDateTime();
    if(dt.time.hours != prev_hour && dt.time.minutes == 0 && dt.time.seconds < 10) {
      prev_hour = dt.time.hours;
      M5.Speaker.setVolume(128);
      playFile(AUDIO_FILE01);
    }
    M5.delay(500);
  }
}

void ntpSyncTaskFunc() {
  while(true) {
    if (!WiFi.smartConfigDone()) {
      // not wifi config yet...
      M5.delay(1000);
      continue;
    }
    if (getTime() - ntp_last_synced < 6 * 60 * 60) {
      M5.delay(1000);
      continue;
    }
    setupRtcByWifi();
    M5.delay(1000);
  }
}

//-------------------------------

// Function that gets current epoch time
unsigned long getTime() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return(0);
  }
  time(&now);
  return now;
}

void setupRtcByWifi() {
  M5.Display.clear();
  M5.Display.setCursor(0,0);

  M5.Log.print("WiFi:");
  for (int i = 20; i && WiFi.status() != WL_CONNECTED; --i) {
    M5.Log.print(".");
    M5.delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    M5.Log.println("\r\nWiFi Connected.");
    M5.Log.printf("IP: %s\r\n", WiFi.localIP());
    M5.Log.print("NTP:");
#if SNTP_ENABLED
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
      M5.Log.print(".");
      M5.delay(1000);
    }
#else
    M5.delay(1600);
    struct tm timeInfo;
    while (!getLocalTime(&timeInfo, 1000)){
      M5.Log.print('.');
    };
#endif
    M5.Log.println("\r\nNTP Connected.");

    time_t t = time(nullptr)+1; // Advance one second.
    while (t > time(nullptr));  /// Synchronization in seconds
    M5.Rtc.setDateTime( gmtime( &t ) );
    ntp_synced = true;
    ntp_last_synced = getTime();
  } else {
    M5.Log.println("\r\nWiFi none...");
  }

  // WiFi.disconnect(true);
  M5.Display.clear();
}

//-------------------------------

bool playFile(const char* filename) {
  auto file = SD.open(filename);
  if (!file) { // file not found or sdcard error
    return false; 
  }

  wav_header_t wav_header;
  file.read((uint8_t*)&wav_header, sizeof(wav_header_t));

  if ( memcmp(wav_header.RIFF,    "RIFF",     4)
    || memcmp(wav_header.WAVEfmt, "WAVEfmt ", 8)
    || wav_header.audiofmt != 1
    || wav_header.bit_per_sample < 8
    || wav_header.bit_per_sample > 16
    || wav_header.channel == 0
    || wav_header.channel > 2
    ) {
    file.close();
    return false;
  }

  file.seek(offsetof(wav_header_t, audiofmt) + wav_header.fmt_chunk_size);
  sub_chunk_t sub_chunk;

  file.read((uint8_t*)&sub_chunk, 8);

  while(memcmp(sub_chunk.identifier, "data", 4)) {
    if (!file.seek(sub_chunk.chunk_size, SeekMode::SeekCur)) { break; }
    file.read((uint8_t*)&sub_chunk, 8);
  }

  if (memcmp(sub_chunk.identifier, "data", 4)) {
    file.close();
    return false;
  }

  int32_t data_len = sub_chunk.chunk_size;
  bool flg_16bit = (wav_header.bit_per_sample >> 4);

  size_t idx = 0;
  while (data_len > 0) {
    size_t len = data_len < buf_size ? data_len : buf_size;
    len = file.read(wav_data[idx], len);
    data_len -= len;

    if (flg_16bit) {
      M5.Speaker.playRaw((const int16_t*)wav_data[idx], len >> 1, wav_header.sample_rate, wav_header.channel > 1, 1, 0);
    } else {
      M5.Speaker.playRaw((const uint8_t*)wav_data[idx], len, wav_header.sample_rate, wav_header.channel > 1, 1, 0);
    }
    idx = idx < (buf_num - 1) ? idx + 1 : 0;
  }
  file.close();

  return true;
}
