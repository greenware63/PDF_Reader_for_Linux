#pragma once

#include "engine/AnnotationTypes.h"

#include <QSizeF>
#include <QString>

#include <vector>

namespace ui {

/// Space kept between a text box's edge and its text, in points.
constexpr double kFreeTextPadding = 2.0;

/// A text box's text laid out as filled glyph outlines (see AnnotationItem::textOutline).
struct FreeTextLayoutResult {
    /// The outline in PDF points relative to the top-left corner of the text block: x grows to the right, y is 0 on
    /// the top edge of the first line and negative below it (the PDF y axis points up). Empty when only measuring.
    std::vector<pdf::AnnotationPathElement> outline;
    /// Width of the widest line and height of all lines, in points.
    QSizeF size;
    int lineCount{0};
    /// Distinct characters that no installed font can draw as an outline (no glyph at all, or a bitmap glyph such as a
    /// colour emoji), in order of appearance. Empty when everything can be drawn.
    QString undrawable;
    /// The outline would need more elements than pdf::kMaxTextOutlineElements; it is incomplete then.
    bool tooLarge{false};
};

/// Lays `text` out in a sans-serif font of `fontSizePt` points, the way a text editor would: a line break starts a new
/// line (blank lines are kept), a line wraps at word boundaries (anywhere inside a word that is longer than the line)
/// once it is `wrapWidthPt` wide, and wrapWidthPt <= 0 means no wrapping. Right-to-left paragraphs (Persian, Arabic,
/// Hebrew) are shaped and ordered by Qt and sit flush right against the wrap width, every other paragraph flush left;
/// without wrapping the lines are aligned against the widest one. Control characters are left out, a tab is four
/// spaces. With wantOutline false only the size is worked out (cheaper, no glyph outlines, no drawability check).
FreeTextLayoutResult layoutFreeText(const QString& text, double fontSizePt, double wrapWidthPt, bool wantOutline = true);

/// Moves every point of an outline by (dx, dy).
void translateOutline(std::vector<pdf::AnnotationPathElement>& outline, double dx, double dy);

} // namespace ui
