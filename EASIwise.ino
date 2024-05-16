#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <NTP.h>

// uncomment below for Serial debugging
//#define SERIALBAUD 115200

// duration in ms to keep logging active
#define LOGGING_DURATION 900000

const char* ewgc_ssid = "EWGC_Easiwise";
const char* ewgc_pass = "12345678";

const char* wifi_ssid = "your_wifi";
const char* wifi_pass = "secretpassword";

const char* mqtt_host = "192.168.1.50";
const char* mqtt_user = "mqtt_user";
const char* mqtt_pass = "mqtt_pass";
const int mqtt_port = 1883;



#ifdef SERIALBAUD
  #define Serialprint(...)  Serial.print(__VA_ARGS__)
  #define Serialprintf(...)  Serial.printf(__VA_ARGS__)
  #define Serialprintln(...)  Serial.println(__VA_ARGS__)
#else
  #define Serialprint(...)
  #define Serialprintf(...)
  #define Serialprintln(...)
#endif

WiFiUDP wifiUDP;
NTP ntp(wifiUDP);
WiFiClient espClient;
PubSubClient mqtt_client(espClient);
ESP8266WebServer webserver(80);
JsonDocument ewgc_index, ewgc_timer;

String ewgc_request_text;
uint8_t ewgc_request_type = 0;
bool got_ewgc_info = false;
unsigned long suspend_logging = LOGGING_DURATION, wifi_timeout_count = 0, ewgc_timeout_count = 0, ewgc_next_check = 60000, ewgc_time_update = 720;
char device_fullmac[13], topic_lwt[25], topic_get[25], topic_info[25], topic_temp[25], topic_act[25];
enum e_stages : byte {EWGC_CONNECT, EWGC_CONNECTING, WIFI_CONNECT, WIFI_CONNECTING};
e_stages stage = WIFI_CONNECT;
enum e_logoptions : byte {NORMAL, FORCE, ERROR};



