# Banking Management System

## 1. Project overview

This is a client-server banking application written in C. It supports customers, employees, managers, and administrators.

- The **client** displays menus and sends requests.
- The **server** processes requests and manages client connections.
- The banking modules implement operations such as deposits, withdrawals, transfers, and loan approval.
- `store.c` reads and updates the banking data stored in files.

## 2. How the server works

The server accepts client connections using `accept()`.

It uses `fork()` to create a separate child process for each connected client. The child handles that client's login and banking operations.

A logger thread receives log messages through a pipe and writes them to the log file.

## 3. Why file locking is needed

Two customers might try to withdraw money from the same account at the same time.

Without locking, both operations could read the same old balance and produce an incorrect result.

The program uses `fcntl()` locks to control access to account records:

- Read locks allow multiple readers.
- Write locks prevent other processes from accessing the locked record incompatibly while it is being updated.

For transfers between two accounts, the program locks accounts in a consistent order to avoid deadlocks.

## 4. How a money transfer works

1. Lock both account records.
2. Read the balances and check whether the accounts are active and the sender has enough money.
3. Write the intended changes to `journal.dat`.
4. Update the account records and transaction history.
5. Mark the journal entry as complete and release the locks.

The journal helps the server finish an interrupted transaction when it restarts.

## 5. Files used by the project

| File | Purpose |
|---|---|
| `users.dat` | User details |
| `accounts.dat` | Account details and balances |
| `txns.dat` | Transaction history |
| `loans.dat` | Loan applications and decisions |
| `feedback.dat` | Customer feedback |
| `journal.dat` | Helps recover interrupted transactions |
| `sessions.lock` | Prevents a user from having multiple simultaneous logins |

Money is stored as integer paise rather than floating-point values.

## 6. Important system calls

| System call | Purpose |
|---|---|
| `fork()` | Creates a child process |
| `open()` | Opens a file |
| `pread()` | Reads from a specified file position |
| `pwrite()` | Writes to a specified file position |
| `fcntl()` | Manages file locks |
| `fsync()` | Requests that file data be flushed to storage |
| `socket()` | Creates a network socket |
| `accept()` | Accepts a client connection |
| `connect()` | Connects the client to the server |
| `pipe()` | Transfers log messages between processes and the logger thread |

## 7. Limitations

- Network communication is not encrypted with TLS.
- The password hashing method is simplified and is not suitable for a real banking system.
- Creating a separate process for each client has resource overhead.

## 8. Viva preparation

Focus on explaining:

- The client-server architecture.
- Why the server uses `fork()`.
- Why file locks are necessary.
- How a transfer updates two accounts.
- How the journal helps recover an interrupted transaction.
- The purpose of `pread()`, `pwrite()`, and `fsync()`.
