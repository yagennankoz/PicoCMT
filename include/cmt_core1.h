#pragma once

void setup1();
void loop1();

void get_core1_playback_stats(bool* session_initialized, uint32_t* swap_count, uint16_t* last_t88_data);
void get_core1_playback_io_stats(bool* pio_enabled, uint8_t* gpio_level);
void log_core1_rec_stats();
void reset_core1_playback_buffers();
void init_core1_playback_prime();
void request_playback_shutdown();