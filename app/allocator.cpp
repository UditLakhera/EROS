#include "allocator.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <system_error>
#include <sys/ioctl.h>
#include <unistd.h>
using namespace std;


Allocator::Allocator(bool simulation) : fd_(-1), simulation_(simulation)
{
    for (__u32 i = 0; i < EA_RESOURCE_COUNT; ++i)
        resources_[i].resource = i;
    if (!simulation_) {
        fd_ = ::open("/dev/" EA_DEVICE_NAME, O_RDWR | O_CLOEXEC);
        if (fd_ < 0)
            throw system_error(errno, generic_category(),
                                    "open /dev/" EA_DEVICE_NAME);
    }
}


Allocator::~Allocator()
{
    if (fd_ >= 0)
        ::close(fd_);
}


bool Allocator::simulated() const
{
    return simulation_;
}


void Allocator::call(unsigned long cmd, void* arg)
{
    if (!simulation_) {
        if (::ioctl(fd_, cmd, arg) < 0)
            throw system_error(errno, generic_category(), "allocator ioctl");
        return;
    }
    lock_guard<mutex> guard(mutex_);
    simulate(cmd, arg);
}


__u32 Allocator::available(__u32 resource_id) const
{
    const auto& res = resources_[resource_id];
    const __u32 unavailable = res.allocated + res.out_of_service;
    return unavailable >= res.total ? 0 : res.total - unavailable;
}


int Allocator::findPatient(__u32 id) const
{
    for (int i = 0; i < EA_MAX_PATIENTS; ++i)
        if (used_[i] && patients_[i].id == id)
            return i;
    return -1;
}


void Allocator::updateStatus(ea_patient& pat) const
{
    bool has_allocation = false;
    bool is_complete = true;
    for (int i = 0; i < EA_RESOURCE_COUNT; ++i) {
        if (pat.allocated[i] > pat.required[i])
            pat.allocated[i] = pat.required[i];
        has_allocation = has_allocation || pat.allocated[i] != 0;
        is_complete = is_complete && pat.allocated[i] == pat.required[i];
    }
    pat.status = is_complete ? EA_ALLOCATED :
                 (has_allocation ? EA_PARTIAL : EA_PENDING);
}


