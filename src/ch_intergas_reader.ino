#include <Arduino.h>

/*
 * cvintergas - read status from Intergas Central Heating and publish it through MQTT
 *
 * Based on info Leo van der Kallen: http://www.circuitsonline.net/forum/view/80667/last/intergas
 * Time is synchronized using NTP.
 *
 * MQTT topics
 *
 * - settings
 *   scan-period - how often is status read from Intergas (in seconds)
 *   sync-period - how often time is synchronized (in seconds)
 * - sensor data
 *   - chig1/...
 *     - timestamp
 *     - uptime
 *     - t1, t2, t3, t4, t5
 *     - ch-pressure
 *     - temperature-setting
 *     - temperature-flow
 *     - fan-speed
 *     - fan-speed-set
 *     - fan-pwm
 *     - ionisation-current
 *     - status-byte1
 *     - status-byte2
 *     - input-buffer
 *
 * - Map
 *   - Taanvoer
 *   - Twarmwater
 *   - Tboiler
 *   - Tbuiten  (no measurement?)
 *   - Tmax
 *   - Opentherm
 *   - On/off thermostaat       roomtherm?
 *   - gasklep                  gasvalve
 *   - pomp loopt               pump running
 *   - Ext. comfort
 *   - Tapschakelaar            tap switch
 *   - CV klep
 *   - Ventilator pwm           fan pwm
 *   - druk sensor              has_pressure_sensor
 *   - ionisatie stroom         io_curr
 *   - status byte1
 *   - status byte2
 *
 * todo
 * - clean up code
 * - implement other messages: VER, CRC, REV, EN
 * - add wifi AP manager
 *    e.g. https://tzapu.com/esp8266-wifi-connection-manager-library-arduino-ide/
 *    preferably based on a button press
 * - add OTA update of firmware
 * - set scan period long when CH is idle, and short when active
 * - consider to send status word instead of two status bytes
 * - consider to implement this in homie
 */

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>
#include <TimeLib.h>
#include <Timezone.h>    // https://github.com/JChristensen/Timezone
#include <WiFiUdp.h>

#define DEBUG  0

#if DEBUG==1
#define debugln(s)    Serial.println(s)
#define debug(s)      Serial.print(s)
#else
#define debugln(s)
#define debug(s)
#endif

#include "config.h"

WiFiClient espClient;
PubSubClient mqttClient(espClient);

bool messageReceived;
bool waitingForAnswer;

#define InputBufferLen   64
#define StatusMsgLen     32

byte inputBuffer[InputBufferLen];
int writeIndex;

#define TimeZoneOffset  0L   // the offset in seconds to your local time;

#define NTP_UDPPort      123
#define MQTT_Port       1883
#define UDP_LocalPort   8888
#define NTP_PacketSize    48 // NTP time stamp is in the first 48 bytes of the message

#define PinLED            12

IPAddress ntpIP;
WiFiUDP ntp;
byte packetBuffer[NTP_PacketSize]; //buffer to hold incoming and outgoing packets

SoftwareSerial intergas(4, 5, false ); // RX, TX, not inverted

#define TimeoutIntergas  (3*1000) // in milliseconds

long  receiveTimer;
long  scanPeriod;
long  syncPeriod;

// Intergas status parameters
double       ch_t1, ch_t2, ch_t3, ch_t4, ch_t5, ch_t6;
double       ch_fanspeed, ch_fanspeed_setpoint, ch_fan_pwm, ch_temp_setpoint, ch_temp_flow;
double       ch_io_current, ch_pressure;

unsigned int ch_status_code, ch_fault_code;
bool         ch_has_pressure_sensor;

unsigned char ch_status_byte1, ch_status_byte2;

char         ch_status_description[32];
char         ch_input_buffer[3*StatusMsgLen+5];      // ASCII bytes with spaces

time_t getNtpTime(void);
void   sendTimestamp(void);

// uptime variables
long upDay = 0;
int  upHour = 0;
int  upMinute = 0;
int  upSecond = 0;
int  upHighMillis = 0;
int  upRollover = 0;

