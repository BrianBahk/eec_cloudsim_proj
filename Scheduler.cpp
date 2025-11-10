//
//  Scheduler.cpp
//  CloudSim
//
//  Created by ELMOOTAZBELLAH ELNOZAHY on 10/20/24.
//
//  PowerNap-Inspired Aggressive Power Management Scheduler
//  CS 378 Cloud Management for Energy and Performance
//
//  Algorithm: PowerNap with workload prediction and aggressive power-down
//  Goal: Minimize energy through very aggressive idle server sleep
//  Literature: Based on "The Case for Energy-Proportional Computing" (Barroso & Hölzle)
//              and "PowerNap: Eliminating Server Idle Power" (Meisner et al., ASPLOS 2009)
//
//  Key Principles:
//    - Transition servers to deep sleep (S4/S5) within seconds of idleness
//    - Predict incoming workload bursts using exponential moving average
//    - Pre-emptively wake servers based on predictions
//    - Consolidate workload aggressively to minimize active server count
//

#include "Scheduler.hpp"
#include <algorithm>
#include <map>
#include <queue>
#include <vector>
#include <cmath>
#include <deque>

// ============================================================================
// CONFIGURATION FLAGS
// ============================================================================

// PowerNap parameters
static const unsigned IDLE_TO_SLEEP_TICKS = 2;     // Very aggressive: sleep after 2 idle ticks
static const unsigned PREDICTION_WINDOW = 10;       // Tasks to consider for arrival rate prediction
static const double PREDICTION_ALPHA = 0.3;         // Exponential moving average weight
static const double WAKE_THRESHOLD = 0.7;           // Wake when predicted utilization > 70%
static const unsigned MIN_ACTIVE_SERVERS = 1;       // Absolute minimum active servers

// Consolidation parameters
static const double CONSOLIDATION_TARGET = 0.85;    // Target 85% utilization for consolidation
static const double OVERLOAD_THRESHOLD = 0.95;      // Above 95% = need more servers

// Enable detailed logging
#define ENABLE_SCHEDULER_LOGGING 1

// ============================================================================
// DATA STRUCTURES
// ============================================================================

struct ServerState {
    MachineId_t machine_id;
    CPUType_t cpu_type;
    
    unsigned total_cpu_cores;
    unsigned total_memory;
    unsigned used_memory;
    unsigned active_tasks;
    unsigned active_vms;
    
    bool is_active;
    bool has_gpu;
    unsigned idle_ticks;
    Time_t last_active_time;
    
    double base_power_watts;
    double peak_power_watts;
    uint64_t energy_consumed;
    
    MachineState_t s_state;
    
    // Utilization tracking
    double current_utilization;
    std::deque<double> utilization_history;
    
    ServerState() : machine_id(0), cpu_type(X86),
                    total_cpu_cores(0), total_memory(0), used_memory(0),
                    active_tasks(0), active_vms(0), is_active(false),
                    has_gpu(false), idle_ticks(0), last_active_time(0),
                    base_power_watts(0), peak_power_watts(0),
                    energy_consumed(0), s_state(S5), current_utilization(0.0) {}
};

struct TaskArrivalRecord {
    Time_t arrival_time;
    unsigned num_tasks;
    
    TaskArrivalRecord(Time_t t, unsigned n) : arrival_time(t), num_tasks(n) {}
};

struct WorkloadPredictor {
    std::deque<TaskArrivalRecord> arrival_history;
    double predicted_arrival_rate;     // Tasks per second
    double moving_avg_load;            // EMA of system load
    
    WorkloadPredictor() : predicted_arrival_rate(0.0), moving_avg_load(0.0) {}
    
    void RecordArrival(Time_t now, unsigned count = 1) {
        arrival_history.push_back(TaskArrivalRecord(now, count));
        if (arrival_history.size() > PREDICTION_WINDOW) {
            arrival_history.pop_front();
        }
        UpdatePrediction();
    }
    
    void UpdatePrediction() {
        if (arrival_history.size() < 2) {
            predicted_arrival_rate = 0.0;
            return;
        }
        
        Time_t time_span = arrival_history.back().arrival_time - arrival_history.front().arrival_time;
        if (time_span == 0) {
            predicted_arrival_rate = 0.0;
            return;
        }
        
        unsigned total_tasks = 0;
        for (const auto& record : arrival_history) {
            total_tasks += record.num_tasks;
        }
        
        // Tasks per microsecond * 1000000 = tasks per second
        predicted_arrival_rate = (double)total_tasks * 1000000.0 / (double)time_span;
    }
    
