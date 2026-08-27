// The clipboard, asynchronously.
//
// prototype-minimal reads the clipboard with navigator.clipboard.readText(),
// which is a promise. Qt's QClipboard::text() is synchronous, and on
// WebAssembly it can only return what a browser paste event already handed
// Qt — an app-initiated read comes back empty, which is what the port did
// before this file existed. So the read is a callback on every target: the
// browser gets the real Clipboard API, and Linux and Android answer
// immediately from QClipboard.

#pragma once

#include <QString>

#include <functional>

namespace Clip {

void write(const QString &text);
void read(std::function<void(const QString &text, bool ok)> then);

} // namespace Clip
