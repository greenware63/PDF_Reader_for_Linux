#include "ViewerAnnotationBuilder.h"
#include "FreeTextLayout.h"

#include <QUuid>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ui {

namespace {

constexpr double sanitizeBorderWidth(double borderWidth) {
    return (borderWidth > 0.0 && borderWidth <= 100.0) ? borderWidth : 2.0;
}

} // namespace

ViewerAnnotationBuilder::BuiltAnnotation ViewerAnnotationBuilder::buildText(
    int pageIndex, double userX, double userY, const QString& contents, uint32_t colorArgb) {
    if (colorArgb == 0) {
        colorArgb = 0xFFFFD700; // Gold / Yellow
    }

    constexpr double kIconSize = 24.0;
    pdf::AnnotationItem note;
    note.name = QStringLiteral("pdfr-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    note.subtype = pdf::AnnotationSubtype::Text;
    note.pageIndex = pageIndex;
    note.left = userX;
    note.bottom = userY - kIconSize;
    note.right = userX + kIconSize;
    note.top = userY;
    note.contents = contents;
    note.colorArgb = colorArgb;
    note.hasColor = true;
    note.flags = 4; // FPDF_ANNOT_FLAG_PRINT

    return {std::move(note), BuildError::None, {}};
}

ViewerAnnotationBuilder::BuiltAnnotation ViewerAnnotationBuilder::buildSquare(
    int pageIndex, const QRectF& pagePointsRect, uint32_t colorArgb, double borderWidth) {
    const QRectF normalized = pagePointsRect.normalized();
    if (normalized.isEmpty()) {
        return {{}, BuildError::Invalid, {}};
    }
    const double bw = sanitizeBorderWidth(borderWidth);

    if (colorArgb == 0) {
        colorArgb = 0xFFFF0000; // Red
    }

    pdf::AnnotationItem item;
    item.name = QStringLiteral("pdfr-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.subtype = pdf::AnnotationSubtype::Square;
    item.pageIndex = pageIndex;
    item.left = normalized.left();
    item.bottom = normalized.top();
    item.right = normalized.right();
    item.top = normalized.bottom();
    item.borderWidth = bw;
    item.colorArgb = colorArgb;
    item.hasColor = true;
    item.flags = 4; // FPDF_ANNOT_FLAG_PRINT

    return {std::move(item), BuildError::None, {}};
}

ViewerAnnotationBuilder::BuiltAnnotation ViewerAnnotationBuilder::buildCircle(
    int pageIndex, const QRectF& pagePointsRect, uint32_t colorArgb, double borderWidth) {
    const QRectF normalized = pagePointsRect.normalized();
    if (normalized.isEmpty()) {
        return {{}, BuildError::Invalid, {}};
    }
    const double bw = sanitizeBorderWidth(borderWidth);

    if (colorArgb == 0) {
        colorArgb = 0xFF0000FF; // Blue
    }

    pdf::AnnotationItem item;
    item.name = QStringLiteral("pdfr-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.subtype = pdf::AnnotationSubtype::Circle;
    item.pageIndex = pageIndex;
    item.left = normalized.left();
    item.bottom = normalized.top();
    item.right = normalized.right();
    item.top = normalized.bottom();
    item.borderWidth = bw;
    item.colorArgb = colorArgb;
    item.hasColor = true;
    item.flags = 4; // FPDF_ANNOT_FLAG_PRINT

    return {std::move(item), BuildError::None, {}};
}

ViewerAnnotationBuilder::BuiltAnnotation ViewerAnnotationBuilder::buildInk(
    int pageIndex, const std::vector<std::vector<pdf::AnnotationPoint>>& strokes,
    uint32_t colorArgb, double borderWidth) {
    if (strokes.empty()) {
        return {{}, BuildError::Invalid, {}};
    }
    const double bw = sanitizeBorderWidth(borderWidth);

    if (colorArgb == 0) {
        colorArgb = 0xFF000000; // Black default for ink
    }

    pdf::AnnotationItem item;
    item.name = QStringLiteral("pdfr-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.subtype = pdf::AnnotationSubtype::Ink;
    item.pageIndex = pageIndex;
    item.inkStrokes = strokes;
    item.borderWidth = bw;
    item.colorArgb = colorArgb;
    item.hasColor = true;
    item.flags = 4; // FPDF_ANNOT_FLAG_PRINT

    return {std::move(item), BuildError::None, {}};
}

ViewerAnnotationBuilder::TextBoxFit ViewerAnnotationBuilder::fitTextBoxOutline(
    pdf::AnnotationItem& item, QString* undrawable) {
    const double fontSize = (item.fontSize > 0.0 && item.fontSize < 144.0) ? item.fontSize : 12.0;
    const double left = std::min(item.left, item.right);
    const double right = std::max(item.left, item.right);
    const double lower = std::min(item.bottom, item.top);
    const double upper = std::max(item.bottom, item.top);
    // The text is laid out here, as glyph outlines: that is what lets a text box show any script (Persian, CJK, ...),
    // wrap inside its width and keep its line breaks. The worker's own font is Latin-1 and puts everything on one line.
    const double innerWidth = right - left - 2.0 * kFreeTextPadding;
    FreeTextLayoutResult laid = layoutFreeText(item.contents, fontSize, innerWidth > 0.0 ? innerWidth : 1.0);
    if (!laid.undrawable.isEmpty()) {
        if (undrawable) {
            *undrawable = laid.undrawable;
        }
        return TextBoxFit::Undrawable;
    }
    if (laid.tooLarge) {
        return TextBoxFit::TooLarge;
    }
    if (laid.outline.empty()) {
        return TextBoxFit::NothingToDraw;
    }
    translateOutline(laid.outline, left + kFreeTextPadding, upper - kFreeTextPadding);
    item.left = left;
    item.right = right;
    item.top = upper;
    item.bottom = std::min(lower, upper - (laid.size.height() + 2.0 * kFreeTextPadding));
    item.textOutline = std::move(laid.outline);
    return TextBoxFit::Ok;
}

ViewerAnnotationBuilder::BuiltAnnotation ViewerAnnotationBuilder::buildFreeText(
    int pageIndex, const QRectF& pagePointsRect, const QString& contents,
    uint32_t colorArgb, double fontSize) {
    const QRectF normalized = pagePointsRect.normalized();
    if (normalized.isEmpty() || contents.trimmed().isEmpty()) {
        return {{}, BuildError::Invalid, {}};
    }

    if (colorArgb == 0) {
        colorArgb = 0xFF000000; // Black default text color
    }

    // In PDF user space QRectF::top() is the lower edge and bottom() the upper edge.
    pdf::AnnotationItem item;
    item.name = QStringLiteral("pdfr-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.subtype = pdf::AnnotationSubtype::FreeText;
    item.pageIndex = pageIndex;
    item.left = normalized.left();
    item.bottom = normalized.top();
    item.right = normalized.right();
    item.top = normalized.bottom();
    item.fontSize = (fontSize > 0.0 && fontSize <= 144.0) ? fontSize : 12.0;
    item.contents = contents;
    item.colorArgb = colorArgb;
    item.hasColor = true;
    item.flags = 4; // FPDF_ANNOT_FLAG_PRINT

    QString undrawable;
    switch (fitTextBoxOutline(item, &undrawable)) {
        case TextBoxFit::Ok:
            break;
        case TextBoxFit::Undrawable:
            return {{}, BuildError::TextUndrawable, undrawable};
        case TextBoxFit::TooLarge:
            return {{}, BuildError::TextTooLarge, {}};
        case TextBoxFit::NothingToDraw:
            return {{}, BuildError::TextNothingToDraw, {}};
    }

    return {std::move(item), BuildError::None, {}};
}

ViewerAnnotationBuilder::BuiltAnnotation ViewerAnnotationBuilder::buildStamp(
    int pageIndex, const QRectF& pagePointsRect, const QString& stampText, uint32_t colorArgb) {
    const QRectF normalized = pagePointsRect.normalized();
    if (normalized.isEmpty() || stampText.trimmed().isEmpty()) {
        return {{}, BuildError::Invalid, {}};
    }
    const QString text = stampText.trimmed();
    if (text.size() > pdf::kMaxStampTextCharacters) {
        return {{}, BuildError::StampTextTooLong, {}};
    }

    if (colorArgb == 0) {
        colorArgb = 0xFFCC0000; // Red default stamp color
    }

    pdf::AnnotationItem item;
    item.name = QStringLiteral("pdfr-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.subtype = pdf::AnnotationSubtype::Stamp;
    item.pageIndex = pageIndex;
    item.left = normalized.left();
    item.bottom = normalized.top();
    item.right = normalized.right();
    item.top = normalized.bottom();
    item.contents = text;
    item.colorArgb = colorArgb;
    item.hasColor = true;
    item.flags = 4; // FPDF_ANNOT_FLAG_PRINT
    item.borderWidth = 2.0;

    // Lay out the stamp text to obtain vector outlines
    const double innerW = (item.right - item.left) - 8.0;
    const double innerH = (item.top - item.bottom) - 8.0;
    const double targetFontSize = std::clamp(innerH * 0.5, 8.0, 72.0);
    FreeTextLayoutResult laid = layoutFreeText(text, targetFontSize, innerW > 0.0 ? innerW : 100.0);
    if (laid.outline.empty() || laid.tooLarge || !laid.undrawable.isEmpty() ||
        laid.outline.size() > pdf::kMaxStampVectorElements) {
        return {{}, BuildError::StampLayoutFailed, {}};
    }
    const double midX = item.left + 4.0 + (innerW - laid.size.width()) / 2.0;
    const double midY = item.top - 4.0 - (innerH - laid.size.height()) / 2.0;
    translateOutline(laid.outline, midX > item.left ? midX : item.left + 4.0,
                     midY < item.top ? midY : item.top - 4.0);
    item.textOutline = std::move(laid.outline);

    return {std::move(item), BuildError::None, {}};
}

ViewerAnnotationBuilder::BuiltAnnotation ViewerAnnotationBuilder::buildFileAttachment(
    int pageIndex, double userX, double userY, const QString& fileName,
    const QByteArray& data, uint32_t colorArgb, const QSizeF& pageSizePoints) {
    if (!pdf::isValidAttachmentFileName(fileName)) {
        return {{}, BuildError::AttachmentBadFileName, {}};
    }
    if (data.isEmpty() || data.size() > pdf::kMaxAnnotationAttachmentBytes) {
        return {{}, BuildError::AttachmentBadPayload, {}};
    }

    if (colorArgb == 0) {
        colorArgb = 0xFF336699;
    }

    const QSizeF iconSize(24.0, 24.0);
    const QRectF rect = placeOnPage(QPointF(userX, userY), iconSize, pageSizePoints);
    if (rect.isEmpty() || rect.width() < 0.5 || rect.height() < 0.5) {
        return {{}, BuildError::Invalid, {}};
    }

    pdf::AnnotationItem item;
    item.name = QStringLiteral("pdfr-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.subtype = pdf::AnnotationSubtype::FileAttachment;
    item.pageIndex = pageIndex;
    item.left = rect.left();
    item.bottom = rect.top();
    item.right = rect.right();
    item.top = rect.bottom();
    item.attachmentFileName = fileName;
    item.binaryPayload = data;
    item.contents = fileName;
    item.colorArgb = colorArgb;
    item.hasColor = true;
    item.flags = pdf::AnnotationFlag::Print | pdf::AnnotationFlag::NoZoom | pdf::AnnotationFlag::NoRotate;

    return {std::move(item), BuildError::None, {}};
}

ViewerAnnotationBuilder::BuiltAnnotation ViewerAnnotationBuilder::buildMarkup(
    const pdf::PageText& pt, int pageIndex, int startChar, int endChar,
    pdf::AnnotationSubtype subtype, uint32_t colorArgb) {
    if (colorArgb == 0) {
        switch (subtype) {
            case pdf::AnnotationSubtype::Highlight: colorArgb = 0x80FFFF00; break;
            case pdf::AnnotationSubtype::Underline: colorArgb = 0xFF00AA00; break;
            case pdf::AnnotationSubtype::StrikeOut: colorArgb = 0xFFFF0000; break;
            case pdf::AnnotationSubtype::Squiggly:  colorArgb = 0xFFFF0000; break;
            default: colorArgb = 0x80FFFF00; break;
        }
    }

    pdf::AnnotationItem hl;
    // The UI owns the annotation id (RFC section 3.1): undo, redo and crash recovery address it by /NM.
    hl.name = QStringLiteral("pdfr-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    hl.subtype = subtype;
    hl.pageIndex = pageIndex;
    hl.colorArgb = colorArgb;
    hl.hasColor = true;
    hl.flags = 4; // FPDF_ANNOT_FLAG_PRINT
    hl.contents = QString::fromStdU16String(pt.getSelectedText(startChar, endChar));

    double minX = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double minY = std::numeric_limits<double>::max();
    double maxY = std::numeric_limits<double>::lowest();
    const int first = std::max(0, startChar);
    const int last = std::min(endChar, static_cast<int>(pt.charCount()) - 1);
    for (int charIndex = first; charIndex <= last; ++charIndex) {
        const auto& b = pt.chars[static_cast<size_t>(charIndex)];
        if (!b.hasUserSpaceBox) {
            continue;
        }
        const double pLeft = b.userSpaceLeft;
        const double pRight = b.userSpaceRight;
        const double pTop = b.userSpaceTop;
        const double pBottom = b.userSpaceBottom;

        minX = std::min(minX, pLeft);
        maxX = std::max(maxX, pRight);
        minY = std::min(minY, pBottom);
        maxY = std::max(maxY, pTop);

        pdf::AnnotationQuad quad;
        quad.x1 = pLeft;  quad.y1 = pTop;
        quad.x2 = pRight; quad.y2 = pTop;
        quad.x3 = pLeft;  quad.y3 = pBottom;
        quad.x4 = pRight; quad.y4 = pBottom;
        hl.quadPoints.push_back(quad);
    }
    if (hl.quadPoints.empty()) {
        return {{}, BuildError::MarkupNoCoordinates, {}};
    }
    hl.left = minX;
    hl.right = maxX;
    hl.bottom = minY;
    hl.top = maxY;

    return {std::move(hl), BuildError::None, {}};
}

QSizeF ViewerAnnotationBuilder::defaultAnnotationSize(
    pdf::AnnotationSubtype subtype, const QSizeF& pageSize, const QString& contents) {
    switch (subtype) {
        case pdf::AnnotationSubtype::Text:
            return QSizeF(24.0, 24.0);
        case pdf::AnnotationSubtype::Square:
        case pdf::AnnotationSubtype::Circle:
            return QSizeF(100.0, 60.0);
        case pdf::AnnotationSubtype::Stamp:
            return QSizeF(120.0, 40.0);
        case pdf::AnnotationSubtype::FileAttachment:
            return QSizeF(24.0, 24.0);
        case pdf::AnnotationSubtype::FreeText: {
            // As wide as its text wants (at most 300 pt, and never wider than the page), as tall as the wrapped text
            // needs, so that nothing is clipped. A little slack keeps rounding from wrapping a line that just fits.
            constexpr double kDefaultFontSize = 12.0;
            constexpr double kPreferredMaxTextWidth = 300.0;
            const double maxInner = std::max(20.0, std::min(kPreferredMaxTextWidth, pageSize.width() - 8.0 - 2.0 * kFreeTextPadding));
            const FreeTextLayoutResult measured = layoutFreeText(contents, kDefaultFontSize, maxInner, false);
            if (measured.size.isEmpty()) {
                return {};
            }
            return QSizeF(std::ceil(measured.size.width()) + 1.0 + 2.0 * kFreeTextPadding,
                          std::ceil(measured.size.height()) + 1.0 + 2.0 * kFreeTextPadding);
        }
        default:
            return {};
    }
}

QRectF ViewerAnnotationBuilder::placeOnPage(const QPointF& anchor, const QSizeF& size, const QSizeF& pageSizePoints) {
    const double values[] = {anchor.x(), anchor.y(), size.width(), size.height(), pageSizePoints.width(), pageSizePoints.height()};
    for (const double v : values) {
        if (!std::isfinite(v)) {
            return {};
        }
    }
    if (size.width() <= 0.0 || size.height() <= 0.0 || pageSizePoints.width() <= 0.0 || pageSizePoints.height() <= 0.0) {
        return {};
    }

    // Keep a little air to the page edge whenever the page is big enough to afford it.
    constexpr double kEdgeMargin = 4.0;
    const double marginX = pageSizePoints.width() > size.width() + 2.0 * kEdgeMargin ? kEdgeMargin : 0.0;
    const double marginY = pageSizePoints.height() > size.height() + 2.0 * kEdgeMargin ? kEdgeMargin : 0.0;
    const double w = std::min(size.width(), pageSizePoints.width());
    const double h = std::min(size.height(), pageSizePoints.height());

    const double left = std::clamp(anchor.x(), marginX, pageSizePoints.width() - marginX - w);
    const double upper = std::clamp(anchor.y(), marginY + h, pageSizePoints.height() - marginY);
    return QRectF(left, upper - h, w, h);
}

QString ViewerAnnotationBuilder::annotationTooltipHtml(const QString& author, const QString& contents) {
    constexpr qsizetype kMaxTipChars = 1000;
    auto prep = [](QString s) {
        if (s.size() > kMaxTipChars) s = s.left(kMaxTipChars) + QStringLiteral("…");
        return s.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"));
    };
    QString html = QStringLiteral("<p>");
    if (!author.isEmpty()) html += QStringLiteral("<b>[") + prep(author) + QStringLiteral("]</b><br>");
    html += prep(contents) + QStringLiteral("</p>");
    return html;
}

} // namespace ui