void setup() {
#ifdef SERIALBAUD
  Serial.begin(SERIALBAUD);
  Serial.println();
  for (uint8_t t = 3; t > 0; t--) { Serial.flush(); delay(1000); }
  //Serial.setDebugOutput(true);
#endif

  byte mac[6];
  char wifi_host_name[12], device_halfmac[7];
  WiFi.macAddress(mac);
  sprintf(device_halfmac, "%02X%02X%02X", mac[3], mac[4], mac[5]);
  sprintf(device_fullmac, "%.2x%.2x%.2x%.2x%.2x%.2x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  sprintf(wifi_host_name, "ewgc_%s", device_halfmac);
  sprintf(topic_act, "ewgc/%s/act", device_fullmac);
  sprintf(topic_lwt, "ewgc/%s/lwt", device_fullmac);
  sprintf(topic_get, "ewgc/%s/get", device_fullmac);
  sprintf(topic_info, "ewgc/%s/info", device_fullmac);
  sprintf(topic_temp, "ewgc/%s/temp", device_fullmac);

  SPIFFS.begin();
  SPIFFS.format();
  FSInfo fs_info;
  SPIFFS.info(fs_info);
  File file = SPIFFS.open("/log.txt","w");
  if (file) {
    file.printf("Firmware compiled: %s\n", __DATE__ " " __TIME__);
    file.printf("Device Hostname: %s\n", wifi_host_name);
    file.printf("Device MAC: %s\n", WiFi.macAddress().c_str());
    file.printf("Geyser SSID: %s\n", ewgc_ssid);
    file.printf("SPIFFS size: %d\n", fs_info.totalBytes-fs_info.usedBytes);
    file.print("Last reboot reason: ");
    file.println(ESP.getResetReason());
    file.println();
    file.close();
  }

  WiFi.disconnect(true);
  WiFi.setAutoConnect(false);
  WiFi.setAutoReconnect(true);
  WiFi.hostname(wifi_host_name);
  WiFi.mode(WIFI_STA);

  mqtt_client.setCallback(mqtt_callback);
  mqtt_client.setServer(mqtt_host, mqtt_port);

  ntp.timeZone(2);
  ntp.isDST(false);
  ntp.begin();

  setupWebServer();
}



void loop() {
  static wl_status_t last_wifi_status = WL_IDLE_STATUS;
  bool seconds_changed = false, minutes_changed = false;
  static unsigned long seconds = 0, minutes = 0, wifi_timeout = 0, mqtt_reconnect_time = 0, ewgc_request_delay = 0;

  if (millis() / 1000 > seconds) {
    seconds = millis() / 1000;
    seconds_changed = true;
    if (seconds / 60 > minutes) {
      minutes = seconds / 60;
      minutes_changed = true;
      if (minutes > 14400) ESP.restart(); // reboot after 10 days
    }
  }

  if (millis() > ewgc_next_check) {
    ewgc_next_check = millis() + 300000;
    mqtt_client.disconnect();
    WiFi.disconnect();
    stage = EWGC_CONNECT;
  }

  if (last_wifi_status != WiFi.status()) {
    last_wifi_status = WiFi.status();
    if (last_wifi_status == WL_CONNECTED) {
      Serialprintf("[WiFi] Connected to %s (%s) Got IP ", WiFi.SSID().c_str(), WiFi.BSSIDstr().c_str());
      Serialprintln(WiFi.localIP());
      stage = WIFI_CONNECT;
    } else {
      Serialprintf("[WiFi] -> %s\n", wl_status_text(last_wifi_status));
    }
    if (ewgc_timeout_count > 0 || wifi_timeout_count > 0) {
      writeLog(NORMAL, "[WiFi] " + String(wl_status_text(last_wifi_status)));
    }
  }

  if (last_wifi_status == WL_CONNECTED) {

    if (WiFi.SSID() == String(ewgc_ssid)) {
      ewgc_timeout_count = 0;
      if (ewgc_request_type > 0) {
        ewgcSendInfo();
        ewgc_request_delay = millis() + 3000;
      } else if (millis() > ewgc_request_delay) {
        ewgc_request_delay = 0;
        ewgcReadInfo();
        WiFi.disconnect();
      }

    } else if (WiFi.SSID() == String(wifi_ssid)) {
      wifi_timeout_count = 0;
      
      if (!mqtt_client.loop() && millis() > mqtt_reconnect_time) {
        mqtt_reconnect_time = millis() + 15000;
        Serialprintf("[MQTT] connecting to server %s\n", mqtt_host);
        if (mqtt_client.connect(device_fullmac, mqtt_user, mqtt_pass, topic_lwt, 1, true, "offline")) {
          mqtt_discovery();
          mqtt_client.subscribe("homeassistant/status");
          mqtt_client.subscribe(topic_act);
          mqtt_client.subscribe(topic_get);
          mqtt_client.subscribe(topic_temp);
          mqtt_client.publish(topic_lwt, "online", true);
          Serialprintln("[MQTT] connected to server!");
        } else { 
          writeLog(ERROR, "[MQTT] Failed to connect"); 
        }

      } else if (got_ewgc_info) {
        got_ewgc_info = false;
        JsonDocument json_post;
        char buffer[512], datetime[18], tt[8];
        json_post["GT"] = ewgc_index["GT"];
        json_post["GBT"] = ewgc_index["GBT"];
        json_post["GON"] = ewgc_index["GON"];
        json_post["Bst"] = ewgc_index["Bst"];
        json_post["Hol"] = ewgc_index["Hol"];
        json_post["UPD"] = ewgc_index["UPD"];
        json_post["PV"] = ewgc_index["PV"];
        json_post["fault"] = (("1"==ewgc_index["GTE"]) || ("1"==ewgc_index["EME"]) || ("1"==ewgc_index["WLE"]) || ("1"==ewgc_index["OTE"]) || ("1"==ewgc_index["SPE"]) || ("1"==ewgc_index["S1E"]) || ("1"==ewgc_index["S2E"])) ? 1 : 0;

        snprintf(datetime, sizeof(datetime), "20%s/%02s/%02s %02s:%02s", (const char*)ewgc_index["YY"], (const char*)ewgc_index["MM"], (const char*)ewgc_index["DD"], (const char*)ewgc_index["hh"], (const char*)ewgc_index["mm"]);
        json_post["time"] = datetime;

        json_post["T1t"] = ewgc_timer["T1t"];
        snprintf(tt, sizeof(tt), "%02s:%02s", (const char*)ewgc_timer["T1hn"], (const char*)ewgc_timer["T1mn"]);
        json_post["T1s"] = tt;
        snprintf(tt, sizeof(tt), "%02s:%02s", (const char*)ewgc_timer["T1hf"], (const char*)ewgc_timer["T1mf"]);
        json_post["T1f"] = tt;
        
        json_post["T2t"] = ewgc_timer["T2t"];
        snprintf(tt, sizeof(tt), "%02s:%02s", (const char*)ewgc_timer["T2hn"], (const char*)ewgc_timer["T2mn"]);
        json_post["T2s"] = tt;
        snprintf(tt, sizeof(tt), "%02s:%02s", (const char*)ewgc_timer["T2hf"], (const char*)ewgc_timer["T2mf"]);
        json_post["T2f"] = tt;

        json_post["T3t"] = ewgc_timer["T3t"];
        snprintf(tt, sizeof(tt), "%02s:%02s", (const char*)ewgc_timer["T3hn"], (const char*)ewgc_timer["T3mn"]);
        json_post["T3s"] = tt;
        snprintf(tt, sizeof(tt), "%02s:%02s", (const char*)ewgc_timer["T3hf"], (const char*)ewgc_timer["T3mf"]);
        json_post["T3f"] = tt;

        json_post["T4t"] = ewgc_timer["T4t"];
        snprintf(tt, sizeof(tt), "%02s:%02s", (const char*)ewgc_timer["T4hn"], (const char*)ewgc_timer["T4mn"]);
        json_post["T4s"] = tt;
        snprintf(tt, sizeof(tt), "%02s:%02s", (const char*)ewgc_timer["T4hf"], (const char*)ewgc_timer["T4mf"]);
        json_post["T4f"] = tt;

        size_t n = serializeJson(json_post, buffer);
        mqtt_client.publish(topic_info, buffer, n);
        Serialprintf("[MQTT] Sent Info: %s\n", buffer);

      } else if (seconds_changed && ntp.ntp() == 0) {
        if (ntp.update()) {
          ntp.updateInterval(600000);
          writeLog(NORMAL, "Booted " + String(seconds) + " seconds ago");
        }

      } else if (minutes_changed) {
        if (ntp.update() && minutes > ewgc_time_update) {
          ewgc_time_update = minutes + 1440; // 24 hours
          char buff[64];
          snprintf(buff, sizeof(buff), "setdt?YY=%d&MM=%d&DD=%d&hh=%d&mm=%d", ntp.year()-2000, ntp.month(), ntp.day(), ntp.hours(), ntp.minutes());
          ewgc_request_text = String(buff);
          ewgc_request_type = 1;
          ewgc_next_check = 0;
        }
      }

    } else { writeLog(ERROR, "[LOST] SSID: " + WiFi.SSID()); }

    webserver.handleClient();

  } else { // not connected to WiFi
    switch (stage) {
      case EWGC_CONNECT:  // connect to EWGC
        WiFi.begin(ewgc_ssid, ewgc_pass);
        Serialprintf("[WiFi] connecting to %s\n", ewgc_ssid);
        wifi_timeout = millis() + 10000;
        stage = EWGC_CONNECTING;
        break;

      case EWGC_CONNECTING: // EWGC check for timeout, back to LAN
        if (millis() > wifi_timeout) {
          ewgc_timeout_count++;
          writeLog(ERROR, "[WiFi] timeout connecting to " + String(ewgc_ssid) + " Failed attempts: " + String(ewgc_timeout_count));
          if (ewgc_timeout_count <= 5) {
            ewgc_next_check = millis() + 120000;
          } else {
            ewgc_next_check = millis() + 600000;
          }
          stage = WIFI_CONNECT;
        }
        break;

      case WIFI_CONNECT: // connect to LAN
        WiFi.begin(wifi_ssid, wifi_pass);
        Serialprintf("[WiFi] connecting to %s\n", wifi_ssid);
        wifi_timeout = millis() + 15000;
        stage = WIFI_CONNECTING;
        break;

      case WIFI_CONNECTING: // LAN timeout, try again
        if (millis() > wifi_timeout) {
          wifi_timeout_count++;
          writeLog(ERROR, "[WiFi] timeout connecting to " + String(wifi_ssid) + " Failed attempts: " + String(wifi_timeout_count));
          stage = WIFI_CONNECT;
        }
        break;
      
    }
  }
  
  if (minutes_changed) {
    if (suspend_logging > 0 && millis() > suspend_logging) {
      suspend_logging = 0;
      writeLog(FORCE, "[LOGGING] Disabled");
    }
  }
}



void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  String txt;
  for (int i = 0; i < length; i++) { txt.concat((char)payload[i]); }
  if (strcmp(topic, "homeassistant/status") == 0) {
    mqtt_client.publish(topic_lwt, "online", true);
  } else if (strcmp(topic, topic_act) == 0) {
    if (txt == "refresh") ewgc_next_check = 0;
  } else if (strcmp(topic, topic_get) == 0) {
    ewgc_request_text = txt;
    ewgc_request_type = 1;
    ewgc_next_check = 0;
  } else if (strcmp(topic, topic_temp) == 0) {
    ewgc_request_text = txt;
    ewgc_request_type = 2;
    ewgc_next_check = 0;
  }
}



