//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//

#include "Scheduler.hpp"
#include <algorithm>
#include <iostream>

using namespace std;

void Scheduler::Init() {
    vms.clear();
    machines.clear();
    vm_to_machine.clear();
    vm_types.clear();
    vm_cpu_types.clear();
    migrating_vms.clear();
    machine_vms.clear();
    
    unsigned total_machines = Machine_GetTotal();
    for(unsigned i = 0; i < total_machines; i++) {
        MachineId_t machine_id = MachineId_t(i);
        MachineInfo_t info = Machine_GetInfo(machine_id);
        
        if(info.s_state == S5) {
            Machine_SetState(machine_id, S0);
        }
        
        machines.push_back(machine_id);
        machine_vms[machine_id] = set<VMId_t>();
    }
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    migrating_vms.erase(vm_id);
}

Priority_t Scheduler::getPriorityForSLA(SLAType_t sla) {
    switch(sla) {
        case SLA0: return HIGH_PRIORITY;
        case SLA1: return HIGH_PRIORITY;
        case SLA2: return MID_PRIORITY;
        case SLA3: return LOW_PRIORITY;
        default: return MID_PRIORITY;
    }
}

bool Scheduler::canPlaceTaskOnVM(VMId_t vm_id, TaskId_t task_id) {
    if(migrating_vms.find(vm_id) != migrating_vms.end()) {
        return false;
    }
    
    TaskInfo_t task_info = GetTaskInfo(task_id);
    VMInfo_t vm_info = VM_GetInfo(vm_id);
    
    if(vm_info.vm_type != task_info.required_vm) {
        return false;
    }
    
    if(vm_info.cpu != task_info.required_cpu) {
        return false;
    }
    
    MachineInfo_t machine_info = Machine_GetInfo(vm_info.machine_id);
    unsigned task_memory = GetTaskMemory(task_id);
    
    if(machine_info.memory_used + task_memory > machine_info.memory_size * 0.9) {
        return false;
    }
    
    return true;
}

MachineId_t Scheduler::findSuitableMachine(CPUType_t cpu_type, bool needs_gpu, unsigned memory_needed) {
    MachineId_t best_machine = MachineId_t(0);
    bool found = false;
    unsigned best_utilization = 0;
    
    for(unsigned i = 0; i < machines.size(); i++) {
        MachineId_t machine_id = machines[i];
        MachineInfo_t info = Machine_GetInfo(machine_id);
        
        if(info.cpu != cpu_type) continue;
        
        if(needs_gpu && !info.gpus) continue;
        
        if(info.s_state != S0 && info.s_state != S0i1) continue;
        
        if(info.memory_used + memory_needed > info.memory_size * 0.9) continue;
        
        unsigned free_memory = info.memory_size - info.memory_used;
        unsigned utilization_score = free_memory - (info.active_tasks * 100);
        
        if(!found || utilization_score > best_utilization) {
            best_machine = machine_id;
            best_utilization = utilization_score;
            found = true;
        }
    }
    
    if(!found) {
        for(unsigned i = 0; i < machines.size(); i++) {
            MachineId_t machine_id = machines[i];
            MachineInfo_t info = Machine_GetInfo(machine_id);
            
            if(info.cpu != cpu_type) continue;
            
            if(needs_gpu && !info.gpus) continue;
            
            if(info.memory_used + memory_needed > info.memory_size * 0.9) continue;
            
            if(info.s_state != S0 && info.s_state != S0i1) {
                Machine_SetState(machine_id, S0);
                info = Machine_GetInfo(machine_id);
                if(info.s_state != S0 && info.s_state != S0i1) {
                    continue;
                }
            }
            
            best_machine = machine_id;
            found = true;
            break;
        }
    }
    
    if(!found) {
        return MachineId_t(Machine_GetTotal() + 1);
    }
    
    return best_machine;
}

