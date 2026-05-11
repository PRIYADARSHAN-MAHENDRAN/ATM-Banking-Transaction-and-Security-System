#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#define DB_FILE "atm.db"
#define NAME_SIZE 128
#define BANK_SIZE 64
#define KEY_B64_SIZE 128
#define PIN_B64_SIZE 256
#define TXN_SIZE 32
#define LINE_SIZE 256

static sqlite3 *db = NULL;
static char accountType = '\0'; /* 'A' = admin, 'C' = customer */
static char bankname[BANK_SIZE] = "";
static int customerId = 0;

static void clear_screen(void) {
    printf("\033[H\033[2J");
    fflush(stdout);
}

static void wait_for_enter(void) {
    char buf[8];
    printf("\nPress Enter to continue...");
    fflush(stdout);
    fgets(buf, sizeof(buf), stdin);
}

static void trim_newline(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static void read_line(const char *prompt, char *out, size_t out_size) {
    if (prompt) {
        printf("%s", prompt);
        fflush(stdout);
    }
    if (fgets(out, (int)out_size, stdin) == NULL) {
        out[0] = '\0';
        return;
    }
    trim_newline(out);
}

static int read_int(const char *prompt) {
    char buf[LINE_SIZE];
    long value;
    char *endptr;

    for (;;) {
        read_line(prompt, buf, sizeof(buf));
        value = strtol(buf, &endptr, 10);
        if (endptr != buf && *endptr == '\0') {
            return (int)value;
        }
        printf("Please enter a valid number.\n");
    }
}

static int equals_ignore_case(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

static int exec_sql(const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite error: %s\n", err ? err : sqlite3_errmsg(db));
        sqlite3_free(err);
        return 0;
    }
    return 1;
}

static int begin_transaction(void) {
    return exec_sql("BEGIN TRANSACTION;");
}

static int commit_transaction(void) {
    return exec_sql("COMMIT;");
}

static int rollback_transaction(void) {
    return exec_sql("ROLLBACK;");
}

static int create_tables(void) {
    const char *queries[] = {
        "CREATE TABLE IF NOT EXISTS machine (time TEXT NOT NULL, atmbalance INTEGER NOT NULL)",
        "CREATE TABLE IF NOT EXISTS user (customerid INTEGER NOT NULL UNIQUE, accounttype TEXT NOT NULL, bankname TEXT NOT NULL, useraccountbalance INTEGER NOT NULL, userwalletbalance INTEGER NOT NULL DEFAULT 0, name TEXT, pin TEXT, secretKey TEXT)",
        "CREATE TABLE IF NOT EXISTS transactionlog (transaction_number TEXT UNIQUE, customerid INTEGER NOT NULL, amount INTEGER NOT NULL, type TEXT NOT NULL, time TEXT NOT NULL, FOREIGN KEY(customerid) REFERENCES user(customerid))"
    };

    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++) {
        if (!exec_sql(queries[i])) {
            return 0;
        }
    }

    /* Seed a machine row if the table is empty so balance screens work. */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM machine", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;
    rc = sqlite3_step(stmt);
    int count = (rc == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);

    if (count == 0) {
        if (!exec_sql("INSERT INTO machine(time, atmbalance) VALUES(datetime('now'), 0)") ) {
            return 0;
        }
    }

    return 1;
}

