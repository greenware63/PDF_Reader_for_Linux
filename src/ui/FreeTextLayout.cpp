#include "FreeTextLayout.h"

#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QList>
#include <QPainterPath>
#include <QPointF>
#include <QRawFont>
#include <QStringList>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>

#include <algorithm>
#include <cmath>
#include <set>

namespace ui {
namespace {

// Text is laid out at 1000 pixels per em and scaled down to the requested size: hinting and rounding then have no
// visible effect and every coordinate is an exact fraction of the font size.
constexpr int kEmPixels = 1000;
constexpr double kUnlimitedWidthPx = 1.0e6;
constexpr int kMaxLines = 20000;      // guards against a pathological wrap width producing one line per character
constexpr int kMaxUndrawableShown = 16;

QFont makeFont() {
    QFont font;
    // Helvetica first so a box looks like the PDF default font where it exists; Qt falls back per script for
    // everything the first match lacks (Arabic script, CJK, ...).
    font.setFamilies({QStringLiteral("Helvetica"), QStringLiteral("Arial"), QStringLiteral("Liberation Sans"),
                      QStringLiteral("DejaVu Sans")});
    font.setStyleHint(QFont::SansSerif);
    font.setPixelSize(kEmPixels);
    font.setHintingPreference(QFont::PreferNoHinting);
    return font;
}

// Line breaks become '\n', a tab becomes four spaces and other control characters (which have no glyph) are dropped.
QString normalizedText(const QString& text) {
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t u = text.at(i).unicode();
        if (u == u'\r') {
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\n')) {
                ++i;
            }
            out += QLatin1Char('\n');
        } else if (u == u'\n' || u == 0x2028 || u == 0x2029) {
            out += QLatin1Char('\n');
        } else if (u == u'\t') {
            out += QStringLiteral("    ");
        } else if (u < 0x20 || (u >= 0x7F && u < 0xA0)) {
            // dropped
        } else {
            out += text.at(i);
        }
    }
    return out;
}

// Characters that are expected to leave no ink of their own: spaces, controls, joiners and direction marks, and
// combining marks (which attach to a base character).
bool leavesNoInk(char32_t cp) {
    switch (QChar::category(cp)) {
        case QChar::Separator_Space:
        case QChar::Separator_Line:
        case QChar::Separator_Paragraph:
        case QChar::Other_Control:
        case QChar::Other_Format:
        case QChar::Mark_NonSpacing:
        case QChar::Mark_SpacingCombining:
        case QChar::Mark_Enclosing:
            return true;
        default:
            return false;
    }
}

// Whether some glyph with an outline comes out when the character is laid out on its own. A character with no glyph
// in any installed font comes out as the font's "missing" box (glyph 0), and a bitmap glyph (a colour emoji) has an
// index but no outline: neither can be put into a text box as vector shapes.
bool canDrawAsOutline(const QFont& font, char32_t cp) {
    if (QChar::category(cp) == QChar::Other_PrivateUse) {
        return false;
    }
    const QString s = QString::fromUcs4(&cp, 1);
    QTextLayout layout(s, font);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    if (line.isValid()) {
        line.setLineWidth(kUnlimitedWidthPx);
    }
    layout.endLayout();
    if (!line.isValid()) {
        return false;
    }
    for (const QGlyphRun& run : line.glyphRuns()) {
        const QRawFont raw = run.rawFont();
        if (!raw.isValid() || !raw.supportsCharacter(cp)) {
            continue;
        }
        for (const quint32 glyph : run.glyphIndexes()) {
            if (glyph != 0 && !raw.pathForGlyph(glyph).isEmpty()) {
                return true;
            }
        }
    }
    return false;
}

QString findUndrawable(const QFont& font, const QString& text) {
    QString bad;
    std::set<char32_t> seen;
    for (const uint code : text.toUcs4()) {
        const char32_t cp = code;
        if (cp == U'\n' || leavesNoInk(cp) || !seen.insert(cp).second) {
            continue;
        }
        if (!canDrawAsOutline(font, cp)) {
            bad += QString::fromUcs4(&cp, 1);
            if (bad.size() >= kMaxUndrawableShown) {
                break;
            }
        }
    }
    return bad;
}

