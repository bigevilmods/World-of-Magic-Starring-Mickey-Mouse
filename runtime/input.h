#pragma once
#include <windows.h>
#include <stdint.h>

void input_init();
void input_shutdown();
void input_set_key(WPARAM vk, int down);
void input_reset_keys();
void input_poll();
