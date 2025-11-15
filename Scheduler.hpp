#ifndef Scheduler_hpp
#define Scheduler_hpp

#include <vector>
#include <map>
#include <set>
#include <algorithm>

#include "Interfaces.h"

struct TaskDeadline {
    TaskId_t task_id;
    Time_t deadline;
    
    bool operator<(const TaskDeadline& other) const {
        return deadline > other.deadline;
    }
};

class Scheduler {
public:
    Scheduler() {}
    void Init();
    void MigrationComplete(Time_t time, VMId_t vm_id);
    void NewTask(Time_t now, TaskId_t task_id);
    void PeriodicCheck(Time_t now);
    void Shutdown(Time_t now);
    void TaskComplete(Time_t now, TaskId_t task_id);
    
private:
    vector<VMId_t> vms;
    vector<MachineId_t> machines;
    map<MachineId_t, VMId_t> machine_to_vm;
    map<VMId_t, MachineId_t> vm_to_machine;
    map<TaskId_t, VMId_t> task_to_vm;
    map<TaskId_t, Time_t> task_deadlines;
    vector<TaskId_t> pending_tasks;
    
    VMId_t FindOrCreateVM(CPUType_t cpu_type, VMType_t vm_type, bool gpu_capable, unsigned memory_needed);
    MachineId_t FindAvailableMachine(CPUType_t cpu_type, bool gpu_capable, unsigned memory_needed);
    Priority_t CalculatePriority(Time_t now, Time_t deadline);
    void ProcessPendingTasks(Time_t now);
    void SortPendingByDeadline();
};

#endif