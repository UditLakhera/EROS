#include "database.hpp"

#include "allocator.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sqlite3.h>

#include <array>
#include <iomanip>
#include <sstream>
#include <system_error>
using namespace std;

namespace {

constexpr int kPasswordIterations = 120000;
constexpr int kSaltBytes = 16;
constexpr int kHashBytes = 32;


void requireSqlite(int result, sqlite3* db, const string& operation)
{
    if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW)
        throw runtime_error(operation + ": " + sqlite3_errmsg(db));
}


class Statement {
public:
    Statement(sqlite3* db, const string& sql) : db_(db)
    {
        requireSqlite(sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement_, nullptr),
                      db_, "prepare SQL");
    }

    ~Statement()
    {
        sqlite3_finalize(statement_);
    }

    sqlite3_stmt* get() const { return statement_; }

    void bindText(int index, const string& value)
    {
        requireSqlite(sqlite3_bind_text(statement_, index, value.c_str(),
                                        static_cast<int>(value.size()), SQLITE_TRANSIENT),
                      db_, "bind text");
    }

    void bindInt64(int index, int64_t value)
    {
        requireSqlite(sqlite3_bind_int64(statement_, index, value), db_, "bind integer");
    }

    void bindBlob(int index, const void* value, int length)
    {
        requireSqlite(sqlite3_bind_blob(statement_, index, value, length, SQLITE_TRANSIENT),
                      db_, "bind blob");
    }

    int step()
    {
        const int result = sqlite3_step(statement_);
        requireSqlite(result, db_, "execute SQL");
        return result;
    }

private:
    sqlite3* db_;
    sqlite3_stmt* statement_ = nullptr;
};


class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db)
    {
        char* error = nullptr;
        const int result = sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, &error);
        if (result != SQLITE_OK) {
            const string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw runtime_error("begin transaction: " + message);
        }
    }

    ~Transaction()
    {
        if (active_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }

    void commit()
    {
        char* error = nullptr;
        const int result = sqlite3_exec(db_, "COMMIT", nullptr, nullptr, &error);
        if (result != SQLITE_OK) {
            const string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw runtime_error("commit transaction: " + message);
        }
        active_ = false;
    }

private:
    sqlite3* db_;
    bool active_ = true;
};


string columnText(sqlite3_stmt* statement, int column)
{
    const auto* value = sqlite3_column_text(statement, column);
    return value ? reinterpret_cast<const char*>(value) : "";
}


string allocationDetail(const ea_decision& decision)
{
    ostringstream output;
    output << "effective_priority=" << decision.effective_priority << "; allocations=";
    for (int i = 0; i < EA_RESOURCE_COUNT; ++i) {
        if (i)
            output << ',';
        output << decision.total_allocated[i];
    }
    return output.str();
}

}


Database::Database(const string& path) : db_(nullptr)
{
    const int result = sqlite3_open_v2(path.c_str(), &db_,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (result != SQLITE_OK) {
        const string message = db_ ? sqlite3_errmsg(db_) : "could not allocate SQLite handle";
        if (db_)
            sqlite3_close(db_);
        db_ = nullptr;
        throw runtime_error("open database: " + message);
    }
    sqlite3_busy_timeout(db_, 5000);
}


Database::~Database()
{
    if (db_)
        sqlite3_close(db_);
}


void Database::execute(const string& sql) const
{
    char* error = nullptr;
    const int result = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const string message = error ? error : sqlite3_errmsg(db_);
        sqlite3_free(error);
        throw runtime_error("SQLite: " + message);
    }
}


void Database::initialize()
{
    execute("PRAGMA foreign_keys = ON");
    execute("CREATE TABLE IF NOT EXISTS admins ("
            "username TEXT PRIMARY KEY, salt BLOB NOT NULL, password_hash BLOB NOT NULL, "
            "iterations INTEGER NOT NULL)");
    execute("CREATE TABLE IF NOT EXISTS app_meta (key TEXT PRIMARY KEY, value INTEGER NOT NULL)");
    seedAdmin("admin", "admin123");
    seedAdmin("dispatcher", "dispatch123");

    if (!tableExists("resources"))
        createHospitalTables();
    else if (!columnExists("resources", "hospital_id"))
        migrateLegacyTables();

    Statement admins(db_, "SELECT username FROM admins");
    while (admins.step() == SQLITE_ROW)
        ensureHospitalResources(columnText(admins.get(), 0));
    ensureHospitalResources("default");

    Statement legacyTick(db_, "SELECT value FROM app_meta WHERE key = 'tick'");
    if (legacyTick.step() == SQLITE_ROW) {
        Statement saveTick(db_, "INSERT OR IGNORE INTO app_meta(key, value) VALUES('tick:admin', ?)");
        saveTick.bindInt64(1, sqlite3_column_int64(legacyTick.get(), 0));
        saveTick.step();
    }
}


