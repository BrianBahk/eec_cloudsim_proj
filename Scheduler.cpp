#include "Scheduler.hpp"
#include <algorithm>
#include <iostream>

void Scheduler::Init() {
    SimOutput("Scheduler::Init(): EDF Scheduler initialization", 1);
    
    unsigned total_machines = Machine_GetTotal();
    
    for(unsigned i = 0; i < total_machines; i++) {
        MachineInfo_t info = Machine_GetInfo(i);
        if(info.s_state == S5) {
            Machine_SetState(i, S0);
        }
        machines.push_back(i);
    }
    
    SimOutput("Scheduler::Init(): Complete - " + to_string(total_machines) + " machines initialized", 1);
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
}

Priority_t Scheduler::CalculatePriority(Time_t now, Time_t deadline) {
    Time_t time_until_deadline = deadline - now;
    
    if(time_until_deadline < 1000000) {
        return HIGH_PRIORITY;
    } else if(time_until_deadline < 5000000) {
        return MID_PRIORITY;
    } else {
        return LOW_PRIORITY;
    }
}

MachineId_t Scheduler::FindAvailableMachine(CPUType_t cpu_type, bool gpu_capable, unsigned memory_needed) {
    for(MachineId_t mid : machines) {
        MachineInfo_t info = Machine_GetInfo(mid);
        
        if(info.cpu != cpu_type) continue;
        if(info.s_state != S0) continue;
        if(gpu_capable && !info.gpus) continue;
        if(info.memory_used + memory_needed + VM_MEMORY_OVERHEAD > info.memory_size) continue;
        
        return mid;
    }
    
    return static_cast<MachineId_t>(-1);
}

VMId_t Scheduler::FindOrCreateVM(CPUType_t cpu_type, VMType_t vm_type, bool gpu_capable, unsigned memory_needed) {
    for(VMId_t vm_id : vms) {
        VMInfo_t vm_info = VM_GetInfo(vm_id);
        
        if(vm_info.vm_type != vm_type) continue;
        if(vm_info.cpu != cpu_type) continue;
        
        MachineInfo_t machine_info = Machine_GetInfo(vm_info.machine_id);
        if(machine_info.s_state != S0) continue;
        if(gpu_capable && !machine_info.gpus) continue;
        if(machine_info.memory_used + memory_needed > machine_info.memory_size) continue;
        
        return vm_id;
    }
    
    MachineId_t machine_id = FindAvailableMachine(cpu_type, gpu_capable, memory_needed);
    if(machine_id == static_cast<MachineId_t>(-1)) {
        return static_cast<VMId_t>(-1);
    }
    
    VMId_t new_vm = VM_Create(vm_type, cpu_type);
    VM_Attach(new_vm, machine_id);
    
    vms.push_back(new_vm);
    machine_to_vm[machine_id] = new_vm;
    vm_to_machine[new_vm] = machine_id;
    
    return new_vm;
}

void Scheduler::SortPendingByDeadline() {
    sort(pending_tasks.begin(), pending_tasks.end(), [this](TaskId_t a, TaskId_t b) {
        Time_t deadline_a = task_deadlines.count(a) > 0 ? task_deadlines[a] : 0;
        Time_t deadline_b = task_deadlines.count(b) > 0 ? task_deadlines[b] : 0;
        return deadline_a < deadline_b;
    });
}

void Scheduler::ProcessPendingTasks(Time_t now) {
    SortPendingByDeadline();
    
    vector<TaskId_t> still_pending;
    
    for(TaskId_t task_id : pending_tasks) {
        if(task_to_vm.count(task_id) > 0) continue;
        
        TaskInfo_t task_info = GetTaskInfo(task_id);
        CPUType_t cpu_type = RequiredCPUType(task_id);
        VMType_t vm_type = RequiredVMType(task_id);
        bool gpu_capable = IsTaskGPUCapable(task_id);
        unsigned memory_needed = GetTaskMemory(task_id);
        
        VMId_t vm_id = FindOrCreateVM(cpu_type, vm_type, gpu_capable, memory_needed);
        
        if(vm_id == static_cast<VMId_t>(-1)) {
            still_pending.push_back(task_id);
            continue;
        }
        
        Time_t deadline = task_deadlines[task_id];
        Priority_t priority = CalculatePriority(now, deadline);
        
        VM_AddTask(vm_id, task_id, priority);
        task_to_vm[task_id] = vm_id;
    }
    
    pending_tasks = still_pending;
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    TaskInfo_t task_info = GetTaskInfo(task_id);
    Time_t deadline = task_info.target_completion;
    
    task_deadlines[task_id] = deadline;
    
    CPUType_t cpu_type = RequiredCPUType(task_id);
    VMType_t vm_type = RequiredVMType(task_id);
    bool gpu_capable = IsTaskGPUCapable(task_id);
    unsigned memory_needed = GetTaskMemory(task_id);
    
    VMId_t vm_id = FindOrCreateVM(cpu_type, vm_type, gpu_capable, memory_needed);
    
    if(vm_id == static_cast<VMId_t>(-1)) {
        pending_tasks.push_back(task_id);
        return;
    }
    
    Priority_t priority = CalculatePriority(now, deadline);
    VM_AddTask(vm_id, task_id, priority);
    task_to_vm[task_id] = vm_id;
}

void Scheduler::PeriodicCheck(Time_t now) {
    ProcessPendingTasks(now);
    
    for(auto& pair : task_to_vm) {
        TaskId_t task_id = pair.first;
        if(task_deadlines.count(task_id) > 0) {
            Time_t deadline = task_deadlines[task_id];
            Priority_t new_priority = CalculatePriority(now, deadline);
            SetTaskPriority(task_id, new_priority);
        }
    }
}

void Scheduler::Shutdown(Time_t time) {
    for(auto& vm : vms) {
        VM_Shutdown(vm);
    }
    
    for(unsigned i = 0; i < machines.size(); i++) {
        Machine_SetState(machines[i], S5);
    }
    
    SimOutput("Scheduler::Shutdown(): Complete", 1);
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    if(task_to_vm.count(task_id) > 0) {
        task_to_vm.erase(task_id);
    }
    
    if(task_deadlines.count(task_id) > 0) {
        task_deadlines.erase(task_id);
    }
    
    ProcessPendingTasks(now);
}

static Scheduler TheScheduler;

void InitScheduler() {
    TheScheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    TheScheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    TheScheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    SimOutput("MemoryWarning(): Machine " + to_string(machine_id), 0);
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    TheScheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    TheScheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    std::cout << "=== EDF Scheduler Results ===" << std::endl;
    std::cout << "SLA Violation Report:" << std::endl;
    std::cout << "  SLA0 (95% compliance): " << GetSLAReport(SLA0) << "%" << std::endl;
    std::cout << "  SLA1 (90% compliance): " << GetSLAReport(SLA1) << "%" << std::endl;
    std::cout << "  SLA2 (80% compliance): " << GetSLAReport(SLA2) << "%" << std::endl;
    std::cout << "Total Energy Consumed: " << Machine_GetClusterEnergy() << " KW-Hour" << std::endl;
    std::cout << "Simulation Duration: " << double(time)/1000000.0 << " seconds" << std::endl;
    std::cout << "====================================" << std::endl;
    
    TheScheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    SetTaskPriority(task_id, HIGH_PRIORITY);
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
}