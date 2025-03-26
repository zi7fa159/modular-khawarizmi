#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>


const char* Gemini_Token = "APIKEY";
const char* Gemini_Max_Tokens = "100";
String res = "";

String askQuestion(String prompt) {
  // Format the prompt (wrap it in quotes for the payload)
  String formattedPrompt = "\"" + prompt + "\"";
  
  Serial.println();
  Serial.print("Asking Your Question: ");
  Serial.println(formattedPrompt);
  
  HTTPClient https;
  String answer = "";

  // Build the API URL using your Gemini token
  String url = String("https://generativelanguage.googleapis.com/v1beta/models/gemini-2.0-flash:generateContent?key=") + Gemini_Token;
  
  if (https.begin(url)) {  // Initialize HTTPS connection
    https.addHeader("Content-Type", "application/json");
    
    // Construct the JSON payload
    String payload = "{\"contents\": [{\"parts\":[{\"text\":" + formattedPrompt + "}]}],"
                     "\"generationConfig\": {\"maxOutputTokens\": " + String(Gemini_Max_Tokens) + "}}";
    
    int httpCode = https.POST(payload);
    
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
      String responsePayload = https.getString();
      
      // Allocate a JSON document
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, responsePayload);
      if (!error) {
        // Extract the answer from the JSON response
        answer = doc["candidates"][0]["content"]["parts"][0]["text"].as<String>();
        
        // Remove extra whitespace and filter out non-alphanumeric characters
        answer.trim();
        String filteredAnswer = "";
        for (size_t i = 0; i < answer.length(); i++) {
          char c = answer[i];
          if (isalnum(c) || isspace(c)) {
            filteredAnswer += c;
          } else {
            filteredAnswer += ' ';
          }
        }
        answer = filteredAnswer;
        
        Serial.println();
        Serial.println("Here is your Answer:");
        Serial.println(answer);
      } else {
        Serial.println("Failed to parse JSON response");
      }
    } else {
      Serial.printf("[HTTPS] POST failed, error: %s\n", https.errorToString(httpCode).c_str());
    }
    https.end();  // End connection
  } else {
    Serial.printf("[HTTPS] Unable to connect\n");
  }
  
  return answer;
}
