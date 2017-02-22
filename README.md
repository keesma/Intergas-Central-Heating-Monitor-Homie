# Intergas Central Heating Monitor for Homie 2.0

This is an update of the previous. The functionality is almost the same but the source is much more simple because of Homie.

* This program can read the the status of a Central heating from Intergas.
  It has been built for an esp8266. The central heating status is sent through MQTT to a central system (MQTT broker).
  
* Configuration is done using the configuration mode Homie. No need to add passwords to the code anymore.

* Connect esp8266 to Intergas (is the same as previous version)

  I have included a diagram how to connect the optocouplers (and a picture of the prototype).

  To connect the central heating to the esp8266.
  It is best to use an optocoupler to connect the esp8266 to the Intergas.
  E.g. an 4n25 can be used (take two 4n25s to protect both tx and rx).
  I got good results by using a 220 Ohm resistor for the input and a 1k Ohm resistor in the output.

  Default config on the esp8266 is:
  - pin 4: Rx
  - pin 5: Tx
  - pin 12: LED. The LED is on during initialization and while sending data to the Intergas.
  The communication speed is 9600 baud.

  The intergas has a 4 pin plug with: Vcc, ground, Tx and Rx.

* Dependencies
  - Homie 2.0: [https://github.com/marvinroger/homie-esp8266] (not officially released)
  
* Openhab: I have connected the esp8266 through MQTT to openhab. Openhab can display the data, save it and create nice graphs. The item definitions are included. The rules are required for translating the status bytes to (bit) values.

* Time: the time is no longer synchronised with an NTP server (server (or pool) name can be configured in config.h). Homie has a timer that counts how many seconds it has been running since the last reset.

* Power: My esp8266 has a separate 3.3V power supply. The Intergas may supply power but prefer not the experiment with that (it only supplies power to one of the optocouplers).
