/*
 * cvintergas - read status from Intergas Central Heating and publish it through MQTT
 *
 * Based on info Leo van der Kallen: http://www.circuitsonline.net/forum/view/80667/last/intergas
 * Time is no longer synchronized using NTP.
 *
 * MQTT topics
 *
 * - settings
 *   -devices/<deviceid>/
 *      - scan-period - how often is status read from Intergas (in seconds)
 *      - sync-period - how often time is synchronized (in seconds)
 *      - send-raw-data - determines whether the raw data is send
 * - sensor data
 *   - devices/igchm/...
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
 *     - pump-running
 *     - roomtherm
 *     - opentherm
 *     - status
 *     - input-buffer
 *     - flow temperature (external sensor)
 *     - return temperature (external sensor)
 *
 * todo
 * - implement other messages: VER, CRC, REV, EN
 * - set scan period long when CH is idle, and short when active
 *
 * Revision history
 * 0.05 201610xx  First public release
 * 0.1x 20161105  Added two new status codes
 *                Added parameterto enable/disable logging of raw data
 *                Increased default scan period from 5.000 to 10.000 ms]
 * 0.2x 20161217  Converted to Homie framework
 * 0.3x 20161230  Converted to Homie v2.0
 * 0.4x 20170103  Added 2 external temperature sensors (flow & return)
 *                Added simulation by other central heating monitor (conditional)
 *
 */

#include <Homie.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <SoftwareSerial.h>

#define FW_NAME       "homie-ch"
#define FW_VERSION    "0.4.36"

#define DEBUG  0

#if DEBUG==1
//#define debugln(s)    Serial.println(s)
//#define debug(s)      Serial.print(s)
#else
#define debugln(s)
#define debug(s)
#endif

HomieNode temperatureNode("temperature", "temperature");
HomieNode fanNode("fan", "fan");
HomieNode pressureNode("pressure", "pressure");
HomieNode centralHeatingNode("heating","heating");
HomieSetting<const char *> flowTAddress("Flow sensor address","Flow temperature sensor address");
HomieSetting<const char *> returnTAddress("Return sensor address","Return temperature sensor address");
#ifdef INTERGAS_SIMULATE
HomieSetting<bool> simulateCentralHeating("Simulate Central Heating", "Simulate the central heating by receiving a dummy response (for testing purposes)");
#endif

bool messageReceived;
bool waitingForAnswer;

#define InputBufferLen   64
#define StatusMsgLen     32

byte inputBuffer[InputBufferLen];
#ifdef INTERGAS_SIMULATE
byte inputBufferSim[] = { 0x65, 0x0c, 0x50, 0x0c, 0x46, 0x0c, 0x77, 0x0a, 0x97, 0xf3,
                          0x96, 0xf3, 0x58, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                          0x00, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x41, 0x40, 0x00, 0xff,
                          0x00, 0xff };
#endif

int writeIndex;
#define PinLED            12

#define ONE_WIRE_BUS    14  // DS18B20 pin
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature DS18B20(&oneWire);
DeviceAddress t1Address, t2Address;
bool t1Present = false, t2Present = false;
char t1AddressString[24], t2AddressString[24];
char t1Name[15];
char t2Name[15];

SoftwareSerial intergas(4, 5, false ); // RX, TX, not inverted

#define TimeoutIntergas  (3*1000) // in milliseconds

long  receiveTimer;
long  scanPeriod;
bool  sendRawData;

double  ch_pressure;
unsigned int ch_status_code, ch_fault_code;
bool         ch_has_pressure_sensor, ch_opentherm;
char         ch_input_buffer[3*StatusMsgLen+5];      // ASCII bytes with spaces

byte toHex(char c) {
   byte  result = 0;
   if (c >= '0' && c <= '9') {
      result = c - '0';
   } else if (tolower(c) >= 'a' && tolower(c) <= 'f') {
      result = c - 'a' + 0x0a;
   } else {
      // Error
      Homie.getLogger() << "ERROR: converting character to hex " << c << endl;
   }
   return result;
}

byte toHex(char c1, char c2) {
   byte result = toHex(c1) << 4 + toHex(c2);
   return result;
}

#ifdef INTERGAS_SIMULATE
bool bufferInputHandler(HomieRange range, String value) {
   for (int i = 0; i < 32; i++) {
      ch_input_buffer[i] = toHex(value.charAt(3*i),value.charAt(3*i+ 1));
   }
   waitingForAnswer = false;
   messageReceived = true;
   return true;
}
#endif

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
}

void ledOff() {
  digitalWrite(PinLED, LOW);    // turn the LED on by making the voltage HIGH
}

long lastMsg = 0;

bool centralHeatingPeriodHandler(const HomieRange& range, const String& value) {
  long period;
  period = value.toInt();
  if (period > 1) {
     scanPeriod = period*1000;  // in milliseconds
     centralHeatingNode.setProperty("scan-period").send(String(period));
     return true;
  } else {
     return false;
  }
}