bool Database::tableExists(const string& table) const
{
    Statement query(db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
    query.bindText(1, table);
    return query.step() == SQLITE_ROW;
}


bool Database::columnExists(const string& table, const string& column) const
{
    Statement query(db_, "PRAGMA table_info(" + table + ")");
    while (query.step() == SQLITE_ROW)
        if (columnText(query.get(), 1) == column)
            return true;
    return false;
}


void Database::createHospitalTables()
{
    execute("CREATE TABLE patients ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, hospital_id TEXT NOT NULL, "
            "name TEXT NOT NULL, phone TEXT NOT NULL, age INTEGER NOT NULL, "
            "location TEXT NOT NULL, priority INTEGER NOT NULL, arrival_tick INTEGER NOT NULL, "
            "required_ambulances INTEGER NOT NULL, required_ventilators INTEGER NOT NULL, "
            "required_icu_beds INTEGER NOT NULL, required_doctors INTEGER NOT NULL, "
            "status INTEGER NOT NULL, UNIQUE(hospital_id, name, phone), UNIQUE(hospital_id, id))");
    execute("CREATE TABLE resources ("
            "hospital_id TEXT NOT NULL, resource_id INTEGER NOT NULL, total INTEGER NOT NULL DEFAULT 0, "
            "allocated INTEGER NOT NULL DEFAULT 0, out_of_service INTEGER NOT NULL DEFAULT 0, "
            "PRIMARY KEY(hospital_id, resource_id))");
    execute("CREATE TABLE allocations ("
            "hospital_id TEXT NOT NULL, patient_id INTEGER NOT NULL, resource_id INTEGER NOT NULL, "
            "amount INTEGER NOT NULL CHECK(amount > 0), "
            "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP, "
            "PRIMARY KEY(hospital_id, patient_id, resource_id), "
            "FOREIGN KEY(hospital_id, patient_id) REFERENCES patients(hospital_id, id) ON DELETE CASCADE, "
            "FOREIGN KEY(hospital_id, resource_id) REFERENCES resources(hospital_id, resource_id))");
    execute("CREATE TABLE scheduling_history ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, hospital_id TEXT NOT NULL, "
            "patient_id INTEGER NOT NULL, event TEXT NOT NULL, detail TEXT NOT NULL, "
            "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
}


void Database::migrateLegacyTables()
{
    execute("PRAGMA foreign_keys = OFF");
    try {
        execute("BEGIN IMMEDIATE");
        execute("ALTER TABLE allocations RENAME TO allocations_legacy");
        execute("ALTER TABLE patients RENAME TO patients_legacy");
        execute("ALTER TABLE resources RENAME TO resources_legacy");
        execute("ALTER TABLE scheduling_history RENAME TO scheduling_history_legacy");
        createHospitalTables();
        execute("INSERT INTO resources(hospital_id, resource_id, total, allocated, out_of_service) "
                "SELECT 'admin', resource_id, total, allocated, out_of_service FROM resources_legacy");
        execute("INSERT INTO patients(id, hospital_id, name, phone, age, location, priority, "
                "arrival_tick, required_ambulances, required_ventilators, required_icu_beds, "
                "required_doctors, status) "
                "SELECT id, 'admin', name, phone, age, location, priority, arrival_tick, "
                "required_ambulances, required_ventilators, required_icu_beds, required_doctors, status "
                "FROM patients_legacy");
        execute("INSERT INTO allocations(hospital_id, patient_id, resource_id, amount, updated_at) "
                "SELECT 'admin', patient_id, resource_id, amount, updated_at FROM allocations_legacy");
        execute("INSERT INTO scheduling_history(id, hospital_id, patient_id, event, detail, created_at) "
                "SELECT id, 'admin', patient_id, event, detail, created_at FROM scheduling_history_legacy");
        execute("DROP TABLE allocations_legacy");
        execute("DROP TABLE scheduling_history_legacy");
        execute("DROP TABLE patients_legacy");
        execute("DROP TABLE resources_legacy");
        execute("COMMIT");
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "PRAGMA foreign_keys = ON", nullptr, nullptr, nullptr);
        throw;
    }
    execute("PRAGMA foreign_keys = ON");
}


void Database::ensureHospitalResources(const string& hospital)
{
    for (int resource = 0; resource < EA_RESOURCE_COUNT; ++resource) {
        Statement insert(db_, "INSERT OR IGNORE INTO resources(hospital_id, resource_id) VALUES(?, ?)");
        insert.bindText(1, hospital);
        insert.bindInt64(2, resource);
        insert.step();
    }
}


void Database::seedAdmin(const string& username, const string& password)
{
    Statement lookup(db_, "SELECT 1 FROM admins WHERE username = ?");
    lookup.bindText(1, username);
    if (lookup.step() == SQLITE_ROW)
        return;

    array<unsigned char, kSaltBytes> salt{};
    array<unsigned char, kHashBytes> hash{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
        PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()), kPasswordIterations,
                          EVP_sha256(), static_cast<int>(hash.size()), hash.data()) != 1)
        throw runtime_error("could not hash initial administrator password");

    Statement insert(db_, "INSERT INTO admins(username, salt, password_hash, iterations) "
                          "VALUES(?, ?, ?, ?)");
    insert.bindText(1, username);
    insert.bindBlob(2, salt.data(), static_cast<int>(salt.size()));
    insert.bindBlob(3, hash.data(), static_cast<int>(hash.size()));
    insert.bindInt64(4, kPasswordIterations);
    insert.step();
}