    void UpdateLoad(double current_load) {
        if (moving_avg_load == 0.0) {
            moving_avg_load = current_load;
        } else {
            moving_avg_load = PREDICTION_ALPHA * current_load + (1.0 - PREDICTION_ALPHA) * moving_avg_load;
        }
    }
    
    bool ShouldWakeServer() const {
        return moving_avg_load > WAKE_THRESHOLD || predicted_arrival_rate > 5.0;
    }
};

// ============================================================================
// SCHEDULER STATE
// ============================================================================

static std::map<MachineId_t, ServerState> server_states;
static std::map<MachineId_t, VMId_t> machine_to_vm;
static std::map<VMId_t, MachineId_t> vm_to_machine;
static std::vector<TaskId_t> pending_tasks;
static unsigned total_machines = 0;
static Time_t last_check_time = 0;

// PowerNap-specific state
static WorkloadPredictor predictor;
static unsigned total_sleep_transitions = 0;
static unsigned total_wake_transitions = 0;
static unsigned tasks_arrived_this_period = 0;

// Statistics
static unsigned total_tasks_arrived = 0;
static unsigned total_tasks_completed = 0;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

void SchedulerLog(const std::string& msg) {
#if ENABLE_SCHEDULER_LOGGING
    SimOutput("[PowerNap] " + msg, 3);
#endif
}

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
    
    // Calculate current utilization
    if (state.total_cpu_cores > 0) {
        state.current_utilization = (double)state.active_tasks / (double)state.total_cpu_cores;
    } else {
        state.current_utilization = 0.0;
    }
    
    // Track utilization history
    state.utilization_history.push_back(state.current_utilization);
    if (state.utilization_history.size() > 10) {
        state.utilization_history.pop_front();
    }
    
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
        state.last_active_time = last_check_time;
    }
}

bool TaskFitsOnServer(const ServerState& server, TaskId_t task_id) {
    CPUType_t required_cpu = RequiredCPUType(task_id);
    if (server.cpu_type != required_cpu) return false;
    
    bool gpu_capable = IsTaskGPUCapable(task_id);
    if (gpu_capable && !server.has_gpu) return false;
    
    unsigned required_mem = GetTaskMemory(task_id) + VM_MEMORY_OVERHEAD;
    if (server.used_memory + required_mem > server.total_memory) return false;
    
    return true;
}

Priority_t DetermineTaskPriority(TaskId_t task_id) {
    SLAType_t sla = RequiredSLA(task_id);
    switch(sla) {
        case SLA0: return HIGH_PRIORITY;
        case SLA1: return MID_PRIORITY;
        case SLA2: return LOW_PRIORITY;
        case SLA3: return LOW_PRIORITY;
        default: return MID_PRIORITY;
    }
}

VMId_t GetOrCreateVM(MachineId_t machine_id, VMType_t vm_type, CPUType_t cpu_type) {
    if (machine_to_vm.find(machine_id) != machine_to_vm.end()) {
        return machine_to_vm[machine_id];
    }
    
    VMId_t vm_id = VM_Create(vm_type, cpu_type);
    machine_to_vm[machine_id] = vm_id;
    vm_to_machine[vm_id] = machine_id;
    
    return vm_id;
}

void WakeServer(MachineId_t machine_id) {
    ServerState& server = server_states[machine_id];
    if (!server.is_active) {
        Machine_SetState(machine_id, S0);
        total_wake_transitions++;
        SchedulerLog("Waking server " + std::to_string(machine_id));
    }
}

void SleepServer(MachineId_t machine_id) {
    ServerState& server = server_states[machine_id];
    if (server.is_active && server.active_tasks == 0) {
        Machine_SetState(machine_id, S5);  // Deep sleep for maximum power savings
        total_sleep_transitions++;
        SchedulerLog("Sleeping server " + std::to_string(machine_id) + 
                     " (idle for " + std::to_string(server.idle_ticks) + " ticks)");
    }
}

// Calculate system-wide load
double CalculateSystemLoad() {
    unsigned total_tasks = 0;
    unsigned total_capacity = 0;
    unsigned active_servers = 0;
    
    for (auto& pair : server_states) {
        ServerState& server = pair.second;
        if (server.is_active) {
            total_tasks += server.active_tasks;
            total_capacity += server.total_cpu_cores;
            active_servers++;
        }
    }
    
    if (total_capacity == 0) return 0.0;
    return (double)total_tasks / (double)total_capacity;
}

