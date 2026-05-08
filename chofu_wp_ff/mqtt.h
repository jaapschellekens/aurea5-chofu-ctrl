#pragma once
#include "globals.h"

// Declaratie MET default-parameter — definitie in mqtt.cpp ZONDER default
void mqtt_log(String message, String level = "INFO");
void stuur_alert(String msg);
void check_mqtt_watchdog();
void mqtt_ontvang(int len);
void disco_pub(const char* topic, String& pl);
void discovery_fase1();
void discovery_fase2();
void discovery_fase3();
void stuur_data();