bool Database::authenticate(const string& username, const string& password)
{
    Statement query(db_, "SELECT salt, password_hash, iterations FROM admins WHERE username = ?");
    query.bindText(1, username);
    if (query.step() != SQLITE_ROW)
        return false;
    const auto* salt = static_cast<const unsigned char*>(sqlite3_column_blob(query.get(), 0));
    const int saltLength = sqlite3_column_bytes(query.get(), 0);
    const auto* expected = static_cast<const unsigned char*>(sqlite3_column_blob(query.get(), 1));
    const int hashLength = sqlite3_column_bytes(query.get(), 1);
    const int iterations = sqlite3_column_int(query.get(), 2);
    if (!salt || !expected || saltLength <= 0 || hashLength != kHashBytes || iterations <= 0)
        return false;
    array<unsigned char, kHashBytes> actual{};
    if (PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          salt, saltLength, iterations, EVP_sha256(),
                          static_cast<int>(actual.size()), actual.data()) != 1)
        throw runtime_error("password verification failed");
    return CRYPTO_memcmp(actual.data(), expected, actual.size()) == 0;
}


void Database::setActiveHospital(const string& username)
{
    if (username == "default") {
        active_hospital_ = username;
        ensureHospitalResources(active_hospital_);
        return;
    }
    Statement query(db_, "SELECT 1 FROM admins WHERE username = ?");
    query.bindText(1, username);
    if (query.step() != SQLITE_ROW)
        throw runtime_error("hospital account does not exist");
    active_hospital_ = username;
    ensureHospitalResources(active_hospital_);
}


bool Database::createAdmin(
    const string& username,
    const string& password,
    const array<__u32, EA_RESOURCE_COUNT>& capacities,
    const function<void(__u32, __u32)>& applyKernelCapacity)
{
    if (username.empty() || password.empty())
        throw invalid_argument("username and password cannot be empty");

    Transaction transaction(db_);
    Statement existing(db_, "SELECT 1 FROM admins WHERE username = ?");
    existing.bindText(1, username);
    if (existing.step() == SQLITE_ROW)
        return false;

    array<unsigned char, kSaltBytes> salt{};
    array<unsigned char, kHashBytes> hash{};
    if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1 ||
        PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                          salt.data(), static_cast<int>(salt.size()), kPasswordIterations,
                          EVP_sha256(), static_cast<int>(hash.size()), hash.data()) != 1)
        throw runtime_error("could not hash administrator password");

    Statement insert(db_, "INSERT INTO admins(username, salt, password_hash, iterations) "
                          "VALUES(?, ?, ?, ?)");
    insert.bindText(1, username);
    insert.bindBlob(2, salt.data(), static_cast<int>(salt.size()));
    insert.bindBlob(3, hash.data(), static_cast<int>(hash.size()));
    insert.bindInt64(4, kPasswordIterations);
    insert.step();

    ensureHospitalResources(username);
    for (__u32 resource = 0; resource < EA_RESOURCE_COUNT; ++resource) {
        Statement current(db_, "SELECT allocated, out_of_service FROM resources "
                               "WHERE hospital_id = ? AND resource_id = ?");
        current.bindText(1, username);
        current.bindInt64(2, resource);
        if (current.step() != SQLITE_ROW)
            throw runtime_error("hospital resource row is missing");
        const auto committed = sqlite3_column_int64(current.get(), 0) +
                               sqlite3_column_int64(current.get(), 1);
        if (capacities[resource] < committed)
            throw runtime_error("capacity cannot be lower than allocated plus out-of-service");

        applyKernelCapacity(resource, capacities[resource]);
        Statement update(db_, "UPDATE resources SET total = ? "
                              "WHERE hospital_id = ? AND resource_id = ?");
        update.bindInt64(1, capacities[resource]);
        update.bindText(2, username);
        update.bindInt64(3, resource);
        update.step();
    }
    transaction.commit();
    return true;
}


