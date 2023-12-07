/*
Wired Daikin
Reads and writes daikin data through serial interface on S21 connector
OTA update accepted
remoteDebug available
webpage available (with websockets)
Publishes data on mqtt and on http page
Receives command on mqtt and on http page

Main activities are implemented with a states-machine to limit blocking in code

v.1.1
moving under config:
- boolean to enable http control/info
- boolean to enable mqtt control/info
- mqtt topics

Consequents modification to code to manage conditional enabling of reporting channels

Added some debugging commands for remoteDebug

Also moved under struct the local state vars, for cleaner code
*/
#include <Arduino.h>
#include <ESP8266WiFi.h>
#define ARDUINOJSON_USE_LONG_LONG 1
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <ezTime.h>
#include <EEPROM.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include "RemoteDebug.h"
#include <SoftwareSerial.h>
#include <ESPAsyncTCP.h>
#include "ESPAsyncWebServer.h"
#include "AsyncJson.h"
#include <ESPAsyncWiFiManager.h>
#include <LittleFS.h>
#include <FS.h>

/* Useful Constants */
#define SECS_PER_MIN  (60UL)
#define SECS_PER_HOUR (3600UL)
#define SECS_PER_DAY  (86400UL)
/* Useful Macros for getting elapsed time */
#define numberOfSeconds(_time_) (_time_ % SECS_PER_MIN)  
#define numberOfMinutes(_time_) ((_time_ / SECS_PER_MIN) % SECS_PER_MIN)
#define numberOfHours(_time_) (( _time_% SECS_PER_DAY) / SECS_PER_HOUR)
#define numberOfDays(_time_) ( _time_ / SECS_PER_DAY) 

const char* apname = "WiFi-daikin"; //name used for config AP when wifi is not found

//ac variables
const uint8_t modes[6] = {'1','2','3','4','6'}; // auto, dry, cool, heat, fan -> in ASCII 49 50 51 52 54
const uint8_t speeds[6] = {'A','3','4','5','6','7'}; //auto and speed from 1 to 5 -> in ASCII 65 51 52 53 54 55
std::uint8_t modeToChar (uint8_t mode){
  switch (mode){
    case 49:
      return modes[0];
    case 50:
      return modes[1];
    case 51:
      return modes[2];
    case 52:
      return modes[3];
    case 54:
      return modes[4];
    default:
      return 0;
  }
}
std::uint8_t fanToChar (uint8_t speed){
  switch (speed){
    case 65:
      return speeds[0];
    case 51:
      return speeds[1];
    case 52:
      return speeds[2];
    case 53:
      return speeds[3];
    case 54:
      return speeds[4];
    case 55:
      return speeds[5];
    default:
      return 0;
  }
}
//turns climate mode val to string
std::string mode_to_string(uint8_t mode) {
  switch (mode) {
    case '0': //it seems it reports 0 when set to auto (1)
    case '1':
      return "Auto";
    case '2':
      return "Dry";
    case '3':
      return "Cool";
    case '4':
      return "Heat";
    case '6':
      return "Fan";
    default:
      return "UNKNOWN";
  }
}
//turns fan mode val to string
std::string speed_to_string(uint8_t mode) {
  switch (mode) {
    case 'A':
      return "Auto";
    case '3':
      return "1";
    case '4':
      return "2";
    case '5':
      return "3";
    case '6':
      return "4";
    case '7':
      return "5";
    default:
      return "UNKNOWN";
  }
}

//useful ac serial chars
#define STX 2
#define ETX 3
#define ACK 6
#define NAK 21

//variables to store AC values
struct {
  bool power_on = false;
  uint8_t mode = '1';
  uint8_t fan = 'A';
  int16_t setpoint = 270;
  bool swing_v = false;
  bool swing_h = false;
  int16_t temp_inside = 0;
  int16_t temp_outside = 0;
  int16_t temp_coil = 0;
  uint16_t fan_rpm = 0;
  bool idle = true;
} acValues;

//variable and consts for states-machine
const std::vector<std::string> acQueries = {"F1", "F5", "RH", "RI", "Ra", "RL", "Rd"}; //list of good used ac queries
uint8_t state = 0, cmdState = 0; //machine state indexes
uint8_t acQuery = 0; //ac query index
uint32_t updateStartTime = 0, serialTimeoutStart = 0, waitTimer = 0; //used to calculate update time
const uint8_t serialTimeout = 100, waitTimeout = 10; //timeout waiting for serial byte or for next command
std::vector<uint8_t> frameBytes = {}; //buffer for frame reading
bool frameReading = false, waiting = false, STXreceived = false, valueChanged = false; //needed when reading a full frame or waiting for next command or checking STX byte
uint8_t serialByte = 0; //buffer for single bytes read
uint8_t frameChecksum = 0, calcChecksum = 0;
std::vector<uint8_t> acCommand = {}; //vector to hold commands
bool resetNeeded = false; //used to recall the need for a reset when changing relevant settings

//general vars
WiFiClient espClient;
PubSubClient mqttClient(espClient);
StaticJsonDocument<512> jsonDoc, lastReading; //json var
Timezone daikinTz;
RemoteDebug Debug;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
DNSServer dns;
AsyncWiFiManager wifiConnManager(&server,&dns);

//settings
struct {
  uint8_t check; //manual value to force update
  char hostname[32]; //device hostname
  //http auth section
  bool httpAuthEnable; //http authentication state
  char httpUser[32];   //http user
  char httpPass[32];   //http pass. Could be stored crypted..
  //http control
  bool httpControlEnable;
  //mqtt control
  bool mqttControlEnable;
  char mqttUser[32];   //mqtt user
  char mqttPass[32];   //mqtt pass. Could be stored crypted..
  char mqttBroker[64];   //mqtt broker
  char mqttTestamentTopic[64];
  char mqttSubTopic[64];   //mqtt topic to subscribe (receive commands)
  char mqttPubTopic[64];   //mqtt topic to publish data to
  
  uint8_t period; //reading period, in seconds
} config;

//software serial to control split
EspSoftwareSerial::UART daikinSWSerial;

//vars declaration
long lastRead, startTimeMsg, lastRssiSend = -30 * 1000L;
char json[512]; //used for the json message to be sent
char wsTxt[256]; //holds ws commands from clients

