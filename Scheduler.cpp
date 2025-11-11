//
//  Scheduler.cpp
//  CloudSim
//
//  pMapper: Power-Aware VM Placement Algorithm
//  Implements power-efficient VM placement based on incremental power cost
//

#include "Scheduler.hpp"
#include <algorithm>
#include <iostream>
#include <cmath>

using namespace std;

void Scheduler::Init() {
    SimOutput("pMapper::Init(): Initializing power-aware scheduler", 2);
    
    // Clear any existing data
    vms.clear();
    machines.clear();
    vm_to_machine.clear();
    vm_types.clear();
    vm_cpu_types.clear();
    migrating_vms.clear();
    machine_vms.clear();
    
    // Scan all machines - don't power them all on initially (power-aware)
    unsigned total_machines = Machine_GetTotal();
    for(unsigned i = 0; i < total_machines; i++) {
        MachineId_t machine_id = MachineId_t(i);
        MachineInfo_t info = Machine_GetInfo(machine_id);
        
        // Keep machines off initially - wake them only when needed
        machines.push_back(machine_id);
        machine_vms[machine_id] = set<VMId_t>();
    }
    
    SimOutput("pMapper::Init(): Initialized " + to_string(total_machines) + " machines", 2);
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    SimOutput("pMapper::MigrationComplete(): VM " + to_string(vm_id) + " migration complete", 3);
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

// Calculate power efficiency: MIPS per Watt (higher is better)
double Scheduler::calculatePowerEfficiency(MachineInfo_t& info) {
    if(info.performance.size() == 0 || info.p_states.size() == 0) {
        return 0.0;
    }
    
    // Use P0 state for maximum performance
    unsigned max_mips = info.performance[0];  // P0 MIPS
    unsigned base_power = info.s_states[0];   // S0 base power
    unsigned cpu_power = info.p_states[0] * info.num_cpus;  // CPU power at P0
    
    unsigned total_power = base_power + cpu_power;
    if(total_power == 0) return 0.0;
    
    // MIPS per Watt
    return double(max_mips * info.num_cpus) / double(total_power);
}

// Calculate incremental power cost of adding a task to this machine
double Scheduler::calculateIncrementalPowerCost(MachineInfo_t& info, unsigned memory_needed) {
    // If machine is off, cost includes wake-up + base power
    if(info.s_state == S5) {
        return info.s_states[0] + (info.p_states[0] * info.num_cpus);
    }
    
    // If machine is idle (S0i1 or S0 with no tasks), cost is just CPU power
    if(info.active_tasks == 0) {
        return info.p_states[0];  // One CPU at P0
    }
    
    // If machine is active, incremental cost depends on utilization
    // If we can fit on existing CPUs, cost is minimal (just scheduling overhead)
    // If we need more CPUs, cost is additional CPU power
    unsigned free_cpus = info.num_cpus - (info.active_tasks / 2);  // Rough estimate
    if(free_cpus > 0) {
        return 0.1 * info.p_states[0];  // Small incremental cost
    } else {
        return info.p_states[0];  // Need another CPU
    }
}

// pMapper: Find machine with best power efficiency
MachineId_t Scheduler::findPowerEfficientMachine(CPUType_t cpu_type, bool needs_gpu, unsigned memory_needed) {
    MachineId_t best_machine = MachineId_t(0);
    bool found = false;
    double best_score = -1.0;
    
    // First, try to find an active machine (S0 or S0i1) with good power efficiency
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
        
        // Calculate power efficiency score
        double efficiency = calculatePowerEfficiency(info);
        double incremental_cost = calculateIncrementalPowerCost(info, memory_needed);
        
        // Score: efficiency / incremental_cost (higher is better)
        // Prefer machines with high efficiency and low incremental cost
        double score = (incremental_cost > 0) ? efficiency / incremental_cost : efficiency * 1000;
        
        // Also consider utilization - prefer machines with some load but not overloaded
        double utilization = double(info.active_tasks) / double(info.num_cpus * 2);
        if(utilization > 0.3 && utilization < 0.8) {
            score *= 1.2;  // Bonus for good utilization
        }
        
        if(!found || score > best_score) {
            best_machine = machine_id;
            best_score = score;
            found = true;
        }
    }
    
    // If no suitable active machine found, find best machine to wake up
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
            
            // Calculate power efficiency
            double efficiency = calculatePowerEfficiency(info);
            double wake_cost = info.s_states[0] + (info.p_states[0] * info.num_cpus);
            double score = (wake_cost > 0) ? efficiency / wake_cost : efficiency * 1000;
            
            if(!found || score > best_score) {
                best_machine = machine_id;
                best_score = score;
                found = true;
            }
        }
    }
    
    if(!found) {
        // Return invalid machine ID
        return MachineId_t(Machine_GetTotal() + 1);
    }
    
    // Wake up machine if needed
    MachineInfo_t info = Machine_GetInfo(best_machine);
    if(info.s_state != S0 && info.s_state != S0i1) {
        SimOutput("pMapper::findPowerEfficientMachine(): Waking machine " + to_string(best_machine) + 
                  " (efficiency score: " + to_string(best_score) + ")", 2);
        Machine_SetState(best_machine, S0);
    }
    
    return best_machine;
}

