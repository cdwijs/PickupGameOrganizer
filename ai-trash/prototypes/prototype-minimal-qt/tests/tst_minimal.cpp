// Drives the real widgets under the offscreen platform, the way a user does:
// clicks the account button, types into the form, presses the toggles. The
// rules being exercised are the C core's, which has its own suite in
// prototype-webassembly — what is checked here is that this shell reaches them
// the same way prototype-minimal's does.

#include "core.h"
#include "credentials.h"
#include "mainwindow.h"
#include "storage.h"

#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTest>

#include <functional>

namespace {

// A stand-in for the platform keyring. It belongs to the device rather than to
// a window, like Storage, so it survives the restarts the suite fakes by
// destroying one Window and building the next. The real one is never touched:
// a suite that wrote into the machine's GNOME Keyring would leave the tester's
// own passwords behind, and on a build machine there is no keyring at all.
namespace Keyring {
QHash<QString, QString> store;
QStringList reads;
int writes = 0;
// Whether a read can happen without the user noticing — true on Linux and
// Android, false on the PWA, where QtKeychain's backend is a modal form.
bool silent = true;

void reset()
{
    store.clear();
    reads.clear();
    writes = 0;
    silent = true;
}
} // namespace Keyring

const char *const SAMPLE = R"(⚽ Terrible Football Haarlem
🕖 19.00 ~ 21:00

__________________________

🗓️ Friday 07.08.2026

01. Teize
3. Alex

__________________________

🗓️ Monday 10.08.2026

01. Teize
3. Amine
03.
04.
)";

// A window that answers its own dialogs, so a test can take the paths a user
// takes without a modal box stopping it.
class Window : public MainWindow
{
public:
    QStringList notices;
    bool answer = true;

protected:
    // Both hooks are asynchronous in the app because WebAssembly needs them to
    // be; a test answers immediately, which keeps each flow inside the click
    // that started it.
    void notify(const QString &, const QString &text, std::function<void()> then) override
    {
        notices << text;
        if (then)
            then();
    }
    void confirm(const QString &, const QString &text, std::function<void(bool)> then) override
    {
        notices << text;
        then(answer);
    }

    // The keyring, answering at once. QtKeychain is asynchronous on every
    // target, so the app is written to cope with a late answer; a test that
    // had to wait for one would be a test about timers.
    bool credentialsSilent() const override { return Keyring::silent; }
    void readCredential(const QString &key, std::function<void(const QString &)> then) override
    {
        Keyring::reads << key;
        then(Keyring::store.value(key));
    }
    void writeCredential(const QString &key, const QString &password) override
    {
        ++Keyring::writes;
        Keyring::store.insert(key, password);
    }
    void forgetCredential(const QString &key) override { Keyring::store.remove(key); }
};

} // namespace

class TestMinimal : public QObject
{
    Q_OBJECT

private:
    // Helpers that find the controls the way the page's ids did.
    template<typename T>
    static T *find(Window &w, const char *name)
    {
        T *widget = w.findChild<T *>(QString::fromLatin1(name));
        Q_ASSERT(widget);
        return widget;
    }

    static void signIn(Window &w, const QString &user, const QString &password)
    {
        QTest::mouseClick(find<QPushButton>(w, "account-btn"), Qt::LeftButton);
        find<QLineEdit>(w, "signin-username")->setText(user);
        find<QLineEdit>(w, "signin-password")->setText(password);
        QTest::mouseClick(find<QPushButton>(w, "signin-submit"), Qt::LeftButton);
    }

    static QString vault() { return Storage::get(Storage::VAULT); }

    // The credential read at startup is queued, because at that point the
    // window does not exist yet and the real read is asynchronous. A fake
    // restart has to let that queue run.
    static void settle() { QCoreApplication::processEvents(); }

private slots:
    void init();

