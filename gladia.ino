// --- audio_functions.ino ---
// Put this file in the same folder as your main sketch (e.g., Myproject.ino)
// This file should NOT contain setup() or loop()
// --- MODIFIED FOR GLADIA.IO ---

#include <Arduino.h> // Good practice to include in library-like .ino files
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "SPIFFS.h" // Needs to be included here AND in the main .ino file
#include <ArduinoJson.h>
#include <driver/i2s.h>

// --- IMPORTANT EXTERNAL REQUIREMENTS ---
// 1. WiFi MUST be connected before calling transcribeAudio() or recordAndTranscribe().
// 2. SPIFFS MUST be initialized with SPIFFS.begin() in the main setup() before calling any function here.
// 3. Partition Scheme: Ensure you've selected a partition scheme in Arduino IDE Tools menu
//    that provides sufficient SPIFFS space (e.g., Default, Huge App, or >= 1MB SPIFFS).
//    Re-upload after changing the partition scheme.
// ---

// --- Gladia.io API Configuration ---
const char* gladiaApiKey = "YOUR_GLADIA_API_KEY"; // <-- PUT YOUR GLADIA KEY HERE
const char* gladiaApiUrl = "https://api.gladia.io/audio/text/audio-transcription/";

// --- I2S and recording settings (Unchanged from original) ---
const i2s_port_t I2S_PORT = I2S_NUM_1;
const int I2S_WS        = 15; // Word Select / LRCL
const int I2S_SCK       = 14; // Bit Clock / BCLK
const int I2S_SD        = 32; // Serial Data / DIN
const int SAMPLE_RATE   = 16000; // 16kHz sample rate
const int SAMPLE_BITS   = 16;    // 16 bits per sample
const int BYTES_PER_SAMPLE = (SAMPLE_BITS / 8); // 2 bytes
const int RECORD_TIME   = 10;     // seconds to record
const int I2S_BUFFER_SIZE = 512; // Bytes for I2S read buffer
const int DMA_BUFFER_COUNT= 4;   // Number of DMA buffers
const int DMA_BUFFER_LEN  = I2S_BUFFER_SIZE / DMA_BUFFER_COUNT / BYTES_PER_SAMPLE; // Samples per DMA buffer

// --- WAV File Settings (Unchanged from original) ---
const int WAV_HDR_SIZE  = 44;    // Size of a standard WAV header
const char* FILENAME    = "/rec.wav"; // Filename on SPIFFS

// Global flag for I2S state (use cautiously if functions could be interrupted)
bool i2s_installed = false;

// --- Function Prototypes ---
bool recordAudio();
String transcribeAudio();
String parseTranscription(String response);
String recordAndTranscribe();
// --- End Prototypes ---


/**
 * @brief Records audio from I2S microphone to a WAV file on SPIFFS.
 *        (Unchanged from original - This function prepares the audio file)
 * @return true if recording succeeded without errors and data was written, false otherwise.
 */
