#pragma once

#include "engine/AnnotationTypes.h"
#include "engine/PageText.h"

#include <QByteArray>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <vector>

namespace ui {

class ViewerAnnotationBuilder {
public:
    ViewerAnnotationBuilder() = delete;

    enum class TextBoxFit {
        Ok,
        Undrawable,
        TooLarge,
        NothingToDraw
    };

    enum class BuildError {
        None,
        Invalid,
        StampTextTooLong,
        StampLayoutFailed,
        TextUndrawable,
        TextTooLarge,
        TextNothingToDraw,
        AttachmentBadFileName,
        AttachmentBadPayload,
        MarkupNoCoordinates
    };

    struct BuiltAnnotation {
        pdf::AnnotationItem item{};
        BuildError error{BuildError::None};
        QString detail{};
    };

    static BuiltAnnotation buildText(int pageIndex, double userX, double userY,
                                     const QString& contents, uint32_t colorArgb = 0);

    static BuiltAnnotation buildSquare(int pageIndex, const QRectF& pagePointsRect,
                                       uint32_t colorArgb = 0, double borderWidth = 0.0);

    static BuiltAnnotation buildCircle(int pageIndex, const QRectF& pagePointsRect,
                                       uint32_t colorArgb = 0, double borderWidth = 0.0);

    static BuiltAnnotation buildInk(int pageIndex,
                                    const std::vector<std::vector<pdf::AnnotationPoint>>& strokes,
                                    uint32_t colorArgb = 0, double borderWidth = 0.0);

    static BuiltAnnotation buildFreeText(int pageIndex, const QRectF& pagePointsRect,
                                         const QString& contents, uint32_t colorArgb = 0,
                                         double fontSize = 0.0);

    static BuiltAnnotation buildStamp(int pageIndex, const QRectF& pagePointsRect,
                                      const QString& stampText, uint32_t colorArgb = 0);

    static BuiltAnnotation buildFileAttachment(int pageIndex, double userX, double userY,
                                               const QString& fileName, const QByteArray& data,
                                               uint32_t colorArgb, const QSizeF& pageSizePoints);

    static BuiltAnnotation buildMarkup(const pdf::PageText& pageText, int pageIndex,
                                       int startChar, int endChar,
                                       pdf::AnnotationSubtype subtype, uint32_t colorArgb = 0);

    static QSizeF defaultAnnotationSize(pdf::AnnotationSubtype subtype,
                                        const QSizeF& pageSize,
                                        const QString& contents = QString());

    static TextBoxFit fitTextBoxOutline(pdf::AnnotationItem& item, QString* undrawable = nullptr);

    static QRectF placeOnPage(const QPointF& anchor, const QSizeF& size, const QSizeF& pageSizePoints);

    static QString annotationTooltipHtml(const QString& author, const QString& contents);
};

} // namespace ui
