// --- audio_functions.ino ---
// Put this file in the same folder as your main sketch (e.g., Myproject.ino)
// This file should NOT contain setup() or loop()

#include <Arduino.h> // Good practice to include in library-like .ino files
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "SPIFFS.h" // Needs to be included here AND in the main .ino file
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <limits.h> // Needed for INT16_MAX, INT16_MIN

// --- IMPORTANT EXTERNAL REQUIREMENTS ---
// 1. WiFi MUST be connected before calling transcribeAudio() or recordAndTranscribe().
// 2. SPIFFS MUST be initialized with SPIFFS.begin() in the main setup() before calling any function here.
// 3. Partition Scheme: Ensure you've selected a partition scheme in Arduino IDE Tools menu
//    that provides sufficient SPIFFS space (e.g., Default, Huge App, or >= 1MB SPIFFS).
//    Re-upload after changing the partition scheme.
// 4. Serial MUST be initialized with Serial.begin() in the main setup() BEFORE calling
//    initializeAudioGain() below.
// 5. The function initializeAudioGain() MUST be called ONCE from your main sketch's setup()
//    after Serial.begin() to set the gain value for the session.
// ---

// Deepgram API key – replace with your actual key
const char* deepgramApiKey = "ef37ae23ce94d481b566ea439423b1e305c08038"; // <-- PUT YOUR KEY HERE

// I2S and recording settings
const i2s_port_t I2S_PORT = I2S_NUM_1;
const int I2S_WS        = 15; // Word Select / LRCL
const int I2S_SCK       = 14; // Bit Clock / BCLK
const int I2S_SD        = 32; // Serial Data / DIN
const int SAMPLE_RATE   = 44100; // 44.1kHz sample rate <--- CHANGED FROM 16000
const int SAMPLE_BITS   = 16;    // 16 bits per sample
const int BYTES_PER_SAMPLE = (SAMPLE_BITS / 8); // 2 bytes
const int RECORD_TIME   = 10;     // seconds to record
const int I2S_BUFFER_SIZE = 512; // Bytes for I2S read buffer (Adjust if needed for 44.1kHz, but keep simple for now)
const int DMA_BUFFER_COUNT= 4;   // Number of DMA buffers
const int DMA_BUFFER_LEN  = I2S_BUFFER_SIZE / DMA_BUFFER_COUNT / BYTES_PER_SAMPLE; // Samples per DMA buffer (512 / 4 / 2 = 64)

// WAV File Settings
const int WAV_HDR_SIZE  = 44;    // Size of a standard WAV header
const char* FILENAME    = "/rec.wav"; // Filename on SPIFFS

// --- Global Gain Setting ---
// This variable stores the gain multiplier.
// It is initialized to 1.0 but MUST be set by calling initializeAudioGain()
// from your main sketch's setup() function AFTER Serial.begin().
float audioGain = 1.0;
// --- End Global Gain Setting ---


// Global flag for I2S state (use cautiously if functions could be interrupted)
bool i2s_installed = false;

// --- Function Prototypes ---
void initializeAudioGain(); // <<< ADDED PROTOTYPE
bool recordAudio();
String transcribeAudio();
String parseTranscription(String response);
String recordAndTranscribe();
// --- End Prototypes ---

/**
 * @brief Reads the desired audio gain multiplier from the Serial Monitor.
 * @note This function MUST be called once from the main sketch's setup()
 *       AFTER Serial.begin() has been called. It will block until
 *       input is received or it times out.
 */
