#ifndef CHOFU_MQTT_H
#define CHOFU_MQTT_H
#include "globals.h"

// Declaratie MET default-parameter — definitie in mqtt.cpp ZONDER default
void mqtt_log(String message, String level = "INFO");
void stuur_alert(String msg);
void mqtt_proto(const char* subtopic, uint8_t* buf, uint8_t len, const String& extra = "");
void check_mqtt_watchdog();
void seriallog_add(const String& msg);   // voeg regel toe aan ringbuffer
void seriallog_flush();                  // publiceer ringbuffer als één bericht
void mqtt_ontvang(int len);
void disco_pub(const char* topic, String& pl);
void discovery_fase1();
void discovery_fase2();
void discovery_fase3();
void stuur_data();
#endif // CHOFU_MQTT_H