// ============================================================================
// SCHEDULER IMPLEMENTATION
// ============================================================================

void Scheduler::Init() {
    total_machines = Machine_GetTotal();
    SimOutput("Scheduler::Init(): PowerNap Scheduler - Total machines = " + std::to_string(total_machines), 1);
    SchedulerLog("Initializing PowerNap aggressive power management scheduler");
    SchedulerLog("Config: IDLE_TO_SLEEP=" + std::to_string(IDLE_TO_SLEEP_TICKS) + 
                 " ticks, CONSOLIDATION_TARGET=" + std::to_string(CONSOLIDATION_TARGET));
    
    // Initialize all server states
    for (unsigned i = 0; i < total_machines; i++) {
        MachineId_t mid = MachineId_t(i);
        UpdateServerState(mid);
        machines.push_back(mid);
        
        MachineInfo_t info = Machine_GetInfo(mid);
        SchedulerLog("Machine " + std::to_string(i) + ": CPU=" + std::to_string(info.cpu) + 
                     ", Cores=" + std::to_string(info.num_cpus) + 
                     ", Mem=" + std::to_string(info.memory_size) + 
                     ", GPU=" + std::to_string(info.gpus));
    }
    
    // PowerNap principle: Start with absolute minimum active servers
    unsigned initial_active = MIN_ACTIVE_SERVERS;
    
    for (unsigned i = 0; i < initial_active; i++) {
        MachineId_t mid = MachineId_t(i);
        MachineInfo_t info = Machine_GetInfo(mid);
        
        VMId_t vm = VM_Create(LINUX, info.cpu);
        vms.push_back(vm);
        VM_Attach(vm, mid);
        
        machine_to_vm[mid] = vm;
        vm_to_machine[vm] = mid;
        
        if (info.s_state != S0) {
            Machine_SetState(mid, S0);
        }
        
        SchedulerLog("Created VM " + std::to_string(vm) + " on machine " + std::to_string(mid));
    }
    
    // Aggressively power down ALL other machines
    for (unsigned i = initial_active; i < total_machines; i++) {
        Machine_SetState(MachineId_t(i), S5);
    }
    
    SchedulerLog("Initialization complete: " + std::to_string(initial_active) + 
                 " active machines (PowerNap mode)");
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    SchedulerLog("Migration complete for VM " + std::to_string(vm_id));
    
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
    total_tasks_arrived++;
    tasks_arrived_this_period++;
    
    // Update workload predictor
    predictor.RecordArrival(now);
    
    TaskInfo_t task_info = GetTaskInfo(task_id);
    CPUType_t required_cpu = RequiredCPUType(task_id);
    VMType_t required_vm = RequiredVMType(task_id);
    
    SchedulerLog("NewTask " + std::to_string(task_id) + 
                 ": CPU=" + std::to_string(required_cpu) +
                 ", SLA=" + std::to_string(RequiredSLA(task_id)) +
                 ", ArrivalRate=" + std::to_string(predictor.predicted_arrival_rate) + " tasks/sec");
    
    // Update all server states
    for (auto& pair : server_states) {
        UpdateServerState(pair.first);
    }
    
    // Phase 1: Try to place on existing active servers (consolidation)
    MachineId_t best_machine = 0;
    double best_utilization = 2.0;  // Want highest utilization < CONSOLIDATION_TARGET
    bool found = false;
    
    for (auto& pair : server_states) {
        ServerState& server = pair.second;
        if (!server.is_active) continue;
        if (!TaskFitsOnServer(server, task_id)) continue;
        
        double util_after = (double)(server.active_tasks + 1) / (double)server.total_cpu_cores;
        
        // Prefer servers that will be well-utilized but not overloaded
        if (util_after <= CONSOLIDATION_TARGET) {
            // Among candidates, pick the one that results in highest utilization
            if (best_machine == 0 || util_after > best_utilization) {
                best_machine = server.machine_id;
                best_utilization = util_after;
                found = true;
            }
        }
    }
    
    // Phase 2: If no good fit, need to wake a server
    if (!found) {
        // Find the best sleeping server to wake
        for (auto& pair : server_states) {
            ServerState& server = pair.second;
            if (server.is_active) continue;
            if (!TaskFitsOnServer(server, task_id)) continue;
            
            // Wake this server
            best_machine = server.machine_id;
            found = true;
            WakeServer(best_machine);
            break;
        }
    }
    
    // Place task if we found a server
    if (found) {
        ServerState& server = server_states[best_machine];
        
        // Get or create VM
        VMId_t vm_id = GetOrCreateVM(best_machine, required_vm, server.cpu_type);
        
        // Ensure VM is attached
        VMInfo_t vm_info = VM_GetInfo(vm_id);
        if (vm_info.machine_id != best_machine) {
            VM_Attach(vm_id, best_machine);
        }
        
        // Add task
        Priority_t priority = DetermineTaskPriority(task_id);
        
        try {
            VM_AddTask(vm_id, task_id, priority);
            SchedulerLog("Placed task " + std::to_string(task_id) + 
                         " on machine " + std::to_string(best_machine) +
                         " (util=" + std::to_string(best_utilization) + ")");
        } catch (...) {
            SchedulerLog("Failed to place task " + std::to_string(task_id));
            pending_tasks.push_back(task_id);
        }
    } else {
        SchedulerLog("No available server for task " + std::to_string(task_id));
        pending_tasks.push_back(task_id);
    }
}

