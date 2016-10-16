# Central-Heating-Intergas-Reader

* This program can read the the status of a Central heating from Intergas.
  It has been built for an esp8266.
  The central heating status is sent through MQTT to a central system (MQTT broker).
  
* Beta version: This version has been tested for several days on a breadboard.
  It has been tested with an Intergas Prestige CW6.

* Configuration
  The following needs to be configured (see config.h):
  -- WiFi access point: SSID and password
  -- MQTT broker server name; username and password authentication is used

* Connect esp8266 to Intergas

  To connect the central heating to the esp8266.
  It is best to use an optocoupler to connect the esp8266 to the Intergas.
  E.g. an 4n25 can be used (take two 4n25s to protect both tx and rx).
  Good results were delivered by a 220 Ohm resistor for the input and a 1kOhm resistor in the output.

  [to do: make a small drawing]

  Default config on the esp8266 is:
  pin 4: Rx
  pin 5: Tx
  pin 12: LED. The LED is on during initialization and while sending data to the Intergas.

  The intergas has a 4 pin plug with: Vcc, ground, Tx and Rx.
  The intergas is connected through an optocoupler to the esp8266 (2x 4n25)

* Dependencies
  -- MQTT: PubSubClient for communication with MQTT broker
  -- Timezone, Timelib: for time calculations
  -- SoftwareSerial: for serial communications (so the intergas serial communication does not interfere with firmware loading and info/debug messages)
  -- WifiUdp: for communcation for UDP

* Openhab: I have connected the esp8266 through MQTT to openhab. Openhab can display the data, save it and create nice graphs.

* Time: the time is synchronised with an NTP server (server name can be configured in config.h). A timer runs how long the program has been running, so you can see if it was restarted unexpectedly.
