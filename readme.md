ATM Banking Transaction and Security System
===========================================

This project is a C-based ATM Banking Transaction and Security System developed using SQLite and OpenSSL. The system simulates real-world ATM banking operations such as secure login authentication, balance checking, deposits, withdrawals, fund transfers, transaction history management, and encrypted PIN handling.

Features
--------

### Admin and Customer Access

#### Admin Features

*   View ATM balance
    
*   View transaction history
    
*   Add ATM balance
    
*   Manage banking operations
    

#### Customer Features

*   Secure login using Customer ID and PIN
    
*   View account details
    
*   Check account and wallet balance
    
*   Deposit money
    
*   Withdraw money
    
*   Transfer funds
    
*   View mini statement
    
*   Change PIN
    

### PIN Encryption and Decryption

*   Secure PIN management using AES encryption through OpenSSL
    
*   Encrypted PIN storage in SQLite database
    
*   Secret key generation for encryption and decryption
    

### Transaction Logging

*   Every transaction is stored in the database
    
*   Supports:
    
    *   Deposit
        
    *   Withdraw
        
    *   Transfer
        
*   Unique transaction number generation
    

### Multi-Bank Support

*   Supports transactions for users from different banks
    
*   Automatic service charge calculation:
    
    *   5% for deposit/withdraw from other banks
        
    *   10% for fund transfer from other banks
        

Technologies Used
-----------------

*   C Language
    
*   SQLite3 Database
    
*   OpenSSL Library
    
*   GCC Compiler
    
*   MSYS2 / MinGW
    

Prerequisites
-------------

Install the following before running the project:

*   GCC Compiler
    
*   SQLite3 Library
    
*   OpenSSL Library
    
*   MSYS2 / MinGW Environment (Windows)
    

Installation
------------

### 1\. Clone Repository

       git clone https://github.com/PRIYADARSHAN-MAHENDRAN/ATM-Banking-Transaction-and-Security-System.git

### 2\. Navigate to Project Directory

      cd ATM-Banking-Transaction-and-Security-System

### 3\. Compile Main ATM Program

      gcc AutomatedTellerMachine.c -o AutomatedTellerMachine.exe -lsqlite3 -lcrypto -lssl

### 4\. Compile Admin Utility

      gcc atm_admin.c -o atm_admin.exe -lsqlite3   

### 5\. Run Programs

Run ATM System:

      ./AutomatedTellerMachine.exe   

Run Admin Panel:

      ./atm_admin.exe   

Usage
-----

### Login

*   Users login using Customer ID and PIN
    
*   New users can set PIN using:
    
    *   Forgot PIN / Set PIN option
        

### Admin Operations

*   View ATM balance
    
*   View transaction history
    
*   Add ATM cash
    
*   Manage users
    

### Customer Operations

*   View account details
    
*   Check balance
    
*   Deposit money
    
*   Withdraw money
    
*   Transfer funds
    
*   View mini statement
    
*   Change PIN securely
    

Database
--------

The system uses SQLite database (atm.db) to store all banking information.

### Tables

#### machine

Stores:

*   ATM balance
    
*   Last updated time
    

#### user

Stores:

*   Customer ID
    
*   Account type
    
*   Bank name
    
*   Account balance
    
*   Wallet balance
    
*   Encrypted PIN
    
*   Secret key
    

#### transactionlog

Stores:

*   Transaction number
    
*   Customer ID
    
*   Amount
    
*   Transaction type
    
*   Transaction time
    

Security
--------

### AES Encryption

*   PINs are encrypted using AES encryption
    
*   Plain text PIN storage is avoided
    

### OpenSSL Integration

*   OpenSSL library is used for:
    
    *   AES encryption
        
    *   AES decryption
        
    *   Secret key generation
        

### Transaction Safety

*   SQL transaction rollback used for failed operations
    
*   Prevents database inconsistency
    

Special Features
----------------

*   Secure encrypted PIN system
    
*   Transaction rollback support
    
*   Multi-bank transaction handling
    
*   Unique 16-digit transaction IDs
    
*   SQLite database integration
    
*   Admin and customer role separation
    

Future Improvements
-------------------

*   GUI-based interface
    
*   OTP authentication
    
*   Receipt printing
    
*   Online banking support
    
*   Card number simulation
    
*   Multi-user support
    

Contributing
------------

Contributions are welcome.Fork the repository and submit a pull request for improvements.

License
-------

This project is licensed under the MIT License.