void Scheduler::PeriodicCheck(Time_t now) {
    last_check_time = now;
    
    // Update all server states
    for (auto& pair : server_states) {
        UpdateServerState(pair.first);
    }
    
    // Calculate current system load
    double system_load = CalculateSystemLoad();
    predictor.UpdateLoad(system_load);
    
    // Retry pending tasks
    if (!pending_tasks.empty()) {
        std::vector<TaskId_t> tasks_to_retry = pending_tasks;
        pending_tasks.clear();
        
        for (TaskId_t task_id : tasks_to_retry) {
            // Task IDs might be stale, skip invalid ones
            if (task_id < GetNumTasks()) {
                try {
                    if (!IsTaskCompleted(task_id)) {
                        NewTask(now, task_id);
                    }
                } catch (...) {
                    // Skip tasks that cause exceptions
                }
            }
        }
    }
    
    // PowerNap core logic: Aggressive power management
    unsigned active_count = 0;
    unsigned idle_count = 0;
    std::map<CPUType_t, unsigned> active_per_type;
    
    for (auto& pair : server_states) {
        ServerState& server = pair.second;
        if (server.is_active) {
            active_count++;
            active_per_type[server.cpu_type]++;
            
            if (server.active_tasks == 0) {
                idle_count++;
            }
        }
    }
    
    // PowerNap Strategy 1: Aggressive sleep of idle servers
    for (auto& pair : server_states) {
        ServerState& server = pair.second;
        
        if (server.is_active && server.active_tasks == 0 && 
            server.idle_ticks >= IDLE_TO_SLEEP_TICKS) {
            
            // Keep at least MIN_ACTIVE_SERVERS active
            if (active_count > MIN_ACTIVE_SERVERS) {
                SleepServer(server.machine_id);
                active_count--;
            }
        }
    }
    
    // PowerNap Strategy 2: Predictive wake-up
    if (predictor.ShouldWakeServer() && system_load > WAKE_THRESHOLD) {
        // Find a sleeping server to wake preemptively
        for (auto& pair : server_states) {
            ServerState& server = pair.second;
            if (!server.is_active) {
                WakeServer(server.machine_id);
                
                // Create VM on the woken server
                VMId_t vm = VM_Create(LINUX, server.cpu_type);
                vms.push_back(vm);
                VM_Attach(vm, server.machine_id);
                machine_to_vm[server.machine_id] = vm;
                vm_to_machine[vm] = server.machine_id;
                
                SchedulerLog("Predictive wake due to high load (" + 
                             std::to_string(system_load) + ")");
                break;
            }
        }
    }
    
    // PowerNap Strategy 3: Handle overload by waking more servers
    if (system_load > OVERLOAD_THRESHOLD) {
        for (auto& pair : server_states) {
            ServerState& server = pair.second;
            if (!server.is_active) {
                WakeServer(server.machine_id);
                
                VMId_t vm = VM_Create(LINUX, server.cpu_type);
                vms.push_back(vm);
                VM_Attach(vm, server.machine_id);
                machine_to_vm[server.machine_id] = vm;
                vm_to_machine[vm] = server.machine_id;
                
                SchedulerLog("Emergency wake due to overload (" + 
                             std::to_string(system_load) + ")");
                break;
            }
        }
    }
    
    SchedulerLog("PeriodicCheck: Active=" + std::to_string(active_count) +
                 ", Load=" + std::to_string(system_load) +
                 ", PredictedRate=" + std::to_string(predictor.predicted_arrival_rate) +
                 ", Sleeps=" + std::to_string(total_sleep_transitions) +
                 ", Wakes=" + std::to_string(total_wake_transitions));
    
    tasks_arrived_this_period = 0;
}

