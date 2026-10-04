#include "mainwindow.h"

#include "clipboard.h"
#include "credentials.h"
#include "storage.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QInputMethodEvent>
#include <QFontMetrics>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputMethod>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {

const char *const PLAIN_PLACEHOLDER =
        "Nothing decrypted. Sign in to unlock a blob; the plaintext appears here.";
const char *const PLAIN_LOCKED =
        "Locked. Press Unlock and enter your password to see the plaintext.";

QLabel *hint(const QString &text)
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setEnabled(false);   // the native equivalent of the muted hint text
    return l;
}

// A section heading with its pill on the right, the way every <h2> on
// prototype-minimal's page carries one.
QWidget *heading(const QString &title, QLabel **pillOut, const QString &pillText)
{
    auto *row = new QWidget;
    auto *h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);

    auto *label = new QLabel(QStringLiteral("<b>%1</b>").arg(title));
    auto *pill = new QLabel(pillText);
    pill->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    h->addWidget(label);
    h->addStretch();
    h->addWidget(pill);
    *pillOut = pill;
    return row;
}

// Android raises the soft keyboard only when the platform is asked to, and
// nothing here was asking. A programmatic setFocus() is not a tap, so it does
// not ask; and a tap on a field that already holds focus changes no focus, so
// that does not ask either. Between them the sign-in form could not be typed
// into on a phone — while long-press Paste still worked, because that is the
// context menu rather than the keyboard. Every route into a text widget now
// asks explicitly. A no-op on desktop, where there is no software keyboard.
void openSoftKeyboard()
{
    if (QInputMethod *im = QGuiApplication::inputMethod())
        im->show();
}

QFrame *separator()
{
    auto *line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(tr("Minimal Prototype — Qt"));

    m_debug = Storage::get(Storage::DEBUG) == QLatin1String("1");

    m_views = new QStackedWidget(this);
    m_views->addWidget(buildMainView());
    m_views->addWidget(buildSignInView());

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(m_views);

    for (QWidget *input : { static_cast<QWidget *>(m_usernameInput),
                            static_cast<QWidget *>(m_passwordInput),
                            static_cast<QWidget *>(m_userdataBox),
                            static_cast<QWidget *>(m_pasteIn) })
        input->installEventFilter(this);

    const bool restored = restoreSession();
    render();
    resize(520, 760);

    // A restored session carries the username but not the key, so the app
    // would start locked. This is the one moment prototype-minimal calls
    // tryPrefill(), and the credential store is the only way to fill the
    // decrypted box without asking. Queued rather than called: the read is
    // asynchronous everywhere, and this window is not on screen yet.
    if (restored)
        QTimer::singleShot(0, this, &MainWindow::trySavedPassword);
}

// ---- construction -----------------------------------------------------------

