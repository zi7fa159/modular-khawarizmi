#include <Arduino.h> // Good practice to include in library-like .ino files
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "SPIFFS.h"
#include <ArduinoJson.h>
#include <driver/i2s.h>

// Deepgram API key – replace with your actual key
const char* deepgramApiKey = "..."; // Keep your key secure!

// I2S and recording settings
const i2s_port_t I2S_PORT = I2S_NUM_0;
const int I2S_WS        = 15;
const int I2S_SCK       = 14;
const int I2S_SD        = 32;
const int SAMPLE_RATE   = 16000;
const int SAMPLE_BITS   = 16;
const int BYTES_PER_SAMPLE = (SAMPLE_BITS / 8);
const int WAV_HDR_SIZE  = 44;
const int RECORD_TIME   = 5;      // seconds
const int BUF_SIZE      = 512;    // Bytes for DMA buffer
const char* FILENAME    = "/rec.wav";

// Flag to track if I2S driver is installed (might need careful management if called concurrently)
bool i2s_installed = false;

// Task watchdog timeout value (increased to prevent timeout) - Note: WDT is managed globally
const int WDT_TIMEOUT = 30;   // seconds (This constant might not be directly used here now)

// --- Function Prototypes ---
// These allow functions to be called even if defined later in the file.
// The Arduino IDE *tries* to auto-generate these, but explicitly defining them is safer.
bool recordAudio();
String transcribeAudio();
String parseTranscription(String response);
String recordAndTranscribe();
// --- End Prototypes ---


