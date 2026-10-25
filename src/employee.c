/*
 * employee.c - bank employee: opens/edits customers, works on loans that the
 * manager has handed to them.
 */
#include "bank.h"

#include <stdio.h>

static int act_my_loans(int fd, const User *u)
{
    int shown = 0, n = loan_total();
    for (int i = 0; i < n; i++) {
        Loan l;
        if (loan_at(i, &l) < 0 || l.employee_id != u->id) continue;
        char b[40];
        fmt_money(l.amount, b, sizeof b);
        net_send(fd, "Loan #%d  account %d  %s  [%s]\n",
                 l.loan_id, l.acc_no, b, loan_status_name(l.status));
        shown++;
    }
    if (!shown) net_send(fd, "No loans are assigned to you.\n");
    return 0;
}

static int act_process_loan(int fd, const User *u)
{
    long id, decision;
    int r = net_ask_number(fd, "Loan id: ", &id);
    if (r != 0) return r < 0 ? -1 : 0;

    Loan l;
    if (loan_get((int)id, &l) < 0) { net_send(fd, "No such loan.\n"); return 0; }
    char b[40];
    fmt_money(l.amount, b, sizeof b);
    net_send(fd, "Loan #%d for account %d, %s, status %s\n",
             l.loan_id, l.acc_no, b, loan_status_name(l.status));

    r = net_ask_number(fd, "1 = approve, 0 = reject: ", &decision);
    if (r != 0) return r < 0 ? -1 : 0;
    if (decision != 0 && decision != 1) { net_send(fd, "Please type 1 or 0.\n"); return 0; }

    int bank_rc = 0;
    int rc = loan_decide((int)id, u->id, (int)decision, &bank_rc);
    if (rc == 0) {
        net_send(fd, decision ? "Loan approved and the amount is credited.\n" : "Loan rejected.\n");
        log_event("%s %s loan %d", u->username, decision ? "approved" : "rejected", (int)id);
    } else if (rc == -2) {
        net_send(fd, "That loan is not assigned to you (or is already decided).\n");
    } else if (rc == -3) {
        net_send(fd, "Could not credit the loan: %s\n", bank_strerror(bank_rc));
    } else {
        net_send(fd, "No such loan.\n");
    }
    return 0;
}

int employee_menu(int fd, const User *u)
{
    for (;;) {
        net_send(fd,
            "\n===== Employee Menu (%s) =====\n"
            "1. Add customer account\n"
            "2. Modify customer login\n"
            "3. Search account / view transactions\n"
            "4. My assigned loans\n"
            "5. Approve / reject a loan\n"
            "6. Change password\n"
            "7. Logout\n", u->username);

        int c = net_ask_choice(fd, "Choice: ");
        if (c < 0) return -1;

        int r = 0;
        switch (c) {
        case 1: r = flow_add_customer(fd);            break;
        case 2: r = flow_modify_user(fd, ROLE_CUSTOMER); break;
        case 3: r = flow_search_account(fd);          break;
        case 4: r = act_my_loans(fd, u);              break;
        case 5: r = act_process_loan(fd, u);          break;
        case 6: r = flow_change_password(fd, u);      break;
        case 7: return 0;
        default: net_send(fd, "Please pick a number from the menu.\n");
        }
        if (r < 0) return -1;
    }
}
