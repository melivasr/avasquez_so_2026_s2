// CALENDARIZADOR EDF
// La prioridad depende del trabajo actual, así que
// cambia con cada liberación.

#include "scheduler/scheduler_edf.h"

bool SchedulerEdf::has_priority(const TaskState &a, const TaskState &b) const{
    if (a.deadline != b.deadline) {
        return a.deadline < b.deadline;
    }
    if (a.task.criticality != b.task.criticality) {
        return a.task.criticality > b.task.criticality;
    }
    return a.task.id < b.task.id;
}