//websocket management function
void sendConfigWs(AsyncWebSocketClient * client){
  debugD("Sending config to client");
  //this sends the game config to clients
  DynamicJsonDocument root(512);
  root["type"] = "config";
  root["period"] = config.period;
  root["hostname"] = config.hostname;
  root["httpSecurityEnable"] = config.httpAuthEnable;
  root["httpSecurityUser"] = config.httpUser;
  root["httpControlEnable"] = config.httpControlEnable;
  root["mqttControlEnable"] = config.mqttControlEnable;
  root["mqttUser"] = config.mqttUser;
  root["mqttBroker"] = config.mqttBroker;
  root["mqttTestamentTopic"] = config.mqttTestamentTopic;
  root["mqttSubTopic"] = config.mqttSubTopic;
  root["mqttPubTopic"] = config.mqttPubTopic;
  root["resetNeeded"] = resetNeeded;

  size_t len = measureJson(root);
  AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  if (buffer) {
    serializeJson(root, (char *)buffer->get(), len + 1);
    if (client) {
      client->text(buffer);
    } else {
      ws.textAll(buffer);
    }
  }
}
void sendInfoWs(AsyncWebSocketClient * client){
  debugD("Sending info to client");
  //this sends the game config to clients
  DynamicJsonDocument root(256);
  root["type"] = "info";
  root["ipAddress"] = WiFi.localIP().toString();
  root["cpuMhz"] = ESP.getCpuFreqMHz();
  root["flashMhz"] = ESP.getFlashChipSpeed()/1000000;
  root["chipId"] = ESP.getChipId();
  root["coreVer"] = ESP.getCoreVersion();
  root["sdkVer"] = ESP.getSdkVersion();
  root["lastRst"] = ESP.getResetReason();
  root["wifiNetwork"] = WiFi.SSID();

  size_t len = measureJson(root);
  AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  if (buffer) {
    serializeJson(root, (char *)buffer->get(), len + 1);
    if (client) {
      client->text(buffer);
    } else {
      ws.textAll(buffer);
    }
  }
}
void sendSensorDataWs(AsyncWebSocketClient * client){
  //this sends the game status to clients
  debugD("Sending sensor data to client");
  DynamicJsonDocument root(512);
  root["type"] = "sensor";
  root["power"] = acValues.power_on;
  root["mode"] = acValues.mode;
  root["fan"] = acValues.fan;
  root["setpoint"] = acValues.setpoint;
  root["swing_v"] = acValues.swing_v;
  root["swing_h"] = acValues.swing_h;
  root["temp_inside"] = acValues.temp_inside;
  root["temp_outside"] = acValues.temp_outside;
  root["temp_coil"] = acValues.temp_coil;
  root["fan_rpm"] = acValues.fan_rpm;
  root["idle"] = acValues.idle;

  size_t len = measureJson(root);
  AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  if (buffer) {
    serializeJson(root, (char *)buffer->get(), len + 1);
    if (client) {
      client->text(buffer);
    } else {
      ws.textAll(buffer);
    }
  }
}
void sendStartTimeWs(AsyncWebSocketClient * client){
  //this sends last error message
  debugD("Sending start time to client");
  DynamicJsonDocument root(256);
  root["type"] = "startTime";
  root["startTime"] = startTimeMsg;
  size_t len = measureJson(root);
  AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
  if (buffer) {
    serializeJson(root, (char *)buffer->get(), len + 1);
    if (client) {
      client->text(buffer);
    } else {
      ws.textAll(buffer);
    }
  }
}
void sendRssiWs(AsyncWebSocketClient * client){
  //this just sends the RSSI value to clients
  if ( ws.count() > 0){ //only if we have WS clients
    debugD("Sending rssi to %d clients", ws.count());
    DynamicJsonDocument root(256);
    root["type"] = "rssi";
    root["value"] = WiFi.RSSI();
    size_t len = measureJson(root);
    AsyncWebSocketMessageBuffer * buffer = ws.makeBuffer(len); //  creates a buffer (len + 1) for you.
    if (buffer) {
      serializeJson(root, (char *)buffer->get(), len + 1);
      if (client) {
        client->text(buffer);
      } else {
        ws.textAll(buffer);
      }
    }
  }
}
//base WS function
void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
  if(type == WS_EVT_CONNECT){
    //new client, sending config and status
    debugD("Websocket client connection received");
    sendConfigWs(client);
    sendSensorDataWs(client);
    sendInfoWs(client);
    sendStartTimeWs(client);
    sendRssiWs(client);
  } else if(type == WS_EVT_DISCONNECT){
    debugD("Client disconnected");
 
  } else if(type == WS_EVT_DATA){
    //data packet received
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    if(info->opcode == WS_TEXT && info->final && info->index == 0 && info->len == len){
      //copy message to char array
      memcpy(wsTxt, (char*)data, len);
      //terminate it
      wsTxt[len] = '\0';
    } else {
      debugE("Something's wrong in received frame");
    }
  }
}

//wifimanager callbacks
void configModeCallback(AsyncWiFiManager *myWiFiManager) {
  WiFi.persistent(true);
  Serial.println("Connection failed. Entering config mode.");
  Serial.println(WiFi.softAPIP());
  Serial.println(myWiFiManager->getConfigPortalSSID());
}
void saveConfigCallback() {
  Serial.println("New config saved.");
  WiFi.persistent(false);
}

//Daikin AC Functions

//calculates checksum
uint8_t s21_checksum(uint8_t *bytes, uint8_t len) {
  uint8_t checksum = 0;
  for (uint8_t i = 0; i < len; i++) {
    checksum += bytes[i];
  }
  return checksum;
}
uint8_t s21_checksum(std::vector<uint8_t> bytes) {
  return s21_checksum(&bytes[0], bytes.size());
}

//turns bytes to num
int16_t bytes_to_num(uint8_t *bytes, size_t len) {
  // <ones><tens><hundreds><neg/pos>
  int16_t val = 0;
  val = bytes[0] - '0';
  val += (bytes[1] - '0') * 10;
  val += (bytes[2] - '0') * 100;
  if (len > 3 && bytes[3] == '-')
    val *= -1;
  return val;
}
int16_t bytes_to_num(std::vector<uint8_t> &bytes) {
  return bytes_to_num(&bytes[0], bytes.size());
}

//turns temp bytes to num
int16_t temp_bytes_to_c10(uint8_t *bytes) {
  return bytes_to_num(bytes, 4);
}
int16_t temp_bytes_to_c10(std::vector<uint8_t> &bytes) {
  return temp_bytes_to_c10(&bytes[0]);
}

//turn temperature num to bytes
uint8_t c10_to_setpoint_byte(int16_t setpoint) {
  return (setpoint + 3) / 5 + 28;
}