char *ftoa(char *a, double f, int precision) {
   long p[] = {0,10,100,1000,10000,100000,1000000,10000000,100000000};

   char *ret = a;
   long heiltal = (long)f;
   itoa(heiltal, a, 10);
   while (*a != '\0') a++;
   *a++ = '.';
   long deci = abs((long)((f - heiltal) * p[precision]));
   itoa(deci, a, 10);

 return ret;
}

void printFreeHeap() {
   Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
}

void ledOn() {
  digitalWrite(PinLED, HIGH);    // turn the LED off by making the voltage LOW
//  isLedOn = true;
}

void ledOff() {
  digitalWrite(PinLED, LOW);    // turn the LED on by making the voltage HIGH
//  isLedOn = false;
}

void setupTime() {
      // Setup retrieval of time through ntp server
   WiFi.hostByName(ntpServerName, ntpIP);
   Serial.print(F("NTP IP: "));
   Serial.println(ntpIP);
   ntp.begin(UDP_LocalPort);
   setSyncProvider(getNtpTime);
   setSyncInterval(syncPeriod);
}

long lastMsg = 0;

void setupWifi() {
  delay(10);

  Serial.println();
  Serial.print(F("Connecting to "));
  Serial.println(ssid);
  WiFi.begin(ssid, ssidPassword);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(F("."));
  }

  Serial.println(F(""));
  Serial.println(F("WiFi connected"));
  Serial.print(F("Intergas reader IP address: "));
  Serial.println(WiFi.localIP());
}

void MQTT_callback(char* topic, byte* payload, unsigned int length) {
     // handle MQTT callback with subscribed topics
  long  period;

  Serial.print(F("MQTT message arrived ["));
  Serial.print(topic);
  Serial.print(F("] "));
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
  }
  Serial.println();

  if (!strncmp_P(topic, PSTR("chig1/set/scan-period"), 21)) {
    // convert payload to value in seconds. check range of value
    period = String((char *)payload).toInt();
    if (period > 1)
       scanPeriod = period*1000;  // in milliseconds
  }
  if (!strncmp_P(topic, PSTR("chig1/set/sync-period"), 21)) {
    // convert payload to value in seconds. check range of value
    period = String((char *)payload).toInt();
    if (period > 1) {
       syncPeriod = period;        // in seconds
       setSyncInterval(syncPeriod);
    }
  }
}

void setup() {
   pinMode(PinLED, OUTPUT);
   ledOn();
   messageReceived = false;
   waitingForAnswer = false;
   syncPeriod = 86400;  // seconds
   scanPeriod =  5000;  // milliseconds
   lastMsg = 0;
   Serial.begin(115200);
   Serial.println(F("\nIntergas Central Heating reader v0.05 20160617"));
   Serial.println(F(__TIMESTAMP__));
//   Serial.print(F("ESP Id: "));       Serial.println(ESP.getChipId(), HEX);
//   Serial.print(F("Flash Id: "));     Serial.println(ESP.getFlashChipId(), HEX);
//   Serial.print(F("Flash Size: "));   Serial.printf("Sketch size: %u\n", ESP.getSketchSize());
//   Serial.printf("Free size: %u\n", ESP.getFreeSketchSpace());

//   delay(500);
   intergas.begin(9600);
   printFreeHeap();
   setupWifi();
   delay(50);
   mqttClient.setServer(mqttServer, MQTT_Port);
   mqttClient.setCallback(MQTT_callback);
   Serial.println(F("Intergas reader initialized"));
   ledOff();
   delay(50);
}

