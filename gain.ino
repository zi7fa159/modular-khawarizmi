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
#include <limits.h> // Required for INT16_MAX, INT16_MIN used in gain clamping

// --- IMPORTANT EXTERNAL REQUIREMENTS ---
// 1. WiFi MUST be connected before calling transcribeAudio() or recordAndTranscribe().
// 2. SPIFFS MUST be initialized with SPIFFS.begin() in the main setup() before calling any function here.
// 3. Partition Scheme: Ensure you've selected a partition scheme in Arduino IDE Tools menu
//    that provides sufficient SPIFFS space (e.g., Default, Huge App, or >= 1MB SPIFFS).
//    Re-upload after changing the partition scheme.
// ---

// Deepgram API key – replace with your actual key
const char* deepgramApiKey = "ef37ae23ce94d481b566ea439423b1e305c08038"; // <-- PUT YOUR KEY HERE

// I2S and recording settings
const i2s_port_t I2S_PORT = I2S_NUM_1;
const int I2S_WS        = 15; // Word Select / LRCL
const int I2S_SCK       = 14; // Bit Clock / BCLK
const int I2S_SD        = 32; // Serial Data / DIN
const int SAMPLE_RATE   = 16000; // 16kHz sample rate
const int SAMPLE_BITS   = 16;    // 16 bits per sample
const int BYTES_PER_SAMPLE = (SAMPLE_BITS / 8); // 2 bytes
const int RECORD_TIME   = 10;     // seconds to record
const int I2S_BUFFER_SIZE = 512; // Bytes for I2S read buffer (needs to be multiple of BYTES_PER_SAMPLE * channels)
const int DMA_BUFFER_COUNT= 4;   // Number of DMA buffers
// Samples per DMA buffer: I2S_BUFFER_SIZE / DMA_BUFFER_COUNT / BYTES_PER_SAMPLE / num_channels
const int DMA_BUFFER_LEN  = I2S_BUFFER_SIZE / DMA_BUFFER_COUNT / BYTES_PER_SAMPLE; // Samples per DMA buffer (512 / 4 / 2 = 64 for mono)

// WAV File Settings
const int WAV_HDR_SIZE  = 44;    // Size of a standard WAV header
const char* FILENAME    = "/rec.wav"; // Filename on SPIFFS

// --- Define the audio gain multiplier ---
// Adjust this value to increase/decrease volume.
// 1.0 = no change, > 1.0 = amplify, < 1.0 = attenuate.
#define AUDIO_GAIN_MULTIPLIER 2.0f

// Global flag for I2S state (use cautiously if functions could be interrupted)
bool i2s_installed = false;

