/*
 * bank.h - shared types and function prototypes
 * Online Banking Management System (EG 301 Operating Systems mini project)
 *
 * Money is always kept as a whole number of paise (1 rupee = 100 paise)
 * so that we never lose a paisa to floating point rounding.
 */
#ifndef BANK_H
#define BANK_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#define DEFAULT_PORT      8090
#define DEFAULT_DATA_DIR  "data"

#define ROLE_CUSTOMER 1
#define ROLE_EMPLOYEE 2
#define ROLE_MANAGER  3
#define ROLE_ADMIN    4

#define ACC_BASE        10001      /* first account number                */
#define MAX_LOAN_RUPEES 500000     /* bank policy: biggest loan we allow  */
#define MIN_PASS_LEN    4
#define MAX_LOGIN_TRIES 3

typedef long long money;           /* paise */

/* ---------- records stored in the data files (fixed size) ---------- */

typedef struct {
    int  id;                 /* 1, 2, 3 ... (position in users.dat + 1)  */
    char username[24];
    char pass_hash[20];
    int  role;
    int  active;             /* 1 = exists, 0 = deleted by admin         */
} User;

typedef struct {
    int   acc_no;
    int   owner_id;          /* primary holder                           */
    int   joint_id;          /* second holder, 0 if not a joint account  */
    int   active;            /* manager can freeze an account            */
    money balance;
} Account;

typedef struct {
    int   txn_id;
    int   acc_no;
    int   user_id;           /* who did it (matters for joint accounts)  */
    char  type[16];          /* DEPOSIT, WITHDRAW, TRANSFER_OUT, ...     */
    money amount;
    money balance_after;
    time_t when;
} Txn;

#define LOAN_PENDING  0
#define LOAN_ASSIGNED 1
#define LOAN_APPROVED 2
#define LOAN_REJECTED 3

typedef struct {
    int   loan_id;
    int   acc_no;
    money amount;
    int   status;
    int   employee_id;       /* 0 until a manager assigns it             */
    time_t applied;
} Loan;

typedef struct {
    int  id;
    int  user_id;
    char text[120];
    int  reviewed;
} Feedback;

/* ---------- error codes returned by the money functions ---------- */
enum { B_OK = 0, B_NOACC, B_INACTIVE, B_FUNDS, B_BADAMT, B_SAMEACC, B_IOERR };
const char *bank_strerror(int code);

/* ---------- store.c : files, locks, journal ---------- */
extern char g_data_dir[128];

int  store_init(void);       /* create files, seed data, run recovery    */
int  store_open(void);       /* every process opens its own descriptors  */
void store_close(void);
int  store_recover(void);    /* returns 1 if a half-done txn was redone  */

void hash_password(const char *pw, char out[20]);

int  user_check_login(int role, const char *name, const char *pw, User *out);
int  user_get(int id, User *out);
int  user_find_by_name(const char *name, User *out);
int  user_add(const char *name, const char *pw, int role, int *new_id);
int  user_update(int id, const char *new_name, const char *new_pw);
int  user_delete(int id);
int  user_change_password(int id, const char *old_pw, const char *new_pw);
int  user_total(void);
int  user_at(int index, User *out);

int  acct_get(int acc_no, Account *out);          /* takes a read lock   */
int  acct_for_user(int user_id, Account *out);
int  acct_create(int owner, int joint, int *acc_no);
int  acct_set_active(int acc_no, int active);

int  bank_deposit(int acc_no, int user_id, money amt, money *new_bal);
int  bank_open_deposit(int acc_no, int user_id, money amt);
int  bank_withdraw(int acc_no, int user_id, money amt, money *new_bal);
int  bank_transfer(int from, int to, int user_id, money amt, money *new_bal);
int  txn_last(int acc_no, Txn *out, int max);

int  loan_create(int acc_no, money amt, int *loan_id);
int  loan_get(int loan_id, Loan *out);
int  loan_total(void);
int  loan_at(int index, Loan *out);
int  loan_assign(int loan_id, int employee_id);
int  loan_decide(int loan_id, int employee_id, int approve, int *bank_rc);

int  fb_add(int user_id, const char *text);
int  fb_total(void);
int  fb_at(int index, Feedback *out);
int  fb_mark_reviewed(int fb_id);

int  session_enter(int user_id);   /* 0 ok, -1 already logged in somewhere */
void session_leave(int user_id);

/* ---------- net.c : socket helpers ---------- */
extern int g_log_fd;
int  net_send(int fd, const char *fmt, ...);
int  net_ask(int fd, const char *prompt, char *buf, size_t size);   /* -1 = client gone */
int  net_ask_choice(int fd, const char *prompt);    /* number >=1, 0 = junk, -1 = gone */
int  net_ask_number(int fd, const char *prompt, long *out);         /* 0 ok, 1 bad, -1 gone */
int  net_ask_money(int fd, const char *prompt, money *out);         /* 0 ok, 1 bad, -1 gone */
int  parse_money(const char *s, money *out);
void fmt_money(money m, char *buf, size_t n);
void log_event(const char *fmt, ...);

/* ---------- shared menu flows (flows.c) ---------- */
int  valid_username(const char *s);
int  flow_add_customer(int fd);
int  flow_modify_user(int fd, int only_role);       /* only_role 0 = any */
int  flow_search_account(int fd);
int  flow_change_password(int fd, const User *u);
void show_account(int fd, const Account *a);
const char *role_name(int role);
const char *loan_status_name(int s);

/* ---------- one menu per role ---------- */
int  customer_menu(int fd, const User *u);          /* each returns 0 = logout, -1 = client gone */
int  employee_menu(int fd, const User *u);
int  manager_menu(int fd, const User *u);
int  admin_menu(int fd, const User *u);

#endif
