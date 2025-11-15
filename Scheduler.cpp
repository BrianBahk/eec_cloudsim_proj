#include "Scheduler.hpp"
#include <algorithm>

#define IDLE_THRESHOLD 50
#define PERIODIC_CHECK_INTERVAL 1000000

void Scheduler::Init() {
    SimOutput("Scheduler::Init(): DVFS-Aware scheduler initialization", 1);
    
    total_machines = Machine_GetTotal();
    last_periodic_check = 0;
    
    DiscoverMachines();
    
    for (int cpu = ARM; cpu <= X86; cpu++) {
        if (machines_by_type[cpu].empty()) continue;
        
        unsigned total = machines_by_type[cpu].size();
        unsigned num_to_keep = (total + 1) / 2;
        if (num_to_keep < 3) num_to_keep = min(3u, total);
        
        for (unsigned i = 0; i < num_to_keep && i < total; i++) {
            MachineId_t mid = machines_by_type[cpu][i];
            machines[mid].s_state = S0;
            machines[mid].p_state = P3;
            machines[mid].highest_sla_on_machine = SLA3;
            Machine_SetCorePerformance(mid, 0, P3);
        }
        
        for (unsigned i = num_to_keep; i < total; i++) {
            MachineId_t mid = machines_by_type[cpu][i];
            Machine_SetState(mid, S5);
            machines[mid].s_state = S5;
        }
    }
    
    SimOutput("Scheduler::Init(): Complete - machines initialized with P-state management", 1);
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
        tracker.s_state = info.s_state;
        tracker.p_state = info.p_state;
        tracker.active_vms = 0;
        tracker.active_tasks = 0;
        tracker.idle_ticks = 0;
        tracker.highest_sla_on_machine = SLA3;
        
        machines[i] = tracker;
        machines_by_type[info.cpu].push_back(i);
    }
}

CPUPerformance_t Scheduler::SLAToPState(SLAType_t sla) {
    switch(sla) {
        case SLA0: return P0;
        case SLA1: return P1;
        case SLA2: return P2;
        case SLA3: return P3;
        default: return P2;
    }
}

Priority_t Scheduler::DeterminePriority(TaskId_t task_id) {
    SLAType_t sla = RequiredSLA(task_id);
    if (sla == SLA0) return HIGH_PRIORITY;
    if (sla == SLA1) return MID_PRIORITY;
    return LOW_PRIORITY;
}

void Scheduler::UpdateMachinePState(MachineId_t machine_id) {
    MachineTracker& machine = machines[machine_id];
    
    if (machine.s_state != S0) return;
    
    if (machine.active_tasks == 0) {
        if (machine.p_state != P3) {
            Machine_SetCorePerformance(machine_id, 0, P3);
            machine.p_state = P3;
            machine.highest_sla_on_machine = SLA3;
        }
        return;
    }
    
    SLAType_t highest_sla = SLA3;
    
    for (VMId_t vm_id : machine.attached_vms) {
        if (vms.count(vm_id) == 0) continue;
        
        for (TaskId_t task_id : vms[vm_id].tasks) {
            if (task_sla_map.count(task_id) > 0) {
                SLAType_t task_sla = task_sla_map[task_id];
                if (task_sla < highest_sla) {
                    highest_sla = task_sla;
                    if (highest_sla == SLA0) break;
                }
            }
        }
        if (highest_sla == SLA0) break;
    }
    
    CPUPerformance_t target_pstate = SLAToPState(highest_sla);
    
    if (machine.p_state != target_pstate) {
        Machine_SetCorePerformance(machine_id, 0, target_pstate);
        machine.p_state = target_pstate;
        machine.highest_sla_on_machine = highest_sla;
    }
}

