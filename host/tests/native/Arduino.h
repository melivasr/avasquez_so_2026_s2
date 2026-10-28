#pragma once
#include <stddef.h>
#include <stdint.h>
#include "pico/time.h"
#define LED_BUILTIN 25
#define OUTPUT 1
#define INPUT_PULLUP 2
#define CHANGE 3
#define LOW 0
class NativeSerial {
public:
    void begin(unsigned) {}
    int available();
    int availableForWrite();
    int read();
    size_t write(const uint8_t *,size_t);
};
extern NativeSerial Serial;
uint32_t micros();
uint32_t millis();
void pinMode(uint8_t,int);
void digitalWrite(uint8_t,int);
int digitalRead(uint8_t);
int digitalPinToInterrupt(uint8_t);
void attachInterrupt(int,void (*)(),int);
