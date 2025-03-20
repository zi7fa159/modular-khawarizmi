#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

String askQuestion(String prompt) {
    // Local Constants - Only Used Inside this Function
    const char* GEMINI_TOKEN = "YOUR_API_KEY";
    const char* MAX_TOKENS = "100";

    // Format the prompt
    String formattedPrompt = "\"" + prompt + "\"";
    
    Serial.println("\n[DEBUG] Asking: " + formattedPrompt);
    
    HTTPClient https;
    String answer = "";
    
    // API URL
    String url = String("https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=") + GEMINI_TOKEN;
    
    if (https.begin(url)) {  
        https.addHeader("Content-Type", "application/json");
        
        // JSON Payload
        String payload = "{\"contents\": [{\"parts\":[{\"text\":" + formattedPrompt + "}]}],"
                         "\"generationConfig\": {\"maxOutputTokens\": " + String(MAX_TOKENS) + "}}";
        
        int httpCode = https.POST(payload);
        
        Serial.printf("[DEBUG] HTTP Response Code: %d\n", httpCode);

        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
            String responsePayload = https.getString();
            
            DynamicJsonDocument doc(1024);
            DeserializationError error = deserializeJson(doc, responsePayload);
            if (!error) {
                answer = doc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
                answer.trim();
                Serial.println("[DEBUG] Answer: " + answer);
            } else {
                Serial.println("[ERROR] Failed to parse JSON response");
            }
        } else {
            Serial.printf("[ERROR] HTTPS POST failed: %s\n", https.errorToString(httpCode).c_str());
        }
        
        https.end();
    } else {
        Serial.println("[ERROR] HTTPS Connection Failed");
    }
    
    return answer;
}
