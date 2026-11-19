#pragma once

#include <QChar>
#include <QString>
#include <algorithm>

#include "control/controlobject.h"

// ShonDJ stealth mode: hides song names and cover art so people looking at
// the screen can't read your set. [Shon],stealth turns it on,
// holding [Shon],stealth_peek shows everything while held.

inline const ConfigKey& shonStealthKey() {
    static const ConfigKey key(QStringLiteral("[Shon]"), QStringLiteral("stealth"));
    return key;
}

inline const ConfigKey& shonStealthPeekKey() {
    static const ConfigKey key(QStringLiteral("[Shon]"), QStringLiteral("stealth_peek"));
    return key;
}

inline bool shonStealthActive() {
    // The controls may not exist (tests, or before the library is set up).
    const ControlObject* pStealth =
            ControlObject::getControl(shonStealthKey(), ControlFlag::NoAssertIfMissing);
    if (!pStealth || pStealth->get() <= 0.0) {
        return false;
    }
    const ControlObject* pPeek =
            ControlObject::getControl(shonStealthPeekKey(), ControlFlag::NoAssertIfMissing);
    return !pPeek || pPeek->get() <= 0.0;
}

/// What a hidden title shows. Artist, album and the rest just go blank.
inline QString shonStealthTitle() {
    return QStringLiteral("Hidden track");
}

/// Dots instead of the real text, roughly the same length.
inline QString shonStealthMask(const QString& text) {
    if (text.isEmpty()) {
        return text;
    }
    const auto length = std::clamp(static_cast<int>(text.size()), 4, 12);
    return QString(length, QChar(0x2022));
}