// Record audio to file
// Returns true on success, false on failure.
bool recordAudio() {
  // Ensure Serial is initialized in the main setup() before calling this
  Serial.println("Initializing I2S...");

  // Configure I2S
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = (i2s_bits_per_sample_t)SAMPLE_BITS,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Assuming mono microphone
    .communication_format = I2S_COMM_FORMAT_STAND_I2S, // Standard I2S format
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4, // Number of DMA buffers
    .dma_buf_len = BUF_SIZE / 4, // Size of each DMA buffer in samples (BUF_SIZE must be multiple of dma_buf_count*bytes_per_sample*channels)
    .use_apll = false
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE, // Not transmitting
    .data_in_num = I2S_SD
  };

  // Install & configure I2S driver
  esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("ERROR - I2S installation failed! Code: %d\n", err);
    return false;
  }
  i2s_installed = true; // Mark as installed

  err = i2s_set_pin(I2S_PORT, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("ERROR - I2S pin configuration failed! Code: %d\n", err);
    i2s_driver_uninstall(I2S_PORT); // Clean up
    i2s_installed = false;
    return false;
  }

  // Clear DMA buffers (good practice before starting)
  err = i2s_zero_dma_buffer(I2S_PORT);
   if (err != ESP_OK) {
    Serial.printf("ERROR - Failed to zero DMA buffer! Code: %d\n", err);
    // Continue but log error
  }

  err = i2s_start(I2S_PORT);
   if (err != ESP_OK) {
    Serial.printf("ERROR - Failed to start I2S! Code: %d\n", err);
    i2s_driver_uninstall(I2S_PORT);
    i2s_installed = false;
    return false;
  }

  // Open file for recording (ensure SPIFFS is begun in main setup())
  File audioFile = SPIFFS.open(FILENAME, "w");
  if (!audioFile) {
    Serial.println("ERROR - Failed to open file for writing");
    i2s_stop(I2S_PORT);
    i2s_driver_uninstall(I2S_PORT);
    i2s_installed = false;
    return false;
  }

  // Write a temporary header (will be updated later)
  uint8_t wavHeader[WAV_HDR_SIZE] = {0};
  size_t written = audioFile.write(wavHeader, WAV_HDR_SIZE);
   if (written != WAV_HDR_SIZE) {
      Serial.println("ERROR - Failed to write temporary WAV header");
      audioFile.close();
      i2s_stop(I2S_PORT);
      i2s_driver_uninstall(I2S_PORT);
      i2s_installed = false;
      return false;
  }


  // Record audio data
  Serial.println("Recording...");
  // Buffer to hold data read from I2S DMA
  uint8_t i2s_read_buffer[BUF_SIZE]; // Use byte buffer matching DMA size
  size_t bytesRead = 0;
  uint32_t totalAudioBytes = 0;
  uint32_t startTime = millis();

  while (millis() - startTime < (uint32_t)RECORD_TIME * 1000) {
    // Read data from I2S driver buffer
    esp_err_t read_err = i2s_read(I2S_PORT, i2s_read_buffer, BUF_SIZE, &bytesRead, pdMS_TO_TICKS(1000)); // Wait up to 1 sec

    if (read_err == ESP_OK && bytesRead > 0) {
      // Write read data to SPIFFS file
      size_t written_bytes = audioFile.write(i2s_read_buffer, bytesRead);
      if (written_bytes == bytesRead) {
          totalAudioBytes += bytesRead;
      } else {
          Serial.println("ERROR - Failed to write all bytes to SPIFFS!");
          // Consider stopping recording on write error
          break;
      }
    } else if (read_err == ESP_ERR_TIMEOUT) {
        Serial.println("Warning: i2s_read timed out.");
        // Continue loop, maybe no data available yet
    }
    else {
        Serial.printf("ERROR - i2s_read failed! Code: %d\n", read_err);
        // Consider stopping recording on read error
        break;
    }
    // Yield to allow other tasks (like WiFi) to run
    yield();
  }

  Serial.printf("Finished recording loop. Total audio bytes: %u\n", totalAudioBytes);

  // Stop I2S *before* finalizing the file
  i2s_stop(I2S_PORT);


  // --- Finalize WAV Header ---
  uint32_t fileSize = totalAudioBytes + WAV_HDR_SIZE - 8; // Overall file size minus RIFF and size fields
  uint32_t byteRate = SAMPLE_RATE * 1 * BYTES_PER_SAMPLE; // SampleRate * NumChannels * BytesPerSample
  uint16_t blockAlign = 1 * BYTES_PER_SAMPLE; // NumChannels * BytesPerSample

  // RIFF chunk descriptor
  wavHeader[0] = 'R'; wavHeader[1] = 'I'; wavHeader[2] = 'F'; wavHeader[3] = 'F';
  wavHeader[4] = (uint8_t)(fileSize & 0xFF);
  wavHeader[5] = (uint8_t)((fileSize >> 8) & 0xFF);
  wavHeader[6] = (uint8_t)((fileSize >> 16) & 0xFF);
  wavHeader[7] = (uint8_t)((fileSize >> 24) & 0xFF);
  wavHeader[8] = 'W'; wavHeader[9] = 'A'; wavHeader[10] = 'V'; wavHeader[11] = 'E';

  // "fmt " sub-chunk
  wavHeader[12] = 'f'; wavHeader[13] = 'm'; wavHeader[14] = 't'; wavHeader[15] = ' ';
  wavHeader[16] = 16; wavHeader[17] = 0; wavHeader[18] = 0; wavHeader[19] = 0; // Subchunk1Size (16 for PCM)
  wavHeader[20] = 1; wavHeader[21] = 0; // AudioFormat (1 for PCM)
  wavHeader[22] = 1; wavHeader[23] = 0; // NumChannels (1 for mono)
  wavHeader[24] = (uint8_t)(SAMPLE_RATE & 0xFF);
  wavHeader[25] = (uint8_t)((SAMPLE_RATE >> 8) & 0xFF);
  wavHeader[26] = (uint8_t)((SAMPLE_RATE >> 16) & 0xFF);
  wavHeader[27] = (uint8_t)((SAMPLE_RATE >> 24) & 0xFF);
  wavHeader[28] = (uint8_t)(byteRate & 0xFF);
  wavHeader[29] = (uint8_t)((byteRate >> 8) & 0xFF);
  wavHeader[30] = (uint8_t)((byteRate >> 16) & 0xFF);
  wavHeader[31] = (uint8_t)((byteRate >> 24) & 0xFF);
  wavHeader[32] = (uint8_t)(blockAlign & 0xFF); // BlockAlign
  wavHeader[33] = (uint8_t)((blockAlign >> 8) & 0xFF);
  wavHeader[34] = (uint8_t)(SAMPLE_BITS & 0xFF); // BitsPerSample
  wavHeader[35] = (uint8_t)((SAMPLE_BITS >> 8) & 0xFF);

  // "data" sub-chunk
  wavHeader[36] = 'd'; wavHeader[37] = 'a'; wavHeader[38] = 't'; wavHeader[39] = 'a';
  wavHeader[40] = (uint8_t)(totalAudioBytes & 0xFF); // Subchunk2Size (data size)
  wavHeader[41] = (uint8_t)((totalAudioBytes >> 8) & 0xFF);
  wavHeader[42] = (uint8_t)((totalAudioBytes >> 16) & 0xFF);
  wavHeader[43] = (uint8_t)((totalAudioBytes >> 24) & 0xFF);

  // Seek back to the beginning and write the correct header
  if (!audioFile.seek(0)) {
     Serial.println("ERROR - Failed to seek to beginning of file");
     // File might be corrupted, handle appropriately
  } else {
      written = audioFile.write(wavHeader, WAV_HDR_SIZE);
       if (written != WAV_HDR_SIZE) {
          Serial.println("ERROR - Failed to write final WAV header");
          // File might be corrupted
       }
  }

  audioFile.close(); // Close the file

  // Uninstall I2S driver AFTER file is closed and header is written
  if (i2s_installed) {
      err = i2s_driver_uninstall(I2S_PORT);
      if (err != ESP_OK) {
          Serial.printf("ERROR - Failed to uninstall I2S driver! Code: %d\n", err);
      }
      i2s_installed = false; // Mark as uninstalled regardless of error
  }

  Serial.printf("Recording complete: %u audio data bytes written to %s.\n", totalAudioBytes, FILENAME);

  return totalAudioBytes > 0; // Return true if some audio was recorded
}