__u32 Database::addPatient(const PatientInput& input, __u64 arrivalTick)
{
    Transaction transaction(db_);
    Statement duplicate(db_, "SELECT 1 FROM patients "
                             "WHERE hospital_id = ? AND name = ? AND phone = ?");
    duplicate.bindText(1, active_hospital_);
    duplicate.bindText(2, input.name);
    duplicate.bindText(3, input.phone);
    if (duplicate.step() == SQLITE_ROW)
        throw DuplicatePatient();

    Statement insert(db_, "INSERT INTO patients(hospital_id, name, phone, age, location, priority, arrival_tick, "
                          "required_ambulances, required_ventilators, required_icu_beds, "
                          "required_doctors, status) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    insert.bindText(1, active_hospital_);
    insert.bindText(2, input.name);
    insert.bindText(3, input.phone);
    insert.bindInt64(4, input.age);
    insert.bindText(5, input.location);
    insert.bindInt64(6, input.priority);
    insert.bindInt64(7, static_cast<int64_t>(arrivalTick));
    for (int i = 0; i < EA_RESOURCE_COUNT; ++i)
        insert.bindInt64(8 + i, input.required[i]);
    insert.bindInt64(12, EA_PENDING);
    insert.step();

    const auto id = sqlite3_last_insert_rowid(db_);
    if (id <= 0 || static_cast<uint64_t>(id) > UINT32_MAX)
        throw runtime_error("patient ID exceeds the kernel ABI range");
    transaction.commit();
    return static_cast<__u32>(id);
}


void Database::persistDecision(const ea_decision& decision)
{
    if (decision.patient_id == 0)
        return;
    Transaction transaction(db_);
    Statement patient(db_, "UPDATE patients SET status = ? WHERE hospital_id = ? AND id = ?");
    patient.bindInt64(1, decision.status);
    patient.bindText(2, active_hospital_);
    patient.bindInt64(3, decision.patient_id);
    patient.step();
    if (sqlite3_changes(db_) != 1)
        throw runtime_error("scheduled patient is missing from SQLite");

    for (int i = 0; i < EA_RESOURCE_COUNT; ++i) {
        if (decision.total_allocated[i] == 0) {
            Statement erase(db_, "DELETE FROM allocations "
                                 "WHERE hospital_id = ? AND patient_id = ? AND resource_id = ?");
            erase.bindText(1, active_hospital_);
            erase.bindInt64(2, decision.patient_id);
            erase.bindInt64(3, i);
            erase.step();
        } else {
            Statement save(db_, "INSERT INTO allocations(hospital_id, patient_id, resource_id, amount) "
                                "VALUES(?, ?, ?, ?) "
                                "ON CONFLICT(hospital_id, patient_id, resource_id) DO UPDATE SET "
                                "amount = excluded.amount, updated_at = CURRENT_TIMESTAMP");
            save.bindText(1, active_hospital_);
            save.bindInt64(2, decision.patient_id);
            save.bindInt64(3, i);
            save.bindInt64(4, decision.total_allocated[i]);
            save.step();
        }
    }
    recomputeAllocated();
    Statement history(db_, "INSERT INTO scheduling_history(hospital_id, patient_id, event, detail) "
                           "VALUES(?, ?, 'ALLOCATE', ?)");
    history.bindText(1, active_hospital_);
    history.bindInt64(2, decision.patient_id);
    history.bindText(3, allocationDetail(decision));
    history.step();
    transaction.commit();
}


void Database::releasePatient(__u32 patientId)
{
    Transaction transaction(db_);
    Statement erase(db_, "DELETE FROM allocations WHERE hospital_id = ? AND patient_id = ?");
    erase.bindText(1, active_hospital_);
    erase.bindInt64(2, patientId);
    erase.step();
    Statement patient(db_, "UPDATE patients SET status = ? WHERE hospital_id = ? AND id = ?");
    patient.bindInt64(1, EA_RELEASED);
    patient.bindText(2, active_hospital_);
    patient.bindInt64(3, patientId);
    patient.step();
    if (sqlite3_changes(db_) != 1)
        throw runtime_error("patient is missing from SQLite");
    recomputeAllocated();
    Statement history(db_, "INSERT INTO scheduling_history(hospital_id, patient_id, event, detail) "
                           "VALUES(?, ?, 'RELEASE', 'all patient allocations released')");
    history.bindText(1, active_hospital_);
    history.bindInt64(2, patientId);
    history.step();
    transaction.commit();
}


void Database::recomputeAllocated() const
{
    Statement update(db_, "UPDATE resources SET allocated = COALESCE((SELECT SUM(amount) "
                          "FROM allocations WHERE allocations.hospital_id = resources.hospital_id "
                          "AND allocations.resource_id = resources.resource_id), 0) "
                          "WHERE hospital_id = ?");
    update.bindText(1, active_hospital_);
    update.step();
}


void Database::setCapacity(__u32 resource, __u32 total,
                           const function<void()>& applyKernel)
{
    if (resource >= EA_RESOURCE_COUNT)
        throw invalid_argument("invalid resource index");
    Transaction transaction(db_);
    Statement lookup(db_, "SELECT allocated, out_of_service FROM resources "
                          "WHERE hospital_id = ? AND resource_id = ?");
    lookup.bindText(1, active_hospital_);
    lookup.bindInt64(2, resource);
    if (lookup.step() != SQLITE_ROW)
        throw runtime_error("resource row is missing");
    const auto unavailable = sqlite3_column_int64(lookup.get(), 0) +
                            sqlite3_column_int64(lookup.get(), 1);
    if (total < unavailable)
        throw runtime_error("capacity cannot be lower than allocated plus out-of-service");
    applyKernel();
    Statement update(db_, "UPDATE resources SET total = ? WHERE hospital_id = ? AND resource_id = ?");
    update.bindInt64(1, total);
    update.bindText(2, active_hospital_);
    update.bindInt64(3, resource);
    update.step();
    transaction.commit();
}


void Database::setOutOfService(__u32 resource, __u32 count,
                               const function<void()>& applyKernel)
{
    if (resource >= EA_RESOURCE_COUNT)
        throw invalid_argument("invalid resource index");
    Transaction transaction(db_);
    Statement lookup(db_, "SELECT total, allocated FROM resources "
                          "WHERE hospital_id = ? AND resource_id = ?");
    lookup.bindText(1, active_hospital_);
    lookup.bindInt64(2, resource);
    if (lookup.step() != SQLITE_ROW)
        throw runtime_error("resource row is missing");
    const auto total = sqlite3_column_int64(lookup.get(), 0);
    const auto allocated = sqlite3_column_int64(lookup.get(), 1);
    if (count > total - allocated)
        throw runtime_error("out-of-service count exceeds unallocated capacity");
    applyKernel();
    Statement update(db_, "UPDATE resources SET out_of_service = ? "
                          "WHERE hospital_id = ? AND resource_id = ?");
    update.bindInt64(1, count);
    update.bindText(2, active_hospital_);
    update.bindInt64(3, resource);
    update.step();
    transaction.commit();
}


void Database::addTicks(__u64 amount, const function<void()>& applyKernel)
{
    Transaction transaction(db_);
    const __u64 next = currentTick() + amount;
    if (next < currentTick())
        throw overflow_error("ageing tick overflow");
    applyKernel();
    setMeta("tick:" + active_hospital_, next);
    transaction.commit();
}


__u64 Database::currentTick() const
{
    Statement query(db_, "SELECT value FROM app_meta WHERE key = ?");
    query.bindText(1, "tick:" + active_hospital_);
    if (query.step() != SQLITE_ROW)
        return 0;
    return static_cast<__u64>(sqlite3_column_int64(query.get(), 0));
}


void Database::setMeta(const string& key, uint64_t value) const
{
    Statement update(db_, "INSERT INTO app_meta(key, value) VALUES(?, ?) "
                         "ON CONFLICT(key) DO UPDATE SET value = excluded.value");
    update.bindText(1, key);
    update.bindInt64(2, static_cast<int64_t>(value));
    update.step();
}


array<ea_resource_status, EA_RESOURCE_COUNT> Database::resourceSnapshot() const
{
    array<ea_resource_status, EA_RESOURCE_COUNT> result{};
    Statement query(db_, "SELECT resource_id, total, allocated, out_of_service "
                         "FROM resources WHERE hospital_id = ? ORDER BY resource_id");
    query.bindText(1, active_hospital_);
    while (query.step() == SQLITE_ROW) {
        const int index = sqlite3_column_int(query.get(), 0);
        if (index < 0 || index >= EA_RESOURCE_COUNT)
            continue;
        auto& item = result[index];
        item.resource = index;
        item.total = static_cast<__u32>(sqlite3_column_int64(query.get(), 1));
        item.allocated = static_cast<__u32>(sqlite3_column_int64(query.get(), 2));
        item.out_of_service = static_cast<__u32>(sqlite3_column_int64(query.get(), 3));
        const auto unavailable = static_cast<uint64_t>(item.allocated) + item.out_of_service;
        item.available = unavailable >= item.total ? 0 : item.total - unavailable;
    }
    return result;
}


vector<PatientRecord> Database::patientSnapshot() const
{
    vector<PatientRecord> result;
    Statement query(db_, "SELECT p.id, p.name, p.phone, p.age, p.location, p.priority, p.arrival_tick, "
                         "p.required_ambulances, p.required_ventilators, p.required_icu_beds, "
                         "p.required_doctors, p.status, "
                         "COALESCE((SELECT amount FROM allocations a WHERE a.hospital_id=p.hospital_id "
                         "AND a.patient_id=p.id AND a.resource_id=0),0), "
                         "COALESCE((SELECT amount FROM allocations a WHERE a.hospital_id=p.hospital_id "
                         "AND a.patient_id=p.id AND a.resource_id=1),0), "
                         "COALESCE((SELECT amount FROM allocations a WHERE a.hospital_id=p.hospital_id "
                         "AND a.patient_id=p.id AND a.resource_id=2),0), "
                         "COALESCE((SELECT amount FROM allocations a WHERE a.hospital_id=p.hospital_id "
                         "AND a.patient_id=p.id AND a.resource_id=3),0) "
                         "FROM patients p WHERE p.hospital_id = ? ORDER BY p.id");
    query.bindText(1, active_hospital_);
    while (query.step() == SQLITE_ROW) {
        PatientRecord record;
        auto& patient = record.kernel;
        const auto id = sqlite3_column_int64(query.get(), 0);
        if (id <= 0 || static_cast<uint64_t>(id) > UINT32_MAX)
            throw runtime_error("patient ID exceeds the kernel ABI range");
        patient.id = static_cast<__u32>(id);
        record.name = columnText(query.get(), 1);
        record.phone = columnText(query.get(), 2);
        record.age = sqlite3_column_int(query.get(), 3);
        record.location = columnText(query.get(), 4);
        patient.priority = static_cast<__u32>(sqlite3_column_int64(query.get(), 5));
        patient.arrival_tick = static_cast<__u64>(sqlite3_column_int64(query.get(), 6));
        for (int i = 0; i < EA_RESOURCE_COUNT; ++i) {
            patient.required[i] = static_cast<__u32>(sqlite3_column_int64(query.get(), 7 + i));
            patient.allocated[i] = static_cast<__u32>(sqlite3_column_int64(query.get(), 12 + i));
        }
        patient.status = static_cast<__u32>(sqlite3_column_int64(query.get(), 11));
        result.push_back(record);
    }
    return result;
}


vector<string> Database::historySnapshot() const
{
    vector<string> result;
    Statement query(db_, "SELECT id, patient_id, event, detail FROM scheduling_history "
                         "WHERE hospital_id = ? ORDER BY id DESC LIMIT 30");
    query.bindText(1, active_hospital_);
    while (query.step() == SQLITE_ROW) {
        ostringstream line;
        line << '#' << sqlite3_column_int64(query.get(), 0)
             << " patient=" << sqlite3_column_int64(query.get(), 1)
             << " " << columnText(query.get(), 2)
             << " " << columnText(query.get(), 3);
        result.push_back(line.str());
    }
    return result;
}


void Database::removePatient(__u32 patientId)
{
    Transaction transaction(db_);
    Statement erase(db_, "DELETE FROM patients WHERE hospital_id = ? AND id = ?");
    erase.bindText(1, active_hospital_);
    erase.bindInt64(2, patientId);
    erase.step();
    transaction.commit();
}


DuplicatePatient::DuplicatePatient() : runtime_error("409 CONFLICT: duplicate patient")
{
}