VMId_t Scheduler::findOrCreateVM(VMType_t vm_type, CPUType_t cpu_type, bool needs_gpu) {
    for(auto vm_id : vms) {
        if(migrating_vms.find(vm_id) != migrating_vms.end()) {
            continue;
        }
        
        VMInfo_t vm_info = VM_GetInfo(vm_id);
        if(vm_info.vm_type == vm_type && vm_info.cpu == cpu_type) {
            if(needs_gpu) {
                MachineInfo_t machine_info = Machine_GetInfo(vm_info.machine_id);
                if(!machine_info.gpus) continue;
            }
            return vm_id;
        }
    }
    
    unsigned memory_needed = VM_MEMORY_OVERHEAD + 1024;
    MachineId_t machine_id = findSuitableMachine(cpu_type, needs_gpu, memory_needed);
    
    if(machine_id >= MachineId_t(Machine_GetTotal())) {
        SimOutput("Scheduler::findOrCreateVM(): ERROR - No suitable machine found!", 0);
        throw runtime_error("No suitable machine found for VM creation");
    }
    
    VMId_t new_vm = VM_Create(vm_type, cpu_type);
    vms.push_back(new_vm);
    vm_types[new_vm] = vm_type;
    vm_cpu_types[new_vm] = cpu_type;
    
    MachineInfo_t machine_info = Machine_GetInfo(machine_id);
    if(machine_info.s_state != S0 && machine_info.s_state != S0i1) {
        Machine_SetState(machine_id, S0);
        machine_info = Machine_GetInfo(machine_id);
        if(machine_info.s_state != S0 && machine_info.s_state != S0i1) {
            SimOutput("Scheduler::findOrCreateVM(): ERROR - Machine not ready for VM attachment!", 0);
            throw runtime_error("Machine not in ready state for VM attachment");
        }
    }
    
    VM_Attach(new_vm, machine_id);
    vm_to_machine[new_vm] = machine_id;
    machine_vms[machine_id].insert(new_vm);
    
    return new_vm;
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    VMType_t required_vm = RequiredVMType(task_id);
    CPUType_t required_cpu = RequiredCPUType(task_id);
    SLAType_t required_sla = RequiredSLA(task_id);
    bool gpu_capable = IsTaskGPUCapable(task_id);
    unsigned task_memory = GetTaskMemory(task_id);
    
    Priority_t priority = getPriorityForSLA(required_sla);
    
    VMId_t target_vm = VMId_t(0);
    bool vm_found = false;
    for(auto vm_id : vms) {
        if(canPlaceTaskOnVM(vm_id, task_id)) {
            target_vm = vm_id;
            vm_found = true;
            break;
        }
    }
    
    if(!vm_found) {
        target_vm = findOrCreateVM(required_vm, required_cpu, gpu_capable);
    }
    
    VMInfo_t vm_info = VM_GetInfo(target_vm);
    if(vm_info.machine_id >= MachineId_t(Machine_GetTotal()) || migrating_vms.find(target_vm) != migrating_vms.end()) {
        target_vm = findOrCreateVM(required_vm, required_cpu, gpu_capable);
    }
    
    bool task_placed = false;
    int retry_count = 0;
    while(!task_placed && retry_count < 3) {
        try {
            VM_AddTask(target_vm, task_id, priority);
            task_placed = true;
        } catch(const exception& e) {
            retry_count++;
            
            MachineId_t machine_id = findSuitableMachine(required_cpu, gpu_capable, task_memory + VM_MEMORY_OVERHEAD);
            if(machine_id < MachineId_t(Machine_GetTotal())) {
                VMId_t new_vm = VM_Create(required_vm, required_cpu);
                vms.push_back(new_vm);
                vm_types[new_vm] = required_vm;
                vm_cpu_types[new_vm] = required_cpu;
                
                MachineInfo_t machine_info = Machine_GetInfo(machine_id);
                if(machine_info.s_state != S0 && machine_info.s_state != S0i1) {
                    Machine_SetState(machine_id, S0);
                    machine_info = Machine_GetInfo(machine_id);
                    if(machine_info.s_state != S0 && machine_info.s_state != S0i1) {
                        continue;
                    }
                }
                
                VM_Attach(new_vm, machine_id);
                vm_to_machine[new_vm] = machine_id;
                machine_vms[machine_id].insert(new_vm);
                target_vm = new_vm;
            } else {
                SimOutput("Scheduler::NewTask(): CRITICAL - No suitable machine found for task " + to_string(task_id), 0);
                break;
            }
        }
    }
    
    if(!task_placed) {
        SimOutput("Scheduler::NewTask(): FAILED to place task " + to_string(task_id) + " after retries", 0);
    }
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    static unsigned consolidation_counter = 0;
    consolidation_counter++;
    if(consolidation_counter % 10 == 0) {
        consolidateMachines();
    }
}