QWidget *MainWindow::buildMainView()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(16, 16, 16, 16);
    v->setSpacing(12);

    // Account row: status, the 🔍🐛 fold, sign in / sign out.
    {
        auto *row = new QHBoxLayout;
        m_accountStatus = new QLabel(tr("not signed in"));
        m_accountStatus->setObjectName(QStringLiteral("account-status"));
        // The wasm build carries one bundled font and it has no emoji, so the
        // magnifier and bug come out as two empty boxes there. Ask before
        // using them.
        const bool hasEmoji = QFontMetrics(font()).inFontUcs4(0x1F50D)
                && QFontMetrics(font()).inFontUcs4(0x1F41B);
        m_debugBtn = new QPushButton(hasEmoji ? QStringLiteral("🔍🐛") : tr("debug"));
        m_debugBtn->setObjectName(QStringLiteral("debug-btn"));
        m_debugBtn->setToolTip(tr("Show or hide the encrypted and decrypted user data"));
        m_debugBtn->setCheckable(true);
        m_debugBtn->setFixedWidth(56);
        m_accountBtn = new QPushButton(tr("Sign in"));
        m_accountBtn->setObjectName(QStringLiteral("account-btn"));

        row->addWidget(m_accountStatus);
        row->addStretch();
        row->addWidget(m_debugBtn);
        row->addWidget(m_accountBtn);
        v->addLayout(row);

        connect(m_debugBtn, &QPushButton::clicked, this, [this] {
            m_debug = !m_debug;
            Storage::set(Storage::DEBUG, m_debug ? QStringLiteral("1") : QStringLiteral("0"));
            render();
        });
        connect(m_accountBtn, &QPushButton::clicked, this, [this] {
            if (!m_username.isEmpty()) {
                signOutLocal();
                render();
            } else {
                m_unlockTargetId.clear();
                showSignIn(true);
            }
        });
    }

    // Both diagnostic panels, folded away as one unit.
    m_debugPanel = new QWidget;
    {
        auto *d = new QVBoxLayout(m_debugPanel);
        d->setContentsMargins(0, 0, 0, 0);
        d->setSpacing(8);

        d->addWidget(separator());

        // TEMPORARY diagnostic. See the note in mainwindow.h.
        m_diag = new QLabel;
        m_diag->setObjectName(QStringLiteral("diag"));
        m_diag->setWordWrap(true);
        m_diag->setTextInteractionFlags(Qt::TextSelectableByMouse);
        d->addWidget(m_diag);

        d->addWidget(heading(tr("Encrypted user data"), &m_userdataStatus, tr("empty")));

        m_userdataBox = new QPlainTextEdit;
        m_userdataBox->setObjectName(QStringLiteral("userdata-box"));
        // Hex blobs and JSON: predictive text has nothing useful to add, and
        // a composing box paints its placeholder underneath the text being
        // composed. ImhSensitiveData for the same reason as the username
        // field — it is the hint that actually stops the composing, and Qt
        // still applies the multi-line flag after it.
        m_userdataBox->setInputMethodHints(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText
                                           | Qt::ImhSensitiveData);
        m_userdataBox->setPlaceholderText(
                tr("No users yet. Sign in and answer \"Create\" to add one, or paste blobs "
                   "from another device here."));
        m_userdataBox->setFixedHeight(110);
        d->addWidget(m_userdataBox);

        auto *copyBtn = new QPushButton(tr("Copy to clipboard"));
        auto *pasteBtn = new QPushButton(tr("Paste from clipboard"));
        auto *row1 = new QHBoxLayout;
        row1->addWidget(copyBtn);
        row1->addWidget(pasteBtn);
        row1->addStretch();
        d->addLayout(row1);

        m_deleteUserBtn = new QPushButton(tr("Delete this user"));
        m_deleteUserBtn->setObjectName(QStringLiteral("delete-user"));
        m_deleteAllBtn = new QPushButton(tr("Delete all users"));
        m_deleteAllBtn->setObjectName(QStringLiteral("delete-all"));
        auto *row2 = new QHBoxLayout;
        row2->addWidget(m_deleteUserBtn);
        row2->addWidget(m_deleteAllBtn);
        row2->addStretch();
        d->addLayout(row2);

        d->addWidget(hint(tr("Each entry carries its own salt next to its blob — a blob without "
                             "its salt cannot be decrypted. Nothing here reveals a username or a "
                             "password.")));

        d->addWidget(separator());
        d->addWidget(heading(tr("Decrypted user data"), &m_plainStatus, QStringLiteral("—")));

        m_plainBox = new QPlainTextEdit;
        m_plainBox->setObjectName(QStringLiteral("plain-box"));
        m_plainBox->setReadOnly(true);
        m_plainBox->setPlaceholderText(tr(PLAIN_PLACEHOLDER));
        m_plainBox->setFixedHeight(110);
        d->addWidget(m_plainBox);

        m_plainUnlockBtn = new QPushButton(tr("Unlock"));
        m_plainUnlockBtn->setObjectName(QStringLiteral("plain-unlock"));
        auto *row3 = new QHBoxLayout;
        row3->addWidget(m_plainUnlockBtn);
        row3->addStretch();
        d->addLayout(row3);

        d->addWidget(hint(tr("Read-only, and never stored in the clear — this is the plaintext "
                             "held in memory for the current sign-in. A restart keeps the session "
                             "but not the key, so it has to be unlocked again.")));
        d->addWidget(separator());

        connect(copyBtn, &QPushButton::clicked, this, [this] {
            copyToClipboard(m_userdataBox->toPlainText(), m_userdataStatus);
        });
        connect(pasteBtn, &QPushButton::clicked, this, [this] {
            readClipboard([this](const QString &text) {
                m_updating = true;
                m_userdataBox->setPlainText(text);
                m_updating = false;
                ingest(text);
            });
        });
        // Typing or pasting into the box merges it, exactly as the web page's
        // `input` handler does. Programmatic refills set m_updating first.
        connect(m_userdataBox, &QPlainTextEdit::textChanged, this, [this] {
            if (!m_updating)
                ingest(m_userdataBox->toPlainText());
        });
        connect(m_deleteUserBtn, &QPushButton::clicked, this, &MainWindow::deleteUser);
        connect(m_deleteAllBtn, &QPushButton::clicked, this, &MainWindow::deleteAll);
        connect(m_plainUnlockBtn, &QPushButton::clicked, this, &MainWindow::unlock);
    }
    v->addWidget(m_debugPanel);

    // Agenda: two cards.
    v->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(tr("Agenda"))));
    v->addWidget(buildCard(0));
    v->addWidget(buildCard(1));

    // Paste roster.
    v->addWidget(heading(tr("Paste roster"), &m_parseStatus, tr("empty")));
    m_pasteIn = new QPlainTextEdit;
    m_pasteIn->setObjectName(QStringLiteral("paste-in"));
    m_pasteIn->setInputMethodHints(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText
                                   | Qt::ImhSensitiveData);
    m_pasteIn->setPlaceholderText(
            tr("Paste the roster message here… Parsing extracts the first two 🗓️ date blocks "
               "and their player lists. The cards above update as soon as you paste."));
    m_pasteIn->setMinimumHeight(120);
    v->addWidget(m_pasteIn);
    {
        auto *pasteBtn = new QPushButton(tr("Paste from clipboard"));
        auto *clearBtn = new QPushButton(tr("Clear"));
        auto *row = new QHBoxLayout;
        row->addWidget(pasteBtn);
        row->addWidget(clearBtn);
        row->addStretch();
        v->addLayout(row);

        connect(m_pasteIn, &QPlainTextEdit::textChanged, this, [this] {
            if (m_updating)
                return;
            m_raw = m_pasteIn->toPlainText();
            render();
        });
        connect(pasteBtn, &QPushButton::clicked, this, [this] {
            readClipboard([this](const QString &text) {
                m_updating = true;
                m_pasteIn->setPlainText(text);
                m_updating = false;
                m_raw = text;
                render();
            });
        });
        connect(clearBtn, &QPushButton::clicked, this, [this] {
            m_updating = true;
            m_pasteIn->clear();
            m_updating = false;
            m_raw.clear();
            render();
        });
    }

    // Updated roster.
    v->addWidget(heading(tr("Updated roster"), &m_outStatus, QStringLiteral("—")));
    m_pasteOut = new QPlainTextEdit;
    m_pasteOut->setObjectName(QStringLiteral("paste-out"));
    m_pasteOut->setReadOnly(true);
    m_pasteOut->setPlaceholderText(tr("The updated roster appears here."));
    m_pasteOut->setMinimumHeight(120);
    v->addWidget(m_pasteOut);
    {
        auto *copyBtn = new QPushButton(tr("Copy to clipboard"));
        auto *row = new QHBoxLayout;
        row->addWidget(copyBtn);
        row->addStretch();
        v->addLayout(row);
        connect(copyBtn, &QPushButton::clicked, this, [this] {
            copyToClipboard(m_pasteOut->toPlainText(), m_outStatus);
        });
    }

    v->addStretch();

    // The page is taller than a phone, so it scrolls — the one thing the
    // browser did for free.
    auto *scroll = new QScrollArea;
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

