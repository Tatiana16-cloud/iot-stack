# ESP32 IoT Sensor - Firmware Documentation

This project contains the firmware for an **ESP32-based IoT Sensor device** (`ESP2`) that integrates with a central Catalog service and communicates via MQTT. It measures temperature and humidity (DHT22) and controls a servo motor based on alerts or manual commands.

## Features

-   **Dynamic Configuration**: Connects to a central **Catalog Service** (REST API) to retrieve its configuration (Room ID, User ID).
-   **Catalog Registration**: Automatically registers/updates its metadata (MQTT topics, timestamps) in the Catalog upon boot.
-   **MQTT Telemetry**: Publishes temperature and humidity data using the **SenML** format.
-   **Remote Control**: Subscribes to actuator topics to control a Servo motor (simulating a fan).
-   **Alert Handling**: Listens for alerts (e.g., temperature out of range) to automatically trigger the actuator.
-   **HTTPS Support**: Handles secure connections (with `setInsecure()` for development environments like Cloudflare Tunnels/ngrok).
-   **Wokwi Simulation**: Configured for easy simulation in Wokwi with VS Code.

## Architecture & Communication

### 1. Boot Sequence
1.  **WiFi Connection**: Connects to the configured SSID.
2.  **Catalog Identity Resolution**:
    *   GET `/rooms`: Iterates through all rooms to find which room contains this device (`DEVICE_ID`).
    *   Sets internal `userId` and `roomId` based on the response.
    *   *Fallback*: If Catalog is unreachable, defaults to hardcoded fallback IDs (configurable).
3.  **Topic Construction**: Dynamically builds MQTT topics using the resolved `userId` and `roomId`.
4.  **Catalog Update (Heartbeat)**:
    *   PATCH `/devices/{DEVICE_ID}`: Updates the device's entry with the current `timestamp`, `topic_pub`, and `topic_sub`.
5.  **MQTT Connection**: Connects to the broker and subscribes to command/alert topics.

### 2. MQTT Topics

| Type | Topic Structure | Description |
| :--- | :--- | :--- |
| **Pub** | `SC/{User}/{Room}/dht` | Telemetry: Temp & Hum (SenML) |
| **Pub** | `SC/{User}/{Room}/ServoDHT` | Actuator State (SenML) |
| **Pub** | `SC/{User}/{Room}/down` | Device Status/Diagnostics (JSON) |
| **Sub** | `SC/alerts/{User}/{Room}/dht` | Direct alerts for this device |
| **Sub** | `SC/alerts/+/+/dht` | Wildcard alerts (broadcast) |
| **Sub** | `SC/{User}/{Room}/sampling` | Enable/Disable monitoring |

## Configuration (Sensors.ino)

All main settings are at the top of `Sensors.ino`:

```cpp
// ---- Wi-Fi & MQTT ----
#define WIFI_SSID               "Wokwi-GUEST"
#define MQTT_BROKER_HOST        "test.mosquitto.org"

// ---- Catalog ----
#define DEVICE_ID               "ESP2"
#define CATALOG_BASE_URL        "https://your-tunnel-url.trycloudflare.com" 

// ---- Flags (Development) ----
#define USE_WIFI                1
#define USE_CATALOG_LOOKUP      1  // Set to 0 to skip Catalog for offline testing
#define USE_MQTT                1
```

## How to Run (Wokwi Simulation)

1.  **Prerequisites**:
    *   VS Code with **Wokwi for VS Code** extension.
    *   **Cloudflare Tunnel** (or similar) exposing your local Catalog service to the internet (Wokwi runs in the cloud).

2.  **Setup Tunnel**:
    Run `cloudflared` to expose your local Catalog (port 9080):
    ```bash
    cloudflared tunnel --url http://localhost:9080
    ```
    Copy the resulting URL (e.g., `https://random-name.trycloudflare.com`) and paste it into `CATALOG_BASE_URL` in `Sensors.ino`.

3.  **Compile & Run**:
    *   Open `Sensors.ino`.
    *   Build/Upload using the Wokwi simulator button or Arduino CLI.
    *   The simulation will start, connect to WiFi, resolve its ID from your Catalog, and begin publishing.

## Dependencies

-   `WiFi.h`, `HTTPClient.h`, `WiFiClientSecure.h` (ESP32 Core)
-   `PubSubClient` (MQTT)
-   `ArduinoJson` (JSON Parsing)
-   `DHT sensor library for ESPx` (Sensors)
-   `ESP32Servo` (Actuators)

## Clean Code Principles

The firmware follows a modular structure:
-   **Configuration**: Centralized `#define` macros.
-   **Network Helpers**: `httpGet`, `httpPatch` wrappers for clean HTTP/HTTPS handling.
-   **Catalog Logic**: Separated functions (`resolveIdentityFromCatalog`, `updateDeviceInCatalog`).
-   **Application Logic**: Distinct handlers for sensors and MQTT callbacks.
