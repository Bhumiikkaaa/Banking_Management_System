/*
 * test_recovery.c - proves Atomicity + Durability.
 *
 * A child process starts a transfer and "crashes" (_exit) right after the
 * journal is written but BEFORE the account files are updated.  We then start
 * the store again (like restarting the server) and check that recovery
 * finished the transfer completely - no money lost, none created.
 */
#include "bank.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  PASS  %s\n", msg); \
    else { printf("  FAIL  %s\n", msg); failures++; } } while (0)

int main(void)
{
    snprintf(g_data_dir, sizeof g_data_dir, "testdata_recovery");
    if (system("rm -rf testdata_recovery")) { }

    printf("== crash recovery test ==\n");
    if (store_init() < 0) { printf("init failed\n"); return 1; }
    store_open();

    Account a, b;
    acct_get(ACC_BASE, &a);          /* asha/ravi joint, Rs 5000 */
    acct_get(ACC_BASE + 1, &b);      /* meera,           Rs 3000 */
    money total_before = a.balance + b.balance;
    store_close();

    pid_t pid = fork();
    if (pid == 0) {
        setenv("BANK_CRASH_TEST", "1", 1);
        store_open();
        bank_transfer(ACC_BASE, ACC_BASE + 1, 1, 100000, NULL);   /* Rs 1000 */
        _exit(0);                                                  /* not reached */
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 99, "child crashed in the middle of the transfer");

    /* look at the files exactly as the crash left them */
    store_open();
    acct_get(ACC_BASE, &a);
    CHECK(a.balance == 500000, "before recovery: sender still shows the old balance");
    store_close();

    /* "restart the server" */
    store_init();
    store_open();
    acct_get(ACC_BASE, &a);
    acct_get(ACC_BASE + 1, &b);
    CHECK(a.balance == 400000, "after recovery: sender debited Rs 1000");
    CHECK(b.balance == 400000, "after recovery: receiver credited Rs 1000");
    CHECK(a.balance + b.balance == total_before, "total money unchanged");

    Txn t[10];
    int n = txn_last(ACC_BASE, t, 10);
    CHECK(n >= 2 && strcmp(t[n - 1].type, "TRANSFER_OUT") == 0, "history row written for the transfer");

    /* recovery must be harmless when run again */
    CHECK(store_recover() == 0, "second recovery finds nothing to do");
    store_close();

    if (system("rm -rf testdata_recovery")) { }
    printf(failures ? "RESULT: %d check(s) failed\n" : "RESULT: all recovery checks passed\n", failures);
    return failures ? 1 : 0;
}
