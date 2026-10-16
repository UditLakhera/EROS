#include "allocator.hpp"
#include "database.hpp"
#include<bits/stdc++.h>

#include <sys/ioctl.h>
using namespace std;

namespace {


string readLine(const string& prompt)
{
    cout << prompt;
    string value;
    if (!getline(cin, value))
        throw runtime_error("input closed");
    return value;
}


uint32_t readNumber(const string& prompt, uint32_t maximum)
{
    const string text = readLine(prompt);
    size_t used = 0;
    unsigned long long value = 0;
    try {
        value = stoull(text, &used);
    } catch (...) {
        throw invalid_argument("enter a whole number");
    }
    if (used != text.size() || value > maximum)
        throw invalid_argument("number is outside the allowed range");
    return static_cast<uint32_t>(value);
}


__u32 readPriority()
{
    cout << "Priority: 1=LOW, 2=MEDIUM, 3=HIGH\n";
    const auto value = readNumber("Choose priority: ", EA_HIGH);
    if (value < EA_LOW)
        throw invalid_argument("priority must be 1, 2, or 3");
    return value;
}


__u32 readRequiredUnit(const string& prompt)
{
    for (;;) {
        string answer = readLine(prompt + " (yes/no): ");
        transform(answer.begin(), answer.end(), answer.begin(),
                  [](unsigned char ch) { return static_cast<char>(tolower(ch)); });
        if (answer == "yes" || answer == "y")
            return 1;
        if (answer == "no" || answer == "n")
            return 0;
        cout << "Please answer yes or no.\n";
    }
}


void recoverKernel(Database& database, Allocator& allocator)
{
    allocator.call(EA_IOC_RESET);

    ea_tick tick{database.currentTick()};
    allocator.call(EA_IOC_TICK, &tick);

    const auto resources = database.resourceSnapshot();
    for (const auto& resource : resources) {
        ea_capacity capacity{resource.resource, resource.total};
        allocator.call(EA_IOC_SET_CAPACITY, &capacity);
        ea_out_of_service unavailable{resource.resource, resource.out_of_service};
        allocator.call(EA_IOC_SET_OUT_OF_SERVICE, &unavailable);
    }

    for (const auto& record : database.patientSnapshot()) {
        auto patient = record.kernel;
        allocator.call(EA_IOC_ADD_PATIENT, &patient);
    }
}


void saveDecision(Database& database, Allocator& allocator, const ea_decision& decision)
{
    try {
        database.persistDecision(decision);
    } catch (...) {
        ea_release release{decision.patient_id};
        try {
            allocator.call(EA_IOC_RELEASE, &release);
            recoverKernel(database, allocator);
        } catch (const exception& recoveryError) {
            throw runtime_error(string("database write failed and kernel recovery failed: ") +
                                     recoveryError.what());
        }
        throw;
    }
}


void runScheduler(Database& database, Allocator& allocator)
{
    for (;;) {
        ea_decision decision{};
        allocator.call(EA_IOC_SCHEDULE_NEXT, &decision);
        if (decision.patient_id == 0)
            break;
        saveDecision(database, allocator, decision);
        cout << "Scheduled patient #" << decision.patient_id
                  << " -> " << statusName(decision.status) << " (effective priority "
                  << decision.effective_priority << ")\n";
    }
}


__u32 registerPatient(Database& database, Allocator& allocator,
                      const PatientInput& input)
{
    ea_tick clock{};
    allocator.call(EA_IOC_TICK, &clock);
    const __u32 id = database.addPatient(input, clock.tick);
    ea_patient patient{};
    patient.id = id;
    patient.priority = input.priority;
    patient.arrival_tick = clock.tick;
    patient.required[0] = input.required[0];
    patient.required[1] = input.required[1];
    patient.required[2] = input.required[2];
    patient.required[3] = input.required[3];
    patient.status = EA_PENDING;
    try {
        allocator.call(EA_IOC_ADD_PATIENT, &patient);
    } catch (...) {
        database.removePatient(id);
        throw;
    }
    runScheduler(database, allocator);
    return id;
}


void menuRegister(Database& database, Allocator& allocator)
{
    PatientInput input;
    input.name = readLine("Patient name: ");
    input.phone = readLine("Phone or national ID: ");
    input.age = static_cast<int>(readNumber("Age: ", 130));
    input.location = readLine("Location: ");
    input.priority = readPriority();
    input.required[EA_AMBULANCES] = readRequiredUnit("Needs an ambulance");
    input.required[EA_VENTILATORS] = readRequiredUnit("Needs a ventilator");
    input.required[EA_ICU_BEDS] = readRequiredUnit("Needs an ICU bed");
    input.required[3] = readNumber("Doctors required: ", 100000);
    try {
        const auto id = registerPatient(database, allocator, input);
        cout << "Registered patient #" << id << ".\n";
    } catch (const DuplicatePatient&) {
        cout << "409 CONFLICT: duplicate patient\n";
    }
}


void menuCapacity(Database& database, Allocator& allocator)
{
    const auto resources = database.resourceSnapshot();
    for (uint32_t i = 0; i < EA_RESOURCE_COUNT; ++i)
        cout << i << "=" << resourceName(i) << " ";
    cout << '\n';
    const auto resource = readNumber("Resource number: ", EA_RESOURCE_COUNT - 1);
    const auto total = readNumber("New total capacity: ", 1000000);
    ea_capacity request{resource, total};
    try {
        database.setCapacity(resource, total, [&] {
            allocator.call(EA_IOC_SET_CAPACITY, &request);
        });
        if (total > resources[resource].total)
            runScheduler(database, allocator);
        cout << "Capacity updated.\n";
    } catch (...) {
        recoverKernel(database, allocator);
        throw;
    }
}


void menuOutOfService(Database& database, Allocator& allocator)
{
    for (uint32_t i = 0; i < EA_RESOURCE_COUNT; ++i)
        cout << i << "=" << resourceName(i) << " ";
    cout << '\n';
    const auto resource = readNumber("Resource number: ", EA_RESOURCE_COUNT - 1);
    const auto count = readNumber("Units out of service: ", 1000000);
    ea_out_of_service request{resource, count};
    try {
        database.setOutOfService(resource, count, [&] {
            allocator.call(EA_IOC_SET_OUT_OF_SERVICE, &request);
        });
        cout << "Out-of-service count updated.\n";
    } catch (...) {
        recoverKernel(database, allocator);
        throw;
    }
}


void menuRelease(Database& database, Allocator& allocator)
{
    const auto id = readNumber("Patient ID: ", numeric_limits<__u32>::max());
    ea_release release{id};
    allocator.call(EA_IOC_RELEASE, &release);
    try {
        database.releasePatient(id);
    } catch (...) {
        recoverKernel(database, allocator);
        throw;
    }
    runScheduler(database, allocator);
}


void menuPatients(const Database& database)
{
    const auto patients = database.patientSnapshot();
    if (patients.empty()) {
        cout << "\nNo patients registered for this hospital.\n";
        return;
    }
    cout << '\n'
         << left << setw(6) << "ID"
         << setw(22) << "NAME"
         << setw(18) << "PHONE / ID"
         << setw(6) << "AGE"
         << setw(20) << "LOCATION"
         << setw(10) << "PRIORITY"
         << setw(12) << "STATUS"
         << setw(17) << "REQUIRED A/V/I/D"
         << "ALLOCATED A/V/I/D\n"
         << string(123, '-') << '\n';
    for (const auto& record : patients) {
        const auto& patient = record.kernel;
        ostringstream required;
        ostringstream allocated;
        for (int i = 0; i < EA_RESOURCE_COUNT; ++i)
            required << (i ? "/" : "") << patient.required[i];
        for (int i = 0; i < EA_RESOURCE_COUNT; ++i)
            allocated << (i ? "/" : "") << patient.allocated[i];
        cout << left << setw(6) << patient.id
             << setw(22) << record.name
             << setw(18) << record.phone
             << setw(6) << record.age
             << setw(20) << record.location
             << setw(10) << priorityName(patient.priority)
             << setw(12) << statusName(patient.status)
             << setw(17) << required.str()
             << allocated.str() << '\n';
    }
    cout << string(123, '-') << '\n';
    cout << right;
}


void menuResources(Allocator& allocator)
{
    for (__u32 resource = 0; resource < EA_RESOURCE_COUNT; ++resource) {
        ea_resource_status status{};
        status.resource = resource;
        allocator.call(EA_IOC_GET_RESOURCES, &status);
        cout << resourceName(resource) << ": total=" << status.total
                  << ", allocated=" << status.allocated
                  << ", out-of-service=" << status.out_of_service
                  << ", available=" << status.available << '\n';
    }
}


void menuHistory(const Database& database)
{
    const auto history = database.historySnapshot();
    if (history.empty()) {
        cout << "No scheduling history.\n";
        return;
    }
    for (const auto& entry : history)
        cout << entry << '\n';
}


void runDemo(Database& database, Allocator& allocator)
{
    const array<__u32, EA_RESOURCE_COUNT> demoCapacity{4, 2, 2, 5};
    auto current = database.resourceSnapshot();
    for (__u32 i = 0; i < EA_RESOURCE_COUNT; ++i) {
        const __u32 target = max(demoCapacity[i],
            current[i].allocated + current[i].out_of_service);
        ea_capacity request{i, target};
        try {
            database.setCapacity(i, target, [&] {
                allocator.call(EA_IOC_SET_CAPACITY, &request);
            });
        } catch (...) {
            recoverKernel(database, allocator);
            throw;
        }
    }

    __u32 demoSequence = 1;
    for (const auto& patient : database.patientSnapshot())
        demoSequence = max(demoSequence, patient.kernel.id + 1);
    const array<PatientInput, 3> examples{{
        {"Demo High", "DEMO-H-" + to_string(demoSequence), "North Gate", 42, EA_HIGH, {1, 1, 1, 2}},
        {"Demo Medium", "DEMO-M-" + to_string(demoSequence + 1), "Central", 28, EA_MEDIUM, {1, 1, 1, 1}},
        {"Demo Low", "DEMO-L-" + to_string(demoSequence + 2), "South Gate", 67, EA_LOW, {1, 1, 1, 2}}
    }};
    for (const auto& example : examples)
        registerPatient(database, allocator, example);
    menuPatients(database);
    menuResources(allocator);
    menuHistory(database);
}


bool createAdmin(Database& database, Allocator& allocator)
{
    const auto username = readLine("New admin username: ");
    const auto password = readLine("New admin password: ");
    array<__u32, EA_RESOURCE_COUNT> capacities{};
    cout << "Enter this hospital's total capacity for each resource.\n";
    capacities[EA_AMBULANCES] = readNumber("Ambulances total: ", 1000000);
    capacities[EA_VENTILATORS] = readNumber("Ventilators total: ", 1000000);
    capacities[EA_ICU_BEDS] = readNumber("ICU beds total: ", 1000000);
    capacities[EA_DOCTORS] = readNumber("Doctors total: ", 1000000);

    try {
        allocator.call(EA_IOC_RESET);
        const bool created = database.createAdmin(
            username, password, capacities, [&](const __u32 resource, const __u32 total) {
                ea_capacity request{resource, total};
                allocator.call(EA_IOC_SET_CAPACITY, &request);
            });
        if (!created) {
            cout << "Admin username already exists.\n";
            return false;
        }
        database.setActiveHospital(username);
        recoverKernel(database, allocator);
    } catch (...) {
        recoverKernel(database, allocator);
        throw;
    }
    cout << "Admin account created for hospital '" << username
         << "'; its capacities are saved.\n";
    runScheduler(database, allocator);
    return true;
}


void login(Database& database, Allocator& allocator)
{
    int failedAttempts = 0;
    for (;;) {
        cout << "\n1 Admin login\n2 Create new admin\n0 Exit\n";
        const auto choice = readNumber("Choice: ", 2);
        if (choice == 0)
            throw runtime_error("login cancelled");
        if (choice == 2) {
            try {
                if (createAdmin(database, allocator))
                    return;
            } catch (const exception& error) {
                cerr << "Admin creation failed: " << error.what() << '\n';
            }
            continue;
        }

        const auto username = readLine("Admin username: ");
        const auto password = readLine("Admin password: ");
        if (database.authenticate(username, password)) {
            database.setActiveHospital(username);
            recoverKernel(database, allocator);
            cout << "Login successful for hospital '" << username << "'.\n";
            return;
        }
        cout << "Invalid username or password.\n";
        if (++failedAttempts >= 3)
            throw runtime_error("too many failed login attempts");
    }
}


void menuLoop(Database& database, Allocator& allocator)
{
    for (;;) {
        cout << "\nEmergency Resource Allocation System"
                  << (allocator.simulated() ? " [SIMULATION]" : " [KERNEL DEVICE]") << "\n"
                  << "1 Register patient\n2 Set/update capacity\n3 Mark out of service\n"
                  << "4 Run scheduler\n5 Release patient resources\n6 View patients\n"
                  << "7 View resources\n8 View scheduling history\n9 Run demo\n"
                  << "10 Advance ageing clock\n0 Exit\n";
        const auto choice = readNumber("Choice: ", 10);
        try {
            switch (choice) {
            case 0: return;
            case 1: menuRegister(database, allocator); break;
            case 2: menuCapacity(database, allocator); break;
            case 3: menuOutOfService(database, allocator); break;
            case 4: runScheduler(database, allocator); break;
            case 5: menuRelease(database, allocator); break;
            case 6: menuPatients(database); break;
            case 7: menuResources(allocator); break;
            case 8: menuHistory(database); break;
            case 9: runDemo(database, allocator); break;
            case 10: {
                const auto amount = readNumber("Ticks to advance: ", 1000000);
                try {
                    database.addTicks(amount, [&] {
                        ea_tick tick{amount};
                        allocator.call(EA_IOC_TICK, &tick);
                    });
                } catch (...) {
                    recoverKernel(database, allocator);
                    throw;
                }
                cout << "Ageing clock is now " << database.currentTick() << ".\n";
                runScheduler(database, allocator);
                break;
            }
            }
        } catch (const exception& error) {
            cerr << "Operation failed: " << error.what() << '\n';
        }
    }
}

}


int main(int argc, char** argv)
{
    try {
        bool simulated = false;
        string databasePath = "emergency.db";
        for (int i = 1; i < argc; ++i) {
            const string argument = argv[i];
            if (argument == "--simulate")
                simulated = true;
            else if (argument == "--db" && i + 1 < argc)
                databasePath = argv[++i];
            else {
                cerr << "Usage: " << argv[0] << " [--simulate] [--db FILE]\n";
                return 2;
            }
        }

        Database database(databasePath);
        database.initialize();
        Allocator allocator(simulated);
        login(database, allocator);
        menuLoop(database, allocator);
        return 0;
    } catch (const exception& error) {
        cerr << "Fatal: " << error.what() << '\n';
        return 1;
    }
}
