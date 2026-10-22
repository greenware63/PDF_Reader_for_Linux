#pragma once

/**
 * Burn-in redaction engine (Stage 1).  See docs/REDACTION.md for the normative
 * guarantees (G1..G10) and limitations (L1..L12).  This header is deliberately
 * free of Qt and of any selection/UI concept: it takes bytes and rectangles and
 * returns bytes and a report.
 *
 * WARNING (L6): redaction creates a NEW file.  Every digital signature present in
 * the input is invalidated/removed.  Callers must warn the user.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pdf::redact {

/// A rectangle in PDF points, origin at the TOP-LEFT of the displayed page
/// (after /Rotate and crop box), i.e. the same space the viewer shows.
struct Rect {
    double left = 0.0;
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
};

struct PageRedactions {
    int pageIndex = 0;          ///< 0-based
    std::vector<Rect> rects;
};

struct Limits {
    std::size_t maxInputBytes = 512ull * 1024 * 1024;
    std::size_t maxOutputBytes = 1024ull * 1024 * 1024;
    int maxPages = 2000;
    int maxDimensionPx = 16384;
    std::uint64_t maxPixelsPerPage = 64ull * 1000 * 1000;
    std::uint64_t maxTotalPixels = 2000ull * 1000 * 1000;
    std::size_t maxRectsPerPage = 10000;
    std::size_t maxTotalRects = 100000;
};

struct Options {
    double dpi = 150.0;      ///< allowed range [72, 300]
    int edgeGrowPx = 1;      ///< extra whole pixels added around every box, [0, 16]
    Limits limits;
    /// Optional liveness/progress callback, invoked on the calling thread with (steps done, total steps).
    /// One step is counted per page in each of three phases (plan, render, verify), so `done` rises
    /// monotonically from 1 to 3 x pageCount. It is ALSO invoked, repeating the current `done`, while the
    /// output is being serialised (a single long PDFium call that would otherwise be silent). A host uses
    /// it as a heartbeat so a long but healthy redaction is not mistaken for a hung one. The callback must
    /// be cheap, must not call back into this engine, and must not throw (an exception thrown during
    /// serialisation is treated as a failed write and the redaction fails closed).
    std::function<void(int done, int total)> onProgress;
    // Note: there is intentionally no option to disable verification (G9).
};

enum class Status {
    Ok,
    InvalidInput,        ///< null/empty data, bad options, over input limit
    InvalidFormat,       ///< not a PDF / unparsable
    PasswordRequired,
    BadPassword,
    InvalidRect,         ///< non-finite, inverted, zero-area or fully off-page rectangle
    PageOutOfRange,
    NothingToRedact,     ///< no rectangles supplied
    LimitExceeded,
    RenderFailed,
    BuildFailed,
    VerificationFailed,  ///< output did not satisfy the self-check; no bytes are returned
    InternalError
};

const char* toString(Status s);

struct PixelBox {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;   ///< half-open [x0,x1) x [y0,y1) in output pixels
};

struct PageReport {
    int pageIndex = 0;
    double widthPt = 0.0, heightPt = 0.0;
    int widthPx = 0, heightPx = 0;
    std::vector<PixelBox> boxes;            ///< merged/clamped boxes actually blackened
    int sourceAnnotationCount = 0;          ///< informational: what the source had on this page
};

struct Report {
    int pageCount = 0;
    double dpi = 0.0;
    int sourceSignatureCount = 0;           ///< >0 => signatures were invalidated (L6)
    int sourceAttachmentCount = 0;
    bool sourceWasEncrypted = false;
    std::size_t outputBytes = 0;
    std::vector<PageReport> pages;
    std::vector<std::string> warnings;      ///< human-readable; UI should surface them
    std::vector<std::string> verification;  ///< checks that ran and passed
};

struct Result {
    Status status = Status::InternalError;
    std::string message;
    std::vector<std::uint8_t> pdf;          ///< empty unless status == Ok (G9)
    Report report;
    bool ok() const { return status == Status::Ok; }
};

/**
 * Redact @p redactions out of the PDF in @p data, producing a new raster-only PDF.
 * Synchronous; serialised by the process-wide PDFium mutex; safe against hostile input
 * in the sense that failure never yields output bytes.
 */
Result redactDocument(const std::uint8_t* data, std::size_t size,
                      const std::string& password,
                      const std::vector<PageRedactions>& redactions,
                      const Options& options = {});

/**
 * Self-check used by redactDocument() and exposed for tests: re-opens @p pdf with PDFium and
 * verifies the structural guarantees and that every pixel of every box in the STORED image is
 * zero. @p tick, if set, is called once per output page before that page is checked.  Returns an empty string on success, otherwise a description of the first failure.
 */
std::string verifyRedactedOutput(const std::vector<std::uint8_t>& pdf,
                                 const std::vector<PageReport>& pages,
                                 const std::function<void()>& tick = {});

} // namespace pdf::redact