static int connect_db(void) {
    if (sqlite3_open(DB_FILE, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return 0;
    }
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    return 1;
}

static void disconnect_db(void) {
    if (db) {
        sqlite3_close(db);
        db = NULL;
    }
}

static int base64_encode(const unsigned char *input, int len, char *output, int out_len) {
    int needed = 4 * ((len + 2) / 3) + 1;
    if (out_len < needed) return 0;
    EVP_EncodeBlock((unsigned char *)output, input, len);
    return 1;
}

static int base64_decode(const char *input, unsigned char *output, int out_len) {
    int in_len = (int)strlen(input);
    int max_needed = 3 * (in_len / 4) + 3;
    if (out_len < max_needed) return -1;
    int decoded = EVP_DecodeBlock(output, (const unsigned char *)input, in_len);
    if (decoded < 0) return -1;
    while (in_len > 0 && input[in_len - 1] == '=') {
        decoded--;
        in_len--;
    }
    return decoded;
}

static int generate_key(unsigned char *key, size_t key_len) {
    return RAND_bytes(key, (int)key_len) == 1;
}

static int encrypt_pin(int pin, const unsigned char *key, char *encrypted_b64, size_t encrypted_b64_size) {
    unsigned char ciphertext[64];
    int out_len1 = 0, out_len2 = 0;
    char plaintext[32];
    snprintf(plaintext, sizeof(plaintext), "%d", pin);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return 0;

    int ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL);
    ok = ok && EVP_CIPHER_CTX_set_padding(ctx, 1);
    ok = ok && EVP_EncryptUpdate(ctx, ciphertext, &out_len1, (const unsigned char *)plaintext, (int)strlen(plaintext));
    ok = ok && EVP_EncryptFinal_ex(ctx, ciphertext + out_len1, &out_len2);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return 0;

    int total = out_len1 + out_len2;
    return base64_encode(ciphertext, total, encrypted_b64, (int)encrypted_b64_size);
}

static int decrypt_pin(const char *encrypted_b64, const char *encoded_key, int *pin_out) {
    unsigned char key[32];
    unsigned char ciphertext[64];
    unsigned char plaintext[64];
    int key_len = base64_decode(encoded_key, key, (int)sizeof(key));
    if (key_len != 16) return 0;

    int cipher_len = base64_decode(encrypted_b64, ciphertext, (int)sizeof(ciphertext));
    if (cipher_len <= 0) return 0;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return 0;

    int out_len1 = 0, out_len2 = 0;
    int ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), NULL, key, NULL);
    ok = ok && EVP_CIPHER_CTX_set_padding(ctx, 1);
    ok = ok && EVP_DecryptUpdate(ctx, plaintext, &out_len1, ciphertext, cipher_len);
    ok = ok && EVP_DecryptFinal_ex(ctx, plaintext + out_len1, &out_len2);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return 0;

    int total = out_len1 + out_len2;
    if (total <= 0 || total >= (int)sizeof(plaintext)) return 0;
    plaintext[total] = '\0';
    *pin_out = atoi((char *)plaintext);
    return 1;
}

static int generate_unique_transaction_number(char *txn, size_t txn_size) {
    sqlite3_stmt *stmt = NULL;
    if (txn_size < 17) return 0;
    for (;;) {
        for (int i = 0; i < 16; i++) {
            txn[i] = (char)('0' + (rand() % 10));
        }
        txn[16] = '\0';

        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM transactionlog WHERE transaction_number = ?", -1, &stmt, NULL) != SQLITE_OK) {
            return 0;
        }
        sqlite3_bind_text(stmt, 1, txn, -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        int count = (rc == SQLITE_ROW) ? sqlite3_column_int(stmt, 0) : 1;
        sqlite3_finalize(stmt);
        stmt = NULL;

        if (count == 0) return 1;
    }
}

static int get_wallet_balance(int id) {
    sqlite3_stmt *stmt = NULL;
    int balance = 0;
    if (sqlite3_prepare_v2(db, "SELECT userwalletbalance FROM user WHERE customerid = ?", -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        balance = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return balance;
}

static int get_account_balance(int id) {
    sqlite3_stmt *stmt = NULL;
    int balance = 0;
    if (sqlite3_prepare_v2(db, "SELECT useraccountbalance FROM user WHERE customerid = ?", -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        balance = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return balance;
}

static int get_atm_balance(void) {
    sqlite3_stmt *stmt = NULL;
    int balance = 0;
    if (sqlite3_prepare_v2(db, "SELECT atmbalance FROM machine LIMIT 1", -1, &stmt, NULL) != SQLITE_OK) return 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        balance = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return balance;
}

static void print_balance_pair(int id) {
    printf("ID: %d\n", id);
    printf("Bank: %s\n", bankname);
    printf("Account Balance: %d\n", get_account_balance(id));
    printf("Wallet Balance: %d\n", get_wallet_balance(id));
}

static int get_customer_bank_name(int id, char *out, size_t out_size) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT bankname FROM user WHERE customerid = ?", -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, id);
    int ok = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *txt = sqlite3_column_text(stmt, 0);
        if (txt) {
            snprintf(out, out_size, "%s", txt);
            ok = 1;
        }
    }
    sqlite3_finalize(stmt);
    return ok;
}

static int is_valid_customer(int id) {
    sqlite3_stmt *stmt = NULL;
    int valid = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM user WHERE customerid = ?", -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        valid = sqlite3_column_int(stmt, 0) > 0;
    }
    sqlite3_finalize(stmt);
    return valid;
}

static int update_account_balance(int id, int amount) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE user SET useraccountbalance = useraccountbalance + ? WHERE customerid = ?", -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, amount);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(db) > 0;
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && changed;
}

