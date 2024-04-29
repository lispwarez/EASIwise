// Need to change MQTT_MAX_PACKET_SIZE to 512 in PubSubClient.h

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
//#define SERIALDEBUG 115200

const char* ewgc_ssid = "EWGC_Easiwise";
const char* ewgc_pass = "12345678";

const char* wifi_ssid = "your_wifi";
const char* wifi_pass = "secretpassword";

const char* mqtt_host = "192.168.1.50";
const char* mqtt_user = "mqtt_username";
const char* mqtt_pass = "mqtt_password";
const int mqtt_port = 1883;



#ifdef SERIALDEBUG
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
JsonDocument json_ewgc;

String ewgc_request_text;
uint8_t ewgc_request_type = 0;
char device_fullmac[13];
char topic_lwt[25], topic_get[25], topic_info[25], topic_temp[25];
unsigned long ewgc_next_check = 0, ewgc_offline_counter = 0, ewgc_time_update = 600;
bool waiting_for_time = true, got_ewgc_info = false;
const char fw_compile_date[] = __DATE__ " " __TIME__;
enum e_stages {NONE, EWGC_CONNECT, EWGC_CONNECTING, WIFI_CONNECT, WIFI_CONNECTING};
e_stages stage = NONE;


void writeLog(String txt) {
  File file = SPIFFS.open("/log.txt", "a");
  if (file) {
    if (!waiting_for_time)
      file.print(ntp.formattedTime("[%F %T] "));
    file.println(txt);
    file.close();
  }
  Serialprintln(txt);
}