    void createsAUserAndSignsIn();
    void wrongPasswordIsRefusedAndOffersToCreate();
    void secondUserIsAddedNotOverwritten();
    void sessionSurvivesRestartButTheKeyDoesNot();
    void unlockRefillsThePlaintext();
    void pastedVaultIsMerged();
    void rosterParsesAndTogglesGoing();
    void deleteAllEmptiesTheVault();

    void signingInSavesThePasswordInTheKeyring();
    void savedPasswordUnlocksOnRestart();
    void aStoreThatIsNotSilentIsLeftForTheUnlockButton();
    void unlockUsesTheSavedPassword();
    void aStalePasswordFallsBackToTheForm();
    void deletingAUserForgetsItsSavedPassword();
    void aRealReadAlwaysCallsBack();
};

void TestMinimal::init()
{
    // Each test starts from an empty device.
    Storage::remove(Storage::VAULT);
    Storage::remove(Storage::SESSION);
    Storage::remove(Storage::DEBUG);
    Keyring::reset();
}

void TestMinimal::createsAUserAndSignsIn()
{
    Window w;
    signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));

    // The failure notice comes first, then the offer to create — the same two
    // steps prototype-minimal takes on an empty vault.
    QCOMPARE(w.notices.size(), 2);
    QVERIFY(w.notices.at(0).contains(QStringLiteral("Incorrect username or password")));
    QVERIFY(w.notices.at(1).contains(QStringLiteral("Create a new user \"Cedric\"")));

    QCOMPARE(Core::count(vault()), 1);
    QVERIFY(find<QLabel>(w, "account-status")->text().contains(QStringLiteral("Cedric")));
    // The plaintext is in memory for this sign-in, username line included.
    QVERIFY(find<QPlainTextEdit>(w, "plain-box")->toPlainText().contains(
            QStringLiteral("Readable: Cedric")));
    QVERIFY(find<QPushButton>(w, "delete-user")->isEnabled());
}

void TestMinimal::wrongPasswordIsRefusedAndOffersToCreate()
{
    Window w;
    signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));
    QTest::mouseClick(find<QPushButton>(w, "account-btn"), Qt::LeftButton);  // sign out

    w.notices.clear();
    w.answer = false;                       // decline the create offer
    signIn(w, QStringLiteral("Cedric"), QStringLiteral("wrong"));

    QVERIFY(w.notices.at(0).contains(QStringLiteral("Incorrect username or password")));
    QCOMPARE(Core::count(vault()), 1);      // nothing was added
    QCOMPARE(find<QLabel>(w, "account-status")->text(), QStringLiteral("not signed in"));
}

void TestMinimal::secondUserIsAddedNotOverwritten()
{
    Window w;
    signIn(w, QStringLiteral("Cedric"), QStringLiteral("shared"));
    QTest::mouseClick(find<QPushButton>(w, "account-btn"), Qt::LeftButton);
    // Same password, different user: two blobs, not one.
    signIn(w, QStringLiteral("Alex"), QStringLiteral("shared"));

    QCOMPARE(Core::count(vault()), 2);
    QVERIFY(find<QLabel>(w, "account-status")->text().contains(QStringLiteral("Alex")));

    // And the spelling stored at creation is the one shown, whatever case is typed.
    QTest::mouseClick(find<QPushButton>(w, "account-btn"), Qt::LeftButton);
    signIn(w, QStringLiteral("CEDRIC"), QStringLiteral("shared"));
    QVERIFY(find<QLabel>(w, "account-status")->text().contains(QStringLiteral("Cedric")));
}