static int update_wallet_balance(int id, int amount) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE user SET userwalletbalance = userwalletbalance + ? WHERE customerid = ?", -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, amount);
    sqlite3_bind_int(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(db) > 0;
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && changed;
}

static int update_atm_balance(int amount) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE machine SET time = datetime('now'), atmbalance = atmbalance + ?", -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, amount);
    int rc = sqlite3_step(stmt);
    int changed = sqlite3_changes(db) > 0;
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE && changed;
}

static int log_transaction(const char *txn, int id, int amount, const char *type) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "INSERT INTO transactionlog (transaction_number, customerid, amount, type, time) VALUES (?, ?, ?, ?, datetime('now'))";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, txn, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, id);
    sqlite3_bind_int(stmt, 3, amount);
    sqlite3_bind_text(stmt, 4, type, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

static char authenticate(int id, int entered_pin) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT accounttype, bankname, pin, secretKey FROM user WHERE customerid = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        printf("Error during authentication: %s\n", sqlite3_errmsg(db));
        return '\0';
    }

    sqlite3_bind_int(stmt, 1, id);
    char ret = '\0';
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        const unsigned char *atype = sqlite3_column_text(stmt, 0);
        const unsigned char *bname = sqlite3_column_text(stmt, 1);
        const unsigned char *enc_pin = sqlite3_column_text(stmt, 2);
        const unsigned char *secret = sqlite3_column_text(stmt, 3);

        if (!atype || !bname || !enc_pin || !secret) {
            printf("Please set PIN.\n");
        } else {
            int decrypted_pin = 0;
            if (decrypt_pin((const char *)enc_pin, (const char *)secret, &decrypted_pin)) {
                if (decrypted_pin == entered_pin) {
                    snprintf(bankname, sizeof(bankname), "%s", bname);
                    ret = (char)atype[0];
                } else {
                    printf("\nInvalid PIN. Please try again.\n");
                }
            } else {
                printf("\nPlease set PIN.\n");
            }
        }
    } else {
        printf("\nInvalid ID or PIN. Please try again.\n");
    }

    sqlite3_finalize(stmt);
    return ret;
}

static void set_pin(int id, const char *name) {
    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT 1 FROM user WHERE customerid = ? AND name = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        printf("Error during authentication: %s\n", sqlite3_errmsg(db));
        wait_for_enter();
        return;
    }

    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    int exists = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);

    if (!exists) {
        printf("Invalid ID or Name. Please try again.\n");
        wait_for_enter();
        return;
    }

    int new_pin = read_int("Enter New PIN (4 digits): ");
    int confirm_pin = read_int("Confirm New PIN (4 digits): ");

    if (new_pin != confirm_pin) {
        printf("New PIN and confirmation do not match. Please try again.\n");
        wait_for_enter();
        return;
    }
    if (new_pin < 1000 || new_pin > 9999) {
        printf("New PIN must be 4 digits.\n");
        wait_for_enter();
        return;
    }

    unsigned char key[16];
    char key_b64[KEY_B64_SIZE];
    char encrypted_pin[PIN_B64_SIZE];

    if (!generate_key(key, sizeof(key)) ||
        !base64_encode(key, (int)sizeof(key), key_b64, sizeof(key_b64)) ||
        !encrypt_pin(new_pin, key, encrypted_pin, sizeof(encrypted_pin))) {
        printf("Error generating or encrypting PIN.\n");
        wait_for_enter();
        return;
    }

    sql = "UPDATE user SET pin = ?, secretKey = ? WHERE customerid = ? AND name = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        printf("Error updating PIN: %s\n", sqlite3_errmsg(db));
        wait_for_enter();
        return;
    }

    sqlite3_bind_text(stmt, 1, encrypted_pin, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, key_b64, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, id);
    sqlite3_bind_text(stmt, 4, name, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) {
        printf("PIN successfully set.\n");
    } else {
        printf("Error updating PIN. Please try again.\n");
    }
    sqlite3_finalize(stmt);
    wait_for_enter();
}