void Allocator::simulate(unsigned long cmd, void* arg)
{
    switch (cmd) {
    case EA_IOC_SET_CAPACITY: {
        auto& req = *static_cast<ea_capacity*>(arg);
        if (req.resource >= EA_RESOURCE_COUNT)
            throw system_error(EINVAL, generic_category(), "invalid resource");
        const auto& res = resources_[req.resource];
        if (req.total < res.allocated + res.out_of_service)
            throw system_error(EBUSY, generic_category(), "capacity below committed resources");
        resources_[req.resource].total = req.total;
        break;
    }
    case EA_IOC_SET_OUT_OF_SERVICE: {
        auto& req = *static_cast<ea_out_of_service*>(arg);
        if (req.resource >= EA_RESOURCE_COUNT)
            throw system_error(EINVAL, generic_category(), "invalid resource");
        const auto& res = resources_[req.resource];
        if (req.count > res.total - res.allocated)
            throw system_error(EBUSY, generic_category(), "out-of-service count exceeds capacity");
        resources_[req.resource].out_of_service = req.count;
        break;
    }
    case EA_IOC_ADD_PATIENT: {
        auto pat = *static_cast<ea_patient*>(arg);
        if (pat.id == 0 || pat.priority < EA_LOW || pat.priority > EA_HIGH ||
            pat.status > EA_RELEASED)
            throw system_error(EINVAL, generic_category(), "invalid patient");
        if (findPatient(pat.id) >= 0)
            throw system_error(EEXIST, generic_category(), "patient already exists");
        int slot = -1;
        for (int i = 0; i < EA_MAX_PATIENTS; ++i)
            if (!used_[i] && slot < 0)
                slot = i;
        if (slot < 0)
            throw system_error(ENOSPC, generic_category(), "patient table full");
        for (int i = 0; i < EA_RESOURCE_COUNT; ++i) {
            if (pat.allocated[i] > pat.required[i] ||
                pat.allocated[i] > available(i))
                throw system_error(ENOSPC, generic_category(), "cannot restore allocation");
        }
        if (pat.status != EA_RELEASED)
            updateStatus(pat);
        for (int i = 0; i < EA_RESOURCE_COUNT; ++i)
            resources_[i].allocated += pat.allocated[i];
        patients_[slot] = pat;
        used_[slot] = true;
        break;
    }
    case EA_IOC_SCHEDULE_NEXT: {
        auto& decision = *static_cast<ea_decision*>(arg);
        decision = {};
        int best = -1;
        __u64 best_pri = 0;
        for (int i = 0; i < EA_MAX_PATIENTS; ++i) {
            if (!used_[i] || patients_[i].status == EA_ALLOCATED ||
                patients_[i].status == EA_RELEASED)
                continue;
            bool can_allocate = false;
            for (int r = 0; r < EA_RESOURCE_COUNT; ++r)
                can_allocate = can_allocate ||
                    (patients_[i].allocated[r] < patients_[i].required[r] && available(r) > 0);
            if (!can_allocate)
                continue;
            const __u64 age = tick_ > patients_[i].arrival_tick
                ? tick_ - patients_[i].arrival_tick : 0;
            const __u64 eff_pri =
                patients_[i].priority + age / EA_AGE_STEP;
            if (best < 0 || eff_pri > best_pri ||
                (eff_pri == best_pri &&
                 patients_[i].arrival_tick < patients_[best].arrival_tick) ||
                (eff_pri == best_pri &&
                 patients_[i].arrival_tick == patients_[best].arrival_tick &&
                 patients_[i].id < patients_[best].id)) {
                best = i;
                best_pri = eff_pri;
            }
        }
        if (best >= 0) {
            auto& pat = patients_[best];
            decision.patient_id = pat.id;
            decision.effective_priority = best_pri;
            for (int r = 0; r < EA_RESOURCE_COUNT; ++r) {
                const __u32 need = pat.required[r] - pat.allocated[r];
                const __u32 give = min(need, available(r));
                pat.allocated[r] += give;
                resources_[r].allocated += give;
                decision.allocated_now[r] = give;
                decision.total_allocated[r] = pat.allocated[r];
            }
            updateStatus(pat);
            decision.status = pat.status;
        }
        break;
    }
    case EA_IOC_RELEASE: {
        const auto& req = *static_cast<ea_release*>(arg);
        const int idx = findPatient(req.patient_id);
        if (idx < 0)
            throw system_error(ENOENT, generic_category(), "patient not found");
        for (int i = 0; i < EA_RESOURCE_COUNT; ++i) {
            resources_[i].allocated -= patients_[idx].allocated[i];
            patients_[idx].allocated[i] = 0;
        }
        patients_[idx].status = EA_RELEASED;
        break;
    }
    case EA_IOC_GET_STATUS: {
        auto& req = *static_cast<ea_status*>(arg);
        const int idx = findPatient(req.patient_id);
        req.found = idx >= 0;
        req.patient = idx >= 0 ? patients_[idx] : ea_patient{};
        break;
    }
    case EA_IOC_GET_RESOURCES: {
        auto& req = *static_cast<ea_resource_status*>(arg);
        if (req.resource >= EA_RESOURCE_COUNT)
            throw system_error(EINVAL, generic_category(), "invalid resource");
        req = resources_[req.resource];
        req.available = available(req.resource);
        break;
    }
    case EA_IOC_RESET:
        patients_ = {};
        used_ = {};
        resources_ = {};
        for (__u32 i = 0; i < EA_RESOURCE_COUNT; ++i)
            resources_[i].resource = i;
        tick_ = 0;
        break;
    case EA_IOC_TICK: {
        auto& req = *static_cast<ea_tick*>(arg);
        if (req.tick > UINT64_MAX - tick_)
            throw system_error(ERANGE, generic_category(), "tick overflow");
        tick_ += req.tick;
        req.tick = tick_;
        break;
    }
    default:
        throw system_error(ENOTTY, generic_category(), "unknown allocator command");
    }
}


string statusName(__u32 status)
{
    switch (status) {
    case EA_PENDING: return "PENDING";
    case EA_PARTIAL: return "PARTIAL";
    case EA_ALLOCATED: return "ALLOCATED";
    case EA_RELEASED: return "RELEASED";
    default: return "UNKNOWN";
    }
}


string priorityName(__u32 priority)
{
    switch (priority) {
    case EA_LOW: return "LOW";
    case EA_MEDIUM: return "MEDIUM";
    case EA_HIGH: return "HIGH";
    default: return "UNKNOWN";
    }
}


string resourceName(__u32 resource)
{
    static const char* names[] = {"ambulances", "ventilators", "ICU beds", "doctors"};
    return resource < EA_RESOURCE_COUNT ? names[resource] : "unknown";
}
