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

// --- IMPORTANT EXTERNAL REQUIREMENTS ---
// 1. WiFi MUST be connected before calling transcribeAudio() or recordAndTranscribe().
// 2. SPIFFS MUST be initialized with SPIFFS.begin() in the main setup() before calling any function here.
// 3. Partition Scheme: Ensure you've selected a partition scheme in Arduino IDE Tools menu
//    that provides sufficient SPIFFS space (e.g., Default, Huge App, or >= 1MB SPIFFS).
//    Re-upload after changing the partition scheme.
// ---

// Deepgram API key – replace with your actual key
const char* deepgramApiKey = "YOUR_DEEPGRAM_API_KEY"; // <-- PUT YOUR KEY HERE

// I2S and recording settings
const i2s_port_t I2S_PORT = I2S_NUM_0;
const int I2S_WS        = 15; // Word Select / LRCL
const int I2S_SCK       = 14; // Bit Clock / BCLK
const int I2S_SD        = 32; // Serial Data / DIN
const int SAMPLE_RATE   = 16000; // 16kHz sample rate
const int SAMPLE_BITS   = 16;    // 16 bits per sample
const int BYTES_PER_SAMPLE = (SAMPLE_BITS / 8); // 2 bytes
const int RECORD_TIME   = 5;     // seconds to record
const int I2S_BUFFER_SIZE = 512; // Bytes for I2S read buffer
const int DMA_BUFFER_COUNT= 4;   // Number of DMA buffers
// Correct calculation: total_bytes / (count * bytes_per_sample * channels)
const int DMA_BUFFER_LEN  = I2S_BUFFER_SIZE / DMA_BUFFER_COUNT / BYTES_PER_SAMPLE; // Samples per DMA buffer (512 / 4 / 2 = 64)

// WAV File Settings
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
    .dma_buf_len = DMA_BUFFER_LEN, // Corrected calculation
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
    // Continue, but note potential stale data if error repeats
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
  // Ensure SPIFFS is already initialized via SPIFFS.begin() in main setup()
  File audioFile = SPIFFS.open(FILENAME, "w"); // Open in write mode (creates/truncates)
  if (!audioFile) {
    Serial.println("ERROR - Failed to open file for writing! Check SPIFFS.begin() in setup().");
    i2s_stop(I2S_PORT);
    i2s_driver_uninstall(I2S_PORT);
    i2s_installed = false;
    return false; // Cannot proceed
  }

  // Write a placeholder WAV header
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
  uint8_t i2s_read_buffer[I2S_BUFFER_SIZE]; // Buffer to hold data from I2S driver
  size_t bytesRead = 0;
  uint32_t totalAudioBytes = 0; // Track successfully written audio bytes
  uint32_t startTime = millis();

  while (millis() - startTime < (uint32_t)RECORD_TIME * 1000) {
    // Read data from I2S into buffer
    esp_err_t read_err = i2s_read(I2S_PORT, i2s_read_buffer, I2S_BUFFER_SIZE, &bytesRead, pdMS_TO_TICKS(100)); // Shorter timeout

    if (read_err == ESP_OK && bytesRead > 0) {
      // Write the received data to the file
      size_t written_bytes = audioFile.write(i2s_read_buffer, bytesRead);

      // *** CRITICAL CHECK: Verify all bytes were written ***
      if (written_bytes == bytesRead) {
          totalAudioBytes += bytesRead; // Increment only if write was successful
      } else {
          Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
          Serial.println("ERROR - Failed to write all bytes to SPIFFS! DISK FULL?");
          Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
          recording_success = false; // Mark recording as failed
          break; // Exit the recording loop immediately
      }
    } else if (read_err == ESP_ERR_TIMEOUT) {
        // Ignore timeout, maybe no data ready yet
        // Serial.print("."); // Uncomment for verbose logging of timeouts
    } else {
        Serial.printf("ERROR - i2s_read failed! Code: %d\n", read_err);
        recording_success = false; // Mark recording as failed
        break; // Exit the recording loop on read error
    }
    yield(); // Allow other tasks (like WiFi) to run
  }

  Serial.printf("Finished recording loop. Status OK: %s, Bytes Attempted/Written: %u\n", recording_success ? "Yes" : "No", totalAudioBytes);

  // Stop I2S *before* finalizing file header or uninstalling driver
  i2s_stop(I2S_PORT);

  // --- Finalize WAV Header ---
  // Calculate final sizes based on successfully written bytes
  uint32_t fileSize = totalAudioBytes + WAV_HDR_SIZE - 8;
  uint32_t byteRate = SAMPLE_RATE * 1 * BYTES_PER_SAMPLE; // Mono
  uint16_t blockAlign = 1 * BYTES_PER_SAMPLE; // Mono

  // RIFF chunk
  wavHeader[0] = 'R'; wavHeader[1] = 'I'; wavHeader[2] = 'F'; wavHeader[3] = 'F';
  memcpy(&wavHeader[4], &fileSize, 4);
  wavHeader[8] = 'W'; wavHeader[9] = 'A'; wavHeader[10] = 'V'; wavHeader[11] = 'E';
  // fmt chunk
  wavHeader[12] = 'f'; wavHeader[13] = 'm'; wavHeader[14] = 't'; wavHeader[15] = ' ';
  wavHeader[16] = 16; wavHeader[17] = 0; wavHeader[18] = 0; wavHeader[19] = 0; // PCM format size
  wavHeader[20] = 1; wavHeader[21] = 0; // PCM format
  wavHeader[22] = 1; wavHeader[23] = 0; // Mono channel
  memcpy(&wavHeader[24], &SAMPLE_RATE, 4);
  memcpy(&wavHeader[28], &byteRate, 4);
  memcpy(&wavHeader[32], &blockAlign, 2);
  memcpy(&wavHeader[34], &SAMPLE_BITS, 2);
  // data chunk
  wavHeader[36] = 'd'; wavHeader[37] = 'a'; wavHeader[38] = 't'; wavHeader[39] = 'a';
  memcpy(&wavHeader[40], &totalAudioBytes, 4); // Size of audio data

  // Seek to beginning and write the finalized header
  if (!audioFile.seek(0)) {
     Serial.println("ERROR - Failed to seek to beginning of WAV file to write header!");
     recording_success = false; // Mark as failed if header can't be written
  } else {
      written = audioFile.write(wavHeader, WAV_HDR_SIZE);
      if (written != WAV_HDR_SIZE) {
          Serial.println("ERROR - Failed to write final WAV header!");
          recording_success = false; // Mark as failed
      }
  }

  // Close the file
  audioFile.close();
  Serial.printf("Audio file '%s' closed.\n", FILENAME);

  // Uninstall I2S driver *after* file is closed
  if (i2s_installed) {
      err = i2s_driver_uninstall(I2S_PORT);
      if (err != ESP_OK) {
          // Log error but don't necessarily fail the whole recording if file ops succeeded
          Serial.printf("Warning - Failed to uninstall I2S driver cleanly. Code: %d\n", err);
      }
      i2s_installed = false;
  }

  Serial.printf("Recording process finished. Overall Success: %s\n", recording_success ? "Yes" : "No");

  // Return true ONLY if the recording_success flag is still true AND some audio data was written
  return recording_success && (totalAudioBytes > 0);
}