static char login(void) {
    for (;;) {
        clear_screen();
        printf("```` Welcome to KDFC ATM ````\n\n");
        printf("1. Insert Card\n");
        printf("2. Forgot PIN / Set PIN\n");
        printf("0. Exit\n\n");

        int choice = read_int("Enter your choice: ");

        if (choice == 0) {
            printf("\n~~~~ Get your ATM card. ~~~~\n\n");
            exit(0);
        } else if (choice == 1) {
            clear_screen();
            int id = read_int("Enter ID num: ");
            int pin = read_int("Enter PIN: ");
            char type = authenticate(id, pin);
            if (type != '\0') {
                customerId = id;
                accountType = type;
                printf("\nSuccessfully login\n");
                wait_for_enter();
                return type;
            } else {
                wait_for_enter();
            }
        } else if (choice == 2) {
            clear_screen();
            int id = read_int("Enter ID num: ");
            char name[NAME_SIZE];
            read_line("Enter name: ", name, sizeof(name));
            set_pin(id, name);
        } else {
            printf("\nPlease enter a valid choice number (0, 1, or 2) only.\n");
            wait_for_enter();
        }
    }
}

static void admin_options(void) {
    printf("1. ATM Balance\n");
    printf("2. Transaction History\n");
    printf("3. Deposit\n");
    printf("4. Withdraw\n");
    printf("5. Change PIN\n");
    printf("0. Logout\n\n");
    printf("Choose an option: ");
}

static void customer_options(void) {
    printf("1. My Account\n");
    printf("2. Balance\n");
    printf("3. Deposit\n");
    printf("4. Withdraw\n");
    printf("5. Mini Statement\n");
    printf("6. Transfer\n");
    printf("7. Change PIN\n");
    printf("0. Logout\n\n");
    printf("Choose an option: ");
}

static void view_atm_balance(void) {
    clear_screen();
    printf("~~~~ ATM BALANCE ~~~~\n\n");

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT time, atmbalance FROM machine LIMIT 1", -1, &stmt, NULL) != SQLITE_OK) {
        printf("Error retrieving ATM balance: %s\n", sqlite3_errmsg(db));
    } else if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *time_val = sqlite3_column_text(stmt, 0);
        int balance = sqlite3_column_int(stmt, 1);
        printf("Amount: %d  time: %s\n", balance, time_val ? (const char *)time_val : "");
    } else {
        printf("No ATM balance found.\n");
    }
    sqlite3_finalize(stmt);

    wait_for_enter();
    clear_screen();
}

static void view_transaction_history(void) {
    clear_screen();
    printf("~~~~ Last 10 Transactions ~~~~\n\n");
    printf("%-20s %-10s %-10s %-30s\n", "Transaction Number", "Amount", "Type", "Time");
    printf("----------------------------------------------------------------------\n");

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT transaction_number, amount, type, time FROM transactionlog ORDER BY time DESC LIMIT 10";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        printf("Error retrieving transactions: %s\n", sqlite3_errmsg(db));
    } else {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *txn = sqlite3_column_text(stmt, 0);
            int amount = sqlite3_column_int(stmt, 1);
            const unsigned char *type = sqlite3_column_text(stmt, 2);
            const unsigned char *time_val = sqlite3_column_text(stmt, 3);
            printf("%-20s %-10d %-10s %-30s\n",
                   txn ? (const char *)txn : "",
                   amount,
                   type ? (const char *)type : "",
                   time_val ? (const char *)time_val : "");
        }
    }
    sqlite3_finalize(stmt);

    wait_for_enter();
    clear_screen();
}

static void view_my_account(void) {
    clear_screen();
    printf("~~~~ My account ~~~~\n\n");
    print_balance_pair(customerId);
    wait_for_enter();
    clear_screen();
}