void ewgcSendInfo() {
  uint32_t now_millis = millis();
  String url;
  WiFiClient this_wifi;
  HTTPClient this_http;
  this_http.useHTTP10(true);

  if (1 == ewgc_request_type) {
    url = "http://192.168.4.1/" + ewgc_request_text;
    writeLog(FORCE, "[EWGC] Sending: " + url);
    if (this_http.begin(this_wifi, url)) {
      this_http.GET();
      writeLog(NORMAL, "[EWGC] Received: " + this_http.getString());
      this_http.end();
    }

  } else if (2 == ewgc_request_type) {
    url = "http://192.168.4.1/settimer?T1t=" + ewgc_request_text + "&T2t=" + ewgc_request_text + "&T3t=" + ewgc_request_text + "&T4t=" + ewgc_request_text;
    writeLog(FORCE, "[EWGC] Sending: " + url);
    if (this_http.begin(this_wifi, url)) {
      this_http.GET();
      writeLog(NORMAL, "[EWGC] Received: " + this_http.getString());
      this_http.end();
    }
    url = "http://192.168.4.1/settemp?GBT=" + ewgc_request_text;
    writeLog(FORCE, "[EWGC] Sending: " + url);
    if (this_http.begin(this_wifi, url)) {
      this_http.GET();
      writeLog(NORMAL, "[EWGC] Received: " + this_http.getString());
      this_http.end();
    }

  }
  ewgc_request_text = "";
  ewgc_request_type = 0;
  writeLog(FORCE, "[EWGC] Send Info " + String(millis()- now_millis) + "ms");
}



