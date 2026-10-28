#pragma once
#include <stdint.h>
struct repeating_timer_t { int unused; };
uint32_t time_us_32();
bool add_repeating_timer_us(int64_t period,bool (*callback)(repeating_timer_t *),
                           void *,repeating_timer_t *timer);
bool cancel_repeating_timer(repeating_timer_t *timer);