void TestMinimal::sessionSurvivesRestartButTheKeyDoesNot()
{
    {
        Window w;
        signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));
    }
    // On a device with nothing in the keyring: what this checks is that the
    // key is not part of the session. The saved-password path, which does get
    // the plaintext back without asking, has its own tests below.
    Keyring::reset();

    Window fresh;                            // a restart
    settle();
    QVERIFY(find<QLabel>(fresh, "account-status")->text().contains(QStringLiteral("Cedric")));
    QVERIFY(find<QPlainTextEdit>(fresh, "plain-box")->toPlainText().isEmpty());

    // The Unlock button lives inside the 🔍🐛 fold, which starts closed, so
    // opening the fold is part of getting at it — as it is on the web page.
    QTest::mouseClick(find<QPushButton>(fresh, "debug-btn"), Qt::LeftButton);
    QVERIFY(find<QPushButton>(fresh, "plain-unlock")->isVisibleTo(&fresh));
    QCOMPARE(Storage::get(Storage::DEBUG), QStringLiteral("1"));
}

void TestMinimal::unlockRefillsThePlaintext()
{
    {
        Window w;
        signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));
    }
    Keyring::reset();                        // nothing saved, so Unlock has to ask

    Window w;
    settle();
    QTest::mouseClick(find<QPushButton>(w, "debug-btn"), Qt::LeftButton);   // open the fold
    QTest::mouseClick(find<QPushButton>(w, "plain-unlock"), Qt::LeftButton);
    // Unlock opens the form on the password field with the user already filled.
    QCOMPARE(find<QLineEdit>(w, "signin-username")->text(), QStringLiteral("Cedric"));

    find<QLineEdit>(w, "signin-password")->setText(QStringLiteral("nope"));
    QTest::mouseClick(find<QPushButton>(w, "signin-submit"), Qt::LeftButton);
    // A wrong password here says so and never offers to create a second user.
    QCOMPARE(w.notices.size(), 1);
    QCOMPARE(w.notices.at(0), QStringLiteral("Incorrect password."));
    QCOMPARE(Core::count(vault()), 1);

    find<QLineEdit>(w, "signin-password")->setText(QStringLiteral("hunter2"));
    QTest::mouseClick(find<QPushButton>(w, "signin-submit"), Qt::LeftButton);
    QVERIFY(find<QPlainTextEdit>(w, "plain-box")->toPlainText().contains(
            QStringLiteral("Readable: Cedric")));
}

void TestMinimal::pastedVaultIsMerged()
{
    QString copied;
    {
        Window w;
        signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));
        copied = find<QPlainTextEdit>(w, "userdata-box")->toPlainText();
        QVERIFY(!copied.isEmpty());
    }
    Storage::remove(Storage::VAULT);         // another device, empty vault

    Window w;
    QCOMPARE(Core::count(vault()), 0);
    find<QPlainTextEdit>(w, "userdata-box")->setPlainText(copied);
    QCOMPARE(Core::count(vault()), 1);

    // Pasting the same thing again is a no-op, not a duplicate.
    find<QPlainTextEdit>(w, "userdata-box")->setPlainText(copied);
    QCOMPARE(Core::count(vault()), 1);

    // And the adopted blob still opens with its original password.
    signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));
    QVERIFY(find<QLabel>(w, "account-status")->text().contains(QStringLiteral("Cedric")));
}

void TestMinimal::rosterParsesAndTogglesGoing()
{
    Window w;
    signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));

    find<QPlainTextEdit>(w, "paste-in")->setPlainText(QString::fromUtf8(SAMPLE));

    auto *toggle0 = find<QPushButton>(w, "toggle-0");
    QVERIFY(toggle0->isEnabled());
    QCOMPARE(toggle0->text(), QStringLiteral("Not going"));

    QTest::mouseClick(toggle0, Qt::LeftButton);
    QCOMPARE(toggle0->text(), QStringLiteral("✓ Going"));
    // Going means the name is in the first block of the rewritten roster.
    const QString out = find<QPlainTextEdit>(w, "paste-out")->toPlainText();
    QVERIFY(out.contains(QStringLiteral("Cedric (app)")));
    QVERIFY(out.contains(QStringLiteral("07.08.2026")));

    QTest::mouseClick(toggle0, Qt::LeftButton);
    QCOMPARE(toggle0->text(), QStringLiteral("Not going"));
    QVERIFY(!find<QPlainTextEdit>(w, "paste-out")->toPlainText().contains(
            QStringLiteral("Cedric (app)")));
}

