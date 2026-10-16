#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include "../include/ea_ioctl.h"

#define EA_DEVICE_MODE 0660

struct ea_resource_counter {
    __u32 total;
    __u32 allocated;
    __u32 out_of_service;
};

static struct ea_patient patients[EA_MAX_PATIENTS];
static bool patient_used[EA_MAX_PATIENTS];
static struct ea_resource_counter resources[EA_RESOURCE_COUNT];
static __u64 current_tick;
static DEFINE_MUTEX(ea_mutex);


static __u32 ea_available(__u32 resource)
{
    struct ea_resource_counter *counter = &resources[resource];
    __u32 unavailable = counter->allocated + counter->out_of_service;

    return unavailable >= counter->total ? 0 : counter->total - unavailable;
}


static int ea_find_patient(__u32 id)
{
    int i;

    for (i = 0; i < EA_MAX_PATIENTS; ++i)
        if (patient_used[i] && patients[i].id == id)
            return i;
    return -1;
}


static void ea_update_status(struct ea_patient *patient)
{
    bool any = false;
    bool complete = true;
    int i;

    for (i = 0; i < EA_RESOURCE_COUNT; ++i) {
        if (patient->allocated[i] > patient->required[i])
            patient->allocated[i] = patient->required[i];
        if (patient->allocated[i] != 0)
            any = true;
        if (patient->allocated[i] < patient->required[i])
            complete = false;
    }
    patient->status = complete ? EA_ALLOCATED : (any ? EA_PARTIAL : EA_PENDING);
}


static long ea_set_capacity(void __user *argument)
{
    struct ea_capacity request;

    if (copy_from_user(&request, argument, sizeof(request)))
        return -EFAULT;
    if (request.resource >= EA_RESOURCE_COUNT)
        return -EINVAL;
    if (request.total < resources[request.resource].allocated +
                        resources[request.resource].out_of_service)
        return -EBUSY;
    resources[request.resource].total = request.total;
    return 0;
}


static long ea_set_out_of_service(void __user *argument)
{
    struct ea_out_of_service request;

    if (copy_from_user(&request, argument, sizeof(request)))
        return -EFAULT;
    if (request.resource >= EA_RESOURCE_COUNT)
        return -EINVAL;
    if (request.count > resources[request.resource].total -
                        resources[request.resource].allocated)
        return -EBUSY;
    resources[request.resource].out_of_service = request.count;
    return 0;
}


