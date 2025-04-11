// --- File: YourSketchName.ino ---
// Main application logic.

#include <Wire.h>                   // For I2C communication
#include <FluxGarage_RoboEyes.h>    // For eye state enums (e.g., HAPPY, N)
#include <Arduino.h>                // For FreeRTOS functions (vTaskDelay)
#include "SPIFFS.h"                 // For your filesystem operations

// --- Forward declarations for functions defined elsewhere ---
// Your existing functions
extern void wifim();
extern String recordAndTranscribe();
extern String askQuestion(String transcript);
extern void playTTS(String response);
// Functions defined in robo_eyes_task.ino
extern bool setupEyeSystem();
extern void setEyeMood(roboEyesExpression mood);
extern void setEyePosition(roboEyesPosition position);
extern void setEyeBlink(bool enabled);
extern void setEyeIdle(bool enabled);
extern void setEyesEnabled(bool enabled);
extern void reactLoudNoise(); // Example reaction
extern void reactPetting();   // Example reaction
// Add extern declarations for your other custom reactions if needed


// --- Setup Function ---
// Runs once on boot, performs initialization and main sequence.
void setup() {
  Serial.begin(115200);
  Serial.println("\n--- Robot Initializing ---");

  // 1. --- Initialize I2C and Eye System ---
  Wire.begin(); // Start I2C using default pins (see table)
  Serial.println("Initializing Eye System...");
  if (!setupEyeSystem()) { // Start background eye task
      Serial.println("FATAL: Eye System Failed! Halting.");
      // Indicate critical failure (e.g., blink built-in LED rapidly)
      pinMode(LED_BUILTIN, OUTPUT);
      while(1) { digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); delay(100); }
  }
  // Set initial eye state immediately after setup succeeds
  setEyesEnabled(true);   // Make eyes visible
  setEyeMood(DEFAULT);    // Normal expression
  setEyePosition(DEFAULT); // Look forward
  Serial.println("Eye System Running.");


  // 2. --- Your Main Setup Sequence with Integrated Eye Feedback ---
  Serial.println("Preparing Storage (SPIFFS)...");
  // Optional: Show 'thinking/working' eyes during storage check
  // setEyeMood(CURIOUS);
  if (!SPIFFS.begin(true)) {
    Serial.println("Storage preparation FAILED!");
    setEyeMood(ANGRY); // Show error state on eyes
    setEyePosition(S);  // Look down sadly
    while (1) delay(1000); // Halt on storage failure
  } else {
    Serial.println("Storage ready.");
    // setEyeMood(DEFAULT); // Back to normal if you set CURIOUS above
  }

  Serial.println("Initializing WiFi...");
  setEyeMood(CURIOUS); // Indicate working/connecting state
  setEyePosition(E);   // Look aside
  wifim();             // Call your WiFi function (eyes update in background)
  setEyeMood(DEFAULT); // Back to normal after connection
  setEyePosition(DEFAULT);
  Serial.println("WiFi initialized.");


  Serial.println("Starting Record & Transcribe...");
  // reactPetting(); // Example: Make eyes happy before listening
  setEyeMood(DEFAULT); // Or a custom LISTENING mood
  setEyePosition(N);   // Look up attentively
  String result = recordAndTranscribe(); // Eyes run in background during this long step


  if (result.length() > 0) {
    Serial.println("Transcript: " + result);
    Serial.println("Asking AI question...");
    setEyeMood(DEFAULT); // Or a THINKING mood
    setEyePosition(W);   // Look other way while processing
    String response = askQuestion(result); // Eyes run in background

    Serial.println("AI Response: " + response);
    Serial.println("Playing TTS response...");
    setEyeMood(HAPPY); // Happy/neutral while speaking
    setEyePosition(DEFAULT);
    playTTS(response); // Eyes run in background

  } else {
    Serial.println("Transcription failed.");
    reactLoudNoise(); // Example: Use angry reaction for failure
  }

  Serial.println("--- Main Setup Sequence Complete ---");
  setEyeMood(DEFAULT); // Reset to default state
  setEyePosition(DEFAULT);

} // --- End of setup() ---


// --- Main Loop ---
// Essential for FreeRTOS stability, even if setup does all the work.
void loop() {
  // This allows the FreeRTOS scheduler to run background tasks, including the EyeTask.
  // It prevents watchdog resets and ensures smooth operation.
  vTaskDelay(pdMS_TO_TICKS(100)); // Yield CPU for 100 milliseconds
}
// --- End of File: YourSketchName.ino ---
