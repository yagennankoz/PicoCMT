// ============================================================================
// PC上でのテスト用 Arduino.h モック (必要な分だけ)
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <functional>
#include <string>

#include "hardware/watchdog.h"

typedef uint8_t byte;

// 時間はテストから進める
inline uint32_t mock_millis_value = 0;
inline uint32_t millis() { return mock_millis_value; }
inline void delay(uint32_t ms) { mock_millis_value += ms; }
inline void delayMicroseconds(uint32_t) {}

class String : public std::string {
   public:
    String() {}
    String(const char* s) : std::string(s ? s : "") {}
    String(const std::string& s) : std::string(s) {}
};
