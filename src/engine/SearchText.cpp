#include "engine/SearchText.h"

#include <algorithm>
#include <vector>

namespace pdf {
namespace {

struct NormalizedText {
    QString text;
    std::vector<int> charToBox;
};

bool isIgnoredJoiner(char32_t cp) {
    return cp == 0x200C || cp == 0x200D || cp == 0x0640;
}

char32_t normalizePersianCharacter(char32_t cp) {
    switch (cp) {
        case 0x0643: return 0x06A9; // Arabic kaf -> Persian keheh
        case 0x064A: // Arabic yeh
        case 0x0649: return 0x06CC; // alef maksura -> Persian yeh
        default:
            if (cp >= 0x0660 && cp <= 0x0669) {
                return U'0' + (cp - 0x0660);
            }
            if (cp >= 0x06F0 && cp <= 0x06F9) {
                return U'0' + (cp - 0x06F0);
            }
            return cp;
    }
}

void appendCodePoint(QString& output, char32_t cp) {
    if (cp <= 0xFFFF) {
        output.append(QChar(static_cast<ushort>(cp)));
        return;
    }
    cp -= 0x10000;
    output.append(QChar(static_cast<ushort>(0xD800 + ((cp >> 10) & 0x3FF))));
    output.append(QChar(static_cast<ushort>(0xDC00 + (cp & 0x3FF))));
}

template <typename CodePointAt>
NormalizedText normalizeText(qsizetype length, CodePointAt codePointAt, bool mapToBoxes) {
    NormalizedText result;
    result.text.reserve(length);
    if (mapToBoxes) {
        result.charToBox.reserve(static_cast<size_t>(length));
    }

    bool previousWasSpace = false;
    for (qsizetype i = 0; i < length;) {
        const char32_t cp = codePointAt(i);
        const qsizetype units = (cp > 0xFFFF) ? 2 : 1;
        if (isIgnoredJoiner(cp)) {
            i += units;
            continue;
        }

        const QString codePoint = QString::fromUcs4(&cp, 1);
        if (codePoint.at(0).isSpace()) {
            if (!previousWasSpace && !result.text.isEmpty()) {
                result.text.append(QChar(u' '));
                if (mapToBoxes) {
                    result.charToBox.push_back(static_cast<int>(i));
                }
            }
            previousWasSpace = true;
            i += units;
            continue;
        }

        previousWasSpace = false;
        const char32_t normalized = normalizePersianCharacter(cp);
        const qsizetype start = result.text.size();
        appendCodePoint(result.text, normalized);
        if (mapToBoxes) {
            for (qsizetype j = start; j < result.text.size(); ++j) {
                result.charToBox.push_back(static_cast<int>(i));
            }
        }
        i += units;
    }

    while (result.text.endsWith(QChar(u' '))) {
        result.text.chop(1);
        if (mapToBoxes) {
            result.charToBox.pop_back();
        }
    }
    return result;
}

} // namespace

std::vector<SearchMatch> findTextInPage(
    const PageText& pageText,
    const QString& query,
    bool matchCase,
    int maxMatches,
    bool* truncated) {
    if (truncated) {
        *truncated = false;
    }
    std::vector<SearchMatch> matches;
    if (query.isEmpty() || pageText.chars.empty() || maxMatches < 0) {
        return matches;
    }

    const auto normalizedQuery = normalizeText(
        query.size(),
        [&query](qsizetype i) -> char32_t {
            const QChar first = query.at(i);
            if (first.isHighSurrogate() && i + 1 < query.size() && query.at(i + 1).isLowSurrogate()) {
                return QChar::surrogateToUcs4(first, query.at(i + 1));
            }
            return first.unicode();
        },
        false);
    if (normalizedQuery.text.isEmpty()) {
        return matches;
    }

    NormalizedText normalizedPage;
    normalizedPage.text.reserve(static_cast<qsizetype>(pageText.chars.size() * 2));
    normalizedPage.charToBox.reserve(pageText.chars.size() * 2);
    bool previousWasSpace = false;
    for (size_t i = 0; i < pageText.chars.size(); ++i) {
        const char32_t cp = pageText.chars[i].unicode;
        if (cp == 0 || isIgnoredJoiner(cp)) {
            continue;
        }
        const QString codePoint = QString::fromUcs4(&cp, 1);
        if (codePoint.at(0).isSpace()) {
            if (!previousWasSpace && !normalizedPage.text.isEmpty()) {
                normalizedPage.text.append(QChar(u' '));
                normalizedPage.charToBox.push_back(static_cast<int>(i));
            }
            previousWasSpace = true;
            continue;
        }

        previousWasSpace = false;
        const qsizetype start = normalizedPage.text.size();
        appendCodePoint(normalizedPage.text, normalizePersianCharacter(cp));
        for (qsizetype j = start; j < normalizedPage.text.size(); ++j) {
            normalizedPage.charToBox.push_back(static_cast<int>(i));
        }
    }
    while (normalizedPage.text.endsWith(QChar(u' '))) {
        normalizedPage.text.chop(1);
        normalizedPage.charToBox.pop_back();
    }

    const Qt::CaseSensitivity cs = matchCase ? Qt::CaseSensitive : Qt::CaseInsensitive;
    qsizetype pos = 0;
    while (pos < normalizedPage.text.size()) {
        const qsizetype found = normalizedPage.text.indexOf(normalizedQuery.text, pos, cs);
        if (found < 0) {
            break;
        }
        const qsizetype matchEnd = found + normalizedQuery.text.size() - 1;
        const int startBox = normalizedPage.charToBox[static_cast<size_t>(found)];
        const int endBox = normalizedPage.charToBox[static_cast<size_t>(matchEnd)];

        if (static_cast<int>(matches.size()) >= maxMatches) {
            if (truncated) {
                *truncated = true;
            }
            break;
        }

        SearchMatch match;
        match.pageIndex = pageText.pageIndex;
        match.charStartIndex = startBox;
        match.charCount = endBox - startBox + 1;
        match.rects = pageText.getSelectionRects(startBox, endBox);
        matches.push_back(std::move(match));
        pos = found + std::max<qsizetype>(1, normalizedQuery.text.size());
    }

    return matches;
}

} // namespace pdf
