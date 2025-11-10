//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//
//  Energy-Aware Greedy Scheduler Implementation
//  CS 378 Cloud Management for Energy and Performance
//
//  Algorithm: Greedy best-fit consolidation scheduler
//  Goal: Minimize energy by consolidating tasks on fewest active servers
//        while respecting SLA and resource constraints
//

#include "Scheduler.hpp"
#include <algorithm>
#include <map>
#include <queue>
#include <cmath>

// ============================================================================
// CONFIGURATION FLAGS - Tune these for experiments
// ============================================================================

// Enable consolidation-first strategy (prefer active servers over waking new ones)
static const bool CONSOLIDATE_FIRST = true;

// Number of ticks a server must be idle before considering sleep/shutdown
static const unsigned IDLE_COOLDOWN_TICKS = 5;

// Estimated wake penalty in watts (used if simulator doesn't expose transition costs)
static const double WAKE_PENALTY_WATTS = 20.0;

// Fallback utilization-to-power slope (0.0-1.0) if detailed power curve unavailable
// Power ≈ base_power + (peak_power - base_power) * UTIL_POWER_SLOPE * utilization
static const double UTIL_POWER_SLOPE = 0.6;

// Enable detailed per-tick logging for report analysis
#define ENABLE_SCHEDULER_LOGGING 1

// ============================================================================
// INTERNAL DATA STRUCTURES
// ============================================================================

// Server state snapshot for scheduling decisions
struct ServerState {
    MachineId_t machine_id;
    CPUType_t cpu_type;
    VMType_t vm_type;           // VM type currently running (if any)
    
    unsigned total_cpu_cores;
    unsigned total_memory;
    unsigned used_memory;
    unsigned active_tasks;
    unsigned active_vms;
    
    bool is_active;             // S0 state vs. S1-S5
    bool has_gpu;
    unsigned idle_ticks;        // Consecutive ticks with zero load
    
    // Power model parameters
    double base_power_watts;    // Idle power when active
    double peak_power_watts;    // Max power at full utilization
    uint64_t energy_consumed;
    
    MachineState_t s_state;
    
    ServerState() : machine_id(0), cpu_type(X86), vm_type(LINUX),
                    total_cpu_cores(0), total_memory(0), used_memory(0),
                    active_tasks(0), active_vms(0), is_active(false),
                    has_gpu(false), idle_ticks(0),
                    base_power_watts(0), peak_power_watts(0),
                    energy_consumed(0), s_state(S5) {}
};

// Pending task information
struct PendingTask {
    TaskId_t task_id;
    CPUType_t required_cpu;
    VMType_t required_vm;
    unsigned required_memory;
    bool gpu_capable;
    
    Time_t arrival_time;
    Time_t target_completion;
    SLAType_t sla_type;
    Priority_t priority;
    
    double slack;               // Computed: target - arrival - estimated_runtime
    
    PendingTask() : task_id(0), required_cpu(X86), required_vm(LINUX),
                    required_memory(0), gpu_capable(false),
                    arrival_time(0), target_completion(0),
                    sla_type(SLA3), priority(LOW_PRIORITY), slack(0) {}
};

// Placement candidate for greedy selection
struct PlacementCandidate {
    MachineId_t machine_id;
    VMId_t vm_id;
    double incremental_power_cost;
    double post_placement_utilization;
    bool needs_wake;
    
    PlacementCandidate(MachineId_t m, VMId_t v, double power, double util, bool wake)
        : machine_id(m), vm_id(v), incremental_power_cost(power),
          post_placement_utilization(util), needs_wake(wake) {}
    
    // Comparison for greedy selection: lower power cost is better
    bool operator<(const PlacementCandidate& other) const {
        if (std::abs(incremental_power_cost - other.incremental_power_cost) > 0.01)
            return incremental_power_cost < other.incremental_power_cost;
        // Tie-break: prefer higher utilization (better consolidation)
        if (std::abs(post_placement_utilization - other.post_placement_utilization) > 0.01)
            return post_placement_utilization > other.post_placement_utilization;
        // Tie-break: prefer already-active servers
        return !needs_wake && other.needs_wake;
    }
};

