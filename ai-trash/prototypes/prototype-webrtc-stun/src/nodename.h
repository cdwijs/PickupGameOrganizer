#pragma once

// Node names are the address of a node: easy to read aloud and type on a
// phone ("brave-otter-417"), and each one maps to an ntfy topic.

#include <QString>

namespace nodename {

// adjective-animal-NNN, about 9 million combinations.
QString generate();

// Lower-cases and trims what the user typed.
QString normalize(const QString &name);

// Letters, digits and dashes, 3 to 40 characters.
bool isValid(const QString &name);

// The ntfy topic this node listens on.
QString topic(const QString &name);

} // namespace nodename