//turn bytes to HEX
std::string hex_repr(uint8_t *bytes, size_t len) {
  std::string res;
  char buf[5];
  for (size_t i = 0; i < len; i++) {
    if (i > 0)
      res += ':';
    sprintf(buf, "%02X", bytes[i]);
    res += buf;
  }
  return res;
}

//turn bytes to string
std::string str_repr(uint8_t *bytes, size_t len) {
  std::string res;
  char buf[5];
  for (size_t i = 0; i < len; i++) {
    if (bytes[i] == 7) {
      res += "\\a";
    } else if (bytes[i] == 8) {
      res += "\\b";
    } else if (bytes[i] == 9) {
      res += "\\t";
    } else if (bytes[i] == 10) {
      res += "\\n";
    } else if (bytes[i] == 11) {
      res += "\\v";
    } else if (bytes[i] == 12) {
      res += "\\f";
    } else if (bytes[i] == 13) {
      res += "\\r";
    } else if (bytes[i] == 27) {
      res += "\\e";
    } else if (bytes[i] == 34) {
      res += "\\\"";
    } else if (bytes[i] == 39) {
      res += "\\'";
    } else if (bytes[i] == 92) {
      res += "\\\\";
    } else if (bytes[i] < 32 || bytes[i] > 127) {
      sprintf(buf, "\\x%02X", bytes[i]);
      res += buf;
    } else {
      res += bytes[i];
    }
  }
  return res;
}
std::string str_repr(std::vector<uint8_t> &bytes) {
  return str_repr(&bytes[0], bytes.size());
}

void write_frame(std::vector<uint8_t> frame) {
  //clearing serial buffer
  daikinSWSerial.flush();
  //writing command to serial
  daikinSWSerial.write(STX);
  daikinSWSerial.write(&frame[0], frame.size());
  debugD("Writing frame contents: %s", str_repr(&frame[0], frame.size()).c_str());
  daikinSWSerial.write(s21_checksum(&frame[0], frame.size()));
  daikinSWSerial.write(ETX);
}

void dumpState() {
  debugI("** BEGIN STATE *****************************");
  debugI("  Power: %i", acValues.power_on);
  debugI("   Mode: %s (%s)", mode_to_string(acValues.mode).c_str(), acValues.idle ? "idle" : "active");
  debugI(" Target: %.1f C", acValues.setpoint / 10.0);
  debugI("    Fan: %s (%d rpm)", speed_to_string(acValues.fan).c_str(), acValues.fan_rpm);
  debugI("  Swing: H:%i V:%i", acValues.swing_h, acValues.swing_v);
  debugI(" Inside: %.1f C", acValues.temp_inside / 10.0);
  debugI("Outside: %.1f C", acValues.temp_outside / 10.0);
  debugI("   Coil: %.1f C", acValues.temp_coil / 10.0);
  debugI("** END STATE *****************************");
}

//mqtt functions
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  debugD("MQTT Message arrived on topic %s (payload: %s)", topic, (char*)payload);

  //easy approach: just put the payload received in the local wsTxt var for working in loop
  strcpy(wsTxt, (char*)payload);
  debugD("%s", wsTxt);
}

uint32_t mqttConnAttempt = 0; //don't want tons of useless connections attemps
bool mqttConnect() {
  // Reconnect to MQTT
  if (WiFi.status() == WL_CONNECTED) {
    // Attempt to connect
    if ( millis() - mqttConnAttempt > 1000UL ){
      const char* sysNotAvailable = "offline";
      if (mqttClient.connect(config.hostname, config.mqttUser, config.mqttPass, config.mqttTestamentTopic, 0, true, sysNotAvailable)) {
        debugD("Connected to MQTT broker");
        // ... and resubscribe
        const char* sysAvailable = "online";
        mqttClient.publish(config.mqttTestamentTopic, sysAvailable, true);
        mqttClient.subscribe(config.mqttSubTopic);
        return true;
      } else {
        debugE("Failed mqtt connection with RC=%i", mqttClient.state());
        mqttConnAttempt = millis();
        return false;
      }
    } else {
      return false;
    }
  } else {
    //not even connected to wifi
    return false;
  }
}

//header for remoteDebug callback function
void processCmdRemoteDebug();

