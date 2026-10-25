/*
 * admin.c - administrator: add / delete / modify / search.
 */
#include "bank.h"

#include <stdio.h>
#include <string.h>

static int act_add_user(int fd)
{
    net_send(fd, "Role of the new user:\n 1. Customer (with account)\n 2. Employee\n 3. Manager\n 4. Admin\n");
    int role = net_ask_choice(fd, "Role: ");
    if (role < 0) return -1;
    if (role < 1 || role > 4) { net_send(fd, "Invalid role.\n"); return 0; }

    if (role == ROLE_CUSTOMER) return flow_add_customer(fd);

    char name[64], pw[64];
    if (net_ask(fd, "Username: ", name, sizeof name) < 0) return -1;
    if (!valid_username(name)) { net_send(fd, "Username must be 3-20 letters, digits or _.\n"); return 0; }
    if (net_ask(fd, "Password: ", pw, sizeof pw) < 0) return -1;
    if (strlen(pw) < MIN_PASS_LEN) { net_send(fd, "Password too short.\n"); return 0; }

    int id;
    int rc = user_add(name, pw, role, &id);
    if (rc == 0) {
        net_send(fd, "%s created with user id %d.\n", role_name(role), id);
        log_event("admin created %s %s (id %d)", role_name(role), name, id);
    } else if (rc == -1) {
        net_send(fd, "That username is already taken.\n");
    } else {
        net_send(fd, "Disk error.\n");
    }
    return 0;
}

static int act_delete_user(int fd, const User *me)
{
    long id;
    int r = net_ask_number(fd, "User id to delete: ", &id);
    if (r != 0) return r < 0 ? -1 : 0;

    User u;
    if (user_get((int)id, &u) < 0) { net_send(fd, "No such user.\n"); return 0; }
    if (u.id == me->id) { net_send(fd, "You cannot delete yourself.\n"); return 0; }

    /* don't let money vanish: a customer must have an empty account first */
    if (u.role == ROLE_CUSTOMER) {
        Account a;
        if (acct_for_user(u.id, &a) == 0 && a.balance > 0) {
            net_send(fd, "This customer still has money in account %d. Withdraw it first.\n", a.acc_no);
            return 0;
        }
    }
    if (user_delete(u.id) == 0) {
        net_send(fd, "User %s deleted.\n", u.username);
        log_event("admin deleted user %s (id %d)", u.username, u.id);
    } else {
        net_send(fd, "Delete failed.\n");
    }
    return 0;
}

static int act_list_users(int fd)
{
    int n = user_total();
    net_send(fd, "\n%-5s %-20s %s\n", "ID", "USERNAME", "ROLE");
    for (int i = 0; i < n; i++) {
        User u;
        if (user_at(i, &u) < 0 || !u.active) continue;
        net_send(fd, "%-5d %-20s %s\n", u.id, u.username, role_name(u.role));
    }
    return 0;
}

int admin_menu(int fd, const User *u)
{
    for (;;) {
        net_send(fd,
            "\n===== Admin Menu (%s) =====\n"
            "1. Add user\n"
            "2. Delete user\n"
            "3. Modify user\n"
            "4. Search account details\n"
            "5. List all users\n"
            "6. Change password\n"
            "7. Logout\n", u->username);

        int c = net_ask_choice(fd, "Choice: ");
        if (c < 0) return -1;

        int r = 0;
        switch (c) {
        case 1: r = act_add_user(fd);            break;
        case 2: r = act_delete_user(fd, u);      break;
        case 3: r = flow_modify_user(fd, 0);     break;
        case 4: r = flow_search_account(fd);     break;
        case 5: r = act_list_users(fd);          break;
        case 6: r = flow_change_password(fd, u); break;
        case 7: return 0;
        default: net_send(fd, "Please pick a number from the menu.\n");
        }
        if (r < 0) return -1;
    }
}
