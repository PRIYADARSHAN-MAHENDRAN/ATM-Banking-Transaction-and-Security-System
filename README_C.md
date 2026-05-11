Automated Teller Machine (ATM) System in C
=========================================

This is the C-language version of the Java ATM project.

Build (Linux/macOS)
-------------------

```bash
gcc AutomatedTellerMachine.c -o AutomatedTellerMachine -lsqlite3 -lcrypto -lssl
```

Run
---

```bash
./AutomatedTellerMachine
```

Notes
-----
- Uses `atm.db` in the same folder.
- Uses SQLite for storage.
- Uses OpenSSL for PIN encryption/decryption.
- If the database is empty, the program creates the required tables and inserts a default `machine` row.
