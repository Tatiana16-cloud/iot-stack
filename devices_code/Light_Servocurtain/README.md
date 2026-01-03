# ESP32 Smart Light & Curtain Actuator

This project contains the firmware for an **ESP32-based Actuator device** (`ESP3`) that controls a light (LED) and a curtain (Servo), and monitors ambient light levels (Photoresistor/Potentiometer). It integrates with a central Catalog service for dynamic configuration.

## Features

-   **Dynamic Configuration**: Connects to a central **Catalog Service** (REST API) via Cloudflare Tunnel/ngrok to retrieve its configuration (`RoomID`, `UserID`).
-   **Smart Actuation**:
    *   **Curtain Control**: Servo motor (Open/Close).
    *   **Lighting**: LED control (On/Off).
-   **Telemetry**: Publishes ambient light levels (simulated with Potentiometer) via MQTT.
-   **Catalog Registration**: Automatically registers/updates its metadata (MQTT topics, timestamps) in the Catalog upon boot.
-   **Secure Communication**: Supports HTTPS (insecure mode) for development tunnels.
-   **Wokwi Compatible**: Ready for simulation with VS Code.

## Architecture

### 1. Boot Flow
1.  **WiFi Connection**: Connects to the configured SSID.
2.  **Catalog Identity Resolution**:
    *   GET `/rooms`: Iterates through all rooms to find which room contains this device (`DEVICE_ID`).
    *   Sets internal `userId` and `roomId`.
3.  **Topic Construction**: Dynamically builds MQTT topics using the resolved `userId` and `roomId`.
4.  **Catalog Update (Heartbeat)**:
    *   PATCH `/devices/{DEVICE_ID}`: Updates the device's entry with the current `timestamp`, `topic_pub`, and `topic_sub`.
5.  **MQTT Connection**: Connects to the broker and subscribes to command topics.

### 2. MQTT Topics

| Type | Topic Structure | Description |
| :--- | :--- | :--- |
| **Pub** | `SC/{User}/{Room}/Light` | Ambient Light Level (SenML) |
| **Pub** | `SC/{User}/{Room}/LedL` | LED State (SenML) |
| **Pub** | `SC/{User}/{Room}/servoCurtain` | Curtain State (SenML) |
| **Pub** | `SC/{User}/{Room}/down` | Device Status/Diagnostics (JSON) |
| **Sub** | `SC/{User}/{Room}/LedL` | Command to toggle LED |
| **Sub** | `SC/{User}/{Room}/servoCurtain` | Command to Open(90)/Close(0) Curtain |
| **Sub** | `SC/{User}/{Room}/sampling` | Enable/Disable light telemetry |

## Configuration

Settings are located at the top of `Light_Servocurtain.ino`. Ensure `CATALOG_BASE_URL` matches your active tunnel.

```cpp
// ---- Catalog ----
#define DEVICE_ID               "ESP3"
#define CATALOG_BASE_URL        "https://your-tunnel-url.trycloudflare.com"

// ---- Hardware ----
#define PIN_LED                 4
#define PIN_SERVO               15
#define PIN_POT                 34
```

## How to Run (Wokwi Simulation)

1.  **Start Tunnel**: Expose your local Catalog service (port 9080) to the internet.
    ```bash
    cloudflared tunnel --url http://localhost:9080
    ```
2.  **Update Firmware**: Copy the generated HTTPS URL into `CATALOG_BASE_URL` in `Light_Servocurtain.ino`.
3.  **Compile**: Use the Wokwi extension button or Arduino CLI.
    *   *Note*: The `wokwi.toml` is configured to look for the compiled binary in `build/`.
4.  **Simulate**: Start the simulation.
    *   The device will connect to WiFi.
    *   It will query the Catalog to find its User/Room.
    *   If successful, it connects to MQTT and starts operation.
    *   *Troubleshooting*: If you see `connection refused`, verify your tunnel URL and restart the simulation.

## Hardware Connections

-   **Servo**: Pin 15 (PWM)
-   **LED**: Pin 4 (Active High)
-   **Potentiometer**: Pin 34 (Analog Input)

## Dependencies

-   `WiFi.h`, `HTTPClient.h`, `WiFiClientSecure.h` (ESP32 Core)
-   `PubSubClient` (MQTT)
-   `ArduinoJson` (JSON Parsing)
-   `ESP32Servo` (Servo Control)