bool centralHeatingRawDataHandler(const HomieRange& range, const String& value) {
  if (value == "true") {
    centralHeatingNode.setProperty("send-raw-data").setRetained(true).send("true");
    sendRawData = true;
  } else if (value == "false") {
    centralHeatingNode.setProperty("send-raw-data").send("false");
    sendRawData = false;
  } else {
    return false;
  }
  return true;
}

void intergasFlush() {
     // Flush already received characters.
   while (intergas.available() > 0) {
      intergas.read();
   }
}

void requestStatus() {
  ledOn();
//  Serial.println(F("Request status from Intergas"));
  Homie.getLogger() << "Request status from Intergas" << endl;
  waitingForAnswer = true;
  intergasFlush();
  intergas.write('S');
  intergas.write('?');
  intergas.write('\r');
  receiveTimer = millis();
  ledOff();
#ifdef INTERGAS_SIMULATE
  if (simulateCentralHeating.get()) {
    int i;
    Homie.getLogger() << "Simulate status from Intergas" << endl;

    waitingForAnswer = false;
    messageReceived = true;
    for (i = 0; i < 32; i++) inputBuffer[i] = inputBufferSim[i];
  }
#endif
}

char c2h(char c){
  return "0123456789ABCDEF"[0x0F & (unsigned char)c];
}

bool centralHeatingHandler(String value) {
  return false;
}

void readStatus(){
      // read status message from central heating
   byte  ch;
   long  t;

   if (waitingForAnswer) {
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
              waitingForAnswer = false;
              messageReceived = true;
            }
         }
      } else {
         writeIndex = 0;
         waitingForAnswer = false;
         Serial.println(F("Timeout Intergas"));
      }
   }
}

double getDouble(byte msb, byte lsb) {
      // Calculate float value from two bytes
   double  result;
//   double  res1;

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
      float  temperature;
      char   scratch[100];

      messageReceived = false;
      temperatureNode.setProperty("t1").send(ftoa(scratch, getDouble(inputBuffer[1],  inputBuffer[0]), 2));
      temperatureNode.setProperty("t2").send(ftoa(scratch, getDouble(inputBuffer[5],  inputBuffer[4]), 2));
      temperatureNode.setProperty("t3").send(ftoa(scratch, getDouble(inputBuffer[7],  inputBuffer[6]), 2));
      temperatureNode.setProperty("t4").send(ftoa(scratch, getDouble(inputBuffer[9],  inputBuffer[8]), 2));
      temperatureNode.setProperty("t5").send(ftoa(scratch, getDouble(inputBuffer[11], inputBuffer[10]), 2));
      temperatureNode.setProperty("flow").send(ftoa(scratch, getDouble(inputBuffer[3], inputBuffer[2]), 2));
      temperatureNode.setProperty("setpoint").send(ftoa(scratch, getDouble(inputBuffer[15], inputBuffer[14]), 2));
      fanNode.setProperty("speed").send(ftoa(scratch, getDouble(inputBuffer[19],  inputBuffer[18])*100, 0));
      fanNode.setProperty("setpoint").send(ftoa(scratch, getDouble(inputBuffer[17],  inputBuffer[16])*100, 0));
      fanNode.setProperty("pwm").send(ftoa(scratch, getDouble(inputBuffer[21],  inputBuffer[20])*10, 0));

      ch_pressure = getDouble(inputBuffer[13],  inputBuffer[12]);
      ch_has_pressure_sensor    = (inputBuffer[28] & 0x20) == 0x20;
      ch_opentherm  = (inputBuffer[26] & 0x80) == 0x80;
      if (!ch_has_pressure_sensor) {
         ch_pressure   = -35;
      }
      centralHeatingNode.setProperty("io-current").send(ftoa(scratch, getDouble(inputBuffer[23],  inputBuffer[22]), 0));
      pressureNode.setProperty("pressure").send(ftoa(scratch, ch_pressure, 0));
      sprintf(scratch, "%u %u",inputBuffer[26], inputBuffer[28]);
      centralHeatingNode.setProperty("status").send(scratch);
      centralHeatingNode.setProperty("opentherm").send((inputBuffer[26] & 0x80) == 0x80 ? "true" : "false");

      ch_status_code = inputBuffer[24];
      if ((inputBuffer[27] & 0x80 != 0x80)) {
         ch_fault_code  = (unsigned int)inputBuffer[29];
         ch_status_code = 256 + ch_fault_code;
      } else {
         ch_fault_code = 0;
      }
      sprintf(scratch, "%u", ch_fault_code);
      centralHeatingNode.setProperty("fault-code").send(scratch);
      sprintf(scratch, "%u", ch_status_code);
      centralHeatingNode.setProperty("status-code").send(scratch);
      centralHeatingNode.setProperty("pump-running").send(((inputBuffer[26] & 0x08) == 0x08 ? "true" : "false"));
      if (sendRawData) {
        for (int i = 0; i < 32; i++) {
           ch_input_buffer[3*i] = c2h((inputBuffer[i] & 0xf0) >> 4);
           ch_input_buffer[3*i + 1] = c2h(inputBuffer[i] & 0x0f);
           ch_input_buffer[3*i + 2] = ' ';
        }
        centralHeatingNode.setProperty("input-buffer").send(ch_input_buffer);
      }

      DS18B20.requestTemperatures();
      if (t1Present) {
        temperature = DS18B20.getTempCByIndex(0);
        temperatureNode.setProperty(t1Name).send(String(temperature));
        Homie.getLogger() << "T1: " << temperature << endl;
      }
      if (t2Present) {
        temperature = DS18B20.getTempCByIndex(1);
        temperatureNode.setProperty(t2Name).send(String(temperature));
        Homie.getLogger() << "T2: " << temperature << endl;
      }
	}
}