static void view_my_balance(void) {
    clear_screen();
    printf("~~~~ Balance ~~~~\n\n");
    printf("Account Balance: %d\n", get_account_balance(customerId));
    printf("Wallet Balance: %d\n", get_wallet_balance(customerId));
    wait_for_enter();
    clear_screen();
}

static void deposit(void) {
    clear_screen();
    int wallet_balance = get_wallet_balance(customerId);
    printf("~~~~ Deposit ~~~~\n\n");
    printf("(#Important note 5%% service charge if other bank customer)\n\n");
    int amount = read_int("Enter amount to deposit: ");
    printf("\n");

    if (amount > wallet_balance) {
        printf("Insufficient wallet balance. Available balance: %d\n", wallet_balance);
        wait_for_enter();
        clear_screen();
        return;
    }

    int deposit_amount = !equals_ignore_case(bankname, "kdfc") ? amount - ((amount / 100) * 5) : amount;

    if (!begin_transaction()) {
        wait_for_enter();
        clear_screen();
        return;
    }

    do {
        if (accountType == 'C') {
            if (!update_account_balance(customerId, deposit_amount)) {
                printf("Failed to debit amount from account. Transaction aborted.\n");
                rollback_transaction();
                break;
            }
            if (!update_wallet_balance(customerId, -amount)) {
                printf("Failed to credit amount to wallet. Transaction aborted.\n");
                rollback_transaction();
                break;
            }
        } else {
            if (!update_wallet_balance(customerId, -amount)) {
                printf("Failed to credit amount to wallet. Transaction aborted.\n");
                rollback_transaction();
                break;
            }
        }

        if (!update_atm_balance(amount)) {
            printf("Failed to debit amount from atm. Transaction aborted.\n");
            rollback_transaction();
            break;
        }

        char txn[TXN_SIZE];
        if (!generate_unique_transaction_number(txn, sizeof(txn))) {
            printf("Failed to create transaction number. Transaction aborted.\n");
            rollback_transaction();
            break;
        }

        if (!log_transaction(txn, customerId, amount, "DEPOSIT")) {
            printf("Failed to log transaction. Transaction aborted.\n");
            rollback_transaction();
            break;
        }

        if (!commit_transaction()) {
            printf("Failed to commit transaction.\n");
            rollback_transaction();
            break;
        }

        printf("Deposit successful. Your new wallet balance is: %d\n", get_wallet_balance(customerId));
    } while (0);

    wait_for_enter();
    clear_screen();
}

static void withdraw_funds(void) {
    clear_screen();
    int account_balance = get_account_balance(customerId);
    printf("~~~~ Withdraw ~~~~\n\n");
    printf("(#Important note 5%% service charge if other bank customer)\n\n");
    int amount = read_int("Enter amount to withdraw: ");
    printf("\n");

    if (amount > account_balance) {
        printf("Insufficient account balance. Available balance: %d\n", account_balance);
        wait_for_enter();
        clear_screen();
        return;
    }

    if (get_atm_balance() < amount) {
        printf("Insufficient ATM balance. Please comeback after some time.\n");
        wait_for_enter();
        clear_screen();
        return;
    }

    int withdraw_amount = !equals_ignore_case(bankname, "kdfc") ? amount + ((amount / 100) * 5) : amount;

    if (!begin_transaction()) {
        wait_for_enter();
        clear_screen();
        return;
    }

    do {
        if (!update_account_balance(customerId, -withdraw_amount)) {
            printf("Failed to debit amount from account. Transaction aborted.\n");
            rollback_transaction();
            break;
        }
        if (!update_wallet_balance(customerId, amount)) {
            printf("Failed to credit amount to wallet. Transaction aborted.\n");
            rollback_transaction();
            break;
        }
        if (!update_atm_balance(-amount)) {
            printf("Failed to debit amount from atm. Transaction aborted.\n");
            rollback_transaction();
            break;
        }

        char txn[TXN_SIZE];
        if (!generate_unique_transaction_number(txn, sizeof(txn))) {
            printf("Failed to create transaction number. Transaction aborted.\n");
            rollback_transaction();
            break;
        }

        if (!log_transaction(txn, customerId, amount, "WITHDRAW")) {
            printf("Failed to log transaction. Transaction aborted.\n");
            rollback_transaction();
            break;
        }

        if (!commit_transaction()) {
            printf("Failed to commit transaction.\n");
            rollback_transaction();
            break;
        }

        printf("Withdraw successful. Your new account balance is: %d\n", get_account_balance(customerId));
    } while (0);

    wait_for_enter();
    clear_screen();
}