MachineId_t Scheduler::findSuitableMachine(CPUType_t cpu_type, bool needs_gpu, unsigned memory_needed) {
    // Use power-efficient machine selection
    return findPowerEfficientMachine(cpu_type, needs_gpu, memory_needed);
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
    
    // No suitable VM found, create a new one using power-efficient placement
    unsigned memory_needed = VM_MEMORY_OVERHEAD + 1024;
    MachineId_t machine_id = findPowerEfficientMachine(cpu_type, needs_gpu, memory_needed);
    
    if(machine_id >= MachineId_t(Machine_GetTotal())) {
        SimOutput("pMapper::findOrCreateVM(): ERROR - No suitable machine found!", 0);
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
    
    SimOutput("pMapper::findOrCreateVM(): Created VM " + to_string(new_vm) + 
              " on power-efficient machine " + to_string(machine_id), 2);
    
    return new_vm;
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    SimOutput("pMapper::NewTask(): Handling task " + to_string(task_id) + " at time " + to_string(now), 3);
    
    // Get task requirements
    VMType_t required_vm = RequiredVMType(task_id);
    CPUType_t required_cpu = RequiredCPUType(task_id);
    SLAType_t required_sla = RequiredSLA(task_id);
    bool gpu_capable = IsTaskGPUCapable(task_id);
    unsigned task_memory = GetTaskMemory(task_id);
    
    // Determine priority based on SLA
    Priority_t priority = getPriorityForSLA(required_sla);
    
    // Try to find a suitable existing VM first (power-aware)
    VMId_t target_vm = VMId_t(0);
    bool vm_found = false;
    double best_power_score = -1.0;
    
    for(auto vm_id : vms) {
        if(canPlaceTaskOnVM(vm_id, task_id)) {
            VMInfo_t vm_info = VM_GetInfo(vm_id);
            MachineInfo_t machine_info = Machine_GetInfo(vm_info.machine_id);
            
            // Calculate power efficiency score for this placement
            double efficiency = calculatePowerEfficiency(machine_info);
            double incremental_cost = calculateIncrementalPowerCost(machine_info, task_memory);
            double score = (incremental_cost > 0) ? efficiency / incremental_cost : efficiency * 1000;
            
            if(!vm_found || score > best_power_score) {
                target_vm = vm_id;
                best_power_score = score;
                vm_found = true;
            }
        }
    }
    
    // If no suitable VM found, create a new one using power-efficient placement
    if(!vm_found) {
        target_vm = findOrCreateVM(required_vm, required_cpu, gpu_capable);
    }
    
    // Verify VM is ready before adding task
    VMInfo_t vm_info = VM_GetInfo(target_vm);
    if(vm_info.machine_id >= MachineId_t(Machine_GetTotal()) || migrating_vms.find(target_vm) != migrating_vms.end()) {
        target_vm = findOrCreateVM(required_vm, required_cpu, gpu_capable);
    }
    
    // Add task to VM
    bool task_placed = false;
    int retry_count = 0;
    while(!task_placed && retry_count < 3) {
        try {
            VM_AddTask(target_vm, task_id, priority);
            SimOutput("pMapper::NewTask(): Placed task " + to_string(task_id) + 
                      " on VM " + to_string(target_vm) + " (power-efficient)", 3);
            task_placed = true;
        } catch(const exception& e) {
            SimOutput("pMapper::NewTask(): ERROR placing task " + to_string(task_id) + ": " + string(e.what()), 1);
            retry_count++;
            
            // Try to create a new VM with power-efficient placement
            MachineId_t machine_id = findPowerEfficientMachine(required_cpu, gpu_capable, task_memory + VM_MEMORY_OVERHEAD);
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
                SimOutput("pMapper::NewTask(): CRITICAL - No suitable machine found for task " + to_string(task_id), 0);
                break;
            }
        }
    }
    
    if(!task_placed) {
        SimOutput("pMapper::NewTask(): FAILED to place task " + to_string(task_id) + " after retries", 0);
    }
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    // Periodically try to consolidate
    static unsigned consolidation_counter = 0;
    consolidation_counter++;
    if(consolidation_counter % 10 == 0) {
        powerAwareConsolidation();
    }
}

