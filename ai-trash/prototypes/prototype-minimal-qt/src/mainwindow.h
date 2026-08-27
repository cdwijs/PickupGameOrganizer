// The shell: the part of prototype-minimal that is not a rule.
//
// Section for section this is prototype-minimal's page — account row, the two
// folded diagnostic panels, two game cards, the paste box and the rewritten
// output, and a sign-in view that replaces the lot while it is up. Every
// decision it appears to make is really the C core's; this file moves strings
// between widgets and Core, and keeps the same five pieces of state the
// JavaScript shell keeps.

#pragma once

#include "core.h"

#include <QList>

#include <functional>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
QT_END_NAMESPACE

class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    // Every dialog the app raises goes through these two, and both are
    // asynchronous: Qt for WebAssembly cannot run the nested event loop that
    // QMessageBox::warning() and ::question() need, so a blocking dialog there
    // opens as an empty frame and never answers. open() plus a continuation
    // works on all three targets. Tests subclass these and answer at once.
    virtual void notify(const QString &title, const QString &text,
                        std::function<void()> then = {});
    virtual void confirm(const QString &title, const QString &text,
                         std::function<void(bool)> then);

private:
    // ---- build ----
    QWidget *buildMainView();
    QWidget *buildSignInView();
    QWidget *buildCard(int index);

    // ---- render ----
    void render();
    void renderAccount();
    void renderDebug();
    void renderUserData();
    void renderRoster();
    void setPill(QLabel *pill, const QString &text, const QString &kind = QString());

    // ---- account ----
    // The sign-in path, in continuation style for the same reason the dialogs
    // are: every branch that has to ask the user something resumes in a
    // callback. `interactive` separates a real submit from a silent attempt,
    // and a non-empty `unlockId` names one blob to open rather than searching.
    void attemptSignIn(const QString &username, const QString &password, bool interactive,
                       const QString &unlockId = QString(),
                       std::function<void(bool)> done = {});
    void finishSignIn(const Account &hit);
    void submitSignIn();
    void signOutLocal();
    void setSignInBusy(bool busy);
    void showSignIn(bool show, bool focusPassword = false);
    bool restoreSession();

    // ---- vault ----
    QString vault() const;
    bool saveVault(const QString &v);
    void ingest(const QString &text);
    void deleteUser();
    void deleteAll();

    // ---- clipboard ----
    // Asynchronous everywhere, because in the browser it has to be; `then` is
    // called only when there is text to use.
    void readClipboard(std::function<void(const QString &)> then);
    void copyToClipboard(const QString &text, QLabel *pill);

    // ---- state, the same five the JS shell keeps ----
    QString m_raw;              // what is in the paste box
    QString m_out;              // the rewritten roster
    QList<Block> m_blocks;
    QString m_username;
    QString m_userId;
    QString m_userData;         // plaintext, only while this run holds the key
    bool m_debug = false;
    QString m_unlockTargetId;
    bool m_updating = false;    // suppresses ingest while the box is refilled

    // ---- widgets ----
    QStackedWidget *m_views = nullptr;
    QLabel *m_accountStatus = nullptr;
    QPushButton *m_accountBtn = nullptr;
    QPushButton *m_debugBtn = nullptr;
    QWidget *m_debugPanel = nullptr;

    QLabel *m_userdataStatus = nullptr;
    QPlainTextEdit *m_userdataBox = nullptr;
    QPushButton *m_deleteUserBtn = nullptr;
    QPushButton *m_deleteAllBtn = nullptr;

    QLabel *m_plainStatus = nullptr;
    QPlainTextEdit *m_plainBox = nullptr;
    QPushButton *m_plainUnlockBtn = nullptr;

    QLabel *m_cardWhen[2] = {};
    QLabel *m_cardCount[2] = {};
    QPushButton *m_cardToggle[2] = {};

    QLabel *m_parseStatus = nullptr;
    QLabel *m_outStatus = nullptr;
    QPlainTextEdit *m_pasteIn = nullptr;
    QPlainTextEdit *m_pasteOut = nullptr;

    QLineEdit *m_usernameInput = nullptr;
    QLineEdit *m_passwordInput = nullptr;
    QPushButton *m_signInSubmit = nullptr;
};