// ============================================================================
// SCHEDULER STATE
// ============================================================================

static std::map<MachineId_t, ServerState> server_states;
static std::map<MachineId_t, VMId_t> machine_to_vm;
static std::map<VMId_t, MachineId_t> vm_to_machine;
static std::queue<TaskId_t> pending_tasks;
static unsigned total_machines = 0;
static Time_t last_check_time = 0;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Update server state from simulator
void UpdateServerState(MachineId_t machine_id) {
    MachineInfo_t info = Machine_GetInfo(machine_id);
    ServerState& state = server_states[machine_id];
    
    state.machine_id = machine_id;
    state.cpu_type = info.cpu;
    state.total_cpu_cores = info.num_cpus;
    state.total_memory = info.memory_size;
    state.used_memory = info.memory_used;
    state.active_tasks = info.active_tasks;
    state.active_vms = info.active_vms;
    state.has_gpu = info.gpus;
    state.s_state = info.s_state;
    state.is_active = (info.s_state == S0 || info.s_state == S0i1);
    state.energy_consumed = info.energy_consumed;
    
    // Extract power model from s_states vector
    if (!info.s_states.empty()) {
        state.base_power_watts = info.s_states[0];
        if (!info.p_states.empty() && info.num_cpus > 0) {
            state.peak_power_watts = state.base_power_watts + info.num_cpus * info.p_states[0];
        } else {
            state.peak_power_watts = state.base_power_watts * 2.0;
        }
    }
    
    if (state.active_tasks == 0) {
        state.idle_ticks++;
    } else {
        state.idle_ticks = 0;
    }
}

// Check if task fits on server (machine type, memory, VM compatibility)
bool Fits(const ServerState& server, const PendingTask& task) {
    if (server.cpu_type != task.required_cpu) return false;
    if (task.gpu_capable && !server.has_gpu) return false;
    
    unsigned required_mem = task.required_memory + VM_MEMORY_OVERHEAD;
    if (server.used_memory + required_mem > server.total_memory) return false;
    
    return true;
}

// Estimate incremental power cost of placing task on server
double IncrementalPowerCost(const ServerState& server, const PendingTask& task) {
    double cost = 0.0;
    
    if (!server.is_active) {
        cost += WAKE_PENALTY_WATTS;
        cost += server.base_power_watts;
    }
    
    double current_util = server.total_cpu_cores > 0 ? 
        (double)server.active_tasks / (double)server.total_cpu_cores : 0.0;
    double new_util = server.total_cpu_cores > 0 ?
        (double)(server.active_tasks + 1) / (double)server.total_cpu_cores : 1.0;
    
    double power_range = server.peak_power_watts - server.base_power_watts;
    double current_power = server.base_power_watts + power_range * UTIL_POWER_SLOPE * current_util;
    double new_power = server.base_power_watts + power_range * UTIL_POWER_SLOPE * new_util;
    
    cost += (new_power - current_power);
    
    return cost;
}

// Compute slack for SLA-aware prioritization
double ComputeSlack(const PendingTask& task, Time_t now) {
    if (task.target_completion <= task.arrival_time) return 0.0;
    
    Time_t time_budget = task.target_completion - task.arrival_time;
    Time_t elapsed = (now > task.arrival_time) ? (now - task.arrival_time) : 0;
    
    return (double)(time_budget - elapsed) / 1000000.0;
}

// Convert SLA type to priority weight (tighter SLA = higher priority)
double SLAToPriority(SLAType_t sla) {
    switch(sla) {
        case SLA0: return 4.0;
        case SLA1: return 3.0;
        case SLA2: return 2.0;
        case SLA3: return 1.0;
        default: return 1.0;
    }
}

// Logging helper
void SchedulerLog(const std::string& msg) {
#if ENABLE_SCHEDULER_LOGGING
    SimOutput("[EnergyScheduler] " + msg, 3);
#endif
}

