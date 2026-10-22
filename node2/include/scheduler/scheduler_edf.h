// Calendarizador EDF: prioridad dinámica, plazo absoluto más cercano primero.
#pragma once

#include "scheduler/scheduler.h"

class SchedulerEdf : public Scheduler {
protected:
    // menor plazo absoluto primero; empate: mayor criticidad; empate: menor id
    bool has_priority(const TaskState &a, const TaskState &b) const override;
};
