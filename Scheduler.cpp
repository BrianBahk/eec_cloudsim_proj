//
//  Scheduler.cpp
//  CloudSim - Simple pMapper Implementation
//
//  Focuses on consolidation and reactive power management
//

#include "Scheduler.hpp"
#include <algorithm>
#include <climits>

#define IDLE_THRESHOLD 50
#define PERIODIC_CHECK_INTERVAL 2000000

void Scheduler::Init() {
    SimOutput("Scheduler::Init(): pMapper initialization", 1);
    
    total_machines = Machine_GetTotal();
    last_periodic_check = 0;
    
    DiscoverMachines();
    
    for (int cpu = ARM; cpu <= X86; cpu++) {
        CPUType_t cpu_type = (CPUType_t)cpu;
        for (MachineId_t mid : machines_by_type[cpu]) {
            MachineInfo_t info = Machine_GetInfo(mid);
            if (info.s_state != S0) {
                Machine_SetState(mid, S0);
            }
            machines[mid].state = S0;
            
            vector<VMType_t> vm_types;
            vm_types.push_back(LINUX);
            vm_types.push_back(LINUX_RT);
            if (cpu_type == X86) vm_types.push_back(WIN);
            if (cpu_type == POWER) vm_types.push_back(AIX);
            
            for (int i = 0; i < 8; i++) {
                for (VMType_t vm_type : vm_types) {
                    if (machines[mid].memory_used + VM_MEMORY_OVERHEAD >= machines[mid].memory_total) break;
                    
                    VMId_t vm_id = VM_Create(vm_type, cpu_type);
                    VM_Attach(vm_id, mid);
                    
                    VMTracker vm_tracker;
                    vm_tracker.vm_id = vm_id;
                    vm_tracker.vm_type = vm_type;
                    vm_tracker.cpu_type = cpu_type;
                    vm_tracker.machine_id = mid;
                    vm_tracker.task_count = 0;
                    vm_tracker.is_migrating = false;
                    vm_tracker.total_task_time = 0;
                    vm_tracker.dedicated_to_long_tasks = false;
                    
                    vms[vm_id] = vm_tracker;
                    vm_pools[cpu_type][vm_type].push_back(vm_id);
                    
                    machines[mid].active_vms++;
                    machines[mid].attached_vms.push_back(vm_id);
                    machines[mid].memory_used += VM_MEMORY_OVERHEAD;
                }
            }
        }
    }
    
    SimOutput("Scheduler::Init(): Complete - work-time based load balancing", 1);
}

void Scheduler::DiscoverMachines() {
    for (unsigned i = 0; i < total_machines; i++) {
        MachineInfo_t info = Machine_GetInfo(i);
        
        MachineTracker tracker;
        tracker.machine_id = i;
        tracker.cpu_type = info.cpu;
        tracker.memory_total = info.memory_size;
        tracker.memory_used = 0;
        tracker.num_cores = info.num_cpus;
        tracker.has_gpu = info.gpus;
        tracker.state = info.s_state;
        tracker.active_vms = 0;
        tracker.active_tasks = 0;
        tracker.idle_ticks = 0;
        tracker.last_activity = 0;
        
        machines[i] = tracker;
        machines_by_type[info.cpu].push_back(i);
    }
}

Priority_t Scheduler::DeterminePriority(TaskId_t task_id) {
    SLAType_t sla = RequiredSLA(task_id);
    if (sla == SLA0) return HIGH_PRIORITY;
    if (sla == SLA1) return MID_PRIORITY;
    return LOW_PRIORITY;
}