void setup() {
  Serial.begin(115200);
  daikinSWSerial.begin(2400, EspSoftwareSerial::SWSERIAL_8E2, D7, D6, false);
  daikinSWSerial.setTimeout(1000);
  // need to store config data in eeprom
  EEPROM.begin(sizeof(config));
  //getting actual config
  EEPROM.get(0,config);

  //manual fix
  if ( config.check != 111 ){ //used for fists config setup
    String temp;
    config.check = 111; 
    //hostname
    strcpy(config.hostname, "myDaikin");
    config.httpAuthEnable = false;
    strcpy(config.httpUser, "user");
    strcpy(config.httpPass, "pass");
    //http control
    config.httpControlEnable = true;
    //mqtt control
    config.mqttControlEnable = false;
    strcpy(config.mqttUser, "user");
    strcpy(config.mqttPass, "pass");
    strcpy(config.mqttBroker, "broker");
    strcpy(config.mqttTestamentTopic, "testamentTopic");
    strcpy(config.mqttSubTopic, "subTopic");
    strcpy(config.mqttPubTopic, "pubTopic");
  
    config.period = 15;
    EEPROM.put(0,config);
    EEPROM.commit();  
  }

  //mqtt
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
  mqttClient.setSocketTimeout(1);
  if ( config.mqttControlEnable == true ) {
    mqttClient.setServer(config.mqttBroker, 1883);
  }

  //add hostname request
  AsyncWiFiManagerParameter hostnameParam("Hostname", "hostname", config.hostname, 32);
  wifiConnManager.addParameter(&hostnameParam);
  
  //sets timeout until configuration portal gets turned off
  //useful to make it all retry or go to sleep
  //in seconds
  wifiConnManager.setTimeout(180);

  //need a callback to setup the wifi credentials persistency
  wifiConnManager.setAPCallback(configModeCallback);
  //and one to remove that persistency
  wifiConnManager.setSaveConfigCallback(saveConfigCallback);

  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //here  "AutoConnectAP"
  //and goes into a blocking loop awaiting configuration
  if(!wifiConnManager.autoConnect(apname)) {
    Serial.println("failed to connect and hit timeout");
    //reset and try again, or maybe put it to deep sleep
    ESP.restart();
  }
  
  Serial.printf("Writing new hostname value <%s> to EEPROM\n", hostnameParam.getValue());
  strcpy(config.hostname, hostnameParam.getValue());
  //write eeprom
  EEPROM.put(0, config);
  EEPROM.commit(); 

  //setting hostname
  WiFi.hostname(config.hostname);

  //remote debug
  Debug.begin(config.hostname); // Initialize the WiFi server
  Debug.setResetCmdEnabled(true); // Enable the reset command
	Debug.showProfiler(true); // Profiler (Good to measure times, to optimize codes)
	Debug.showColors(true); // Colors
  Debug.showColors(true); // Colors
  Debug.setSerialEnabled(true);
  //callback to manage custom commands
  String helpCmd = "millis      -> Return actual millis() counter\r\n";
    helpCmd.concat("time        -> Return actual server time\r\n");
    helpCmd.concat("timestamp   -> Return actual server timestamp\r\n");
    helpCmd.concat("uptime      -> Return server start time and uptime\r\n");
    helpCmd.concat("restart     -> Restart device\r\n");
    helpCmd.concat("resetWiFi   -> Reset WiFi\r\n");
    helpCmd.concat("settings    -> Dump settings\r\n");
    helpCmd.concat("acvalues    -> Dump AC values\r\n");
    helpCmd.concat("\r\n");
  Debug.setHelpProjectsCmds(helpCmd);
	Debug.setCallBackProjectCmds(&processCmdRemoteDebug);

  Serial.print("Setting up timeserver");
  configTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org");
  if (!ezt::waitForSync(10)){
    Serial.println("Can't sync timeserver");
  }
  daikinTz.setLocation(F("Europe/Rome"));
  daikinTz.setPosix(F("CET-1CEST,M3.5.0/2,M10.5.0/3"));
  Serial.println("Done");

  //resetting lastread
  lastRead = - config.period * 1000U;

  //OTA section
  // Port defaults to 8266
  ArduinoOTA.setPort(3232);
  ArduinoOTA.setHostname(config.hostname);
  // Authentication
  ArduinoOTA.setPassword("123*");
  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      debugE("No SPIFFS image accepted");
      return;
    debugI("Start updating %s\n", type.c_str());
    }
  });

  ArduinoOTA.onEnd([]() {
    debugI("\nEnd");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    debugI("Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    debugE("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      debugE("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      debugE("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      debugE("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      debugE("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      debugE("End Failed");
    }
  });
  ArduinoOTA.begin();
    
  //if not deepsleep, setup the web and websocket server
  if(!LittleFS.begin()){
    debugE("An Error has occurred while mounting LittleFS");
    return;
  } 

  //base routes to html and js files
  server.onNotFound([](AsyncWebServerRequest *request){
    request->send(404);
  });
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if(config.httpAuthEnable == true && !request->authenticate(config.httpUser, config.httpPass))
      return request->requestAuthentication();
    request->send(LittleFS, "/index.html", "text/html");
  }).setFilter(ON_STA_FILTER);
  server.on("/daikin.js", HTTP_GET, [](AsyncWebServerRequest *request){
    if(config.httpAuthEnable == true && !request->authenticate(config.httpUser, config.httpPass))
      return request->requestAuthentication();
    request->send(LittleFS, "/daikin.js", "text/js");
  }).setFilter(ON_STA_FILTER);
  //images
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(LittleFS, "/favicon.ico", "image/x-icon");
  }).setFilter(ON_STA_FILTER);

  if ( config.httpControlEnable == true ){
    //returns info.
    server.on("/state", HTTP_GET, [](AsyncWebServerRequest *request) {
        if(config.httpAuthEnable == true && !request->authenticate(config.httpUser, config.httpPass))
          return request->requestAuthentication();
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        StaticJsonDocument<256> root;
        root["type"] = "sensor";
        root["power"] = acValues.power_on;
        root["mode"] = acValues.mode;
        root["fan"] = acValues.fan;
        root["setpoint"] = acValues.setpoint;
        root["swing_v"] = acValues.swing_v;
        root["swing_h"] = acValues.swing_h;
        root["temp_inside"] = acValues.temp_inside;
        root["temp_outside"] = acValues.temp_outside;
        root["temp_coil"] = acValues.temp_coil;
        root["fan_rpm"] = acValues.fan_rpm;
        root["idle"] = acValues.idle;
        serializeJson(root, *response);
        request->send(response);
    }).setFilter(ON_STA_FILTER);

    //accepts command, same format
    AsyncCallbackJsonWebHandler *handler = new AsyncCallbackJsonWebHandler("/control", [](AsyncWebServerRequest *request, JsonVariant &json) {
      StaticJsonDocument<256> data;
      if (json.is<JsonArray>()){
        data = json.as<JsonArray>();
      } else if (json.is<JsonObject>()){
        data = json.as<JsonObject>();
      }
      serializeJson(data, wsTxt);
      request->send(200, "application/json", "{\"received\":true}");
    });
    if ( config.httpAuthEnable == true ){
      handler->setAuthentication(config.httpUser, config.httpPass);
    }
    server.addHandler(handler);
  }

  //base websockets
  if ( config.httpAuthEnable == true ){
    ws.setAuthentication(config.httpUser, config.httpPass);
  }
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();
  
  //we are up. Setting up start time message
  startTimeMsg = makeTime(hour(), minute(), second(), day(), month(), year());

}

