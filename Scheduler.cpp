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
    // Clear any existing data
    vms.clear();
    machines.clear();
    vm_to_machine.clear();
    vm_types.clear();
    vm_cpu_types.clear();
    migrating_vms.clear();
    machine_vms.clear();
    
    // Scan all machines and power them on initially
    unsigned total_machines = Machine_GetTotal();
    for(unsigned i = 0; i < total_machines; i++) {
        MachineId_t machine_id = MachineId_t(i);
        MachineInfo_t info = Machine_GetInfo(machine_id);
        
        // Power on machine if it's off
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
    // Check if VM is migrating
    if(migrating_vms.find(vm_id) != migrating_vms.end()) {
        return false;
    }
    
    // Get task requirements
    TaskInfo_t task_info = GetTaskInfo(task_id);
    VMInfo_t vm_info = VM_GetInfo(vm_id);
    
    // Check VM type match
    if(vm_info.vm_type != task_info.required_vm) {
        return false;
    }
    
    // Check CPU type match
    if(vm_info.cpu != task_info.required_cpu) {
        return false;
    }
    
    // Check if machine has enough memory
    MachineInfo_t machine_info = Machine_GetInfo(vm_info.machine_id);
    unsigned task_memory = GetTaskMemory(task_id);
    
    // Check if adding this task would exceed memory (with some headroom)
    if(machine_info.memory_used + task_memory > machine_info.memory_size * 0.9) {
        return false;
    }
    
    return true;
}

MachineId_t Scheduler::findSuitableMachine(CPUType_t cpu_type, bool needs_gpu, unsigned memory_needed) {
    MachineId_t best_machine = MachineId_t(0);
    bool found = false;
    unsigned best_utilization = 0;
    
    // First, try to find a machine that's already on (S0 or S0i1)
    for(unsigned i = 0; i < machines.size(); i++) {
        MachineId_t machine_id = machines[i];
        MachineInfo_t info = Machine_GetInfo(machine_id);
        
        // Check CPU type
        if(info.cpu != cpu_type) continue;
        
        // Check GPU requirement
        if(needs_gpu && !info.gpus) continue;
        
        // Check if machine is ready (S0 or S0i1)
        if(info.s_state != S0 && info.s_state != S0i1) continue;
        
        // Check memory availability
        if(info.memory_used + memory_needed > info.memory_size * 0.9) continue;
        
        // Prefer machines with more free memory and fewer active tasks
        unsigned free_memory = info.memory_size - info.memory_used;
        unsigned utilization_score = free_memory - (info.active_tasks * 100);
        
        if(!found || utilization_score > best_utilization) {
            best_machine = machine_id;
            best_utilization = utilization_score;
            found = true;
        }
    }
    
    // If no suitable machine found, try to wake one up
    if(!found) {
        for(unsigned i = 0; i < machines.size(); i++) {
            MachineId_t machine_id = machines[i];
            MachineInfo_t info = Machine_GetInfo(machine_id);
            
            // Check CPU type
            if(info.cpu != cpu_type) continue;
            
            // Check GPU requirement
            if(needs_gpu && !info.gpus) continue;
            
            // Check memory
            if(info.memory_used + memory_needed > info.memory_size * 0.9) continue;
            
            // Wake up machine if needed
            if(info.s_state != S0 && info.s_state != S0i1) {
                Machine_SetState(machine_id, S0);
            }
            
            best_machine = machine_id;
            found = true;
            break;
        }
    }
    
    if(!found) {
        // Return invalid machine ID (use a value that's out of range)
        return MachineId_t(Machine_GetTotal() + 1);
    }
    
    return best_machine;
}