bool recordAudio() {
  // Ensure Serial is initialized in the main setup()
  Serial.println("Initializing I2S...");
  bool recording_success = true; // Flag to track success throughout the process

  // Configure I2S
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = (i2s_bits_per_sample_t)SAMPLE_BITS,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Mono microphone
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = DMA_BUFFER_COUNT,
    .dma_buf_len = DMA_BUFFER_LEN,
    .use_apll = false
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
  Serial.println("Recording...");
  uint8_t i2s_read_buffer[I2S_BUFFER_SIZE];
  size_t bytesRead = 0;
  uint32_t totalAudioBytes = 0;
  uint32_t startTime = millis();

  while (millis() - startTime < (uint32_t)RECORD_TIME * 1000) {
    esp_err_t read_err = i2s_read(I2S_PORT, i2s_read_buffer, I2S_BUFFER_SIZE, &bytesRead, pdMS_TO_TICKS(100));

    if (read_err == ESP_OK && bytesRead > 0) {
      size_t written_bytes = audioFile.write(i2s_read_buffer, bytesRead);
      if (written_bytes == bytesRead) {
          totalAudioBytes += bytesRead;
      } else {
          Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
          Serial.println("ERROR - Failed to write all bytes to SPIFFS! DISK FULL?");
          Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
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

  Serial.printf("Finished recording loop. Status OK: %s, Bytes Written: %u\n", recording_success ? "Yes" : "No", totalAudioBytes);
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
  wavHeader[22] = 1; wavHeader[23] = 0; // Mono
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

  // Uninstall I2S driver
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


/**
 * @brief Sends the recorded WAV file to the Gladia API for transcription.
 *        --- MODIFIED FOR GLADIA.IO ---
 * @return String containing the JSON response from Gladia, or empty string on failure.
 */
String transcribeAudio() {
  // Ensure WiFi is connected and SPIFFS is begun in the main sketch
  Serial.println("Preparing to send audio to Gladia.io...");
  yield(); delay(100);

  WiFiClientSecure client;
  client.setInsecure(); // <<< FOR TESTING ONLY - Insecure! Use setCACert() in production.
  // Consider adding Gladia's root CA certificate for production use.
  // Example: client.setCACert(gladia_root_ca);
  client.setTimeout(20000); // 20 seconds timeout

  HTTPClient https;
  https.setReuse(false);
  https.setTimeout(20000);

  // Begin HTTPS connection to Gladia API
  if (!https.begin(client, gladiaApiUrl)) {
    Serial.println("ERROR - HTTPS begin failed. Check URL and client setup.");
    return "";
  }

  // --- Add Gladia Headers ---
  https.addHeader("x-gladia-key", gladiaApiKey); // Gladia API Key
  https.addHeader("Content-Type", "audio/wav");  // Type of audio being sent
  // Optional: Specify language behavior (check Gladia docs for options)
  // https.addHeader("language_behaviour", "automatic single language");
  // https.addHeader("language", "english"); // Or specify a language
  https.addHeader("accept", "application/json"); // Expect JSON response
  https.addHeader("Connection", "close");        // Close connection after request

  // Open recorded file for reading
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
  size_t expectedMinSize = WAV_HDR_SIZE + (SAMPLE_RATE * BYTES_PER_SAMPLE / 5); // Header + ~0.2s audio
  if (fileSize < expectedMinSize) {
      Serial.printf("Warning: Audio file size (%d bytes) seems very small. Uploading anyway.\n", fileSize);
  }
  Serial.printf("Sending '%s' (%d bytes) to Gladia...\n", FILENAME, fileSize);

  // Send the POST request with the file stream as the body
  // Gladia supports sending the raw audio file directly in the body for this endpoint.
  int httpCode = https.sendRequest("POST", &audioFile, fileSize);
  audioFile.close(); // Close file AFTER sending

  Serial.printf("Gladia HTTP response code: %d\n", httpCode);

  // Process response
  String response = "";
  if (httpCode > 0) {
    response = https.getString();

    if (httpCode >= 200 && httpCode < 300) { // Success codes (2xx)
      Serial.println("Gladia request successful.");
      // --- Optional: Print raw response for debugging ---
      // Serial.println("--- RAW GLADIA JSON RESPONSE ---");
      // Serial.println(response);
      // Serial.println("------------------------------------");
      // ------------------------------------------------
    } else { // Error codes (4xx, 5xx)
      Serial.printf("ERROR - Gladia request failed with HTTP code: %d\n", httpCode);
      Serial.println("Error Payload: " + response);
      response = ""; // Return empty string on API error
    }
  } else { // Connection or other HTTPS errors (httpCode < 0)
    Serial.printf("ERROR - HTTPS request failed. Error: %s\n", https.errorToString(httpCode).c_str());
    response = ""; // Return empty string on connection error
  }

  https.end(); // Release resources

  if (response.isEmpty() && httpCode >= 200 && httpCode < 300) {
      Serial.println("Warning: Successful HTTP code from Gladia but received empty response body.");
  }

  return response; // Return JSON response or empty string
}

/**
 * @brief Parses the JSON response from Gladia to extract the full transcript.
 *        --- MODIFIED FOR GLADIA.IO ---
 * @param response The JSON string received from Gladia.
 * @return String containing the transcribed text, or empty string on failure or no transcript.
 */
String parseTranscription(String response) {
  if (response.isEmpty()) {
    Serial.println("Parsing skipped: Empty response received.");
    return "";
  }

  // Adjust document size based on expected response complexity.
  // Gladia's response might be larger if includes timestamps, etc.
  // Start with 4096, monitor memory if needed.
  DynamicJsonDocument doc(4096);
  DeserializationError jsonError = deserializeJson(doc, response);

  if (jsonError) {
    Serial.print("ERROR - JSON parsing failed: ");
    Serial.println(jsonError.c_str());
    // Avoid printing large responses here unless debugging memory issues
    // Serial.println("Response was: " + response);
    return "";
  }

  // Safely navigate the Gladia JSON structure
  // Common path for the full transcript is result.transcription.full_transcript
  // Verify this with the actual response you get from Gladia.
  JsonVariant transcriptVar = doc["result"]["transcription"]["full_transcript"];

  if (transcriptVar.isNull() || !transcriptVar.is<const char*>()) {
    Serial.println("ERROR - Transcript data not found or not a string in Gladia JSON response.");
    Serial.println("Check Gladia JSON structure. Response structure was:");
    serializeJsonPretty(doc, Serial); // Print structure for debugging
    Serial.println();
    return "";
  }

  // Extract the transcript
  String transcript = transcriptVar.as<String>();
  Serial.println("Transcription: " + transcript);
  return transcript;
}


/**
 * @brief Orchestrates the entire process: records audio, sends for transcription, parses result.
 *        (Unchanged from original - Calls the modified functions)
 * @return String containing the final transcribed text, or empty string on failure at any step.
 */
String recordAndTranscribe() {
  Serial.println("--- Starting Record and Transcribe Process (using Gladia.io) ---");

  // Step 1: Record audio
  Serial.println("Step 1: Recording Audio...");
  if (!recordAudio()) {
    Serial.println("Step 1 FAILED: Recording audio was unsuccessful.");
    // Consider uninstalling I2S driver here if not done in recordAudio on failure
    if (i2s_installed) {
      i2s_driver_uninstall(I2S_PORT);
      i2s_installed = false;
      Serial.println("I2S driver uninstalled after recording failure.");
    }
    Serial.println("--- Record and Transcribe Process FAILED ---");
    return "";
  }
  Serial.println("Step 1 SUCCESS: Audio recorded.");

  // Step 2: Transcribe audio (requires WiFi connection)
  Serial.println("Step 2: Sending Audio for Transcription (Gladia)...");
  String jsonResponse = transcribeAudio();
  if (jsonResponse.isEmpty()) {
    Serial.println("Step 2 FAILED: Transcription request failed or returned error.");
    Serial.println("--- Record and Transcribe Process FAILED ---");
    return "";
  }
  Serial.println("Step 2 SUCCESS: Received response from Gladia.");

  // Step 3: Parse response to get transcript
  Serial.println("Step 3: Parsing Transcription (Gladia)...");
  String finalTranscript = parseTranscription(jsonResponse);
  if (finalTranscript.isEmpty()) {
      Serial.println("Step 3 FAILED: Parsing failed or no transcript found in response.");
  } else {
       Serial.println("Step 3 SUCCESS: Transcript parsed.");
  }

  // Optional: Clean up the recorded file
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
