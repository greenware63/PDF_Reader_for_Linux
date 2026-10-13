#pragma once

#include "IPage.h"
#include <memory>
#include <string>

namespace pdf {

/**
 * @brief Abstract interface representing a PDF document.
 * Decouples the UI / Viewport layer from PDFium or any future worker process.
 */
class IDocument {
public:
    virtual ~IDocument() = default;

    /**
     * @brief Opens a PDF file from disk.
     * @param filePath Absolute or relative path to the PDF file.
     * @param password Optional password for encrypted PDFs.
     * @return true if successfully loaded, false otherwise.
     */
    virtual bool loadFromFile(const std::string& filePath, const std::string& password = "") = 0;

    /**
     * @brief Opens a PDF document from an in-memory buffer.
     * @param data Pointer to raw PDF bytes.
     * @param size Size in bytes.
     * @param password Optional password for encrypted PDFs.
     * @return true if successfully loaded, false otherwise.
     */
    virtual bool loadFromMemory(const void* data, size_t size, const std::string& password = "") = 0;

    /**
     * @return Total number of pages in the document.
     */
    virtual int pageCount() const = 0;

    /**
     * @brief Loads and returns a page object.
     * @param pageIndex 0-based page index.
     * @return Unique pointer to IPage, or nullptr if invalid.
     */
    virtual std::unique_ptr<IPage> getPage(int pageIndex) = 0;

    /**
     * @brief Closes the open document and releases engine resources.
     */
    virtual void close() = 0;

    /**
     * @return true if a document is currently loaded.
     */
    virtual bool isOpen() const = 0;
};

/**
 * @brief Factory function to create the default engine document instance (PDFium).
 */
std::unique_ptr<IDocument> createDocument();

} // namespace pdf