QWidget *MainWindow::buildCard(int index)
{
    auto *card = new QFrame;
    card->setFrameShape(QFrame::StyledPanel);
    auto *h = new QHBoxLayout(card);

    auto *mid = new QVBoxLayout;
    mid->addWidget(new QLabel(QStringLiteral("<b>Terrible Football Haarlem</b>")));
    m_cardWhen[index] = new QLabel(QStringLiteral("—"));
    mid->addWidget(m_cardWhen[index]);

    auto *right = new QVBoxLayout;
    m_cardCount[index] = new QLabel(QStringLiteral("0"));
    m_cardCount[index]->setAlignment(Qt::AlignRight);
    auto *players = new QLabel(tr("players"));
    players->setAlignment(Qt::AlignRight);
    players->setEnabled(false);
    m_cardToggle[index] = new QPushButton(tr("Not going"));
    m_cardToggle[index]->setObjectName(QStringLiteral("toggle-%1").arg(index));
    m_cardToggle[index]->setEnabled(false);

    right->addWidget(m_cardCount[index]);
    right->addWidget(players);
    right->addWidget(m_cardToggle[index]);

    h->addLayout(mid, 1);
    h->addLayout(right);

    connect(m_cardToggle[index], &QPushButton::clicked, this, [this, index] {
        if (m_username.isEmpty() || index >= m_blocks.size())
            return;
        m_out = Core::toggle(&m_raw, m_username, index);
        m_updating = true;
        m_pasteIn->setPlainText(m_raw);
        m_updating = false;
        render();
    });
    return card;
}