void TestMinimal::deleteAllEmptiesTheVault()
{
    Window w;
    signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));
    QTest::mouseClick(find<QPushButton>(w, "account-btn"), Qt::LeftButton);
    signIn(w, QStringLiteral("Alex"), QStringLiteral("other"));
    QCOMPARE(Core::count(vault()), 2);

    QTest::mouseClick(find<QPushButton>(w, "delete-all"), Qt::LeftButton);
    QCOMPARE(Core::count(vault()), 0);
    QCOMPARE(find<QLabel>(w, "account-status")->text(), QStringLiteral("not signed in"));
    QVERIFY(!find<QPushButton>(w, "delete-all")->isEnabled());

    // Only the signed-in user's saved password goes with the blobs. Cedric's
    // stays, because the vault keeps every username encrypted inside its own
    // blob and there is no way to name the other users — see README.md.
    QVERIFY(!Keyring::store.contains(QStringLiteral("Alex")));
    QVERIFY(Keyring::store.contains(QStringLiteral("Cedric")));
}

// ---- the credential store ---------------------------------------------------

void TestMinimal::signingInSavesThePasswordInTheKeyring()
{
    Window w;
    signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));
    QCOMPARE(Keyring::store.value(QStringLiteral("Cedric")), QStringLiteral("hunter2"));

    // Keyed by the spelling the blob holds, not the one that was typed, so
    // signing in as CEDRIC does not leave a second entry behind.
    QTest::mouseClick(find<QPushButton>(w, "account-btn"), Qt::LeftButton);
    signIn(w, QStringLiteral("CEDRIC"), QStringLiteral("hunter2"));
    QCOMPARE(Keyring::store.size(), 1);
    QVERIFY(Keyring::store.contains(QStringLiteral("Cedric")));
}

void TestMinimal::savedPasswordUnlocksOnRestart()
{
    {
        Window w;
        signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));
    }
    Window fresh;                            // a restart, keyring intact
    settle();

    // What prototype-minimal's tryPrefill() does on a page load: the session
    // names the user, the keyring hands back the password, and the app spends
    // one derivation on it. No form, no notice, nothing to press.
    QVERIFY(find<QPlainTextEdit>(fresh, "plain-box")->toPlainText().contains(
            QStringLiteral("Readable: Cedric")));
    QVERIFY(fresh.notices.isEmpty());
    QCOMPARE(Keyring::reads, QStringList{ QStringLiteral("Cedric") });
    QTest::mouseClick(find<QPushButton>(fresh, "debug-btn"), Qt::LeftButton);
    QVERIFY(!find<QPushButton>(fresh, "plain-unlock")->isVisibleTo(&fresh));
}

void TestMinimal::aStoreThatIsNotSilentIsLeftForTheUnlockButton()
{
    {
        Window w;
        signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));
    }
    Keyring::silent = false;                 // the PWA: a read is a modal form

    Window fresh;
    settle();
    // Nothing was asked of the store, so nothing was put in the user's way on
    // a page load; the app starts locked and the Unlock button is the way in.
    QVERIFY(Keyring::reads.isEmpty());
    QVERIFY(find<QPlainTextEdit>(fresh, "plain-box")->toPlainText().isEmpty());
    QTest::mouseClick(find<QPushButton>(fresh, "debug-btn"), Qt::LeftButton);
    QVERIFY(find<QPushButton>(fresh, "plain-unlock")->isVisibleTo(&fresh));
}

