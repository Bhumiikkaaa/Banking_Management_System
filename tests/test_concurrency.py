#!/usr/bin/env python3
"""
Starts a fresh server and hammers it with parallel clients.

What it checks
  1. one login per user (second login is refused)
  2. joint account: two holders depositing at the same time -> no lost updates
  3. withdraw race on a joint account -> balance never goes negative
  4. opposite transfers at the same time -> no deadlock, money conserved
  5. loan flow across customer -> manager -> employee, and admin functions

run:  python3 tests/test_concurrency.py      (from the project folder)
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import threading
import time

PORT = 9099
DATA = "testdata_conc"
failures = 0


def check(cond, msg):
    global failures
    print(("  PASS  " if cond else "  FAIL  ") + msg)
    if not cond:
        failures += 1


class Client:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", PORT), timeout=15)
        self.last = self.read()                      # main menu

    def read(self):
        buf = b""
        while not buf.endswith(b": "):
            d = self.s.recv(4096)
            if not d:
                break
            buf += d
        return buf.decode()

    def say(self, line):
        self.s.sendall((line + "\n").encode())
        self.last = self.read()
        return self.last

    def login(self, role, user, pw):
        self.say(str(role))
        self.say(user)
        return self.say(pw)

    def logout(self, number):
        self.s.sendall((str(number) + "\n").encode())
        self.read()

    def close(self):
        self.s.close()


# customer menu numbers
BAL, DEP, WD, XFER, LOGOUT_CUST = "1", "2", "3", "4", "11"


def rupees(text):
    m = re.search(r"Rs\. (-?\d+)\.(\d\d)", text)
    return int(m.group(1)) * 100 + int(m.group(2)) if m else None


def balance_of(user, pw):
    c = Client()
    c.login(1, user, pw)
    b = rupees(c.say(BAL))
    c.logout(LOGOUT_CUST)
    c.close()
    return b


def run_threads(targets):
    ts = [threading.Thread(target=t) for t in targets]
    for t in ts:
        t.start()
    for t in ts:
        t.join(timeout=60)
    return all(not t.is_alive() for t in ts)


def main():
    shutil.rmtree(DATA, ignore_errors=True)
    server = subprocess.Popen(["./server", str(PORT), DATA],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    try:
        run_all()
    finally:
        server.terminate()
        server.wait()
        shutil.rmtree(DATA, ignore_errors=True)
    print("\nRESULT:", "all checks passed" if failures == 0 else f"{failures} check(s) failed")
    sys.exit(1 if failures else 0)


def run_all():
    print("== 1. login rules ==")
    a = Client()
    check("Welcome, asha" in a.login(1, "asha", "asha123"), "asha can log in")
    b = Client()
    check("already logged in" in b.login(1, "asha", "asha123"), "second login of same user is refused")
    check("Wrong username" in b.login(1, "asha", "nope"), "wrong password is refused")
    a.logout(LOGOUT_CUST)
    check("Welcome, asha" in b.login(1, "asha", "asha123"), "user can log in again after logout")
    b.logout(LOGOUT_CUST)
    a.close()
    b.close()
    c = Client()
    c.login(1, "kiran", "bad1")
    c.login(1, "kiran", "bad2")
    c.say("1")
    c.say("kiran")
    out = c.say("bad3")
    check("Too many wrong attempts" in out or out == "", "3 wrong passwords disconnect the client")
    c.close()

    print("== 2. joint account, simultaneous deposits ==")
    start = balance_of("asha", "asha123")

    def depositor(user, pw, times):
        def work():
            cl = Client()
            cl.login(1, user, pw)
            for _ in range(times):
                cl.say(DEP)
                cl.say("10")
            cl.logout(LOGOUT_CUST)
            cl.close()
        return work

    ok = run_threads([depositor("asha", "asha123", 25),
                      depositor("ravi", "ravi123", 25),
                      depositor("meera", "meera123", 25)])
    check(ok, "no client got stuck")
    after = balance_of("asha", "asha123")
    check(after - start == 50 * 1000, f"joint account gained exactly Rs 500 (no lost update): {start/100} -> {after/100}")
    check(balance_of("meera", "meera123") == 300000 + 25 * 1000, "meera's own deposits all counted")

    print("== 3. withdraw race on the joint account ==")
    bal = balance_of("asha", "asha123")
    successes = []
    lock = threading.Lock()

    def withdrawer(user, pw):
        def work():
            cl = Client()
            cl.login(1, user, pw)
            for _ in range(6):
                out = cl.say(WD) and cl.say("700")
                if "collect your cash" in out:
                    with lock:
                        successes.append(1)
            cl.logout(LOGOUT_CUST)
            cl.close()
        return work

    run_threads([withdrawer("asha", "asha123"), withdrawer("ravi", "ravi123")])
    final = balance_of("asha", "asha123")
    possible = bal // 70000
    check(len(successes) == possible, f"exactly {possible} withdrawals of Rs 700 succeeded (got {len(successes)})")
    check(final == bal - 70000 * len(successes) and final >= 0, f"balance never negative, final {final/100}")

    print("== 4. opposite transfers at the same time ==")
    before = sum(balance_of(u, p) for u, p in
                 [("asha", "asha123"), ("meera", "meera123"), ("kiran", "kiran123")])
    ACC = {"asha": 10001, "meera": 10002, "kiran": 10003}

    def mover(user, pw, target, times):
        def work():
            cl = Client()
            cl.login(1, user, pw)
            for _ in range(times):
                cl.say(XFER)
                cl.say(str(ACC[target]))
                cl.say("5")
            cl.logout(LOGOUT_CUST)
            cl.close()
        return work

    ok = run_threads([mover("asha", "asha123", "meera", 20),
                      mover("meera", "meera123", "asha", 20),
                      mover("kiran", "kiran123", "meera", 15)])
    check(ok, "no deadlock between opposite transfers")
    after = sum(balance_of(u, p) for u, p in
                [("asha", "asha123"), ("meera", "meera123"), ("kiran", "kiran123")])
    check(before == after, f"total money unchanged ({before/100} -> {after/100})")

    print("== 5. loan flow + admin ==")
    k = Client()
    k.login(1, "kiran", "kiran123")
    k.say("7")
    out = k.say("2000")
    check("Loan application submitted" in out, "customer applies for a loan")
    check("cannot give more" in (k.say("7") and k.say("900000")), "loan above the bank limit is refused")
    k_before = rupees(k.say(BAL))

    m = Client()
    m.login(3, "manager1", "manager123")
    m.say("2")
    check("Loan #1" in m.last, "manager sees the open loan")
    m.say("3")
    m.say("1")
    out = m.say("3")                  # emp1 has user id 3
    check("assigned to emp1" in out, "manager assigns the loan to emp1")
    m.logout("6")
    m.close()

    e = Client()
    e.login(2, "emp1", "emp123")
    e.say("5")
    e.say("1")
    out = e.say("1")
    check("approved" in out, "employee approves the loan")
    e.say("5")
    e.say("1")
    out = e.say("1")
    check("not assigned to you" in out, "a decided loan cannot be processed twice")
    e.logout("7")
    e.close()

    k_after = rupees(k.say(BAL))
    check(k_after - k_before == 200000, "loan amount was credited to the customer")
    k.logout(LOGOUT_CUST)
    k.close()

    ad = Client()
    ad.login(4, "admin", "admin123")
    ad.say("1")
    ad.say("2")                       # employee
    ad.say("emp2")
    out = ad.say("pass1234")
    check("created with user id" in out, "admin adds an employee")
    ad.say("4")
    out = ad.say("10001")
    check("Joint with" in out and "ravi" in out, "admin searches a joint account")
    ad.say("2")
    out = ad.say("1")
    check("cannot delete yourself" in out, "admin cannot delete himself")
    ad.logout("7")
    ad.close()


if __name__ == "__main__":
    main()
