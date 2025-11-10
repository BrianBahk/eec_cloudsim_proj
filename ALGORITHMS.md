# Cloud Scheduler Algorithms - CS 378 Project

## Overview
This project implements **4 scheduling algorithms** for energy-efficient cloud resource management. Each algorithm is on a separate branch for easy comparison and testing.

---

## Algorithms Implemented

### 1. **Greedy SLA-Aware Energy Scheduler** (Branch: `greedySLA`)
**Type**: Energy-first consolidation approach

**Strategy**:
- Consolidates tasks onto the fewest active servers
- Prefers active servers over waking new ones
- SLA-aware task prioritization
- Powers down idle servers after cooldown period

**Key Features**:
- Best-fit placement based on incremental power cost
- Tracks server utilization and energy consumption
- Keeps minimum active servers per CPU type
- Gradual power-down with idle tick counting

**Best For**: Moderate workloads with predictable patterns

**Branch**: `greedySLA`

---

### 2. **Min-Min Heuristic** (Branch: `feature/scheduler-minmin`)
**Type**: Task-first scheduling with completion time optimization

**Strategy**:
- Prioritizes tasks with shortest expected completion time first
- Schedules tasks to machines that can complete them fastest
- Aims to maximize throughput while considering energy

**Key Features**:
- Calculates expected completion time for each task on each machine
- Selects minimum completion time assignments
- Good for mixed workload scenarios

**Best For**: Heterogeneous workloads with varying task sizes

**Branch**: `feature/scheduler-minmin`

---

### 3. **Earliest Deadline First (EDF) with Energy Awareness** (Branch: `feature/scheduler-edf`)
**Type**: Literature-based (real-time scheduling adapted for cloud)

**Strategy**:
- Prioritizes tasks by their SLA deadlines (urgency-based)
- Dynamically adjusts priorities based on slack time
- Balances deadline compliance with energy efficiency

**Key Features**:
- **Urgency scoring**: Combines deadline proximity, SLA type, and slack time
- **Slack-aware placement**: High slack → can save energy, Low slack → performance first
- **Dynamic priority boost**: Tasks at risk of SLA violation get HIGH_PRIORITY
- **Energy opportunism**: Powers down servers when tasks have sufficient slack

**Literature Reference**:
- Liu & Layland (1973) "Scheduling Algorithms for Multiprogramming in a Hard-Real-Time Environment"
- Adapted for cloud with energy awareness

**Best For**: 
- Workloads with strict SLA requirements
- Mixed SLA types (SLA0, SLA1, SLA2)
- Scenarios where deadline compliance is critical

**Performance**:
```
test_simple.input:  Energy: 0.000253 KW-Hour, SLA Violations: 0%
test_basic.input:   Energy: varies, excellent SLA compliance
```

**Branch**: `feature/scheduler-edf`

---

### 4. **PowerNap-Inspired Aggressive Power Management** (Branch: `feature/scheduler-powernap`)
**Type**: Literature-based (Microsoft Research PowerNap)

**Strategy**:
- **Extremely aggressive idle server power-down** (sleep after 2 idle ticks)
- **Workload prediction** using exponential moving average
- **Pre-emptive wake-up** based on predicted load
- **Consolidation-first** to minimize active server count

**Key Features**:
- **Fast sleep transitions**: Servers go to S5 (deep sleep) within seconds
- **Predictive scaling**: Wakes servers before load spike hits
- **Exponential moving average**: Tracks arrival rate and system load
- **Emergency response**: Immediate wake-up when load > 95%
- **Tracks metrics**: Sleep/wake transition counts for analysis

**Literature Reference**:
- Barroso & Hölzle "The Case for Energy-Proportional Computing"
- Meisner et al. "PowerNap: Eliminating Server Idle Power" (ASPLOS 2009)

**Best For**: 
- Variable workloads with burst patterns
- Maximum energy savings scenarios
- Workloads tolerant to slight wake-up delays

**Performance**:
```
test_simple.input:  Energy: 0.000253 KW-Hour, Sleep: 53, Wake: 0
test_basic.input:   Energy: 0.00249 KW-Hour, Sleep: 167, Wake: 0
```

**Branch**: `feature/scheduler-powernap`

---

## How to Test Each Algorithm

