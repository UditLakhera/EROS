#ifndef EA_ALLOCATOR_HPP
#define EA_ALLOCATOR_HPP

#include "ea_ioctl.h"

#include <array>
#include <mutex>
#include <string>
using namespace std;

class Allocator {
public:
    explicit Allocator(bool simulation);
    ~Allocator();
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    bool simulated() const;
    void call(unsigned long cmd, void* arg = nullptr);

private:
    int fd_;
    bool simulation_;
    mutex mutex_;
    array<ea_patient, EA_MAX_PATIENTS> patients_{};
    array<bool, EA_MAX_PATIENTS> used_{};
    array<ea_resource_status, EA_RESOURCE_COUNT> resources_{};
    __u64 tick_ = 0;

    __u32 available(__u32 resource_id) const;
    int findPatient(__u32 id) const;
    void updateStatus(ea_patient& patient) const;
    void simulate(unsigned long cmd, void* arg);
};

string statusName(__u32 status);
string priorityName(__u32 priority);
string resourceName(__u32 resource);

#endif
