/*
 * store.c - everything that touches the data files.
 *
 * Files (all in the data directory, fixed-size binary records):
 *   users.dat  accounts.dat  txns.dat  loans.dat  feedback.dat
 *   journal.dat   - one record, the write-ahead log used for atomic updates
 *   sessions.lock - never holds data, only used for "one login per user" locks
 *
 * Only system calls are used for file work: open, pread, pwrite, fsync,
 * fstat, fcntl.  Locking is done with fcntl() record locks, so a lock only
 * covers the bytes of one record and not the whole file.
 *
 * IMPORTANT fcntl detail: locks belong to the *process*, and closing ANY
 * descriptor of a file drops all of that process's locks on it.  That is why
 * the server forks one process per client (threads would not exclude each
 * other) and why a process opens its files once and keeps them open.
 */
#define _GNU_SOURCE
#include "bank.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

char g_data_dir[128] = DEFAULT_DATA_DIR;

static int users_fd = -1, acc_fd = -1, txn_fd = -1, loan_fd = -1;
static int fb_fd = -1, jr_fd = -1, sess_fd = -1;

/* A made-up byte far past the end of a file. Whoever wants to append a new
 * record locks this byte first, so two processes can't both pick the same id. */
#define TAIL_LOCK_OFF 0x40000000L

/* ------------------------------------------------------------------ */
/* low level helpers                                                   */
/* ------------------------------------------------------------------ */

static int lock_range(int fd, short type, off_t start, off_t len, int wait)
{
#ifdef NO_LOCKS      /* demo build only: shows what goes wrong without locking */
    (void)fd; (void)type; (void)start; (void)len; (void)wait;
    return 0;
#endif
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = type;            /* F_RDLCK, F_WRLCK or F_UNLCK */
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;              /* 0 means "till end of file"  */

    int r;
    do {
        r = fcntl(fd, wait ? F_SETLKW : F_SETLK, &fl);
    } while (r == -1 && errno == EINTR && wait);
    return r;
}

static int lock_rec(int fd, size_t recsz, int idx, short type)
{
    return lock_range(fd, type, (off_t)idx * (off_t)recsz, (off_t)recsz, 1);
}

static int rec_count(int fd, size_t recsz)
{
    struct stat st;
    if (fstat(fd, &st) < 0) return 0;
    return (int)(st.st_size / (off_t)recsz);
}

static int rec_read(int fd, int idx, void *buf, size_t recsz)
{
    ssize_t n = pread(fd, buf, recsz, (off_t)idx * (off_t)recsz);
    return n == (ssize_t)recsz ? 0 : -1;
}

static int rec_write(int fd, int idx, const void *buf, size_t recsz)
{
    ssize_t n = pwrite(fd, buf, recsz, (off_t)idx * (off_t)recsz);
    return n == (ssize_t)recsz ? 0 : -1;
}

static int open_one(const char *name)
{
    char path[256];
    snprintf(path, sizeof path, "%s/%s", g_data_dir, name);
    return open(path, O_RDWR | O_CREAT, 0644);
}

int store_open(void)
{
    users_fd = open_one("users.dat");
    acc_fd   = open_one("accounts.dat");
    txn_fd   = open_one("txns.dat");
    loan_fd  = open_one("loans.dat");
    fb_fd    = open_one("feedback.dat");
    jr_fd    = open_one("journal.dat");
    sess_fd  = open_one("sessions.lock");
    if (users_fd < 0 || acc_fd < 0 || txn_fd < 0 || loan_fd < 0 ||
        fb_fd < 0 || jr_fd < 0 || sess_fd < 0)
        return -1;
    return 0;
}

void store_close(void)
{
    int *all[] = { &users_fd, &acc_fd, &txn_fd, &loan_fd, &fb_fd, &jr_fd, &sess_fd };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        if (*all[i] >= 0) close(*all[i]);
        *all[i] = -1;
    }
}

