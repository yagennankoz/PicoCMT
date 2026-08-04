#pragma once

void setup1();
void loop1();

void get_core1_playback_stats(bool* session_initialized, uint32_t* swap_count);
void get_core1_playback_stats(bool* session_initialized, uint32_t* swap_count, uint16_t* last_t88_data, bool* using_buffer_a);
void get_core1_playback_io_stats(bool* pio_enabled, bool* dma_busy, uint8_t* gpio_level);
void reset_core1_playback_buffers();
void init_core1_playback_prime();
void request_playback_shutdown();