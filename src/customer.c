/*
 * customer.c - menu for normal and joint-account customers.
 * Both kinds of customer use the same code: a joint account simply has two
 * users pointing at it, and the file locks in store.c keep them from
 * trampling on each other.
 */
#include "bank.h"

#include <stdio.h>
#include <string.h>

static int my_account(int fd, const User *u, Account *a)
{
    if (acct_for_user(u->id, a) < 0) {
        net_send(fd, "No account is linked to your login. Please visit the branch.\n");
        return -1;
    }
    return 0;
}

static int act_balance(int fd, const User *u)
{
    Account a;
    char b[40];
    if (my_account(fd, u, &a) < 0) return 0;
    fmt_money(a.balance, b, sizeof b);       /* acct_for_user took a read lock */
    net_send(fd, "Current balance: %s\n", b);
    return 0;
}

static int act_deposit(int fd, const User *u)
{
    Account a;
    money amt, nb;
    if (my_account(fd, u, &a) < 0) return 0;
    int r = net_ask_money(fd, "Amount to deposit (Rs.): ", &amt);
    if (r != 0) return r < 0 ? -1 : 0;

    int rc = bank_deposit(a.acc_no, u->id, amt, &nb);
    if (rc != B_OK) { net_send(fd, "%s\n", bank_strerror(rc)); return 0; }

    char b[40];
    fmt_money(nb, b, sizeof b);
    net_send(fd, "Deposit successful. New balance: %s\n", b);
    log_event("%s deposited %lld paise into %d", u->username, amt, a.acc_no);
    return 0;
}

static int act_withdraw(int fd, const User *u)
{
    Account a;
    money amt, nb;
    if (my_account(fd, u, &a) < 0) return 0;
    int r = net_ask_money(fd, "Amount to withdraw (Rs.): ", &amt);
    if (r != 0) return r < 0 ? -1 : 0;

    int rc = bank_withdraw(a.acc_no, u->id, amt, &nb);
    if (rc != B_OK) { net_send(fd, "%s\n", bank_strerror(rc)); return 0; }

    char b[40];
    fmt_money(nb, b, sizeof b);
    net_send(fd, "Please collect your cash. New balance: %s\n", b);
    log_event("%s withdrew %lld paise from %d", u->username, amt, a.acc_no);
    return 0;
}

static int act_transfer(int fd, const User *u)
{
    Account a, to;
    long target;
    money amt, nb;
    if (my_account(fd, u, &a) < 0) return 0;

    int r = net_ask_number(fd, "Receiver account number: ", &target);
    if (r != 0) return r < 0 ? -1 : 0;
    if (acct_get((int)target, &to) < 0) { net_send(fd, "%s\n", bank_strerror(B_NOACC)); return 0; }

    r = net_ask_money(fd, "Amount to transfer (Rs.): ", &amt);
    if (r != 0) return r < 0 ? -1 : 0;

    int rc = bank_transfer(a.acc_no, (int)target, u->id, amt, &nb);
    if (rc != B_OK) { net_send(fd, "%s\n", bank_strerror(rc)); return 0; }

    char b[40];
    fmt_money(nb, b, sizeof b);
    net_send(fd, "Transfer done. New balance: %s\n", b);
    log_event("%s moved %lld paise from %d to %d", u->username, amt, a.acc_no, (int)target);
    return 0;
}

static int act_details(int fd, const User *u)
{
    Account a;
    if (my_account(fd, u, &a) < 0) return 0;
    show_account(fd, &a);
    return 0;
}