void ewgcReadInfo() {
  int httpCode;
  uint32_t now_millis = millis();
  WiFiClient this_wifi;
  HTTPClient this_http;
  this_http.useHTTP10(true);
  ewgc_index.clear();
  ewgc_timer.clear();
  Serialprintln("[HTTP] Requesting Geyser Info");

  if (this_http.begin(this_wifi, "http://192.168.4.1/index")) {  
    httpCode = this_http.GET();
    if (httpCode == HTTP_CODE_OK) {
      Serialprintln("[HTTP] Got response for /index");
      deserializeJson(ewgc_index, this_http.getStream());
    } else {
      writeLog(ERROR, "[HTTP] GET /index failed: " + this_http.errorToString(httpCode));
    }
    this_http.end();
  }
  if (this_http.begin(this_wifi, "http://192.168.4.1/timer")) {  
    httpCode = this_http.GET();
    if (httpCode == HTTP_CODE_OK) {
      Serialprintln("[HTTP] Got response for /timer");
      deserializeJson(ewgc_timer, this_http.getStream());
    } else {
      writeLog(ERROR, "[HTTP] GET /timer failed: " + this_http.errorToString(httpCode));
    }
    this_http.end();
  }

  got_ewgc_info = ewgc_index.size() > 0 && ewgc_timer.size() > 0;
  writeLog(NORMAL, "[EWGC] Read Info " + String(millis()- now_millis) + "ms");
}


void writeLog(e_logoptions opt, String txt) {
  if (opt == ERROR) {
    suspend_logging = millis() + LOGGING_DURATION;
    Serialprintln("[LOGGING] Enabled");
  }
  if (suspend_logging > 0 || opt == FORCE) {
    File file = SPIFFS.open("/log.txt", "a");
    if (file) {
      if (ntp.ntp() > 0)
        file.print(ntp.formattedTime("[%F %T] "));
      file.println(txt);
      file.close();
    }
  }
  Serialprintln(txt);
}