void loop() {
  if ( millis() - lastRead > config.period * 1000UL ) {
    lastRead = millis();
    if( state > 0 ){
      debugE("AC not ready to start update, state is still %i", state);
    } else {
      debugD("Starting AC update.");
      state = 1;
      acQuery = 0;
      updateStartTime = millis();
    }
  }

  //states-machine part
  if ( state > 0 ){
    if ( state == 1 ){ //state 1: sending query
      //state of querying
      if ( acQuery < acQueries.size() ){
        //sending query
        std::vector<uint8_t> code(acQueries[acQuery].begin(), acQueries[acQuery].end());
        write_frame(code);
        //starting serial timeout counter
        serialTimeoutStart = millis();
        //going to next state
        state = 2;
      } else {
        //over with queryes, let's go back to idle
        state = 0;
        //resetting indes
        acQuery = 0;
        //showing values
        dumpState();
        if ( valueChanged ){
          debugD("Values changed!");
          //sending values to clients
          sendSensorDataWs(0);
          //publishing on mqtt
          if ( config.mqttControlEnable == true ){
            if (mqttClient.connected() || mqttConnect() ){
              debugD("Publishing values");
              DynamicJsonDocument root(512);
              root["type"] = "sensor";
              root["power"] = acValues.power_on;
              root["mode"] = acValues.mode;
              root["fan"] = acValues.fan;
              root["setpoint"] = acValues.setpoint;
              root["swing_v"] = acValues.swing_v;
              root["swing_h"] = acValues.swing_h;
              root["temp_inside"] = acValues.temp_inside;
              root["temp_outside"] = acValues.temp_outside;
              root["temp_coil"] = acValues.temp_coil;
              root["fan_rpm"] = acValues.fan_rpm;
              root["idle"] = acValues.idle;
            
              char buffer[512];
              serializeJson(root, buffer);
              mqttClient.publish(config.mqttPubTopic, buffer, true);
            };
          }
          //resetting boolean
          valueChanged = false;
        }
        //and printing total time
        debugI("Total update time: %.2fs", (millis()-updateStartTime)/1000.0);
      }
    } //end state 1: sending query
    if ( state == 2 ){ //state 2: checking ACK
      if ( !daikinSWSerial.available() && (millis()-serialTimeoutStart) > serialTimeout ){
        //got no answer! error, going to state 5 to wait for the next command
        debugE("Timeout waiting for ACK for query %s, timeout", acQueries[acQuery].c_str());
        state = 5;
      } else {
        if ( daikinSWSerial.available() ){
          //got an answer, check if it's an ACK
          serialByte = daikinSWSerial.read();
          if (serialByte == NAK) {
            debugE("NAK from S21 for %s query", acQueries[acQuery].c_str());
            //ko for this query, so going to state 5 to wait for the next command
            state = 5;
          }
          if (serialByte != ACK) {
            debugE("No ACK from S21 for %s query (received %i)", acQueries[acQuery].c_str(), serialByte);
            //ko for this query, so going to state 5 to wait for the next command
            state = 5;
          }
          state = 3;
        }
      }
    } //end state 2: checking ACK
    if ( state == 3 ){ //state 3: reading frame
      if ( !frameReading ) {
        //starts a new frame reading, so resetting buffer 
        frameReading = true;
        frameBytes.clear();
        serialTimeoutStart = millis();
      }
      if ( !daikinSWSerial.available() && (millis()-serialTimeoutStart) > serialTimeout ){
        //got no answer! error, going to state 5 to wait for the next command
        debugE("Timeout waiting frame for query %s, timeout", acQueries[acQuery].c_str());
        frameReading = false;
        state = 5;
      } else {
        if ( daikinSWSerial.available() ){
          //ok let's read a frame byte
          serialByte = daikinSWSerial.read();
          //i can also reset counter for next byte timeout
          serialTimeoutStart = millis();
          if (serialByte == ACK) {
            debugE("Unexpected ACK waiting to read start of frame");
          } else if ( !STXreceived && serialByte != STX) {
            debugE("Unexpected byte waiting to read start of frame: %x", serialByte);
          } else if (serialByte == STX) {
            debugD("Received STX byte");
            STXreceived = true;
          } else if (serialByte == ETX) {
            //frame ended
            frameReading = false;
            STXreceived = false;
            //checking checksum
            frameChecksum = frameBytes[frameBytes.size() - 1];
            frameBytes.pop_back();
            calcChecksum = s21_checksum(frameBytes);
            if (calcChecksum != frameChecksum) {
              debugE("Checksum mismatch: %x (frame) != %x (calc from %s)", frameChecksum, calcChecksum, hex_repr(&frameBytes[0], frameBytes.size()).c_str());
              //as always, going to state 5 to wait for the next command
              state = 5;
            } else {
              //everything seems ok, let's go to next state and parse the frame!
              debugD("Correctly received frame: %s - %s", hex_repr(&frameBytes[0], frameBytes.size()).c_str(), str_repr(&frameBytes[0], frameBytes.size()).c_str());
              state = 4;
              //also sending an ACK to split
              daikinSWSerial.write(ACK);
            }
          } else {
            //the byte seems good for the buffer!
            frameBytes.push_back(serialByte);
          }
        }
      }
    } //end state 3: reading frame
    if ( state == 4 ){ //state 4: parse frame
      //parsing frame and filling local vars if good
      //for each value check if it has changed to limit network traffic over WS and MQTT
      switch (frameBytes[0]) {
        case 'G':      // F -> G
          switch (frameBytes[1]) {
            case '1':  // F1 -> G1 -- Basic State
              if ( acValues.power_on != (bool)(frameBytes[2] == '1') ){
                debugD("Power changed from %i to %i", acValues.power_on, (bool)(frameBytes[2] == '1'));
                acValues.power_on = (frameBytes[2] == '1');
                valueChanged = true;
              }
              if ( acValues.mode != (uint8_t)frameBytes[3] ){
                debugD("Mode changed from %i to %i", acValues.mode, (uint8_t)frameBytes[3]);
                acValues.mode = frameBytes[3];
                valueChanged = true;
              }
              if ( acValues.fan != (uint8_t)frameBytes[5] ){
                debugD("Fan changed from %i to %i", acValues.fan, (uint8_t)frameBytes[5]);
                acValues.fan = frameBytes[5];
                valueChanged = true;
              }
              if ( acValues.setpoint != (int16_t)((frameBytes[4] - 28) * 5) ){
                //only valid if mode is different from DRY and FAN
                if ( acValues.mode != 50 && acValues.mode != 54 ){
                  debugD("Setpoint changed from %i to %i", acValues.setpoint, (int16_t)((frameBytes[4] - 28) * 5));
                  acValues.setpoint = ((frameBytes[4] - 28) * 5);  // Celsius * 10
                  valueChanged = true;
                }
              }
              debugD("Power is %i, mode is %s, setpoint is %i, fan is %s", acValues.power_on, mode_to_string(acValues.mode).c_str(), acValues.setpoint, speed_to_string(acValues.fan).c_str());
              break;
            case '5':  // F5 -> G5 -- Swing state
              if ( acValues.swing_v != (bool)(frameBytes[2] & 1) ){
                debugD("SwingV changed from %i to %i", acValues.swing_v, (bool)(frameBytes[2] & 1));
                acValues.swing_v = frameBytes[2] & 1;
                valueChanged = true;
              }
              if ( acValues.swing_h != (bool)(frameBytes[2] & 2) ){
                debugD("SwingH changed from %i to %i", acValues.swing_h, (bool)(frameBytes[2] & 2));
                acValues.swing_h = frameBytes[2] & 2;
                valueChanged = true;
              }
              debugD("V-Swing is %i, H-Swing is %i", acValues.swing_v, acValues.swing_h);
              break;
          }
          break;
        case 'S':      // R -> S
          switch (frameBytes[1]) {
            case 'H':  // Inside temperature
              if ( acValues.temp_inside != temp_bytes_to_c10(&frameBytes[2]) ){
                debugD("Temp Inside changed from %i to %i", acValues.temp_inside, temp_bytes_to_c10(&frameBytes[2]));
                acValues.temp_inside = temp_bytes_to_c10(&frameBytes[2]);
                valueChanged = true;
              }
              debugD("Temp inside is %i", acValues.temp_inside);
              break;
            case 'I':  // Coil temperature
              if ( acValues.temp_coil != temp_bytes_to_c10(&frameBytes[2]) ){
                debugD("Temp Coil changed from %i to %i", acValues.temp_coil, temp_bytes_to_c10(&frameBytes[2]));
                acValues.temp_coil = temp_bytes_to_c10(&frameBytes[2]);
                valueChanged = true;
              }
              debugD("Temp coil is %i", acValues.temp_coil);
              break;
            case 'a':  // Outside temperature
              if ( acValues.temp_outside != temp_bytes_to_c10(&frameBytes[2]) ){
                debugD("Temp Outside changed from %i to %i", acValues.temp_outside, temp_bytes_to_c10(&frameBytes[2]));
                acValues.temp_outside = temp_bytes_to_c10(&frameBytes[2]);
                valueChanged = true;
              }
              debugD("Temp outside is %i", acValues.temp_outside);
              break;
            case 'L':  // Fan speed
              if ( acValues.fan_rpm != (bytes_to_num(&frameBytes[2], frameBytes.size()-2) * 10) ){
                debugD("Fan RPM changed from %i to %i", acValues.fan_rpm, (bytes_to_num(&frameBytes[2], frameBytes.size()-2) * 10));
                acValues.fan_rpm = bytes_to_num(&frameBytes[2], frameBytes.size()-2) * 10;
                valueChanged = true;
              }
              debugD("Fan rpm is %i", acValues.fan_rpm);
              break;
            case 'd':  // Compressor state / frequency? Idle if 0.
              if ( acValues.idle != (bool)(frameBytes[2] == '0' && frameBytes[3] == '0' && frameBytes[4] == '0') ){
                debugD("Idle changed from %i to %i", acValues.idle, (bool)(frameBytes[2] == '0' && frameBytes[3] == '0' && frameBytes[4] == '0'));
                acValues.idle = (frameBytes[2] == '0' && frameBytes[3] == '0' && frameBytes[4] == '0');
                valueChanged = true;
              }
              break;
            default:
              if (frameBytes.size() > 5) {
                int8_t temp = temp_bytes_to_c10(&frameBytes[2]);
                debugD("Unknown temp: %s -> %.1f C", str_repr(frameBytes).c_str(), temp / 10.0);
              }
          }
          break;
        default:
          debugW("Unknown response %s ", str_repr(frameBytes).c_str());
      }
      //going to state to wait before next query
      state = 5;
    } //end state 4: parse frame
    if ( state == 5 ){ //state 5: waiting
      if ( !waiting ){
        waitTimer = millis();
        waiting = true;
      } else {
        if ( millis() - waitTimer > waitTimeout ){
          //time to go back to business!
          waiting = false;
          state = 1;
          acQuery++;
        }
      }
    } //end state 5: waiting
  } //end if state > 0

  //we also have to manage commands! with a states-machine
  if ( cmdState > 0 ){
    if ( cmdState == 1 ){
      //so we need to send a new command. Disable update and prepare sending new command
      //if an update is en course, we need to wait some time to clear responses
      if ( state > 0){
        //so going to state 2 and wait a "serial Timeout time" before sending command
        debugD("An update is en course, waiting a SERIALTIMEOUT before sending command");
        cmdState = 2;
        serialTimeoutStart = millis();
      } else {
        //we can skip to state 3 to send command
        cmdState = 3;
      }
      state = 0;
      //resetting indes
      acQuery = 0;
      //also avoid starting new updates
      lastRead = millis();
    } //end cmdState 1: disabling update and preparing sending new command
    if ( cmdState == 2 ){ //cmdstate 2: waiting..
      if ( serialTimeoutStart - millis() > serialTimeout ){
        //go to state 3
        cmdState = 3;
      }
    } //end cmdstate 2: waiting
    if ( cmdState == 3 ){ //cmdstate 3: sending command
      debugD("Sending AC Command %s", str_repr(&acCommand[0], acCommand.size()).c_str());
      //now sending command
      write_frame(acCommand);
      //and wait for an ack!
      serialTimeoutStart = millis();
      cmdState = 4;
    } //end cmdstate 3: sending command
    if ( cmdState == 4 ){ //cmdstate 4: checking ACK
      if ( !daikinSWSerial.available() && (millis()-serialTimeoutStart) > serialTimeout ){
        //got no answer! error, going to state 5 to wait for the next command
        debugE("Timeout waiting for ACK for command %s, timeout", str_repr(&acCommand[0], acCommand.size()).c_str());
        cmdState = 0;
        //triggering an update
        lastRead = millis() - config.period * 1000UL;
      } else {
        if ( daikinSWSerial.available() ){
          //got an answer, check if it's an ACK
          serialByte = daikinSWSerial.read();
          if (serialByte == NAK) {
            debugE("NAK from S21 for %s command", str_repr(&acCommand[0], acCommand.size()).c_str());
          } else if (serialByte != ACK) {
            debugE("No ACK from S21 for %s command (received %i)", str_repr(&acCommand[0], acCommand.size()).c_str(), serialByte);
          } else if (serialByte == ACK) {
            debugI("Command %s acknowledged", str_repr(&acCommand[0], acCommand.size()).c_str());
          }
          //command over, good or bad
          cmdState = 0;
          //clearing command
          acCommand.clear();
          //triggering an update
          lastRead = millis() - config.period * 1000UL;
        }
      }
    } //end cmdstate 4: checking ack
  } //end cmd state > 0

  //management of clients commands in loop. Parameters' values could be checked for security..
  if ( wsTxt[0] != '\0' ){
    debugD("Working WS message <%s>.", wsTxt);
    DynamicJsonDocument wsMsg(256);
    auto error = deserializeJson(wsMsg, wsTxt);
    if (error) {
      debugE("deserializeJson() failed with code %s", error.c_str());
      wsTxt[0] = '\0';
      return;
    }
    if ( wsMsg["command"].as<String>() == "rstDevice" ){
      debugD("Resetting device");
      ESP.restart();
    }
    if ( wsMsg["command"].as<String>() == "rstWifi" ){
      debugD("Resetting wifi");
      WiFi.persistent(true);
      wifiConnManager.resetSettings();
      ESP.restart();
    }

    //manage config settings
    if ( wsMsg["command"].as<String>() == "config" ){
      if ( wsMsg["target"].as<String>() == "period" ){
        config.period = wsMsg["value"].as<byte>();
        debugD("Updating period to %i", wsMsg["value"].as<byte>());
      }
      if ( wsMsg["target"].as<String>() == "hostname" ){
        String hostname;
        hostname = wsMsg["value"].as<String>();
        hostname.toCharArray(config.hostname, 32);
        WiFi.hostname(config.hostname);
        debugD("Updating hostname to %s", hostname.c_str());
        //need a reset
        resetNeeded = true;
      }
      if ( wsMsg["target"].as<String>() == "httpEnable" ){
        config.httpAuthEnable = wsMsg["value"].as<bool>();
        debugD("Updating httpAuthEnable %d", wsMsg["value"].as<bool>());
        //need a reset
        resetNeeded = true;
      }
      if ( wsMsg["target"].as<String>() == "httpAccessData" ){
        String user,pass;
        user = wsMsg["username"].as<String>();
        pass = wsMsg["password"].as<String>();    
        
        user.toCharArray(config.httpUser, 32);
        pass.toCharArray(config.httpPass, 32);

        debugD("Updating Http access data: User: %s - Pass: %s", user.c_str(), pass.c_str());
        //need a reset
        resetNeeded = true;
      }
      if ( wsMsg["target"].as<String>() == "httpControlEnable" ){
        config.httpControlEnable = wsMsg["value"].as<bool>();
        debugD("Updating httpControlEnable %d", wsMsg["value"].as<bool>());
        //need a reset
        resetNeeded = true;
      }
      if ( wsMsg["target"].as<String>() == "mqttControlEnable" ){
        config.mqttControlEnable = wsMsg["value"].as<bool>();
        debugD("Updating mqttControlEnable %d", wsMsg["value"].as<bool>());
        //need a reset
        resetNeeded = true;
      }
      if ( wsMsg["target"].as<String>() == "mqttAccessData" ){
        String user,pass;
        user = wsMsg["username"].as<String>();
        pass = wsMsg["password"].as<String>();    
        
        user.toCharArray(config.mqttUser, 32);
        pass.toCharArray(config.mqttPass, 32);

        debugD("Updating Mqtt access data: User: %s - Pass: %s", user.c_str(), pass.c_str());
        //need a reset
        resetNeeded = true;
      }
      if ( wsMsg["target"].as<String>() == "mqttData" ){
        String broker, testamentTopic, subTopic, pubTopic;
        broker = wsMsg["broker"].as<String>();
        testamentTopic = wsMsg["testamentTopic"].as<String>();    
        subTopic = wsMsg["subTopic"].as<String>();    
        pubTopic = wsMsg["pubTopic"].as<String>();    

        broker.toCharArray(config.mqttBroker, 64);
        testamentTopic.toCharArray(config.mqttTestamentTopic, 64);
        subTopic.toCharArray(config.mqttSubTopic, 64);
        pubTopic.toCharArray(config.mqttPubTopic, 64);

        debugD("Updating Mqtt data: Broker: %s - SubTopic: %s - PubTopic: %s - TestamentTopic: %s", broker.c_str(), subTopic.c_str(), pubTopic.c_str(), testamentTopic.c_str());
        //need a reset
        resetNeeded = true;
      }

      //now writing values to eeprom
      debugD("Writing new config to eeprom");
      EEPROM.put(0,config);
      EEPROM.commit(); 

      //we also need to send updated config to all clients
      sendConfigWs(0);
    }

    //manage ac commands, split by single command so to ease HA integration
    if ( wsMsg["command"].as<String>() == "acPower" ){
      debugD("Sending AC Power: %i", wsMsg["power"].as<bool>());
      
      acCommand = {'D', '1',
        (uint8_t)(wsMsg["power"].as<bool>() ? '1' : '0'),
        (uint8_t) acValues.mode,
        c10_to_setpoint_byte(acValues.setpoint),
        (uint8_t) acValues.fan
      };

      //triggering send command
      cmdState = 1;
    }
    if ( wsMsg["command"].as<String>() == "acMode" ){
      debugD("Sending AC Mode: %s", mode_to_string(wsMsg["mode"].as<uint8_t>()).c_str());
      
      acCommand = {'D', '1',
        (uint8_t)(acValues.power_on ? '1' : '0'),
        (uint8_t) modeToChar(wsMsg["mode"].as<uint8_t>()),
        c10_to_setpoint_byte(acValues.setpoint),
        (uint8_t) acValues.fan
      };

      //triggering send command
      cmdState = 1;
    }
    //needed for HA integration
    if ( wsMsg["command"].as<String>() == "acHaMode" ){
      if( wsMsg["mode"].as<uint8_t>() == 0 ){
        debugD("Turning AC power off.");
        acCommand = {'D', '1',
          (uint8_t)'0',
          (uint8_t) acValues.mode,
          c10_to_setpoint_byte(acValues.setpoint),
          (uint8_t) acValues.fan
        };
      } else {
        debugD("Turning power on and setting AC Mode: %s", mode_to_string(wsMsg["mode"].as<uint8_t>()).c_str());
        acCommand = {'D', '1',
          (uint8_t)'1',
          (uint8_t) modeToChar(wsMsg["mode"].as<uint8_t>()),
          c10_to_setpoint_byte(acValues.setpoint),
          (uint8_t) acValues.fan
        };
      }

      //triggering send command
      cmdState = 1;
    }
    if ( wsMsg["command"].as<String>() == "acFan" ){
      debugD("Sending AC Fan: %s", speed_to_string(wsMsg["fan"].as<uint8_t>()).c_str());
      
      acCommand = {'D', '1',
        (uint8_t)(acValues.power_on ? '1' : '0'),
        (uint8_t) acValues.mode,
        c10_to_setpoint_byte(acValues.setpoint),
        (uint8_t) fanToChar(wsMsg["fan"].as<uint8_t>())
      };

      //triggering send command
      cmdState = 1;
    }
    if ( wsMsg["command"].as<String>() == "acTemp" ){
      debugD("Sending AC TargetTemp: %i", wsMsg["temp"].as<int16_t>());
      
      acCommand = {'D', '1',
        (uint8_t)(acValues.power_on ? '1' : '0'),
        (uint8_t) acValues.mode,
        c10_to_setpoint_byte(wsMsg["temp"].as<int16_t>() * 10),
        (uint8_t) acValues.fan
      };

      //triggering send command
      cmdState = 1;
    }
    if ( wsMsg["command"].as<String>() == "acSwingV" ){
      //swing control command
      debugD("Sending AC Swing Vertical command: %i", wsMsg["swingV"].as<bool>());

      acCommand = {'D', '5',
        (uint8_t) ('0' + (acValues.swing_h ? 2 : 0) + (wsMsg["swingV"].as<bool>() ? 1 : 0) + (acValues.swing_h && wsMsg["swingV"].as<bool>() ? 4 : 0)),
        (uint8_t) (wsMsg["swingV"].as<bool>() || acValues.swing_h ? '?' : '0'), 
        '0', '0'
      };

      //triggering send command
      cmdState = 1;
    }
    if ( wsMsg["command"].as<String>() == "acSwingH" ){
      //swing control command
      debugD("Sending AC Swing Horizontal command: %i", wsMsg["swingH"].as<bool>());

      acCommand = {'D', '5',
        (uint8_t) ('0' + (wsMsg["swingH"].as<bool>() ? 2 : 0) + (acValues.swing_v ? 1 : 0) + (wsMsg["swingH"].as<bool>() && acValues.swing_v ? 4 : 0)),
        (uint8_t) (acValues.swing_v || wsMsg["swingH"].as<bool>() ? '?' : '0'), 
        '0', '0'
      };
      //triggering send command
      cmdState = 1;
    }
    //for HA integration
    if ( wsMsg["command"].as<String>() == "acSwing" ){
      //swing control command
      debugD("Sending AC Swing Horizontal command: %i and vertical command: %i", wsMsg["swingH"].as<bool>(), wsMsg["swingV"].as<bool>());

      acCommand = {'D', '5',
        (uint8_t) ('0' + (wsMsg["swingH"].as<bool>() ? 2 : 0) + (wsMsg["swingV"].as<bool>() ? 1 : 0) + (wsMsg["swingH"].as<bool>() && wsMsg["swingV"].as<bool>() ? 4 : 0)),
        (uint8_t) (wsMsg["swingV"].as<bool>() || wsMsg["swingH"].as<bool>() ? '?' : '0'), 
        '0', '0'
      };
      //triggering send command
      cmdState = 1;
    }
        
    //clear the ws message - null terminate the first array element
    wsTxt[0] = '\0';
  }
  
  //periodically send RSSI data to clients, if any
  if ( millis() - lastRssiSend > 30000UL ){
    lastRssiSend = millis();
    //sending RSSI to clients
    sendRssiWs(0);
  }

  //time management
  ezt::events();
  //for OTA update
  ArduinoOTA.handle();
  //remote debug
  Debug.handle();
  //mqtt
  if ( config.mqttControlEnable == true ){
    if (mqttClient.connected() || mqttConnect() ){
      mqttClient.loop();
    };
  }
}

