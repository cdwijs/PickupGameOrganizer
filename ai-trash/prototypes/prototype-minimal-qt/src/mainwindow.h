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

    // Where a password came from, which decides whether it is worth offering
    // to the keyring. One that came out of the keyring is already in it, and
    // on the PWA a write is a second modal — so an unlock from a saved
    // password would put a "Save" form on screen the instant the "Sign In"
    // one closed.
    enum class PasswordSource { Typed, Keyring };

protected:
    // Taps on the text widgets are watched so the soft keyboard can be asked
    // for explicitly; see openSoftKeyboard() in the .cpp.
    bool eventFilter(QObject *watched, QEvent *event) override;

    // Every dialog the app raises goes through these two, and both are
    // asynchronous: Qt for WebAssembly cannot run the nested event loop that
    // QMessageBox::warning() and ::question() need, so a blocking dialog there
    // opens as an empty frame and never answers. open() plus a continuation
    // works on all three targets. Tests subclass these and answer at once.
    virtual void notify(const QString &title, const QString &text,
                        std::function<void()> then = {});
    virtual void confirm(const QString &title, const QString &text,
                         std::function<void(bool)> then);

    // The credential store, behind the same kind of hook and for the same
    // reason: QtKeychain's jobs are asynchronous on every target, and on the
    // PWA they are asynchronous because a person has to answer a form. `then`
    // runs only when there is a password to use, the way readClipboard()
    // does. Tests override all four so the suite never reaches — or writes
    // into — the machine's real keyring.
    virtual bool credentialsSilent() const;
    virtual void readCredential(const QString &key, std::function<void(const QString &)> then);
    virtual void writeCredential(const QString &key, const QString &password);
    virtual void forgetCredential(const QString &key);

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
                       std::function<void(bool)> done = {},
                       PasswordSource source = PasswordSource::Typed);
    void finishSignIn(const Account &hit, const QString &password, PasswordSource source);
    void submitSignIn();
    // The two places the credential store is consulted: once at startup on a
    // restored session, where it can be read without the user noticing, and
    // when Unlock is pressed, which is a deliberate act and therefore the only
    // route the PWA's modal form can take. Both are no-ops when there is
    // nothing stored.
    void trySavedPassword();
    void unlock();
    void showUnlockForm();
    void signOutLocal();
    void setSignInBusy(bool busy);
    void showSignIn(bool show, bool focusPassword = false);
    void focusField(QLineEdit *field);
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
    // TEMPORARY — the on-screen half of the repaint/IME investigation. Both
    // "typing does not show" and "signing out takes forever" look like
    // content-only repaints never reaching the screen; this reports what the
    // widgets actually receive and how long the work actually takes, so the
    // phone can answer it without adb. Remove once the cause is settled.
    QLabel *m_diag = nullptr;
    int m_imeCount = 0;
    int m_keyCount = 0;
    QString m_lastIme;
    qint64 m_lastRenderMs = -1;
    void updateDiag();

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
