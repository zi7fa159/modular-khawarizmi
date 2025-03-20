#include <Arduino.h>
#include "Audio.h"
#include "UrlEncode.h" 
#include "WiFi.h"

extern Audio audio;  // Use the global `audio` object from the main file

void playTTS(String text) {
    const int I2S_BCLK = 26;  // Local constants
    const int I2S_LRC = 25;
    const int I2S_DOUT = 22;

    const char* BASE_URL = "https://smoltts.vercel.app/api/waves";  
    const char* VOICE_ID = "arman";  

    if (WiFi.status() != WL_CONNECTED) return;  

    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(100);  

    String encodedText = urlEncode(text);  
    String url = String(BASE_URL) + "?text=" + encodedText + "&voice_id=" + VOICE_ID;

    audio.connecttohost(url.c_str());

    while (audio.isRunning()) audio.loop();  
}
