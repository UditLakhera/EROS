# Emergency Resource Allocation System

A small terminal-only Operating Systems + DBMS project. The Linux module owns
the fixed-size patient table, resource counters, priority/ageing scheduler, and
mutex. The C++17 program owns the CLI, salted administrator passwords, SQLite
records, history, recovery, and tests. The module never opens a database or a
file.

## File tree

```text
.
├── README.md
├── include/
│   └── ea_ioctl.h
├── kernel/
│   ├── Makefile
│   └── emergency_allocator.c
└── app/
    ├── Makefile
    ├── allocator.cpp
    ├── allocator.hpp
    ├── database.cpp
    ├── database.hpp
    ├── main.cpp
    └── tests.cpp
```

## Requirements and build

Build and load the module on a real Ubuntu machine or Ubuntu VM running the
matching kernel. Install the compiler, matching kernel headers, SQLite, and
OpenSSL development packages:

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) libsqlite3-dev libssl-dev

cd kernel
make
cd ../app
make
```

`kernel/Makefile` is a kbuild wrapper for the current running kernel. If that
kernel's headers are installed elsewhere, pass their build directory:

```bash
cd kernel
make KDIR=/path/to/kernel/build
```

The app Makefile builds both `emergency_app` and `ea_tests` with C++17, SQLite,
OpenSSL PBKDF2/SHA-256, and `std::thread`.

## Load, permissions, run, and unload

```bash
cd kernel
sudo insmod emergency_allocator.ko
ls -l /dev/emergency_allocator

# Temporary access for the current user's primary group; repeat after reload.
sudo chgrp "$(id -gn)" /dev/emergency_allocator
sudo chmod 660 /dev/emergency_allocator

cd ../app
./emergency_app
```

The module creates `/dev/emergency_allocator` with mode `0660`. The example
`chgrp`/`chmod` commands allow a regular user to open it for this session.
For persistent permissions, create a udev rule appropriate to the machine:

```bash
echo 'KERNEL=="emergency_allocator", MODE="0660", GROUP="plugdev"' \
  | sudo tee /etc/udev/rules.d/99-emergency-allocator.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo usermod -aG plugdev "$USER"
```

Log out and back in after changing group membership. To unload:

```bash
sudo rmmod emergency_allocator
```

Run the automated integration tests against the loaded module with:

```bash
cd app
./ea_tests
```

The tests reset the module's volatile state and are intended for a dedicated
development machine/VM. The app's persistent database is `app/emergency.db`
when launched from `app/`; use `--db FILE` to choose another location.

## Userspace simulation fallback

Custom kernel modules usually do not load on the default WSL2 kernel. A real
Ubuntu VM is recommended for the kernel-device demonstration. The same ioctl
commands and scheduling rules are also implemented by a mutex-protected C++
simulation backend:

```bash
cd app
./emergency_app --simulate
./ea_tests --simulate
```

The simulation does not require root, a kernel build tree, or `/dev`. Its
allocator state exists only for that process; the SQLite database remains
persistent, and startup recovery reconstructs the simulated state from it.

## Login and menu

On a new database, two demo administrator accounts are seeded with independent
random salts and PBKDF2-HMAC-SHA-256 password hashes:

```text
admin / admin123
dispatcher / dispatch123
```

Change these demo passwords before using the program outside a class exercise.
Each admin account represents a hospital. Patients (including duplicate-name
checks), allocations, history, ageing clock, and resource totals are separate
for each hospital; the admin username is used as that hospital's ID. A new hospital starts with zero
capacity. At the login screen, choose `2 Create new admin` to add a hospital;
the setup prompts for its ambulance, ventilator, ICU-bed, and doctor totals and
saves the salted password hash and capacities to SQLite. Existing admins can
change their hospital's totals from menu option 2. After new capacities are
saved, that hospital's scheduler runs for its waiting patients.
The earlier single-hospital SQLite schema is migrated on startup: its existing
patients, allocations, history, and resources are assigned to the seeded
`admin` hospital. Other admin accounts begin with separate, zeroed hospital
resources and patient lists.

The terminal menu registers patients, changes capacity and out-of-service
counts, schedules waiting patients, releases a patient's allocations, shows
patients/resources/history, runs a demo, and advances the deterministic ageing
clock. During patient registration, ambulance, ventilator, and ICU-bed needs
are answered with yes/no; each answer means one unit or zero units. Doctor
requirements remain a numeric count. A patient is unique by
`(name, phone-or-national-ID)` within a hospital;
duplicates in the same hospital print `409 CONFLICT: duplicate patient`, while
another hospital may register its own patient with the same details.

Releasing a patient frees all their allocations and marks them `RELEASED`, so
automatic scheduling cannot allocate resources back to a patient who has been
released. This is an additional terminal state beyond the scheduler's normal
`PENDING`, `PARTIAL`, and `ALLOCATED` states.

## Sample terminal session

The following is an illustrative run from a fresh simulation database after
logging into an admin and running the demo; IDs, timestamps, and output can vary:

```text
$ ./emergency_app --simulate
1 Admin login
2 Create new admin
0 Exit
Choice: 1
Admin username: admin
Admin password: admin123
Login successful.