void mqtt_discovery() {
  char topic[60], payload[512];
  JsonDocument device, available, thing;

  device["ids"].add(device_fullmac);
  device["name"] = "EASIwise";
  device["mdl"] = "Bridge";
  device["cu"] = "http://" + WiFi.localIP().toString();
  available["t"] = topic_lwt;

  thing.clear();
  thing["uniq_id"] = String(device_fullmac) + "_temp";
  thing["dev"] = device;
  thing["name"] = "Temperature";
  thing["stat_t"] = topic_info;
  thing["val_tpl"] = "{{value_json.GT}}";
  thing["json_attr_t"] = thing["stat_t"];
  thing["avty"] = available;
  thing["dev_cla"] = "temperature";
  thing["stat_cla"] = "measurement";
  thing["unit_of_meas"] = "°C";
  serializeJson(thing, payload);
  snprintf(topic, sizeof(topic), "homeassistant/sensor/%s_temp/config", device_fullmac);
  mqtt_client.publish(topic, payload, true);
  delay(50);

  device.remove("name");
  device.remove("mdl");
  device.remove("cu");

  thing.clear();
  thing["uniq_id"] = String(device_fullmac) + "_tempmax";
  thing["dev"] = device;
  thing["name"] = "Max Temp";
  thing["stat_t"] = topic_info;
  thing["val_tpl"] = "{{value_json.GBT}}";
  thing["avty"] = available;
  thing["dev_cla"] = "temperature";
  thing["stat_cla"] = "measurement";
  thing["ent_cat"] = "config";
  thing["cmd_t"] = topic_temp;
  thing["min"] = "30";
  thing["max"] = "65";
  thing["step"] = "5";
  thing["unit_of_meas"] = "°C";
  serializeJson(thing, payload);
  snprintf(topic, sizeof(topic), "homeassistant/number/%s_tempmax/config", device_fullmac);
  mqtt_client.publish(topic, payload, true);
  delay(50);

  thing.clear();
  thing["uniq_id"] = String(device_fullmac) + "_power";
  thing["dev"] = device;
  thing["name"] = "Power";
  thing["stat_t"] = topic_info;
  thing["val_tpl"] = "{{value_json.GON}}";
  thing["avty"] = available;
  thing["pl_on"] = "1";
  thing["pl_off"] = "0";
  serializeJson(thing, payload);
  snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/%s_power/config", device_fullmac);
  mqtt_client.publish(topic, payload, true);
  delay(50);

  thing.clear();
  thing["uniq_id"] = String(device_fullmac) + "_boost";
  thing["dev"] = device;
  thing["name"] = "Boost";
  thing["stat_t"] = topic_info;
  thing["val_tpl"] = "{{value_json.Bst}}";
  thing["avty"] = available;
  thing["pl_on"] = "1";
  thing["pl_off"] = "0";
  serializeJson(thing, payload);
  snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/%s_boost/config", device_fullmac);
  mqtt_client.publish(topic, payload, true);
  delay(50);

  thing.clear();
  thing["uniq_id"] = String(device_fullmac) + "_holiday";
  thing["dev"] = device;
  thing["name"] = "Holiday Mode";
  thing["stat_t"] = topic_info;
  thing["val_tpl"] = "{{value_json.Hol}}";
  thing["avty"] = available;
  thing["pl_on"] = "1";
  thing["pl_off"] = "0";
  serializeJson(thing, payload);
  snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/%s_holiday/config", device_fullmac);
  mqtt_client.publish(topic, payload, true);
  delay(50);

  thing.clear();
  thing["uniq_id"] = String(device_fullmac) + "_fault";
  thing["dev"] = device;
  thing["name"] = "Geyser Fault";
  thing["stat_t"] = topic_info;
  thing["val_tpl"] = "{{value_json.fault}}";
  thing["avty"] = available;
  thing["pl_on"] = "1";
  thing["pl_off"] = "0";
  thing["dev_cla"] = "problem";
  thing["ent_cat"] = "diagnostic";
  serializeJson(thing, payload);
  snprintf(topic, sizeof(topic), "homeassistant/binary_sensor/%s_fault/config", device_fullmac);
  mqtt_client.publish(topic, payload, true);
  delay(50);

  thing.clear();
  thing["uniq_id"] = String(device_fullmac) + "_btn_boost";
  thing["dev"] = device;
  thing["name"] = "Toggle Boost";
  thing["cmd_t"] = topic_get;
  thing["pl_prs"] = "boost";
  thing["avty"] = available;
  serializeJson(thing, payload);
  snprintf(topic, sizeof(topic), "homeassistant/button/%s_btn_boost/config", device_fullmac);
  mqtt_client.publish(topic, payload, true);
  delay(50);

  thing.clear();
  thing["uniq_id"] = String(device_fullmac) + "_btn_holiday";
  thing["dev"] = device;
  thing["name"] = "Toggle Holiday";
  thing["cmd_t"] = topic_get;
  thing["pl_prs"] = "holiday";
  thing["avty"] = available;
  serializeJson(thing, payload);
  snprintf(topic, sizeof(topic), "homeassistant/button/%s_btn_holiday/config", device_fullmac);
  mqtt_client.publish(topic, payload, true);
  delay(50);

  thing.clear();
  thing["uniq_id"] = String(device_fullmac) + "_btn_refresh";
  thing["dev"] = device;
  thing["name"] = "Refresh";
  thing["cmd_t"] = topic_act;
  thing["pl_prs"] = "refresh";
  thing["avty"] = available;
  serializeJson(thing, payload);
  snprintf(topic, sizeof(topic), "homeassistant/button/%s_btn_refresh/config", device_fullmac);
  mqtt_client.publish(topic, payload, true);
  delay(50);
}