void reconnect() {
     // Loop until we're reconnected
  while (!mqttClient.connected()) {
    Serial.print(F("Attempting MQTT connection..."));
    // Attempt to connect
    if (mqttClient.connect(mqttClientId, mqttUsername, mqttPassword)) {
      Serial.println(F("connected"));
      // Subscribe to settings that can be set remotely
      delay(500);
      setupTime();
      mqttClient.subscribe("chig1/set/scan-period");
      mqttClient.subscribe("chig1/set/sync-period");
      sendTimestamp();
      sendUptime();
   } else {
      Serial.print(F("failed, result="));
      Serial.print(mqttClient.state());
      Serial.println(F(" try again in 5 seconds"));
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void intergasFlush() {
     // Flush already received characters.
   while (intergas.available() > 0) {
      intergas.read();
   }
}

void requestStatus() {
  ledOn();
  Serial.println(F("Request status from Intergas"));
  waitingForAnswer = true;
  intergasFlush();
  intergas.write('S');
  intergas.write('?');
  intergas.write('\r');
  receiveTimer = millis();
  ledOff();
}

char c2h(char c){
  return "0123456789ABCDEF"[0x0F & (unsigned char)c];
}

void readStatus(){
      // read status message from central heating
   byte  ch;
   long  t;

   if (waitingForAnswer) {
//      ledOn();
      t = millis();
      if ((t - receiveTimer) <= TimeoutIntergas) {
//         Serial.print(F("timer: ")); Serial.println(t);
         if (intergas.available()) {
            ch = intergas.read();
            if (writeIndex == 0) {
               Serial.println(F("Received from Intergas:"));
            }
            inputBuffer[writeIndex++] = ch;
            Serial.print(F(" ")); Serial.print(c2h( ((ch&0xf0) >> 4))); Serial.print(c2h(ch&0x0f));
            if (writeIndex == StatusMsgLen) {
              // Message does not have markers. When 32 bytes are received stop reading
              Serial.println();
              writeIndex = 0;
//              ledOff();
              waitingForAnswer = false;
              messageReceived = true;
            }
         }
      } else {
         writeIndex = 0;
//         ledOff();
         waitingForAnswer = false;
         Serial.println(F("Timeout Intergas"));
      }
   }
}

double getDouble(byte msb, byte lsb) {
      // Calculate float value from two bytes
   double  result;
   double  res1;

   if (msb > 127) {
       result = -(((msb ^ 255) + 1) * 256 - lsb) / 100;
   } else {
       result = ((float)(msb * 256 + lsb)) / 100;
   }
   return result;
}

void processStatus() {
      // Retrieve the variables from the message
   if (messageReceived) {
      ch_t1 = getDouble(inputBuffer[1],  inputBuffer[0]);
      ch_t2 = getDouble(inputBuffer[5],  inputBuffer[4]);
      ch_t3 = getDouble(inputBuffer[7],  inputBuffer[6]);
      ch_t4 = getDouble(inputBuffer[9],  inputBuffer[8]);
      ch_t5 = getDouble(inputBuffer[11], inputBuffer[10]);

      ch_fanspeed      = getDouble(inputBuffer[19], inputBuffer[18]) * 100;
      ch_fanspeed_setpoint = getDouble(inputBuffer[17], inputBuffer[16]) * 100;
      ch_fan_pwm       = getDouble(inputBuffer[21], inputBuffer[20]) *  10;
      ch_io_current    = getDouble(inputBuffer[23], inputBuffer[22]);
      ch_pressure      = getDouble(inputBuffer[13], inputBuffer[12]);
      ch_temp_flow     = getDouble(inputBuffer[ 3], inputBuffer[ 2]);
      ch_temp_setpoint = getDouble(inputBuffer[15], inputBuffer[14]);

      ch_status_byte1 = inputBuffer[26];
      ch_status_byte2 = inputBuffer[28];

      ch_has_pressure_sensor    = (inputBuffer[28] & 0x20) == 0x20;
      if (!ch_has_pressure_sensor) {
         ch_pressure   = -35;
      }

      ch_status_code = inputBuffer[24];
      if ((inputBuffer[27] & 0x80 != 0x80)) {
         ch_fault_code  = (unsigned int)inputBuffer[29];
         ch_status_code = 256 + ch_fault_code;
      } else {
         ch_fault_code = 0;
      }
      switch (ch_status_code) {
      // ontbreken: ventileren, ontsteken, opwarmen boiler
      // op display
      // - uit; wachtstand; 0 Nadraaien CV; 1 Gew. temperatuur bereikt; 2 Zelftest
      //   3 Ventileren; 4  Ontsteken; 5 CV; 6 Warm water; 7 Opwarmen boiler
      case  51: strcpy_P(ch_status_description,  PSTR("Warm water"));   break;
      case 102: strcpy_P(ch_status_description,  PSTR("CV brandt"));   break;
      case 126: strcpy_P(ch_status_description,  PSTR("CV in rust"));  break;
      case 204: strcpy_P(ch_status_description,  PSTR("Tapwater nadraaien"));   break;  //ventileren?
      case 231: strcpy_P(ch_status_description,  PSTR("CV Nadraaien"));   break;
      case 999: strcpy_P(ch_status_description,  PSTR("Status 999"));  break;
      default:  sprintf_P(ch_status_description, PSTR("Code %d"), ch_status_code);
      }
      // Convert input buffer to readable format
      for (int i = 0; i < 32; i++) {
         ch_input_buffer[3*i] = c2h((inputBuffer[i] & 0xf0) >> 4);
         ch_input_buffer[3*i + 1] = c2h(inputBuffer[i] & 0x0f);
         ch_input_buffer[3*i + 2] = ' ';
      }
   }
}

void sendTimestamp() {
   char   scratch[35];
   time_t utc, t;

   //Central European Time (Frankfurt, Paris)
   TimeChangeRule CEST = {"CEST", Last, Sun, Mar, 2, 120};     //Central European Summer Time
   TimeChangeRule CET  = {"CET ", Last, Sun, Oct, 3,  60};       //Central European Standard Time
   Timezone CE(CEST, CET);

   utc = now();    //current time from the Time Library
   t = CE.toLocal(utc);
   sprintf_P(scratch, PSTR("%04d%02d%02d %02d%02d%02d"), year(t), month(t), day(t), hour(t), minute(t), second(t));
   sendString("timestamp", scratch);
}

void sendUptime() {
   char  scratch[35];

   if (upDay > 0)
      sprintf_P(scratch, PSTR("%d days, %02d:%02d:%02d up"), upDay, upHour, upMinute, upSecond);
   else
      sprintf_P(scratch, PSTR("%02d:%02d:%02d up"), upHour, upMinute, upSecond);
   sendString("uptime", scratch);
}

void sendDouble(const char *topic, double payload) {
   char scratch[10];
   char t[100];

   strcpy_P(t, PSTR(MQTT_TopicRoot));
   strcat(t, topic);
   ftoa(scratch, payload, 2);
   Serial.print(t); Serial.print(F(": "));Serial.println(scratch);
   mqttClient.publish(t, scratch);
}

void sendUint(const char *topic, uint payload) {
   char t[100];
   char scratch[10];

   strcpy_P(t, PSTR(MQTT_TopicRoot));
   strcat(t, topic);
   sprintf(scratch, "%u", payload);
   Serial.print(t); Serial.print(F(": "));Serial.println(scratch);
   mqttClient.publish(t, scratch);
}

void sendString(const char *topic, char *payload) {
   char t[100];

   strcpy_P(t, PSTR(MQTT_TopicRoot));
   strcat(t, topic);
   Serial.print(t); Serial.print(F(": "));Serial.println(payload);
   mqttClient.publish(t, payload);
}

void sendByte(const char *topic, unsigned char payload) {
   char t[100];

   strcpy_P(t, PSTR(MQTT_TopicRoot));
   strcat(t, topic);
   char scratch[10];

   Serial.print(t); Serial.print(F(": "));Serial.println(payload);
   sprintf(scratch, "%d", payload);
   mqttClient.publish(t, scratch);
}

void sendStatus() {
      // Send the variables to the MQTT broker
   if (messageReceived) {
      messageReceived = false;
      sendTimestamp();
      sendUptime();
      sendDouble("t1", ch_t1);
      sendDouble("t2", ch_t2);
      sendDouble("t3", ch_t3);
      sendDouble("t4", ch_t4);
      sendDouble("t5", ch_t5);

      sendDouble("fan-speed",            ch_fanspeed);
      sendDouble("fan-speed-setpoint",   ch_fanspeed_setpoint);
      sendDouble("fan-pwm",              ch_fan_pwm);
      sendDouble("ionisation-current",   ch_io_current);
      sendDouble("pressure",             ch_pressure);
      sendDouble("temperature-flow",     ch_temp_flow);
      sendDouble("temperature-setpoint", ch_temp_setpoint);

      sendUint("status-code", ch_status_code);
      sendString("status-description", ch_status_description);
      sendUint("fault-code", ch_fault_code);

      sendByte("status-byte1", ch_status_byte1);
      sendByte("status-byte2", ch_status_byte2);

      sendString("ch_input_buffer", ch_input_buffer);
   }
}

void sendNTPpacket(IPAddress ntpServer) {
      // send an NTP request to the NTP time server at the given address
  debugln("Sending NTP packet.");
  memset(packetBuffer, 0, NTP_PacketSize);
  // Initialize values needed to form NTP request (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;            // Stratum, or type of clock
  packetBuffer[2] = 6;            // Polling Interval
  packetBuffer[3] = 0xEC;         // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12]  = 49;
  packetBuffer[13]  = 0x4E;
  packetBuffer[14]  = 49;
  packetBuffer[15]  = 52;

  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  ntp.beginPacket(ntpServer, NTP_UDPPort);
  ntp.write(packetBuffer,NTP_PacketSize);
  ntp.endPacket();
}

time_t getNtpTime() {
  int iBytes;

  sendNTPpacket(ntpIP); // send an NTP packet to a time server

  // wait for a reply / timeout after 10 seconds
  iBytes = 0;
  while (ntp.parsePacket() != 48 && iBytes < 2000) {
    iBytes++;
    delay(5);
  }

  if (ntp.available() == 48)  {
    Serial.print(F("Time synchronized in "));
    Serial.print(iBytes*5);
    Serial.println(F(" ms"));

    ntp.read(packetBuffer,NTP_PacketSize);  // read the packet into the buffer

    // The timestamp starts at byte 40 of the received packet and is four bytes,
    // or two words, long. First, esxtract the two words:

    unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
    unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);

    // combine the four bytes (two words) into a long integer
    // this is NTP time (seconds since Jan 1 1900):
    unsigned long secsSince1900 = highWord << 16 | lowWord;
    const unsigned long seventyYears = 2208988800UL - TimeZoneOffset;
    unsigned long epoch = secsSince1900 - seventyYears;
    return epoch;
  } else {
    debug("ntp available(): "); debugln(ntp.available());
    debugln(F("No NTP packet received"));
    return 0;
  }
}

// uptime is from: https://hackaday.io/project/7008-fly-wars-a-hackers-solution-to-world-hunger/log/25043-updated-uptime-counter
void uptime() {
   // ** Making Note of an expected rollover *****//
   long m = millis();
   if (m >= 3000000000){
      upHighMillis = 1;
   }
   //** Making note of actual rollover **//
   if (m <= 100000 && upHighMillis == 1) {
      upRollover++;
      upHighMillis = 0;
   }

   long secsUp = m/1000;
   upSecond = secsUp%60;
   upMinute = (secsUp/60)%60;
   upHour = (secsUp/(60*60))%24;
   upDay = (upRollover*50)+(secsUp/(60*60*24));  //First portion takes care of a rollover [around 50 days]
}

void loop() {
  uptime();
  if (!mqttClient.connected()) {
     reconnect();
     // Get a new NTP server IP address
//     WiFi.hostByName(ntpServerName, ntpIP);
//     Serial.print(F("NTP IP address: "));
//     Serial.println(ntpIP);
  }
  mqttClient.loop();

  long timeInMillis = millis();
  if (timeInMillis - lastMsg > scanPeriod) {
     printFreeHeap();
//     debug(F("now: ")); debugln(timeInMillis);
     lastMsg = timeInMillis;
     requestStatus();
  }
  readStatus();
  processStatus();
  sendStatus();
}
