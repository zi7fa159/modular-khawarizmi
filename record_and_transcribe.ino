#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "SPIFFS.h"
#include <ArduinoJson.h>
#include <driver/i2s.h>

// Deepgram API key – replace with your actual key
const char* deepgramApiKey = "...";

// I2S and recording settings
const I2S_PORT      I2S_NUM_0
const int I2S_WS        15
const int I2S_SCK       14
const int I2S_SD        32
const int SAMPLE_RATE   16000
const int SAMPLE_BITS   16
const int BYTES_PER_SAMPLE (SAMPLE_BITS / 8)
const int WAV_HDR_SIZE  44
const int RECORD_TIME   5      // seconds
const int BUF_SIZE      512
const char FILENAME      "/rec.wav"

// Flag to track if I2S driver is installed
bool i2s_installed = false;

// Task watchdog timeout value (increased to prevent timeout)
const int WDT_TIMEOUT 30   // seconds

// Record audio to file
bool recordAudio() {
  Serial.println("Initializing I2S...");
  
  // Configure I2S
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = (i2s_bits_per_sample_t)SAMPLE_BITS,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = BUF_SIZE,
    .use_apll = false
  };
  
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };
  
  // Install & configure I2S driver
  if (i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL) != ESP_OK) {
    Serial.println("ERROR - I2S installation failed!");
    return false;
  }
  i2s_installed = true;
  
  if (i2s_set_pin(I2S_PORT, &pin_config) != ESP_OK) {
    Serial.println("ERROR - I2S pin configuration failed!");
    i2s_driver_uninstall(I2S_PORT);
    i2s_installed = false;
    return false;
  }
  
  i2s_start(I2S_PORT);
  
  // Open file for recording
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
  audioFile.write(wavHeader, WAV_HDR_SIZE);

  // Record audio data
  Serial.println("Recording...");
  int16_t buffer[BUF_SIZE];
  size_t bytesRead = 0, totalAudioBytes = 0;
  uint32_t startTime = millis();
  
  while (millis() - startTime < RECORD_TIME * 1000) {
    if (i2s_read(I2S_PORT, buffer, sizeof(buffer), &bytesRead, portMAX_DELAY) == ESP_OK && bytesRead > 0) {
      audioFile.write((uint8_t*)buffer, bytesRead);
      totalAudioBytes += bytesRead;
    }
    // Add a small yield to prevent watchdog timer from triggering
    yield();
  }
  
  // Update WAV header with actual data size and file size information
  uint32_t fileSize = totalAudioBytes + WAV_HDR_SIZE - 8;
  wavHeader[0] = 'R'; wavHeader[1] = 'I'; wavHeader[2] = 'F'; wavHeader[3] = 'F';
  wavHeader[4] = fileSize & 0xFF; 
  wavHeader[5] = (fileSize >> 8) & 0xFF; 
  wavHeader[6] = (fileSize >> 16) & 0xFF; 
  wavHeader[7] = (fileSize >> 24) & 0xFF;
  wavHeader[8] = 'W'; wavHeader[9] = 'A'; wavHeader[10] = 'V'; wavHeader[11] = 'E';
  wavHeader[12] = 'f'; wavHeader[13] = 'm'; wavHeader[14] = 't'; wavHeader[15] = ' ';
  wavHeader[16] = 16; wavHeader[17] = 0; wavHeader[18] = 0; wavHeader[19] = 0;
  wavHeader[20] = 1; wavHeader[21] = 0; 
  wavHeader[22] = 1; wavHeader[23] = 0;
  wavHeader[24] = SAMPLE_RATE & 0xFF; 
  wavHeader[25] = (SAMPLE_RATE >> 8) & 0xFF; 
  wavHeader[26] = (SAMPLE_RATE >> 16) & 0xFF; 
  wavHeader[27] = (SAMPLE_RATE >> 24) & 0xFF;
  uint32_t byteRate = SAMPLE_RATE * BYTES_PER_SAMPLE;
  wavHeader[28] = byteRate & 0xFF; 
  wavHeader[29] = (byteRate >> 8) & 0xFF; 
  wavHeader[30] = (byteRate >> 16) & 0xFF; 
  wavHeader[31] = (byteRate >> 24) & 0xFF;
  wavHeader[32] = BYTES_PER_SAMPLE; 
  wavHeader[33] = 0;
  wavHeader[34] = SAMPLE_BITS; 
  wavHeader[35] = 0;
  wavHeader[36] = 'd'; wavHeader[37] = 'a'; wavHeader[38] = 't'; wavHeader[39] = 'a';
  wavHeader[40] = totalAudioBytes & 0xFF; 
  wavHeader[41] = (totalAudioBytes >> 8) & 0xFF; 
  wavHeader[42] = (totalAudioBytes >> 16) & 0xFF; 
  wavHeader[43] = (totalAudioBytes >> 24) & 0xFF;
  
  audioFile.seek(0);
  audioFile.write(wavHeader, WAV_HDR_SIZE);
  audioFile.close();
  
  Serial.printf("Recording complete: %u bytes written.\n", totalAudioBytes);
  
  // Stop I2S
  i2s_stop(I2S_PORT);
  i2s_driver_uninstall(I2S_PORT);
  i2s_installed = false;
  
  return totalAudioBytes > 0;
}

