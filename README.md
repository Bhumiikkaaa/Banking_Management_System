# Online Banking Management System

EG 301 Operating Systems - Mini Project
Author: Bhumika   Roll no: MT2026018   Branch: MTech CSE

A client-server banking system written in C for Linux. The server keeps all
data in plain files (no database) and serves many clients at the same time.
It is built around operating-system features: processes, threads, pipes,
sockets, file locking and `fsync`.

## Modules

| Role | What they can do |
|------|------------------|
| **Customer** (normal or joint account) | balance, deposit, withdraw, transfer, account details, mini statement, apply for loan, loan status, change password, feedback |
| **Bank Employee** | add customer (single or joint account), modify customer login, search account + transactions, see assigned loans, approve / reject loan |
| **Manager** | activate / deactivate accounts, list open loans, assign loan to an employee, read feedback |
| **Administrator** | add / delete / modify users of any role, search account details, list users |

Extra things added on top of the brief: transfers between accounts, mini
statement that shows *who* made each transaction on a joint account, loan
approval that credits the money automatically, 3-wrong-password disconnect,
one login per user at a time, passwords stored as hashes, a server log, and
crash recovery.

## Build and run

Needs `gcc`, `make` and `python3` (python only for the tests).

```
make                 # builds ./server and ./client
./server             # terminal 1  (port 8090, data kept in ./data)
./client             # terminal 2, 3, 4 ...  (any number of clients)
./client 192.168.1.5 8090      # connect from another machine
```

The first start creates the `data/` folder with demo users:

| Role | Username | Password |
|------|----------|----------|
| Administrator | admin | admin123 |
| Manager | manager1 | manager123 |
| Employee | emp1 | emp123 |
| Customer (joint account 10001, Rs 5000) | asha and ravi | asha123 / ravi123 |
| Customer (account 10002, Rs 3000) | meera | meera123 |
| Customer (account 10003, Rs 1000) | kiran | kiran123 |

Delete the `data/` folder to start again from scratch.

## Tests

```
make test            # crash-recovery test + concurrency test
make nolock          # build WITHOUT locking, then run: python3 tests/test_concurrency.py
make                 # go back to the normal build
```

With locking, all checks pass. With `make nolock` the same tests fail:
deposits are lost, more withdrawals succeed than the balance allows, and
money disappears in transfers. That is the proof that the locking is doing
real work.

## Folder layout

```
src/        server.c client.c store.c net.c flows.c
            customer.c employee.c manager.c admin.c bank.h
tests/      test_recovery.c  test_concurrency.py
docs/       architecture.png  class_diagram.png  transaction_flow.png  DESIGN.md
Makefile
```

Read `docs/DESIGN.md` for the design, the ACID explanation and viva questions.
