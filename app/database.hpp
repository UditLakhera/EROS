#ifndef EA_DATABASE_HPP
#define EA_DATABASE_HPP

#include "ea_ioctl.h"

#include <array>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std;

struct PatientInput {
    string name;
    string phone;
    string location;
    int age;
    __u32 priority;
    array<__u32, EA_RESOURCE_COUNT> required;
};

struct PatientRecord {
    ea_patient kernel{};
    string name;
    string phone;
    int age = 0;
    string location;
};

class DuplicatePatient : public runtime_error {
public:
    DuplicatePatient();
};

class Database {
public:
    explicit Database(const string& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void initialize();
    bool authenticate(const string& username, const string& password);
    void setActiveHospital(const string& username);
    bool createAdmin(const string& username, const string& password,
                     const array<__u32, EA_RESOURCE_COUNT>& capacities,
                     const function<void(__u32, __u32)>& applyKernelCapacity);
    __u32 addPatient(const PatientInput& input, __u64 arrivalTick);
    void persistDecision(const ea_decision& decision);
    void releasePatient(__u32 patientId);
    void setCapacity(__u32 resource, __u32 total,
                     const function<void()>& applyKernel);
    void setOutOfService(__u32 resource, __u32 count,
                         const function<void()>& applyKernel);
    void addTicks(__u64 amount, const function<void()>& applyKernel);
    __u64 currentTick() const;
    array<ea_resource_status, EA_RESOURCE_COUNT> resourceSnapshot() const;
    vector<PatientRecord> patientSnapshot() const;
    vector<string> historySnapshot() const;
    void removePatient(__u32 patientId);

private:
    struct sqlite3* db_;
    string active_hospital_ = "default";
    void execute(const string& sql) const;
    bool tableExists(const string& table) const;
    bool columnExists(const string& table, const string& column) const;
    void createHospitalTables();
    void migrateLegacyTables();
    void ensureHospitalResources(const string& hospital);
    void recomputeAllocated() const;
    void seedAdmin(const string& username, const string& password);
    void setMeta(const string& key, uint64_t value) const;
};

#endif