void Scheduler::consolidateMachines() {
    for(auto machine_id : machines) {
        MachineInfo_t info = Machine_GetInfo(machine_id);
        
        if(info.s_state == S5 || info.s_state == S3 || info.s_state == S4) {
            continue;
        }
        
        if(info.active_tasks == 0 && info.active_vms == 0) {
            Machine_SetState(machine_id, S5);
        }
        else if(info.active_tasks == 0 && info.active_vms > 0) {
            set<VMId_t> vms_to_migrate = machine_vms[machine_id];
            for(auto vm_id : vms_to_migrate) {
                if(migrating_vms.find(vm_id) != migrating_vms.end()) {
                    continue;
                }
                
                VMInfo_t vm_info = VM_GetInfo(vm_id);
                if(vm_info.active_tasks.size() == 0) {
                    VM_Shutdown(vm_id);
                    machine_vms[machine_id].erase(vm_id);
                    vm_to_machine.erase(vm_id);
                }
            }
            
            if(machine_vms[machine_id].size() == 0) {
                Machine_SetState(machine_id, S5);
            }
        }
    }
}

void Scheduler::handleSLAViolation(TaskId_t task_id) {
    TaskInfo_t task_info = GetTaskInfo(task_id);
    
    if(task_info.priority != HIGH_PRIORITY) {
        SetTaskPriority(task_id, HIGH_PRIORITY);
    }
    
    for(auto vm_id : vms) {
        if(migrating_vms.find(vm_id) != migrating_vms.end()) {
            continue;
        }
        
        VMInfo_t vm_info = VM_GetInfo(vm_id);
        for(auto active_task : vm_info.active_tasks) {
            if(active_task == task_id) {
                MachineInfo_t current_machine = Machine_GetInfo(vm_info.machine_id);
                
                for(auto machine_id : machines) {
                    MachineInfo_t candidate = Machine_GetInfo(machine_id);
                    if(candidate.cpu == current_machine.cpu &&
                       candidate.s_state == S0 &&
                       candidate.memory_used < current_machine.memory_used &&
                       candidate.active_tasks < current_machine.active_tasks) {
                        try {
                            VM_Migrate(vm_id, machine_id);
                            migrating_vms.insert(vm_id);
                        } catch(...) {
                        }
                        return;
                    }
                }
                break;
            }
        }
    }
}

void Scheduler::PeriodicCheck(Time_t now) {
    static unsigned check_counter = 0;
    check_counter++;
    
    if(check_counter % 20 == 0) {
        consolidateMachines();
    }
}

void Scheduler::Shutdown(Time_t time) {
    for(auto vm_id : vms) {
        try {
            VM_Shutdown(vm_id);
        } catch(...) {
        }
    }
    
    for(auto machine_id : machines) {
        try {
            Machine_SetState(machine_id, S5);
        } catch(...) {
        }
    }
}

static Scheduler Scheduler;

void InitScheduler() {
    Scheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    Scheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    Scheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    MachineInfo_t info = Machine_GetInfo(machine_id);
    if(info.active_vms > 0) {
        for(unsigned i = 0; i < Machine_GetTotal(); i++) {
            MachineId_t target_machine = MachineId_t(i);
            if(target_machine == machine_id) continue;
            
            MachineInfo_t target_info = Machine_GetInfo(target_machine);
            if(target_info.s_state == S0 && 
               target_info.memory_used < target_info.memory_size * 0.7 &&
               target_info.cpu == info.cpu) {
                break;
            }
        }
    }
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    Scheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    Scheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    cout << "SLA violation report" << endl;
    cout << "SLA0: " << GetSLAReport(SLA0) << "%" << endl;
    cout << "SLA1: " << GetSLAReport(SLA1) << "%" << endl;
    cout << "SLA2: " << GetSLAReport(SLA2) << "%" << endl;
    cout << "Total Energy " << Machine_GetClusterEnergy() << "KW-Hour" << endl;
    cout << "Simulation run finished in " << double(time)/1000000 << " seconds" << endl;
    
    Scheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    Scheduler.handleSLAViolation(task_id);
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
}