void initializeAudioGain() {
    // Check if Serial is actually running - basic safeguard
    if (!Serial) {
        Serial.begin(115200); // Attempt to start Serial if not already
        delay(100); // Give it a moment
        if (!Serial) {
            // Still not working, use default gain and warn (though printing won't work)
            audioGain = 1.0;
            // We can't print a warning if Serial isn't working.
            return; // Exit, using default gain.
        }
         Serial.println("Warning: Serial was not initialized before initializeAudioGain(). Started it now.");
    }

    Serial.println("\n---------------------------------");
    Serial.println("Audio Gain Setup");
    Serial.println("Enter audio gain multiplier (e.g., 1.0, 2.5, 0.8) and press Enter:");
    Serial.println("Waiting for input...");

    Serial.setTimeout(30000); // Set timeout for waiting for input (30 seconds)

    // Wait for data to become available
    while (!Serial.available()) {
        Serial.print(".");
        delay(500); // Prevent busy-waiting
        // Check if timeout occurred within the loop (Serial.setTimeout handles the read timeout)
        // No explicit timeout check here, rely on Serial.parseFloat timeout.
    }

    // Read the float value
    float inputGain = Serial.parseFloat();

    // Consume any trailing newline characters (\n or \r) left in the buffer
    // This prevents them from interfering with later Serial reads if any.
    while(Serial.available() && (Serial.peek() == '\n' || Serial.peek() == '\r')) {
        Serial.read();
    }

    // Validate the input
    if (inputGain <= 0.0 || isnan(inputGain)) { // Check for invalid, zero, or negative gain
        Serial.printf("\nInvalid input, zero/negative gain, or timeout. Using default gain 1.0\n");
        audioGain = 1.0; // Default to 1.0 if input is invalid or timed out
    } else {
        audioGain = inputGain;
        Serial.printf("\nAudio gain set to: %.2f\n", audioGain);
    }
    Serial.println("---------------------------------");
    Serial.setTimeout(1000); // Reset timeout to default (or desired value for normal operation)
}


/**
 * @brief Records audio from I2S microphone to a WAV file on SPIFFS, applying gain.
 * @return true if recording succeeded without errors and data was written, false otherwise.
 */