/**
 * @brief Sends the recorded WAV file to the Deepgram API for transcription.
 * @return String containing the JSON response from Deepgram, or empty string on failure.
 */
String transcribeAudio() {
  // Ensure WiFi is connected and SPIFFS is begun in the main sketch
  Serial.println("Preparing to send audio to Deepgram...");

  // Allow network stack time to stabilize if needed
  yield(); delay(100);

  // Create secure client (Use Root CA for production!)
  WiFiClientSecure client;
  client.setInsecure(); // <<< FOR TESTING ONLY - Insecure! Use setCACert() in production.
  client.setTimeout(20000); // 20 seconds timeout for connection and transfer

  HTTPClient https;
  https.setReuse(false); // Helps avoid state issues on ESP32
  https.setTimeout(20000); // Match client timeout

  // Construct Deepgram API URL
  String deepgramURL = "https://api.deepgram.com/v1/listen?model=nova-2&language=en"; // Example: Specify model and language
  // String deepgramURL = "https://api.deepgram.com/v1/listen?model=nova-2-general&detect_language=true"; // Or use auto-detect

  // Begin HTTPS connection
  if (!https.begin(client, deepgramURL)) {
    Serial.println("ERROR - HTTPS begin failed. Check URL and client setup.");
    return "";
  }

  // Add Headers
  https.addHeader("Authorization", String("Token ") + deepgramApiKey);
  https.addHeader("Content-Type", "audio/wav");
  https.addHeader("Connection", "close");

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
  // Sanity check file size - is it reasonable?
  size_t expectedMinSize = WAV_HDR_SIZE + (SAMPLE_RATE * BYTES_PER_SAMPLE / 5); // Header + ~0.2s audio
  if (fileSize < expectedMinSize) {
      Serial.printf("Warning: Audio file size (%d bytes) seems very small. Uploading anyway.\n", fileSize);
  }
  Serial.printf("Sending '%s' (%d bytes) to Deepgram...\n", FILENAME, fileSize);

  // Send the POST request with the file stream
  int httpCode = https.sendRequest("POST", &audioFile, fileSize);
  audioFile.close(); // Close file AFTER sending

  Serial.printf("Deepgram HTTP response code: %d\n", httpCode);

  // Process response
  String response = "";
  if (httpCode > 0) { // Got a response code from server
    response = https.getString(); // Get payload

    if (httpCode >= 200 && httpCode < 300) { // Success codes (2xx)
      Serial.println("Deepgram request successful.");
      // Serial.println("Raw Response: " + response); // Uncomment for debug
    } else { // Error codes (4xx, 5xx)
      Serial.printf("ERROR - Deepgram request failed with HTTP code: %d\n", httpCode);
      Serial.println("Error Payload: " + response);
      response = ""; // Return empty string on API error
    }
  } else { // Connection or other HTTPS errors (httpCode < 0)
    Serial.printf("ERROR - HTTPS request failed. Error: %s\n", https.errorToString(httpCode).c_str());
    response = ""; // Return empty string on connection error
  }

  https.end(); // Release resources

  if (response.isEmpty() && httpCode >= 200 && httpCode < 300) {
      Serial.println("Warning: Successful HTTP code from Deepgram but received empty response body.");
  }

  return response; // Return JSON response or empty string
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

  // Adjust document size based on expected response complexity
  // Start reasonably large, monitor memory usage if needed.
  DynamicJsonDocument doc(3072);
  DeserializationError jsonError = deserializeJson(doc, response);

  if (jsonError) {
    Serial.print("ERROR - JSON parsing failed: ");
    Serial.println(jsonError.c_str());
    // Avoid printing the whole response here unless debugging memory issues
    return "";
  }

  // Safely navigate the JSON structure (adjust path if Deepgram format changes)
  JsonVariant transcriptVar = doc["results"]["channels"][0]["alternatives"][0]["transcript"];

  if (transcriptVar.isNull() || !transcriptVar.is<const char*>()) {
    Serial.println("ERROR - Transcript data not found or not a string in Deepgram JSON response.");
    Serial.println("Check JSON structure. Response was:");
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
 * @return String containing the final transcribed text, or empty string on failure at any step.
 */
String recordAndTranscribe() {
  // Ensure SPIFFS is initialized in the main setup() before calling this
  Serial.println("--- Starting Record and Transcribe Process ---");

  // Step 1: Record audio
  Serial.println("Step 1: Recording Audio...");
  if (!recordAudio()) {
    Serial.println("Step 1 FAILED: Recording audio was unsuccessful.");
    // Optional: Clean up failed recording file?
    // if (SPIFFS.exists(FILENAME)) SPIFFS.remove(FILENAME);
    Serial.println("--- Record and Transcribe Process FAILED ---");
    return ""; // Exit if recording failed
  }
  Serial.println("Step 1 SUCCESS: Audio recorded.");

  // Step 2: Transcribe audio (requires WiFi connection)
  Serial.println("Step 2: Sending Audio for Transcription...");
  String jsonResponse = transcribeAudio();
  if (jsonResponse.isEmpty()) {
    Serial.println("Step 2 FAILED: Transcription request failed or returned error.");
    Serial.println("--- Record and Transcribe Process FAILED ---");
    return ""; // Exit if transcription failed
  }
  Serial.println("Step 2 SUCCESS: Received response from Deepgram.");

  // Step 3: Parse response to get transcript
  Serial.println("Step 3: Parsing Transcription...");
  String finalTranscript = parseTranscription(jsonResponse);
  if (finalTranscript.isEmpty()) {
      Serial.println("Step 3 FAILED: Parsing failed or no transcript found in response.");
      // Note: process doesn't necessarily "fail" here, just didn't get text
  } else {
       Serial.println("Step 3 SUCCESS: Transcript parsed.");
  }

  // Optional: Clean up the recorded file now that it's processed
  if (SPIFFS.exists(FILENAME)) {
    if(SPIFFS.remove(FILENAME)) {
        Serial.printf("Cleaned up temporary file: %s\n", FILENAME);
    } else {
        Serial.printf("Warning: Failed to remove temporary file: %s\n", FILENAME);
    }
  }

  Serial.println("--- Record and Transcribe Process Finished ---");
  return finalTranscript; // Return the parsed transcript (or "" if parsing failed)
}

// End of audio_functions.ino
