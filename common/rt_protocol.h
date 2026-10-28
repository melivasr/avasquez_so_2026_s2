#ifndef RT_PROTOCOL_H
#define RT_PROTOCOL_H

/* Contrato serial compartido por C/Linux, ESP-IDF y Pico SDK.
 * Todos los enteros multibyte se codifican little-endian, sin padding. */
#define RT_MAGIC_0 0xA5U
#define RT_MAGIC_1 0x5AU
#define RT_MAX_PAYLOAD 512U
#define RT_MAX_TASK_ID 65535U
#define RT_MAX_TASKS 8U
#define RT_START_SIZE 3U
#define RT_EVENT_RECORD_SIZE 9U
#define RT_EVENT_MAX_BATCH 56U
#define RT_TICK_REPORT_SIZE 22U
#define RT_CAL_WINDOW_TICKS 50U

enum rt_message_type {
    RT_CAL_REQ = 0x01, RT_CAL_RESP, RT_DEPLOY, RT_START, RT_EVENT,
    RT_TICK_REPORT, RT_SPORADIC, RT_STOP, RT_ACK, RT_ADD_TASK
};
enum rt_algorithm { RT_RMS = 0, RT_EDF = 1 };
enum rt_status { RT_OK = 0, RT_INVALID = 1, RT_UNSUPPORTED = 2, RT_BAD_STATE = 3 };
enum rt_event_type {
    RT_RELEASE = 1, RT_SWITCH, RT_COMPLETE, RT_MISS,
    RT_SPORADIC_IGNORED, RT_TASK_ADDED, RT_TICK_LOST, RT_BAD_FRAME
};

#endif
