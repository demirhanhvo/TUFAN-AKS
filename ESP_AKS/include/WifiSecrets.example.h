#pragma once
// Wi-Fi TEST telemetri yolu ([env:esp32dev_wifi]) gizli ayarları — ŞABLON.
//
//   cp include/WifiSecrets.example.h include/WifiSecrets.h
//
// ve aşağıdaki değerleri doldur. include/WifiSecrets.h .gitignore'dadır;
// şifreleri bu ŞABLONA yazıp commit'leme.

// Telefon hotspot'u (2.4 GHz olmalı — ESP32 5 GHz ağları göremez).
#define WIFI_TEL_SSID     "hotspot-adi"
#define WIFI_TEL_PASSWORD "hotspot-sifresi"

// HiveMQ Cloud → Cluster "Overview": URL + "TLS MQTT" portu (8883).
// Access Management'ta ESP32 için PUBLISH izinli bir kimlik oluştur;
// tarayıcı sayfası için ayrı, yalnız SUBSCRIBE izinli bir kimlik önerilir.
#define MQTT_TEL_BROKER_URI "mqtts://xxxxxxxx.s1.eu.hivemq.cloud:8883"
#define MQTT_TEL_USERNAME   "tufan-aks"
#define MQTT_TEL_PASSWORD   "mqtt-sifresi"

// Konu kökü: satırlar <kök>/tel, durum <kök>/status konusuna gider.
#define MQTT_TEL_TOPIC_BASE "tufan/aks"