// Appends the outline of one glyph, drawn with its origin at `origin` (layout pixels, y down), to `out` in points
// with y up. QPainterPath encodes a cubic curve as a CurveTo element followed by two CurveToData elements, which is
// exactly three consecutive CurveTo elements in the wire format.
void appendGlyph(const QPainterPath& path, const QPointF& origin, double scale,
                 std::vector<pdf::AnnotationPathElement>& out) {
    for (int i = 0; i < path.elementCount(); ++i) {
        const QPainterPath::Element e = path.elementAt(i);
        pdf::AnnotationPathElement p;
        p.x = (origin.x() + e.x) * scale;
        p.y = -(origin.y() + e.y) * scale;
        switch (e.type) {
            case QPainterPath::MoveToElement:
                p.kind = pdf::AnnotationPathElement::Kind::MoveTo;
                break;
            case QPainterPath::LineToElement:
                p.kind = pdf::AnnotationPathElement::Kind::LineTo;
                break;
            case QPainterPath::CurveToElement:
            case QPainterPath::CurveToDataElement:
                p.kind = pdf::AnnotationPathElement::Kind::CurveTo;
                break;
        }
        out.push_back(p);
    }
}

enum class Collect { Nothing, Size, Outline };

// One pass over all paragraphs at a given line width (layout pixels). Returns the natural width of the widest line in
// layout pixels. `collect` says what is stored in `result` besides: the size and line count, or the outline as well.
double layoutPass(const QStringList& paragraphs, const QFont& font, double widthPx, double scale, Collect collect,
                  FreeTextLayoutResult& result) {
    double widest = 0.0;
    double y = 0.0;
    int totalLines = 0;
    const double emptyLineHeight = QFontMetricsF(font).lineSpacing();

    for (const QString& paragraph : paragraphs) {
        QTextLayout layout(paragraph, font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        option.setTextDirection(Qt::LayoutDirectionAuto);
        if (paragraph.isRightToLeft()) {
            option.setAlignment(Qt::AlignRight);
        }
        layout.setTextOption(option);

        std::vector<QTextLine> lines;
        layout.beginLayout();
        for (;;) {
            QTextLine line = layout.createLine();
            if (!line.isValid()) {
                break;
            }
            line.setLineWidth(widthPx);
            line.setPosition(QPointF(0.0, y));
            y += (paragraph.isEmpty() && line.height() <= 0.0) ? emptyLineHeight : line.height();
            lines.push_back(line);
            if (++totalLines > kMaxLines) {
                result.tooLarge = true;
                break;
            }
        }
        layout.endLayout();

        for (const QTextLine& line : lines) {
            widest = std::max(widest, line.naturalTextWidth());
            if (collect == Collect::Nothing) {
                continue;
            }
            ++result.lineCount;
            if (collect == Collect::Size) {
                continue;
            }
            for (const QGlyphRun& run : line.glyphRuns()) {
                const QRawFont raw = run.rawFont();
                const QList<quint32> glyphs = run.glyphIndexes();
                const QList<QPointF> positions = run.positions();
                for (qsizetype i = 0; i < glyphs.size() && i < positions.size(); ++i) {
                    const QPainterPath path = raw.pathForGlyph(glyphs[i]);
                    if (result.outline.size() + static_cast<size_t>(path.elementCount()) > pdf::kMaxTextOutlineElements) {
                        result.tooLarge = true;
                        return widest;
                    }
                    appendGlyph(path, positions[i], scale, result.outline);
                }
            }
        }
        if (result.tooLarge) {
            break;
        }
    }
    if (collect != Collect::Nothing) {
        result.size = QSizeF(widest * scale, y * scale);
    }
    return widest;
}

} // namespace

FreeTextLayoutResult layoutFreeText(const QString& text, double fontSizePt, double wrapWidthPt, bool wantOutline) {
    FreeTextLayoutResult result;
    if (!std::isfinite(fontSizePt) || fontSizePt <= 0.0 || !std::isfinite(wrapWidthPt)) {
        return result;
    }
    const QString normalized = normalizedText(text);
    const QStringList paragraphs = normalized.split(QLatin1Char('\n'));
    const QFont font = makeFont();
    const double scale = fontSizePt / kEmPixels;

    double widthPx = wrapWidthPt > 0.0 ? wrapWidthPt / scale : kUnlimitedWidthPx;
    if (wrapWidthPt <= 0.0) {
        // Without wrapping the lines are aligned against the widest one, so find it first.
        FreeTextLayoutResult unused;
        widthPx = layoutPass(paragraphs, font, kUnlimitedWidthPx, scale, Collect::Nothing, unused) + 1.0;
    }
    layoutPass(paragraphs, font, widthPx, scale, wantOutline ? Collect::Outline : Collect::Size, result);
    if (wantOutline) {
        result.undrawable = findUndrawable(font, normalized);
    }
    return result;
}

void translateOutline(std::vector<pdf::AnnotationPathElement>& outline, double dx, double dy) {
    for (auto& e : outline) {
        e.x += dx;
        e.y += dy;
    }
}

} // namespace ui