Emergency Resource Allocation System [SIMULATION]
1 Register patient
2 Set/update capacity
3 Mark out of service
4 Run scheduler
5 Release patient resources
6 View patients
7 View resources
8 View scheduling history
9 Run demo
10 Advance ageing clock
0 Exit
Choice: 9
Scheduled patient #1 -> ALLOCATED (effective priority 3)
Scheduled patient #2 -> ALLOCATED (effective priority 2)
Scheduled patient #3 -> PARTIAL (effective priority 1)
ID    NAME                  PHONE / ID        AGE   LOCATION            PRIORITY  STATUS      REQUIRED A/V/I/D ALLOCATED A/V/I/D
---------------------------------------------------------------------------------------------------------------------------
1     Demo High             DEMO-H-1          42    North Gate          HIGH      ALLOCATED   1/1/1/2          1/1/1/2
2     Demo Medium           DEMO-M-2          28    Central             MEDIUM    ALLOCATED   1/1/1/1          1/1/1/1
3     Demo Low              DEMO-L-3          67    South Gate          LOW       PARTIAL     1/1/1/2          1/0/0/2
ambulances: total=4, allocated=3, out-of-service=0, available=1
ventilators: total=2, allocated=2, out-of-service=0, available=0
ICU beds: total=2, allocated=2, out-of-service=0, available=0
doctors: total=5, allocated=5, out-of-service=0, available=0
#3 patient=3 ALLOCATE effective_priority=1; allocations=1,0,0,2
#2 patient=2 ALLOCATE effective_priority=2; allocations=1,1,1,1
#1 patient=1 ALLOCATE effective_priority=3; allocations=1,1,1,2

Choice: 0

$ ./ea_tests --simulate
Running tests against the C++ simulation backend
PASS: Priority order
PASS: Ageing
PASS: Arrival tie-break
PASS: Concurrent duplicate prevention
PASS: Capacity enforcement
PASS: Concurrent resource exclusivity
PASS: Partial allocation and completion
PASS: Restart recovery
All 10 tests passed.
```

## Viva guide

- **Where is the mutex?** `ea_mutex` is the one global kernel mutex. The
  `unlocked_ioctl` dispatcher locks it before dispatching every command and
  unlocks it after the operation. The fixed patient array, resource counters,
  and tick are therefore updated as one serialized state.
- **Why does ageing prevent starvation?** A patient's effective priority is
  `base_priority + (current_tick - arrival_tick) / EA_AGE_STEP`. Every five
  ticks, an eligible waiting patient's effective priority increases by one.
  The linear scan selects the greatest value, then the earliest arrival.
- **Why does the kernel not touch SQLite?** The module is responsible only for
  scheduling and safe in-memory resource accounting. Keeping persistence in
  userspace avoids putting database/file I/O in kernel context; SQLite also
  remains the recovery source of truth and can be changed without changing
  kernel scheduling.
- **How does recovery work?** At startup, userspace resets the volatile module
  state, replays the saved tick and resource totals, then re-adds each patient
  with their saved allocations. Each scheduling decision and capacity/allocation
  change is saved inside an SQLite `BEGIN IMMEDIATE` transaction. If saving a
  scheduler decision fails, userspace issues `RELEASE` and reconstructs kernel
  memory from the last committed database state.
