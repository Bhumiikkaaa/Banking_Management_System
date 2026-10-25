/*
 * manager.c - manager: freezes accounts, hands loans to employees,
 * reads customer feedback.
 */
#include "bank.h"

#include <stdio.h>

static int act_set_active(int fd)
{
    long acc, flag;
    int r = net_ask_number(fd, "Account number: ", &acc);
    if (r != 0) return r < 0 ? -1 : 0;
    r = net_ask_number(fd, "1 = activate, 0 = deactivate: ", &flag);
    if (r != 0) return r < 0 ? -1 : 0;
    if (flag != 0 && flag != 1) { net_send(fd, "Please type 1 or 0.\n"); return 0; }

    if (acct_set_active((int)acc, (int)flag) == 0) {
        net_send(fd, "Account %ld is now %s.\n", acc, flag ? "active" : "deactivated");
        log_event("account %ld set active=%ld", acc, flag);
    } else {
        net_send(fd, "Account not found.\n");
    }
    return 0;
}

static int act_open_loans(int fd)
{
    int shown = 0, n = loan_total();
    for (int i = 0; i < n; i++) {
        Loan l;
        if (loan_at(i, &l) < 0) continue;
        if (l.status != LOAN_PENDING && l.status != LOAN_ASSIGNED) continue;
        char b[40];
        fmt_money(l.amount, b, sizeof b);
        net_send(fd, "Loan #%d  account %d  %s  [%s]  employee %d\n",
                 l.loan_id, l.acc_no, b, loan_status_name(l.status), l.employee_id);
        shown++;
    }
    if (!shown) net_send(fd, "No open loan applications.\n");
    return 0;
}

static int act_assign_loan(int fd)
{
    long lid, eid;
    int r = net_ask_number(fd, "Loan id: ", &lid);
    if (r != 0) return r < 0 ? -1 : 0;
    r = net_ask_number(fd, "Employee user id: ", &eid);
    if (r != 0) return r < 0 ? -1 : 0;

    User e;
    if (user_get((int)eid, &e) < 0 || e.role != ROLE_EMPLOYEE) {
        net_send(fd, "That id is not an employee.\n");
        return 0;
    }
    int rc = loan_assign((int)lid, e.id);
    if (rc == 0) {
        net_send(fd, "Loan %ld assigned to %s.\n", lid, e.username);
        log_event("loan %ld assigned to %s", lid, e.username);
    } else if (rc == -2) {
        net_send(fd, "That loan is already decided.\n");
    } else {
        net_send(fd, "No such loan.\n");
    }
    return 0;
}

static int act_feedback(int fd)
{
    int n = fb_total();
    if (n == 0) { net_send(fd, "No feedback yet.\n"); return 0; }

    for (int i = 0; i < n; i++) {
        Feedback f;
        User u;
        if (fb_at(i, &f) < 0) continue;
        const char *who = (user_get(f.user_id, &u) == 0) ? u.username : "-";
        net_send(fd, "#%d [%s] from %s: %s\n", f.id, f.reviewed ? "seen" : "NEW", who, f.text);
    }
    long id;
    int r = net_ask_number(fd, "Feedback id to mark as reviewed (0 = none): ", &id);
    if (r != 0) return r < 0 ? -1 : 0;
    if (id > 0) {
        if (fb_mark_reviewed((int)id) == 0) net_send(fd, "Marked as reviewed.\n");
        else net_send(fd, "No such feedback.\n");
    }
    return 0;
}

int manager_menu(int fd, const User *u)
{
    for (;;) {
        net_send(fd,
            "\n===== Manager Menu (%s) =====\n"
            "1. Activate / deactivate an account\n"
            "2. Open loan applications\n"
            "3. Assign a loan to an employee\n"
            "4. Customer feedback\n"
            "5. Change password\n"
            "6. Logout\n", u->username);

        int c = net_ask_choice(fd, "Choice: ");
        if (c < 0) return -1;

        int r = 0;
        switch (c) {
        case 1: r = act_set_active(fd);          break;
        case 2: r = act_open_loans(fd);          break;
        case 3: r = act_assign_loan(fd);         break;
        case 4: r = act_feedback(fd);            break;
        case 5: r = flow_change_password(fd, u); break;
        case 6: return 0;
        default: net_send(fd, "Please pick a number from the menu.\n");
        }
        if (r < 0) return -1;
    }
}
