#include <iostream>
#include <QImage>
#include "engine/IDocument.h"

int main(int argc, char* argv[]) {
    std::cout << "[TEST] Starting PDF rendering spike test..." << std::endl;

    std::string pdfPath = "sample.pdf";
    if (argc > 1) {
        pdfPath = argv[1];
    }

    auto doc = pdf::createDocument();
    if (!doc) {
        std::cerr << "[FAIL] Failed to create IDocument instance" << std::endl;
        return 1;
    }

    std::cout << "[TEST] Loading PDF: " << pdfPath << std::endl;
    if (!doc->loadFromFile(pdfPath)) {
        std::cerr << "[FAIL] Failed to load PDF file: " << pdfPath << std::endl;
        return 1;
    }

    int count = doc->pageCount();
    std::cout << "[TEST] Total pages: " << count << std::endl;
    if (count <= 0) {
        std::cerr << "[FAIL] Expected at least 1 page" << std::endl;
        return 1;
    }

    auto page = doc->getPage(0);
    if (!page) {
        std::cerr << "[FAIL] Failed to retrieve page 0" << std::endl;
        return 1;
    }

    std::cout << "[TEST] Page 0 dimensions: " 
              << page->widthPoints() << " x " << page->heightPoints() << " pt" << std::endl;

    double scale = 1.0;
    double dpr = 2.0;   // 2x HiDPI
    std::cout << "[TEST] Rendering page (scale=" << scale << ", dpr=" << dpr << ")..." << std::endl;
    QImage rendered = page->render(scale, dpr);

    if (rendered.isNull()) {
        std::cerr << "[FAIL] Rendered QImage is null!" << std::endl;
        return 1;
    }

    std::cout << "[TEST] Rendered bitmap size: " 
              << rendered.width() << " x " << rendered.height() << " px" << std::endl;

    bool saved = rendered.save("test_rendered_page0.png");
    if (!saved) {
        std::cerr << "[FAIL] Failed to save output PNG" << std::endl;
        return 1;
    }

    std::cout << "[PASS] Page 0 rendered and saved successfully to test_rendered_page0.png!" << std::endl;
    return 0;
}