static void view_mini_statement(void) {
    clear_screen();
    printf("~~~~ Mini Statement ~~~~\n\n");
    printf("%-20s %-10s %-10s %-30s\n", "Transaction Number", "Amount", "Type", "Time");
    printf("----------------------------------------------------------------------\n");

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT transaction_number, amount, type, time FROM transactionlog WHERE customerid = ? ORDER BY time DESC LIMIT 10";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        printf("Error retrieving transaction history: %s\n", sqlite3_errmsg(db));
    } else {
        sqlite3_bind_int(stmt, 1, customerId);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *txn = sqlite3_column_text(stmt, 0);
            int amount = sqlite3_column_int(stmt, 1);
            const unsigned char *type = sqlite3_column_text(stmt, 2);
            const unsigned char *time_val = sqlite3_column_text(stmt, 3);
            printf("%-20s %-10d %-10s %-30s\n",
                   txn ? (const char *)txn : "",
                   amount,
                   type ? (const char *)type : "",
                   time_val ? (const char *)time_val : "");
        }
    }
    sqlite3_finalize(stmt);

    wait_for_enter();
    clear_screen();
}

static void transfer_funds(void) {
    clear_screen();
    printf("~~~~ Fund Transfer ~~~~\n\n");
    printf("(#Important note 10%% service charge if other bank customer)\n\n");

    int transfer_id = read_int("Enter Customer ID to transfer to: ");
    printf("\n");

    if (!is_valid_customer(transfer_id)) {
        printf("Entered customer ID is incorrect. Please try again.\n");
        wait_for_enter();
        clear_screen();
        return;
    }

    int transfer_amount = read_int("Enter amount to transfer: ");
    printf("\n");

    int account_balance = get_account_balance(customerId);
    if (account_balance < transfer_amount) {
        printf("Insufficient account balance. Available balance: %d\n", account_balance);
        wait_for_enter();
        clear_screen();
        return;
    }

    char customer_bank[BANK_SIZE] = "";
    get_customer_bank_name(customerId, customer_bank, sizeof(customer_bank));
    int amount_to_debit = equals_ignore_case(customer_bank, "kdfc") ? transfer_amount : transfer_amount + (transfer_amount / 10);

    if (!begin_transaction()) {
        wait_for_enter();
        clear_screen();
        return;
    }

    do {
        if (!update_account_balance(customerId, -amount_to_debit)) {
            printf("Failed to debit amount. Transaction aborted.\n");
            rollback_transaction();
            break;
        }
        if (!update_account_balance(transfer_id, transfer_amount)) {
            printf("Failed to credit amount. Transaction aborted.\n");
            rollback_transaction();
            break;
        }

        char txn[TXN_SIZE];
        if (!generate_unique_transaction_number(txn, sizeof(txn))) {
            printf("Failed to create transaction number. Transaction aborted.\n");
            rollback_transaction();
            break;
        }

        if (!log_transaction(txn, customerId, transfer_amount, "TRANSFER")) {
            printf("Failed to log transaction. Transaction aborted.\n");
            rollback_transaction();
            break;
        }

        if (!commit_transaction()) {
            printf("Failed to commit transaction.\n");
            rollback_transaction();
            break;
        }

        printf("Transfer successful!\n");
    } while (0);

    wait_for_enter();
    clear_screen();
}