// Send recorded audio to Deepgram API
// Returns the JSON response string or an empty string on failure.
String transcribeAudio() {
  // Ensure WiFi is connected and SPIFFS is begun in the main sketch before calling
  Serial.println("Connecting to Deepgram...");

  // Allow a bit of time for the system to stabilize - maybe less needed here
  yield();
  // delay(100); // Shorter delay?

  // Create a secure client (use Root CA for production)
  WiFiClientSecure client;
  // IMPORTANT: For production, use client.setCACert(rootCACertificate)
  // instead of setInsecure(). Get the appropriate CA cert for api.deepgram.com.
  client.setInsecure(); // For demo/testing ONLY
  client.setTimeout(20000); // Increased timeout for potentially slow network/API

  HTTPClient https;
  https.setReuse(false); // Less likely to cause issues on ESP32
  https.setTimeout(20000); // Match client timeout

  // Use HTTPS connection
  if (!https.begin(client, "https://api.deepgram.com/v1/listen?model=nova-2-general&detect_language=true")) {
    Serial.println("ERROR - HTTPS begin failed");
    return ""; // Return empty on failure
  }

  // Add necessary headers
  https.addHeader("Content-Type", "audio/wav");
  https.addHeader("Authorization", String("Token ") + deepgramApiKey);
  https.addHeader("Connection", "close"); // Explicitly close connection

  // Open the recorded file for reading
  File audioFile = SPIFFS.open(FILENAME, "r");
  if (!audioFile) {
    Serial.println("ERROR - Failed to open audio file for reading");
    https.end();
    return ""; // Return empty on failure
  }
  if (!audioFile.available()) {
      Serial.println("ERROR - Audio file is empty or unavailable.");
      audioFile.close();
      https.end();
      return "";
  }

  size_t fileSize = audioFile.size();
  Serial.printf("Sending %s (%d bytes) to Deepgram...\n", FILENAME, fileSize);

  // Send the file using stream method
  int httpCode = https.sendRequest("POST", &audioFile, fileSize);
  audioFile.close(); // Close the file *after* request is sent

  Serial.printf("HTTP response code: %d\n", httpCode);

  // Process the response
  String response = "";
  if (httpCode > 0) { // Check if we got any HTTP status code
    response = https.getString(); // Get response payload regardless of code initially
    if (httpCode >= 200 && httpCode < 300) { // Success range
      Serial.println("Deepgram request successful.");
      // Optionally print raw response for debugging:
      // Serial.println("Raw Response: " + response);
    } else {
      // Handle HTTP errors (4xx, 5xx)
      Serial.printf("ERROR - Deepgram request failed with HTTP code: %d\n", httpCode);
      Serial.println("Error Payload: " + response); // Print error message from Deepgram
      response = ""; // Clear response string on error to indicate failure
    }
  } else {
    // Handle connection errors (httpCode < 0)
    Serial.printf("ERROR - HTTPS request failed. Error: %s\n", https.errorToString(httpCode).c_str());
    response = ""; // Clear response string on error
  }

  https.end(); // Release resources

  // Short delay for stability
  yield();
  // delay(100);

  if (response.isEmpty() && httpCode >= 200 && httpCode < 300) {
      Serial.println("Warning: Successful HTTP code but empty response body.");
      // This might be valid depending on the API, but often indicates an issue.
  }

  return response; // Return the JSON response or empty string
}