VMId_t Scheduler::FindOrCreateVM(CPUType_t cpu_type, VMType_t vm_type, 
                                 unsigned memory_needed, bool gpu_capable,
                                 Time_t expected_runtime, SLAType_t sla_type) {
    VMId_t best_vm = static_cast<VMId_t>(-1);
    double best_score = 1e18;
    
    if (vm_pools.count(cpu_type) && vm_pools[cpu_type].count(vm_type)) {
        for (VMId_t vm_id : vm_pools[cpu_type][vm_type]) {
            VMTracker& vm = vms[vm_id];
            MachineTracker& machine = machines[vm.machine_id];
            
            if (machine.state != S0) continue;
            if (machine.memory_total - machine.memory_used < memory_needed) continue;
            if (gpu_capable && !machine.has_gpu) continue;
            
            double score = vm.total_task_time;
            
            if (sla_type == SLA0) {
                score = score;
            } else if (sla_type == SLA1) {
                score = score - 2000000.0;
            } else {
                score = score - 4000000.0;
            }
            
            if (score < best_score) {
                best_score = score;
                best_vm = vm_id;
            }
        }
    }
    
    if (best_vm != static_cast<VMId_t>(-1)) {
        return best_vm;
    }
    
    MachineId_t best_machine = static_cast<MachineId_t>(-1);
    double best_util = -1;
    
    for (MachineId_t mid : machines_by_type[cpu_type]) {
        MachineTracker& m = machines[mid];
        
        if (m.state != S0) continue;
        if (m.memory_total - m.memory_used < memory_needed + VM_MEMORY_OVERHEAD) continue;
        if (gpu_capable && !m.has_gpu) continue;
        
        double util = (double)m.memory_used / m.memory_total;
        if (util > best_util) {
            best_util = util;
            best_machine = mid;
        }
    }
    
    if (best_machine == static_cast<MachineId_t>(-1)) {
        for (MachineId_t mid : machines_by_type[cpu_type]) {
            MachineTracker& m = machines[mid];
            
            if (m.state == S0) continue;
            if (waking_machines.count(mid) > 0) continue;
            if (m.memory_total < memory_needed + VM_MEMORY_OVERHEAD) continue;
            if (gpu_capable && !m.has_gpu) continue;
            
            Machine_SetState(mid, S0);
            waking_machines.insert(mid);
            best_machine = mid;
            break;
        }
    }
    
    if (best_machine == static_cast<MachineId_t>(-1)) {
        return static_cast<VMId_t>(-1);
    }
    
    if (waking_machines.count(best_machine) > 0) {
        return static_cast<VMId_t>(-1);
    }
    
    VMId_t vm_id = VM_Create(vm_type, cpu_type);
    VM_Attach(vm_id, best_machine);
    
    VMTracker vm_tracker;
    vm_tracker.vm_id = vm_id;
    vm_tracker.vm_type = vm_type;
    vm_tracker.cpu_type = cpu_type;
    vm_tracker.machine_id = best_machine;
    vm_tracker.task_count = 0;
    vm_tracker.is_migrating = false;
    
    vms[vm_id] = vm_tracker;
    vm_pools[cpu_type][vm_type].push_back(vm_id);
    
    machines[best_machine].active_vms++;
    machines[best_machine].attached_vms.push_back(vm_id);
    machines[best_machine].memory_used += VM_MEMORY_OVERHEAD;
    
    return vm_id;
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    TaskInfo_t info = GetTaskInfo(task_id);
    Priority_t priority = DeterminePriority(task_id);
    
    VMId_t vm_id = FindOrCreateVM(info.required_cpu, info.required_vm, 
                                   info.required_memory, info.gpu_capable,
                                   info.target_completion, info.required_sla);
    
    if (vm_id == static_cast<VMId_t>(-1)) {
        pending_tasks.push_back(task_id);
        return;
    }
    
    VM_AddTask(vm_id, task_id, priority);
    
    vms[vm_id].task_count++;
    vms[vm_id].tasks.push_back(task_id);
    vms[vm_id].total_task_time += info.target_completion;
    task_to_vm[task_id] = vm_id;
    
    MachineId_t mid = vms[vm_id].machine_id;
    machines[mid].active_tasks++;
    machines[mid].memory_used += info.required_memory;
    machines[mid].last_activity = now;
    machines[mid].idle_ticks = 0;
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    if (task_to_vm.count(task_id) == 0) return;
    
    VMId_t vm_id = task_to_vm[task_id];
    task_to_vm.erase(task_id);
    
    if (vms.count(vm_id) == 0) return;
    
    VMTracker& vm = vms[vm_id];
    if (vm.task_count > 0) vm.task_count--;
    
    auto it = find(vm.tasks.begin(), vm.tasks.end(), task_id);
    if (it != vm.tasks.end()) vm.tasks.erase(it);
    
    TaskInfo_t info = GetTaskInfo(task_id);
    if (vm.total_task_time >= info.target_completion) {
        vm.total_task_time -= info.target_completion;
    }
    
    MachineId_t mid = vm.machine_id;
    if (machines[mid].active_tasks > 0) machines[mid].active_tasks--;
    
    unsigned memory = info.required_memory;
    if (machines[mid].memory_used >= memory) {
        machines[mid].memory_used -= memory;
    }
}

void Scheduler::ProcessPendingTasks(Time_t now) {
    if (pending_tasks.empty()) return;
    
    sort(pending_tasks.begin(), pending_tasks.end(), [this](TaskId_t a, TaskId_t b) {
        SLAType_t sla_a = GetTaskInfo(a).required_sla;
        SLAType_t sla_b = GetTaskInfo(b).required_sla;
        return sla_a < sla_b;
    });
    
    vector<TaskId_t> still_pending;
    
    for (TaskId_t task_id : pending_tasks) {
        if (task_to_vm.count(task_id) > 0) continue;
        
        TaskInfo_t info = GetTaskInfo(task_id);
        Priority_t priority = DeterminePriority(task_id);
        
        VMId_t vm_id = FindOrCreateVM(info.required_cpu, info.required_vm,
                                       info.required_memory, info.gpu_capable,
                                       info.target_completion, info.required_sla);
        
        if (vm_id == static_cast<VMId_t>(-1)) {
            still_pending.push_back(task_id);
            continue;
        }
        
        VM_AddTask(vm_id, task_id, priority);
        
        vms[vm_id].task_count++;
        vms[vm_id].tasks.push_back(task_id);
        vms[vm_id].total_task_time += info.target_completion;
        task_to_vm[task_id] = vm_id;
        
        MachineId_t mid = vms[vm_id].machine_id;
        machines[mid].active_tasks++;
        machines[mid].memory_used += info.required_memory;
        machines[mid].last_activity = now;
        machines[mid].idle_ticks = 0;
    }
    
    pending_tasks = still_pending;
}

