# reThingSpeak for ESP32

### ВНИМАНИЕ ! УСТАРЕВШАЯ БИБЛИОТЕКА ! 
### Теперь вместо нее следует использовать https://github.com/kotyara12/reDataSend
---

Sending sensor data to https://thingspeak.com/ with a specified interval and sending queue. For ESP32 only, since it was released as a FreeRTOS task and on ESP32-specific functions. Channel field values (data) are passed to the queue as a string (char*), which is automatically deleted after sending. That is, to send, you must place a line with data on the heap, and then send it to the library queue.

## Dependencies:
  - https://github.com/kotyara12/rLog
  - https://github.com/kotyara12/rStrings
  - https://github.com/kotyara12/reEsp32
  - https://github.com/kotyara12/reLed
  - https://github.com/kotyara12/reWifi

### Notes:
  - libraries starting with the <b>re</b> prefix are only suitable for ESP32 and ESP-IDF
  - libraries starting with the <b>ra</b> prefix are only suitable for ARDUINO compatible code
  - libraries starting with the <b>r</b> prefix can be used in both cases (in ESP-IDF and in ARDUINO)
