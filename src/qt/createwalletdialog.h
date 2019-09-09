// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef UMKOIN_QT_CREATEWALLETDIALOG_H
#define UMKOIN_QT_CREATEWALLETDIALOG_H

#include <QDialog>

class WalletModel;

namespace Ui {
    class CreateWalletDialog;
}

/** Dialog for creating wallets
 */
class CreateWalletDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CreateWalletDialog(QWidget* parent);
    virtual ~CreateWalletDialog();

    QString walletName() const;
    bool encrypt() const;
    bool disablePrivateKeys() const;
    bool blank() const;

private:
    Ui::CreateWalletDialog *ui;
};

#endif // UMKOIN_QT_CREATEWALLETDIALOG_H
