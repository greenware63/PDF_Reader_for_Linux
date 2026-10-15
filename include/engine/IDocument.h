#pragma once

#include "IPage.h"
#include "OutlineTypes.h"
#include <memory>
#include <string>
#include <cstdint>
#include <utility>

namespace pdf {

/**
 * @brief Document loading error codes.
 */
enum class DocumentError {
    None,
    FileNotFound,
    InvalidFormat,
    PasswordRequired,
    BadPassword,
    EmptyDocument,
    Unknown
};

/**
 * @brief Pure C++ abstract interface representing a PDF document.
 * Decouples the UI / Viewport layer from PDFium or any future worker process.
 */
class IDocument {
public:
    virtual ~IDocument() = default;

    /**
     * @brief Opens a PDF file from disk without destroying the current document on failure.
     * @param filePath Absolute or relative path to the PDF file.
     * @param password Optional password for encrypted PDFs.
     * @return DocumentError indicating success or the specific failure cause.
     */
    virtual DocumentError loadFromFile(const std::string& filePath, const std::string& password = "") = 0;

    /**
     * @brief Opens a PDF document from an in-memory buffer without destroying current document on failure.
     * @param data Pointer to raw PDF bytes.
     * @param size Size in bytes.
     * @param password Optional password for encrypted PDFs.
     * @return DocumentError indicating success or the specific failure cause.
     */
    virtual DocumentError loadFromMemory(const void* data, size_t size, const std::string& password = "") = 0;

    /**
     * @return Total number of pages in the document.
     */
    virtual int pageCount() const = 0;

    /**
     * @brief Queries page dimensions in PDF points without fully loading/rasterizing the page.
     * @param pageIndex 0-based page index.
     * @return std::pair of {widthPoints, heightPoints}, or {0.0, 0.0} on failure.
     */
    virtual std::pair<double, double> pageSizePoints(int pageIndex) const = 0;

    /**
     * @brief Loads and returns a page object. Keeps document alive via shared ownership.
     * @param pageIndex 0-based page index.
     * @return Unique pointer to IPage, or nullptr if invalid or failed to load.
     */
    virtual std::unique_ptr<IPage> getPage(int pageIndex) = 0;

    /**
     * @brief Closes the open document and releases engine resources.
     */
    /**
     * @brief Extracts the document outline (table of contents / bookmarks) hierarchy.
     * @return Vector of top-level OutlineItem nodes, each potentially containing nested children.
     */
    virtual std::vector<OutlineItem> getOutline() const = 0;

    virtual void close() = 0;

    /**
     * @return true if a document is currently loaded.
     */
    virtual bool isOpen() const = 0;

    /**
     * @return Last error encountered.
     */
    virtual DocumentError lastError() const = 0;
};

/**
 * @brief Factory function to create the default engine document instance (PDFium).
 */
std::unique_ptr<IDocument> createDocument();

} // namespace pdf
