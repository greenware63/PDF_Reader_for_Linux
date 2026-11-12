// Milestone M5: what do the AcroForm fixtures in tests/fixtures/ cost if their fields are listed naively?
// Developer tool, not part of the product build (see m5_form_spike.cpp for the same conventions).
//
// For every page of a PDF it walks the widget annotations the way a "list form fields" call would, but WITHOUT any
// cap, and reports how much data a naive listing would produce, how long PDFium takes and how much memory the
// process peaked at. It proves that a fixture really is hostile (bigger than one IPC response may be) and shows how
// the pinned PDFium behaves on pathological structure. Run it under `timeout`: a crash or a hang is a finding.
//
// build: g++ -std=c++17 -O1 -I<pdfium>/include m5_fixture_probe.cpp -L<pdfium>/lib -lpdfium -Wl,-rpath,<pdfium>/lib -o m5_fixture_probe
// usage: m5_fixture_probe <file.pdf>
// The "naive payload" uses the same arithmetic as formFieldBudgetCost() in include/engine/FormFieldTypes.h
// (128 bytes + 2 bytes per UTF-16 unit of name, value and options + 8 bytes per option), without any clipping.
#include "fpdf_annot.h"
#include "fpdf_formfill.h"
#include "fpdfview.h"

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace {

struct Stats {
    long widgets = 0;
    std::map<int, long> types;
    unsigned long long nameChars = 0, valueChars = 0, optionChars = 0, options = 0;
    unsigned long maxName = 0, maxValue = 0, maxOption = 0, maxOptionsOnField = 0;
    unsigned long long naiveBytes = 0;
};

// Length in UTF-16 units that PDFium reports for a text value (the first call with a null buffer).
unsigned long units(unsigned long bytesWithTerminator) { return bytesWithTerminator > 2 ? bytesWithTerminator / 2 - 1 : 0; }

const char* typeName(int t) {
    switch (t) {
        case FPDF_FORMFIELD_PUSHBUTTON: return "button";
        case FPDF_FORMFIELD_CHECKBOX: return "check";
        case FPDF_FORMFIELD_RADIOBUTTON: return "radio";
        case FPDF_FORMFIELD_COMBOBOX: return "combo";
        case FPDF_FORMFIELD_LISTBOX: return "list";
        case FPDF_FORMFIELD_TEXTFIELD: return "text";
        case FPDF_FORMFIELD_SIGNATURE: return "sig";
        default: return "unknown";
    }
}

double peakRssMb() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss / 1024.0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <file.pdf>\n", argv[0]);
        return 2;
    }
    FPDF_InitLibrary();
    FPDF_DOCUMENT doc = FPDF_LoadDocument(argv[1], nullptr);
    if (!doc) {
        std::printf("LOAD FAILED (FPDF error %lu)\n", FPDF_GetLastError());
        return 3;
    }
    FPDF_FORMFILLINFO info;
    std::memset(&info, 0, sizeof info);
    info.version = 1;  // like the engine: no callbacks at all
    FPDF_FORMHANDLE form = FPDFDOC_InitFormFillEnvironment(doc, &info);
    const int pages = FPDF_GetPageCount(doc);
    std::printf("%s: %d page(s)\n", argv[1], pages);

    for (int p = 0; p < pages; ++p) {
        const auto t0 = std::chrono::steady_clock::now();
        FPDF_PAGE page = FPDF_LoadPage(doc, p);
        if (!page) {
            std::printf("PAGE %d: FPDF_LoadPage failed\n", p);
            continue;
        }
        const auto t1 = std::chrono::steady_clock::now();
        FORM_OnAfterLoadPage(page, form);  // PDFium builds the appearance of every widget that has none, here
        const auto t2 = std::chrono::steady_clock::now();
        const double loadMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const double formMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
        Stats s;
        const int count = FPDFPage_GetAnnotCount(page);
        for (int i = 0; i < count; ++i) {
            FPDF_ANNOTATION a = FPDFPage_GetAnnot(page, i);
            if (!a) continue;
            if (FPDFAnnot_GetSubtype(a) == FPDF_ANNOT_WIDGET) {
                ++s.widgets;
                const int type = FPDFAnnot_GetFormFieldType(form, a);
                ++s.types[type];
                (void)FPDFAnnot_GetFormFieldFlags(form, a);
                FS_RECTF r{};
                (void)FPDFAnnot_GetRect(a, &r);
                const unsigned long nameU = units(FPDFAnnot_GetFormFieldName(form, a, nullptr, 0));
                const unsigned long valueU = units(FPDFAnnot_GetFormFieldValue(form, a, nullptr, 0));
                unsigned long optChars = 0, optN = 0;
                if (type == FPDF_FORMFIELD_COMBOBOX || type == FPDF_FORMFIELD_LISTBOX) {
                    const int oc = FPDFAnnot_GetOptionCount(form, a);
                    for (int o = 0; o < oc; ++o) {
                        const unsigned long ou = units(FPDFAnnot_GetOptionLabel(form, a, o, nullptr, 0));
                        optChars += ou;
                        s.maxOption = std::max(s.maxOption, ou);
                        (void)FPDFAnnot_IsOptionSelected(form, a, o);
                    }
                    optN = oc > 0 ? static_cast<unsigned long>(oc) : 0;
                }
                s.nameChars += nameU; s.valueChars += valueU; s.optionChars += optChars; s.options += optN;
                s.maxName = std::max(s.maxName, nameU); s.maxValue = std::max(s.maxValue, valueU);
                s.maxOptionsOnField = std::max(s.maxOptionsOnField, optN);
                s.naiveBytes += 128ull + 2ull * (nameU + valueU + optChars) + 8ull * optN;
            }
            FPDFPage_CloseAnnot(a);
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();
        std::printf("PAGE %d: annots=%d widgets=%ld types{", p, count, s.widgets);
        for (const auto& kv : s.types) std::printf(" %s=%ld", typeName(kv.first), kv.second);
        std::printf(" }\n         name max=%lu total=%llu | value max=%lu total=%llu | options total=%llu max-per-field=%lu max-label=%lu\n",
                    s.maxName, s.nameChars, s.maxValue, s.valueChars, s.options, s.maxOptionsOnField, s.maxOption);
        std::printf("         naive payload = %.1f MiB (%s the 16 MiB response budget) | peak RSS so far %.0f MiB\n",
                    s.naiveBytes / 1048576.0, s.naiveBytes > 16ull * 1048576 ? "OVER" : "under", peakRssMb());
        std::printf("         time: FPDF_LoadPage %.0f ms | FORM_OnAfterLoadPage %.0f ms | walking the widgets %.0f ms\n", loadMs, formMs, ms);
        FORM_OnBeforeClosePage(page, form);
        FPDF_ClosePage(page);
    }
    FPDFDOC_ExitFormFillEnvironment(form);
    FPDF_CloseDocument(doc);
    FPDF_DestroyLibrary();
    return 0;
}