void Scheduler::powerAwareConsolidation() {
    SimOutput("pMapper::powerAwareConsolidation(): Attempting power-aware consolidation", 3);
    
    // Find machines with low utilization that can be consolidated
    for(auto machine_id : machines) {
        MachineInfo_t info = Machine_GetInfo(machine_id);
        
        // Skip if machine is already off or transitioning
        if(info.s_state == S5 || info.s_state == S3 || info.s_state == S4) {
            continue;
        }
        
        // If machine has no active tasks and no VMs, power it down
        if(info.active_tasks == 0 && info.active_vms == 0) {
            SimOutput("pMapper::powerAwareConsolidation(): Powering down empty machine " + to_string(machine_id), 2);
            Machine_SetState(machine_id, S5);
        }
        // If machine has no active tasks but has VMs, check if we can consolidate
        // Only power down if machine has no active tasks AND no VMs (VMs will be cleaned up when they have no tasks)
        else if(info.active_tasks == 0 && info.active_vms == 0) {
            // This case is already handled above, but keep for safety
            Machine_SetState(machine_id, S5);
        }
    }
}

void Scheduler::consolidateMachines() {
    // Use power-aware consolidation
    powerAwareConsolidation();
}

void Scheduler::handleSLAViolation(TaskId_t task_id) {
    SimOutput("pMapper::handleSLAViolation(): Handling SLA violation for task " + to_string(task_id), 1);
    
    TaskInfo_t task_info = GetTaskInfo(task_id);
    
    // Increase priority to highest - this is the main action
    if(task_info.priority != HIGH_PRIORITY) {
        SetTaskPriority(task_id, HIGH_PRIORITY);
        SimOutput("pMapper::handleSLAViolation(): Increased priority for task " + to_string(task_id), 1);
    }
    
    // Don't migrate during SLA violations - migration takes time and might make things worse
    // Just increase priority and let the system handle it
}

void Scheduler::PeriodicCheck(Time_t now) {
    // Periodic power-aware consolidation
    static unsigned check_counter = 0;
    check_counter++;
    
    if(check_counter % 20 == 0) {
        powerAwareConsolidation();
    }
}

void Scheduler::Shutdown(Time_t time) {
    SimOutput("pMapper::Shutdown(): Shutting down scheduler", 2);
    
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

void Scheduler::handleMemoryOverflow(MachineId_t machine_id) {
    SimOutput("pMapper::handleMemoryOverflow(): Overflow at machine " + to_string(machine_id), 1);
    
    // For memory overflow, we'll let the system handle it naturally
    // Trying to migrate during overflow can cause issues
    // The periodic consolidation will handle rebalancing
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    Scheduler.handleMemoryOverflow(machine_id);
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
    SimOutput("pMapper::StateChangeComplete(): Machine " + to_string(machine_id) + " state change complete", 3);
}