QWidget *MainWindow::buildSignInView()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(16, 16, 16, 16);
    v->setSpacing(8);

    v->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(tr("Sign in"))));

    v->addWidget(new QLabel(tr("Username")));
    m_usernameInput = new QLineEdit;
    m_usernameInput->setObjectName(QStringLiteral("signin-username"));
    // Android's keyboard composes text before committing it, and a composing
    // QLineEdit paints nothing until the composition ends — typing into this
    // field looked dead until focus moved away, at which point everything
    // typed appeared at once.
    //
    // ImhNoPredictiveText alone does not stop it, which is why the first
    // attempt at this changed nothing on the phone: Qt only turns that hint
    // into TYPE_TEXT_FLAG_NO_SUGGESTIONS when the environment variable
    // QT_ANDROID_ENABLE_WORKAROUND_TO_DISABLE_PREDICTIVE_TEXT is set
    // (QtEditText.isDisablePredictiveTextWorkaround in Qt6Android.jar), and
    // nothing sets it. ImhSensitiveData needs no opt-in: Qt maps it to
    // TYPE_TEXT_VARIATION_VISIBLE_PASSWORD, an input type keyboards do not
    // compose in, so every character is committed as it is typed and painted
    // immediately. It is the same reason the password field was never
    // affected — QLineEdit adds ImhHiddenText for a non-Normal echo mode,
    // which maps to the password input type.
    //
    // The hints together are also what prototype-minimal asks the browser for
    // on this field: autocapitalize="none", autocorrect="off",
    // spellcheck="false".
    m_usernameInput->setInputMethodHints(Qt::ImhNoAutoUppercase | Qt::ImhNoPredictiveText
                                         | Qt::ImhSensitiveData);
    v->addWidget(m_usernameInput);

    v->addWidget(new QLabel(tr("Password")));
    m_passwordInput = new QLineEdit;
    m_passwordInput->setObjectName(QStringLiteral("signin-password"));
    m_passwordInput->setEchoMode(QLineEdit::Password);
    v->addWidget(m_passwordInput);

    m_signInSubmit = new QPushButton(tr("Sign in"));
        m_signInSubmit->setObjectName(QStringLiteral("signin-submit"));
    m_signInSubmit->setDefault(true);
    auto *cancel = new QPushButton(tr("Cancel"));
    v->addWidget(m_signInSubmit);
    v->addWidget(cancel);

    v->addWidget(hint(tr("Signing in tries every stored blob with this password and keeps the "
                         "one whose encrypted username matches; if none does, the app offers to "
                         "create a new user.")));
    v->addStretch();

    connect(m_signInSubmit, &QPushButton::clicked, this, &MainWindow::submitSignIn);
    connect(m_passwordInput, &QLineEdit::returnPressed, this, &MainWindow::submitSignIn);
    connect(m_usernameInput, &QLineEdit::returnPressed, this, &MainWindow::submitSignIn);
    connect(cancel, &QPushButton::clicked, this, [this] {
        m_usernameInput->setText(m_username);
        m_passwordInput->clear();
        m_unlockTargetId.clear();
        showSignIn(false);
    });

    // The soft keyboard takes half the screen; the form has to be reachable
    // under it.
    auto *scroll = new QScrollArea;
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    return scroll;
}

// ---- dialogs ----------------------------------------------------------------

void MainWindow::notify(const QString &title, const QString &text, std::function<void()> then)
{
    auto *box = new QMessageBox(QMessageBox::Warning, title, text, QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QDialog::finished, this, [then](int) {
        if (then)
            then();
    });
    box->open();
}