void setupHandler() {
   const char *t;

   DS18B20.begin();
   Serial.print("Found "); Serial.print(DS18B20.getDeviceCount()); Serial.println(" sensors");
     Serial.print("Sensor flow config: ["); Serial.print(flowTAddress.get()); Serial.println("]");
     Serial.print("Sensor return config: ["); Serial.print(returnTAddress.get()); Serial.println("]");
     if (DS18B20.getAddress(t1Address, 0)) {
        t1Present = true;
//        Homie.getLogger() << "Temperature sensor 1: ";
        Serial.print("Temperature sensor 1: ");
        addressToString(t1Address, t1AddressString);
//        Homie.getLogger() << t1AddressString << endl;
        Serial.println(t1AddressString);;
        t = flowTAddress.get();
        if (t != 0) {
          if (!strncmp(t,t1AddressString,23)) {
             strcpy(t1Name, "flow-ext");
          } else if (!strncmp(t,t2AddressString,23)) {
             strcpy(t1Name, "return-ext");
          } else {
             Homie.getLogger() << "Warning: unknown sensor address" << endl;
          }
        } else {
          Homie.getLogger() << "T1 sensor addresss config is empty" << endl;
        }
     }
     if (DS18B20.getAddress(t2Address, 1)) {
        t2Present = true;
        Homie.getLogger() << "Temperature sensor 2: ";
        addressToString(t2Address, t2AddressString);
        Homie.getLogger() << t2AddressString << endl;
        if (!strncmp(returnTAddress.get(),t1AddressString,23)) {
           strcpy(t2Name, "flow-ext");
        } else if (!strncmp(returnTAddress.get(),t2AddressString,23)) {
           strcpy(t2Name, "return-ext");
        } else {
          Homie.getLogger() << "Warning: unknown sensor address" << endl;
        }
     }
   temperatureNode.setProperty("unit").send("°C");
   temperatureNode.setProperty("T1-address").send(t1AddressString);
   temperatureNode.setProperty("T2-address").send(t2AddressString);
   fanNode.setProperty("unit").send("rpm");
   centralHeatingNode.advertise("send-raw-data").settable(centralHeatingRawDataHandler);
   centralHeatingNode.advertise("scan-period").settable(centralHeatingPeriodHandler);
#ifdef INTERGAS_SIMULATE
   centralHeatingNode.advertise("input-buffer").settable(bufferInputHandler);
#endif
}

void addressToString(DeviceAddress deviceAddress, char *s) {
  for (uint8_t i = 0; i < 8; i++) {
    // zero pad the address if necessary
    s[3*i] = c2h(deviceAddress[i] & 0xf00 >> 4);
    s[3*i + 1] = c2h(deviceAddress[i] & 0x0f);
    if (i != 7) s[3*i + 2] = ':';
  }
  s[23] = '\0';
}

void setup() {
   Serial.begin(115200);
//   Serial << endl << endl;
   Serial.println();
   Serial.println();
   Homie_setFirmware(FW_NAME, FW_VERSION);

   pinMode(PinLED, OUTPUT);
   ledOn();
   messageReceived = false;
   waitingForAnswer = false;
   scanPeriod =  10000;  // milliseconds
   sendRawData = false;

   lastMsg = 0;

   Serial.println(F("\nIntergas Central Heating Monitor " FW_VERSION));
   Serial.println(F(__TIMESTAMP__));
   Serial.flush();
   intergas.begin(9600);
   printFreeHeap();
#ifdef INTERGAS_SIMULATE
   simulateCentralHeating.setDefaultValue(false);
#endif
//   Homie.enableLogging(true);
//   Homie.enableBuiltInLedIndicator(false);
//   Homie.disableResetTrigger();

//   Homie.getLogger().setLogging(true);
   Homie.setSetupFunction(setupHandler);
   Homie.setLoopFunction(loopHandler);

   strcpy(t1Name, "t1-ext");
   strcpy(t2Name, "t2-ext");
   strcpy(t1AddressString, "undefined");
   strcpy(t2AddressString, "undefined");
   flowTAddress.setDefaultValue("");
   returnTAddress.setDefaultValue("");
   Homie.setup();
   ledOff();
}

void loopHandler() {
  long timeInMillis = millis();
  if (timeInMillis - lastMsg > scanPeriod) {
     lastMsg = timeInMillis;
     requestStatus();
  }
  readStatus();
  processStatus();
}

void loop() {
  Homie.loop();
}
