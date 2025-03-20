#include <SPIFFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>

// Function to record audio and save it as a WAV file
bool recordAudio() {
    const int I2S_PORT = I2S_NUM_0;
    const int I2S_WS = 15;
    const int I2S_SCK = 14;
    const int I2S_SD = 32;
    const int SAMPLE_RATE = 16000;
    const int SAMPLE_BITS = 16;
    const int BYTES_PER_SAMPLE = SAMPLE_BITS / 8;
    const int WAV_HDR_SIZE = 44;
    const int RECORD_TIME = 5; // seconds
    const int BUF_SIZE = 512;
    const char* FILENAME = "/rec.wav";

    bool i2s_installed = false;
    
    Serial.println("Initializing I2S...");

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

    if (i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL) != ESP_OK) {
        Serial.println("ERROR - I2S installation failed!");
        return false;
    }
    i2s_installed = true;

    if (i2s_set_pin(I2S_PORT, &pin_config) != ESP_OK) {
        Serial.println("ERROR - I2S pin configuration failed!");
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }

    i2s_start(I2S_PORT);

    File audioFile = SPIFFS.open(FILENAME, "w");
    if (!audioFile) {
        Serial.println("ERROR - Failed to open file for writing");
        i2s_stop(I2S_PORT);
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }

    uint8_t wavHeader[WAV_HDR_SIZE] = {0};
    audioFile.write(wavHeader, WAV_HDR_SIZE);

    Serial.println("Recording...");
    int16_t buffer[BUF_SIZE];
    size_t bytesRead = 0, totalAudioBytes = 0;
    uint32_t startTime = millis();

    while (millis() - startTime < RECORD_TIME * 1000) {
        if (i2s_read(I2S_PORT, buffer, sizeof(buffer), &bytesRead, portMAX_DELAY) == ESP_OK && bytesRead > 0) {
            audioFile.write((uint8_t*)buffer, bytesRead);
            totalAudioBytes += bytesRead;
        }
        yield();
    }

    audioFile.close();
    Serial.printf("Recording complete: %u bytes written.\n", totalAudioBytes);

    i2s_stop(I2S_PORT);
    i2s_driver_uninstall(I2S_PORT);

    return totalAudioBytes > 0;
}

// Function to send recorded audio to Deepgram API
String transcribeAudio(const char* deepgramApiKey) {
    const char* FILENAME = "/rec.wav";

    Serial.println("Connecting to Deepgram...");
    yield();
    delay(500);

    WiFiClientSecure *client = new WiFiClientSecure;
    client->setInsecure();
    client->setTimeout(15000);

    HTTPClient https;
    https.setTimeout(15000);

    if (!https.begin(*client, "https://api.deepgram.com/v1/listen?model=nova-2-general&detect_language=true")) {
        Serial.println("ERROR - HTTPS setup failed");
        delete client;
        return "";
    }

    https.addHeader("Content-Type", "audio/wav");
    https.addHeader("Authorization", String("Token ") + deepgramApiKey);

    File audioFile = SPIFFS.open(FILENAME, "r");
    if (!audioFile) {
        Serial.println("ERROR - Failed to open audio file for reading");
        https.end();
        delete client;
        return "";
    }

    int httpCode = https.sendRequest("POST", &audioFile, audioFile.size());
    audioFile.close();

    Serial.printf("HTTP response code: %d\n", httpCode);

    String response = "";
    if (httpCode == HTTP_CODE_OK) {
        response = https.getString();
    } else {
        Serial.println("ERROR - Failed to get response from Deepgram");
    }

    https.end();
    delete client;
    yield();
    delay(500);

    return response;
}

// Function to parse JSON response and extract transcript
String parseTranscription(String response) {
    DynamicJsonDocument doc(2048);
    DeserializationError jsonError = deserializeJson(doc, response);

    if (jsonError) {
        Serial.print("JSON parsing failed: ");
        Serial.println(jsonError.c_str());
        return "";
    }

    if (doc["results"]["channels"][0]["alternatives"][0].containsKey("transcript")) {
        String transcript = doc["results"]["channels"][0]["alternatives"][0]["transcript"].as<String>();
        Serial.println("Transcription: " + transcript);
        return transcript;
    } else {
        Serial.println("ERROR - Transcript not found in the JSON response.");
        return "";
    }
}

// Function to handle full audio recording and transcription process
String recordAndTranscribe(const char* deepgramApiKey) {
    Serial.println("Initializing SPIFFS...");
    if (!SPIFFS.begin(true)) {
        Serial.println("ERROR - SPIFFS initialization failed!");
        return "";
    }

    if (!recordAudio()) {
        Serial.println("Recording failed!");
        return "";
    }

    String jsonResponse = transcribeAudio(deepgramApiKey);
    if (jsonResponse.isEmpty()) {
        Serial.println("Transcription request failed!");
        return "";
    }

    return parseTranscription(jsonResponse);
}