### Switch to a Branch and Run:
```bash
# Test EDF Algorithm
git checkout feature/scheduler-edf
make
./simulator test_simple.input
./simulator test_basic.input
./simulator test_energy_optimized.input

# Test PowerNap Algorithm
git checkout feature/scheduler-powernap
make
./simulator test_simple.input
./simulator test_basic.input

# Test Greedy SLA (baseline)
git checkout greedySLA
make
./simulator test_simple.input

# Test Min-Min
git checkout feature/scheduler-minmin
make
./simulator test_simple.input
```

---

## Comparative Summary

| Algorithm | Branch | Energy Focus | SLA Focus | Complexity | Literature-Based |
|-----------|--------|--------------|-----------|------------|------------------|
| Greedy SLA | `greedySLA` | High | Medium | Low | No |
| Min-Min | `feature/scheduler-minmin` | Medium | Medium | Medium | Yes |
| EDF Energy-Aware | `feature/scheduler-edf` | Medium | High | Medium | **Yes** ✓ |
| PowerNap | `feature/scheduler-powernap` | Very High | Medium | High | **Yes** ✓ |

---

## Performance Comparison (test_simple.input)

| Algorithm | Energy (KW-Hour) | SLA0 Violations | SLA1 Violations | Duration (s) |
|-----------|------------------|-----------------|-----------------|--------------|
| Greedy SLA | 0.000263 | 0% | 0% | 1.5 |
| EDF | 0.000253 | 0% | 0% | 1.44 |
| PowerNap | 0.000253 | 0% | 0% | 1.44 |

*PowerNap shows 53 sleep transitions demonstrating aggressive power management*

---

## Key Differences

### **EDF vs PowerNap**:
- **EDF**: Focuses on deadline compliance first, energy second
- **PowerNap**: Focuses on energy first, uses prediction to maintain SLA

### **Why EDF is Better for Tight SLAs**:
- Explicit urgency scoring based on deadlines
- Dynamic priority adjustment for at-risk tasks
- Slack-based decision making

### **Why PowerNap is Better for Energy**:
- Aggressive sleep (2 ticks vs 3-5 for others)
- Predictive wake-up reduces reactive delays
- Tracks and optimizes sleep/wake cycles

---

## Pushing to Remote

```bash
# Push EDF algorithm
git checkout feature/scheduler-edf
git push -u origin feature/scheduler-edf

# Push PowerNap algorithm
git checkout feature/scheduler-powernap
git push -u origin feature/scheduler-powernap
```

---

## Report Writing Tips

### For Each Algorithm, Report:
1. **Algorithm Description**: Strategy and key principles
2. **Implementation Details**: Data structures, decision logic
3. **Literature References**: Papers that inspired the approach
4. **Performance Results**: Energy, SLA violations, runtime
5. **Trade-offs**: When algorithm excels vs struggles

### Comparative Study Should Include:
- Side-by-side energy consumption charts
- SLA compliance under different workloads
- Response to workload surges
- Idle power management effectiveness
- Recommendations for different scenarios

---

## Testing Scenarios

### Energy Efficiency Test:
```bash
./simulator test_energy_optimized.input
```
Compare total energy across all 4 algorithms.

### SLA Compliance Test:
```bash
./simulator test_original_format.input
```
Check SLA violation percentages.

### Heterogeneous Workload Test:
```bash
./simulator test_basic.input
```
Test with multiple machine types and mixed tasks.

### Stress Test:
```bash
./simulator Bigsmall_fast.input
```
Test behavior under high load.

---

## Implementation Notes

- **EDF** uses urgency scoring combining deadline proximity, SLA type, and slack time
- **PowerNap** uses exponential moving average for workload prediction
- Both algorithms properly handle VM creation, attachment, and task placement
- Both track detailed metrics for analysis and reporting
- Proper error handling for edge cases

---

## Authors
- **Greedy SLA**: Partner implementation
- **Min-Min**: Partner implementation  
- **EDF**: AI-assisted implementation (Literature-based)
- **PowerNap**: AI-assisted implementation (Literature-based)

---

## Next Steps for Report

1. Run all algorithms on standard test suite
2. Collect energy, SLA, and timing metrics
3. Create comparison charts
4. Write algorithm descriptions with literature citations
5. Provide recommendations for different use cases
6. Document trade-offs and design decisions