bool recordAudio() {
  // Ensure Serial is initialized in the main setup()
  Serial.println("Initializing I2S...");
  bool recording_success = true; // Flag to track success throughout the process

  // Configure I2S
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE, // Uses the updated 44100 Hz
    .bits_per_sample = (i2s_bits_per_sample_t)SAMPLE_BITS,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Mono microphone
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = DMA_BUFFER_COUNT,
    .dma_buf_len = DMA_BUFFER_LEN, // Corrected calculation
    .use_apll = false // Set true for higher sample rates if needed, but check docs/stability
                      // For 44.1kHz, false is often sufficient with internal APLL auto-selection.
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE, // Not transmitting
    .data_in_num = I2S_SD
  };

  // Install I2S driver
  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("ERROR - I2S Driver Install failed! Code: %d\n", err);
    return false; // Cannot proceed
  }
  i2s_installed = true;

  // Set I2S pins
  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("ERROR - I2S Pin Configuration failed! Code: %d\n", err);
    i2s_driver_uninstall(I2S_PORT);
    i2s_installed = false;
    return false; // Cannot proceed
  }

  // Clear DMA buffer (good practice)
  err = i2s_zero_dma_buffer(I2S_PORT);
  if (err != ESP_OK) {
    Serial.printf("Warning - Failed to zero I2S DMA buffer. Code: %d\n", err);
  }

  // Start I2S
  err = i2s_start(I2S_PORT);
  if (err != ESP_OK) {
    Serial.printf("ERROR - Failed to start I2S! Code: %d\n", err);
    i2s_driver_uninstall(I2S_PORT);
    i2s_installed = false;
    return false; // Cannot proceed
  }

  // --- File Handling ---
  File audioFile = SPIFFS.open(FILENAME, "w");
  if (!audioFile) {
    Serial.println("ERROR - Failed to open file for writing! Check SPIFFS.begin() in setup().");
    i2s_stop(I2S_PORT);
    i2s_driver_uninstall(I2S_PORT);
    i2s_installed = false;
    return false;
  }

  // Write placeholder WAV header
  uint8_t wavHeader[WAV_HDR_SIZE] = {0};
  size_t written = audioFile.write(wavHeader, WAV_HDR_SIZE);
  if (written != WAV_HDR_SIZE) {
      Serial.println("ERROR - Failed to write temporary WAV header to file!");
      audioFile.close();
      i2s_stop(I2S_PORT);
      i2s_driver_uninstall(I2S_PORT);
      i2s_installed = false;
      return false;
  }

  // --- Recording Loop ---
  // *** USES THE audioGain value set by initializeAudioGain() ***
  Serial.printf("Recording for %d seconds with gain %.2f...\n", RECORD_TIME, audioGain);
  uint8_t i2s_read_buffer[I2S_BUFFER_SIZE];
  size_t bytesRead = 0;
  uint32_t totalAudioBytes = 0;
  uint32_t startTime = millis();

  while (millis() - startTime < (uint32_t)RECORD_TIME * 1000) {
    esp_err_t read_err = i2s_read(I2S_PORT, i2s_read_buffer, I2S_BUFFER_SIZE, &bytesRead, pdMS_TO_TICKS(100));

    if (read_err == ESP_OK && bytesRead > 0) {
      // --- Apply Gain --- START ---
      // Uses the global audioGain variable
      if (audioGain != 1.0f && bytesRead > 0) {
        int16_t* samples = (int16_t*)i2s_read_buffer;
        int num_samples = bytesRead / BYTES_PER_SAMPLE;

        for (int i = 0; i < num_samples; i++) {
            int32_t amplified_sample = (int32_t)((float)samples[i] * audioGain);
            // Clamp the value
            if (amplified_sample > INT16_MAX) amplified_sample = INT16_MAX;
            else if (amplified_sample < INT16_MIN) amplified_sample = INT16_MIN;
            samples[i] = (int16_t)amplified_sample;
        }
      }
      // --- Apply Gain --- END ---

      size_t written_bytes = audioFile.write(i2s_read_buffer, bytesRead);
      if (written_bytes == bytesRead) {
          totalAudioBytes += bytesRead;
      } else {
          Serial.println("ERROR - Failed to write all bytes to SPIFFS! DISK FULL?");
          recording_success = false;
          break;
      }
    } else if (read_err == ESP_ERR_TIMEOUT) {
        // Ignore timeout
    } else {
        Serial.printf("ERROR - i2s_read failed! Code: %d\n", read_err);
        recording_success = false;
        break;
    }
    yield();
  }

  Serial.printf("Finished recording loop. Status OK: %s, Bytes Attempted/Written: %u\n", recording_success ? "Yes" : "No", totalAudioBytes);
  i2s_stop(I2S_PORT);

  // --- Finalize WAV Header ---
  uint32_t fileSize = totalAudioBytes + WAV_HDR_SIZE - 8;
  uint32_t byteRate = SAMPLE_RATE * 1 * BYTES_PER_SAMPLE;
  uint16_t blockAlign = 1 * BYTES_PER_SAMPLE;

  wavHeader[0] = 'R'; wavHeader[1] = 'I'; wavHeader[2] = 'F'; wavHeader[3] = 'F';
  memcpy(&wavHeader[4], &fileSize, 4);
  wavHeader[8] = 'W'; wavHeader[9] = 'A'; wavHeader[10] = 'V'; wavHeader[11] = 'E';
  wavHeader[12] = 'f'; wavHeader[13] = 'm'; wavHeader[14] = 't'; wavHeader[15] = ' ';
  wavHeader[16] = 16; wavHeader[17] = 0; wavHeader[18] = 0; wavHeader[19] = 0;
  wavHeader[20] = 1; wavHeader[21] = 0;
  wavHeader[22] = 1; wavHeader[23] = 0;
  memcpy(&wavHeader[24], &SAMPLE_RATE, 4);
  memcpy(&wavHeader[28], &byteRate, 4);
  memcpy(&wavHeader[32], &blockAlign, 2);
  memcpy(&wavHeader[34], &SAMPLE_BITS, 2);
  wavHeader[36] = 'd'; wavHeader[37] = 'a'; wavHeader[38] = 't'; wavHeader[39] = 'a';
  memcpy(&wavHeader[40], &totalAudioBytes, 4);

  if (!audioFile.seek(0)) {
     Serial.println("ERROR - Failed to seek to beginning of WAV file to write header!");
     recording_success = false;
  } else {
      written = audioFile.write(wavHeader, WAV_HDR_SIZE);
      if (written != WAV_HDR_SIZE) {
          Serial.println("ERROR - Failed to write final WAV header!");
          recording_success = false;
      }
  }

  audioFile.close();
  Serial.printf("Audio file '%s' closed.\n", FILENAME);

  if (i2s_installed) {
      err = i2s_driver_uninstall(I2S_PORT);
      if (err != ESP_OK) {
          Serial.printf("Warning - Failed to uninstall I2S driver cleanly. Code: %d\n", err);
      }
      i2s_installed = false;
  }

  Serial.printf("Recording process finished. Overall Success: %s\n", recording_success ? "Yes" : "No");
  return recording_success && (totalAudioBytes > 0);
}


