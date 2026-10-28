// ============================================================================
// PC上でのテスト用 pico/util/queue.h モック (シングルスレッド)
// ============================================================================
#pragma once

#include <stdint.h>
#include <string.h>

#include <vector>

typedef unsigned int uint;

typedef struct {
    std::vector<uint8_t> buf;
    uint element_size;
    uint capacity;
    uint head;
    uint count;
} queue_t;

inline void queue_init(queue_t* q, uint element_size, uint element_count) {
    q->buf.assign((size_t)element_size * element_count, 0);
    q->element_size = element_size;
    q->capacity = element_count;
    q->head = 0;
    q->count = 0;
}

inline uint queue_get_level(queue_t* q) { return q->count; }
inline bool queue_is_empty(queue_t* q) { return q->count == 0; }
inline bool queue_is_full(queue_t* q) { return q->count >= q->capacity; }

inline bool queue_try_add(queue_t* q, const void* data) {
    if (queue_is_full(q)) {
        return false;
    }
    uint tail = (q->head + q->count) % q->capacity;
    memcpy(&q->buf[(size_t)tail * q->element_size], data, q->element_size);
    q->count++;
    return true;
}

inline bool queue_try_remove(queue_t* q, void* data) {
    if (queue_is_empty(q)) {
        return false;
    }
    memcpy(data, &q->buf[(size_t)q->head * q->element_size], q->element_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return true;
}
