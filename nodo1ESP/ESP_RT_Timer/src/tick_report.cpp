#include "lib/tick_report.h"

static uint16_t clamp16(uint32_t value)
{
    return static_cast<uint16_t>(value > 0xFFFFU ? 0xFFFFU : value);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFU);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFU);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFFU);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFU);
}

size_t tick_report_encode(const TickReportData &data, uint8_t *out, size_t out_capacity)
{
    if (out == nullptr || out_capacity < TICK_REPORT_SIZE) {
        return 0;
    }

    put32(out + 0, data.tick);
    put16(out + 4, clamp16(data.deadline_misses));
    put16(out + 6, clamp16(data.sporadic_ignored));
    put16(out + 8, clamp16(data.ticks_lost));
    put16(out + 10, clamp16(data.events_dropped));
    put16(out + 12, clamp16(data.bad_frames));
    put32(out + 14, data.max_busy_us);
    put32(out + 18, data.max_wake_latency_us);
    return TICK_REPORT_SIZE;
}

bool tick_report_due(uint32_t period, uint32_t tick, uint32_t *next_report)
{
    if (period == 0 || next_report == nullptr || tick < *next_report) {
        return false;
    }

    *next_report = (tick / period + 1) * period;
    return true;
}