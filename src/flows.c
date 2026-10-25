/*
 * flows.c - menu actions that more than one role needs
 * (adding a customer, searching an account, changing a password ...)
 */
#include "bank.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *role_name(int role)
{
    switch (role) {
    case ROLE_CUSTOMER: return "Customer";
    case ROLE_EMPLOYEE: return "Employee";
    case ROLE_MANAGER:  return "Manager";
    case ROLE_ADMIN:    return "Admin";
    }
    return "?";
}

const char *loan_status_name(int s)
{
    switch (s) {
    case LOAN_PENDING:  return "pending";
    case LOAN_ASSIGNED: return "assigned";
    case LOAN_APPROVED: return "approved";
    case LOAN_REJECTED: return "rejected";
    }
    return "?";
}

/* 3-20 chars, letters / digits / underscore only */
int valid_username(const char *s)
{
    size_t len = strlen(s);
    if (len < 3 || len > 20) return 0;
    for (size_t i = 0; i < len; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') return 0;
    return 1;
}

static const char *name_of(int uid, char *buf, size_t n)
{
    User u;
    if (uid > 0 && user_get(uid, &u) == 0) snprintf(buf, n, "%s", u.username);
    else snprintf(buf, n, "-");
    return buf;
}

void show_account(int fd, const Account *a)
{
    char own[32], jnt[32], bal[40];
    fmt_money(a->balance, bal, sizeof bal);
    net_send(fd, "\nAccount no : %d (%s)\n", a->acc_no, a->joint_id ? "joint" : "single");
    net_send(fd, "Holder     : %s\n", name_of(a->owner_id, own, sizeof own));
    if (a->joint_id)
        net_send(fd, "Joint with : %s\n", name_of(a->joint_id, jnt, sizeof jnt));
    net_send(fd, "Status     : %s\n", a->active ? "active" : "DEACTIVATED");
    net_send(fd, "Balance    : %s\n", bal);
}

/* reads a username + password pair and creates the user. 1 ok, 0 failed, -1 gone */
static int ask_new_login(int fd, const char *label, int *new_id)
{
    char name[64], pw[64];
    char prompt[64];

    snprintf(prompt, sizeof prompt, "%s username: ", label);
    if (net_ask(fd, prompt, name, sizeof name) < 0) return -1;
    if (!valid_username(name)) {
        net_send(fd, "Username must be 3-20 letters, digits or _.\n");
        return 0;
    }
    snprintf(prompt, sizeof prompt, "%s password: ", label);
    if (net_ask(fd, prompt, pw, sizeof pw) < 0) return -1;
    if (strlen(pw) < MIN_PASS_LEN) {
        net_send(fd, "Password needs at least %d characters.\n", MIN_PASS_LEN);
        return 0;
    }
    int rc = user_add(name, pw, ROLE_CUSTOMER, new_id);
    if (rc == -1) { net_send(fd, "That username is already taken.\n"); return 0; }
    if (rc < 0)   { net_send(fd, "Disk error.\n"); return 0; }
    return 1;
}

/* used by employee and admin: new login + account (+ optional joint holder) */
int flow_add_customer(int fd)
{
    int owner = 0, joint = 0, acc = 0;
    char buf[16];

    int r = ask_new_login(fd, "Customer", &owner);
    if (r <= 0) return r;

    if (net_ask(fd, "Joint account? (y/n): ", buf, sizeof buf) < 0) return -1;
    if (buf[0] == 'y' || buf[0] == 'Y') {
        r = ask_new_login(fd, "Second holder", &joint);
        if (r < 0) return -1;
        if (r == 0) {
            net_send(fd, "Joint holder not created - making a single account instead.\n");
            joint = 0;
        }
    }

    money opening = 0;
    r = net_ask_money(fd, "Opening deposit in Rs.: ", &opening);
    if (r < 0) return -1;
    if (r > 0) opening = 0;            /* bad amount: open with zero balance */

    if (acct_create(owner, joint, &acc) < 0) {
        net_send(fd, "Could not create the account.\n");
        return 0;
    }
    if (opening > 0) bank_open_deposit(acc, owner, opening);

    net_send(fd, "Done! Account number %d created.\n", acc);
    log_event("account %d created (owner uid %d, joint uid %d)", acc, owner, joint);
    return 0;
}

/* change username and/or password of somebody else. only_role = 0 means any role */
int flow_modify_user(int fd, int only_role)
{
    long id;
    char name[64], pw[64];

    int r = net_ask_number(fd, "User id to modify: ", &id);
    if (r != 0) return r < 0 ? -1 : 0;

    User u;
    if (user_get((int)id, &u) < 0 || (only_role && u.role != only_role)) {
        net_send(fd, "No such user.\n");
        return 0;
    }
    net_send(fd, "Editing %s (%s). Type - to keep a field as it is.\n", u.username, role_name(u.role));

    if (net_ask(fd, "New username: ", name, sizeof name) < 0) return -1;
    if (net_ask(fd, "New password: ", pw, sizeof pw) < 0) return -1;

    const char *nn = (strcmp(name, "-") == 0 || name[0] == '\0') ? NULL : name;
    const char *np = (strcmp(pw, "-") == 0 || pw[0] == '\0') ? NULL : pw;
    if (nn && !valid_username(nn)) { net_send(fd, "Invalid username.\n"); return 0; }
    if (np && strlen(np) < MIN_PASS_LEN) { net_send(fd, "Password too short.\n"); return 0; }

    int rc = user_update(u.id, nn, np);
    if (rc == 0) {
        net_send(fd, "Updated.\n");
        log_event("user %d modified", u.id);
    } else if (rc == -2) {
        net_send(fd, "That username is already taken.\n");
    } else {
        net_send(fd, "Update failed.\n");
    }
    return 0;
}

/* accepts an account number or a username */
int flow_search_account(int fd)
{
    char in[64];
    if (net_ask(fd, "Account number or username: ", in, sizeof in) < 0) return -1;

    Account a;
    char *end;
    long num = strtol(in, &end, 10);
    int found = -1;

    if (*end == '\0' && num >= ACC_BASE) {
        found = acct_get((int)num, &a);
    } else {
        User u;
        if (user_find_by_name(in, &u) == 0) found = acct_for_user(u.id, &a);
    }
    if (found < 0) {
        net_send(fd, "No matching account.\n");
        return 0;
    }

    show_account(fd, &a);

    Txn t[5];
    int n = txn_last(a.acc_no, t, 5);
    net_send(fd, "\nLast %d transaction(s):\n", n);
    for (int i = 0; i < n; i++) {
        char amt[40], bal[40], when[32];
        struct tm tmv;
        localtime_r(&t[i].when, &tmv);
        strftime(when, sizeof when, "%d-%m-%Y %H:%M", &tmv);
        fmt_money(t[i].amount, amt, sizeof amt);
        fmt_money(t[i].balance_after, bal, sizeof bal);
        net_send(fd, "  %s  %-12s %-14s balance %s\n", when, t[i].type, amt, bal);
    }
    return 0;
}

int flow_change_password(int fd, const User *u)
{
    char old_pw[64], new_pw[64];
    if (net_ask(fd, "Current password: ", old_pw, sizeof old_pw) < 0) return -1;
    if (net_ask(fd, "New password: ", new_pw, sizeof new_pw) < 0) return -1;
    if (strlen(new_pw) < MIN_PASS_LEN) {
        net_send(fd, "New password needs at least %d characters.\n", MIN_PASS_LEN);
        return 0;
    }
    if (user_change_password(u->id, old_pw, new_pw) == 0) {
        net_send(fd, "Password changed.\n");
        log_event("%s changed password", u->username);
    } else {
        net_send(fd, "Current password is wrong.\n");
    }
    return 0;
}