void setup() {
#ifdef SERIALDEBUG
  Serial.begin(SERIALDEBUG);
  Serial.println();
  for (uint8_t t = 3; t > 0; t--) { Serial.flush(); delay(1000); }
  //Serial.setDebugOutput(true);
#endif

  byte mac[6];
  char wifi_host_name[12], device_halfmac[7];
  WiFi.macAddress(mac);
  sprintf(device_halfmac, "%.2x%.2x%.2x", mac[3], mac[4], mac[5]);
  sprintf(device_fullmac, "%.2x%.2x%.2x%.2x%.2x%.2x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  sprintf(topic_lwt, "ewgc/%s/lwt", device_fullmac);
  sprintf(topic_get, "ewgc/%s/get", device_fullmac);
  sprintf(topic_info, "ewgc/%s/info", device_fullmac);
  sprintf(topic_temp, "ewgc/%s/temp", device_fullmac);
  sprintf(wifi_host_name, "ewgc_%s", device_halfmac);

  SPIFFS.begin();
  if (SPIFFS.exists("/log.txt")) SPIFFS.remove("/log.txt");
  File file = SPIFFS.open("/log.txt","w");
  if (file) {
    file.printf("Firmware compiled: %s\n", fw_compile_date);
    file.printf("Device Hostname: %s\n", wifi_host_name);
    file.printf("Device MAC: %s\n", device_fullmac);
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


void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  String txt;
  for (int i = 0; i < length; i++) { txt.concat((char)payload[i]); }
  if (strcmp(topic, "homeassistant/status") == 0) {
    mqtt_client.publish(topic_lwt, "online", true);
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
    writeLog("[EWGC] Sending: " + url);
    if (this_http.begin(this_wifi, url)) {
      this_http.GET();
      writeLog("[EWGC] received: " + this_http.getString());
      this_http.end();
    }

  } else if (2 == ewgc_request_type) {
    url = "http://192.168.4.1/settimer?T1t=" + ewgc_request_text + "&T2t=" + ewgc_request_text + "&T3t=" + ewgc_request_text + "&T4t=" + ewgc_request_text;
    writeLog("[EWGC] Sending: " + url);
    if (this_http.begin(this_wifi, url)) {
      this_http.GET();
      writeLog("[EWGC] received: " + this_http.getString());
      this_http.end();
    }
    url = "http://192.168.4.1/settemp?GBT=" + ewgc_request_text;
    writeLog("[EWGC] Sending: " + url);
    if (this_http.begin(this_wifi, url)) {
      this_http.GET();
      writeLog("[EWGC] received: " + this_http.getString());
      this_http.end();
    }

  }
  ewgc_request_text = "";
  ewgc_request_type = 0;
  writeLog("[EWGC] Send Info took " + String(millis()- now_millis));
}

void ewgcReadInfo() {
  uint32_t now_millis = millis();
  WiFiClient this_wifi;
  HTTPClient this_http;
  this_http.useHTTP10(true);

  Serialprintln("[HTTP] Requesting Geyser Info");
  if (this_http.begin(this_wifi, "http://192.168.4.1/index")) {  
    int httpCode = this_http.GET();
    if (httpCode == HTTP_CODE_OK) {
      Serialprintln("[HTTP] Received response");
      json_ewgc.clear();
      deserializeJson(json_ewgc, this_http.getStream());
      got_ewgc_info = true;
    } else {
      writeLog("[HTTP] GET failed with error: " + this_http.errorToString(httpCode));
    }
    this_http.end();
  }
  writeLog("[EWGC] Read Info took " + String(millis()- now_millis));
}



void loop() {
  bool seconds_changed = false;
  static wl_status_t last_wifi_status = WL_IDLE_STATUS;
  static unsigned long seconds = 0, wifi_timeout = 0, mqtt_reconnect_time = 0, ewgc_request_delay = 0;

  if (millis() / 1000 > seconds) {
    seconds = millis() / 1000;
    seconds_changed = true;
    if (seconds > 864000) ESP.restart(); // reboot after 10 days

    if (seconds > ewgc_next_check) {
      ewgc_next_check = seconds + 300;
      mqtt_client.disconnect();
      WiFi.disconnect();
      stage = EWGC_CONNECT;
    }
  }

  if (last_wifi_status != WiFi.status()) {
    last_wifi_status = WiFi.status();
    if (last_wifi_status == WL_CONNECTED) {
      Serialprintf("[WiFi] Connected to %s Got IP ", WiFi.SSID().c_str());
      Serialprintln(WiFi.localIP());
    } else {
      Serialprint("[WiFi] Status changed to: ");
      Serialprintln(last_wifi_status);
    }
  }

  if (last_wifi_status == WL_CONNECTED) {

    if (WiFi.SSID() == String(ewgc_ssid)) {
      ewgc_offline_counter = 0;
      if (ewgc_request_type > 0) {
        ewgcSendInfo();
        ewgc_request_delay = millis() + 3000;
      } else if (millis() > ewgc_request_delay) {
        ewgc_request_delay = 0;
        ewgcReadInfo();
        stage = WIFI_CONNECT;
        WiFi.disconnect();
      }

    } else if (WiFi.SSID() == String(wifi_ssid)) {
      if (!mqtt_client.loop()) {
        if (millis() > mqtt_reconnect_time) {
          Serialprintf("[MQTT] connecting to server %s\n", mqtt_host);
          mqtt_reconnect_time = millis() + 15000;
          if (mqtt_client.connect(device_fullmac, mqtt_user, mqtt_pass, topic_lwt, 1, true, "offline")) {
            mqtt_discovery();
            mqtt_client.subscribe("homeassistant/status");
            mqtt_client.subscribe(topic_get);
            mqtt_client.subscribe(topic_temp);
            mqtt_client.publish(topic_lwt, "online", true);
            Serialprintln("[MQTT] connected to server!");
          } else { 
            writeLog("[MQTT] Failed connection"); 
          }
        }
      } else if (got_ewgc_info) {
        got_ewgc_info = false;
        JsonDocument json_post;
        char buffer[512], datetime[18];
        sprintf(datetime, "20%s/%02s/%02s %02s:%02s", (const char*)json_ewgc["YY"], (const char*)json_ewgc["MM"], (const char*)json_ewgc["DD"], (const char*)json_ewgc["hh"], (const char*)json_ewgc["mm"]);
        json_post["GT"] = json_ewgc["GT"];
        json_post["GBT"] = json_ewgc["GBT"];
        json_post["GON"] = json_ewgc["GON"];
        json_post["Bst"] = json_ewgc["Bst"];
        json_post["Hol"] = json_ewgc["Hol"];
        json_post["PV"] = json_ewgc["PV"];
        json_post["fault"] = (("1"==json_ewgc["GTE"]) || ("1"==json_ewgc["EME"]) || ("1"==json_ewgc["WLE"]) || ("1"==json_ewgc["OTE"]) || ("1"==json_ewgc["SPE"]) || ("1"==json_ewgc["S1E"]) || ("1"==json_ewgc["S2E"])) ? 1 : 0;
        json_post["time"] = datetime;
        size_t n = serializeJson(json_post, buffer);
        mqtt_client.publish(topic_info, buffer, n);
        Serialprintf("[MQTT] Sent Info: %s\n", buffer);
      }
    }

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
          ewgc_offline_counter++;
          writeLog("[WiFi] timeout connecting to " + String(ewgc_ssid) + " Failed attempts: " + String(ewgc_offline_counter));
          if (ewgc_offline_counter <= 5) {
            ewgc_next_check = seconds + 120;
          } else {
            ewgc_next_check = seconds + 600;
          }
          stage = WIFI_CONNECT;
        }
        break;

      case WIFI_CONNECT: // connect to LAN
        WiFi.begin(wifi_ssid, wifi_pass);
        Serialprintf("[WiFi] connecting to %s\n", wifi_ssid);
        wifi_timeout = millis() + 30000;
        stage = WIFI_CONNECTING;
        break;

      case WIFI_CONNECTING: // LAN timeout, try again
        if (millis() > wifi_timeout) {
          Serialprintf("[WiFi] timeout connecting to %s\n", ewgc_ssid);
          stage = WIFI_CONNECT;
        }
        break;
    }
  }

  if (seconds_changed) {
    if (waiting_for_time) {
      if (ntp.update()) {
        waiting_for_time = false;
        ntp.updateInterval(600000); // milliseconds
        writeLog("Booted " + String(seconds) + " seconds ago");
      }
    } else {
      ntp.update();
      if (seconds > ewgc_time_update) {
        ewgc_time_update = seconds + 86400; // 24 hours
        char buff[64];
        sprintf(buff, "setdt?DD=%d&MM=%d&YY=%d&hh=%d&mm=%d", ntp.day(), ntp.month(), ntp.year()-2000, ntp.hours(), ntp.minutes());
        ewgc_request_text = String(buff);
        ewgc_request_type = 1;
        ewgc_next_check = 0;
      }
    }
  }
  
  webserver.handleClient();
}