const char *bank_strerror(int code)
{
    switch (code) {
    case B_OK:       return "ok";
    case B_NOACC:    return "Account not found.";
    case B_INACTIVE: return "Account is deactivated.";
    case B_FUNDS:    return "Insufficient balance.";
    case B_BADAMT:   return "Invalid amount.";
    case B_SAMEACC:  return "Cannot transfer to the same account.";
    default:         return "Disk error, please try again.";
    }
}

/* Not a secure hash!  FNV-1a is enough to avoid keeping plain passwords in
 * the file for a college project.  A real bank would use bcrypt/argon2. */
void hash_password(const char *pw, char out[20])
{
    unsigned long long h = 1469598103934665603ULL;
    const char *salt = "bit-durg-os";
    for (const char *s = salt; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    for (const char *s = pw;   *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    snprintf(out, 20, "%016llx", h);
}

/* ------------------------------------------------------------------ */
/* journal (write-ahead log) - this is what gives us atomicity          */
/* ------------------------------------------------------------------ */

typedef struct {
    int     state;          /* 1 = transaction in progress, 0 = finished */
    int     n;              /* how many account images (1 or 2)          */
    Account after[2];       /* balances the accounts must end up with    */
    Txn     txn[2];         /* history rows that belong to this change   */
    int     has_loan;
    Loan    loan;           /* new loan status (only for loan approval)  */
} Journal;

/* Writes the "after images" to the real files.  The same function is used
 * for normal commits and for crash recovery.  It can safely run twice
 * because it always writes absolute values at fixed positions. */
static void redo(const Journal *j)
{
    for (int i = 0; i < j->n; i++)
        rec_write(acc_fd, j->after[i].acc_no - ACC_BASE, &j->after[i], sizeof(Account));
    fsync(acc_fd);

    for (int i = 0; i < j->n; i++)
        rec_write(txn_fd, j->txn[i].txn_id - 1, &j->txn[i], sizeof(Txn));
    fsync(txn_fd);

    if (j->has_loan) {
        rec_write(loan_fd, j->loan.loan_id - 1, &j->loan, sizeof(Loan));
        fsync(loan_fd);
    }
}

int store_recover(void)
{
    Journal j;
    if (rec_read(jr_fd, 0, &j, sizeof j) < 0) return 0;   /* empty journal */
    if (j.state != 1) return 0;                           /* nothing pending */

    redo(&j);
    j.state = 0;
    rec_write(jr_fd, 0, &j, sizeof j);
    fsync(jr_fd);
    return 1;
}

/* ------------------------------------------------------------------ */
/* the one function that moves money                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int         acc_no;
    money       delta;      /* + for credit, - for debit */
    const char *type;
} Leg;

static void release_accounts(const int *idx, const int *order, int how_many)
{
    for (int k = 0; k < how_many; k++)
        lock_rec(acc_fd, sizeof(Account), idx[order[k]], F_UNLCK);
}

/*
 * Applies 1 or 2 balance changes as ONE atomic unit.
 *
 *  1. lock the account record(s) for writing, always in ascending account
 *     number order (so two opposite transfers can never deadlock)
 *  2. read them, check rules (active? enough money?)   -> Consistency
 *  3. write the intended result to the journal and fsync -> Atomicity
 *  4. copy the result into the real files and fsync      -> Durability
 *  5. mark the journal finished, release the locks       -> Isolation
 *
 * If the process dies after step 3, store_recover() repeats step 4 on the
 * next server start, so the change is never half applied.
 */
static int apply_legs(int uid, const Leg *legs, int n, const Loan *loan_after, money *new_bal)
{
    int idx[2], order[2] = { 0, 1 }, locked = 0, rc = B_OK;
    Account cur[2];
    int total = rec_count(acc_fd, sizeof(Account));

    for (int i = 0; i < n; i++) {
        idx[i] = legs[i].acc_no - ACC_BASE;
        if (idx[i] < 0 || idx[i] >= total) return B_NOACC;
    }
    if (n == 2 && idx[1] < idx[0]) { order[0] = 1; order[1] = 0; }

    /* step 1 */
    for (int k = 0; k < n; k++) {
        if (lock_rec(acc_fd, sizeof(Account), idx[order[k]], F_WRLCK) < 0) {
            release_accounts(idx, order, locked);
            return B_IOERR;
        }
        locked++;
    }

    /* step 2 */
    for (int i = 0; i < n; i++) {
        if (rec_read(acc_fd, idx[i], &cur[i], sizeof(Account)) < 0) { rc = B_IOERR; break; }
        if (!cur[i].active) { rc = B_INACTIVE; break; }
        cur[i].balance += legs[i].delta;
        if (cur[i].balance < 0) { rc = B_FUNDS; break; }
    }
    if (rc != B_OK) {
        release_accounts(idx, order, locked);
        return rc;
    }

    /* step 3: the journal is a single record, so we lock all of it */
    lock_range(jr_fd, F_WRLCK, 0, 0, 1);

    Journal j;
    memset(&j, 0, sizeof j);
    j.state = 1;
    j.n = n;
    int base = rec_count(txn_fd, sizeof(Txn));   /* safe: only journal holders append */
    time_t now = time(NULL);
    for (int i = 0; i < n; i++) {
        j.after[i] = cur[i];
        j.txn[i].txn_id = base + i + 1;
        j.txn[i].acc_no = legs[i].acc_no;
        j.txn[i].user_id = uid;
        snprintf(j.txn[i].type, sizeof j.txn[i].type, "%s", legs[i].type);
        j.txn[i].amount = legs[i].delta < 0 ? -legs[i].delta : legs[i].delta;
        j.txn[i].balance_after = cur[i].balance;
        j.txn[i].when = now;
    }
    if (loan_after) { j.has_loan = 1; j.loan = *loan_after; }

    if (rec_write(jr_fd, 0, &j, sizeof j) < 0) rc = B_IOERR;
    else fsync(jr_fd);

    /* Test hook: pretend the power went out right after the journal was
     * written. Used only by tests/test_recovery.c (never set in normal runs). */
    if (rc == B_OK && getenv("BANK_CRASH_TEST")) _exit(99);

    if (rc == B_OK) {
        /* step 4 (txn file is locked while we append so readers see whole rows) */
        lock_range(txn_fd, F_WRLCK, 0, 0, 1);
        redo(&j);
        lock_range(txn_fd, F_UNLCK, 0, 0, 0);

        /* step 5 */
        j.state = 0;
        rec_write(jr_fd, 0, &j, sizeof j);
        fsync(jr_fd);
        if (new_bal) *new_bal = cur[0].balance;
    }

    lock_range(jr_fd, F_UNLCK, 0, 0, 0);
    release_accounts(idx, order, locked);
    return rc;
}

int bank_deposit(int acc_no, int uid, money amt, money *new_bal)
{
    if (amt <= 0) return B_BADAMT;
    Leg l = { acc_no, amt, "DEPOSIT" };
    return apply_legs(uid, &l, 1, NULL, new_bal);
}

int bank_open_deposit(int acc_no, int uid, money amt)
{
    if (amt <= 0) return B_BADAMT;
    Leg l = { acc_no, amt, "OPENING" };
    return apply_legs(uid, &l, 1, NULL, NULL);
}

int bank_withdraw(int acc_no, int uid, money amt, money *new_bal)
{
    if (amt <= 0) return B_BADAMT;
    Leg l = { acc_no, -amt, "WITHDRAW" };
    return apply_legs(uid, &l, 1, NULL, new_bal);
}

int bank_transfer(int from, int to, int uid, money amt, money *new_bal)
{
    if (amt <= 0) return B_BADAMT;
    if (from == to) return B_SAMEACC;
    Leg l[2] = { { from, -amt, "TRANSFER_OUT" }, { to, amt, "TRANSFER_IN" } };
    return apply_legs(uid, l, 2, NULL, new_bal);   /* new_bal = sender's balance */
}

/* ------------------------------------------------------------------ */
/* users                                                                */
/* ------------------------------------------------------------------ */

int user_total(void) { return rec_count(users_fd, sizeof(User)); }

int user_at(int index, User *out)
{
    return rec_read(users_fd, index, out, sizeof(User));
}

int user_get(int id, User *out)
{
    if (id < 1 || id > user_total()) return -1;
    if (rec_read(users_fd, id - 1, out, sizeof(User)) < 0) return -1;
    return out->active ? 0 : -1;
}

int user_find_by_name(const char *name, User *out)
{
    int n = user_total();
    for (int i = 0; i < n; i++) {
        User u;
        if (rec_read(users_fd, i, &u, sizeof u) < 0) continue;
        if (u.active && strcmp(u.username, name) == 0) { *out = u; return 0; }
    }
    return -1;
}

int user_check_login(int role, const char *name, const char *pw, User *out)
{
    User u;
    char h[20];
    if (user_find_by_name(name, &u) < 0) return -1;
    hash_password(pw, h);
    if (u.role != role || strcmp(u.pass_hash, h) != 0) return -1;
    *out = u;
    return 0;
}

/* returns 0 ok, -1 name already used, -2 disk error */
int user_add(const char *name, const char *pw, int role, int *new_id)
{
    int rc = -2;
    lock_range(users_fd, F_WRLCK, TAIL_LOCK_OFF, 1, 1);

    User dup;
    if (user_find_by_name(name, &dup) == 0) {
        rc = -1;
    } else {
        User u;
        memset(&u, 0, sizeof u);
        int n = user_total();
        u.id = n + 1;
        snprintf(u.username, sizeof u.username, "%s", name);
        hash_password(pw, u.pass_hash);
        u.role = role;
        u.active = 1;
        if (rec_write(users_fd, n, &u, sizeof u) == 0) {
            fsync(users_fd);
            if (new_id) *new_id = u.id;
            rc = 0;
        }
    }

    lock_range(users_fd, F_UNLCK, TAIL_LOCK_OFF, 1, 0);
    return rc;
}

/* NULL means "leave that field alone". 0 ok, -1 not found, -2 name taken, -3 disk */
int user_update(int id, const char *new_name, const char *new_pw)
{
    int rc = -3;
    lock_range(users_fd, F_WRLCK, TAIL_LOCK_OFF, 1, 1);   /* name uniqueness */
    if (id < 1 || id > user_total()) {
        lock_range(users_fd, F_UNLCK, TAIL_LOCK_OFF, 1, 0);
        return -1;
    }
    lock_rec(users_fd, sizeof(User), id - 1, F_WRLCK);

    User u;
    if (rec_read(users_fd, id - 1, &u, sizeof u) == 0 && u.active) {
        User other;
        if (new_name && strcmp(new_name, u.username) != 0 &&
            user_find_by_name(new_name, &other) == 0) {
            rc = -2;
        } else {
            if (new_name) snprintf(u.username, sizeof u.username, "%s", new_name);
            if (new_pw) hash_password(new_pw, u.pass_hash);
            if (rec_write(users_fd, id - 1, &u, sizeof u) == 0) { fsync(users_fd); rc = 0; }
        }
    } else {
        rc = -1;
    }

    lock_rec(users_fd, sizeof(User), id - 1, F_UNLCK);
    lock_range(users_fd, F_UNLCK, TAIL_LOCK_OFF, 1, 0);
    return rc;
}

int user_delete(int id)
{
    if (id < 1 || id > user_total()) return -1;
    int rc = -1;
    lock_rec(users_fd, sizeof(User), id - 1, F_WRLCK);
    User u;
    if (rec_read(users_fd, id - 1, &u, sizeof u) == 0 && u.active) {
        u.active = 0;
        if (rec_write(users_fd, id - 1, &u, sizeof u) == 0) { fsync(users_fd); rc = 0; }
    }
    lock_rec(users_fd, sizeof(User), id - 1, F_UNLCK);
    return rc;
}

/* 0 ok, -1 wrong old password / no such user, -2 disk error */
int user_change_password(int id, const char *old_pw, const char *new_pw)
{
    if (id < 1 || id > user_total()) return -1;
    int rc = -1;
    lock_rec(users_fd, sizeof(User), id - 1, F_WRLCK);
    User u;
    char h[20];
    if (rec_read(users_fd, id - 1, &u, sizeof u) == 0 && u.active) {
        hash_password(old_pw, h);
        if (strcmp(h, u.pass_hash) == 0) {
            hash_password(new_pw, u.pass_hash);
            rc = (rec_write(users_fd, id - 1, &u, sizeof u) == 0) ? 0 : -2;
            if (rc == 0) fsync(users_fd);
        }
    }
    lock_rec(users_fd, sizeof(User), id - 1, F_UNLCK);
    return rc;
}

/* ------------------------------------------------------------------ */
/* accounts                                                             */
/* ------------------------------------------------------------------ */

/* Shared (read) lock while looking, so we never read a balance that a
 * withdraw/deposit in another process is in the middle of changing. */
int acct_get(int acc_no, Account *out)
{
    int idx = acc_no - ACC_BASE;
    if (idx < 0 || idx >= rec_count(acc_fd, sizeof(Account))) return -1;
    lock_rec(acc_fd, sizeof(Account), idx, F_RDLCK);
    int r = rec_read(acc_fd, idx, out, sizeof(Account));
    lock_rec(acc_fd, sizeof(Account), idx, F_UNLCK);
    return r;
}

/* account where this user is owner or joint holder */
int acct_for_user(int user_id, Account *out)
{
    int n = rec_count(acc_fd, sizeof(Account));
    for (int i = 0; i < n; i++) {
        Account a;
        if (rec_read(acc_fd, i, &a, sizeof a) < 0) continue;
        if (a.owner_id == user_id || a.joint_id == user_id) {
            return acct_get(a.acc_no, out);
        }
    }
    return -1;
}

int acct_create(int owner, int joint, int *acc_no)
{
    int rc = -1;
    lock_range(acc_fd, F_WRLCK, TAIL_LOCK_OFF, 1, 1);
    int n = rec_count(acc_fd, sizeof(Account));
    Account a;
    memset(&a, 0, sizeof a);
    a.acc_no = ACC_BASE + n;
    a.owner_id = owner;
    a.joint_id = joint;
    a.active = 1;
    if (rec_write(acc_fd, n, &a, sizeof a) == 0) {
        fsync(acc_fd);
        if (acc_no) *acc_no = a.acc_no;
        rc = 0;
    }
    lock_range(acc_fd, F_UNLCK, TAIL_LOCK_OFF, 1, 0);
    return rc;
}

int acct_set_active(int acc_no, int active)
{
    int idx = acc_no - ACC_BASE, rc = -1;
    if (idx < 0 || idx >= rec_count(acc_fd, sizeof(Account))) return -1;
    lock_rec(acc_fd, sizeof(Account), idx, F_WRLCK);
    Account a;
    if (rec_read(acc_fd, idx, &a, sizeof a) == 0) {
        a.active = active;
        if (rec_write(acc_fd, idx, &a, sizeof a) == 0) { fsync(acc_fd); rc = 0; }
    }
    lock_rec(acc_fd, sizeof(Account), idx, F_UNLCK);
    return rc;
}

/* ------------------------------------------------------------------ */
/* transaction history                                                  */
/* ------------------------------------------------------------------ */

/* Last `max` rows of one account, oldest first. Returns how many. */
int txn_last(int acc_no, Txn *out, int max)
{
    int count = 0;
    lock_range(txn_fd, F_RDLCK, 0, 0, 1);
    int n = rec_count(txn_fd, sizeof(Txn));
    for (int i = 0; i < n; i++) {
        Txn t;
        if (rec_read(txn_fd, i, &t, sizeof t) < 0 || t.acc_no != acc_no) continue;
        if (count < max) {
            out[count++] = t;
        } else {                                  /* slide the window */
            memmove(out, out + 1, (size_t)(max - 1) * sizeof(Txn));
            out[max - 1] = t;
        }
    }
    lock_range(txn_fd, F_UNLCK, 0, 0, 0);
    return count;
}

/* ------------------------------------------------------------------ */
/* loans                                                                */
/* ------------------------------------------------------------------ */

int loan_total(void) { return rec_count(loan_fd, sizeof(Loan)); }

int loan_at(int index, Loan *out) { return rec_read(loan_fd, index, out, sizeof(Loan)); }

int loan_get(int loan_id, Loan *out)
{
    if (loan_id < 1 || loan_id > loan_total()) return -1;
    return rec_read(loan_fd, loan_id - 1, out, sizeof(Loan));
}

int loan_create(int acc_no, money amt, int *loan_id)
{
    int rc = -1;
    lock_range(loan_fd, F_WRLCK, TAIL_LOCK_OFF, 1, 1);
    int n = loan_total();
    Loan l;
    memset(&l, 0, sizeof l);
    l.loan_id = n + 1;
    l.acc_no = acc_no;
    l.amount = amt;
    l.status = LOAN_PENDING;
    l.applied = time(NULL);
    if (rec_write(loan_fd, n, &l, sizeof l) == 0) {
        fsync(loan_fd);
        if (loan_id) *loan_id = l.loan_id;
        rc = 0;
    }
    lock_range(loan_fd, F_UNLCK, TAIL_LOCK_OFF, 1, 0);
    return rc;
}

/* manager gives a loan to an employee. 0 ok, -1 not found, -2 already decided */
int loan_assign(int loan_id, int employee_id)
{
    if (loan_id < 1 || loan_id > loan_total()) return -1;
    int rc = -1;
    lock_rec(loan_fd, sizeof(Loan), loan_id - 1, F_WRLCK);
    Loan l;
    if (rec_read(loan_fd, loan_id - 1, &l, sizeof l) == 0) {
        if (l.status == LOAN_APPROVED || l.status == LOAN_REJECTED) {
            rc = -2;
        } else {
            l.status = LOAN_ASSIGNED;
            l.employee_id = employee_id;
            if (rec_write(loan_fd, loan_id - 1, &l, sizeof l) == 0) { fsync(loan_fd); rc = 0; }
        }
    }
    lock_rec(loan_fd, sizeof(Loan), loan_id - 1, F_UNLCK);
    return rc;
}

/*
 * Employee approves or rejects.  Approving credits the customer's account
 * and flips the loan status inside the SAME journal entry, so we can never
 * end up with money paid out but the loan still showing "assigned".
 * Returns 0 ok, -1 not found, -2 not yours / already decided, -3 bank error
 * (the bank error code goes to *bank_rc).
 */
int loan_decide(int loan_id, int employee_id, int approve, int *bank_rc)
{
    if (loan_id < 1 || loan_id > loan_total()) return -1;
    int rc = -1;
    lock_rec(loan_fd, sizeof(Loan), loan_id - 1, F_WRLCK);

    Loan l;
    if (rec_read(loan_fd, loan_id - 1, &l, sizeof l) == 0) {
        if (l.status != LOAN_ASSIGNED || l.employee_id != employee_id) {
            rc = -2;
        } else if (approve) {
            Loan after = l;
            after.status = LOAN_APPROVED;
            Leg leg = { l.acc_no, l.amount, "LOAN_CREDIT" };
            int b = apply_legs(employee_id, &leg, 1, &after, NULL);
            if (b == B_OK) rc = 0;
            else { rc = -3; if (bank_rc) *bank_rc = b; }
        } else {
            l.status = LOAN_REJECTED;
            if (rec_write(loan_fd, loan_id - 1, &l, sizeof l) == 0) { fsync(loan_fd); rc = 0; }
        }
    }
    lock_rec(loan_fd, sizeof(Loan), loan_id - 1, F_UNLCK);
    return rc;
}

/* ------------------------------------------------------------------ */
/* feedback                                                             */
/* ------------------------------------------------------------------ */

int fb_total(void) { return rec_count(fb_fd, sizeof(Feedback)); }

int fb_at(int index, Feedback *out) { return rec_read(fb_fd, index, out, sizeof(Feedback)); }

int fb_add(int user_id, const char *text)
{
    int rc = -1;
    lock_range(fb_fd, F_WRLCK, TAIL_LOCK_OFF, 1, 1);
    int n = fb_total();
    Feedback f;
    memset(&f, 0, sizeof f);
    f.id = n + 1;
    f.user_id = user_id;
    snprintf(f.text, sizeof f.text, "%s", text);
    if (rec_write(fb_fd, n, &f, sizeof f) == 0) { fsync(fb_fd); rc = 0; }
    lock_range(fb_fd, F_UNLCK, TAIL_LOCK_OFF, 1, 0);
    return rc;
}

int fb_mark_reviewed(int fb_id)
{
    if (fb_id < 1 || fb_id > fb_total()) return -1;
    int rc = -1;
    lock_rec(fb_fd, sizeof(Feedback), fb_id - 1, F_WRLCK);
    Feedback f;
    if (rec_read(fb_fd, fb_id - 1, &f, sizeof f) == 0) {
        f.reviewed = 1;
        if (rec_write(fb_fd, fb_id - 1, &f, sizeof f) == 0) { fsync(fb_fd); rc = 0; }
    }
    lock_rec(fb_fd, sizeof(Feedback), fb_id - 1, F_UNLCK);
    return rc;
}

/* ------------------------------------------------------------------ */
/* one login per user                                                   */
/* ------------------------------------------------------------------ */

/* Holding a lock on byte <user id> of sessions.lock means "this user is
 * logged in".  If a client process crashes the kernel drops the lock by
 * itself, so nobody stays locked out. */
int session_enter(int user_id)
{
    return lock_range(sess_fd, F_WRLCK, user_id, 1, 0);
}

void session_leave(int user_id)
{
    lock_range(sess_fd, F_UNLCK, user_id, 1, 0);
}

/* ------------------------------------------------------------------ */
/* first-time setup                                                     */
/* ------------------------------------------------------------------ */

static void seed_demo_data(void)
{
    int id, acc, a, b;

    user_add("admin",    "admin123", ROLE_ADMIN,    &id);
    user_add("manager1", "manager123", ROLE_MANAGER, &id);
    user_add("emp1",     "emp123",   ROLE_EMPLOYEE, &id);

    /* a joint account: asha + ravi */
    user_add("asha", "asha123", ROLE_CUSTOMER, &a);
    user_add("ravi", "ravi123", ROLE_CUSTOMER, &b);
    acct_create(a, b, &acc);
    bank_open_deposit(acc, a, 500000);        /* Rs. 5000.00 */

    user_add("meera", "meera123", ROLE_CUSTOMER, &a);
    acct_create(a, 0, &acc);
    bank_open_deposit(acc, a, 300000);        /* Rs. 3000.00 */

    user_add("kiran", "kiran123", ROLE_CUSTOMER, &a);
    acct_create(a, 0, &acc);
    bank_open_deposit(acc, a, 100000);        /* Rs. 1000.00 */
}

int store_init(void)
{
    mkdir(g_data_dir, 0755);
    if (store_open() < 0) return -1;

    if (rec_count(jr_fd, sizeof(Journal)) == 0) {   /* brand new journal */
        Journal empty;
        memset(&empty, 0, sizeof empty);
        rec_write(jr_fd, 0, &empty, sizeof empty);
        fsync(jr_fd);
    }

    if (store_recover())
        fprintf(stderr, "[store] found an unfinished transaction in the journal - redone.\n");

    if (user_total() == 0) {
        seed_demo_data();
        fprintf(stderr, "[store] first run: demo users created (see README).\n");
    }

    store_close();
    return 0;
}