// --- Function Prototypes ---
bool recordAudio();
String transcribeAudio();
String parseTranscription(String response);
String recordAndTranscribe();
// --- End Prototypes ---


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
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = (i2s_bits_per_sample_t)SAMPLE_BITS,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, // Mono microphone
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = DMA_BUFFER_COUNT,
    .dma_buf_len = DMA_BUFFER_LEN, // Samples per DMA buffer
    .use_apll = false,
    .tx_desc_auto_clear = false, // Not used for RX
    .fixed_mclk = 0             // Not used for RX
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
    i2s_driver_uninstall(I2S_PORT); // Attempt cleanup
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
    i2s_driver_uninstall(I2S_PORT); // Attempt cleanup
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
  Serial.printf("Recording for %d seconds with gain %.2f...\n", RECORD_TIME, AUDIO_GAIN_MULTIPLIER);
  uint8_t i2s_read_buffer[I2S_BUFFER_SIZE]; // Buffer to hold data from I2S driver
  size_t bytesRead = 0;
  uint32_t totalAudioBytes = 0; // Track successfully written audio bytes
  uint32_t startTime = millis();

  while (millis() - startTime < (uint32_t)RECORD_TIME * 1000) {
    // Read data from I2S into buffer
    esp_err_t read_err = i2s_read(I2S_PORT, i2s_read_buffer, I2S_BUFFER_SIZE, &bytesRead, pdMS_TO_TICKS(100)); // Short timeout

    if (read_err == ESP_OK && bytesRead > 0) {
      // Ensure bytesRead is a multiple of sample size, should always be true with I2S
      if (bytesRead % BYTES_PER_SAMPLE != 0) {
          Serial.printf("Warning: bytesRead (%d) not multiple of BYTES_PER_SAMPLE (%d)\n", bytesRead, BYTES_PER_SAMPLE);
          // Decide how to handle: skip buffer, process partial, etc. Skipping is safest.
          continue;
      }

      // --- Apply Gain (Digital Volume Control) ---
      // Cast buffer to int16_t pointer for sample access (assuming 16-bit samples)
      int16_t* samples = (int16_t*)i2s_read_buffer;
      int num_samples = bytesRead / BYTES_PER_SAMPLE; // Calculate number of 16-bit samples

      for (int i = 0; i < num_samples; i++) {
        // Apply gain using floating point for accuracy with non-integer gains.
        // Use int32_t for the intermediate result to prevent overflow during multiplication,
        // as int16_t * gain can exceed the int16_t range.
        int32_t amplified_sample = (int32_t)((float)samples[i] * AUDIO_GAIN_MULTIPLIER);

        // Clamp the amplified sample to the valid range for int16_t.
        // This prevents clipping distortion caused by wrap-around (e.g., 32768 becoming -32768).
        if (amplified_sample > INT16_MAX) {
          samples[i] = INT16_MAX;
        } else if (amplified_sample < INT16_MIN) {
          samples[i] = INT16_MIN;
        } else {
          // Cast back to int16_t after clamping
          samples[i] = (int16_t)amplified_sample;
        }
      }
      // --- End Apply Gain ---

      // Write the processed (amplified and clamped) data to the file
      size_t written_bytes = audioFile.write(i2s_read_buffer, bytesRead);

      // *** CRITICAL CHECK: Verify all bytes were written ***
      if (written_bytes == bytesRead) {
          totalAudioBytes += bytesRead; // Increment only if write was successful
      } else {
          Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
          Serial.printf("ERROR - Failed to write all %d bytes to SPIFFS! Wrote only %d. DISK FULL?\n", bytesRead, written_bytes);
          Serial.println("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
          recording_success = false; // Mark recording as failed
          break; // Exit the recording loop immediately
      }
    } else if (read_err == ESP_ERR_TIMEOUT) {
        // Ignore timeout, common if no data is ready from I2S yet.
        // Serial.print("."); // Uncomment for verbose logging of timeouts
    } else {
        // Handle other potential I2S read errors
        Serial.printf("ERROR - i2s_read failed! Code: %d (%s)\n", read_err, esp_err_to_name(read_err));
        recording_success = false; // Mark recording as failed
        break; // Exit the recording loop on read error
    }
    yield(); // Allow other tasks (like WiFi background processes) to run
  }

  Serial.printf("Finished recording loop. Status OK: %s, Bytes Written: %u\n", recording_success ? "Yes" : "No", totalAudioBytes);

  // Stop I2S *before* finalizing file header or uninstalling driver
  i2s_stop(I2S_PORT);

  // --- Finalize WAV Header ---
  // Calculate final sizes based on successfully written bytes
  uint32_t fileSize = totalAudioBytes + WAV_HDR_SIZE - 8; // RIFF chunk size
  uint32_t dataChunkSize = totalAudioBytes;
  uint32_t formatChunkSize = 16;
  uint16_t audioFormat = 1; // PCM
  uint16_t numChannels = 1; // Mono
  uint32_t sampleRate = SAMPLE_RATE;
  uint32_t byteRate = sampleRate * numChannels * BYTES_PER_SAMPLE;
  uint16_t blockAlign = numChannels * BYTES_PER_SAMPLE;
  uint16_t bitsPerSample = SAMPLE_BITS;


  // RIFF chunk descriptor
  wavHeader[0] = 'R'; wavHeader[1] = 'I'; wavHeader[2] = 'F'; wavHeader[3] = 'F';
  memcpy(&wavHeader[4], &fileSize, 4); // ChunkSize
  wavHeader[8] = 'W'; wavHeader[9] = 'A'; wavHeader[10] = 'V'; wavHeader[11] = 'E'; // Format
  // "fmt " sub-chunk
  wavHeader[12] = 'f'; wavHeader[13] = 'm'; wavHeader[14] = 't'; wavHeader[15] = ' '; // Subchunk1ID
  memcpy(&wavHeader[16], &formatChunkSize, 4); // Subchunk1Size (16 for PCM)
  memcpy(&wavHeader[20], &audioFormat, 2);     // AudioFormat (1 for PCM)
  memcpy(&wavHeader[22], &numChannels, 2);     // NumChannels
  memcpy(&wavHeader[24], &sampleRate, 4);      // SampleRate
  memcpy(&wavHeader[28], &byteRate, 4);        // ByteRate
  memcpy(&wavHeader[32], &blockAlign, 2);      // BlockAlign
  memcpy(&wavHeader[34], &bitsPerSample, 2);   // BitsPerSample
  // "data" sub-chunk
  wavHeader[36] = 'd'; wavHeader[37] = 'a'; wavHeader[38] = 't'; wavHeader[39] = 'a'; // Subchunk2ID
  memcpy(&wavHeader[40], &dataChunkSize, 4); // Subchunk2Size (data size)


  // Seek to beginning and write the finalized header
  if (!audioFile.seek(0)) {
     Serial.println("ERROR - Failed to seek to beginning of WAV file to write header!");
     recording_success = false; // Mark as failed if header can't be written
  } else {
      written = audioFile.write(wavHeader, WAV_HDR_SIZE);
      if (written != WAV_HDR_SIZE) {
          Serial.printf("ERROR - Failed to write final WAV header! Wrote %d bytes.\n", written);
          recording_success = false; // Mark as failed
      } else {
          Serial.println("Final WAV header written successfully.");
      }
  }

  // Close the file (releases file handle)
  audioFile.close();
  Serial.printf("Audio file '%s' closed.\n", FILENAME);

  // Uninstall I2S driver *after* file is closed and I2S is stopped
  if (i2s_installed) {
      err = i2s_driver_uninstall(I2S_PORT);
      if (err != ESP_OK) {
          // Log error but don't necessarily fail the whole recording if file ops succeeded
          Serial.printf("Warning - Failed to uninstall I2S driver cleanly. Code: %d (%s)\n", err, esp_err_to_name(err));
      } else {
          Serial.println("I2S driver uninstalled.");
      }
      i2s_installed = false; // Ensure flag is cleared even if uninstall fails
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

  if (WiFi.status() != WL_CONNECTED) {
      Serial.println("ERROR - WiFi not connected. Cannot transcribe.");
      return "";
  }

  // Allow network stack time to stabilize if needed
  yield(); delay(100);

  // Create secure client (Use Root CA for production!)
  WiFiClientSecure client;
  // >>> IMPORTANT SECURITY NOTE <<<
  // setInsecure() skips server certificate validation.
  // This is convenient for testing but makes the connection vulnerable to
  // Man-In-The-Middle attacks.
  // For production, use client.setCACert(root_ca_certificate_string)
  // with the appropriate root CA certificate for api.deepgram.com.
  client.setInsecure();
  client.setTimeout(20000); // 20 seconds timeout for connection and transfer

  HTTPClient https;
  https.setReuse(false); // Helps avoid potential state issues on ESP32 with multiple requests
  https.setTimeout(20000); // Match client timeout

  // Construct Deepgram API URL
  // Example: Specify model and language
  String deepgramURL = "https://api.deepgram.com/v1/listen?model=nova-2&language=en";
  // Alternative: Use auto-detect language and potentially different model
  // String deepgramURL = "https://api.deepgram.com/v1/listen?model=nova-2-general&detect_language=true";

  Serial.printf("Connecting to Deepgram: %s\n", deepgramURL.c_str());

  // Begin HTTPS connection
  // Use the WiFiClientSecure object
  if (!https.begin(client, deepgramURL)) {
    Serial.println("ERROR - HTTPS begin failed. Check URL, client setup, and memory.");
    return "";
  }

  // Add Headers
  https.addHeader("Authorization", String("Token ") + deepgramApiKey);
  https.addHeader("Content-Type", "audio/wav");
  // Consider adding Accept header if needed, though usually not required by Deepgram for standard response
  // https.addHeader("Accept", "application/json");
  https.addHeader("Connection", "close"); // Important for ESP32 stability

  // Open recorded file for reading
  File audioFile = SPIFFS.open(FILENAME, "r");
  if (!audioFile) {
    Serial.printf("ERROR - Failed to open audio file '%s' for reading!\n", FILENAME);
    https.end(); // Clean up HTTPS client
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
  size_t expectedMinSize = WAV_HDR_SIZE + (SAMPLE_RATE * BYTES_PER_SAMPLE / 10); // Header + ~0.1s audio
  if (fileSize < expectedMinSize) {
      Serial.printf("Warning: Audio file size (%d bytes) seems very small (min expected ~%d). Uploading anyway.\n", fileSize, expectedMinSize);
  }
  // Sanity check max size - avoid sending huge files if something went wrong
  size_t reasonableMaxSize = WAV_HDR_SIZE + (SAMPLE_RATE * BYTES_PER_SAMPLE * (RECORD_TIME + 5)); // Header + Expected audio + buffer
   if (fileSize > reasonableMaxSize ) {
       Serial.printf("Warning: Audio file size (%d bytes) seems too large (max expected ~%d). Possible recording issue? Uploading anyway.\n", fileSize, reasonableMaxSize);
   }

  Serial.printf("Sending '%s' (%d bytes) to Deepgram...\n", FILENAME, fileSize);

  // Send the POST request with the file stream
  // Pass the File object directly to sendRequest - more memory efficient for large files
  int httpCode = https.sendRequest("POST", &audioFile, fileSize);

  // It's crucial to close the file *after* sendRequest has finished reading it.
  audioFile.close();
  Serial.printf("File '%s' closed after sending.\n", FILENAME);

  Serial.printf("Deepgram HTTP response code: %d\n", httpCode);

  // Process response
  String response = "";
  if (httpCode > 0) { // Check if we received an HTTP status code from the server
    if (httpCode >= 200 && httpCode < 300) { // Success codes (e.g., 200 OK)
      Serial.println("Deepgram request successful.");
      // Get the response payload
      // Note: response can be large, ensure sufficient heap memory.
      response = https.getString();
      // --- Optional: Print raw response for debugging ---
      // Serial.println("--- RAW DEEPGRAM JSON RESPONSE ---");
      // Serial.println(response);
      // Serial.println("------------------------------------");
      // --- End Optional Print ---
      if (response.isEmpty()) {
         Serial.println("Warning: Successful HTTP code from Deepgram but received empty response body.");
      }
    } else { // Client or Server error codes (e.g., 4xx, 5xx)
      Serial.printf("ERROR - Deepgram request failed with HTTP code: %d\n", httpCode);
      // Attempt to get error payload for debugging
      String errorPayload = https.getString();
      Serial.println("Error Payload:");
      Serial.println(errorPayload);
      response = ""; // Ensure empty string is returned on API error
    }
  } else { // https.sendRequest failed (httpCode < 0), e.g., connection error, timeout
    Serial.printf("ERROR - HTTPS request failed. Error: %s\n", https.errorToString(httpCode).c_str());
    // Common causes: WiFi issue, DNS failure, timeout, server unreachable, incorrect client setup (like missing CA cert in secure mode)
    response = ""; // Ensure empty string is returned on connection error
  }

  https.end(); // Release resources associated with the HTTPClient instance
  Serial.println("HTTP client ended.");

  return response; // Return JSON response string or empty string on failure
}

/**
 * @brief Parses the JSON response from Deepgram to extract the transcript.
 * @param response The JSON string received from Deepgram. Should not be empty.
 * @return String containing the transcribed text, or empty string on failure or no transcript.
 */
String parseTranscription(String response) {
  // Pre-check: If the input response is empty, don't attempt parsing.
  if (response.isEmpty()) {
    Serial.println("Parsing skipped: Empty response string received.");
    return "";
  }

  // Estimate required document size. This might need adjustment based on:
  // - Length of transcription
  // - Whether word-level confidence, timestamps, etc., are enabled in the Deepgram request (affecting JSON size)
  // Start with a reasonable size. If parsing fails with NoMemory, increase this.
  // If memory is tight, consider using ArduinoJson Assistant to calculate precisely.
  const size_t jsonCapacity = JSON_OBJECT_SIZE(3) + JSON_OBJECT_SIZE(2) + JSON_ARRAY_SIZE(1) + JSON_OBJECT_SIZE(4) + JSON_ARRAY_SIZE(1) + JSON_OBJECT_SIZE(3) + response.length() * 2; // Rough estimate
  DynamicJsonDocument doc(jsonCapacity > 4096 ? jsonCapacity : 4096); // Use estimate or 4k minimum

  // Parse the JSON string
  DeserializationError jsonError = deserializeJson(doc, response);

  // Check for parsing errors
  if (jsonError) {
    Serial.print("ERROR - JSON parsing failed: ");
    Serial.println(jsonError.c_str());
    Serial.println("Received response was:");
    // Print only a snippet if it's very long to avoid flooding Serial
    if (response.length() > 500) {
        Serial.println(response.substring(0, 500) + "...");
    } else {
        Serial.println(response);
    }
    return ""; // Return empty on parsing failure
  }

  // Safely navigate the expected JSON structure
  // Structure: { "results": { "channels": [ { "alternatives": [ { "transcript": "..." } ] } ] } }
  // Using explicit checks at each level for robustness against slightly malformed JSON or API changes.
  JsonVariant resultsVar = doc["results"];
  if (resultsVar.isNull() || !resultsVar.is<JsonObject>()) {
      Serial.println("ERROR - JSON Parsing: 'results' key missing or not an object.");
      goto HandleJsonError; // Use goto for cleaner exit from nested checks
  }

  JsonVariant channelsVar = resultsVar["channels"];
  if (channelsVar.isNull() || !channelsVar.is<JsonArray>() || channelsVar.size() == 0) {
      Serial.println("ERROR - JSON Parsing: 'channels' key missing, not an array, or empty.");
      goto HandleJsonError;
  }

  JsonVariant channel0Var = channelsVar[0]; // Assuming mono audio, take the first channel
  if (channel0Var.isNull() || !channel0Var.is<JsonObject>()) {
      Serial.println("ERROR - JSON Parsing: First element in 'channels' is missing or not an object.");
      goto HandleJsonError;
  }

  JsonVariant alternativesVar = channel0Var["alternatives"];
  if (alternativesVar.isNull() || !alternativesVar.is<JsonArray>() || alternativesVar.size() == 0) {
      Serial.println("ERROR - JSON Parsing: 'alternatives' key missing, not an array, or empty.");
      goto HandleJsonError;
  }

  JsonVariant alternative0Var = alternativesVar[0]; // Taking the first alternative (usually the most likely)
  if (alternative0Var.isNull() || !alternative0Var.is<JsonObject>()) {
      Serial.println("ERROR - JSON Parsing: First element in 'alternatives' is missing or not an object.");
      goto HandleJsonError;
  }

  JsonVariant transcriptVar = alternative0Var["transcript"];
  // Check if the key exists AND it holds a string value
  if (transcriptVar.isNull() || !transcriptVar.is<const char*>()) {
    Serial.println("ERROR - JSON Parsing: 'transcript' key missing or not a string.");
    goto HandleJsonError;
  }

  // Extract the transcript string
  String transcript = transcriptVar.as<String>(); // .as<String>() creates a copy

  // Check if transcript is empty (API might return success but no words detected)
  if (transcript.isEmpty()) {
      Serial.println("Parsing successful, but transcript received is empty.");
      // This isn't necessarily an error, but maybe worth noting.
  } else {
      Serial.println("Transcription: " + transcript);
  }

  return transcript; // Return the extracted transcript (could be empty)

HandleJsonError:
    // Common error handling point if JSON structure checks fail
    Serial.println("Failed to navigate expected JSON structure. Response structure might have changed or be invalid.");
    Serial.println("Response Snippet:");
    if (response.length() > 500) {
        Serial.println(response.substring(0, 500) + "...");
    } else {
        Serial.println(response);
    }
    // Optional: Print the parsed doc structure for deep debugging
    // Serial.println("Parsed JSON structure:");
    // serializeJsonPretty(doc, Serial);
    // Serial.println();
    return ""; // Return empty string indicating failure to extract transcript
}


/**
 * @brief Orchestrates the entire process: records audio, sends for transcription, parses result.
 * @return String containing the final transcribed text, or empty string on failure at any step.
 */
String recordAndTranscribe() {
  // Ensure SPIFFS is initialized in the main setup() before calling this
  Serial.println("\n--- Starting Record and Transcribe Process ---");

  // Step 1: Record audio with gain
  Serial.println("Step 1: Recording Audio...");
  if (!recordAudio()) {
    Serial.println("Step 1 FAILED: Recording audio was unsuccessful.");
    // Note: recordAudio() should handle I2S cleanup on its internal failures.
    // We don't need to explicitly call i2s_driver_uninstall here unless recordAudio doesn't guarantee it.
    // Optional: Clean up potentially incomplete recording file if it exists
    if (SPIFFS.exists(FILENAME)) {
        if (SPIFFS.remove(FILENAME)) {
            Serial.printf("Cleaned up partially recorded file: %s\n", FILENAME);
        } else {
            Serial.printf("Warning: Failed to remove potentially partial file: %s\n", FILENAME);
        }
    }
    Serial.println("--- Record and Transcribe Process FAILED ---");
    return ""; // Exit if recording failed
  }
  Serial.println("Step 1 SUCCESS: Audio recorded.");

  // Step 2: Transcribe audio (requires WiFi connection)
  Serial.println("Step 2: Sending Audio for Transcription...");
  String jsonResponse = transcribeAudio();
  if (jsonResponse.isEmpty()) {
    Serial.println("Step 2 FAILED: Transcription request failed or returned empty/error response.");
    // No transcript means we can't proceed to parsing.
    // File cleanup will happen later regardless.
    Serial.println("--- Record and Transcribe Process FAILED ---");
    return ""; // Exit if transcription failed to return valid JSON response string
  }
  Serial.println("Step 2 SUCCESS: Received response from Deepgram.");

  // Step 3: Parse response to get transcript
  Serial.println("Step 3: Parsing Transcription...");
  String finalTranscript = parseTranscription(jsonResponse);
  // parseTranscription returns "" on failure OR if the transcript field itself was empty.
  if (finalTranscript.isEmpty() && !jsonResponse.isEmpty()) {
      // Distinguish between parsing failure and genuinely empty transcript
      // We know jsonResponse wasn't empty here. Check if parsing failed vs transcript field was empty.
      // Note: The parseTranscription function already prints detailed errors.
      Serial.println("Step 3 Note: Parsing finished, but no transcript text was extracted (either parsing error or empty transcript).");
      // Depending on requirements, you might treat this as a failure or just an empty result.
      // For now, we proceed but the returned string will be empty.
  } else if (!finalTranscript.isEmpty()) {
       Serial.println("Step 3 SUCCESS: Transcript parsed.");
  }
   // If finalTranscript is empty AND jsonResponse was empty, Step 2 already caught it.

  // Step 4: Clean up the recorded file now that it's processed (or attempted)
  Serial.println("Step 4: Cleaning up temporary file...");
  if (SPIFFS.exists(FILENAME)) {
    if(SPIFFS.remove(FILENAME)) {
        Serial.printf("Successfully removed temporary file: %s\n", FILENAME);
    } else {
        // This might indicate a SPIFFS issue.
        Serial.printf("Warning: Failed to remove temporary file: %s\n", FILENAME);
    }
  } else {
      Serial.printf("Note: Temporary file %s not found for cleanup (may have failed creation or already removed).\n", FILENAME);
  }

  Serial.println("--- Record and Transcribe Process Finished ---");
  return finalTranscript; // Return the parsed transcript (or "" if any step failed or transcript was empty)
}

// End of audio_functions.ino