// --- transcribeAudio(), parseTranscription(), recordAndTranscribe() ---
// --- (These functions remain unchanged from the previous version) ---
// --- (They don't directly interact with the gain setting) ---

/**
 * @brief Sends the recorded WAV file to the Deepgram API for transcription.
 * @return String containing the JSON response from Deepgram, or empty string on failure.
 */
String transcribeAudio() {
  // Ensure WiFi is connected and SPIFFS is begun in the main sketch
  Serial.println("Preparing to send audio to Deepgram...");

  yield(); delay(100);

  WiFiClientSecure client;
  client.setInsecure(); // <<< FOR TESTING ONLY
  client.setTimeout(30000); // Increased timeout

  HTTPClient https;
  https.setReuse(false);
  https.setTimeout(30000); // Match client timeout

  String deepgramURL = "https://api.deepgram.com/v1/listen?model=nova-2&language=en";

  if (!https.begin(client, deepgramURL)) {
    Serial.println("ERROR - HTTPS begin failed.");
    return "";
  }

  https.addHeader("Authorization", String("Token ") + deepgramApiKey);
  https.addHeader("Content-Type", "audio/wav");
  https.addHeader("Connection", "close");

  File audioFile = SPIFFS.open(FILENAME, "r");
  if (!audioFile) {
    Serial.printf("ERROR - Failed to open audio file '%s' for reading!\n", FILENAME);
    https.end();
    return "";
  }
  if (!audioFile.available()) {
      Serial.printf("ERROR - Audio file '%s' is empty or unavailable.\n", FILENAME);
      audioFile.close();
      https.end();
      return "";
  }

  size_t fileSize = audioFile.size();
  size_t expectedMinSize = WAV_HDR_SIZE + (uint32_t)(SAMPLE_RATE * BYTES_PER_SAMPLE * 0.1);
  if (fileSize < expectedMinSize) {
      Serial.printf("Warning: Audio file size (%d bytes) seems very small for 44.1kHz. Uploading anyway.\n", fileSize);
  } else if (fileSize > 2000000) {
      Serial.printf("Warning: Audio file size (%d bytes) is large. Upload might take time.\n", fileSize);
  }
  Serial.printf("Sending '%s' (%d bytes) to Deepgram...\n", FILENAME, fileSize);

  int httpCode = https.sendRequest("POST", &audioFile, fileSize);
  audioFile.close();

  Serial.printf("Deepgram HTTP response code: %d\n", httpCode);

  String response = "";
  if (httpCode > 0) {
    response = https.getString();
    if (httpCode >= 200 && httpCode < 300) {
      Serial.println("Deepgram request successful.");
    } else {
      Serial.printf("ERROR - Deepgram request failed with HTTP code: %d\n", httpCode);
      Serial.println("Error Payload: " + response);
      response = "";
    }
  } else {
    Serial.printf("ERROR - HTTPS request failed. Error: %s\n", https.errorToString(httpCode).c_str());
    response = "";
  }
    Serial.println("--- RAW DEEPGRAM JSON RESPONSE ---");
    Serial.println(response);
    Serial.println("------------------------------------");

  https.end();

  if (response.isEmpty() && httpCode >= 200 && httpCode < 300) {
      Serial.println("Warning: Successful HTTP code from Deepgram but received empty response body.");
  }

  return response;
}

