/**
 * This example turns the ESP32 into a Bluetooth LE keyboard that writes the words, presses Enter, presses a media key and then Ctrl+Alt+Delete
 */
#include <Arduino.h>
#include "buttons.h"

uint8_t PWR_LED = 13;
uint8_t PLAY_PAUSE = 15;
uint8_t VOL_UP = 18;
uint8_t VOL_DOWN = 19;

RTC_DATA_ATTR int clickCount = 0;

void onPlayPauseClick()
{
  Serial.printf("Play/Pause clicked %d times!\n", clickCount);
  clickCount++;
}

void onVolUpClick()
{
  Serial.printf("Vol +\n");
}

void onVolDownClick()
{
  Serial.printf("Vol -\n");
}

void setup()
{
  Serial.begin(115200);
  // delay(1000);

  pinMode(PWR_LED, OUTPUT);
  pinMode(PLAY_PAUSE, INPUT_PULLUP);
  pinMode(VOL_UP, INPUT_PULLUP);
  pinMode(VOL_DOWN, INPUT_PULLUP);

  onClick(PLAY_PAUSE, onPlayPauseClick);
  onClick(VOL_UP, onVolUpClick);
  onClick(VOL_DOWN, onVolDownClick);
  // attachInterrupt(digitalPinToInterrupt(PLAY_PAUSE), onPlayPauseClick, CHANGE);
  // attachInterrupt(digitalPinToInterrupt(VOL_UP), onVolUpClick, CHANGE);
  // attachInterrupt(digitalPinToInterrupt(VOL_DOWN), onVolDownClick, CHANGE);
}

void loop()
{
  buttonEventLoop();
}