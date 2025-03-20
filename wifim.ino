#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager

void wifim() {
    const char* AP_NAME = "AutoConnectAP";  // Local constant for WiFi AP name
    const char* AP_PASSWORD = "password";   // Local constant for WiFi AP password

    Serial.begin(115200);

    WiFiManager wm;  // Local WiFiManager instance

    // reset settings - wipe stored credentials for testing
    // these are stored by the ESP library
    // wm.resetSettings();

    bool res;

    // Automatically connect using saved credentials
    // res = wm.autoConnect(); // Auto-generated AP name from chip ID
    // res = wm.autoConnect("AutoConnectAP"); // Anonymous AP (no password)
    res = wm.autoConnect(AP_NAME, AP_PASSWORD); // Password-protected AP

    if (!res) {
        Serial.println("Failed to connect");
        // ESP.restart(); // Uncomment if you want to restart on failure
    } else {
        Serial.println("Connected... yeey :)");
    }
}