void Scheduler::Shutdown(Time_t time) {
    SchedulerLog("=== PowerNap Scheduler Shutdown ===");
    
    double total_energy = Machine_GetClusterEnergy();
    unsigned total_active = 0;
    
    for (auto& pair : server_states) {
        UpdateServerState(pair.first);
        if (pair.second.is_active) total_active++;
    }
    
    SchedulerLog("Total energy: " + std::to_string(total_energy) + " KW-Hour");
    SchedulerLog("Tasks arrived: " + std::to_string(total_tasks_arrived));
    SchedulerLog("Tasks completed: " + std::to_string(total_tasks_completed));
    SchedulerLog("Sleep transitions: " + std::to_string(total_sleep_transitions));
    SchedulerLog("Wake transitions: " + std::to_string(total_wake_transitions));
    SchedulerLog("Final active machines: " + std::to_string(total_active));
    
    for(auto & vm: vms) {
        VM_Shutdown(vm);
    }
    
    SimOutput("Scheduler::Shutdown(): PowerNap scheduler shutdown complete", 1);
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    total_tasks_completed++;
    SchedulerLog("Task " + std::to_string(task_id) + " completed");
    
    // Update server states
    for (auto& pair : server_states) {
        UpdateServerState(pair.first);
    }
}

// ============================================================================
// PUBLIC INTERFACE
// ============================================================================

static Scheduler Scheduler;

void InitScheduler() {
    SimOutput("InitScheduler(): Initializing PowerNap scheduler", 4);
    Scheduler.Init();
}

void HandleNewTask(Time_t time, TaskId_t task_id) {
    SimOutput("HandleNewTask(): Task " + std::to_string(task_id) + " at time " + std::to_string(time), 4);
    Scheduler.NewTask(time, task_id);
}

void HandleTaskCompletion(Time_t time, TaskId_t task_id) {
    SimOutput("HandleTaskCompletion(): Task " + std::to_string(task_id) + " completed", 4);
    Scheduler.TaskComplete(time, task_id);
}

void MemoryWarning(Time_t time, MachineId_t machine_id) {
    SimOutput("MemoryWarning(): Machine " + std::to_string(machine_id) + " overcommitted", 0);
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    SimOutput("MigrationDone(): VM " + std::to_string(vm_id) + " migration complete", 4);
    Scheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    SimOutput("SchedulerCheck(): Called at " + std::to_string(time), 4);
    Scheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    std::cout << "\n=== SIMULATION COMPLETE ===" << std::endl;
    std::cout << "Algorithm: PowerNap - Aggressive Power Management with Prediction" << std::endl;
    std::cout << "\nSLA Violation Report:" << std::endl;
    std::cout << "  SLA0 (95% target): " << GetSLAReport(SLA0) << "%" << std::endl;
    std::cout << "  SLA1 (90% target): " << GetSLAReport(SLA1) << "%" << std::endl;
    std::cout << "  SLA2 (80% target): " << GetSLAReport(SLA2) << "%" << std::endl;
    std::cout << "\nEnergy Report:" << std::endl;
    std::cout << "  Total Energy: " << Machine_GetClusterEnergy() << " KW-Hour" << std::endl;
    std::cout << "  Sleep Transitions: " << total_sleep_transitions << std::endl;
    std::cout << "  Wake Transitions: " << total_wake_transitions << std::endl;
    std::cout << "\nSimulation Duration: " << double(time)/1000000 << " seconds" << std::endl;
    std::cout << "========================\n" << std::endl;
    
    SimOutput("SimulationComplete(): Simulation finished", 4);
    Scheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    SchedulerLog("SLA WARNING: Task " + std::to_string(task_id) + " at risk");
    
    // React: boost priority and potentially wake more servers
    SetTaskPriority(task_id, HIGH_PRIORITY);
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    SchedulerLog("StateChange complete for machine " + std::to_string(machine_id));
    UpdateServerState(machine_id);
}
