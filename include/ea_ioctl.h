#ifndef EA_IOCTL_H
#define EA_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define EA_DEVICE_NAME "emergency_allocator"
#define EA_MAX_PATIENTS 64
#define EA_RESOURCE_COUNT 4
#define EA_AGE_STEP 5

enum ea_resource {
    EA_AMBULANCES = 0,
    EA_VENTILATORS = 1,
    EA_ICU_BEDS = 2,
    EA_DOCTORS = 3
};

enum ea_priority {
    EA_LOW = 1,
    EA_MEDIUM = 2,
    EA_HIGH = 3
};

enum ea_patient_status {
    EA_PENDING = 0,
    EA_PARTIAL = 1,
    EA_ALLOCATED = 2,
    EA_RELEASED = 3
};

struct ea_capacity {
    __u32 resource;
    __u32 total;
};

struct ea_out_of_service {
    __u32 resource;
    __u32 count;
};

struct ea_patient {
    __u32 id;
    __u32 priority;
    __u64 arrival_tick;
    __u32 required[EA_RESOURCE_COUNT];
    __u32 allocated[EA_RESOURCE_COUNT];
    __u32 status;
};

struct ea_decision {
    __u32 patient_id;
    __u32 status;
    __u64 effective_priority;
    __u32 allocated_now[EA_RESOURCE_COUNT];
    __u32 total_allocated[EA_RESOURCE_COUNT];
};

struct ea_release {
    __u32 patient_id;
};

struct ea_status {
    __u32 patient_id;
    __u32 found;
    struct ea_patient patient;
};

struct ea_resource_status {
    __u32 resource;
    __u32 total;
    __u32 allocated;
    __u32 out_of_service;
    __u32 available;
};

struct ea_tick {
    __u64 tick;
};

#define EA_IOC_MAGIC 'E'
#define EA_IOC_SET_CAPACITY       _IOW(EA_IOC_MAGIC, 1, struct ea_capacity)
#define EA_IOC_SET_OUT_OF_SERVICE _IOW(EA_IOC_MAGIC, 2, struct ea_out_of_service)
#define EA_IOC_ADD_PATIENT        _IOW(EA_IOC_MAGIC, 3, struct ea_patient)
#define EA_IOC_SCHEDULE_NEXT      _IOWR(EA_IOC_MAGIC, 4, struct ea_decision)
#define EA_IOC_RELEASE            _IOW(EA_IOC_MAGIC, 5, struct ea_release)
#define EA_IOC_GET_STATUS         _IOWR(EA_IOC_MAGIC, 6, struct ea_status)
#define EA_IOC_GET_RESOURCES      _IOWR(EA_IOC_MAGIC, 7, struct ea_resource_status)
#define EA_IOC_RESET              _IO(EA_IOC_MAGIC, 8)
#define EA_IOC_TICK               _IOWR(EA_IOC_MAGIC, 9, struct ea_tick)

#endif