void MainWindow::confirm(const QString &title, const QString &text,
                         std::function<void(bool)> then)
{
    auto *box = new QMessageBox(QMessageBox::Question, title, text,
                                QMessageBox::Yes | QMessageBox::No, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    connect(box, &QDialog::finished, this, [then](int result) {
        then(result == QMessageBox::Yes);
    });
    box->open();
}

// ---- credential store -------------------------------------------------------
//
// Four one-line forwards to Cred:: so the whole of the platform's keyring sits
// behind a hook a test can replace, like the two dialogs above it.

bool MainWindow::credentialsSilent() const
{
    return Cred::silent() && Cred::available();
}

void MainWindow::readCredential(const QString &key, std::function<void(const QString &)> then)
{
    // A read outlives the widget that started it: QtKeychain queues its jobs
    // on the application, so a keyring still thinking when the window closes
    // will answer a `this` that is gone. Both callers capture `this`, so the
    // guard belongs here rather than in each of them.
    QPointer<MainWindow> alive(this);
    Cred::read(key, [alive, then = std::move(then)](const QString &password) {
        if (alive && then)
            then(password);
    });
}

void MainWindow::writeCredential(const QString &key, const QString &password)
{
    Cred::write(key, password);
}

void MainWindow::forgetCredential(const QString &key)
{
    Cred::forget(key);
}

// ---- render -----------------------------------------------------------------

void MainWindow::setPill(QLabel *pill, const QString &text, const QString &kind)
{
    pill->setText(text);
    // The web page colours these with a CSS class. Native widgets have no
    // equivalent, and the state is worth seeing, so the pill text alone is
    // coloured — no theme, no stylesheet on anything else.
    if (kind == QLatin1String("ok"))
        pill->setStyleSheet(QStringLiteral("color: #1a7f37;"));
    else if (kind == QLatin1String("err"))
        pill->setStyleSheet(QStringLiteral("color: #b42318;"));
    else
        pill->setStyleSheet(QString());
}

void MainWindow::renderAccount()
{
    if (!m_username.isEmpty()) {
        setPill(m_accountStatus, tr("signed in as %1").arg(m_username), QStringLiteral("ok"));
        m_accountBtn->setText(tr("Sign out"));
    } else {
        setPill(m_accountStatus, tr("not signed in"));
        m_accountBtn->setText(tr("Sign in"));
    }
}

void MainWindow::renderDebug()
{
    m_debugPanel->setVisible(m_debug);
    m_debugBtn->setChecked(m_debug);
}

void MainWindow::renderUserData()
{
    const QString v = vault();
    const int count = Core::count(v);

    // Never overwrite what the user is typing — the web page checks
    // document.activeElement for the same reason.
    if (!m_userdataBox->hasFocus()) {
        m_updating = true;
        m_userdataBox->setPlainText(count ? Core::text(v) : QString());
        m_updating = false;
    }

    if (!count) {
        setPill(m_userdataStatus, tr("empty"));
    } else {
        const QString n = count == 1 ? tr("1 blob") : tr("%1 blobs").arg(count);
        setPill(m_userdataStatus,
                m_userData.isEmpty() ? n : tr("%1 · unlocked").arg(n),
                QStringLiteral("ok"));
    }
    m_deleteUserBtn->setEnabled(!m_userId.isEmpty());
    m_deleteAllBtn->setEnabled(count > 0);

    m_plainBox->setPlainText(m_userData);
    if (!m_userData.isEmpty())
        setPill(m_plainStatus, tr("%1 chars").arg(m_userData.size()), QStringLiteral("ok"));
    else
        setPill(m_plainStatus, m_username.isEmpty() ? QStringLiteral("—") : tr("locked"));

    const bool locked = !m_username.isEmpty() && m_userData.isEmpty();
    m_plainUnlockBtn->setVisible(locked);
    m_plainBox->setPlaceholderText(locked ? tr(PLAIN_LOCKED) : tr(PLAIN_PLACEHOLDER));
}

void MainWindow::renderRoster()
{
    m_blocks = Core::parse(m_raw, m_username, &m_out);

    if (m_raw.isEmpty()) {
        setPill(m_parseStatus, tr("empty"));
        setPill(m_outStatus, QStringLiteral("—"));
    } else {
        const int n = m_blocks.size();
        setPill(m_parseStatus,
                n == 1 ? tr("1 date block") : tr("%1 date blocks").arg(n),
                n ? QStringLiteral("ok") : QStringLiteral("err"));
        setPill(m_outStatus, tr("%1 chars").arg(m_out.size()), QStringLiteral("ok"));
    }

    // Two cards; extra blocks are ignored and a missing block resets its card.
    for (int i = 0; i < 2; ++i) {
        if (i >= m_blocks.size()) {
            m_cardWhen[i]->setText(QStringLiteral("—"));
            m_cardCount[i]->setText(QStringLiteral("0"));
            m_cardToggle[i]->setText(tr("Not going"));
            m_cardToggle[i]->setEnabled(false);
            continue;
        }
        const Block &b = m_blocks.at(i);
        m_cardWhen[i]->setText(b.time.isEmpty() ? b.weekday
                                                : QStringLiteral("%1 · %2").arg(b.weekday, b.time));
        m_cardCount[i]->setText(QString::number(b.count));
        m_cardToggle[i]->setEnabled(!m_username.isEmpty());
        m_cardToggle[i]->setText(b.going ? tr("✓ Going") : tr("Not going"));
    }

    m_updating = true;
    m_pasteOut->setPlainText(m_raw.isEmpty() ? QString() : m_out);
    m_updating = false;
}

void MainWindow::render()
{
    // TEMPORARY timing + repaint. See the note in mainwindow.h: this says
    // whether "sign out takes a long time" is work or a missing repaint.
    QElapsedTimer timer;
    timer.start();

    renderAccount();
    renderDebug();
    renderUserData();
    renderRoster();

    m_lastRenderMs = timer.elapsed();
    updateDiag();
    window()->update();
}

// ---- account ----------------------------------------------------------------

void MainWindow::showSignIn(bool show, bool focusPassword)
{
    m_views->setCurrentIndex(show ? 1 : 0);
    if (show) {
        QLineEdit *field = focusPassword ? m_passwordInput : m_usernameInput;
        // After the view switch, so the field is on screen when it takes focus.
        QTimer::singleShot(0, this, [this, field] { focusField(field); });
    }
}

void MainWindow::focusField(QLineEdit *field)
{
    field->setFocus();
    openSoftKeyboard();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    // A tap on an already-focused field produces no focus change, so this is
    // the only chance to bring the keyboard back after it has been dismissed.
    if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::FocusIn) {
        if (auto *widget = qobject_cast<QWidget *>(watched); widget && widget->isEnabled())
            openSoftKeyboard();
    }

    // TEMPORARY diagnostic + candidate fix. If the input is arriving and the
    // screen simply is not being refreshed, the text is already in the widget
    // by the time this runs and a full-window repaint will reveal it; the
    // label then shows an event count that keeps climbing while the field
    // still looks empty, which is the same finding from the other side.
    if (event->type() == QEvent::InputMethod) {
        auto *ime = static_cast<QInputMethodEvent *>(event);
        ++m_imeCount;
        m_lastIme = QStringLiteral("pre=\"%1\" commit=\"%2\"")
                            .arg(ime->preeditString(), ime->commitString());
        updateDiag();
        if (auto *widget = qobject_cast<QWidget *>(watched))
            widget->window()->update();
    } else if (event->type() == QEvent::KeyPress) {
        ++m_keyCount;
        updateDiag();
        if (auto *widget = qobject_cast<QWidget *>(watched))
            widget->window()->update();
    }

    return QWidget::eventFilter(watched, event);
}

