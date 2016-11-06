/*
 * config.h: defines local settings.
 */

const char *ssid = "<your ssid>";
const char *ssidPassword = "<your ssid password>";

const char *mqttServer   = "<your mqtt broker server name>";
const char *mqttClientId = "<your mqtt client id>";
const char *mqttUsername = "<your mqtt user name>";
const char *mqttPassword = "<your mqtt password>";

#define MQTT_TopicRoot  "chig1/"                 // Central Heating InterGas 1

const char ntpServerName[] = "nl.pool.ntp.org";  // NTP server pool (http://www.pool.ntp.org/zone)
