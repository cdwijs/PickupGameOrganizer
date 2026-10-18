#include "nodename.h"

#include <QRandomGenerator>
#include <QRegularExpression>

namespace nodename {

namespace {

const char *const kAdjectives[] = {
    "able",   "amber",  "bold",   "brave",  "brisk",  "calm",   "clever", "cosmic", "crisp",  "curly",
    "daring", "dusty",  "eager",  "early",  "fancy",  "fast",   "fierce", "fluffy", "frosty", "funny",
    "gentle", "giant",  "glad",   "golden", "grand",  "happy",  "hardy",  "honest", "humble", "icy",
    "jolly",  "keen",   "kind",   "lively", "lucky",  "lunar",  "mellow", "merry",  "mighty", "misty",
    "modest", "noble",  "odd",    "proud",  "quick",  "quiet",  "rapid",  "rocky",  "rosy",   "royal",
    "rusty",  "shiny",  "silent", "silver", "simple", "sleepy", "smart",  "snowy",  "solar",  "speedy",
    "spicy",  "steady", "stormy", "sunny",  "swift",  "tidy",   "tiny",   "tough",  "trusty", "vivid",
    "warm",   "wild",   "windy",  "wise",   "witty",  "young",  "zany",   "zesty",  "mossy",  "sandy",
};

const char *const kAnimals[] = {
    "badger", "bat",    "bear",   "beaver", "bison",  "camel",  "cat",    "cobra",  "crab",   "crane",
    "crow",   "deer",   "dingo",  "dog",    "dove",   "duck",   "eagle",  "eel",    "falcon", "ferret",
    "finch",  "fox",    "frog",   "gecko",  "goat",   "goose",  "hare",   "hawk",   "heron",  "hippo",
    "horse",  "ibis",   "jackal", "koala",  "lemur",  "lion",   "llama",  "lynx",   "mole",   "moose",
    "mouse",  "newt",   "otter",  "owl",    "ox",     "panda",  "parrot", "pig",    "pony",   "puffin",
    "quail",  "rabbit", "raven",  "seal",   "shark",  "sheep",  "skunk",  "sloth",  "snail",  "squid",
    "stork",  "swan",   "tapir",  "tiger",  "toad",   "trout",  "turtle", "viper",  "walrus", "wasp",
    "whale",  "wolf",   "wombat", "yak",    "zebra",  "robin",  "gull",   "mink",   "orca",   "kiwi",
};

template<typename T, size_t N>
QString pick(const T (&words)[N])
{
    return QString::fromLatin1(words[QRandomGenerator::system()->bounded(int(N))]);
}

} // namespace

QString generate()
{
    return QStringLiteral("%1-%2-%3")
        .arg(pick(kAdjectives), pick(kAnimals))
        .arg(QRandomGenerator::system()->bounded(100, 1000));
}

QString normalize(const QString &name)
{
    return name.trimmed().toLower();
}

bool isValid(const QString &name)
{
    static const QRegularExpression re(QStringLiteral("^[a-z0-9][a-z0-9-]{1,38}[a-z0-9]$"));
    return re.match(name).hasMatch();
}

QString topic(const QString &name)
{
    return QStringLiteral("pgo-webrtc-") + name; // ntfy allows [-_A-Za-z0-9]{1,64}
}

} // namespace nodename
