#include <Arduino.h>
#include "Audio.h"
#include "UrlEncode.h" 
#include "WiFi.h"

extern Audio audio;  // Use global `audio` from main file

void playTTS(String text) {
    // HARD-CODED CONSTANTS
    audio.setPinout(26, 25, 22);  // I2S_BCLK, I2S_LRC, I2S_DOUT
    audio.setVolume(100);  

    if (WiFi.status() != WL_CONNECTED) return;  

    String encodedText = urlEncode(text);
    String url = "https://smoltts.vercel.app/api/waves?text=" + encodedText + "&voice_id=arman";

    audio.connecttohost(url.c_str());

    while (audio.isRunning()) audio.loop();  
}
