#pragma once

#include <QString>
#include <QDataStream>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace pdf {

enum class FormFieldType : uint32_t {
    Unknown = 0,
    PushButton = 1,
    CheckBox = 2,
    RadioButton = 3,
    ComboBox = 4,
    ListBox = 5,
    TextField = 6,
    Signature = 7
};

namespace FormFieldFlag {
    constexpr uint32_t ReadOnly      = 1u << 0;  // Bit 1: PDF ISO 32000-1 Table 220
    constexpr uint32_t Required      = 1u << 1;  // Bit 2
    constexpr uint32_t NoExport      = 1u << 2;  // Bit 3
    constexpr uint32_t Multiline     = 1u << 12; // Bit 13: Text field multiline
    constexpr uint32_t Password      = 1u << 13; // Bit 14: Text field password
    constexpr uint32_t NoToggleToOff = 1u << 14; // Bit 15: Radio buttons exactly one must be selected
    constexpr uint32_t Radio         = 1u << 15; // Bit 16: Radio button group flag
    constexpr uint32_t PushButton    = 1u << 16; // Bit 17: Pushbutton
    constexpr uint32_t Combo         = 1u << 17; // Bit 18: Choice field combo box
    constexpr uint32_t Edit          = 1u << 18; // Bit 19: Combo box with editable text box
    constexpr uint32_t Sort          = 1u << 19; // Bit 20: Options sorted alphabetically
    constexpr uint32_t MultiSelect   = 1u << 21; // Bit 22: Choice field multi-selection
}

// Bounded capacity constants aligned with RFC §6 and threat model
constexpr size_t kMaxFormFieldsPerPage = 1000;
constexpr size_t kMaxFormFieldOptions = 1000;
constexpr size_t kMaxFormFieldTextCodeUnits = 32768; // 32,768 UTF-16 code units (64 KiB wire bytes)
constexpr size_t kMaxFormFieldNameChars = 1024;
constexpr size_t kMaxFormFieldOptionChars = 1024;
constexpr uint64_t kMaxFormFieldListBytes = 16ull * 1024 * 1024; // 16 MiB per-page IPC response budget

struct FormFieldItem {
    int pageIndex{0};              // 0-based page index
    QString fieldName;             // Field name (/T or fully qualified name) - max 1024 chars
    int controlIndex{0};           // Control index for this field on this page (0-based)
    FormFieldType type{FormFieldType::Unknown};
    uint32_t flags{0};             // Form field flags bitfield

    // Bounding box in PDF user space points (standard bottom-left origin: left, top, right, bottom)
    double left{0.0};
    double top{0.0};
    double right{0.0};
    double bottom{0.0};

    // State & values
    QString value;                 // Value string (text content or selected option export value) - max 32,768 UTF-16 code units
    bool isChecked{false};         // Checkbox / Radio checked state
    std::vector<QString> options;  // Choice options (ComboBox / ListBox) - max 1000 options, 1024 chars each
    std::vector<int> selectedIndices; // Indices of selected options (ComboBox / ListBox)

    bool isValid() const noexcept {
        return pageIndex >= 0 && controlIndex >= 0 &&
               !std::isnan(left) && !std::isnan(top) &&
               !std::isnan(right) && !std::isnan(bottom) &&
               !std::isinf(left) && !std::isinf(top) &&
               !std::isinf(right) && !std::isinf(bottom);
    }

    bool isReadOnly() const noexcept { return (flags & FormFieldFlag::ReadOnly) != 0; }
    bool isRequired() const noexcept { return (flags & FormFieldFlag::Required) != 0; }
    bool isMultiline() const noexcept { return (flags & FormFieldFlag::Multiline) != 0; }
    bool isPassword() const noexcept { return (flags & FormFieldFlag::Password) != 0; }
    bool isMultiSelect() const noexcept { return (flags & FormFieldFlag::MultiSelect) != 0; }

    bool contains(double x, double y) const noexcept {
        if (!isValid() || std::isnan(x) || std::isnan(y) || std::isinf(x) || std::isinf(y)) {
            return false;
        }
        const double minX = std::min(left, right);
        const double maxX = std::max(left, right);
        const double minY = std::min(bottom, top);
        const double maxY = std::max(bottom, top);
        return x >= minX && x <= maxX && y >= minY && y <= maxY;
    }
};

inline uint64_t formFieldBudgetCost(const FormFieldItem& f) noexcept {
    uint64_t optChars = 0;
    for (const auto& opt : f.options) {
        optChars += static_cast<uint64_t>(opt.size());
    }
    const uint64_t chars = static_cast<uint64_t>(f.fieldName.size()) +
                           static_cast<uint64_t>(f.value.size()) +
                           optChars;
    return 128ull + 2ull * chars + 8ull * static_cast<uint64_t>(f.options.size()) +
           4ull * static_cast<uint64_t>(f.selectedIndices.size());
}

QDataStream& operator<<(QDataStream& out, const FormFieldItem& item);
QDataStream& operator>>(QDataStream& in, FormFieldItem& item);

} // namespace pdf