/**
 * @brief Parses the JSON response from Deepgram to extract the transcript.
 * @param response The JSON string received from Deepgram.
 * @return String containing the transcribed text, or empty string on failure or no transcript.
 */
String parseTranscription(String response) {
  if (response.isEmpty()) {
    Serial.println("Parsing skipped: Empty response received.");
    return "";
  }

  DynamicJsonDocument doc(4096); // Increased size slightly
  DeserializationError jsonError = deserializeJson(doc, response);

  if (jsonError) {
    Serial.print("ERROR - JSON parsing failed: ");
    Serial.println(jsonError.c_str());
    return "";
  }

  JsonVariant transcriptVar = doc["results"][0]["channels"][0]["alternatives"][0]["transcript"];

  if (transcriptVar.isNull() || !transcriptVar.is<const char*>()) {
    Serial.println("ERROR - Transcript data not found or not a string in Deepgram JSON response.");
    Serial.println("Check JSON structure. Response structure was:");
    JsonObject root = doc.as<JsonObject>();
     for (JsonPair kv : root) {
         Serial.print(kv.key().c_str()); Serial.print(" ");
     }
     Serial.println();
    return "";
  }

  String transcript = transcriptVar.as<String>();
  if (transcript.length() == 0) {
      Serial.println("Transcription result is empty (likely silence or no speech detected).");
  } else {
      Serial.println("Transcription: " + transcript);
  }
  return transcript;
}


/**
 * @brief Orchestrates the entire process: records audio, sends for transcription, parses result.
 * @return String containing the final transcribed text, or empty string on failure at any step.
 */
String recordAndTranscribe() {
  Serial.println("--- Starting Record and Transcribe Process ---");

  Serial.println("Step 1: Recording Audio...");
  if (!recordAudio()) { // recordAudio now uses the globally set gain
    Serial.println("Step 1 FAILED: Recording audio was unsuccessful.");
    if (i2s_installed) {
        i2s_driver_uninstall(I2S_PORT);
        i2s_installed = false;
    }
    Serial.println("--- Record and Transcribe Process FAILED ---");
    return "";
  }
  Serial.println("Step 1 SUCCESS: Audio recorded.");

  Serial.println("Step 2: Sending Audio for Transcription...");
  String jsonResponse = transcribeAudio();
  if (jsonResponse.isEmpty()) {
    Serial.println("Step 2 FAILED: Transcription request failed or returned error.");
    Serial.println("--- Record and Transcribe Process FAILED ---");
    return "";
  }
  Serial.println("Step 2 SUCCESS: Received response from Deepgram.");

  Serial.println("Step 3: Parsing Transcription...");
  String finalTranscript = parseTranscription(jsonResponse);
  if (finalTranscript.isEmpty() && jsonResponse.length() > 0) {
      Serial.println("Step 3 Result: Parsing successful, but no transcript text found (e.g., silence).");
  } else if (!finalTranscript.isEmpty()) {
       Serial.println("Step 3 SUCCESS: Transcript parsed.");
  } else {
      Serial.println("Step 3 FAILED: Parsing likely failed or response was empty.");
  }


  if (SPIFFS.exists(FILENAME)) {
    if(SPIFFS.remove(FILENAME)) {
        Serial.printf("Cleaned up temporary file: %s\n", FILENAME);
    } else {
        Serial.printf("Warning: Failed to remove temporary file: %s\n", FILENAME);
    }
  }

  Serial.println("--- Record and Transcribe Process Finished ---");
  return finalTranscript;
}

// End of audio_functions.ino