//body for remoteDebug callback function
void processCmdRemoteDebug(){
	String lastCmd = Debug.getLastCommand();
	if (lastCmd == "millis") {
    //return actual millis
    debugA("Actual millis: %lu", millis());
	} else if (lastCmd == "time") {
    //return actual time:
    debugA("Device time: %s", daikinTz.dateTime().c_str());
  } else if (lastCmd == "timestamp") {
    //return actual time:
    debugA("Device timestamp: %lld", makeTime(hour(), minute(), second(), day(), month(), year()));
  } else if (lastCmd == "uptime") {
    //Return start time and uptime
    debugA("Returning up time and start time");
    debugA("Start Time: %s", daikinTz.dateTime(startTimeMsg - daikinTz.getOffset()*60).c_str());
    int upTime = makeTime(hour(), minute(), second(), day(), month(), year()) - startTimeMsg;
    debugA("Uptime: %02lu:%02lu:%02lu:%02lu", numberOfDays(upTime), numberOfHours(upTime), numberOfMinutes(upTime), numberOfSeconds(upTime));
  } else if (lastCmd == "restart") {
    //return actual time:
    debugA("Restarting device");
    ESP.restart();
  } else if (lastCmd == "resetWiFi") {
    //resetting WiFi config:
    debugA("Resetting WiFi Config");
    WiFi.persistent(true);
    wifiConnManager.resetSettings();
    ESP.restart();
  } else if (lastCmd == "settings") {
    //dumping system settings:
    debugA("Dumping system settings");
    debugA("struct {");
    debugA("  uint8_t check = %i;", config.check);
    debugA("  char hostname[32] = %s;", config.hostname);
    debugA("  //http auth section");
    debugA("  bool httpAuthEnable = %i;", config.httpAuthEnable);
    debugA("  char httpUser[32] = %s;", config.httpUser);
    debugA("  char httpPass[32] = <xxxx>;");
    debugA("  //http control");
    debugA("  bool httpControlEnable = %i;", config.httpControlEnable);
    debugA("  //mqtt control");
    debugA("  bool mqttControlEnable = %i;", config.mqttControlEnable);
    debugA("  char mqttUser[32] = %s;", config.mqttUser);
    debugA("  char mqttPass[32] = <xxxx>;");
    debugA("  char mqttBroker[64] = %s;", config.mqttBroker);
    debugA("  char mqttTestamentTopic[64] = %s;", config.mqttTestamentTopic);
    debugA("  char mqttSubTopic[64] = %s;", config.mqttSubTopic);
    debugA("  char mqttPubTopic[64] = %s;", config.mqttPubTopic);
    debugA("  uint8_t period = %i;", config.period);
    debugA("} config;");
  } else if (lastCmd == "acvalues") {
    //dumping ac values:
    debugA("Dumping AC values");
    debugA("struct {");
    debugA("  bool power_on = %i;", acValues.power_on);
    debugA("  uint8_t mode = %s;", mode_to_string(acValues.mode).c_str());
    debugA("  uint8_t fan = %s;", speed_to_string(acValues.fan).c_str());
    debugA("  int16_t setpoint = %i;", acValues.setpoint);
    debugA("  bool swing_v = %i;", acValues.swing_v);
    debugA("  bool swing_h = %i;", acValues.swing_h);
    debugA("  int16_t temp_inside = %i;", acValues.temp_inside);
    debugA("  int16_t temp_outside = %i;", acValues.temp_outside);
    debugA("  int16_t temp_coil = %i;", acValues.temp_coil);
    debugA("  uint16_t fan_rpm = %i;", acValues.fan_rpm);
    debugA("  bool idle = %i;", acValues.idle);
    debugA("} acValues;");
  }
}