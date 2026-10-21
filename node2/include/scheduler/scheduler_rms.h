// Calendarizador RMS: prioridad fija, menor período primero.
#pragma once

#include "scheduler/scheduler.h"

class SchedulerRms : public Scheduler {
protected:
    // menor T primero; empate: mayor criticidad; empate: menor id
    bool has_priority(const TaskState &a, const TaskState &b) const override;
};