void Scheduler::Init() {
    total_machines = Machine_GetTotal();
    SimOutput("Scheduler::Init(): Total machines = " + to_string(total_machines), 1);
    SchedulerLog("Initializing energy-aware greedy scheduler");
    SchedulerLog("Config: CONSOLIDATE_FIRST=" + to_string(CONSOLIDATE_FIRST) + 
                 ", IDLE_COOLDOWN=" + to_string(IDLE_COOLDOWN_TICKS));
    
    // Initialize server state tracking for all machines
    for (unsigned i = 0; i < total_machines; i++) {
        MachineId_t mid = MachineId_t(i);
        UpdateServerState(mid);
        machines.push_back(mid);
        
        MachineInfo_t info = Machine_GetInfo(mid);
        SchedulerLog("Machine " + to_string(i) + ": CPU=" + to_string(info.cpu) + 
                     ", Cores=" + to_string(info.num_cpus) + 
                     ", Mem=" + to_string(info.memory_size) + 
                     ", GPU=" + to_string(info.gpus));
    }
    
    // Create one VM per machine type for initial placement
    // Start with minimal active set for energy efficiency
    unsigned initial_active = std::min(4u, total_machines);
    
    for (unsigned i = 0; i < initial_active; i++) {
        MachineId_t mid = MachineId_t(i);
        MachineInfo_t info = Machine_GetInfo(mid);
        
        // Create VM matching machine's CPU type
        VMId_t vm = VM_Create(LINUX, info.cpu);
        vms.push_back(vm);
        VM_Attach(vm, mid);
        
        machine_to_vm[mid] = vm;
        vm_to_machine[vm] = mid;
        
        // Ensure machine is active
        if (info.s_state != S0) {
            Machine_SetState(mid, S0);
        }
        
        SchedulerLog("Created VM " + to_string(vm) + " on machine " + to_string(mid));
    }
    
    // Put remaining machines in low-power state
    for (unsigned i = initial_active; i < total_machines; i++) {
        Machine_SetState(MachineId_t(i), S5);
    }
    
    SchedulerLog("Initialization complete: " + to_string(initial_active) + " active machines");
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    SchedulerLog("Migration complete for VM " + to_string(vm_id) + " at time " + to_string(time));
    // Update VM-to-machine mapping if needed
    VMInfo_t vm_info = VM_GetInfo(vm_id);
    if (vm_to_machine.find(vm_id) != vm_to_machine.end()) {
        MachineId_t old_machine = vm_to_machine[vm_id];
        if (old_machine != vm_info.machine_id) {
            machine_to_vm.erase(old_machine);
        }
    }
    vm_to_machine[vm_id] = vm_info.machine_id;
    machine_to_vm[vm_info.machine_id] = vm_id;
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    // Gather task requirements
    TaskInfo_t task_info = GetTaskInfo(task_id);
    
    PendingTask task;
    task.task_id = task_id;
    task.required_cpu = RequiredCPUType(task_id);
    task.required_vm = RequiredVMType(task_id);
    task.required_memory = GetTaskMemory(task_id);
    task.gpu_capable = IsTaskGPUCapable(task_id);
    task.arrival_time = task_info.arrival;
    task.target_completion = task_info.target_completion;
    task.sla_type = RequiredSLA(task_id);
    task.priority = task_info.priority;
    task.slack = ComputeSlack(task, now);
    
    SchedulerLog("NewTask " + to_string(task_id) + ": CPU=" + to_string(task.required_cpu) +
                 ", Mem=" + to_string(task.required_memory) + 
                 ", SLA=" + to_string(task.sla_type) + 
                 ", Slack=" + to_string(task.slack));
    
    // Update all server states
    for (auto& pair : server_states) {
        UpdateServerState(pair.first);
    }
    
    // Find candidate placements using greedy best-fit
    std::vector<PlacementCandidate> candidates;
    
    // Phase 1: Check active servers (consolidation-first)
    if (CONSOLIDATE_FIRST) {
        for (auto& pair : server_states) {
            ServerState& server = pair.second;
            if (!server.is_active) continue;
            if (!Fits(server, task)) continue;
            
            // Find or create VM on this machine
            VMId_t vm_id = 0;
            if (machine_to_vm.find(server.machine_id) != machine_to_vm.end()) {
                vm_id = machine_to_vm[server.machine_id];
            }
            
            if (vm_id == 0) continue;
            
            double power_cost = IncrementalPowerCost(server, task);
            double util = server.total_cpu_cores > 0 ?
                (double)(server.active_tasks + 1) / (double)server.total_cpu_cores : 1.0;
            
            candidates.push_back(PlacementCandidate(server.machine_id, vm_id, power_cost, util, false));
        }
    }
    
    // Phase 2: Consider waking inactive servers if no good active fit
    if (candidates.empty() || !CONSOLIDATE_FIRST) {
        for (auto& pair : server_states) {
            ServerState& server = pair.second;
            if (server.is_active) continue;
            if (!Fits(server, task)) continue;
            
            VMId_t vm_id = 0;
            if (machine_to_vm.find(server.machine_id) != machine_to_vm.end()) {
                vm_id = machine_to_vm[server.machine_id];
            } else {
                // Need to create VM on this machine
                vm_id = VM_Create(task.required_vm, server.cpu_type);
                machine_to_vm[server.machine_id] = vm_id;
                vm_to_machine[vm_id] = server.machine_id;
                vms.push_back(vm_id);
            }
            
            double power_cost = IncrementalPowerCost(server, task);
            double util = server.total_cpu_cores > 0 ? 
                1.0 / (double)server.total_cpu_cores : 1.0;
            
            candidates.push_back(PlacementCandidate(server.machine_id, vm_id, power_cost, util, true));
        }
    }
    
    // Select best candidate
    if (!candidates.empty()) {
        std::sort(candidates.begin(), candidates.end());
        PlacementCandidate& best = candidates[0];
        
        // Wake machine if needed
        if (best.needs_wake) {
            Machine_SetState(best.machine_id, S0);
            SchedulerLog("Waking machine " + to_string(best.machine_id) + " for task " + to_string(task_id));
        }
        
        // Attach VM if not already attached
        VMInfo_t vm_info = VM_GetInfo(best.vm_id);
        if (vm_info.machine_id != best.machine_id) {
            VM_Attach(best.vm_id, best.machine_id);
        }
        
        // Add task to VM with SLA-based priority
        Priority_t priority = MID_PRIORITY;
        if (task.sla_type == SLA0) priority = HIGH_PRIORITY;
        else if (task.sla_type == SLA1) priority = MID_PRIORITY;
        else priority = LOW_PRIORITY;
        
        VM_AddTask(best.vm_id, task_id, priority);
        
        SchedulerLog("Placed task " + to_string(task_id) + " on machine " + 
                     to_string(best.machine_id) + " (VM " + to_string(best.vm_id) + 
                     "), power_cost=" + to_string(best.incremental_power_cost));
    } else {
        SchedulerLog("WARNING: No feasible placement for task " + to_string(task_id));
        pending_tasks.push(task_id);
    }
}

