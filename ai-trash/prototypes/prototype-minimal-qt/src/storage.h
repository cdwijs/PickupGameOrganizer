// Where the vault, the session and the debug-fold state live.
//
// prototype-minimal keeps three localStorage keys. In the browser this keeps
// the same three, under this prototype's own namespace so a Qt PWA and
// prototype-minimal can be open in one browser without sharing a vault (the
// blob format is identical, so a vault still copy-pastes across). Outside the
// browser there is no localStorage and QSettings is the equivalent: an INI
// file per user on Linux, SharedPreferences-backed on Android.

#pragma once

#include <QString>

namespace Storage {

QString get(const QString &key);
void set(const QString &key, const QString &value);
void remove(const QString &key);

inline const QString VAULT = QStringLiteral("prototype-minimal-qt:vault:v1");
inline const QString SESSION = QStringLiteral("prototype-minimal-qt:session:v1");
inline const QString DEBUG = QStringLiteral("prototype-minimal-qt:debug:v1");

} // namespace Storage