VMId_t Scheduler::findOrCreateVM(VMType_t vm_type, CPUType_t cpu_type, bool needs_gpu) {
    // First, try to find an existing VM that matches
    for(auto vm_id : vms) {
        if(migrating_vms.find(vm_id) != migrating_vms.end()) {
            continue;
        }
        
        VMInfo_t vm_info = VM_GetInfo(vm_id);
        if(vm_info.vm_type == vm_type && vm_info.cpu == cpu_type) {
            // Check if the machine has GPU if needed
            if(needs_gpu) {
                MachineInfo_t machine_info = Machine_GetInfo(vm_info.machine_id);
                if(!machine_info.gpus) continue;
            }
            return vm_id;
        }
    }
    
    // No suitable VM found, create a new one
    // Find a suitable machine
    unsigned memory_needed = VM_MEMORY_OVERHEAD + 1024; // VM overhead + some buffer
    MachineId_t machine_id = findSuitableMachine(cpu_type, needs_gpu, memory_needed);
    
    if(machine_id >= MachineId_t(Machine_GetTotal())) {
        SimOutput("Scheduler::findOrCreateVM(): ERROR - No suitable machine found!", 0);
        throw runtime_error("No suitable machine found for VM creation");
    }
    
    // Create VM
    VMId_t new_vm = VM_Create(vm_type, cpu_type);
    vms.push_back(new_vm);
    vm_types[new_vm] = vm_type;
    vm_cpu_types[new_vm] = cpu_type;
    
    // Attach to machine
    VM_Attach(new_vm, machine_id);
    vm_to_machine[new_vm] = machine_id;
    machine_vms[machine_id].insert(new_vm);
    
    return new_vm;
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    // Get task requirements
    VMType_t required_vm = RequiredVMType(task_id);
    CPUType_t required_cpu = RequiredCPUType(task_id);
    SLAType_t required_sla = RequiredSLA(task_id);
    bool gpu_capable = IsTaskGPUCapable(task_id);
    unsigned task_memory = GetTaskMemory(task_id);
    
    // Determine priority based on SLA
    Priority_t priority = getPriorityForSLA(required_sla);
    
    // Try to find a suitable existing VM first
    VMId_t target_vm = VMId_t(0);
    bool vm_found = false;
    for(auto vm_id : vms) {
        if(canPlaceTaskOnVM(vm_id, task_id)) {
            target_vm = vm_id;
            vm_found = true;
            break;
        }
    }
    
    // If no suitable VM found, create a new one
    if(!vm_found) {
        target_vm = findOrCreateVM(required_vm, required_cpu, gpu_capable);
    }
    
    // Verify VM is ready before adding task
    VMInfo_t vm_info = VM_GetInfo(target_vm);
    if(vm_info.machine_id >= MachineId_t(Machine_GetTotal()) || migrating_vms.find(target_vm) != migrating_vms.end()) {
        // VM not ready, create a new one
        target_vm = findOrCreateVM(required_vm, required_cpu, gpu_capable);
    }
    
    // Add task to VM
    bool task_placed = false;
    int retry_count = 0;
    while(!task_placed && retry_count < 3) {
        try {
            VM_AddTask(target_vm, task_id, priority);
            task_placed = true;
        } catch(const exception& e) {
            retry_count++;
            
            // Try to create a new VM and place there
            MachineId_t machine_id = findSuitableMachine(required_cpu, gpu_capable, task_memory + VM_MEMORY_OVERHEAD);
            if(machine_id < MachineId_t(Machine_GetTotal())) {
                VMId_t new_vm = VM_Create(required_vm, required_cpu);
                vms.push_back(new_vm);
                vm_types[new_vm] = required_vm;
                vm_cpu_types[new_vm] = required_cpu;
                
                // Make sure machine is on
                MachineInfo_t machine_info = Machine_GetInfo(machine_id);
                if(machine_info.s_state != S0 && machine_info.s_state != S0i1) {
                    Machine_SetState(machine_id, S0);
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
    // Periodically try to consolidate
    static unsigned consolidation_counter = 0;
    consolidation_counter++;
    if(consolidation_counter % 10 == 0) {
        consolidateMachines();
    }
}

void Scheduler::consolidateMachines() {
    // Find machines with no active tasks
    for(auto machine_id : machines) {
        MachineInfo_t info = Machine_GetInfo(machine_id);
        
        // Skip if machine is already off or transitioning
        if(info.s_state == S5 || info.s_state == S3 || info.s_state == S4) {
            continue;
        }
        
        // If machine has no active tasks and no VMs, power it down
        if(info.active_tasks == 0 && info.active_vms == 0) {
            Machine_SetState(machine_id, S5);
        }
        // If machine has very low utilization, consider moving VMs
        else if(info.active_tasks == 0 && info.active_vms > 0) {
            // Try to migrate VMs to other machines
            set<VMId_t> vms_to_migrate = machine_vms[machine_id];
            for(auto vm_id : vms_to_migrate) {
                if(migrating_vms.find(vm_id) != migrating_vms.end()) {
                    continue;
                }
                
                VMInfo_t vm_info = VM_GetInfo(vm_id);
                if(vm_info.active_tasks.size() == 0) {
                    // VM has no tasks, just shut it down
                    VM_Shutdown(vm_id);
                    machine_vms[machine_id].erase(vm_id);
                    vm_to_machine.erase(vm_id);
                }
            }
            
            // If all VMs are gone, power down
            if(machine_vms[machine_id].size() == 0) {
                Machine_SetState(machine_id, S5);
            }
        }
    }
}

void Scheduler::handleSLAViolation(TaskId_t task_id) {
    TaskInfo_t task_info = GetTaskInfo(task_id);
    
    // Increase priority to highest
    if(task_info.priority != HIGH_PRIORITY) {
        SetTaskPriority(task_id, HIGH_PRIORITY);
    }
    
    // Try to find the VM hosting this task and potentially migrate it
    for(auto vm_id : vms) {
        if(migrating_vms.find(vm_id) != migrating_vms.end()) {
            continue;
        }
        
        VMInfo_t vm_info = VM_GetInfo(vm_id);
        for(auto active_task : vm_info.active_tasks) {
            if(active_task == task_id) {
                // Found the VM hosting this task
                // Try to find a better machine (one with more resources)
                MachineInfo_t current_machine = Machine_GetInfo(vm_info.machine_id);
                
                // Look for a machine with more free resources
                for(auto machine_id : machines) {
                    MachineInfo_t candidate = Machine_GetInfo(machine_id);
                    if(candidate.cpu == current_machine.cpu &&
                       candidate.s_state == S0 &&
                       candidate.memory_used < current_machine.memory_used &&
                       candidate.active_tasks < current_machine.active_tasks) {
                        // Migrate to better machine
                        try {
                            VM_Migrate(vm_id, machine_id);
                            migrating_vms.insert(vm_id);
                        } catch(...) {
                            // Migration failed, continue
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
    // Periodic consolidation
    static unsigned check_counter = 0;
    check_counter++;
    
    if(check_counter % 20 == 0) {
        consolidateMachines();
    }
}

void Scheduler::Shutdown(Time_t time) {
    // Shutdown all VMs
    for(auto vm_id : vms) {
        try {
            VM_Shutdown(vm_id);
        } catch(...) {
            // Ignore errors during shutdown
        }
    }
    
    // Power down all machines
    for(auto machine_id : machines) {
        try {
            Machine_SetState(machine_id, S5);
        } catch(...) {
            // Ignore errors during shutdown
        }
    }
}

// Public interface below

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
    // Try to migrate some VMs from this machine
    MachineInfo_t info = Machine_GetInfo(machine_id);
    if(info.active_vms > 0) {
        // Find a less loaded machine to migrate to
        for(unsigned i = 0; i < Machine_GetTotal(); i++) {
            MachineId_t target_machine = MachineId_t(i);
            if(target_machine == machine_id) continue;
            
            MachineInfo_t target_info = Machine_GetInfo(target_machine);
            if(target_info.s_state == S0 && 
               target_info.memory_used < target_info.memory_size * 0.7 &&
               target_info.cpu == info.cpu) {
                // Try to migrate a VM
                // This is simplified - in practice, we'd pick a specific VM
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
    // Machine state change complete
}