// TEMPORARY. See the note in mainwindow.h.
void MainWindow::updateDiag()
{
    if (!m_diag)
        return;
    m_diag->setText(QStringLiteral("ime %1 · key %2 · render %3 ms\n%4")
                            .arg(m_imeCount)
                            .arg(m_keyCount)
                            .arg(m_lastRenderMs < 0 ? QStringLiteral("—")
                                                    : QString::number(m_lastRenderMs))
                            .arg(m_lastIme));
}

void MainWindow::setSignInBusy(bool busy)
{
    m_signInSubmit->setDisabled(busy);
    m_signInSubmit->setText(busy ? tr("Working…") : tr("Sign in"));
    m_usernameInput->setDisabled(busy);
    m_passwordInput->setDisabled(busy);
}

void MainWindow::signOutLocal()
{
    m_username.clear();
    m_userId.clear();
    m_userData.clear();
    m_passwordInput->clear();
    Storage::remove(Storage::SESSION);
}

void MainWindow::attemptSignIn(const QString &username, const QString &password,
                               bool interactive, const QString &unlockId,
                               std::function<void(bool)> done, PasswordSource source)
{
    const auto finished = [done](bool ok) {
        if (done)
            done(ok);
    };

    setSignInBusy(true);
    // 310 000 PBKDF2 iterations per stored blob, synchronously, so let the
    // disabled state paint before the core takes the thread.
    QApplication::processEvents();

    if (!unlockId.isEmpty()) {
        const Account hit = Core::unlock(vault(), unlockId, password);
        setSignInBusy(false);
        if (hit.error == QLatin1String("gone")) {
            signOutLocal();
            showSignIn(false);
            render();
            if (interactive)
                notify(tr("Sign in"), tr("That user was deleted on this device."));
            finished(false);
            return;
        }
        if (!hit.ok()) {
            // Unlock knows the account exists, so a wrong password says so and
            // never offers to create a second one.
            if (interactive)
                notify(tr("Sign in"), tr("Incorrect password."));
            finished(false);
            return;
        }
        finishSignIn(hit, password, source);
        finished(true);
        return;
    }

    const Account hit = Core::signIn(vault(), username, password);
    setSignInBusy(false);
    if (hit.ok()) {
        finishSignIn(hit, password, source);
        finished(true);
        return;
    }

    if (!interactive) {
        finished(false);
        return;
    }

    // One message for every failure: unknown user, wrong password and someone
    // else's password all look the same from outside. Then the offer to create.
    notify(tr("Sign in"), tr("Incorrect username or password."), [this, username, password, finished] {
        confirm(tr("Create user"),
                tr("Create a new user \"%1\" with this password?").arg(username.trimmed()),
                [this, username, password, finished](bool yes) {
                    if (!yes) {
                        finished(false);
                        return;
                    }
                    setSignInBusy(true);
                    QApplication::processEvents();
                    const Account made = Core::create(vault(), username.trimmed(), password);
                    setSignInBusy(false);
                    if (!made.ok()) {
                        notify(tr("Create user"),
                               tr("Could not create the user: %1").arg(made.error));
                        finished(false);
                        return;
                    }
                    if (!saveVault(made.vault)) {
                        notify(tr("Create user"),
                               tr("Created, but this device would not store it — copy the "
                                  "vault out of the user-data box, it is gone on restart."));
                    }
                    finishSignIn(made, password, PasswordSource::Typed);
                    finished(true);
                });
    });
}