static int act_statement(int fd, const User *u)
{
    Account a;
    Txn t[10];
    if (my_account(fd, u, &a) < 0) return 0;

    int n = txn_last(a.acc_no, t, 10);
    if (n == 0) { net_send(fd, "No transactions yet.\n"); return 0; }

    net_send(fd, "\nLast %d transaction(s):\n", n);
    for (int i = 0; i < n; i++) {
        char amt[40], bal[40], when[32];
        struct tm tmv;
        User who;
        localtime_r(&t[i].when, &tmv);
        strftime(when, sizeof when, "%d-%m-%Y %H:%M", &tmv);
        fmt_money(t[i].amount, amt, sizeof amt);
        fmt_money(t[i].balance_after, bal, sizeof bal);
        const char *by = (user_get(t[i].user_id, &who) == 0) ? who.username : "-";
        net_send(fd, "#%-4d %s  %-12s %-14s bal %-14s by %s\n",
                 t[i].txn_id, when, t[i].type, amt, bal, by);
    }
    return 0;
}

static int act_apply_loan(int fd, const User *u)
{
    Account a;
    money amt;
    if (my_account(fd, u, &a) < 0) return 0;
    if (!a.active) { net_send(fd, "%s\n", bank_strerror(B_INACTIVE)); return 0; }

    int r = net_ask_money(fd, "Loan amount (Rs.): ", &amt);
    if (r != 0) return r < 0 ? -1 : 0;
    if (amt > (money)MAX_LOAN_RUPEES * 100) {
        net_send(fd, "We cannot give more than Rs. %d as a loan.\n", MAX_LOAN_RUPEES);
        return 0;
    }
    int id;
    if (loan_create(a.acc_no, amt, &id) < 0) { net_send(fd, "Could not save the application.\n"); return 0; }
    net_send(fd, "Loan application submitted. Your loan id is %d.\n", id);
    log_event("%s applied for loan %d", u->username, id);
    return 0;
}

static int act_loan_status(int fd, const User *u)
{
    Account a;
    if (my_account(fd, u, &a) < 0) return 0;
    int shown = 0, n = loan_total();
    for (int i = 0; i < n; i++) {
        Loan l;
        if (loan_at(i, &l) < 0 || l.acc_no != a.acc_no) continue;
        char b[40];
        fmt_money(l.amount, b, sizeof b);
        net_send(fd, "Loan #%d  %s  %s\n", l.loan_id, b, loan_status_name(l.status));
        shown++;
    }
    if (!shown) net_send(fd, "You have no loan applications.\n");
    return 0;
}

static int act_feedback(int fd, const User *u)
{
    char text[120];
    if (net_ask(fd, "Your feedback: ", text, sizeof text) < 0) return -1;
    if (text[0] == '\0') { net_send(fd, "Nothing to send.\n"); return 0; }
    if (fb_add(u->id, text) == 0) net_send(fd, "Thank you for your feedback!\n");
    else net_send(fd, "Could not save feedback.\n");
    return 0;
}

int customer_menu(int fd, const User *u)
{
    for (;;) {
        net_send(fd,
            "\n===== Customer Menu (%s) =====\n"
            " 1. Balance enquiry\n"
            " 2. Deposit\n"
            " 3. Withdraw\n"
            " 4. Transfer to another account\n"
            " 5. View account details\n"
            " 6. Mini statement\n"
            " 7. Apply for a loan\n"
            " 8. Loan status\n"
            " 9. Change password\n"
            "10. Send feedback\n"
            "11. Logout\n", u->username);

        int c = net_ask_choice(fd, "Choice: ");
        if (c < 0) return -1;

        int r = 0;
        switch (c) {
        case 1:  r = act_balance(fd, u);     break;
        case 2:  r = act_deposit(fd, u);     break;
        case 3:  r = act_withdraw(fd, u);    break;
        case 4:  r = act_transfer(fd, u);    break;
        case 5:  r = act_details(fd, u);     break;
        case 6:  r = act_statement(fd, u);   break;
        case 7:  r = act_apply_loan(fd, u);  break;
        case 8:  r = act_loan_status(fd, u); break;
        case 9:  r = flow_change_password(fd, u); break;
        case 10: r = act_feedback(fd, u);    break;
        case 11: return 0;
        default: net_send(fd, "Please pick a number from the menu.\n");
        }
        if (r < 0) return -1;
    }
}