void setupWebServer() {
  webserver.on("/", HTTP_GET, []() {
    webserver.sendHeader("Connection", "close");
    webserver.send(200, "text/html", "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'></head><body><div style='text-align:center'><input type='button' value='View Logs' onclick=\"location='/log'\" /><br /><br /><input type='button' value='Reboot' onclick=\"location='/reboot'\" /><br /><br /><input type='button' value='Upload Firmware' onclick=\"location='/fw'\" /></div></body></html>");
  });
  webserver.on("/log", HTTP_GET, []() {
    webserver.sendHeader("Connection", "close");
    File file = SPIFFS.open("/log.txt", "r");
    if (file) {
      webserver.streamFile(file, "text/plain");
      file.close();
    } else {
      webserver.send(404, "text/html", "Log file not found");
    }
  });
  webserver.on("/reboot", HTTP_GET, []() {
    webserver.sendHeader("Connection", "close");
    webserver.send(200, "text/html", "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='20;URL=/'></head><body><div style='text-align:center'>Rebooting. Please wait...</div></body></html>");
    delay(100);
    ESP.restart();
  });
  webserver.on("/fw", HTTP_GET, []() {
    webserver.sendHeader("Connection", "close");
    webserver.send(200, "text/html", "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'></head><body><div style='text-align:center'><form method='POST' action='/fw' enctype='multipart/form-data'><input type='file' name='update'><input type='submit' value='Update'></form></div></body></html>");
  });
  webserver.on("/fw", HTTP_POST, []() {
    char txt[512];
    snprintf(txt, sizeof(txt), "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='20;URL=/'></head><body><div style='text-align:center'>Update result: %s<br />Rebooting. Please wait</div></body></html>", Update.hasError() ? "FAILED" : "SUCCESS");
    webserver.sendHeader("Connection", "close");
    webserver.send(200, "text/html", txt);
    delay(100);
    ESP.restart();
  },[]() {
    HTTPUpload& upload = webserver.upload();
    if (upload.status == UPLOAD_FILE_START) {
#ifdef SERIALBAUD
      Serial.setDebugOutput(true);
#endif
      WiFiUDP::stopAll();
      Serialprintf("Update: %s\n", upload.filename.c_str());
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
      if (!Update.begin(maxSketchSpace)) {  // start with max available size
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {  // true to set the size to the current progress
        Serialprintf("Update Success: %u\nRebooting...\n", upload.totalSize);
      }
    }
    yield();
  });
  webserver.begin();
}



const char* wl_status_text(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD: return "WL_NO_SHIELD";
    case WL_IDLE_STATUS: return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL: return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "WL_SCAN_COMPLETED";
    case WL_CONNECTED: return "WL_CONNECTED";
    case WL_CONNECT_FAILED: return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED: return "WL_DISCONNECTED";
  }
  return "UNKNOWN";
}