// Everything a successful sign-in, unlock or create has in common.
void MainWindow::finishSignIn(const Account &hit, const QString &password, PasswordSource source)
{
    m_username = hit.username;
    m_userId = hit.id;
    m_userData = hit.text;

    QJsonObject session;
    session.insert(QStringLiteral("id"), hit.id);
    session.insert(QStringLiteral("username"), hit.username);
    Storage::set(Storage::SESSION,
                 QString::fromUtf8(QJsonDocument(session).toJson(QJsonDocument::Compact)));

    // Offer the password to the platform's keyring, exactly where
    // prototype-minimal calls storeCredential(): after a sign-in, an unlock or
    // a create, once the blob has proved the password right. Keyed by the
    // stored spelling of the username rather than the typed one, so a login as
    // "CEDRIC" does not leave a second entry behind. The write is fire and
    // forget — a refused or missing keyring is not a failed sign-in, it only
    // means the next start asks again. A password that came out of the keyring
    // is not written back: it is already there, and on the PWA it would raise
    // a "Save" form the moment the "Sign In" one closed.
    if (!password.isEmpty() && source == PasswordSource::Typed)
        writeCredential(hit.username, password);

    m_passwordInput->clear();
    showSignIn(false);
    render();
}

// Startup, on a restored session. The equivalent of prototype-minimal's
// tryPrefill() for the case where the session is already known: look the
// restored user up in the keyring and, if there is a password, spend one
// derivation on it. A miss, a refusal, a keyring that is not running or a
// password that has since been changed all end the same way — the app stays
// locked and the Unlock button is still there.
void MainWindow::trySavedPassword()
{
    if (!credentialsSilent() || m_userId.isEmpty() || !m_userData.isEmpty())
        return;

    const QString id = m_userId;
    readCredential(m_username, [this, id](const QString &password) {
        // The read came back from an event loop, so the app may have moved on:
        // the user may have unlocked by hand, signed out, signed in as someone
        // else or deleted the blob while the keyring was answering. Only the
        // situation the read was started for is still worth acting on.
        if (password.isEmpty() || m_userId != id || !m_userData.isEmpty()
            || m_views->currentIndex() != 0)
            return;
        attemptSignIn(m_username, password, /*interactive=*/false, id, {},
                      PasswordSource::Keyring);
    });
}

// The Unlock button. It asks the keyring first, which is what makes the store
// reachable on the PWA at all: there the read is a modal form, so it needs an
// act by the user to hang off, and Unlock is that act. When the keyring has
// nothing — or hands back a password the blob refuses — this falls through to
// the form, which is what the button did before.
void MainWindow::unlock()
{
    const QString id = m_userId;
    if (id.isEmpty()) {
        showUnlockForm();
        return;
    }

    // Nothing is put on screen while the keyring is consulted. On the PWA its
    // own form is what the user is looking at; on Linux and Android the answer
    // comes back within an event loop turn.
    readCredential(m_username, [this, id](const QString &password) {
        if (m_userId != id || !m_userData.isEmpty())
            return;             // unlocked, signed out or deleted in between
        if (password.isEmpty()) {
            showUnlockForm();
            return;
        }
        attemptSignIn(m_username, password, /*interactive=*/false, id,
                      [this, id](bool ok) {
                          // A stored password the blob no longer accepts — it
                          // was changed elsewhere, or the entry belongs to an
                          // older vault. Ask, and the next success replaces it.
                          if (!ok && m_userId == id && m_userData.isEmpty())
                              showUnlockForm();
                      },
                      PasswordSource::Keyring);
    });
}

void MainWindow::showUnlockForm()
{
    m_usernameInput->setText(m_username);
    m_passwordInput->clear();
    m_unlockTargetId = m_userId;
    showSignIn(true, /*focusPassword=*/true);
}