void Scheduler::PeriodicCheck(Time_t now) {
    last_check_time = now;
    
    // Update all server states
    for (auto& pair : server_states) {
        UpdateServerState(pair.first);
    }
    
    // Retry pending tasks
    unsigned pending_count = pending_tasks.size();
    for (unsigned i = 0; i < pending_count; i++) {
        TaskId_t task_id = pending_tasks.front();
        pending_tasks.pop();
        
        if (!IsTaskCompleted(task_id)) {
            NewTask(now, task_id);
        }
    }
    
    // Power management: identify idle servers for sleep
    unsigned active_count = 0;
    unsigned idle_candidates = 0;
    double total_power = 0.0;
    
    for (auto& pair : server_states) {
        ServerState& server = pair.second;
        
        if (server.is_active) {
            active_count++;
            
            // Estimate current power consumption
            double util = server.total_cpu_cores > 0 ?
                (double)server.active_tasks / (double)server.total_cpu_cores : 0.0;
            double power = server.base_power_watts + 
                          (server.peak_power_watts - server.base_power_watts) * UTIL_POWER_SLOPE * util;
            total_power += power;
            
            // Consider sleeping idle servers
            if (server.active_tasks == 0 && server.idle_ticks >= IDLE_COOLDOWN_TICKS) {
                idle_candidates++;
                
                // Keep at least one server per CPU type active
                bool is_last_of_type = true;
                for (auto& other_pair : server_states) {
                    ServerState& other = other_pair.second;
                    if (other.machine_id != server.machine_id &&
                        other.cpu_type == server.cpu_type &&
                        other.is_active &&
                        other.active_tasks > 0) {
                        is_last_of_type = false;
                        break;
                    }
                }
                
                if (!is_last_of_type) {
                    Machine_SetState(server.machine_id, S5);
                    SchedulerLog("Sleeping idle machine " + to_string(server.machine_id) + 
                                 " after " + to_string(server.idle_ticks) + " idle ticks");
                }
            }
        }
    }
    
    SchedulerLog("PeriodicCheck: Active=" + to_string(active_count) + 
                 ", IdleCandidates=" + to_string(idle_candidates) +
                 ", EstPower=" + to_string(total_power) + "W" +
                 ", Pending=" + to_string(pending_tasks.size()));
}

