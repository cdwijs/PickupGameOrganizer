// Drives the real widgets under the offscreen platform, the way a user does:
// clicks the account button, types into the form, presses the toggles. The
// rules being exercised are the C core's, which has its own suite in
// prototype-webassembly — what is checked here is that this shell reaches them
// the same way prototype-minimal's does.

#include "core.h"
#include "mainwindow.h"
#include "storage.h"

#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTest>

#include <functional>

namespace {

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
};

void TestMinimal::init()
{
    // Each test starts from an empty device.
    Storage::remove(Storage::VAULT);
    Storage::remove(Storage::SESSION);
    Storage::remove(Storage::DEBUG);
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
    Window fresh;                            // a restart
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
    Window w;
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