// Parse the Deepgram JSON response
// Returns the transcribed text or an empty string on failure/no transcript.
String parseTranscription(String response) {
  if (response.isEmpty()) {
    Serial.println("Parsing skipped: Empty response received.");
    return "";
  }

  // Adjust JSON document size based on expected response complexity
  // DynamicJsonDocument doc(ESP.getMaxAllocHeap() / 4); // Example: Use portion of available heap
  DynamicJsonDocument doc(3072); // Increased size, adjust as needed
  DeserializationError jsonError = deserializeJson(doc, response);

  if (jsonError) {
    Serial.print("JSON parsing failed: ");
    Serial.println(jsonError.c_str());
    // Print response only if parsing failed, might be large
    // Serial.println("Response causing error was: ");
    // Serial.println(response);
    return ""; // Return empty on parsing error
  }

  // Safely navigate the JSON structure
  JsonVariant transcriptVar = doc["results"][0]["channels"][0]["alternatives"][0]["transcript"];

  if (transcriptVar.isNull() || !transcriptVar.is<const char*>()) {
    Serial.println("ERROR - Transcript data not found or not a string in JSON response.");
    // Optionally print the JSON structure for debugging:
    // serializeJsonPretty(doc, Serial);
    // Serial.println();
    return ""; // Return empty if transcript path is invalid
  }

  // Extract the transcript
  String transcript = transcriptVar.as<String>();
  Serial.println("Transcription: " + transcript);
  return transcript;
}

// Main orchestrator function
// Returns the final transcribed string or an empty string on failure at any step.
String recordAndTranscribe() {
  // Ensure SPIFFS is initialized in the main setup() before calling this
  Serial.println("Starting record and transcribe process...");

  // Step 1: Record audio
  if (!recordAudio()) {
    Serial.println("Recording step failed!");
    // No SPIFFS.end() here, assume main sketch handles it if needed
    return ""; // Return empty string on failure
  }
  Serial.println("Recording step completed successfully.");

  // Step 2: Transcribe audio (requires WiFi connection)
  String jsonResponse = transcribeAudio();
  if (jsonResponse.isEmpty()) {
    Serial.println("Transcription step failed!");
    // No SPIFFS.end() here
    return ""; // Return empty string on failure
  }
  Serial.println("Transcription step completed successfully.");

  // Step 3: Parse response to get transcript
  String finalTranscript = parseTranscription(jsonResponse);
  if (finalTranscript.isEmpty()) {
      Serial.println("Parsing step failed or no transcript found.");
      // Still return empty string from parseTranscription
  } else {
      Serial.println("Parsing step completed successfully.");
  }

  // Optional: Clean up the recorded file if no longer needed
  // if (SPIFFS.exists(FILENAME)) {
  //   SPIFFS.remove(FILENAME);
  //   Serial.printf("Removed temporary file: %s\n", FILENAME);
  // }

  Serial.println("Record and transcribe process finished.");
  return finalTranscript; // Return the parsed transcript (or "" if any step failed)
}

// NO setup() or loop() functions in this file.
// They should exist in your main sketch .ino file.
