// --- File: robo_eyes_task.ino ---
// Description: Optimized background eye control system using FreeRTOS Queue.
// Provides direct control and pre-defined reaction functions.

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>        // Display driver library
#include <FluxGarage_RoboEyes.h>    // Eye animation library
#include <Arduino.h>                // For FreeRTOS types and functions

// --- Configuration ---
#define SCREEN_WIDTH 128            // OLED display width, pixels
#define SCREEN_HEIGHT 64            // OLED display height, pixels
#define OLED_RESET -1               // Reset pin (-1 if not used)
#define I2C_ADDRESS 0x3c            // *** COMMON I2C ADDRESS - Use 0x3d if this doesn't work ***
#define EYE_UPDATE_RATE_MS 30       // Background task update interval (~33fps)
#define EYE_TASK_STACK 4096         // Stack size in words for the eye task
#define EYE_TASK_PRIORITY 1         // Priority for the eye task (1 is usually fine)
#define EYE_COMMAND_QUEUE_LENGTH 10 // Max number of eye commands that can be waiting

// --- Library Objects ---
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET); // Use SH1107G if that's your chip
roboEyes robotEyes;

// --- Command Queue Handle ---
QueueHandle_t eyeCommandQueue = NULL; // Global handle for the command queue

// --- Internal Command Structures (Implementation Detail) ---
typedef enum { CMD_SET_MOOD, CMD_SET_POSITION, CMD_SET_AUTOBLINK, CMD_SET_IDLEMODE, CMD_SET_ENABLED } EyeCommandType;
typedef struct { EyeCommandType type; union { roboEyesExpression mood; roboEyesPosition position; bool enabled; } value; } EyeCommand;

// --- Function Declarations (Prototypes - Public Interface) ---
// Low-level controls
void setEyeMood(roboEyesExpression mood);
void setEyePosition(roboEyesPosition position);
void setEyeBlink(bool enabled);
void setEyeIdle(bool enabled);
void setEyesEnabled(bool enabled);
// High-level reactions (Add your own reaction prototypes here)
void reactLoudNoise(); // EXAMPLE REACTION
void reactPetting();   // EXAMPLE REACTION
// System setup
bool setupEyeSystem();


// --- Internal Task Function (Handles commands & display updates - Runs in background) ---
void eyeControllerTask(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xUpdateFrequency = pdMS_TO_TICKS(EYE_UPDATE_RATE_MS);
    EyeCommand receivedCommand;
    bool currentlyEnabled = true;

    // Apply initial default settings once task starts
    // Note: robotEyes.begin() must have been called successfully in setupEyeSystem() before this task runs
    robotEyes.setAutoblinker(true, 3, 2);
    robotEyes.setIdleMode(true, 2, 2);
    robotEyes.setMood(DEFAULT);
    robotEyes.setPosition(DEFAULT);

    while (1) { // Task loop
        // 1. Check for incoming commands without blocking
        if (eyeCommandQueue != NULL && xQueueReceive(eyeCommandQueue, &receivedCommand, 0) == pdTRUE) {
            // Process the command immediately
            switch (receivedCommand.type) {
                case CMD_SET_MOOD:      robotEyes.setMood(receivedCommand.value.mood); break;
                case CMD_SET_POSITION:  robotEyes.setPosition(receivedCommand.value.position); break;
                case CMD_SET_AUTOBLINK: robotEyes.setAutoblinker(receivedCommand.value.enabled, 3, 2); break; // Uses default timing params
                case CMD_SET_IDLEMODE:  robotEyes.setIdleMode(receivedCommand.value.enabled, 2, 2); break; // Uses default timing params
                case CMD_SET_ENABLED:
                    currentlyEnabled = receivedCommand.value.enabled;
                    if (!currentlyEnabled) { display.clearDisplay(); display.display(); } // Clear screen if disabled
                    // else { /* Optional: Re-init display state if needed on enable */ }
                    break;
            }
        }

        // 2. Update the eye graphics AND display on screen if enabled
        if (currentlyEnabled) {
             robotEyes.update(); // Prepare the eye graphics in the display buffer
             display.display();  // <<< *** CRITICAL FIX ADDED HERE *** Sends buffer to the OLED screen
        }

        // 3. Wait precisely until the next update interval
        vTaskDelayUntil(&xLastWakeTime, xUpdateFrequency);
    }
}


