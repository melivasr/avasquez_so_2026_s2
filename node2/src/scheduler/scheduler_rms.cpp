// CALENDARIZADOR RMS
// La prioridad depende solo de T, así que es fija toda la ejecución.

#include "scheduler/scheduler_rms.h"

bool SchedulerRms::has_priority(const TaskState &a, const TaskState &b) const{
    if (a.task.T != b.task.T) {
        return a.task.T < b.task.T;
    }
    if (a.task.criticality != b.task.criticality) {
        return a.task.criticality > b.task.criticality;
    }
    return a.task.id < b.task.id;
}