void Scheduler::PeriodicCheck(Time_t now) {
    if (now - last_periodic_check < PERIODIC_CHECK_INTERVAL) return;
    
    last_periodic_check = now;
    
    if (pending_tasks.size() > 10) {
        for (int cpu = ARM; cpu <= X86; cpu++) {
            unsigned to_wake = pending_tasks.size() / 20;
            if (to_wake == 0) to_wake = 1;
            if (to_wake > 5) to_wake = 5;
            
            unsigned woken = 0;
            for (MachineId_t mid : machines_by_type[cpu]) {
                if (woken >= to_wake) break;
                if (machines[mid].state != S0 && waking_machines.count(mid) == 0) {
                    Machine_SetState(mid, S0);
                    waking_machines.insert(mid);
                    woken++;
                }
            }
        }
    }
    
    ProcessPendingTasks(now);
    
    PowerDownIdleMachines(now);
}

void Scheduler::PowerDownIdleMachines(Time_t now) {
    if (!pending_tasks.empty()) return;
    
    for (auto& pair : machines) {
        MachineId_t mid = pair.first;
        MachineTracker& m = pair.second;
        
        if (m.state != S0) continue;
        if (waking_machines.count(mid) > 0) continue;
        
        if (m.active_tasks == 0 && m.active_vms == 0) {
            m.idle_ticks++;
            
            unsigned active_count = 0;
            unsigned total_count = machines_by_type[m.cpu_type].size();
            for (MachineId_t id : machines_by_type[m.cpu_type]) {
                if (machines[id].state == S0) active_count++;
            }
            
            unsigned min_active = (total_count + 1) / 2;
            if (min_active < 3) min_active = 3;
            
            if (active_count > min_active && m.idle_ticks >= IDLE_THRESHOLD) {
                Machine_SetState(mid, S5);
                m.state = S5;
                m.idle_ticks = 0;
            }
        } else {
            m.idle_ticks = 0;
        }
    }
}

void Scheduler::UpdateMachineState(MachineId_t machine_id) {
    MachineInfo_t info = Machine_GetInfo(machine_id);
    machines[machine_id].state = info.s_state;
    machines[machine_id].memory_used = info.memory_used;
    machines[machine_id].active_tasks = info.active_tasks;
    machines[machine_id].active_vms = info.active_vms;
}

void Scheduler::StateChanged(Time_t time, MachineId_t machine_id) {
    UpdateMachineState(machine_id);
    waking_machines.erase(machine_id);
    
    if (machines[machine_id].state == S0 && !pending_tasks.empty()) {
        ProcessPendingTasks(time);
    }
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
    if (vms.count(vm_id) > 0) {
        vms[vm_id].is_migrating = false;
    }
}

void Scheduler::Shutdown(Time_t time) {
    for (auto& pair : vms) {
        VMInfo_t info = VM_GetInfo(pair.first);
        if (info.active_tasks.empty()) {
            VM_Shutdown(pair.first);
        }
    }
}

void Scheduler::LogState(const string& context, Time_t now) {
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
    SimOutput("MemoryWarning: Machine " + to_string(machine_id), 0);
}

void MigrationDone(Time_t time, VMId_t vm_id) {
    TheScheduler.MigrationComplete(time, vm_id);
}

void SchedulerCheck(Time_t time) {
    TheScheduler.PeriodicCheck(time);
}

void SimulationComplete(Time_t time) {
    cout << "=== Simulation Complete ===" << endl;
    cout << "SLA Violation Report:" << endl;
    cout << "  SLA0 (95% compliance): " << GetSLAReport(SLA0) << "%" << endl;
    cout << "  SLA1 (90% compliance): " << GetSLAReport(SLA1) << "%" << endl;
    cout << "  SLA2 (80% compliance): " << GetSLAReport(SLA2) << "%" << endl;
    cout << "Total Energy Consumed: " << Machine_GetClusterEnergy() << " KW-Hour" << endl;
    cout << "Simulation Duration: " << double(time)/1000000.0 << " seconds" << endl;
    cout << "========================" << endl;
    
    TheScheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    SetTaskPriority(task_id, HIGH_PRIORITY);
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    TheScheduler.StateChanged(time, machine_id);
}