void Scheduler::Shutdown(Time_t time) {
    SchedulerLog("=== Scheduler Shutdown Report ===");
    
    // Final energy and utilization statistics
    double total_energy = Machine_GetClusterEnergy();
    unsigned total_active = 0;
    unsigned total_tasks_processed = 0;
    
    for (auto& pair : server_states) {
        UpdateServerState(pair.first);
        if (pair.second.is_active) total_active++;
    }
    
    SchedulerLog("Total energy consumed: " + to_string(total_energy) + " KW-Hour");
    SchedulerLog("Final active machines: " + to_string(total_active) + "/" + to_string(total_machines));
    SchedulerLog("Simulation duration: " + to_string(double(time)/1000000.0) + " seconds");
    
    // Shutdown all VMs
    for(auto & vm: vms) {
        VM_Shutdown(vm);
    }
    
    SimOutput("Scheduler::Shutdown(): Energy-aware scheduler shutdown complete", 1);
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    SchedulerLog("TaskComplete: Task " + to_string(task_id) + " finished at " + to_string(now));
    
    // Update server states to reflect task completion
    for (auto& pair : server_states) {
        UpdateServerState(pair.first);
    }
    
    // Opportunistically check for consolidation opportunities
    // This could trigger migrations or power-downs in a more sophisticated implementation
}

// Public interface below

static Scheduler Scheduler;

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing scheduler", 4);
    Scheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): Received new task " + to_string(task_id) + " at time " + to_string(time), 4);
    Scheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): Task " + to_string(task_id) + " completed at time " + to_string(time), 4);
    Scheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    // The simulator is alerting you that machine identified by machine_id is overcommitted
    SimOutput("MemoryWarning(): Overflow at " + to_string(machine_id) + " was detected at time " + to_string(time), 0);
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    SimOutput("MigrationDone(): Migration of VM " + to_string(vm_id) + " was completed at time " + to_string(time), 4);
    Scheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    SimOutput("SchedulerCheck(): Called at " + to_string(time), 4);
    Scheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    cout << "\n=== SIMULATION COMPLETE ===" << endl;
    cout << "SLA Violation Report:" << endl;
    cout << "  SLA0 (95% target): " << GetSLAReport(SLA0) << "%" << endl;
    cout << "  SLA1 (90% target): " << GetSLAReport(SLA1) << "%" << endl;
    cout << "  SLA2 (80% target): " << GetSLAReport(SLA2) << "%" << endl;
    cout << "\nEnergy Report:" << endl;
    cout << "  Total Energy: " << Machine_GetClusterEnergy() << " KW-Hour" << endl;
    cout << "\nSimulation Duration: " << double(time)/1000000 << " seconds" << endl;
    cout << "========================\n" << endl;
    
    SimOutput("SimulationComplete(): Simulation finished at time " + to_string(time), 4);
    Scheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    SchedulerLog("SLA WARNING: Task " + to_string(task_id) + " at risk at time " + to_string(time));
    // Could implement priority boost or migration here
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    SchedulerLog("StateChange complete for machine " + to_string(machine_id) + " at " + to_string(time));
    UpdateServerState(machine_id);
}

