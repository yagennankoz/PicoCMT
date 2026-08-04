#pragma once

// Common hardware / behavior configuration
#define SCREEN_ADDRESS   0x3C

// Common stream and queue sizing
#define BUFFER_SIZE      512
#define SD_WRITE_TRIGGER 512
#define SD_READ_SIZE     512
#define PLAY_QUEUE_LENGTH 2048

#define DEBOUNCE_MS      20
#define LONG_PRESS_MS    1000
#define REPEAT_MS        60

#define REC_TIMEOUT_MS   10000

#define LINE_HEIGHT      16

#define MAX_DATA_PAYLOAD 32768
