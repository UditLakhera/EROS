#include "allocator.hpp"
#include "database.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <sqlite3.h>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>
using namespace std;

namespace {


void check(bool condition, const string& message)
{
    if (!condition)
        throw runtime_error(message);
}


void reset(Allocator& allocator)
{
    allocator.call(EA_IOC_RESET);
}


void capacity(Allocator& allocator, __u32 resource, __u32 total)
{
    ea_capacity request{resource, total};
    allocator.call(EA_IOC_SET_CAPACITY, &request);
}


void patient(Allocator& allocator, __u32 id, __u32 priority,
             __u64 arrival, __u32 ambulances = 1)
{
    ea_patient request{};
    request.id = id;
    request.priority = priority;
    request.arrival_tick = arrival;
    request.required[EA_AMBULANCES] = ambulances;
    request.status = EA_PENDING;
    allocator.call(EA_IOC_ADD_PATIENT, &request);
}


ea_decision schedule(Allocator& allocator)
{
    ea_decision decision{};
    allocator.call(EA_IOC_SCHEDULE_NEXT, &decision);
    return decision;
}


ea_patient status(Allocator& allocator, __u32 id)
{
    ea_status result{};
    result.patient_id = id;
    allocator.call(EA_IOC_GET_STATUS, &result);
    check(result.found != 0, "patient missing from allocator");
    return result.patient;
}


void testPriority(Allocator& allocator)
{
    reset(allocator);
    capacity(allocator, EA_AMBULANCES, 3);
    patient(allocator, 30, EA_LOW, 0);
    patient(allocator, 20, EA_MEDIUM, 0);
    patient(allocator, 10, EA_HIGH, 0);
    check(schedule(allocator).patient_id == 10, "HIGH patient was not first");
    check(schedule(allocator).patient_id == 20, "MEDIUM patient was not second");
    check(schedule(allocator).patient_id == 30, "LOW patient was not third");
}


void testAgeing(Allocator& allocator)
{
    reset(allocator);
    capacity(allocator, EA_AMBULANCES, 2);
    patient(allocator, 1, EA_LOW, 0);
    ea_tick advance{8};
    allocator.call(EA_IOC_TICK, &advance);
    patient(allocator, 2, EA_MEDIUM, advance.tick);
    ea_tick more{2};
    allocator.call(EA_IOC_TICK, &more);
    check(schedule(allocator).patient_id == 1, "ageing did not promote the older LOW patient");
}


void testTieBreak(Allocator& allocator)
{
    reset(allocator);
    capacity(allocator, EA_AMBULANCES, 2);
    patient(allocator, 1, EA_MEDIUM, 5);
    patient(allocator, 2, EA_MEDIUM, 2);
    check(schedule(allocator).patient_id == 2, "arrival-time tie-break was incorrect");
}


void testConcurrentDuplicate()
{
    const auto path = filesystem::temp_directory_path() /
        ("ea_duplicate_" + to_string(::getpid()) + ".db");
    filesystem::remove(path);
    {
        Database setup(path.string());
        setup.initialize();
    }
    PatientInput input{"Concurrent Patient", "ID-CONCURRENT-1", "Ward A", 31,
                       EA_HIGH, {1, 0, 0, 1}};
    atomic<bool> start{false};
    atomic<int> inserted{0};
    atomic<int> duplicates{0};
    atomic<int> unexpected{0};
    vector<thread> workers;
    for (int i = 0; i < 2; ++i) {
        workers.emplace_back([&] {
            while (!start.load())
                this_thread::yield();
            try {
                Database database(path.string());
                database.addPatient(input, 0);
                ++inserted;
            } catch (const DuplicatePatient&) {
                ++duplicates;
            } catch (...) {
                ++unexpected;
            }
        });
    }
    start.store(true);
    for (auto& worker : workers)
        worker.join();
    Database result(path.string());
    check(inserted == 1 && duplicates == 1 && unexpected == 0,
          "concurrent registration did not produce one insert and one conflict");
    check(result.patientSnapshot().size() == 1, "duplicate patient row was created");
    filesystem::remove(path);
}


void testCapacityEnforcement(Allocator& allocator)
{
    reset(allocator);
    capacity(allocator, EA_AMBULANCES, 1);
    patient(allocator, 1, EA_HIGH, 0);
    patient(allocator, 2, EA_MEDIUM, 0);
    check(schedule(allocator).patient_id == 1, "first patient was not allocated");
    check(schedule(allocator).patient_id == 0, "scheduler exceeded available capacity");
    ea_resource_status resource{};
    resource.resource = EA_AMBULANCES;
    allocator.call(EA_IOC_GET_RESOURCES, &resource);
    check(resource.available == 0 && resource.allocated == 1,
          "resource counters violate capacity");
    bool rejected = false;
    try {
        capacity(allocator, EA_AMBULANCES, 0);
    } catch (const system_error&) {
        rejected = true;
    }
    check(rejected, "capacity below allocated resources was accepted");
}


void testConcurrentExclusivity(Allocator& allocator)
{
    reset(allocator);
    capacity(allocator, EA_AMBULANCES, 1);
    constexpr int count = 12;
    for (int i = 1; i <= count; ++i)
        patient(allocator, i, EA_HIGH, 0);
    vector<thread> workers;
    for (int i = 0; i < count; ++i)
        workers.emplace_back([&] { (void)schedule(allocator); });
    for (auto& worker : workers)
        worker.join();
    uint32_t allocated = 0;
    for (int i = 1; i <= count; ++i)
        allocated += status(allocator, i).allocated[EA_AMBULANCES];
    check(allocated == 1, "concurrent schedulers allocated the same resource more than once");
}


void testPartialThenCompletion(Allocator& allocator)
{
    reset(allocator);
    capacity(allocator, EA_AMBULANCES, 2);
    patient(allocator, 1, EA_HIGH, 0, 1);
    patient(allocator, 2, EA_MEDIUM, 1, 2);
    check(schedule(allocator).status == EA_ALLOCATED, "first patient's allocation failed");
    check(schedule(allocator).status == EA_PARTIAL, "second patient was not partially allocated");
    ea_release release{1};
    allocator.call(EA_IOC_RELEASE, &release);
    const auto completion = schedule(allocator);
    check(completion.patient_id == 2 && completion.status == EA_ALLOCATED,
          "partial patient did not complete after capacity was released");
}


void testRecovery(Allocator& allocator)
{
    reset(allocator);
    const auto path = filesystem::temp_directory_path() /
        ("ea_recovery_" + to_string(::getpid()) + ".db");
    filesystem::remove(path);
    {
        Database database(path.string());
        database.initialize();
        ea_capacity capacityRequest{EA_AMBULANCES, 1};
        database.setCapacity(EA_AMBULANCES, 1, [&] {
            allocator.call(EA_IOC_SET_CAPACITY, &capacityRequest);
        });
        ea_capacity doctorCapacity{EA_DOCTORS, 1};
        database.setCapacity(EA_DOCTORS, 1, [&] {
            allocator.call(EA_IOC_SET_CAPACITY, &doctorCapacity);
        });
        PatientInput input{"Recovery Patient", "ID-RECOVERY-1", "Ward B", 46,
                           EA_HIGH, {2, 0, 0, 1}};
        const auto id = database.addPatient(input, 0);
        ea_patient request{};
        request.id = id;
        request.priority = input.priority;
        request.required[EA_AMBULANCES] = 2;
        request.required[EA_DOCTORS] = 1;
        allocator.call(EA_IOC_ADD_PATIENT, &request);
        const auto decision = schedule(allocator);
        check(decision.status == EA_PARTIAL, "recovery fixture was not partial");
        database.persistDecision(decision);

        reset(allocator);
        ea_tick tick{database.currentTick()};
        allocator.call(EA_IOC_TICK, &tick);
        for (const auto& resource : database.resourceSnapshot()) {
            ea_capacity capacityValue{resource.resource, resource.total};
            allocator.call(EA_IOC_SET_CAPACITY, &capacityValue);
            ea_out_of_service unavailable{resource.resource, resource.out_of_service};
            allocator.call(EA_IOC_SET_OUT_OF_SERVICE, &unavailable);
        }
        for (const auto& record : database.patientSnapshot()) {
            auto restored = record.kernel;
            allocator.call(EA_IOC_ADD_PATIENT, &restored);
        }
        check(status(allocator, id).status == EA_PARTIAL &&
              status(allocator, id).allocated[EA_AMBULANCES] == 1,
              "replayed kernel state did not match SQLite");

        ea_capacity increase{EA_AMBULANCES, 2};
        database.setCapacity(EA_AMBULANCES, 2, [&] {
            allocator.call(EA_IOC_SET_CAPACITY, &increase);
        });
        const auto completed = schedule(allocator);
        check(completed.patient_id == id && completed.status == EA_ALLOCATED,
              "recovered partial patient did not resume scheduling");
        database.persistDecision(completed);
    }
    filesystem::remove(path);
}


void testAdminCreation(Allocator& allocator)
{
    reset(allocator);
    const auto path = filesystem::temp_directory_path() /
        ("ea_admin_" + to_string(::getpid()) + ".db");
    filesystem::remove(path);
    {
        Database database(path.string());
        database.initialize();
        const auto emptyResources = database.resourceSnapshot();
        for (const auto& resource : emptyResources)
            check(resource.total == 0, "new database resource capacity was not zero");

        const array<__u32, EA_RESOURCE_COUNT> totals{3, 4, 2, 5};
        const bool created = database.createAdmin(
            "new-admin", "secret-pass", totals, [&](const __u32 resource, const __u32 total) {
                ea_capacity request{resource, total};
                allocator.call(EA_IOC_SET_CAPACITY, &request);
            });
        check(created, "new administrator was not created");
        check(database.authenticate("new-admin", "secret-pass"),
              "new admin credentials did not authenticate");
        database.setActiveHospital("new-admin");
        const auto savedResources = database.resourceSnapshot();
        for (__u32 resource = 0; resource < EA_RESOURCE_COUNT; ++resource)
            check(savedResources[resource].total == totals[resource],
                  "admin capacity was not persisted");
        PatientInput patientInput{"Same Patient", "HOSPITAL-SHARED-ID", "Ward C", 25,
                                  EA_MEDIUM, {0, 0, 0, 0}};
        const auto firstHospitalPatient = database.addPatient(patientInput, 0);
        database.setActiveHospital("default");
        check(database.resourceSnapshot()[EA_AMBULANCES].total == 0,
              "new hospital capacity leaked into another hospital");
        const auto secondHospitalPatient = database.addPatient(patientInput, 0);
        check(firstHospitalPatient != secondHospitalPatient,
              "patient IDs were not distinct between hospitals");
        check(database.patientSnapshot().size() == 1,
              "default hospital can view another hospital's patients");
        check(!database.createAdmin("new-admin", "another-pass", totals,
                                    [](const __u32, const __u32) {}),
              "duplicate admin username was accepted");
    }
    {
        Database reopened(path.string());
        check(reopened.authenticate("new-admin", "secret-pass"),
              "new admin login did not persist after reopening database");
        reopened.setActiveHospital("new-admin");
        const auto savedResources = reopened.resourceSnapshot();
        check(savedResources[EA_AMBULANCES].total == 3 &&
              savedResources[EA_VENTILATORS].total == 4 &&
              savedResources[EA_ICU_BEDS].total == 2 &&
              savedResources[EA_DOCTORS].total == 5,
              "resource capacities did not persist after reopening database");
        check(reopened.patientSnapshot().size() == 1,
              "hospital patient list did not persist or was not isolated");
        reopened.setActiveHospital("dispatcher");
        check(reopened.patientSnapshot().empty() &&
              reopened.resourceSnapshot()[EA_AMBULANCES].total == 0,
              "second seeded hospital did not have isolated empty data");
    }
    filesystem::remove(path);
}


void testLegacyMigration()
{
    const auto path = filesystem::temp_directory_path() /
        ("ea_legacy_" + to_string(::getpid()) + ".db");
    filesystem::remove(path);
    sqlite3* legacy = nullptr;
    check(sqlite3_open(path.string().c_str(), &legacy) == SQLITE_OK,
          "could not create migration fixture");
    const char* schema =
        "CREATE TABLE patients (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, "
        "phone TEXT NOT NULL, age INTEGER NOT NULL, location TEXT NOT NULL, priority INTEGER NOT NULL, "
        "arrival_tick INTEGER NOT NULL, required_ambulances INTEGER NOT NULL, "
        "required_ventilators INTEGER NOT NULL, required_icu_beds INTEGER NOT NULL, "
        "required_doctors INTEGER NOT NULL, status INTEGER NOT NULL, UNIQUE(name, phone));"
        "CREATE TABLE resources (resource_id INTEGER PRIMARY KEY, total INTEGER NOT NULL DEFAULT 0, "
        "allocated INTEGER NOT NULL DEFAULT 0, out_of_service INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE allocations (patient_id INTEGER NOT NULL REFERENCES patients(id), "
        "resource_id INTEGER NOT NULL REFERENCES resources(resource_id), amount INTEGER NOT NULL, "
        "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, PRIMARY KEY(patient_id, resource_id));"
        "CREATE TABLE scheduling_history (id INTEGER PRIMARY KEY AUTOINCREMENT, patient_id INTEGER NOT NULL, "
        "event TEXT NOT NULL, detail TEXT NOT NULL, created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE app_meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL);"
        "INSERT INTO resources(resource_id,total,allocated) VALUES(0,3,1),(1,2,0),(2,1,0),(3,4,1);"
        "INSERT INTO patients(id,name,phone,age,location,priority,arrival_tick,required_ambulances,"
        "required_ventilators,required_icu_beds,required_doctors,status) "
        "VALUES(11,'Legacy Patient','LEGACY-ID',55,'Legacy Ward',3,2,1,0,0,1,2);"
        "INSERT INTO allocations(patient_id,resource_id,amount) VALUES(11,0,1),(11,3,1);"
        "INSERT INTO scheduling_history(patient_id,event,detail) "
        "VALUES(11,'ALLOCATE','legacy allocation');"
        "INSERT INTO app_meta(key,value) VALUES('tick',12);";
    char* error = nullptr;
    const int result = sqlite3_exec(legacy, schema, nullptr, nullptr, &error);
    const string errorMessage = error ? error : "";
    sqlite3_free(error);
    sqlite3_close(legacy);
    check(result == SQLITE_OK, "could not prepare migration fixture: " + errorMessage);

    {
        Database migrated(path.string());
        migrated.initialize();
        migrated.setActiveHospital("admin");
        check(migrated.patientSnapshot().size() == 1 &&
              migrated.patientSnapshot()[0].name == "Legacy Patient",
              "legacy patient was not preserved in admin hospital");
        check(migrated.resourceSnapshot()[EA_AMBULANCES].total == 3 &&
              migrated.resourceSnapshot()[EA_DOCTORS].allocated == 1,
              "legacy resource totals were not preserved");
        check(migrated.currentTick() == 12, "legacy ageing clock was not preserved");
        check(!migrated.historySnapshot().empty(), "legacy scheduling history was not preserved");
        migrated.setActiveHospital("dispatcher");
        check(migrated.patientSnapshot().empty() &&
              migrated.resourceSnapshot()[EA_AMBULANCES].total == 0,
              "other hospital received migrated admin data");
    }
    filesystem::remove(path);
}


int runTests(Allocator& allocator)
{
    const vector<pair<string, function<void()>>> tests{
        {"Priority order", [&] { testPriority(allocator); }},
        {"Ageing", [&] { testAgeing(allocator); }},
        {"Arrival tie-break", [&] { testTieBreak(allocator); }},
        {"Concurrent duplicate prevention", testConcurrentDuplicate},
        {"Capacity enforcement", [&] { testCapacityEnforcement(allocator); }},
        {"Concurrent resource exclusivity", [&] { testConcurrentExclusivity(allocator); }},
        {"Partial allocation and completion", [&] { testPartialThenCompletion(allocator); }},
        {"Restart recovery", [&] { testRecovery(allocator); }},
        {"New admin and saved capacities", [&] { testAdminCreation(allocator); }},
        {"Legacy database migration", testLegacyMigration}
    };
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.second();
            cout << "PASS: " << test.first << '\n';
        } catch (const exception& error) {
            ++failures;
            cout << "FAIL: " << test.first << " - " << error.what() << '\n';
        }
    }
    cout << (failures == 0 ? "All 10 tests passed.\n" : "Test failures: ")
              << (failures == 0 ? "" : to_string(failures) + "\n");
    return failures == 0 ? 0 : 1;
}

}


int main(int argc, char** argv)
{
    try {
        const bool simulated = argc == 2 && string(argv[1]) == "--simulate";
        if (argc > 2 || (argc == 2 && !simulated)) {
            cerr << "Usage: " << argv[0] << " [--simulate]\n";
            return 2;
        }
        Allocator allocator(simulated);
        cout << "Running tests against "
                  << (simulated ? "the C++ simulation backend" : "/dev/" EA_DEVICE_NAME) << '\n';
        return runTests(allocator);
    } catch (const exception& error) {
        cerr << "Test setup failed: " << error.what() << '\n';
        return 2;
    }
}