void MainWindow::submitSignIn()
{
    const QString username = m_usernameInput->text().trimmed();
    const QString password = m_passwordInput->text();
    if (username.isEmpty()) {
        notify(tr("Sign in"), tr("Username can not be empty."));
        focusField(m_usernameInput);
        return;
    }
    if (password.isEmpty()) {
        notify(tr("Sign in"), tr("Password can not be empty."));
        focusField(m_passwordInput);
        return;
    }
    attemptSignIn(username, password, /*interactive=*/true, m_unlockTargetId,
                  [this](bool ok) {
                      if (ok)
                          m_unlockTargetId.clear();
                  });
}

bool MainWindow::restoreSession()
{
    const QString raw = Storage::get(Storage::SESSION);
    if (raw.isEmpty())
        return false;

    const QJsonObject o = QJsonDocument::fromJson(raw.toUtf8()).object();
    const QString id = o.value(QStringLiteral("id")).toString();
    const QString username = o.value(QStringLiteral("username")).toString();
    if (id.isEmpty() || username.isEmpty())
        return false;

    // The blob may have been deleted since; a session pointing at nothing is
    // worse than none.
    if (!Core::has(vault(), id)) {
        Storage::remove(Storage::SESSION);
        return false;
    }
    m_username = username;
    m_userId = id;
    m_usernameInput->setText(username);
    // The key is not part of the session, so this run starts locked and the
    // Unlock button is the way back in. A browser would try its credential
    // store here; nothing outside one has an equivalent.
    return true;
}

// ---- vault ------------------------------------------------------------------

QString MainWindow::vault() const
{
    const QString v = Storage::get(Storage::VAULT);
    return v.isEmpty() ? QStringLiteral("[]") : v;
}

bool MainWindow::saveVault(const QString &v)
{
    Storage::set(Storage::VAULT, v);
    return Storage::get(Storage::VAULT) == v;
}

void MainWindow::ingest(const QString &text)
{
    if (text.trimmed().isEmpty())
        return;

    // The merge rule is the core's, not this file's: same id replaces, an
    // identical salt and blob is a no-op, anything else is added.
    const Ingest res = Core::ingest(vault(), text);
    if (!res.error.isEmpty()) {
        setPill(m_userdataStatus, tr("bad paste"), QStringLiteral("err"));
        m_userdataBox->setToolTip(res.error);
        return;
    }
    m_userdataBox->setToolTip(QString());
    saveVault(res.vault);
    render();
    if (res.added || res.replaced) {
        setPill(m_userdataStatus, QStringLiteral("+%1 ~%2").arg(res.added).arg(res.replaced),
                QStringLiteral("ok"));
        QTimer::singleShot(1500, this, &MainWindow::render);
    }
}

void MainWindow::deleteUser()
{
    if (m_userId.isEmpty())
        return;
    confirm(tr("Delete user"),
            tr("Delete this user? The encrypted blob is removed from this device and cannot "
               "be recovered without a copy."),
            [this](bool yes) {
                if (!yes || m_userId.isEmpty())
                    return;
                // Before signOutLocal(), which is what forgets the username.
                forgetCredential(m_username);
                saveVault(Core::remove(vault(), m_userId));
                signOutLocal();
                render();
            });
}

void MainWindow::deleteAll()
{
    const int count = Core::count(vault());
    if (!count)
        return;
    confirm(tr("Delete all users"),
            count == 1 ? tr("Delete all 1 user? Every encrypted blob on this device is removed "
                            "and cannot be recovered without a copy.")
                       : tr("Delete all %1 users? Every encrypted blob on this device is removed "
                            "and cannot be recovered without a copy.").arg(count),
            [this](bool yes) {
                if (!yes)
                    return;
                // Only the signed-in user's saved password can be cleared
                // here, because only their name is known: the vault stores
                // every username encrypted inside its own blob, so there is
                // nothing to enumerate. A second user's entry is left behind
                // with no blob left to open — see README.md.
                if (!m_username.isEmpty())
                    forgetCredential(m_username);
                saveVault(QStringLiteral("[]"));
                signOutLocal();
                render();
            });
}

// ---- clipboard --------------------------------------------------------------

void MainWindow::readClipboard(std::function<void(const QString &)> then)
{
    Clip::read([this, then](const QString &text, bool ok) {
        if (!ok) {
            notify(tr("Paste"), tr("Clipboard read failed. The browser may have refused "
                                   "permission; paste into the box by hand instead."));
            return;
        }
        if (text.isEmpty()) {
            notify(tr("Paste"), tr("The clipboard is empty."));
            return;
        }
        then(text);
    });
}

void MainWindow::copyToClipboard(const QString &text, QLabel *pill)
{
    Clip::write(text);
    setPill(pill, tr("copied"), QStringLiteral("ok"));
    QTimer::singleShot(1200, this, &MainWindow::render);
}
