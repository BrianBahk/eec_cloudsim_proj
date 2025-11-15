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

class Scheduler {
public:
    Scheduler()                 {}
    void Init();
    void MigrationComplete(Time_t time, VMId_t vm_id);
    void NewTask(Time_t now, TaskId_t task_id);
    void PeriodicCheck(Time_t now);
    void Shutdown(Time_t now);
    void TaskComplete(Time_t now, TaskId_t task_id);
private:
    vector<VMId_t> vms;
    vector<MachineId_t> machines;
    map<VMId_t, MachineId_t> vm_to_machine;  // VM -> Machine mapping
    map<VMId_t, VMType_t> vm_types;          // VM -> VM type mapping
    map<VMId_t, CPUType_t> vm_cpu_types;     // VM -> CPU type mapping
    set<VMId_t> migrating_vms;               // VMs currently migrating
    map<MachineId_t, set<VMId_t>> machine_vms; // Machine -> set of VMs
    
    VMId_t findOrCreateVM(VMType_t vm_type, CPUType_t cpu_type, bool needs_gpu);
    MachineId_t findSuitableMachine(CPUType_t cpu_type, bool needs_gpu, unsigned memory_needed);
    Priority_t getPriorityForSLA(SLAType_t sla);
    bool canPlaceTaskOnVM(VMId_t vm_id, TaskId_t task_id);
    void consolidateMachines();
public:
    void handleSLAViolation(TaskId_t task_id);
};



#endif