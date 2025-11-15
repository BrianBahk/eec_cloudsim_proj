//
//  Scheduler.hpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#ifndef Scheduler_hpp
#define Scheduler_hpp

#include <vector>
#include <map>
#include <set>

#include "Interfaces.h"

struct MachineTracker {
    MachineId_t machine_id;
    CPUType_t cpu_type;
    unsigned memory_total;
    unsigned memory_used;
    unsigned num_cores;
    bool has_gpu;
    MachineState_t state;
    unsigned active_vms;
    unsigned active_tasks;
    unsigned idle_ticks;
    Time_t last_activity;
    vector<VMId_t> attached_vms;
};

struct VMTracker {
    VMId_t vm_id;
    VMType_t vm_type;
    CPUType_t cpu_type;
    MachineId_t machine_id;
    unsigned task_count;
    bool is_migrating;
    vector<TaskId_t> tasks;
    bool dedicated_to_long_tasks;
    Time_t total_task_time;
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
    void StateChanged(Time_t time, MachineId_t machine_id);
    
private:
    map<MachineId_t, MachineTracker> machines;
    vector<MachineId_t> machines_by_type[4];
    
    map<VMId_t, VMTracker> vms;
    map<CPUType_t, map<VMType_t, vector<VMId_t>>> vm_pools;
    
    unsigned total_machines;
    Time_t last_periodic_check;
    map<TaskId_t, VMId_t> task_to_vm;
    set<MachineId_t> waking_machines;
    vector<TaskId_t> pending_tasks;
    
    void DiscoverMachines();
    VMId_t FindOrCreateVM(CPUType_t cpu_type, VMType_t vm_type, unsigned memory_needed, bool gpu_capable, Time_t expected_runtime, SLAType_t sla_type);
    MachineId_t FindBestMachine(CPUType_t cpu_type, unsigned memory_needed, bool gpu_capable, bool allow_wake);
    void PowerDownIdleMachines(Time_t now);
    void UpdateMachineState(MachineId_t machine_id);
    Priority_t DeterminePriority(TaskId_t task_id);
    void LogState(const string& context, Time_t now);
    void ProcessPendingTasks(Time_t now);
};

#endif