// Send recorded audio to Deepgram API
String transcribeAudio() {
  Serial.println("Connecting to Deepgram...");
  
  // Allow a bit of time for the system to stabilize
  yield();
  delay(500);
  
  // Create a secure client with increased timeout
  WiFiClientSecure *client = new WiFiClientSecure;
  client->setInsecure(); // For demo purposes only
  client->setTimeout(15000); // Increase timeout to 15 seconds
  
  HTTPClient https;
  // Set timeout for HTTP client too
  https.setTimeout(15000);
  
  if (!https.begin(*client, "https://api.deepgram.com/v1/listen?model=nova-2-general&detect_language=true")) {
    Serial.println("ERROR - HTTPS setup failed");
    delete client;
    return "";
  }
  
  https.addHeader("Content-Type", "audio/wav");
  https.addHeader("Authorization", String("Token ") + deepgramApiKey);

  // Open the recorded file for reading
  File audioFile = SPIFFS.open(FILENAME, "r");
  if (!audioFile) {
    Serial.println("ERROR - Failed to open audio file for reading");
    https.end();
    delete client;
    return "";
  }
  
  // Send the file in smaller chunks to avoid watchdog timer issues
  int httpCode = https.sendRequest("POST", &audioFile, audioFile.size());
  audioFile.close();
  
  Serial.printf("HTTP response code: %d\n", httpCode);
  
  // If successful, get the response
  String response = "";
  if (httpCode == HTTP_CODE_OK) {
    response = https.getString();
  } else {
    Serial.println("ERROR - Failed to get response from Deepgram");
  }
  
  https.end();
  delete client;
  
  // Give the system some time to recover
  yield();
  delay(500);
  
  // Check if the response is valid
  if (response.length() == 0) {
    Serial.println("ERROR - Empty response from Deepgram.");
    return "";
  }
  
  return response;
}

// Parse the Deepgram JSON response
String parseTranscription(String response) {
  // Use a smaller JSON document size to avoid memory issues
  DynamicJsonDocument doc(2048);
  DeserializationError jsonError = deserializeJson(doc, response);
  
  if (jsonError) {
    Serial.print("JSON parsing failed: ");
    Serial.println(jsonError.c_str());
    return "";
  }
  
  // Extract the transcript if available
  if (doc["results"]["channels"][0]["alternatives"][0].containsKey("transcript")) {
    String transcript = doc["results"]["channels"][0]["alternatives"][0]["transcript"].as<String>();
    Serial.println("Transcription: " + transcript);
    return transcript;
  } else {
    Serial.println("ERROR - Transcript not found in the JSON response.");
    return "";
  }
}

// Main function that handles the complete process
String recordAndTranscribe() {
  // Initialize SPIFFS for file storage
  Serial.println("Initializing SPIFFS...");
  if (!SPIFFS.begin(true)) {
    Serial.println("ERROR - SPIFFS initialization failed!");
    return "";
  }
  
  // Step 1: Record audio
  if (!recordAudio()) {
    Serial.println("Recording failed!");
    return "";
  }
  
  // Step 2: Get JSON response from Deepgram
  String jsonResponse = transcribeAudio();
  if (jsonResponse.isEmpty()) {
    Serial.println("Transcription request failed!");
    return "";
  }
  
  // Step 3: Parse response to get transcript
  return parseTranscription(jsonResponse);
}

void setup() {
  delay(1000)
}

void loop() {
  // For now, just delay
  delay(1000);
}