MachineId_t Scheduler::FindBestMachine(CPUType_t cpu_type, unsigned memory_needed, bool gpu_capable) {
    MachineId_t best_machine = static_cast<MachineId_t>(-1);
    double best_score = -1;
    
    for (MachineId_t mid : machines_by_type[cpu_type]) {
        MachineTracker& m = machines[mid];
        
        if (m.s_state != S0) continue;
        if (m.memory_total - m.memory_used < memory_needed + VM_MEMORY_OVERHEAD) continue;
        if (gpu_capable && !m.has_gpu) continue;
        
        double utilization = (double)m.memory_used / m.memory_total;
        double score = utilization;
        
        if (m.active_vms > 0) score += 0.3;
        if (gpu_capable && m.has_gpu) score += 0.2;
        
        if (score > best_score) {
            best_score = score;
            best_machine = mid;
        }
    }
    
    if (best_machine == static_cast<MachineId_t>(-1)) {
        for (MachineId_t mid : machines_by_type[cpu_type]) {
            MachineTracker& m = machines[mid];
            
            if (m.s_state == S0) continue;
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
        return static_cast<MachineId_t>(-1);
    }
    
    if (waking_machines.count(best_machine) > 0) {
        return static_cast<MachineId_t>(-1);
    }
    
    return best_machine;
}

VMId_t Scheduler::FindOrCreateVM(CPUType_t cpu_type, VMType_t vm_type, 
                                 unsigned memory_needed, bool gpu_capable) {
    if (vm_pools.count(cpu_type) && vm_pools[cpu_type].count(vm_type)) {
        for (VMId_t vm_id : vm_pools[cpu_type][vm_type]) {
            VMTracker& vm = vms[vm_id];
            MachineTracker& machine = machines[vm.machine_id];
            
            if (machine.s_state != S0) continue;
            if (machine.memory_total - machine.memory_used < memory_needed) continue;
            if (gpu_capable && !machine.has_gpu) continue;
            
            return vm_id;
        }
    }
    
    MachineId_t machine_id = FindBestMachine(cpu_type, memory_needed, gpu_capable);
    
    if (machine_id == static_cast<MachineId_t>(-1)) {
        return static_cast<VMId_t>(-1);
    }
    
    VMId_t vm_id = VM_Create(vm_type, cpu_type);
    VM_Attach(vm_id, machine_id);
    
    VMTracker vm_tracker;
    vm_tracker.vm_id = vm_id;
    vm_tracker.vm_type = vm_type;
    vm_tracker.cpu_type = cpu_type;
    vm_tracker.machine_id = machine_id;
    vm_tracker.task_count = 0;
    
    vms[vm_id] = vm_tracker;
    vm_pools[cpu_type][vm_type].push_back(vm_id);
    
    machines[machine_id].active_vms++;
    machines[machine_id].attached_vms.push_back(vm_id);
    machines[machine_id].memory_used += VM_MEMORY_OVERHEAD;
    
    return vm_id;
}

void Scheduler::NewTask(Time_t now, TaskId_t task_id) {
    TaskInfo_t info = GetTaskInfo(task_id);
    SLAType_t sla = info.required_sla;
    Priority_t priority = DeterminePriority(task_id);
    
    task_sla_map[task_id] = sla;
    
    VMId_t vm_id = FindOrCreateVM(info.required_cpu, info.required_vm, 
                                   info.required_memory, info.gpu_capable);
    
    if (vm_id == static_cast<VMId_t>(-1)) {
        pending_tasks.push_back(task_id);
        
        if (pending_tasks.size() >= 2) {
            unsigned to_wake = 1 + (pending_tasks.size() / 10);
            if (to_wake > 3) to_wake = 3;
            
            unsigned woken = 0;
            for (MachineId_t mid : machines_by_type[info.required_cpu]) {
                if (woken >= to_wake) break;
                if (machines[mid].s_state != S0 && waking_machines.count(mid) == 0) {
                    Machine_SetState(mid, S0);
                    waking_machines.insert(mid);
                    woken++;
                }
            }
        }
        return;
    }
    
    VM_AddTask(vm_id, task_id, priority);
    
    vms[vm_id].task_count++;
    vms[vm_id].tasks.push_back(task_id);
    task_to_vm[task_id] = vm_id;
    
    MachineId_t mid = vms[vm_id].machine_id;
    machines[mid].active_tasks++;
    machines[mid].memory_used += info.required_memory;
    machines[mid].idle_ticks = 0;
    
    UpdateMachinePState(mid);
}

void Scheduler::TaskComplete(Time_t now, TaskId_t task_id) {
    if (task_to_vm.count(task_id) == 0) return;
    
    VMId_t vm_id = task_to_vm[task_id];
    task_to_vm.erase(task_id);
    task_sla_map.erase(task_id);
    
    if (vms.count(vm_id) == 0) return;
    
    VMTracker& vm = vms[vm_id];
    if (vm.task_count > 0) vm.task_count--;
    
    auto it = find(vm.tasks.begin(), vm.tasks.end(), task_id);
    if (it != vm.tasks.end()) vm.tasks.erase(it);
    
    MachineId_t mid = vm.machine_id;
    if (machines[mid].active_tasks > 0) machines[mid].active_tasks--;
    
    unsigned memory = GetTaskInfo(task_id).required_memory;
    if (machines[mid].memory_used >= memory) {
        machines[mid].memory_used -= memory;
    }
    
    UpdateMachinePState(mid);
}

void Scheduler::ProcessPendingTasks(Time_t now) {
    if (pending_tasks.empty()) return;
    
    vector<TaskId_t> still_pending;
    
    for (TaskId_t task_id : pending_tasks) {
        if (task_to_vm.count(task_id) > 0) continue;
        
        TaskInfo_t info = GetTaskInfo(task_id);
        Priority_t priority = DeterminePriority(task_id);
        
        VMId_t vm_id = FindOrCreateVM(info.required_cpu, info.required_vm,
                                       info.required_memory, info.gpu_capable);
        
        if (vm_id == static_cast<VMId_t>(-1)) {
            still_pending.push_back(task_id);
            continue;
        }
        
        VM_AddTask(vm_id, task_id, priority);
        
        vms[vm_id].task_count++;
        vms[vm_id].tasks.push_back(task_id);
        task_to_vm[task_id] = vm_id;
        task_sla_map[task_id] = info.required_sla;
        
        MachineId_t mid = vms[vm_id].machine_id;
        machines[mid].active_tasks++;
        machines[mid].memory_used += info.required_memory;
        machines[mid].idle_ticks = 0;
        
        UpdateMachinePState(mid);
    }
    
    pending_tasks = still_pending;
}

void Scheduler::PeriodicCheck(Time_t now) {
    if (now - last_periodic_check < PERIODIC_CHECK_INTERVAL) return;
    
    last_periodic_check = now;
    
    if (pending_tasks.size() > 10) {
        for (int cpu = ARM; cpu <= X86; cpu++) {
            unsigned to_wake = 1;
            unsigned woken = 0;
            for (MachineId_t mid : machines_by_type[cpu]) {
                if (woken >= to_wake) break;
                if (machines[mid].s_state != S0 && waking_machines.count(mid) == 0) {
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
        
        if (m.s_state != S0) continue;
        if (waking_machines.count(mid) > 0) continue;
        
        if (m.active_tasks == 0 && m.active_vms == 0) {
            m.idle_ticks++;
            
            unsigned active_count = 0;
            unsigned total_count = machines_by_type[m.cpu_type].size();
            for (MachineId_t id : machines_by_type[m.cpu_type]) {
                if (machines[id].s_state == S0) active_count++;
            }
            
            unsigned min_active = (total_count + 1) / 2;
            if (min_active < 3) min_active = 3;
            
            if (active_count > min_active && m.idle_ticks >= IDLE_THRESHOLD) {
                Machine_SetState(mid, S5);
                m.s_state = S5;
                m.idle_ticks = 0;
            }
        } else {
            m.idle_ticks = 0;
        }
    }
}

void Scheduler::UpdateMachineState(MachineId_t machine_id) {
    MachineInfo_t info = Machine_GetInfo(machine_id);
    machines[machine_id].s_state = info.s_state;
    machines[machine_id].p_state = info.p_state;
    machines[machine_id].memory_used = info.memory_used;
    machines[machine_id].active_tasks = info.active_tasks;
    machines[machine_id].active_vms = info.active_vms;
}

void Scheduler::StateChanged(Time_t time, MachineId_t machine_id) {
    UpdateMachineState(machine_id);
    waking_machines.erase(machine_id);
    
    if (machines[machine_id].s_state == S0) {
        machines[machine_id].p_state = P3;
        Machine_SetCorePerformance(machine_id, 0, P3);
        
        if (!pending_tasks.empty()) {
            ProcessPendingTasks(time);
        }
    }
}

void Scheduler::MigrationComplete(Time_t time, VMId_t vm_id) {
}

void Scheduler::Shutdown(Time_t time) {
    for (auto& pair : vms) {
        VMInfo_t info = VM_GetInfo(pair.first);
        if (info.active_tasks.empty()) {
            VM_Shutdown(pair.first);
        }
    }
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
    cout << "=== DVFS-Aware Scheduler Results ===" << endl;
    cout << "SLA Violation Report:" << endl;
    cout << "  SLA0 (95% compliance): " << GetSLAReport(SLA0) << "%" << endl;
    cout << "  SLA1 (90% compliance): " << GetSLAReport(SLA1) << "%" << endl;
    cout << "  SLA2 (80% compliance): " << GetSLAReport(SLA2) << "%" << endl;
    cout << "Total Energy Consumed: " << Machine_GetClusterEnergy() << " KW-Hour" << endl;
    cout << "Simulation Duration: " << double(time)/1000000.0 << " seconds" << endl;
    cout << "====================================" << endl;
    
    TheScheduler.Shutdown(time);
}

void SLAWarning(Time_t time, TaskId_t task_id) {
    SetTaskPriority(task_id, HIGH_PRIORITY);
}

void StateChangeComplete(Time_t time, MachineId_t machine_id) {
    TheScheduler.StateChanged(time, machine_id);
}