static long ea_add_patient(void __user *argument)
{
    struct ea_patient request;
    int free_slot = -1;
    int i;

    if (copy_from_user(&request, argument, sizeof(request)))
        return -EFAULT;
    if (request.id == 0 || request.priority < EA_LOW || request.priority > EA_HIGH ||
        request.status > EA_RELEASED)
        return -EINVAL;
    if (ea_find_patient(request.id) >= 0)
        return -EEXIST;
    for (i = 0; i < EA_MAX_PATIENTS; ++i) {
        if (!patient_used[i] && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return -ENOSPC;
    for (i = 0; i < EA_RESOURCE_COUNT; ++i) {
        if (request.allocated[i] > request.required[i] ||
            request.allocated[i] > ea_available(i) ||
            (request.status == EA_RELEASED && request.allocated[i] != 0))
            return -ENOSPC;
    }
    if (request.status != EA_RELEASED)
        ea_update_status(&request);
    for (i = 0; i < EA_RESOURCE_COUNT; ++i)
        resources[i].allocated += request.allocated[i];
    patients[free_slot] = request;
    patient_used[free_slot] = true;
    return 0;
}


static long ea_schedule_next(void __user *argument)
{
    struct ea_decision decision = { };
    struct ea_patient *selected = NULL;
    struct ea_patient updated;
    __u64 selected_priority = 0;
    int i;

    for (i = 0; i < EA_MAX_PATIENTS; ++i) {
        __u64 age;
        __u64 effective;
        bool can_progress = false;
        int r;

        if (!patient_used[i] || patients[i].status == EA_ALLOCATED ||
            patients[i].status == EA_RELEASED)
            continue;
        for (r = 0; r < EA_RESOURCE_COUNT; ++r)
            if (patients[i].allocated[r] < patients[i].required[r] &&
                ea_available(r) > 0)
                can_progress = true;
        if (!can_progress)
            continue;
        age = current_tick > patients[i].arrival_tick ?
              current_tick - patients[i].arrival_tick : 0;
        effective = patients[i].priority + age / EA_AGE_STEP;
        if (!selected || effective > selected_priority ||
            (effective == selected_priority &&
             patients[i].arrival_tick < selected->arrival_tick) ||
            (effective == selected_priority &&
             patients[i].arrival_tick == selected->arrival_tick &&
             patients[i].id < selected->id)) {
            selected = &patients[i];
            selected_priority = effective;
        }
    }
    if (selected) {
        decision.patient_id = selected->id;
        decision.effective_priority = selected_priority;
        updated = *selected;
        for (i = 0; i < EA_RESOURCE_COUNT; ++i) {
            __u32 needed = updated.required[i] - updated.allocated[i];
            __u32 amount = min(needed, ea_available(i));

            updated.allocated[i] += amount;
            decision.allocated_now[i] = amount;
            decision.total_allocated[i] = updated.allocated[i];
        }
        ea_update_status(&updated);
        decision.status = updated.status;
    }
    if (copy_to_user(argument, &decision, sizeof(decision)))
        return -EFAULT;
    if (selected) {
        for (i = 0; i < EA_RESOURCE_COUNT; ++i)
            resources[i].allocated += decision.allocated_now[i];
        *selected = updated;
    }
    return 0;
}


static long ea_release_patient(void __user *argument)
{
    struct ea_release request;
    int index;
    int i;

    if (copy_from_user(&request, argument, sizeof(request)))
        return -EFAULT;
    index = ea_find_patient(request.patient_id);
    if (index < 0)
        return -ENOENT;
    for (i = 0; i < EA_RESOURCE_COUNT; ++i) {
        resources[i].allocated -= patients[index].allocated[i];
        patients[index].allocated[i] = 0;
    }
    patients[index].status = EA_RELEASED;
    return 0;
}


static long ea_get_status(void __user *argument)
{
    struct ea_status request;
    int index;

    if (copy_from_user(&request, argument, sizeof(request)))
        return -EFAULT;
    index = ea_find_patient(request.patient_id);
    request.found = index >= 0;
    if (request.found)
        request.patient = patients[index];
    else
        memset(&request.patient, 0, sizeof(request.patient));
    if (copy_to_user(argument, &request, sizeof(request)))
        return -EFAULT;
    return 0;
}


static long ea_get_resources(void __user *argument)
{
    struct ea_resource_status request;

    if (copy_from_user(&request, argument, sizeof(request)))
        return -EFAULT;
    if (request.resource >= EA_RESOURCE_COUNT)
        return -EINVAL;
    request.total = resources[request.resource].total;
    request.allocated = resources[request.resource].allocated;
    request.out_of_service = resources[request.resource].out_of_service;
    request.available = ea_available(request.resource);
    if (copy_to_user(argument, &request, sizeof(request)))
        return -EFAULT;
    return 0;
}


static long ea_reset(void)
{
    memset(patients, 0, sizeof(patients));
    memset(patient_used, 0, sizeof(patient_used));
    memset(resources, 0, sizeof(resources));
    current_tick = 0;
    return 0;
}


static long ea_tick(void __user *argument)
{
    struct ea_tick request;

    if (copy_from_user(&request, argument, sizeof(request)))
        return -EFAULT;
    if (U64_MAX - current_tick < request.tick)
        return -ERANGE;
    request.tick += current_tick;
    if (copy_to_user(argument, &request, sizeof(request)))
        return -EFAULT;
    current_tick = request.tick;
    return 0;
}


static long ea_ioctl(struct file *file, unsigned int command, unsigned long value)
{
    void __user *argument = (void __user *)value;
    long result;

    mutex_lock(&ea_mutex);
    switch (command) {
    case EA_IOC_SET_CAPACITY:
        result = ea_set_capacity(argument);
        break;
    case EA_IOC_SET_OUT_OF_SERVICE:
        result = ea_set_out_of_service(argument);
        break;
    case EA_IOC_ADD_PATIENT:
        result = ea_add_patient(argument);
        break;
    case EA_IOC_SCHEDULE_NEXT:
        result = ea_schedule_next(argument);
        break;
    case EA_IOC_RELEASE:
        result = ea_release_patient(argument);
        break;
    case EA_IOC_GET_STATUS:
        result = ea_get_status(argument);
        break;
    case EA_IOC_GET_RESOURCES:
        result = ea_get_resources(argument);
        break;
    case EA_IOC_RESET:
        result = ea_reset();
        break;
    case EA_IOC_TICK:
        result = ea_tick(argument);
        break;
    default:
        result = -ENOTTY;
    }
    mutex_unlock(&ea_mutex);
    return result;
}

static const struct file_operations ea_file_operations = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = ea_ioctl,
};

static struct miscdevice ea_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = EA_DEVICE_NAME,
    .fops = &ea_file_operations,
    .mode = EA_DEVICE_MODE,
};


static int __init ea_init(void)
{
    return misc_register(&ea_device);
}


static void __exit ea_exit(void)
{
    misc_deregister(&ea_device);
}

module_init(ea_init);
module_exit(ea_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Emergency Resource Allocation System");
MODULE_DESCRIPTION("Fixed-array emergency resource scheduler");
