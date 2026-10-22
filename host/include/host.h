#ifndef HOST_H
#define HOST_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <semaphore.h>
#include <sys/types.h>
#include "../../common/rt_protocol.h"
#define NODES 2
#define NODE_CAP RT_MAX_TASKS
#define TASK_CAP 512
#define PAYLOAD RT_MAX_PAYLOAD
#define FRAME_SIZE 519
#define QUEUE_CAP 128
#define NO_TASK UINT32_MAX

typedef enum { EDF, RMS } Algorithm;
typedef enum { FIRST_FIT, WORST_FIT } Heuristic;
typedef struct { uint32_t id, W, T, D; int32_t criticality; char type; } Task;
typedef struct {
    Algorithm algorithm; Heuristic heuristic;
    uint32_t tick_us, report_ticks, missed_reports, timeout_ms;
    char ports[NODES][256], tasks[512], output[512];
} Config;
typedef struct { bool operational; uint32_t speed; size_t count; Task tasks[NODE_CAP]; } Node;
int bounded_line(FILE *, char *, size_t);
bool parse_u32(const char *, uint32_t *);
bool task_parse(const char *, Task *, char *, size_t);
bool task_load(const char *, Task *, size_t *, char *, size_t);
bool config_load(const char *, Config *, char *, size_t);
uint32_t task_cost(uint32_t, uint32_t);
long double node_util(const Node *);
const char *admission(const Node *, const Task *, Algorithm);
int place(Node [NODES], const Task *, Algorithm, Heuristic, char *, size_t);
void sort_tasks(Task *, size_t, uint32_t);
size_t migrate_plan(Node [NODES], int, Algorithm, Heuristic, Task *, bool *);

enum { CAL_REQ=RT_CAL_REQ, CAL_RESP=RT_CAL_RESP, DEPLOY=RT_DEPLOY,
       START=RT_START, EVENT=RT_EVENT, TICK=RT_TICK_REPORT,
       SPORADIC=RT_SPORADIC, STOP=RT_STOP, ACK=RT_ACK, ADD_TASK=RT_ADD_TASK };
typedef struct { uint8_t type; uint16_t length; uint8_t data[PAYLOAD]; } Frame;
typedef struct { uint8_t bytes[FRAME_SIZE]; size_t used; uint64_t errors; } Parser;
uint16_t crc16(const uint8_t *, size_t);
uint32_t get32(const uint8_t *);
void put32(uint8_t *, uint32_t);
size_t frame_encode(const Frame *, uint8_t *);
bool parser_byte(Parser *, uint8_t, Frame *);
void parser_expire(Parser *);
uint32_t command_timeout_ms(const Config *, uint8_t);
Frame start_frame(const Config *);

enum { R_COMMAND=1, R_REPLY, R_NODE, R_TICK, R_NOTE, R_TASK, R_END, R_EXIT, R_STOP };
enum { REPLY_OK=0, REPLY_REJECT=1, REPLY_UNCERTAIN=2, REPLY_IO=3 };
typedef struct {
    int kind, node, code; uint32_t tick, id, value;
    uint64_t ns; Task task; Frame frame; char text[192];
} Record;
typedef struct { sem_t spaces, items, mutex; unsigned head, tail; Record data[QUEUE_CAP]; } Queue;
typedef struct {
    sem_t lock;
    uint64_t last_tick_ns[NODES], dropped[NODES+1];
    uint32_t last_tick[NODES]; bool io_failed[NODES];
    Queue commands[NODES], replies[NODES], events;
} Shared;
uint64_t monotonic_ns(void);
int sem_wait_ms(sem_t *, int);
int queue_init(Queue *);
void queue_destroy(Queue *);
int queue_push(Queue *, const Record *, int);
int queue_pop(Queue *, Record *, int);
Shared *ipc_create(char *, size_t, int *);
void ipc_destroy(Shared *, int, const char *);
void emit(Shared *, Record *);
void note(Shared *, int, const char *, ...);
bool receive_telemetry(Shared *, int, const Frame *);
int serial_open(const char *);
int serial_write(int, const uint8_t *, size_t, uint32_t);
void driver_run(Shared *, int, int, const Config *);
int report_run(Shared *, const Config *);
#endif