static void change_pin(void) {
    clear_screen();
    printf("~~~~ Change PIN ~~~~\n\n");

    int current_pin = read_int("Enter Current PIN: ");
    printf("\n");
    int new_pin = read_int("Enter New PIN (4 digits): ");
    printf("\n");
    int confirm_pin = read_int("Enter Confirm New PIN (4 digits): ");
    printf("\n");

    if (new_pin != confirm_pin) {
        printf("New PIN and confirmation do not match. Please try again.\n");
        wait_for_enter();
        clear_screen();
        return;
    }
    if (new_pin < 1000 || new_pin > 9999) {
        printf("New PIN must be 4 digits.\n");
        wait_for_enter();
        clear_screen();
        return;
    }

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT pin, secretKey FROM user WHERE customerid = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        printf("Error retrieving PIN: %s\n", sqlite3_errmsg(db));
        wait_for_enter();
        return;
    }

    sqlite3_bind_int(stmt, 1, customerId);
    char encrypted_pin[PIN_B64_SIZE] = "";
    char secret_key[KEY_B64_SIZE] = "";
    int found = 0;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *pin_txt = sqlite3_column_text(stmt, 0);
        const unsigned char *key_txt = sqlite3_column_text(stmt, 1);
        if (pin_txt && key_txt) {
            snprintf(encrypted_pin, sizeof(encrypted_pin), "%s", pin_txt);
            snprintf(secret_key, sizeof(secret_key), "%s", key_txt);
            found = 1;
        }
    }
    sqlite3_finalize(stmt);

    if (!found) {
        printf("PIN not found.\n");
        wait_for_enter();
        clear_screen();
        return;
    }

    int decrypted_pin = 0;
    if (!decrypt_pin(encrypted_pin, secret_key, &decrypted_pin)) {
        printf("Error decrypting PIN.\n");
        wait_for_enter();
        clear_screen();
        return;
    }

    if (decrypted_pin != current_pin) {
        printf("Entered Current PIN is incorrect.\n");
        wait_for_enter();
        clear_screen();
        return;
    }

    unsigned char new_key[16];
    char new_key_b64[KEY_B64_SIZE];
    char new_encrypted_pin[PIN_B64_SIZE];

    if (!generate_key(new_key, sizeof(new_key)) ||
        !base64_encode(new_key, (int)sizeof(new_key), new_key_b64, sizeof(new_key_b64)) ||
        !encrypt_pin(new_pin, new_key, new_encrypted_pin, sizeof(new_encrypted_pin))) {
        printf("Error encrypting new PIN.\n");
        wait_for_enter();
        clear_screen();
        return;
    }

    sql = "UPDATE user SET pin = ?, secretKey = ? WHERE customerid = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        printf("Error updating PIN: %s\n", sqlite3_errmsg(db));
        wait_for_enter();
        return;
    }

    sqlite3_bind_text(stmt, 1, new_encrypted_pin, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, new_key_b64, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, customerId);

    if (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) {
        printf("PIN changed successfully.\n");
    } else {
        printf("Failed to update PIN. Please try again.\n");
    }
    sqlite3_finalize(stmt);

    wait_for_enter();
    clear_screen();
}

int main(void) {
    srand((unsigned int)time(NULL));

    if (!connect_db()) {
        return 1;
    }
    if (!create_tables()) {
        disconnect_db();
        return 1;
    }

    accountType = login();
    clear_screen();

    while (1) {
        printf("```` KDFC ATM ````\n\n");
        if (accountType == 'A') {
            admin_options();
        } else if (accountType == 'C') {
            customer_options();
        } else {
            printf("Login failed. Please try again.\n");
            break;
        }

        int choice = read_int("");
        switch (choice) {
            case 1:
                if (accountType == 'A') view_atm_balance();
                else view_my_account();
                break;
            case 2:
                if (accountType == 'A') view_transaction_history();
                else view_my_balance();
                break;
            case 3:
                deposit();
                break;
            case 4:
                withdraw_funds();
                break;
            case 5:
                if (accountType == 'C') view_mini_statement();
                else change_pin();
                break;
            case 6:
                if (accountType == 'C') transfer_funds();
                else printf("Invalid choice. Please try again.\n");
                break;
            case 7:
                if (accountType == 'C') change_pin();
                else printf("Invalid choice. Please try again.\n");
                break;
            case 0:
                printf("Get your ATM card.\n");
                accountType = login();
                clear_screen();
                break;
            default:
                printf("Invalid choice. Please try again.\n");
                break;
        }
    }

    disconnect_db();
    return 0;
}