// --- Function Definitions ---

// --- Low-Level Control Functions ---
// Send commands to the background task via the queue. Non-blocking.
void setEyeMood(roboEyesExpression mood) {
    if (!eyeCommandQueue) return; EyeCommand cmd = {.type = CMD_SET_MOOD, .value.mood = mood};
    xQueueSend(eyeCommandQueue, &cmd, 0); // 0 = Don't block if queue is full
}
void setEyePosition(roboEyesPosition position) {
    if (!eyeCommandQueue) return; EyeCommand cmd = {.type = CMD_SET_POSITION, .value.position = position};
    xQueueSend(eyeCommandQueue, &cmd, 0);
}
void setEyeBlink(bool enabled) {
    if (!eyeCommandQueue) return; EyeCommand cmd = {.type = CMD_SET_AUTOBLINK, .value.enabled = enabled};
    xQueueSend(eyeCommandQueue, &cmd, 0);
}
void setEyeIdle(bool enabled) {
    if (!eyeCommandQueue) return; EyeCommand cmd = {.type = CMD_SET_IDLEMODE, .value.enabled = enabled};
    xQueueSend(eyeCommandQueue, &cmd, 0);
}
void setEyesEnabled(bool enabled) {
    if (!eyeCommandQueue) return; EyeCommand cmd = {.type = CMD_SET_ENABLED, .value.enabled = enabled};
    xQueueSend(eyeCommandQueue, &cmd, 0);
}


// --- High-Level Reaction Functions ---
// Combine low-level calls into meaningful reactions. Add your own!
void reactLoudNoise() {
    // Example sequence: Angry, look down, stop blinking
    setEyeMood(ANGRY);
    setEyePosition(S);
    setEyeBlink(false);
}
void reactPetting() {
    // Example sequence: Happy, centered, blinking and idle on
    setEyeMood(HAPPY);
    setEyePosition(DEFAULT);
    setEyeBlink(true);
    setEyeIdle(true);
}
// --- Define your other reaction function implementations here ---
// void reactLowBattery() { ... }


// --- System Setup Function ---
// Initializes everything needed for the eye system. Call ONCE from main setup().
bool setupEyeSystem() {
    // 1. Create the command queue
    eyeCommandQueue = xQueueCreate(EYE_COMMAND_QUEUE_LENGTH, sizeof(EyeCommand));
    if (!eyeCommandQueue) { Serial.println("E: Eye Queue creation failed!"); return false; }

    // 2. Initialize the OLED display via I2C (Assumes Wire.begin() was called previously in main setup)
    if (!display.begin(I2C_ADDRESS, true)) { // true = reset display
        Serial.println("E: OLED Display initialization failed!");
        vQueueDelete(eyeCommandQueue); eyeCommandQueue = NULL; // Clean up queue
        return false;
    }
    display.setRotation(0); // Adjust 0, 1, 2, 3 if screen is rotated
    display.clearDisplay(); // Clear buffer
    display.display();      // Push empty buffer to screen initially
    Serial.println("OLED Initialized.");

    // 3. Initialize the RoboEyes library instance
    // Note: Assuming robotEyes.begin doesn't return a status, or we're ignoring it.
    robotEyes.begin(&display, SCREEN_WIDTH, SCREEN_HEIGHT);
    Serial.println("RoboEyes Library Initialized.");

    // 4. Create and start the background FreeRTOS task
    BaseType_t taskCreated = xTaskCreate(
        eyeControllerTask,      // Function that implements the task
        "EyeTask",              // Name of the task (for debugging)
        EYE_TASK_STACK,         // Stack size (words)
        NULL,                   // Parameter passed to the task (not used)
        EYE_TASK_PRIORITY,      // Priority of the task
        NULL                    // Task handle (not stored)
    );

    if (taskCreated != pdPASS) {
        Serial.println("E: Eye Task creation failed!");
        vQueueDelete(eyeCommandQueue); eyeCommandQueue = NULL; // Clean up queue
        // Consider potentially calling display.end() or similar if available
        return false;
    }
    Serial.println("Eye Background Task Started.");
    return true; // Success
}
// --- End of File: robo_eyes_task.ino ---
