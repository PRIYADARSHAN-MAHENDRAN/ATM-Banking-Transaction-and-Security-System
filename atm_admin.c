#include <stdio.h>
#include <stdlib.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>

sqlite3 *db;

void createTables() {
    char *errMsg = 0;

    const char *machineTable =
        "CREATE TABLE IF NOT EXISTS machine ("
        "time TEXT NOT NULL,"
        "atmbalance INTEGER NOT NULL);";

    const char *userTable =
        "CREATE TABLE IF NOT EXISTS user ("
        "customerid INTEGER PRIMARY KEY,"
        "accounttype TEXT NOT NULL,"
        "bankname TEXT NOT NULL,"
        "useraccountbalance INTEGER NOT NULL,"
        "userwalletbalance INTEGER NOT NULL DEFAULT 0,"
        "name TEXT,"
        "pin TEXT,"
        "secretKey TEXT);";

    sqlite3_exec(db, machineTable, 0, 0, &errMsg);
    sqlite3_exec(db, userTable, 0, 0, &errMsg);
}

void initializeATM() {
    sqlite3_stmt *stmt;
    const char *checkSQL = "SELECT COUNT(*) FROM machine";

    sqlite3_prepare_v2(db, checkSQL, -1, &stmt, NULL);
    sqlite3_step(stmt);

    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);

    if (count == 0) {
        const char *insertSQL =
            "INSERT INTO machine (time, atmbalance) VALUES (datetime('now'), 0);";

        sqlite3_exec(db, insertSQL, 0, 0, NULL);
    }
}

void addATMbalance() {
    int amount;

    printf("\nEnter amount to add to ATM: ");
    scanf("%d", &amount);

    sqlite3_stmt *stmt;

    const char *sql =
        "UPDATE machine SET atmbalance = atmbalance + ?, time = datetime('now');";

    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, amount);

    if (sqlite3_step(stmt) == SQLITE_DONE) {
        printf("\nATM balance updated successfully.\n");
    } else {
        printf("\nFailed to update ATM balance.\n");
    }

    sqlite3_finalize(stmt);
}

void viewATMbalance() {
    sqlite3_stmt *stmt;

    const char *sql = "SELECT atmbalance, time FROM machine";

    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int balance = sqlite3_column_int(stmt, 0);
        const unsigned char *time = sqlite3_column_text(stmt, 1);

        printf("\nCurrent ATM Balance : %d\n", balance);
        printf("Last Updated        : %s\n", time);
    }

    sqlite3_finalize(stmt);
}

void addUser() {
    int id, accountBalance, walletBalance;
    char accountType[5], bankName[50], name[100];

    printf("\nEnter Customer ID        : ");
    scanf("%d", &id);

    getchar();

    printf("Enter Name               : ");
    fgets(name, sizeof(name), stdin);
    name[strcspn(name, "\n")] = '\0';

    printf("Enter Account Type (A/C) : ");
    scanf("%s", accountType);

    printf("Enter Bank Name          : ");
    scanf("%s", bankName);

    printf("Enter Account Balance    : ");
    scanf("%d", &accountBalance);

    printf("Enter Wallet Balance     : ");
    scanf("%d", &walletBalance);

    sqlite3_stmt *stmt;

    const char *sql =
        "INSERT INTO user "
        "(customerid, accounttype, bankname, useraccountbalance, userwalletbalance, name) "
        "VALUES (?, ?, ?, ?, ?, ?);";

    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);

    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, accountType, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, bankName, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, accountBalance);
    sqlite3_bind_int(stmt, 5, walletBalance);
    sqlite3_bind_text(stmt, 6, name, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_DONE) {
        printf("\nUser added successfully.\n");
    } else {
        printf("\nError adding user: %s\n", sqlite3_errmsg(db));
    }

    sqlite3_finalize(stmt);
}

int main() {
    int choice;

    if (sqlite3_open("atm.db", &db)) {
        printf("Cannot open database.\n");
        return 1;
    }

    createTables();
    initializeATM();

    while (1) {
        printf("\n====== ATM ADMIN PANEL ======\n");
        printf("1. Add User\n");
        printf("2. Add ATM Balance\n");
        printf("3. View ATM Balance\n");
        printf("0. Exit\n");

        printf("\nEnter choice: ");
        scanf("%d", &choice);

        switch (choice) {
            case 1:
                addUser();
                break;

            case 2:
                addATMbalance();
                break;

            case 3:
                viewATMbalance();
                break;

            case 0:
                sqlite3_close(db);
                printf("\nProgram Closed.\n");
                return 0;

            default:
                printf("\nInvalid choice.\n");
        }
    }

    sqlite3_close(db);
    return 0;
}