void TestMinimal::unlockUsesTheSavedPassword()
{
    {
        Window w;
        signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));
    }
    Keyring::silent = false;                 // as on the PWA, where only this
                                             // button may reach the store
    Keyring::writes = 0;                     // the write above was the sign-in

    Window w;
    settle();
    QTest::mouseClick(find<QPushButton>(w, "debug-btn"), Qt::LeftButton);
    QTest::mouseClick(find<QPushButton>(w, "plain-unlock"), Qt::LeftButton);

    // Unlocked from the store, and the sign-in form was never shown.
    QVERIFY(find<QPlainTextEdit>(w, "plain-box")->toPlainText().contains(
            QStringLiteral("Readable: Cedric")));
    QCOMPARE(w.findChild<QStackedWidget *>()->currentIndex(), 0);
    QVERIFY(w.notices.isEmpty());

    // And the password is not written back. It is already in the store, and on
    // the PWA a write is a second modal: unlocking would close the "Sign In"
    // form only to open a "Save" one.
    QCOMPARE(Keyring::writes, 0);
}

void TestMinimal::aStalePasswordFallsBackToTheForm()
{
    {
        Window w;
        signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));
    }
    // A password the blob no longer accepts — changed on another device, or an
    // entry left over from an older vault.
    Keyring::store.insert(QStringLiteral("Cedric"), QStringLiteral("stale"));

    Window w;
    settle();
    // The silent attempt at startup fails without a word, exactly as a missing
    // entry would.
    QVERIFY(find<QPlainTextEdit>(w, "plain-box")->toPlainText().isEmpty());
    QVERIFY(w.notices.isEmpty());

    // And Unlock asks, rather than failing again in silence.
    QTest::mouseClick(find<QPushButton>(w, "debug-btn"), Qt::LeftButton);
    QTest::mouseClick(find<QPushButton>(w, "plain-unlock"), Qt::LeftButton);
    QCOMPARE(find<QLineEdit>(w, "signin-username")->text(), QStringLiteral("Cedric"));

    // Typing the right one replaces the stale entry.
    find<QLineEdit>(w, "signin-password")->setText(QStringLiteral("hunter2"));
    QTest::mouseClick(find<QPushButton>(w, "signin-submit"), Qt::LeftButton);
    QCOMPARE(Keyring::store.value(QStringLiteral("Cedric")), QStringLiteral("hunter2"));
}

void TestMinimal::deletingAUserForgetsItsSavedPassword()
{
    Window w;
    signIn(w, QStringLiteral("Cedric"), QStringLiteral("hunter2"));
    QVERIFY(Keyring::store.contains(QStringLiteral("Cedric")));

    QTest::mouseClick(find<QPushButton>(w, "debug-btn"), Qt::LeftButton);
    QTest::mouseClick(find<QPushButton>(w, "delete-user"), Qt::LeftButton);

    QCOMPARE(Core::count(vault()), 0);
    // The blob is gone, so the password that opened it has nothing left to
    // open; leaving it in the keyring would be a stray secret.
    QVERIFY(!Keyring::store.contains(QStringLiteral("Cedric")));
}

// The one thing the fake above cannot check: that the real Cred::read() keeps
// its promise to call back exactly once whatever the machine has. Every
// fallback in the app hangs off the miss — Unlock only opens the form because
// the read came back empty — so a backend that answered by saying nothing
// would leave the button dead. This reads a key nothing will ever have stored
// and writes nothing, so it is safe on a developer's machine as well as on a
// build box with no keyring at all.
void TestMinimal::aRealReadAlwaysCallsBack()
{
    int calls = 0;
    QString got = QStringLiteral("untouched");
    Cred::read(QStringLiteral("prototype-minimal-qt test: no such user"),
               [&](const QString &password) {
                   ++calls;
                   got = password;
               });

    QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 10000);
    QVERIFY(got.isEmpty());

    // And it stays called once — no second answer from a queued job.
    QTest::qWait(200);
    QCOMPARE(calls, 1);
    qInfo("backend available here: %s", Cred::available() ? "yes" : "no");
}

int main(int argc, char *argv[])
{
    // Keep the suite off the real user's settings.
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("pgo"));
    QApplication::setApplicationName(QStringLiteral("prototype-minimal-qt"));
    TestMinimal tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_minimal.moc"