void mqtt_discovery() {
  JsonDocument device, available, sensor_temp, sensor_tmax, binary_power, binary_boost, binary_holiday, binary_fault, button_boost, button_holiday;
  char discovery_topic[60], payload[512];
  device["ids"].add(device_fullmac);
  device["name"] = "EASIwise";
  device["mdl"] = "Bridge";
  device["cu"] = "http://" + WiFi.localIP().toString();
  available["t"] = topic_lwt;

  sensor_temp["uniq_id"] = String(device_fullmac) + "_temp";
  sensor_temp["dev"] = device;
  sensor_temp["name"] = "Temperature";
  sensor_temp["stat_t"] = topic_info;
  sensor_temp["val_tpl"] = "{{value_json.GT}}";
  sensor_temp["json_attr_t"] = sensor_temp["stat_t"];
  sensor_temp["avty"] = available;
  sensor_temp["dev_cla"] = "temperature";
  sensor_temp["stat_cla"] = "measurement";
  sensor_temp["unit_of_meas"] = "°C";
  serializeJson(sensor_temp, payload);
  sprintf(discovery_topic, "homeassistant/sensor/%s_temp/config", device_fullmac);
  mqtt_client.publish(discovery_topic, payload, true);
  delay(100);

  device.remove("name");
  device.remove("mdl");
  device.remove("cu");

  sensor_tmax["uniq_id"] = String(device_fullmac) + "_tempmax";
  sensor_tmax["dev"] = device;
  sensor_tmax["name"] = "Max Temp";
  sensor_tmax["stat_t"] = topic_info;
  sensor_tmax["val_tpl"] = "{{value_json.GBT}}";
  sensor_tmax["avty"] = available;
  sensor_tmax["dev_cla"] = "temperature";
  sensor_tmax["stat_cla"] = "measurement";
  sensor_tmax["ent_cat"] = "config";
  sensor_tmax["cmd_t"] = topic_temp;
  sensor_tmax["min"] = "30";
  sensor_tmax["max"] = "65";
  sensor_tmax["unit_of_meas"] = "°C";
  serializeJson(sensor_tmax, payload);
  sprintf(discovery_topic, "homeassistant/number/%s_tempmax/config", device_fullmac);
  mqtt_client.publish(discovery_topic, payload, true);
  delay(100);

  binary_power["uniq_id"] = String(device_fullmac) + "_power";
  binary_power["dev"] = device;
  binary_power["name"] = "Power";
  binary_power["stat_t"] = topic_info;
  binary_power["val_tpl"] = "{{value_json.GON}}";
  binary_power["avty"] = available;
  binary_power["pl_on"] = "1";
  binary_power["pl_off"] = "0";
  serializeJson(binary_power, payload);
  sprintf(discovery_topic, "homeassistant/binary_sensor/%s_power/config", device_fullmac);
  mqtt_client.publish(discovery_topic, payload, true);
  delay(100);

  binary_boost["uniq_id"] = String(device_fullmac) + "_boost";
  binary_boost["dev"] = device;
  binary_boost["name"] = "Boost";
  binary_boost["stat_t"] = topic_info;
  binary_boost["val_tpl"] = "{{value_json.Bst}}";
  binary_boost["avty"] = available;
  binary_boost["pl_on"] = "1";
  binary_boost["pl_off"] = "0";
  serializeJson(binary_boost, payload);
  sprintf(discovery_topic, "homeassistant/binary_sensor/%s_boost/config", device_fullmac);
  mqtt_client.publish(discovery_topic, payload, true);
  delay(100);

  binary_holiday["uniq_id"] = String(device_fullmac) + "_holiday";
  binary_holiday["dev"] = device;
  binary_holiday["name"] = "Holiday Mode";
  binary_holiday["stat_t"] = topic_info;
  binary_holiday["val_tpl"] = "{{value_json.Hol}}";
  binary_holiday["avty"] = available;
  binary_holiday["pl_on"] = "1";
  binary_holiday["pl_off"] = "0";
  serializeJson(binary_holiday, payload);
  sprintf(discovery_topic, "homeassistant/binary_sensor/%s_holiday/config", device_fullmac);
  mqtt_client.publish(discovery_topic, payload, true);
  delay(100);

  binary_fault["uniq_id"] = String(device_fullmac) + "_fault";
  binary_fault["dev"] = device;
  binary_fault["name"] = "Geyser Fault";
  binary_fault["stat_t"] = topic_info;
  binary_fault["val_tpl"] = "{{value_json.fault}}";
  binary_fault["avty"] = available;
  binary_fault["pl_on"] = "1";
  binary_fault["pl_off"] = "0";
  binary_fault["dev_cla"] = "problem";
  binary_fault["ent_cat"] = "diagnostic";
  serializeJson(binary_fault, payload);
  sprintf(discovery_topic, "homeassistant/binary_sensor/%s_fault/config", device_fullmac);
  mqtt_client.publish(discovery_topic, payload, true);
  delay(100);

  button_boost["uniq_id"] = String(device_fullmac) + "_btn_boost";
  button_boost["dev"] = device;
  button_boost["name"] = "Toggle Boost";
  button_boost["cmd_t"] = topic_get;
  button_boost["pl_prs"] = "boost";
  button_boost["avty"] = available;
  serializeJson(button_boost, payload);
  sprintf(discovery_topic, "homeassistant/button/%s_btn_boost/config", device_fullmac);
  mqtt_client.publish(discovery_topic, payload, true);
  delay(100);

  button_holiday["uniq_id"] = String(device_fullmac) + "_btn_holiday";
  button_holiday["dev"] = device;
  button_holiday["name"] = "Toggle Holiday";
  button_holiday["cmd_t"] = topic_get;
  button_holiday["pl_prs"] = "holiday";
  button_holiday["avty"] = available;
  serializeJson(button_holiday, payload);
  sprintf(discovery_topic, "homeassistant/button/%s_btn_holiday/config", device_fullmac);
  mqtt_client.publish(discovery_topic, payload, true);
  delay(100);
}




void setupWebServer() {
  webserver.on("/", HTTP_GET, []() {
    webserver.sendHeader("Connection", "close");
    webserver.send(200, "text/html", "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'></head><body><div style='text-align:center'><input type='button' value='View Logs' onclick=\"location='/log'\" /><br /><br /><input type='button' value='Reboot' onclick=\"location='/reboot'\" /><br /><br /><input type='button' value='Upload Firmware' onclick=\"location='/fw'\" /></div></body></html>");
  });
  webserver.on("/log", HTTP_GET, []() {
    File file = SPIFFS.open("/log.txt", "r");
    webserver.sendHeader("Connection", "close");
    size_t sent = webserver.streamFile(file, "text/plain");
    file.close();
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
    sprintf(txt, "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='20;URL=/'></head><body><div style='text-align:center'>Update result: %s<br />Rebooting. Please wait</div></body></html>", Update.hasError() ? "FAILED" : "SUCCESS");
    webserver.sendHeader("Connection", "close");
    webserver.send(200, "text/html", txt);
    delay(100);
    ESP.restart();
  },[]() {
    HTTPUpload& upload = webserver.upload();
    if (upload.status == UPLOAD_FILE_START) {
#ifdef SERIALDEBUG
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

