#include "DLGEditorPanel.hpp"
#include <neoshared/GffResourceDocument.hpp>
#include "core/AppModel.hpp"
#include "core/GffJson.hpp"
#include "core/Version.hpp"
#include "neodlg/model/DlgDocument.hpp"
#include "neodlg/model/DlgFieldApplicability.hpp"
#include "neodlg/model/DlgTreeVisibility.hpp"
#include "neodlg/model/DlgSemanticOptions.hpp"
#include "neodlg/patcher/DlgPatcher.hpp"

#include "NeoDlgDialogs.hpp"

#include "NeoDocumentTabs.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoSettings.hpp"
#include "NeoPatcherExport.hpp"
#include "NeoTreeState.hpp"
#include "NeoViewState.hpp"
#include "NeoWxUi.hpp"
#include "TslPatcher.hpp"

#include <wx/aui/auibook.h>
#include <wx/checkbox.h>
#include <wx/clrpicker.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/dcbuffer.h>
#include <wx/grid.h>
#include <wx/clipbrd.h>
#include <wx/icon.h>
#include <wx/iconbndl.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/statbox.h>
#include <wx/treectrl.h>
#include <wx/wupdlock.h>
#include <wx/wrapsizer.h>
#include <wx/wx.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

static_assert(wxui::kPatcherExportUiApiVersion >= 3u,
              "NeoDLG requires the exact-INI/Fragment patch-export UI from the current neoshared checkout.");
#if defined(__EMSCRIPTEN__)
static_assert(neobrowser::kBrowserFileApiVersion >= 10u,
              "NeoDLG requires owned browser imports and transactional write-back from the current neoshared checkout.");
#endif

namespace {

using namespace neodlg;

constexpr const char* kAppName = "NeoDLG";
constexpr const char* kDlgWildcard = "DLG conversation files (*.dlg)|*.dlg|All files (*.*)|*.*";
constexpr const char* kTlkWildcard = "TLK files (*.tlk)|*.tlk|All files (*.*)|*.*";
constexpr const char* kXmlWildcard = "XML files (*.xml)|*.xml|All files (*.*)|*.*";
constexpr const char* kJsonWildcard = "JSON files (*.json)|*.json|All files (*.*)|*.*";
constexpr std::size_t kUndoLimit = 8;

std::string readTextFile(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("Unable to open text file: " + neosettings::pathToUtf8(file));
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

void writeTextFile(const std::filesystem::path& file, const std::string& text) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Unable to create text file: " + neosettings::pathToUtf8(file));
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) throw std::runtime_error("Unable to write text file: " + neosettings::pathToUtf8(file));
}

std::filesystem::path ensureDlgExtension(std::filesystem::path path) {
    if (!path.empty() && path.extension().empty()) path.replace_extension(".dlg");
    return path;
}

std::string trimHeader(std::string text) {
    while (!text.empty() && (text.back() == '\0' || text.back() == ' ' || text.back() == '\t')) text.pop_back();
    return text;
}

std::string dialectName(DlgDialect dialect) {
    switch (dialect) {
    case DlgDialect::Kotor: return "KotOR-style DLG";
    case DlgDialect::JadeEmpire: return "Jade Empire DLG";
    case DlgDialect::Unsupported: return "Raw/unsupported DLG schema";
    }
    return "Unknown DLG";
}

std::string flavorName(DlgFlavor flavor) {
    switch (flavor) {
    case DlgFlavor::Kotor: return "Knights of the Old Republic";
    case DlgFlavor::Kotor2: return "Knights of the Old Republic II";
    case DlgFlavor::JadeEmpire: return "Jade Empire";
    }
    return "Unknown";
}

std::string issueSeverityName(DlgIssueSeverity severity) {
    switch (severity) {
    case DlgIssueSeverity::Error: return "Error";
    case DlgIssueSeverity::Warning: return "Warning";
    case DlgIssueSeverity::Information: return "Info";
    }
    return "Info";
}

std::string linkConditionSummary(const DlgDocument& document, DlgLinkRef ref) {
    std::string active = document.linkField(ref, "Active");
    std::string active2 = document.linkField(ref, "Active2");
    std::string result;
    if (!active.empty()) result = "if " + active;
    if (!active2.empty()) {
        if (!result.empty()) result += " + ";
        result += active2;
    }
    if (result.empty() && document.hasLinkField(ref, "ReverseCond") && document.linkField(ref, "ReverseCond") != "0") {
        result = "reversed condition";
    }
    return result;
}

// Each field occupies a label/value pair in a real grid row. Choose the largest
// number of pairs whose column minima fit the *viewport*, not a wrapped sizer's
// cached minimum width. Returning one pair permits narrow inspectors without
// widening scalar controls or changing their input ranges.
int inspectorFieldColumns(const std::vector<std::array<int, 2>>& widths,
                          int availableWidth, int gap, int maximumColumns) {
    const int limit = std::min(std::max(1, maximumColumns),
                               static_cast<int>(widths.size()));
    for (int columns = limit; columns > 1; --columns) {
        std::vector<std::array<int, 2>> maxima(static_cast<std::size_t>(columns), {0, 0});
        for (std::size_t i = 0; i < widths.size(); ++i) {
            auto& slot = maxima[i % static_cast<std::size_t>(columns)];
            slot[0] = std::max(slot[0], widths[i][0]);
            slot[1] = std::max(slot[1], widths[i][1]);
        }
        int required = (2 * columns - 1) * gap;
        for (const auto& slot : maxima) required += slot[0] + slot[1];
        if (required <= availableWidth) return columns;
    }
    return 1;
}

// Pack complete label/value groups in reading order using their actual widths.
// Unlike a column grid, a long label on one row does not pad every other row.
// No two/three-field ceiling: short groups keep filling the current row. An
// oversized group gets a row of its own without truncating or splitting it.
std::vector<int> inspectorFieldRows(const std::vector<int>& widths,
                                    int availableWidth, int gap) {
    const std::int64_t available = std::max(1, availableWidth);
    const std::int64_t spacing = std::max(0, gap);
    std::vector<int> rows;
    rows.reserve(widths.size());
    std::int64_t used = 0;
    int row = 0;
    bool occupied = false;
    for (const int value : widths) {
        const std::int64_t width = std::max(0, value);
        if (occupied && used + spacing + width > available) {
            ++row;
            used = 0;
            occupied = false;
        }
        used += (occupied ? spacing : 0) + width;
        occupied = true;
        rows.push_back(row);
    }
    return rows;
}

// Conversation keeps its original grid; Single Panel uses content-sized native
// fields without an unused grid canvas. Switching copies only pending UI text,
// never applies it to the document or changes numeric parsing/validation.
class IntegerParameterFields final : public wxPanel {
public:
    IntegerParameterFields(wxWindow* parent,
                           const wxString& firstColumn,
                           const wxString& secondColumn)
        : wxPanel(parent, wxID_ANY) {
        const std::array<wxString, 2> titles{firstColumn, secondColumn};
        // One responsive strip per script/condition, rather than a header and
        // five tall parameter rows. The grid presentation remains unchanged in
        // Conversation. All ten controls keep their original row/column keys.
        auto* form = new wxFlexGridSizer(2, FromDIP(3), FromDIP(8));
        for (std::size_t column = 0; column < titles.size(); ++column) {
            compactTitles_[column] = new wxStaticText(this, wxID_ANY, titles[column]);
            form->Add(compactTitles_[column], 0, wxALIGN_CENTER_VERTICAL);
            auto* strip = new wxFlexGridSizer(2, FromDIP(3), FromDIP(4));
            compactRows_[column] = strip;
            for (std::size_t row = 0; row < cells_.size(); ++row) {
                auto* label = new wxStaticText(this, wxID_ANY,
                    wxString::Format("%d:", static_cast<int>(row + 1)));
                label->SetToolTip(titles[column] + wxString::Format(
                    " parameter %d", static_cast<int>(row + 1)));
                compactLabels_[row][column] = label;
                auto* field = new wxTextCtrl(this, wxID_ANY);
                field->SetName(titles[column] + wxString::Format(
                    " parameter %d", static_cast<int>(row + 1)));
                field->SetToolTip(field->GetName());
                cells_[row][column] = field;
                strip->Add(label, 0, wxALIGN_CENTER_VERTICAL);
                strip->Add(field, 0, wxALIGN_CENTER_VERTICAL);
            }
            form->Add(strip, 0);
        }
        compactSizer_ = form;
        grid_ = new wxGrid(this, wxID_ANY);
        grid_->SetName("NeoDLG conversation parameter grid");
        grid_->CreateGrid(5, 2);
        for (int column = 0; column < 2; ++column) grid_->SetColLabelValue(column, titles[column]);
        for (int row = 0; row < 5; ++row)
            grid_->SetRowLabelValue(row, wxString::Format("Param %d", row + 1));
        grid_->SetMinSize(FromDIP(wxSize(420, 190)));
        grid_->Bind(wxEVT_GRID_CELL_CHANGED, [this](wxGridEvent& event) {
            const int row = event.GetRow(), column = event.GetCol();
            if (row >= 0 && row < 5 && column >= 0 && column < 2)
                gridEdited_[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)] = true;
            event.Skip();
            wxCommandEvent changed(wxEVT_TEXT, GetId());
            changed.SetEventObject(this);
            GetEventHandler()->ProcessEvent(changed);
        });
        auto* root = new wxBoxSizer(wxVERTICAL);
        root->Add(grid_, 1, wxEXPAND);
        root->Add(compactSizer_, 0, wxEXPAND);
        SetSizer(root);
        compactSizer_->ShowItems(false);
        RefreshFieldMetrics();
    }

    void SetSinglePanel(bool singlePanel) {
        if (singlePanel_ == singlePanel) return;
        CommitGridEditor();
        for (std::size_t row = 0; row < cells_.size(); ++row) {
            for (std::size_t column = 0; column < cells_[row].size(); ++column) {
                if (singlePanel) {
                    auto* field = cells_[row][column];
                    const wxString value = grid_->GetCellValue(static_cast<int>(row), static_cast<int>(column));
                    const bool changed = field->IsModified() || gridEdited_[row][column] || field->GetValue() != value;
                    field->ChangeValue(value);
                    if (changed) field->MarkDirty();
                } else
                    grid_->SetCellValue(static_cast<int>(row), static_cast<int>(column),
                                        cells_[row][column]->GetValue());
            }
        }
        singlePanel_ = singlePanel;
        grid_->Show(!singlePanel);
        compactSizer_->ShowItems(singlePanel);
        SetVisibleColumns(compactColumns_[0], compactColumns_[1]);
        RefreshFieldMetrics();
    }

    wxString GetCellValue(std::size_t row, std::size_t column) const {
        auto* field = cells_.at(row).at(column); // Preserve bounds checks in both modes.
        CommitGridEditor();
        return singlePanel_ ? field->GetValue()
                            : grid_->GetCellValue(static_cast<int>(row), static_cast<int>(column));
    }

    void SetCellValue(std::size_t row, std::size_t column, const wxString& value) {
        auto* field = cells_.at(row).at(column);
        // Programmatic loading replaces pending input; do not leave an editor
        // for the previously selected node alive over the new grid contents.
        if (grid_->IsCellEditControlEnabled()) {
            grid_->HideCellEditControl();
            grid_->DisableCellEditControl();
        }
        field->ChangeValue(value);
        gridEdited_[row][column] = false;
        grid_->SetCellValue(static_cast<int>(row), static_cast<int>(column), value);
    }

    void ClearValues() {
        for (std::size_t row = 0; row < cells_.size(); ++row)
            for (std::size_t column = 0; column < cells_[row].size(); ++column)
                SetCellValue(row, column, wxEmptyString);
    }

    void SetVisibleColumns(bool first, bool second) {
        const std::array<bool, 2> columns{first, second};
        if (columns != compactColumns_) CommitGridEditor();
        compactColumns_ = columns;
        for (std::size_t column = 0; column < compactColumns_.size(); ++column) {
            const bool show = compactColumns_[column];
            if (show) grid_->ShowCol(static_cast<int>(column));
            else grid_->HideCol(static_cast<int>(column));
            compactTitles_[column]->Show(singlePanel_ && show);
            compactSizer_->Show(compactRows_[column], singlePanel_ && show);
        }
        InvalidateBestSize();
    }

    bool ColumnShown(std::size_t column) const {
        return compactColumns_.at(column);
    }

    bool HasPendingOrNonzeroValue(std::size_t column) const {
        for (std::size_t row = 0; row < cells_.size(); ++row) {
            const auto* cell = cells_[row][column];
            wxString value = GetCellValue(row, column);
            value.Trim(true).Trim(false);
            if (cell->IsModified() || gridEdited_[row][column] || (!value.empty() && value != "0")) return true;
        }
        return false;
    }

    void SetAvailableWidth(int width) {
        availableWidth_ = std::max(1, width);
        ReflowCompactRows();
    }

    void RefreshFieldMetrics() {
        for (const auto& row : cells_) {
            for (auto* field : row) {
                // This is a text viewport, not the numeric range. Keep four
                // digits visible in Single Panel; longer/signed values scroll
                // inside the same control without reflowing the inspector.
                const int width = singlePanel_
                    ? field->GetTextExtent("0000").x + field->FromDIP(16)
                    : std::max(FromDIP(100), field->GetTextExtent("-2147483648").x + FromDIP(20));
                field->SetMinSize(wxSize(width, -1));
                field->SetMaxSize(singlePanel_ ? wxSize(width, -1) : wxDefaultSize);
                field->InvalidateBestSize();
            }
        }
        ReflowCompactRows();
        InvalidateBestSize();
        Layout();
    }

private:
    void ReflowCompactRows() {
        if (!singlePanel_) return;
        // Use the enclosing scroll viewport, not this panel's cached best size.
        // Invalidate upwards after changing the column count so FitInside sees
        // the extra rows immediately on a narrow resize or font-scale change.
        int titleWidth = 0;
        for (auto* title : compactTitles_)
            titleWidth = std::max(titleWidth, title->GetEffectiveMinSize().x);
        const int width = std::max(1, availableWidth_ - titleWidth - FromDIP(8));
        for (std::size_t column = 0; column < compactRows_.size(); ++column) {
            std::vector<std::array<int, 2>> widths;
            for (std::size_t row = 0; row < cells_.size(); ++row) {
                widths.push_back({compactLabels_[row][column]->GetEffectiveMinSize().x,
                                  cells_[row][column]->GetEffectiveMinSize().x});
            }
            const int columns = inspectorFieldColumns(widths, width, FromDIP(4), 5);
            compactRows_[column]->SetCols(2 * columns);
        }
        InvalidateBestSize();
    }

    void CommitGridEditor() const {
        // Include the value being typed in a cell, not just the last committed
        // value, when applying changes or switching the inspector layout.
        if (grid_->IsCellEditControlEnabled()) {
            grid_->SaveEditControlValue();
            grid_->HideCellEditControl();
            grid_->DisableCellEditControl();
        }
    }
    std::array<std::array<wxTextCtrl*, 2>, 5> cells_{};
    std::array<std::array<bool, 2>, 5> gridEdited_{};
    std::array<std::array<wxStaticText*, 2>, 5> compactLabels_{};
    std::array<wxStaticText*, 2> compactTitles_{};
    std::array<wxFlexGridSizer*, 2> compactRows_{};
    int availableWidth_ = 1;
    wxGrid* grid_ = nullptr;
    wxSizer* compactSizer_ = nullptr;
    bool singlePanel_ = false;
    std::array<bool, 2> compactColumns_{true, true};
};


// Forward width constraints across the panel boundary. Otherwise a wrap sizer
// inside a section can keep a cached one-row height after the inspector narrows.
class InspectorContentPanel final : public wxPanel {
public:
    explicit InspectorContentPanel(wxWindow* parent) : wxPanel(parent, wxID_ANY) {}

    void SetSinglePanel(bool singlePanel) {
        singlePanel_ = singlePanel;
        InvalidateBestSize();
    }

    bool InformFirstDirection(int direction, int size, int availableOtherDir) override {
        if (!singlePanel_ || direction != wxHORIZONTAL || size <= 0 || !GetSizer()) {
            return wxPanel::InformFirstDirection(direction, size, availableOtherDir);
        }
        const int clientWidth = std::max(1, size - GetWindowBorderSize().x);
        const bool changed = GetSizer()->InformFirstDirection(
            direction, clientWidth, availableOtherDir);
        InvalidateBestSize();
        return changed;
    }
private:
    bool singlePanel_ = false;
};

// A real multiline text control with a small, separate resize grip. Do not put
// children on a native wxTextCtrl: that is not portable between GTK/MSW/Cocoa.
// Only the height is user-sized; the field width follows the inspector.
class ResizableInspectorText final : public wxPanel {
public:
    ResizableInspectorText(wxWindow* parent, const wxString& name, long style,
                           std::function<void()> relayout, int conversationHeightDip)
        : wxPanel(parent, wxID_ANY), relayout_(std::move(relayout)),
          conversationHeightDip_(conversationHeightDip) {
        SetName(name + " editor");
        text_ = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxDefaultSize, style | wxTE_MULTILINE);
        text_->SetName(name);
        grip_ = new wxWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxBORDER_NONE | wxWANTS_CHARS);
        grip_->SetName(name + " resize grip");
        grip_->SetToolTip("Drag up/down to resize. Double-click to reset. "
                          "Arrow keys resize; Home resets.");
        grip_->SetCursor(wxCursor(wxCURSOR_SIZENS));
        grip_->SetBackgroundStyle(wxBG_STYLE_PAINT);
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(text_, 1, wxEXPAND);
        row->Add(grip_, 0, wxALIGN_BOTTOM | wxLEFT, FromDIP(2));
        SetSizer(row);
        RefreshMetrics();

        grip_->Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(grip_);
            dc.SetBackground(wxBrush(GetBackgroundColour()));
            dc.Clear();
            dc.SetPen(wxPen(GetForegroundColour()));
            const wxSize size = grip_->GetClientSize();
            const int right = size.x - FromDIP(2);
            const int bottom = size.y - FromDIP(2);
            for (int length : {3, 6, 9}) {
                const int n = FromDIP(length);
                dc.DrawLine(right - n, bottom, right, bottom - n);
            }
            if (grip_->HasFocus()) {
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawRectangle(0, 0, std::max(1, size.x - 1), std::max(1, size.y - 1));
            }
        });
        const auto focusChanged = [this](wxFocusEvent& event) {
            grip_->Refresh();
            event.Skip();
        };
        grip_->Bind(wxEVT_SET_FOCUS, focusChanged);
        grip_->Bind(wxEVT_KILL_FOCUS, focusChanged);
        grip_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            grip_->SetFocus();
            dragOriginY_ = wxGetMousePosition().y;
            dragExtraHeightDip_ = extraHeightDip_;
            dragging_ = true;
            if (!grip_->HasCapture()) grip_->CaptureMouse();
        });
        grip_->Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            if (!dragging_) { event.Skip(); return; }
            if (!event.LeftIsDown()) { endDrag(); return; }
            // Screen coordinates stay stable when FitInside moves the field.
            setExtraHeight(dragExtraHeightDip_ + ToDIP(wxGetMousePosition().y - dragOriginY_));
        });
        grip_->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent&) { endDrag(); });
        grip_->Bind(wxEVT_MOUSE_CAPTURE_LOST, [this](wxMouseCaptureLostEvent&) {
            dragging_ = false;
        });
        grip_->Bind(wxEVT_LEFT_DCLICK, [this](wxMouseEvent&) {
            endDrag();
            setExtraHeight(0);
        });
        grip_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
            const int line = std::max(1, ToDIP(text_->GetCharHeight()));
            switch (event.GetKeyCode()) {
                case WXK_UP: setExtraHeight(extraHeightDip_ - line); break;
                case WXK_DOWN: setExtraHeight(extraHeightDip_ + line); break;
                case WXK_PAGEUP: setExtraHeight(extraHeightDip_ - 4 * line); break;
                case WXK_PAGEDOWN: setExtraHeight(extraHeightDip_ + 4 * line); break;
                case WXK_HOME: setExtraHeight(0); break;
                case WXK_ESCAPE: endDrag(); break;
                case WXK_TAB:
                    grip_->Navigate(event.ShiftDown() ? wxNavigationKeyEvent::IsBackward
                                                     : wxNavigationKeyEvent::IsForward);
                    break;
                default: event.Skip(); break;
            }
        });
    }

    ~ResizableInspectorText() override { endDrag(); }
    wxTextCtrl* Text() const { return text_; }

    void SetSinglePanel(bool singlePanel) {
        if (singlePanel_ == singlePanel) return;
        endDrag();
        singlePanel_ = singlePanel;
        RefreshMetrics();
    }

    void RefreshMetrics() {
        // User growth belongs to Single Panel only. Conversation restores its
        // original text heights without erasing Single Panel's remembered size.
        const int height = singlePanel_
            ? std::max(1, text_->GetCharHeight()) + FromDIP(8 + extraHeightDip_)
            : FromDIP(conversationHeightDip_);
        text_->SetMinSize(wxSize(singlePanel_ ? FromDIP(80) : -1, height));
        grip_->Show(singlePanel_);
        grip_->SetMinSize(FromDIP(wxSize(12, 12)));
        SetMinSize(wxSize(singlePanel_ ? FromDIP(94) : -1, height));
        InvalidateBestSize();
        Layout();
        grip_->Refresh();
    }

private:
    void endDrag() {
        dragging_ = false;
        if (grip_ && grip_->HasCapture()) grip_->ReleaseMouse();
    }
    void setExtraHeight(int dip) {
        if (!singlePanel_) return;
        const int next = std::clamp(dip, 0, 1600);
        if (next == extraHeightDip_) return;
        extraHeightDip_ = next;
        RefreshMetrics();
        if (relayout_) relayout_();
    }

    wxTextCtrl* text_ = nullptr;
    wxWindow* grip_ = nullptr;
    std::function<void()> relayout_;
    int conversationHeightDip_ = 0;
    bool singlePanel_ = false;
    int extraHeightDip_ = 0;
    int dragExtraHeightDip_ = 0;
    int dragOriginY_ = 0;
    bool dragging_ = false;
};

wxTextCtrl* addTextField(wxWindow* parent,
                         wxFlexGridSizer* form,
                         const wxString& label,
                         long style = 0,
                         const wxSize& minSize = wxDefaultSize,
                         wxStaticText** labelOut = nullptr) {
    auto* labelControl = new wxStaticText(parent, wxID_ANY, label);
    if (labelOut) *labelOut = labelControl;
    form->Add(labelControl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    auto* control = new wxTextCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, style);
    if (minSize != wxDefaultSize) control->SetMinSize(minSize);
    form->Add(control, 1, wxEXPAND);
    return control;
}

wxCheckBox* addCheckField(wxWindow* parent,
                          wxFlexGridSizer* form,
                          const wxString& label,
                          wxStaticText** placeholderOut = nullptr) {
    auto* placeholder = new wxStaticText(parent, wxID_ANY, wxEmptyString);
    if (placeholderOut) *placeholderOut = placeholder;
    form->Add(placeholder, 0);
    auto* control = new wxCheckBox(parent, wxID_ANY, label);
    form->Add(control, 0, wxALIGN_CENTER_VERTICAL);
    return control;
}

wxTextCtrl* addTextFieldWithUnit(wxWindow* parent,
                                 wxFlexGridSizer* form,
                                 const wxString& label,
                                 const wxString& unit,
                                 wxStaticText** labelOut = nullptr,
                                 wxStaticText** unitOut = nullptr) {
    auto* labelControl = new wxStaticText(parent, wxID_ANY, label);
    if (labelOut) *labelOut = labelControl;
    form->Add(labelControl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* control = new wxTextCtrl(parent, wxID_ANY);
    row->Add(control, 1, wxEXPAND | wxRIGHT, 6);
    auto* unitControl = new wxStaticText(parent, wxID_ANY, unit);
    if (unitOut) *unitOut = unitControl;
    row->Add(unitControl, 0, wxALIGN_CENTER_VERTICAL);
    form->Add(row, 1, wxEXPAND);
    return control;
}

template <std::size_t N>
void populateIntegerChoice(wxChoice* control,
                           std::vector<std::string>& values,
                           const std::array<DlgIntegerOption, N>& options,
                           std::string current,
                           int defaultValue,
                           const std::string& unknownDescription) {
    if (!control) return;
    if (current.empty()) current = std::to_string(defaultValue);
    control->Clear();
    values.clear();

    int selected = wxNOT_FOUND;
    for (const auto& option : options) {
        values.push_back(std::to_string(option.value));
        control->Append(wxui::toWx(std::to_string(option.value) + " - " + option.label));
        if (values.back() == current) selected = static_cast<int>(values.size() - 1);
    }

    if (selected == wxNOT_FOUND) {
        values.push_back(current);
        control->Append(wxui::toWx(current + " - " + unknownDescription));
        selected = static_cast<int>(values.size() - 1);
    }
    control->SetSelection(selected);
}

std::string selectedIntegerChoice(const wxChoice* control,
                                  const std::vector<std::string>& values,
                                  const std::string& fieldName) {
    if (!control) throw std::runtime_error(fieldName + " control is unavailable.");
    const int selection = control->GetSelection();
    if (selection == wxNOT_FOUND || selection < 0 || static_cast<std::size_t>(selection) >= values.size()) {
        throw std::invalid_argument("Select a value for " + fieldName + ".");
    }
    return values[static_cast<std::size_t>(selection)];
}

std::string trimAscii(std::string text) {
    const auto isSpace = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!text.empty() && isSpace(static_cast<unsigned char>(text.front()))) text.erase(text.begin());
    while (!text.empty() && isSpace(static_cast<unsigned char>(text.back()))) text.pop_back();
    return text;
}

float parseFiniteFloat(const wxTextCtrl* control, const std::string& fieldName) {
    if (!control) throw std::runtime_error(fieldName + " control is unavailable.");
    const std::string text = trimAscii(wxui::toStd(control->GetValue()));
    if (text.empty()) throw std::invalid_argument(fieldName + " requires a decimal value.");
    const float value = neogff::ParseFloatDecimal(text);
    if (!std::isfinite(value)) throw std::invalid_argument(fieldName + " must be a finite number.");
    return value;
}

std::optional<float> parseOptionalFiniteFloat(const wxTextCtrl* control, const std::string& fieldName) {
    if (!control) return std::nullopt;
    if (trimAscii(wxui::toStd(control->GetValue())).empty()) return std::nullopt;
    return parseFiniteFloat(control, fieldName);
}

std::uint32_t parseStrRef(const wxString& value) {
    const std::string text = wxui::toStd(value);
    if (text.empty() || text == "-1") return 0xFFFFFFFFu;
    return neogff::ParseUInt32Decimal(text);
}

void setBoolControl(wxCheckBox* control, const std::string& value) {
    if (control) control->SetValue(!value.empty() && value != "0");
}

std::string boolText(const wxCheckBox* control) {
    return control && control->GetValue() ? "1" : "0";
}

std::string lowerAsciiValue(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::int32_t indexedComboValue(const wxComboBox* control, const std::string& fieldName) {
    if (!control) throw std::runtime_error(fieldName + " control is unavailable.");
    std::string text = trimAscii(wxui::toStd(control->GetValue()));
    const std::size_t separator = text.find(':');
    if (separator != std::string::npos) text = trimAscii(text.substr(0, separator));
    if (text.empty()) throw std::invalid_argument("Select a value for " + fieldName + ".");
    return neogff::ParseInt32Decimal(text);
}

std::string jadeParticipantText(std::int32_t index,
                                const std::vector<std::string>& tags,
                                bool includeUnassigned) {
    if (index == -1) return "-1: Conversation/camera owner";
    if (index == -2) return "-2: Runtime default participant";
    if (includeUnassigned && index == std::numeric_limits<std::int32_t>::max()) {
        return std::to_string(index) + ": Unassigned/default";
    }
    if (index >= 0 && static_cast<std::size_t>(index) < tags.size()) {
        return std::to_string(index) + ": " + tags[static_cast<std::size_t>(index)];
    }
    return std::to_string(index) + ": Unknown stored participant index";
}

void populateJadeParticipantCombo(wxComboBox* control,
                                  const std::vector<std::string>& tags,
                                  std::int32_t current,
                                  bool includeUnassigned) {
    if (!control) return;
    control->Clear();
    control->Append(wxui::toWx(jadeParticipantText(-1, tags, includeUnassigned)));
    control->Append(wxui::toWx(jadeParticipantText(-2, tags, includeUnassigned)));
    if (includeUnassigned) {
        control->Append(wxui::toWx(jadeParticipantText(
            std::numeric_limits<std::int32_t>::max(), tags, true)));
    }
    for (std::size_t i = 0; i < tags.size(); ++i) {
        control->Append(wxui::toWx(jadeParticipantText(static_cast<std::int32_t>(i), tags, includeUnassigned)));
    }

    const wxString wanted = wxui::toWx(jadeParticipantText(current, tags, includeUnassigned));
    const int existing = control->FindString(wanted);
    if (existing == wxNOT_FOUND) {
        control->Append(wanted);
        control->SetSelection(static_cast<int>(control->GetCount() - 1));
    } else {
        control->SetSelection(existing);
    }
}

std::string singleLineText(std::string text) {
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }

    std::string result;
    result.reserve(text.size());
    bool previousSpace = false;
    for (unsigned char ch : text) {
        const bool isSpace = std::isspace(ch) != 0;
        if (isSpace) {
            if (!result.empty() && !previousSpace) result.push_back(' ');
        } else {
            result.push_back(static_cast<char>(ch));
        }
        previousSpace = isSpace;
    }
    if (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

class AnimationEditDialog final : public wxDialog {
public:
    AnimationEditDialog(wxWindow* parent,
                        bool jade,
                        DlgNodeKind nodeKind,
                        const std::vector<std::string>& speakerTags,
                        const DlgAnimation& initial)
        : wxDialog(parent, wxID_ANY, "Dialogue Animation", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          jade_(jade),
          jadeReply_(jade && nodeKind == DlgNodeKind::Reply) {
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* form = new wxFlexGridSizer(2, 8, 8);
        form->AddGrowableCol(1, 1);

        if (jade_ && !jadeReply_) {
            participantLabel_ = new wxStaticText(this, wxID_ANY, "Participant:");
            form->Add(participantLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            participantChoice_ = new wxChoice(this, wxID_ANY);
            appendParticipantChoice(-1, "Conversation/camera owner");
            appendParticipantChoice(-2, "Runtime default participant");
            appendParticipantChoice(std::numeric_limits<std::int32_t>::max(),
                                    "Default/unassigned participant");
            for (std::size_t i = 0; i < speakerTags.size(); ++i) {
                appendParticipantChoice(static_cast<std::int32_t>(i), speakerTags[i]);
            }
            int selection = wxNOT_FOUND;
            for (std::size_t i = 0; i < participantValues_.size(); ++i) {
                if (participantValues_[i] == initial.participantIndex) {
                    selection = static_cast<int>(i);
                    break;
                }
            }
            if (selection == wxNOT_FOUND) {
                appendParticipantChoice(initial.participantIndex, "Unknown participant index (preserve until changed)");
                selection = static_cast<int>(participantValues_.size() - 1);
            }
            participantChoice_->SetSelection(selection);
            form->Add(participantChoice_, 1, wxEXPAND);
        } else if (!jade_) {
            participant_ = addTextField(this, form, "Participant:");
            participant_->ChangeValue(wxui::toWx(initial.participant));
        }

        animation_ = addTextField(this, form, jadeReply_ ? "Reply animation ID:" : "Animation ID:");
        animation_->ChangeValue(wxString::Format("%d", initial.animation));

        if (jade_) {
            emotion_ = addTextField(this, form, "Emotion ID:");
            emotion_->ChangeValue(wxString::Format("%d", initial.emotion));
        }

        if (jadeReply_) {
            auto* note = new wxStaticText(
                this, wxID_ANY,
                "Jade Empire Reply nodes store one Animation/Emotion pair. Animation 65535 is the unset value.");
            note->Wrap(FromDIP(460));
            root->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);
        }

        root->Add(form, 1, wxEXPAND | wxALL, 12);
        root->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        SetSizerAndFit(root);
        wxui::configureResponsiveWindow(*this, wxSize(560, 320), wxSize(420, 220));
        CentreOnParent();
        wxui::constrainWindowToDisplay(*this);
    }

    DlgAnimation value() const {
        DlgAnimation result;
        if (jade_ && !jadeReply_) {
            const int selection = participantChoice_ ? participantChoice_->GetSelection() : wxNOT_FOUND;
            if (selection == wxNOT_FOUND || static_cast<std::size_t>(selection) >= participantValues_.size()) {
                throw std::invalid_argument("Select a Jade Empire animation participant.");
            }
            result.participantIndex = participantValues_[static_cast<std::size_t>(selection)];
        } else if (!jade_ && participant_) {
            result.participant = wxui::toStd(participant_->GetValue());
        }
        result.animation = neogff::ParseInt32Decimal(wxui::toStd(animation_->GetValue()));
        result.emotion = emotion_ ? neogff::ParseInt32Decimal(wxui::toStd(emotion_->GetValue())) : 0;
        return result;
    }

private:
    void appendParticipantChoice(std::int32_t value, const std::string& label) {
        participantValues_.push_back(value);
        participantChoice_->Append(wxString::Format("%d: ", value) + wxui::toWx(label));
    }

    bool jade_ = false;
    bool jadeReply_ = false;
    wxStaticText* participantLabel_ = nullptr;
    wxTextCtrl* participant_ = nullptr;
    wxChoice* participantChoice_ = nullptr;
    std::vector<std::int32_t> participantValues_;
    wxTextCtrl* animation_ = nullptr;
    wxTextCtrl* emotion_ = nullptr;
};

constexpr int kMoveParticipantUpId = wxID_HIGHEST + 12980;
constexpr int kMoveParticipantDownId = wxID_HIGHEST + 12981;

class ConversationPropertiesDialog final : public wxDialog {
public:
    ConversationPropertiesDialog(wxWindow* parent, const DlgDocument& document)
        : wxDialog(parent, wxID_ANY, "Conversation Properties", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          jade_(document.dialect() == DlgDialect::JadeEmpire) {
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* book = new wxNotebook(this, wxID_ANY);
        root->Add(book, 1, wxEXPAND | wxALL, 10);

        if (!jade_) {
            auto* basics = new wxScrolledWindow(book, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
            basics->SetScrollRate(0, FromDIP(10));
            auto* pageSizer = new wxBoxSizer(wxVERTICAL);
            auto* form = new wxFlexGridSizer(2, 8, 8);
            form->AddGrowableCol(1, 1);

            addRootText(document, basics, form, "EndConversation", "End conversation script:");
            addRootText(document, basics, form, "EndConverAbort", "Abort conversation script:");
            addRootText(document, basics, form, "CameraModel", "Camera model:");
            addRootText(document, basics, form, "AmbientTrack", "Ambient track:");
            addRootText(document, basics, form, "VO_ID", "Voice-over ID:");
            addRootText(document, basics, form, "DelayEntry", "Entry delay:");
            addRootText(document, basics, form, "DelayReply", "Reply delay:");

            form->Add(new wxStaticText(basics, wxID_ANY, "Conversation type:"),
                      0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
            conversationType_ = new wxChoice(basics, wxID_ANY);
            populateIntegerChoice(conversationType_, conversationTypeValues_, kConversationTypeOptions,
                                  document.rootField("ConversationType"), 0,
                                  "Unknown value (preserve until changed)");
            form->Add(conversationType_, 1, wxEXPAND);

            computerType_ = addRootText(document, basics, form, "ComputerType", "Computer type:");
            conversationType_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { updateConversationTypeControls(); });
            updateConversationTypeControls();
            if (document.hasRootField("NextNodeID")) addRootText(document, basics, form, "NextNodeID", "Next node ID:");
            if (document.hasRootField("PostProcOwner")) addRootText(document, basics, form, "PostProcOwner", "Post-process owner:");
            if (document.hasRootField("AlienRaceOwner")) addRootText(document, basics, form, "AlienRaceOwner", "Alien-race owner:");
            if (document.hasRootField("RecordNoVO")) addRootText(document, basics, form, "RecordNoVO", "Record no-VO mode:");

            addRootCheck(document, basics, form, "Skippable", "Conversation is skippable");
            addRootCheck(document, basics, form, "AnimatedCut", "Animated cutscene");
            addRootCheck(document, basics, form, "UnequipItems", "Unequip items");
            addRootCheck(document, basics, form, "UnequipHItem", "Unequip hand item");

            pageSizer->Add(form, 1, wxEXPAND | wxALL, 12);
            basics->SetSizer(pageSizer);
            book->AddPage(basics, "General", true);

            auto* stuntPage = new wxPanel(book);
            auto* stuntSizer = new wxBoxSizer(wxVERTICAL);
            stunts_ = new wxListCtrl(stuntPage, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                     wxLC_REPORT | wxLC_SINGLE_SEL);
            stunts_->InsertColumn(0, "Participant");
            stunts_->InsertColumn(1, "Stunt model");
            stunts_->SetColumnWidth(0, FromDIP(180));
            stunts_->SetColumnWidth(1, FromDIP(260));
            stuntValues_ = document.stunts();
            refreshStunts();
            stuntSizer->Add(stunts_, 1, wxEXPAND | wxALL, 10);
            auto* buttons = new wxBoxSizer(wxHORIZONTAL);
            auto* add = new wxButton(stuntPage, wxID_ADD, "Add...");
            auto* edit = new wxButton(stuntPage, wxID_EDIT, "Edit...");
            auto* remove = new wxButton(stuntPage, wxID_DELETE, "Delete");
            buttons->Add(add, 0, wxRIGHT, 6);
            buttons->Add(edit, 0, wxRIGHT, 6);
            buttons->Add(remove, 0);
            stuntSizer->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
            stuntPage->SetSizer(stuntSizer);
            book->AddPage(stuntPage, "Cutscene Models", false);
            add->Bind(wxEVT_BUTTON, &ConversationPropertiesDialog::onAddStunt, this);
            edit->Bind(wxEVT_BUTTON, &ConversationPropertiesDialog::onEditStunt, this);
            remove->Bind(wxEVT_BUTTON, &ConversationPropertiesDialog::onDeleteStunt, this);
        } else {
            auto* basics = new wxScrolledWindow(book, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
            basics->SetScrollRate(0, FromDIP(10));
            auto* pageSizer = new wxBoxSizer(wxVERTICAL);
            auto* form = new wxFlexGridSizer(2, 8, 8);
            form->AddGrowableCol(1, 1);
            jadeEndConversationPresent_ = document.hasRootField("EndConversation");
            jadeEndConversation_ = addRootText(
                document, basics, form, "EndConversation", "End conversation script:");
            jadeEndConversation_->SetToolTip(
                "Jade Empire CResRef script run when the conversation ends.");
            pageSizer->Add(form, 0, wxEXPAND | wxALL, 12);
            basics->SetSizer(pageSizer);
            book->AddPage(basics, "General", true);

            auto* tagPage = new wxPanel(book);
            auto* tagSizer = new wxBoxSizer(wxVERTICAL);
            auto* tagNote = new wxStaticText(
                tagPage, wxID_ANY,
                "Jade Empire SpeakerIndex, ListenerIndex, and Entry-animation Index values refer to this TagList. "
                "Tags are stored lowercase.");
            tagNote->Wrap(FromDIP(560));
            tagSizer->Add(tagNote, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);
            tags_ = new wxListCtrl(tagPage, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                   wxLC_REPORT | wxLC_SINGLE_SEL);
            tags_->InsertColumn(0, "Index");
            tags_->InsertColumn(1, "Speaker tag");
            tags_->SetColumnWidth(0, FromDIP(80));
            tags_->SetColumnWidth(1, FromDIP(360));
            tagValues_ = document.speakerTags();
            refreshTags();
            tagSizer->Add(tags_, 1, wxEXPAND | wxALL, 10);
            auto* buttons = new wxBoxSizer(wxHORIZONTAL);
            auto* add = new wxButton(tagPage, wxID_ADD, "Add...");
            auto* edit = new wxButton(tagPage, wxID_EDIT, "Edit...");
            auto* remove = new wxButton(tagPage, wxID_DELETE, "Delete");
            buttons->Add(add, 0, wxRIGHT, 6);
            buttons->Add(edit, 0, wxRIGHT, 6);
            buttons->Add(remove, 0);
            tagSizer->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
            tagPage->SetSizer(tagSizer);
            book->AddPage(tagPage, "Participants", false);
            add->Bind(wxEVT_BUTTON, &ConversationPropertiesDialog::onAddTag, this);
            edit->Bind(wxEVT_BUTTON, &ConversationPropertiesDialog::onEditTag, this);
            remove->Bind(wxEVT_BUTTON, &ConversationPropertiesDialog::onDeleteTag, this);
            Bind(wxEVT_MENU, [this](wxCommandEvent&) { moveTag(-1); }, kMoveParticipantUpId);
            Bind(wxEVT_MENU, [this](wxCommandEvent&) { moveTag(1); }, kMoveParticipantDownId);
            tags_->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, [this](wxListEvent& event) {
                const long row = event.GetIndex();
                if (row < 0 || static_cast<std::size_t>(row) >= tagValues_.size()) return;
                tags_->SetItemState(row, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                    wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
                wxMenu menu;
                auto* moveUp = menu.Append(kMoveParticipantUpId, "Move Participant Up");
                auto* moveDown = menu.Append(kMoveParticipantDownId, "Move Participant Down");
                moveUp->Enable(row > 0);
                moveDown->Enable(static_cast<std::size_t>(row + 1) < tagValues_.size());
                tags_->PopupMenu(&menu);
            });
        }

        root->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        SetSizer(root);
        wxui::configureResponsiveWindow(*this, wxSize(760, 620), wxSize(560, 400));
        CentreOnParent();
        wxui::constrainWindowToDisplay(*this);
    }

    void apply(DlgDocument& document) const {
        if (jade_) {
            const std::string endConversation = jadeEndConversation_
                ? trimAscii(wxui::toStd(jadeEndConversation_->GetValue()))
                : std::string{};
            if (jadeEndConversationPresent_ || !endConversation.empty()) {
                document.setRootField("EndConversation", FIELD_TYPE_RESREF, endConversation);
            }
            document.replaceSpeakerTags(tagValues_);
            return;
        }
        document.setRootField("ConversationType", FIELD_TYPE_INT,
                              selectedIntegerChoice(conversationType_, conversationTypeValues_, "conversation type"));
        for (const auto& item : rootText_) {
            const std::string value = wxui::toStd(item.second->GetValue());
            std::uint32_t type = FIELD_TYPE_CEXOSTRING;
            if (item.first == "EndConversation" || item.first == "EndConverAbort" ||
                item.first == "CameraModel" || item.first == "AmbientTrack") {
                type = FIELD_TYPE_RESREF;
            } else if (item.first == "DelayEntry" || item.first == "DelayReply") {
                type = FIELD_TYPE_DWORD;
            } else if (item.first == "ComputerType") {
                type = FIELD_TYPE_BYTE;
            } else if (item.first == "NextNodeID" ||
                       item.first == "PostProcOwner" || item.first == "AlienRaceOwner" ||
                       item.first == "RecordNoVO") {
                type = FIELD_TYPE_INT;
            }
            document.setRootField(item.first, type, value.empty() && type != FIELD_TYPE_CEXOSTRING && type != FIELD_TYPE_RESREF ? "0" : value);
        }
        for (const auto& item : rootChecks_) document.setRootField(item.first, FIELD_TYPE_BYTE, boolText(item.second));
        document.replaceStunts(stuntValues_);
    }

private:
    wxTextCtrl* addRootText(const DlgDocument& document,
                            wxWindow* page,
                            wxFlexGridSizer* form,
                            const std::string& field,
                            const wxString& label) {
        wxTextCtrl* control = addTextField(page, form, label);
        control->ChangeValue(wxui::toWx(document.rootField(field)));
        rootText_[field] = control;
        return control;
    }

    void updateConversationTypeControls() {
        if (!computerType_ || !conversationType_) return;
        const std::string value = selectedIntegerChoice(conversationType_, conversationTypeValues_, "conversation type");
        computerType_->Enable(value == "1");
    }

    void addRootCheck(const DlgDocument& document,
                      wxWindow* page,
                      wxFlexGridSizer* form,
                      const std::string& field,
                      const wxString& label) {
        wxCheckBox* control = addCheckField(page, form, label);
        setBoolControl(control, document.rootField(field));
        rootChecks_[field] = control;
    }

    long selectedRow(wxListCtrl* list) const {
        return list ? list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) : -1;
    }

    void refreshStunts() {
        if (!stunts_) return;
        stunts_->DeleteAllItems();
        for (std::size_t i = 0; i < stuntValues_.size(); ++i) {
            const long row = stunts_->InsertItem(static_cast<long>(i), wxui::toWx(stuntValues_[i].participant));
            stunts_->SetItem(row, 1, wxui::toWx(stuntValues_[i].model));
        }
    }

    void onAddStunt(wxCommandEvent&) {
        const auto participant = wxui::promptText(this, "Add Cutscene Model", "Participant:", "OWNER");
        if (!participant) return;
        const auto model = wxui::promptText(this, "Add Cutscene Model", "Stunt model resref:", "");
        if (!model) return;
        stuntValues_.push_back({*participant, *model});
        refreshStunts();
    }

    void onEditStunt(wxCommandEvent&) {
        const long row = selectedRow(stunts_);
        if (row < 0 || static_cast<std::size_t>(row) >= stuntValues_.size()) return;
        auto participant = wxui::promptText(this, "Edit Cutscene Model", "Participant:", stuntValues_[row].participant);
        if (!participant) return;
        auto model = wxui::promptText(this, "Edit Cutscene Model", "Stunt model resref:", stuntValues_[row].model);
        if (!model) return;
        stuntValues_[row] = {*participant, *model};
        refreshStunts();
        stunts_->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }

    void onDeleteStunt(wxCommandEvent&) {
        const long row = selectedRow(stunts_);
        if (row < 0 || static_cast<std::size_t>(row) >= stuntValues_.size()) return;
        stuntValues_.erase(stuntValues_.begin() + row);
        refreshStunts();
    }

    void refreshTags() {
        if (!tags_) return;
        tags_->DeleteAllItems();
        for (std::size_t i = 0; i < tagValues_.size(); ++i) {
            const long row = tags_->InsertItem(static_cast<long>(i), wxString::Format("%zu", i));
            tags_->SetItem(row, 1, wxui::toWx(tagValues_[i]));
        }
    }

    void onAddTag(wxCommandEvent&) {
        const auto tag = wxui::promptText(this, "Add Speaker", "Speaker tag:", "");
        if (!tag) return;
        const std::string normalized = lowerAsciiValue(trimAscii(*tag));
        if (normalized.empty()) {
            wxui::showMessage(this, "Add Participant", "Participant tags cannot be empty.");
            return;
        }
        tagValues_.push_back(normalized);
        refreshTags();
    }

    void onEditTag(wxCommandEvent&) {
        const long row = selectedRow(tags_);
        if (row < 0 || static_cast<std::size_t>(row) >= tagValues_.size()) return;
        const auto tag = wxui::promptText(this, "Edit Speaker", "Speaker tag:", tagValues_[row]);
        if (!tag) return;
        const std::string normalized = lowerAsciiValue(trimAscii(*tag));
        if (normalized.empty()) {
            wxui::showMessage(this, "Edit Participant", "Participant tags cannot be empty.");
            return;
        }
        tagValues_[row] = normalized;
        refreshTags();
        tags_->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }

    void onDeleteTag(wxCommandEvent&) {
        const long row = selectedRow(tags_);
        if (row < 0 || static_cast<std::size_t>(row) >= tagValues_.size()) return;
        tagValues_.erase(tagValues_.begin() + row);
        refreshTags();
    }

    void moveTag(int delta) {
        const long row = selectedRow(tags_);
        const long target = row + delta;
        if (row < 0 || target < 0 ||
            static_cast<std::size_t>(target) >= tagValues_.size()) {
            return;
        }
        std::swap(tagValues_[static_cast<std::size_t>(row)],
                  tagValues_[static_cast<std::size_t>(target)]);
        refreshTags();
        tags_->SetItemState(target, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                            wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    }

    bool jade_ = false;
    bool jadeEndConversationPresent_ = false;
    wxTextCtrl* jadeEndConversation_ = nullptr;
    wxChoice* conversationType_ = nullptr;
    wxTextCtrl* computerType_ = nullptr;
    std::vector<std::string> conversationTypeValues_;
    std::map<std::string, wxTextCtrl*> rootText_;
    std::map<std::string, wxCheckBox*> rootChecks_;
    wxListCtrl* stunts_ = nullptr;
    wxListCtrl* tags_ = nullptr;
    std::vector<DlgStunt> stuntValues_;
    std::vector<std::string> tagValues_;
};

class ValidationDialog final : public wxDialog {
public:
    ValidationDialog(wxWindow* parent,
                     const DlgDocument& document,
                     std::vector<DlgIssue> issues)
        : wxDialog(parent, wxID_ANY, "Dialogue Validation", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          issues_(std::move(issues)) {
        auto* root = new wxBoxSizer(wxVERTICAL);
        const DlgStatistics stats = document.statistics();
        const std::string summary =
            std::to_string(stats.entries) + " entries, " +
            std::to_string(stats.replies) + " replies, " +
            std::to_string(stats.totalLinks) + " links, " +
            std::to_string(stats.unreachableNodes) + " unreachable nodes.";
        root->Add(new wxStaticText(this, wxID_ANY, wxui::toWx(summary)), 0, wxEXPAND | wxALL, 10);

        list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                               wxLC_REPORT | wxLC_SINGLE_SEL);
        list_->InsertColumn(0, "Severity");
        list_->InsertColumn(1, "Location");
        list_->InsertColumn(2, "Message");
        list_->SetColumnWidth(0, FromDIP(90));
        list_->SetColumnWidth(1, FromDIP(150));
        list_->SetColumnWidth(2, FromDIP(560));
        for (std::size_t i = 0; i < issues_.size(); ++i) {
            const DlgIssue& issue = issues_[i];
            const long row = list_->InsertItem(static_cast<long>(i), wxui::toWx(issueSeverityName(issue.severity)));
            std::string location;
            if (issue.node) location = document.nodeKindName(issue.node->kind) + " " + std::to_string(issue.node->index);
            else if (issue.link) location = "Link " + std::to_string(issue.link->position);
            list_->SetItem(row, 1, wxui::toWx(location));
            list_->SetItem(row, 2, wxui::toWx(issue.message));
        }
        if (issues_.empty()) {
            const long row = list_->InsertItem(0, "OK");
            list_->SetItem(row, 2, "No structural dialogue problems were found.");
        }
        root->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        root->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        SetSizer(root);
        wxui::configureResponsiveWindow(*this, wxSize(980, 620), wxSize(620, 380));
        CentreOnParent();
        wxui::constrainWindowToDisplay(*this);
    }

    std::optional<DlgIssue> selectedIssue() const {
        if (!list_) return std::nullopt;
        const long row = list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (row < 0 || static_cast<std::size_t>(row) >= issues_.size()) return std::nullopt;
        return issues_[static_cast<std::size_t>(row)];
    }

private:
    wxListCtrl* list_ = nullptr;
    std::vector<DlgIssue> issues_;
};

enum class ConversationTreeKind {
    ConversationRoot,
    Group,
    Node,
    InvalidLink,
};

class ConversationTreeData final : public wxTreeItemData {
public:
    ConversationTreeData(ConversationTreeKind itemKind,
                         std::optional<DlgNodeRef> nodeRef = std::nullopt,
                         std::optional<DlgLinkRef> linkRef = std::nullopt,
                         bool referenceOnly = false,
                         std::string stableKey = {})
        : kind(itemKind), node(std::move(nodeRef)), link(std::move(linkRef)),
          reference(referenceOnly), key(std::move(stableKey)) {}

    ConversationTreeKind kind = ConversationTreeKind::Group;
    std::optional<DlgNodeRef> node;
    std::optional<DlgLinkRef> link;
    bool reference = false;
    std::string key;
};

std::string conversationNodeKey(DlgNodeRef ref) {
    return std::string(ref.kind == DlgNodeKind::Entry ? "entry:" : "reply:") +
           std::to_string(ref.index);
}

std::string conversationLinkKey(DlgLinkRef ref) {
    char owner = 'S';
    if (ref.owner == DlgLinkOwner::Entry) owner = 'E';
    else if (ref.owner == DlgLinkOwner::Reply) owner = 'R';
    return std::string("link:") + owner + ":" + std::to_string(ref.ownerIndex) +
           ":" + std::to_string(ref.position);
}


std::string gffTreeParentPath(std::string path) {
    const auto suffix = path.find('(');
    if (suffix != std::string::npos) return path.substr(0, suffix);
    const auto pos = path.find_last_of('\\');
    return pos == std::string::npos ? std::string{} : path.substr(0, pos);
}

std::string gffTreePathLeaf(std::string path) {
    const auto suffix = path.find('(');
    if (suffix != std::string::npos) path = path.substr(0, suffix);
    const auto pos = path.find_last_of('\\');
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string gffTreeEllipsize(std::string text, std::size_t maxChars = 96) {
    for (char& ch : text) {
        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
    }
    if (text.size() <= maxChars) return text;
    text.resize(maxChars > 3 ? maxChars - 3 : maxChars);
    if (maxChars > 3) text += "...";
    return text;
}

std::string gffTreeText(const GffFieldRow& row) {
    std::string name = row.label.empty() || row.label == "(empty)" ? gffTreePathLeaf(row.path) : row.label;
    if (name.empty()) name = row.path.empty() ? std::string("Main Struct") : row.path;
    std::string text = name;
    if (!row.type.empty()) text += " [" + row.type + "]";
    if (!row.value.empty()) text += " " + gffTreeEllipsize(row.value);
    if (!row.resolved.empty()) text += " -> " + gffTreeEllipsize(row.resolved);
    return text;
}

class RawGffTreeItemData final : public wxTreeItemData {
public:
    RawGffTreeItemData(std::string path, int rowIndex)
        : path_(std::move(path)), rowIndex_(rowIndex) {}

    const std::string& path() const noexcept { return path_; }
    int rowIndex() const noexcept { return rowIndex_; }

private:
    std::string path_;
    int rowIndex_ = -1;
};

enum : int {
    ID_NewK1 = wxID_HIGHEST + 13000,
    ID_NewK2,
    ID_NewJade,
    ID_Open,
    ID_Save,
    ID_SaveAs,
    ID_DialogueInformation,
    ID_OpenTlk,
    ID_ClearTlk,
    ID_CloseTab,
    ID_CloseOtherTabs,
    ID_NextTab,
    ID_PreviousTab,
    ID_Undo,
    ID_Redo,
    ID_AddStartingEntry,
    ID_AddChild,
    ID_LinkExisting,
    ID_DuplicateNode,
    ID_RemoveLink,
    ID_DeleteNode,
    ID_MoveLinkUp,
    ID_MoveLinkDown,
    ID_ConversationProperties,
    ID_Validate,
    ID_FindNext,
    ID_ImportXml,
    ID_ImportJson,
    ID_ExportXml,
    ID_ExportJson,
    ID_ExportPatcher,
    ID_ViewConversation,
    ID_ViewSinglePanel,
    ID_ViewRaw,
    ID_DarkMode,
    ID_FontIncrease,
    ID_FontDecrease,
    ID_FontReset,
    ID_DocumentTabs,
    ID_Workspace,
    ID_ConversationTree,
    ID_RawTree,
    ID_ApplyNode,
    ID_ApplyScripts,
    ID_ApplyPresentation,
    ID_ApplyLink,
    ID_AnimationAdd,
    ID_AnimationEdit,
    ID_AnimationDelete,
    ID_AnimationUp,
    ID_AnimationDown,
};

enum class WorkspaceView {
    Conversation,
    SinglePanel,
    Raw,
};

constexpr int ID_ModuleExit = wxID_HIGHEST + 13450;
constexpr int ID_ModuleAbout = wxID_HIGHEST + 13451;
constexpr int kRecentFileBaseId = wxID_HIGHEST + 13500;
constexpr int kClearRecentFilesId = kRecentFileBaseId + neosettings::kMaxRecentFiles;

class NeoDLGPanelImpl final : public neodlg::ui::DLGEditorPanel {
public:
    NeoDLGPanelImpl(wxWindow* parent, neomodules::Context context)
        : DLGEditorPanel(parent, std::move(context)),
          settings_(kAppName) {
        buildMenus();
        buildWindow();
        createModuleStatusBar(2);
        darkMode_ = wxui::readDarkMode(kAppName);
        fontScale_ = settings_.fontScale();
        if (!context_.embedded) fontScaleWheelFilter_.attach(this, [this](int steps) { changeFontScaleSteps(steps); });
        neoview::bindFontScaleDpiRefresh(this, [this]() { applyFontScale(); });
        createDocumentTab(true);
        tryLoadCachedTlk();
        applyDarkMode();
        refreshAll();
    }



    bool activateResource(const std::string& identity) override {
        if (identity.empty()) return false;
        for (std::size_t i=0; i<documents_.size(); ++i)
            if (documents_[i].resourceIdentity == identity) { selectDocumentTab(i); return true; }
        return false;
    }
    std::size_t documentCount() const override { return documents_.size(); }
    bool openFile(const std::filesystem::path& path) override { return openModelPath(path); }
    neogff::GffModel* activeModel() override { return hasActiveDocument() ? &model() : nullptr; }
    void refreshActiveDocument() override { if (hasActiveDocument()) refreshAll(); }
    void setAppearance(bool dark, double scale) override { darkMode_=dark; fontScale_=scale; applyDarkMode(); }
    bool canClose() override {
        if (browserSaveActive_) return false;
        for (std::size_t i=0; i<documents_.size(); ++i)
            if (!confirmCloseDocument(i)) return false;
        return true;
    }
    std::vector<std::filesystem::path> openPaths() const override {
        std::vector<std::filesystem::path> result;
        for (const auto& document : documents_) {
            const auto path = documentFilename(document);
            if (!path.empty()) result.push_back(path);
        }
        return result;
    }
    bool openResource(neoshared::ResourceDocument input) override {
        if (activateResource(input.identity)) return true;
        auto candidate = std::make_unique<GffModel>();
        neoshared::loadGffResource(input, candidate->gff(), "DLG ");
        removeRetiredDlgFields(*candidate);
        // Parse before creating/replacing a tab, so a failed load leaves the UI intact.
        ensureTabForOpen();
        auto& document = activeDocument();
        document.model = std::move(candidate);
        document.logicalFilename.clear();
        document.resourceIdentity = std::move(input.identity);
        document.sourceDescription = std::move(input.sourceDescription);
        document.protectedInputs = std::move(input.protectedInputs);
        document.untitledName = std::move(input.fileName);
#if defined(__EMSCRIPTEN__)
        document.sourceImport.reset();
#endif
        document.undo.clear(); document.redo.clear();
        document.selectedNode.reset(); document.selectedLink.reset();
        document.conversationTreeState.reset(); document.rawTreeState.reset();
        document.rawFilterTerm.clear();
        conversationTreeRenderedDocumentPage_ = nullptr; rawTreeRenderedDocumentPage_ = nullptr;
        if (rawFilter_) rawFilter_->ChangeValue(wxString{});
        tryLoadCachedTlk();
        setWorkspaceView(dialogue().semanticallyEditable()
                             ? WorkspaceView::Conversation
                             : WorkspaceView::Raw);

        for (const auto& source : document.protectedInputs) {
            tryLoadResolvedTlkForPath(source);
            if (model().tlk().loaded()) break;
        }
        refreshAll();
        setModuleStatusText("Archive snapshot: " + document.sourceDescription + ". Save As creates a separate working file.");
        return true;
    }
    bool saveActiveAs(const std::filesystem::path& path) override {
        if (!hasActiveDocument() || !model().gff().loaded() || path.empty()) return false;
        checkDestination(path);
        return save(false, path);
    }

private:
    void checkOutput(const std::filesystem::path& path, bool exporting = false) const {
        validateHostOutput(path);
        for (const auto& document : documents_) {
            neoshared::checkResourceOutput(path, document.protectedInputs);
            if ((exporting || &document != &activeDocument()) &&
                neoshared::sameResourcePath(path, documentFilename(document)))
                throw std::runtime_error("That destination belongs to an open document. Choose a separate working file.");
        }
    }
    void checkDestination(const std::filesystem::path& path) const {
        checkOutput(path);
        neoshared::checkGffOutputType(path, ".dlg");
    }

    struct UndoSnapshot {
        std::string description;
        std::string xml;
    };

    struct DocumentTab {
        std::unique_ptr<GffModel> model = std::make_unique<GffModel>();
        std::filesystem::path logicalFilename;
        std::string resourceIdentity;
        std::string sourceDescription;
        std::vector<std::filesystem::path> protectedInputs;
        std::string untitledName = "Untitled DLG";
        std::string tlkAutoLoadWarning;
        wxWindow* tabPage = nullptr;
        bool saveInProgress = false;
#if defined(__EMSCRIPTEN__)
        neobrowser::BrowserImportLease sourceImport;
#endif
        WorkspaceView workspaceView = WorkspaceView::Conversation;
        WorkspaceView semanticView = WorkspaceView::Conversation;
        std::optional<DlgNodeRef> selectedNode;
        std::optional<DlgLinkRef> selectedLink;
        neotree::TreeViewState conversationTreeState;
        neotree::TreeViewState rawTreeState;
        std::string rawFilterTerm;
        std::vector<UndoSnapshot> undo;
        std::vector<UndoSnapshot> redo;
    };

    struct InspectorSection {
        wxScrolledWindow* tabPage = nullptr;
        wxBoxSizer* tabSizer = nullptr;
        wxPanel* content = nullptr;
        wxPanel* singleHost = nullptr;
        wxStaticBoxSizer* singleSizer = nullptr;
    };

    struct DialogueSummary {
        wxString file;
        wxString type;
        wxString statistics;
        wxString tlk;
        wxString warning;
    };

    bool hasActiveDocument() const {
        return activeDocumentIndex_ != neotabs::npos && activeDocumentIndex_ < documents_.size();
    }

    DocumentTab& activeDocument() { return documents_.at(activeDocumentIndex_); }
    const DocumentTab& activeDocument() const { return documents_.at(activeDocumentIndex_); }
    GffModel& model() { return *activeDocument().model; }
    const GffModel& model() const { return *activeDocument().model; }
    DlgDocument dialogue() { return DlgDocument(model()); }
    DlgDocument dialogue() const { return DlgDocument(model()); }

    std::filesystem::path documentFilename(const DocumentTab& tab) const {
        if (!tab.logicalFilename.empty()) return tab.logicalFilename;
        return tab.model ? tab.model->filename() : std::filesystem::path{};
    }

    bool tabDirty(const DocumentTab& tab) const {
        return tab.saveInProgress || (tab.model && tab.model->dirty());
    }

    std::string tabDisplayName(const DocumentTab& tab) const {
        return neotabs::displayNameForPath(documentFilename(tab), tab.untitledName);
    }

    DialogueSummary currentDialogueSummary() const {
        DialogueSummary summary;
        if (!hasActiveDocument() || !model().loaded()) {
            summary.file = "No DLG loaded";
            summary.type = "None";
            summary.statistics = "No conversation data";
            summary.tlk = "none";
            return summary;
        }

        const auto path = documentFilename(activeDocument());
        if (!path.empty()) {
            summary.file = neosettings::pathToWx(path);
        } else if (!activeDocument().sourceDescription.empty()) {
            summary.file = wxui::toWx(activeDocument().sourceDescription);
        } else {
            summary.file = wxui::toWx(tabDisplayName(activeDocument()));
        }

        const DlgDocument document = dialogue();
        summary.type = wxui::toWx(trimHeader(model().fileType()) + " " +
                                  trimHeader(model().version()) + " - " +
                                  dialectName(document.dialect()));
        if (document.semanticallyEditable()) {
            const DlgStatistics stats = document.statistics();
            summary.statistics = wxui::toWx(
                std::to_string(stats.entries) + " entries, " +
                std::to_string(stats.replies) + " replies, " +
                std::to_string(stats.totalLinks) + " links");
        } else {
            summary.statistics = "Use GFF Structure Tree for this schema";
        }
        summary.tlk = model().tlk().loaded()
            ? neosettings::pathToWx(model().tlk().filename())
            : wxString("none");
        summary.warning = wxui::toWx(activeDocument().tlkAutoLoadWarning);
        return summary;
    }

    void buildMenus() {
        auto* file = new wxMenu;
        auto* newMenu = new wxMenu;
        newMenu->Append(ID_NewK1, "KotOR DLG");
        newMenu->Append(ID_NewK2, "KotOR II DLG");
        newMenu->Append(ID_NewJade, "Jade Empire DLG");
        file->AppendSubMenu(newMenu, "&New");
        file->Append(ID_Open, "&Open DLG...\tCtrl-O");
        recentFilesMenu_ = new wxMenu;
        rebuildRecentFilesMenu();
        file->AppendSubMenu(recentFilesMenu_, "Open &Recent");
        file->AppendSeparator();
        auto* dialogueFileMenu = new wxMenu;
        dialogueFileMenu->Append(ID_DialogueInformation, "&Information...");
        dialogueFileMenu->AppendSeparator();
        dialogueFileMenu->Append(ID_OpenTlk, "Open optional &TLK...");
        dialogueFileMenu->Append(ID_ClearTlk, "Clear TLK");
        file->AppendSubMenu(dialogueFileMenu, "&Dialogue");
        file->AppendSeparator();
        file->Append(ID_Save, "&Save\tCtrl-S");
        file->Append(ID_SaveAs, "Save &As...");
        file->AppendSeparator();
        file->Append(ID_CloseTab, "&Close Tab\tCtrl-W");
        file->Append(ID_CloseOtherTabs, "Close &Other Tabs");
        file->Append(ID_NextTab, "Next Tab\tCtrl-Tab");
        file->Append(ID_PreviousTab, "Previous Tab\tCtrl-Shift-Tab");
        gameDirectoryMenu_ = neogames::appendOpenGameDirectoryMenu(
            *this, *file, [this](const std::filesystem::path& directory) { chooseAndOpenDlg(directory); });
        file->AppendSeparator();
        if (!context_.embedded) file->Append(ID_ModuleExit, "E&xit");

        auto* edit = new wxMenu;
        undoItem_ = edit->Append(ID_Undo, "&Undo\tCtrl-Z");
        redoItem_ = edit->Append(ID_Redo, "&Redo\tCtrl-Y");
        edit->AppendSeparator();
        edit->Append(ID_AddStartingEntry, "Add Starting &Entry");
        edit->Append(ID_AddChild, "Add &Child Node\tInsert");
        edit->Append(ID_LinkExisting, "Link Existing Node...");
        edit->Append(ID_DuplicateNode, "&Duplicate Node");
        edit->AppendSeparator();
        edit->Append(ID_RemoveLink, "Remove This &Link");
        edit->Append(ID_DeleteNode, "Delete Node &Everywhere");
        edit->AppendSeparator();
        edit->Append(ID_MoveLinkUp, "Move Choice &Up");
        edit->Append(ID_MoveLinkDown, "Move Choice &Down");
        edit->AppendSeparator();
        edit->Append(ID_FindNext, "Find &Next\tF3");

        auto* dialogueMenu = new wxMenu;
        dialogueMenu->Append(ID_ConversationProperties, "Conversation &Properties...");
        dialogueMenu->Append(ID_Validate, "&Validate Dialogue...");

        auto* importMenu = new wxMenu;
        importMenu->Append(ID_ImportXml, "Import XML...");
        importMenu->Append(ID_ImportJson, "Import JSON...");

        auto* exportMenu = new wxMenu;
        exportMenu->Append(ID_ExportXml, "Export XML...");
        exportMenu->Append(ID_ExportJson, "Export JSON...");
        exportMenu->AppendSeparator();
        exportMenu->Append(ID_ExportPatcher, "Export TSL/HoloPatcher Instructions...");

        auto* view = new wxMenu;
        conversationViewItem_ = view->AppendRadioItem(ID_ViewConversation, "Conversation Editor");
        singlePanelViewItem_ = view->AppendRadioItem(ID_ViewSinglePanel, "Single Panel Editor");
        rawViewItem_ = view->AppendRadioItem(ID_ViewRaw, "GFF Structure Tree");
        conversationViewItem_->Check(true);
        view->AppendSeparator();
        if (!context_.embedded) {
        darkModeItem_ = view->AppendCheckItem(ID_DarkMode, "Dark Mode");
        view->AppendSeparator();
        view->Append(ID_FontIncrease, "Increase Text Size\tCtrl++");
        view->Append(ID_FontDecrease, "Decrease Text Size\tCtrl+-");
        view->Append(ID_FontReset, "Reset Text Size\tCtrl+0");
        }

        auto* help = new wxMenu;
        help->Append(ID_ModuleAbout, "&About NeoDLG");

        auto* bar = new wxMenuBar;
        bar->Append(file, "&File");
        bar->Append(edit, "&Edit");
        bar->Append(dialogueMenu, "&Dialogue");
        bar->Append(importMenu, "&Import");
        bar->Append(exportMenu, "E&xport");
        bar->Append(view, "&View");
        if (!context_.embedded) bar->Append(help, "&Help"); else delete help;
        setModuleMenus(bar);

        Bind(wxEVT_MENU, [this](wxCommandEvent&) { newDocument(DlgFlavor::Kotor); }, ID_NewK1);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { newDocument(DlgFlavor::Kotor2); }, ID_NewK2);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { newDocument(DlgFlavor::JadeEmpire); }, ID_NewJade);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onOpen, this, ID_Open);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onOpenRecent, this, kRecentFileBaseId, kClearRecentFilesId);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onSave, this, ID_Save);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onDialogueInformation, this, ID_DialogueInformation);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onOpenTlk, this, ID_OpenTlk);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onClearTlk, this, ID_ClearTlk);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onCloseTab, this, ID_CloseTab);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onCloseOtherTabs, this, ID_CloseOtherTabs);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onNextTab, this, ID_NextTab);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onPreviousTab, this, ID_PreviousTab);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onUndo, this, ID_Undo);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onRedo, this, ID_Redo);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onAddStartingEntry, this, ID_AddStartingEntry);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onAddChild, this, ID_AddChild);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onLinkExisting, this, ID_LinkExisting);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onDuplicateNode, this, ID_DuplicateNode);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onRemoveLink, this, ID_RemoveLink);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onDeleteNode, this, ID_DeleteNode);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { moveSelectedLink(-1); }, ID_MoveLinkUp);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { moveSelectedLink(1); }, ID_MoveLinkDown);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onFindNext, this, ID_FindNext);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onConversationProperties, this, ID_ConversationProperties);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onValidate, this, ID_Validate);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(false); }, ID_ImportXml);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(true); }, ID_ImportJson);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(false); }, ID_ExportXml);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(true); }, ID_ExportJson);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onExportPatcherPackage, this, ID_ExportPatcher);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setWorkspaceView(WorkspaceView::Conversation); }, ID_ViewConversation);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setWorkspaceView(WorkspaceView::SinglePanel); }, ID_ViewSinglePanel);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setWorkspaceView(WorkspaceView::Raw); }, ID_ViewRaw);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onToggleDarkMode, this, ID_DarkMode);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onIncreaseFontScale, this, ID_FontIncrease);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onDecreaseFontScale, this, ID_FontDecrease);
        Bind(wxEVT_MENU, &NeoDLGPanelImpl::onResetFontScale, this, ID_FontReset);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { requestModuleClose(); }, ID_ModuleExit);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            wxui::showMessage(this, "About NeoDLG",
                              std::string("NeoDLG v") + kVersion + "\nPurpose-built BioWare conversation editor\n\n"
                              "Conversation graph editing, link conditions, TLK text, scripts, animations, validation, and structured GFF access.");
        }, ID_ModuleAbout);
    }

    void buildWindow() {
        auto* panel = new wxPanel(this);
        auto* root = new wxBoxSizer(wxVERTICAL);

        documentTabs_ = new wxAuiNotebook(panel, ID_DocumentTabs, wxDefaultPosition, wxDefaultSize,
                                          wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_CLOSE_ON_ACTIVE_TAB |
                                              wxAUI_NB_SCROLL_BUTTONS);
        neotabs::configureDocumentTabStrip(documentTabs_);
        root->Add(documentTabs_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

        workspaceBook_ = new wxNotebook(panel, ID_Workspace);
        buildConversationPage(workspaceBook_);
        buildRawPage(workspaceBook_);
        root->Add(workspaceBook_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        panel->SetSizer(root);
        auto* moduleLayout = new wxBoxSizer(wxVERTICAL);
        moduleLayout->Add(panel, 1, wxEXPAND);
        SetSizer(moduleLayout);

        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onAddStartingEntry, this, ID_AddStartingEntry);
        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onAddChild, this, ID_AddChild);
        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onLinkExisting, this, ID_LinkExisting);
        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onDuplicateNode, this, ID_DuplicateNode);
        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onRemoveLink, this, ID_RemoveLink);
        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onDeleteNode, this, ID_DeleteNode);
        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onFindNext, this, ID_FindNext);
        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onApplyNode, this, ID_ApplyNode);
        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onApplyScripts, this, ID_ApplyScripts);
        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onApplyPresentation, this, ID_ApplyPresentation);
        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onApplyLink, this, ID_ApplyLink);
        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onAnimationAdd, this, ID_AnimationAdd);
        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onAnimationEdit, this, ID_AnimationEdit);
        Bind(wxEVT_BUTTON, &NeoDLGPanelImpl::onAnimationDelete, this, ID_AnimationDelete);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { moveAnimation(-1); }, ID_AnimationUp);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { moveAnimation(1); }, ID_AnimationDown);
        Bind(wxEVT_TREE_ITEM_ACTIVATED, &NeoDLGPanelImpl::onRawTreeActivated, this, ID_RawTree);
        Bind(wxEVT_TREE_ITEM_EXPANDING, &NeoDLGPanelImpl::onRawTreeExpanding, this, ID_RawTree);
        Bind(wxEVT_TREE_SEL_CHANGED, &NeoDLGPanelImpl::onTreeSelection, this, ID_ConversationTree);
        Bind(wxEVT_TREE_ITEM_ACTIVATED, &NeoDLGPanelImpl::onTreeActivated, this, ID_ConversationTree);
        Bind(wxEVT_TREE_ITEM_MENU, &NeoDLGPanelImpl::onTreeContextMenu, this, ID_ConversationTree);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &NeoDLGPanelImpl::onDocumentTabChanged, this);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &NeoDLGPanelImpl::onDocumentTabCloseRequested, this);
        workspaceBook_->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, &NeoDLGPanelImpl::onWorkspacePageChanged,
                             this, ID_Workspace);
    }

    void buildConversationPage(wxNotebook* parent) {
        conversationWorkspacePage_ = new wxPanel(parent);
        singlePanelWorkspacePage_ = new wxPanel(parent);
        conversationWorkspaceSizer_ = new wxBoxSizer(wxVERTICAL);
        singlePanelWorkspaceSizer_ = new wxBoxSizer(wxVERTICAL);
        conversationWorkspacePage_->SetSizer(conversationWorkspaceSizer_);
        singlePanelWorkspacePage_->SetSizer(singlePanelWorkspaceSizer_);

        semanticWorkspace_ = new wxPanel(conversationWorkspacePage_);
        auto* page = semanticWorkspace_;
        auto* root = new wxBoxSizer(wxVERTICAL);

        semanticToolbarSizer_ = new wxBoxSizer(wxVERTICAL);
        const auto addToolbarButton = [&](int id, const wxString& label) {
            semanticToolbarButtons_.push_back(new wxButton(page, id, label));
        };
        addToolbarButton(ID_AddStartingEntry, "Add Start Entry");
        addToolbarButton(ID_AddChild, "Add Child");
        addToolbarButton(ID_LinkExisting, "Link Existing...");
        addToolbarButton(ID_DuplicateNode, "Duplicate");
        addToolbarButton(ID_RemoveLink, "Remove Link");
        addToolbarButton(ID_DeleteNode, "Delete Node");

        findLabel_ = new wxStaticText(page, wxID_ANY, "Find:");
        findText_ = new wxTextCtrl(page, wxID_ANY);
        findText_->SetName("NeoDLG dialogue find");
        findText_->SetMinSize(FromDIP(wxSize(160, -1)));
        findNextButton_ = new wxButton(page, ID_FindNext, "Next");
        rebuildSemanticToolbar(false);
        root->Add(semanticToolbarSizer_, 0, wxEXPAND);

        auto* splitter = new wxSplitterWindow(page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                               wxSP_LIVE_UPDATE | wxSP_3D);
        conversationTree_ = new wxTreeCtrl(splitter, ID_ConversationTree, wxDefaultPosition, wxDefaultSize,
                                            wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE);
        conversationTree_->SetName("NeoDLG conversation tree");

        inspectorHost_ = new wxPanel(splitter);
        auto* inspectorHostSizer = new wxBoxSizer(wxVERTICAL);
        inspectorBook_ = new wxNotebook(inspectorHost_, wxID_ANY);
        singleInspector_ = new wxScrolledWindow(inspectorHost_, wxID_ANY, wxDefaultPosition,
                                                 wxDefaultSize, wxVSCROLL);
        singleInspector_->SetName("NeoDLG single panel inspector");
        singleInspector_->SetMinSize(FromDIP(wxSize(160, 80)));
        singleInspector_->SetScrollRate(0, FromDIP(10));
        singleInspector_->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            event.Skip();
            if (singleInspector_->GetClientSize() != inspectorClientSize_) {
                inspectorClientSize_ = singleInspector_->GetClientSize();
                queueInspectorLayoutRefresh();
            }
        });
        singleInspectorSizer_ = new wxBoxSizer(wxVERTICAL);
        singleInspector_->SetSizer(singleInspectorSizer_);
        optionalFields_ = new wxCheckBox(inspectorHost_, wxID_ANY, "Show optional fields");
        optionalFields_->SetName("NeoDLG optional fields");
        optionalFields_->SetToolTip(
            "Reveal unused parameter groups and preserved/uncertain fields in all dialogue views. "
            "Does not restore removed authoring controls or modify the document.");
        inspectorHostSizer->Add(optionalFields_, 0, wxALL, FromDIP(4));
        optionalFields_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            setShowOptionalFields(optionalFields_->GetValue());
        });
        singleInspector_->Hide();
        inspectorHostSizer->Add(inspectorBook_, 1, wxEXPAND);
        inspectorHostSizer->Add(singleInspector_, 1, wxEXPAND);
        inspectorHost_->SetSizer(inspectorHostSizer);

        buildNodePage(inspectorBook_);
        buildScriptsPage(inspectorBook_);
        buildPresentationPage(inspectorBook_);
        buildLinkPage(inspectorBook_);
        buildAnimationsPage(inspectorBook_);
        initializeInspectorLayouts();
        rebuildInspectorForms(false);
        splitter->SplitVertically(conversationTree_, inspectorHost_, FromDIP(520));
        splitter->SetMinimumPaneSize(FromDIP(280));
        splitter->SetSashGravity(0.38);
        root->Add(splitter, 1, wxEXPAND);

        page->SetSizer(root);
        conversationWorkspaceSizer_->Add(page, 1, wxEXPAND);
        parent->AddPage(conversationWorkspacePage_, "Conversation", true);
        parent->AddPage(singlePanelWorkspacePage_, "Single Panel", false);
        bindInspectorWheelForwarding(inspectorHost_);
        // ChangeValue() while loading is silent; only actual user edits queue
        // contextual reflow. Use current state when the deferred callback runs.
        for (auto* control : {nodeScript1_, nodeScript2_, nodeQuest_, nodeQuestEntry_,
                              nodeActionStrA_, nodeActionStrB_, linkActive1_, linkActive2_,
                              linkParamStrA_, linkParamStrB_}) {
            control->Bind(wxEVT_TEXT, [this](wxCommandEvent& event) {
                event.Skip();
                queueContextualInspectorRefresh();
            });
        }
        for (auto* parameters : {actionParamFields_, linkParamFields_}) {
            parameters->Bind(wxEVT_TEXT, [this](wxCommandEvent& event) {
                event.Skip();
                queueContextualInspectorRefresh();
            });
        }
        nodeFadeType_->Bind(wxEVT_CHOICE, [this](wxCommandEvent& event) {
            event.Skip();
            inspectorFadeTypeEdited_ = true;
            queueContextualInspectorRefresh();
        });
    }

    void rebuildSemanticToolbar(bool singlePanel) {
        if (!semanticToolbarSizer_) return;

        // Rebuild only the sizers: retain the same buttons, search text,
        // selection, keyboard focus and event IDs across workspace switches.
        semanticToolbarSizer_->Clear(false);
        const auto sizeButton = [singlePanel](wxButton* button) {
            const long style = button->GetWindowStyleFlag();
            button->SetWindowStyleFlag(singlePanel
                ? style | wxBU_EXACTFIT : style & ~wxBU_EXACTFIT);
            button->InvalidateBestSize();
        };
        sizeButton(findNextButton_);

        // wxWrapSizer normally stretches the last item on *each* line. With
        // Delete Node last, that turns it into a wide destructive-action bar.
        // In Single Panel no action may stretch, including after wrapping.
        auto* toolbar = new wxWrapSizer(wxHORIZONTAL, singlePanel
            ? wxREMOVE_LEADING_SPACES : wxWRAPSIZER_DEFAULT_FLAGS);
        for (auto* button : semanticToolbarButtons_) {
            sizeButton(button);
            toolbar->Add(button, 0, wxRIGHT | wxBOTTOM |
                (singlePanel ? wxALIGN_CENTER_VERTICAL : 0), FromDIP(4));
        }

        auto* findRow = new wxBoxSizer(wxHORIZONTAL);
        findRow->Add(findLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        findRow->Add(findText_, 1, wxEXPAND | wxRIGHT, FromDIP(4));
        findRow->Add(findNextButton_, 0);

        if (singlePanel) {
            // One intact group immediately to the right of the actions when
            // it fits. On narrow windows the group wraps without clipping or
            // stretching the preceding button. There is no reserved find row.
            toolbar->Add(findRow, 0, wxALIGN_CENTER_VERTICAL | wxBOTTOM, FromDIP(4));
        }
        semanticToolbarSizer_->Add(toolbar, 0,
            wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(2));
        if (!singlePanel) {
            // Preserve Conversation's original toolbar, full-width search
            // row and spacing. GFF Tree has a separate, untouched filter.
            semanticToolbarSizer_->Add(findRow, 0,
                wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));
        }
        semanticWorkspace_->InvalidateBestSize();
    }

    void setSemanticWorkspaceHost(bool singlePanel) {
        if (!semanticWorkspace_ || !conversationWorkspacePage_ ||
            !singlePanelWorkspacePage_ || !conversationWorkspaceSizer_ ||
            !singlePanelWorkspaceSizer_) {
            return;
        }

        wxPanel* const targetPage = singlePanel
            ? singlePanelWorkspacePage_
            : conversationWorkspacePage_;
        if (semanticWorkspace_->GetParent() == targetPage) return;

        wxPanel* const sourcePage = semanticWorkspace_->GetParent() == singlePanelWorkspacePage_
            ? singlePanelWorkspacePage_
            : conversationWorkspacePage_;
        wxBoxSizer* const sourceSizer = sourcePage == singlePanelWorkspacePage_
            ? singlePanelWorkspaceSizer_
            : conversationWorkspaceSizer_;
        wxBoxSizer* const targetSizer = singlePanel
            ? singlePanelWorkspaceSizer_
            : conversationWorkspaceSizer_;

        wxWindowUpdateLocker updateLocker(workspaceBook_);
        semanticWorkspace_->Hide();
        sourceSizer->Detach(semanticWorkspace_);
        semanticWorkspace_->Reparent(targetPage);
        targetSizer->Add(semanticWorkspace_, 1, wxEXPAND);
        semanticWorkspace_->Show();
        sourcePage->Layout();
        targetPage->Layout();
    }

    wxPanel* makeInspectorSection(wxNotebook* book,
                                  const wxString& title,
                                  wxBoxSizer*& root) {
        auto* tabPage = new wxScrolledWindow(book, wxID_ANY, wxDefaultPosition,
                                              wxDefaultSize, wxVSCROLL);
        tabPage->SetMinSize(FromDIP(wxSize(160, 80)));
        tabPage->SetScrollRate(0, FromDIP(10));
        tabPage->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            event.Skip();
            if (!inspectorLayoutRefreshInProgress_) queueInspectorLayoutRefresh();
        });
        auto* tabSizer = new wxBoxSizer(wxVERTICAL);
        auto* content = new InspectorContentPanel(tabPage);
        root = new wxBoxSizer(wxVERTICAL);
        content->SetSizer(root);
        tabSizer->Add(content, 1, wxEXPAND);
        tabPage->SetSizer(tabSizer);
        book->AddPage(tabPage, title, false);

        auto* singleHost = new InspectorContentPanel(singleInspector_);
        singleHost->SetSinglePanel(true);
        auto* singleSizer = new wxStaticBoxSizer(wxVERTICAL, singleHost, title);
        singleHost->SetSizer(singleSizer);
        singleInspectorSizer_->Add(singleHost, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
                                   FromDIP(3));

        inspectorSections_.push_back({tabPage, tabSizer, content, singleHost, singleSizer});
        return content;
    }

    void setInspectorSectionTitle(std::size_t index, const wxString& title) {
        if (index >= inspectorSections_.size()) return;
        if (inspectorBook_ && index < inspectorBook_->GetPageCount()) {
            inspectorBook_->SetPageText(index, title);
        }
        auto* const staticBox = inspectorSections_[index].singleSizer
            ? inspectorSections_[index].singleSizer->GetStaticBox()
            : nullptr;
        if (staticBox) staticBox->SetLabel(title);
    }

    void refreshInspectorFieldLabels() {
        const bool singlePanel = singlePanelActive_;
        const bool jade = hasActiveDocument() && model().loaded() &&
                          dialogue().dialect() == DlgDialect::JadeEmpire;
        const bool jadeEntry = jade && (!activeDocument().selectedNode ||
            activeDocument().selectedNode->kind == DlgNodeKind::Entry);
        // Dialect refreshes also change these captions. Apply the view-specific
        // captions on every layout refresh, not just when switching views.
        const auto label = [singlePanel](wxControl* control, const wxString& compact,
                                         const wxString& conversation) {
            const wxString& text = singlePanel ? compact : conversation;
            if (control->GetLabel() != text) control->SetLabel(text);
            control->SetToolTip(singlePanel ? conversation : wxString{});
        };
        label(nodeSpeakerLabel_, "Speaker:", jadeEntry ? "Speaker participant:" : "Speaker:");
        label(nodeListenerLabel_, "Listener:", jadeEntry ? "Listener participant:" : "Listener:");
        label(nodeVoLabel_, jadeEntry ? "VO ID:" : "VO resref:",
              jadeEntry ? "Voice-over ID:" : "Voice-over resref:");
        label(nodeScript1Label_, jade ? (jadeEntry ? "Action:" : "Reply script:") : "Action 1:",
              jade ? (jadeEntry ? "Action script:" : "Reply script:") : "Action script 1:");
        label(nodeScript2Label_, jadeEntry ? "Entry script:" : "Action 2:",
              jadeEntry ? "Entry script:" : "Action script 2:");
        label(linkActive1Label_, jade ? "Condition:" : "Condition 1:",
              jade ? "Condition script:" : "Conditional script 1:");
        label(linkActive2Label_, "Condition 2:", "Conditional script 2:");
        label(nodeScriptCamEntryLabel_, "Entry-cam script:", "Entry-camera script:");
        label(nodeCameraEntryLabel_, "Entry tag:", "Entry camera tag:");
        label(nodeScriptCamRepliesLabel_, "Replies-cam script:", "Replies-camera script:");
        label(nodeCameraRepliesLabel_, "Replies tag:", "Replies camera tag:");
        label(nodeActionStrALabel_, "String A:", "Action string A:");
        label(nodeActionStrBLabel_, "String B:", "Action string B:");
        label(linkParamStrALabel_, "String A:", "Conditional string A:");
        label(linkParamStrBLabel_, "String B:", "Conditional string B:");
        label(nodePlotXpLabel_, "Plot XP %:", "Plot XP percentage:");
        label(nodeCamHeightOffsetLabel_, "Camera offset:", "Camera height offset:");
        label(nodeTarHeightOffsetLabel_, "Target offset:", "Target height offset:");
        label(nodeCameraAnimationLabel_, "Camera anim:", "Camera animation:");
        label(nodeAlienRaceLabel_, "Alien-race:", "Alien-race node:");
        label(nodeCameraFovLabel_, "FOV:", "Camera field of view:");
        label(nodeCameraAngleLabel_, "Camera:", "Camera angle:");
        label(nodeCamVidEffectLabel_, "Video effect:", "Camera video effect:");
        label(nodeFadeTypeLabel_, "Fade:", "Fade type:");
        label(nodeFacialAnimLabel_, "Facial:", "Facial animation:");
        label(nodeUnskippable_, "Unskippable", "Node is unskippable");
        label(linkNot1_, "Not 1", "Negate conditional 1");
        label(linkNot2_, "Not 2", "Negate conditional 2");
        label(linkDesignerNumberLabel_, "Designer number:", "Designer number available to script:");
        linkDesignerNumber_->SetToolTip(singlePanel ? "Designer number available to script." : "");
        nodeFadeDelayUnit_->SetLabel(singlePanel ? "s" : "seconds");
        nodeFadeLengthUnit_->SetLabel(singlePanel ? "s" : "seconds");
    }

    void refreshCompactInspectorMetrics() {
        refreshInspectorFieldLabels();
        for (auto* editor : resizableInspectorText_) {
            editor->Show(editor->Text()->IsShown());
            editor->SetSinglePanel(singlePanelActive_);
            editor->RefreshMetrics();
        }
        for (const auto& scalar : inspectorFieldMetrics_) {
            auto* field = scalar.control;
            if (singlePanelActive_) {
                const int chrome = dynamic_cast<wxComboBox*>(field) ? 36 : 20;
                const int width = field->GetTextExtent(scalar.widthSample).x + field->FromDIP(chrome);
                field->SetMinSize(wxSize(width, -1));
                field->SetMaxSize(wxSize(width, -1));
            } else {
                field->SetMinSize(scalar.conversationMin);
                field->SetMaxSize(wxDefaultSize);
            }
            field->InvalidateBestSize();
        }
        for (auto* parameters : {actionParamFields_, linkParamFields_}) {
            if (!parameters) continue;
            parameters->SetSinglePanel(singlePanelActive_);
            parameters->RefreshFieldMetrics();
        }
    }

    void initializeInspectorLayouts() {
        const auto scalar = [&](wxWindow* field, wxStaticText* label,
                                const wxString& sample, wxSize legacyMin = wxDefaultSize) {
            if (label) field->SetName("NeoDLG " + label->GetLabel());
            inspectorFieldMetrics_.push_back({field, sample, legacyMin});
        };
        // Single Panel budgets visible characters, not the entire storage
        // range. These remain unrestricted text controls: long values scroll,
        // and parsing/validation still uses the full text. Conversation restores
        // its original minimums in refreshCompactInspectorMetrics().
        scalar(nodeStrRef_, nodeStrRefLabel_, "0000000");
        scalar(nodeStringType_, nodeStringTypeLabel_, "00");
        scalar(nodeQuestEntry_, nodeQuestEntryLabel_, "0000");
        scalar(nodePlotIndex_, nodePlotIndexLabel_, "-0000");
        scalar(nodePlotXp_, nodePlotXpLabel_, "100.0");
        scalar(nodeDelay_, nodeDelayLabel_, "00000");
        scalar(nodeWaitFlags_, nodeWaitFlagsLabel_, "000");
        scalar(nodeCameraId_, nodeCameraIdLabel_, "-0000");
        scalar(nodeCamHeightOffset_, nodeCamHeightOffsetLabel_, "-0.00");
        scalar(nodeTarHeightOffset_, nodeTarHeightOffsetLabel_, "-0.00");
        scalar(nodeCameraFov_, nodeCameraFovLabel_, "180.0", FromDIP(wxSize(110, -1)));
        scalar(nodeCameraAnimation_, nodeCameraAnimationLabel_, "0000");
        scalar(nodeEmotion_, nodeEmotionLabel_, "000");
        scalar(nodeFacialAnim_, nodeFacialAnimLabel_, "000");
        scalar(nodeFadeColorR_, nullptr, "0.00", FromDIP(wxSize(72, -1)));
        scalar(nodeFadeColorG_, nullptr, "0.00", FromDIP(wxSize(72, -1)));
        scalar(nodeFadeColorB_, nullptr, "0.00", FromDIP(wxSize(72, -1)));
        scalar(nodeFadeDelay_, nodeFadeDelayLabel_, "0.000");
        scalar(nodeFadeLength_, nodeFadeLengthLabel_, "0.000");
        scalar(nodeAlienRace_, nodeAlienRaceLabel_, "000");
        scalar(linkLogic_, linkLogicLabel_, "00");
        scalar(linkDesignerNumber_, linkDesignerNumberLabel_, "-0000");

        // These are only display widths: free-form tags/strings are not given
        // a maximum input length. Restore their original minimums in Conversation.
        const auto compactText = [&](wxWindow* field, wxStaticText* label,
                                     const wxString& sample = "abcdefghijklmnop") {
            scalar(field, label, sample, field->GetMinSize());
        };
        compactText(nodeSpeaker_, nodeSpeakerLabel_);
        compactText(nodeListener_, nodeListenerLabel_);
        compactText(nodeVo_, nodeVoLabel_);
        compactText(nodeScript1_, nodeScript1Label_);
        compactText(nodeScript2_, nodeScript2Label_);
        compactText(nodeScriptCamEntry_, nodeScriptCamEntryLabel_);
        compactText(nodeCameraEntry_, nodeCameraEntryLabel_);
        compactText(nodeScriptCamReplies_, nodeScriptCamRepliesLabel_);
        compactText(nodeCameraReplies_, nodeCameraRepliesLabel_);
        compactText(nodeQuest_, nodeQuestLabel_);
        compactText(nodeSound_, nodeSoundLabel_);
        compactText(linkActive1_, linkActive1Label_);
        compactText(linkActive2_, linkActive2Label_);
        compactText(nodeActionStrA_, nodeActionStrALabel_, "abcdefghijklmnopqrst");
        compactText(nodeActionStrB_, nodeActionStrBLabel_, "abcdefghijklmnopqrst");
        compactText(linkParamStrA_, linkParamStrALabel_, "abcdefghijklmnopqrst");
        compactText(linkParamStrB_, linkParamStrBLabel_, "abcdefghijklmnopqrst");
    }

    struct InspectorField {
        wxWindow* label;
        wxWindow* control;
        wxWindow* unit = nullptr;
        // Optional companions stay with their primary field (e.g. custom FOV
        // and its units), but consume no space while contextually hidden.
        std::vector<wxWindow*> companions{};
    };

    struct CompactInspectorBand {
        wxBoxSizer* rows;
        std::vector<InspectorField> fields;
        std::vector<std::vector<wxWindow*>> shown;
        std::vector<int> rowIndices;
        wxSizerItem* item;
    };

    void refreshCompactInspectorBands() {
        if (!singlePanelActive_ || !singleInspector_) return;
        // Measure the visible scroll viewport, never a cached section minimum.
        const int availableWidth = std::max(1,
            singleInspector_->GetClientSize().x - FromDIP(36));
        const int groupGap = FromDIP(10);
        const int fieldGap = FromDIP(4);
        for (auto* parameters : {actionParamFields_, linkParamFields_}) {
            if (parameters) parameters->SetAvailableWidth(availableWidth);
        }
        // The video's choice list changes with game flavor. Do not keep the
        // containing panel's best width from a previously selected document.
        nodeCamVidEffectPanel_->InvalidateBestSize();
        for (auto& band : compactInspectorBands_) {
            std::vector<std::vector<wxWindow*>> shown;
            std::vector<int> widths;
            for (const auto& field : band.fields) {
                if (!field.control->IsShown() || (field.label && !field.label->IsShown())) continue;
                std::vector<wxWindow*> parts;
                if (field.label) parts.push_back(field.label);
                parts.push_back(field.control);
                if (field.unit && field.unit->IsShown()) parts.push_back(field.unit);
                for (auto* companion : field.companions)
                    if (companion->IsShown()) parts.push_back(companion);
                std::int64_t width = 0;
                for (auto* part : parts)
                    width += std::max(0, part->GetEffectiveMinSize().x);
                width += static_cast<std::int64_t>(fieldGap) * (parts.size() - 1);
                widths.push_back(static_cast<int>(std::min<std::int64_t>(
                    width, std::numeric_limits<int>::max())));
                shown.push_back(std::move(parts));
            }
            const auto rowIndices = inspectorFieldRows(widths, availableWidth, groupGap);
            band.item->SetBorder(shown.empty() ? 0 : FromDIP(3));
            if (shown == band.shown && rowIndices == band.rowIndices) continue;
            // Rebuild only sizers. Preserve live controls, pending edits, native
            // undo buffers, focus, visibility and all event bindings.
            band.rows->Clear(false);
            band.shown = std::move(shown);
            band.rowIndices = rowIndices;
            wxBoxSizer* row = nullptr;
            int previousRow = -1;
            for (std::size_t i = 0; i < band.shown.size(); ++i) {
                const bool newRow = rowIndices[i] != previousRow;
                if (newRow) {
                    row = new wxBoxSizer(wxHORIZONTAL);
                    band.rows->Add(row, 0, wxEXPAND | wxTOP,
                                   previousRow < 0 ? 0 : FromDIP(3));
                    previousRow = rowIndices[i];
                }
                auto* group = new wxBoxSizer(wxHORIZONTAL);
                for (std::size_t part = 0; part < band.shown[i].size(); ++part)
                    group->Add(band.shown[i][part], 0, wxALIGN_CENTER_VERTICAL | wxLEFT,
                               part == 0 ? 0 : fieldGap);
                row->Add(group, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, newRow ? 0 : groupGap);
            }
        }
    }

    wxWindow* inspectorFieldWindow(wxWindow* control) const {
        if (auto* wrapper = dynamic_cast<ResizableInspectorText*>(control->GetParent()))
            return wrapper;
        return control;
    }

    // Rebuild sizers, not controls. Editing state, undo buffers, validators,
    // event bindings and selection therefore survive a workspace-view change.
    // Only the five semantic sections are laid out here; GFF has a separate row filter.
    void rebuildInspectorForms(bool singlePanel) {
        if (inspectorSections_.size() != 5) return;
        refreshCompactInspectorMetrics();
        // The following Clear(false) calls destroy the old row sizers.
        compactInspectorBands_.clear();
        compactInspectorRows_.clear();
        nodeHeader_->Show(singlePanel);
        conversationNodeHeader_->Show(!singlePanel);
        const int pad = singlePanel ? FromDIP(4) : 10;
        const auto newForm = [&]() {
            auto* form = new wxFlexGridSizer(2, singlePanel ? FromDIP(3) : 8,
                                               singlePanel ? FromDIP(6) : 8);
            form->AddGrowableCol(1, 1);
            return form;
        };
        const auto row = [&](wxFlexGridSizer* form, const InspectorField& field) {
            if (!singlePanel && (!field.label->IsShown() || !field.control->IsShown())) return;
            form->Add(field.label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                      singlePanel ? FromDIP(3) : 8);
            wxWindow* control = inspectorFieldWindow(field.control);
            if (field.unit) {
                auto* withUnit = new wxBoxSizer(wxHORIZONTAL);
                withUnit->Add(control, singlePanel ? 0 : 1, wxEXPAND | wxRIGHT, 6);
                withUnit->Add(field.unit, 0, wxALIGN_CENTER_VERTICAL);
                form->Add(withUnit, 1, wxEXPAND);
            } else if (dynamic_cast<wxCheckBox*>(control)) {
                form->Add(control, 0, wxALIGN_CENTER_VERTICAL);
            } else {
                form->Add(control, 1, wxEXPAND);
            }
        };
        const auto rows = [&](wxFlexGridSizer* form, std::initializer_list<InspectorField> fields) {
            for (const auto& field : fields) row(form, field);
        };
        const auto addForm = [&](wxSizer* root, wxSizer* form, int stretch = 0) {
            if (form->GetItemCount() == 0) { delete form; return; }
            auto* item = root->Add(form, stretch, wxEXPAND | wxALL, pad);
            if (singlePanel) compactInspectorRows_.push_back(item);
        };
        const auto band = [&](wxSizer* root, std::initializer_list<InspectorField> fields) {
            auto* lines = new wxBoxSizer(wxVERTICAL);
            auto* item = root->Add(lines, 0, wxEXPAND | wxALL, FromDIP(3));
            compactInspectorBands_.push_back({lines, fields, {}, {}, item});
        };
        const auto apply = [&](wxSizer* root, wxWindow* page, int id) {
            root->Add(page->FindWindow(id), 0, wxALIGN_RIGHT | wxALL, pad);
        };
        const auto parameters = [&](wxSizer* root, wxWindow* title, IntegerParameterFields* fields) {
            if (!singlePanel && !fields->IsShown()) return;
            root->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, pad);
            root->Add(fields, 0, wxEXPAND | wxALL, pad);
        };

        for (std::size_t index = 0; index < inspectorSections_.size(); ++index) {
            auto* page = inspectorSections_[index].content;
            static_cast<InspectorContentPanel*>(page)->SetSinglePanel(singlePanel);
            auto* root = page->GetSizer();
            // Clear(false) recursively deletes sizers but NOT the live controls.
            root->Clear(false);
            if (index == 0) {
                root->Add(singlePanel ? static_cast<wxWindow*>(nodeHeader_)
                                      : static_cast<wxWindow*>(conversationNodeHeader_),
                          0, wxEXPAND | wxALL, pad);
                auto* form = newForm();
                if (singlePanel) {
                    band(root, {{nodeSpeakerLabel_, nodeSpeaker_}, {nodeListenerLabel_, nodeListener_},
                                {nodeStrRefLabel_, nodeStrRef_}, {nodeVoLabel_, nodeVo_},
                                {nodeStringTypeLabel_, nodeStringType_}, {nullptr, nodeJadeSkippable_}});
                    rows(form, {{nodeLocalTextLabel_, nodeLocalText_}, {nodeResolvedTextLabel_, nodeResolvedText_},
                                {nodeCommentLabel_, nodeComment_}});
                } else {
                    rows(form, {{nodeSpeakerLabel_, nodeSpeaker_}, {nodeListenerLabel_, nodeListener_},
                                {nodeStrRefLabel_, nodeStrRef_}, {nodeStringTypeLabel_, nodeStringType_},
                                {nodeLocalTextLabel_, nodeLocalText_}, {nodeResolvedTextLabel_, nodeResolvedText_},
                                {nodeVoLabel_, nodeVo_}, {nodeJadeSkippablePlaceholder_, nodeJadeSkippable_},
                                {nodeCommentLabel_, nodeComment_}});
                }
                root->Add(form, singlePanel ? 0 : 1,
                          wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);
                apply(root, page, ID_ApplyNode);
            } else if (index == 1) {
                auto* form = newForm();
                if (singlePanel) {
                    band(root, {{nodeScript1Label_, nodeScript1_}, {nodeScript2Label_, nodeScript2_},
                                {nodeScriptCamEntryLabel_, nodeScriptCamEntry_}, {nodeCameraEntryLabel_, nodeCameraEntry_},
                                {nodeScriptCamRepliesLabel_, nodeScriptCamReplies_}, {nodeCameraRepliesLabel_, nodeCameraReplies_},
                                {nodeActionStrALabel_, nodeActionStrA_}, {nodeActionStrBLabel_, nodeActionStrB_},
                                {nodeQuestLabel_, nodeQuest_}, {nodeQuestEntryLabel_, nodeQuestEntry_},
                                {nodePlotIndexLabel_, nodePlotIndex_}, {nodePlotXpLabel_, nodePlotXp_}});
                    delete form; // No expanded single-column form in this view.
                } else {
                    rows(form, {{nodeScript1Label_, nodeScript1_}, {nodeScript2Label_, nodeScript2_},
                                {nodeScriptCamEntryLabel_, nodeScriptCamEntry_}, {nodeCameraEntryLabel_, nodeCameraEntry_},
                                {nodeScriptCamRepliesLabel_, nodeScriptCamReplies_}, {nodeCameraRepliesLabel_, nodeCameraReplies_},
                                {nodeQuestLabel_, nodeQuest_}, {nodeQuestEntryLabel_, nodeQuestEntry_},
                                {nodePlotIndexLabel_, nodePlotIndex_}, {nodePlotXpLabel_, nodePlotXp_},
                                {nodeActionStrALabel_, nodeActionStrA_}, {nodeActionStrBLabel_, nodeActionStrB_}});
                    addForm(root, form);
                }
                parameters(root, actionParamHeading_, actionParamFields_);
                apply(root, page, ID_ApplyScripts);
            } else if (index == 2) {
                root->Add(jadePresentationNote_, 0, wxEXPAND | wxALL, pad);
                if (singlePanel) {
                    // One continuous flow instead of separate sound, camera,
                    // FOV, video, fade and flag rows with half-empty tails.
                    band(root, {{nodeSoundLabel_, nodeSound_}, {nodeDelayLabel_, nodeDelay_},
                                {nodeWaitFlagsLabel_, nodeWaitFlags_},
                                {nodeCameraAngleLabel_, nodeCameraAngle_}, {nodeCameraIdLabel_, nodeCameraId_},
                                {nodeCameraAnimationLabel_, nodeCameraAnimation_},
                                {nodeCameraFovLabel_, nodeCameraFovMode_, nullptr,
                                    {nodeCameraFov_, nodeCameraFovUnit_}},
                                {nodeCamHeightOffsetLabel_, nodeCamHeightOffset_},
                                {nodeTarHeightOffsetLabel_, nodeTarHeightOffset_},
                                {nodeCamVidEffectLabel_, nodeCamVidEffectPanel_},
                                {nodeEmotionLabel_, nodeEmotion_}, {nodeFacialAnimLabel_, nodeFacialAnim_},
                                {nodeAlienRaceLabel_, nodeAlienRace_}, {nullptr, nodeUnskippable_},
                                {nodeFadeTypeLabel_, nodeFadeType_},
                                {nodeFadeDelayLabel_, nodeFadeDelay_, nodeFadeDelayUnit_},
                                {nodeFadeLengthLabel_, nodeFadeLength_, nodeFadeLengthUnit_},
                                {nodeFadeColorLabel_, nodeFadeColorPicker_},
                                {nodeFadeColorRLabel_, nodeFadeColorR_},
                                {nodeFadeColorGLabel_, nodeFadeColorG_},
                                {nodeFadeColorBLabel_, nodeFadeColorB_}});
                } else {
                    // Conversation keeps its existing row order and sizing.
                    auto* form = newForm();
                    rows(form, {{nodeSoundLabel_, nodeSound_}, {nodeDelayLabel_, nodeDelay_},
                                {nodeWaitFlagsLabel_, nodeWaitFlags_}, {nodeCameraAngleLabel_, nodeCameraAngle_},
                                {nodeCameraIdLabel_, nodeCameraId_}, {nodeCamHeightOffsetLabel_, nodeCamHeightOffset_},
                                {nodeTarHeightOffsetLabel_, nodeTarHeightOffset_}});
                    if (nodeCameraFovLabel_->IsShown()) {
                        form->Add(nodeCameraFovLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
                        auto* fov = new wxBoxSizer(wxHORIZONTAL);
                        fov->Add(nodeCameraFovMode_, 0, wxRIGHT, 6);
                        fov->Add(nodeCameraFov_, 1, wxEXPAND | wxRIGHT, 6);
                        fov->Add(nodeCameraFovUnit_, 0, wxALIGN_CENTER_VERTICAL);
                        form->Add(fov, 1, wxEXPAND);
                    }
                    rows(form, {{nodeCameraAnimationLabel_, nodeCameraAnimation_},
                                {nodeEmotionLabel_, nodeEmotion_}, {nodeFacialAnimLabel_, nodeFacialAnim_},
                                {nodeCamVidEffectLabel_, nodeCamVidEffectPanel_},
                                {nodeFadeTypeLabel_, nodeFadeType_}});
                    if (nodeFadeColorLabel_->IsShown()) {
                        form->Add(nodeFadeColorLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
                        auto* color = new wxBoxSizer(wxHORIZONTAL);
                        color->Add(nodeFadeColorPicker_, 0, wxRIGHT, 8);
                        for (const auto& field : {InspectorField{nodeFadeColorRLabel_, nodeFadeColorR_},
                                                  InspectorField{nodeFadeColorGLabel_, nodeFadeColorG_},
                                                  InspectorField{nodeFadeColorBLabel_, nodeFadeColorB_}}) {
                            color->Add(field.label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
                            color->Add(field.control, 0, wxRIGHT, field.control == nodeFadeColorB_ ? 0 : 6);
                        }
                        form->Add(color, 1, wxEXPAND);
                    }
                    rows(form, {{nodeFadeDelayLabel_, nodeFadeDelay_, nodeFadeDelayUnit_},
                                {nodeFadeLengthLabel_, nodeFadeLength_, nodeFadeLengthUnit_},
                                {nodeAlienRaceLabel_, nodeAlienRace_},
                                {nodeUnskippablePlaceholder_, nodeUnskippable_}});
                    addForm(root, form, 1);
                }
                apply(root, page, ID_ApplyPresentation);
            } else if (index == 3) {
                root->Add(linkHeader_, 0, wxEXPAND | wxALL, pad);
                auto* form = newForm();
                if (singlePanel) {
                    band(root, {{linkActive1Label_, linkActive1_}, {nullptr, linkNot1_},
                                {linkActive2Label_, linkActive2_}, {nullptr, linkNot2_},
                                {linkParamStrALabel_, linkParamStrA_}, {linkParamStrBLabel_, linkParamStrB_},
                                {linkLogicLabel_, linkLogic_}, {linkDesignerNumberLabel_, linkDesignerNumber_},
                                {nullptr, linkReverseCond_}, {nullptr, linkDisplayInactive_}});
                    delete form;
                } else {
                    rows(form, {{linkActive1Label_, linkActive1_}, {linkActive2Label_, linkActive2_},
                                {linkLogicLabel_, linkLogic_}, {linkParamStrALabel_, linkParamStrA_},
                                {linkParamStrBLabel_, linkParamStrB_},
                                {linkDesignerNumberLabel_, linkDesignerNumber_}, {linkNot1Placeholder_, linkNot1_},
                                {linkNot2Placeholder_, linkNot2_},
                                {linkReverseCondPlaceholder_, linkReverseCond_}});
                    addForm(root, form);
                }
                if (!singlePanel && linkDisplayInactive_->IsShown())
                    root->Add(linkDisplayInactive_, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);
                parameters(root, linkParamHeading_, linkParamFields_);
                apply(root, page, ID_ApplyLink);
            } else if (singlePanel) {
                // One small action/status row; never stretch the last button.
                auto* actions = new wxWrapSizer(wxHORIZONTAL, wxREMOVE_LEADING_SPACES);
                actions->Add(animationSummary_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT,
                             FromDIP(6));
                for (auto* button : {animationAddButton_, animationEditButton_, animationDeleteButton_})
                    actions->Add(button, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(3));
                root->Add(actions, 0, wxEXPAND | wxALL, pad);
                // The list is sized to its columns and at most three rows, not
                // the section width or the old fixed 220-DIP empty canvas.
                root->Add(animationList_, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);
            } else {
                root->Add(animationList_, 1, wxEXPAND | wxALL, pad);
                auto* buttons = new wxBoxSizer(wxHORIZONTAL);
                buttons->Add(animationAddButton_, 0, wxRIGHT, 4);
                buttons->Add(animationEditButton_, 0, wxRIGHT, 4);
                buttons->Add(animationDeleteButton_, 0);
                root->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, pad);
            }
            page->InvalidateBestSize();
        }
    }

    void setNodeHeader(const wxString& text) {
        wxString singleLine = text;
        singleLine.Replace("\r", " ");
        singleLine.Replace("\n", " ");
        nodeHeader_->SetLabel(singleLine);
        nodeHeader_->SetToolTip(text);
        if (conversationNodeHeader_) conversationNodeHeader_->ChangeValue(text);
    }

    wxTextCtrl* addResizableInspectorText(wxPanel* page, wxFlexGridSizer* form,
                                            const wxString& label, const wxString& name,
                                            wxStaticText** labelOut, long style = 0,
                                            int conversationHeightDip = 80) {
        auto* labelControl = new wxStaticText(page, wxID_ANY, label);
        if (labelOut) *labelOut = labelControl;
        form->Add(labelControl, 0, wxALIGN_TOP | wxRIGHT | wxTOP, FromDIP(3));
        auto* editor = new ResizableInspectorText(page, name, style,
                                                  [this]() { queueInspectorLayoutRefresh(); },
                                                  conversationHeightDip);
        form->Add(editor, 0, wxEXPAND);
        resizableInspectorText_.push_back(editor);
        return editor->Text();
    }

    void queueInspectorLayoutRefresh() {
        if (IsBeingDeleted() || inspectorLayoutRefreshPending_) return;
        inspectorLayoutRefreshPending_ = true;
        // Coalesce live sash/field resizing. Posting on this handler also drops
        // the callback if the editor is destroyed before it can run.
        CallAfter([this]() {
            inspectorLayoutRefreshPending_ = false;
            if (!IsBeingDeleted()) refreshInspectorLayouts();
        });
    }

    void bindInspectorWheelForwarding(wxWindow* window) {
        if (!window) return;
        const auto* text = dynamic_cast<wxTextCtrl*>(window);
        const bool background = dynamic_cast<wxPanel*>(window) ||
                                dynamic_cast<wxStaticText*>(window) ||
                                dynamic_cast<wxStaticBox*>(window);
        // Leave multiline editors and lists with their own native scrolling.
        // Forward only while hosted by Single Panel. Conversation retains its
        // native wheel handling, even though these controls are shared.
        if (!dynamic_cast<wxScrolledWindow*>(window) &&
            (background || (text && !text->HasFlag(wxTE_MULTILINE)))) {
            window->Bind(wxEVT_MOUSEWHEEL, [this, window](wxMouseEvent& event) {
                if (!singlePanelActive_ || event.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL ||
                    event.ControlDown() || event.CmdDown() || event.ShiftDown() ||
                    event.GetWheelDelta() <= 0) {
                    event.Skip();
                    return;
                }
                wxScrolledWindow* target = nullptr;
                for (auto* parent = window->GetParent(); parent; parent = parent->GetParent()) {
                    target = dynamic_cast<wxScrolledWindow*>(parent);
                    if (target) break;
                }
                if (target != singleInspector_ || !target->IsEnabled()) { event.Skip(); return; }
                if (wheelTarget_ != target) { wheelTarget_ = target; wheelRotation_ = 0; }
                wheelRotation_ += event.GetWheelRotation();
                const int turns = wheelRotation_ / event.GetWheelDelta();
                wheelRotation_ %= event.GetWheelDelta();
                if (!turns) return;
                int unitY = 0;
                target->GetScrollPixelsPerUnit(nullptr, &unitY);
                if (unitY <= 0) { event.Skip(); return; }
                const int lines = event.IsPageScroll()
                    ? std::max(1, target->GetClientSize().y / unitY)
                    : std::max(1, event.GetLinesPerAction());
                const int y = target->GetViewStart().y;
                target->Scroll(-1, std::max(0, y - turns * lines));
            });
        }
        for (auto* child : window->GetChildren()) bindInspectorWheelForwarding(child);
    }

    void refreshInspectorLayouts() {
        if (inspectorLayoutRefreshInProgress_) return;
        inspectorLayoutRefreshInProgress_ = true;
        refreshCompactInspectorMetrics();
        // Establish the real viewport first; FitInside must not measure a
        // hidden or pre-splitter size and make that stale height persistent.
        if (inspectorHost_) inspectorHost_->Layout();
        refreshCompactInspectorBands();
        // An empty form/check row must not retain its outer margins after
        // contextual hiding. Walk actual windows, not cached minimum sizes.
        if (singlePanelActive_) {
            const std::function<bool(wxSizer*)> shown = [&](wxSizer* sizer) {
                for (auto* item : sizer->GetChildren()) {
                    if (item->IsWindow() && item->GetWindow()->IsShown()) return true;
                    if (item->IsSizer() && shown(item->GetSizer())) return true;
                }
                return false;
            };
            for (auto* item : compactInspectorRows_)
                item->SetBorder(shown(item->GetSizer()) ? FromDIP(4) : 0);
        }
        const auto refit = [](wxScrolledWindow* scroll) {
            if (!scroll) return;
            const wxPoint previous = scroll->GetViewStart();
            scroll->InvalidateBestSize();
            scroll->FitInside();
            scroll->Layout();
            // Scroll() clamps naturally after shrinking; don't jump to the top
            // on every selection, font change or resize-grip motion.
            scroll->Scroll(previous);
        };
        // First propagate widths into wrapped rows, then recompute their
        // heights. Invalidate panel caches from the content outwards because
        // Layout() alone doesn't invalidate a wxWindow's cached best size.
        for (int pass = 0; pass < 2; ++pass) {
            // FitInside may add/remove the vertical scrollbar on the first
            // pass. Pack once more against that final visible width.
            if (pass != 0) refreshCompactInspectorBands();
            refreshAnimationPresentation();
            for (auto& section : inspectorSections_) {
                if (section.content) {
                    section.content->InvalidateBestSize();
                    section.content->Layout();
                }
                if (section.singleSizer) section.singleSizer->GetStaticBox()->InvalidateBestSize();
                if (section.singleHost) {
                    section.singleHost->InvalidateBestSize();
                    section.singleHost->Layout();
                }
                if (!singlePanelActive_) refit(section.tabPage);
            }
            if (singlePanelActive_) refit(singleInspector_);
        }
        inspectorLayoutRefreshInProgress_ = false;
    }

    void setSinglePanelLayout(bool singlePanel) {
        if (!inspectorBook_ || !singleInspector_ || !inspectorHost_) return;
        if (singlePanelActive_ == singlePanel) {
            inspectorBook_->Show(!singlePanel);
            singleInspector_->Show(singlePanel);
            refreshContextualInspector();
            return;
        }

        wxWindowUpdateLocker updateLocker(inspectorHost_);
        inspectorBook_->Hide();
        singleInspector_->Hide();

        for (auto& section : inspectorSections_) {
            if (!section.content || !section.tabPage || !section.tabSizer ||
                !section.singleSizer) {
                continue;
            }

            section.content->Hide();
            if (section.content->GetParent() == section.tabPage) {
                section.tabSizer->Detach(section.content);
            } else {
                section.singleSizer->Detach(section.content);
            }

            if (singlePanel) {
                wxWindow* const target = section.singleSizer->GetStaticBox();
                section.content->Reparent(target);
                section.singleSizer->Add(section.content, 0, wxEXPAND | wxALL, FromDIP(2));
            } else {
                section.content->Reparent(section.tabPage);
                section.tabSizer->Add(section.content, 1, wxEXPAND);
            }
            section.content->Show();
        }

        singlePanelActive_ = singlePanel;
        rebuildSemanticToolbar(singlePanel);
        // Relayout the whole workspace, not only the inspector: the splitter
        // must receive the height released when the separate find row vanishes.
        semanticWorkspace_->Layout();
        rebuildInspectorForms(singlePanel);
        inspectorBook_->Show(!singlePanel);
        singleInspector_->Show(singlePanel);
        refreshContextualInspector();
    }

    void buildNodePage(wxNotebook* book) {
        wxBoxSizer* root = nullptr;
        wxPanel* page = makeInspectorSection(book, "Line", root);
        nodeHeader_ = new wxStaticText(page, wxID_ANY, "Select a dialogue node.", wxDefaultPosition,
                                       wxDefaultSize, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        nodeHeader_->SetMinSize(FromDIP(wxSize(120, -1)));
        nodeHeader_->SetName("NeoDLG selected node");
        wxFont bold = nodeHeader_->GetFont();
        bold.SetWeight(wxFONTWEIGHT_BOLD);
        nodeHeader_->SetFont(bold);
        conversationNodeHeader_ = new wxTextCtrl(page, wxID_ANY, "Select a dialogue node.",
            wxDefaultPosition, FromDIP(wxSize(-1, 92)),
            wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP | wxBORDER_NONE);
        conversationNodeHeader_->SetName("NeoDLG conversation selected node");
        conversationNodeHeader_->SetFont(bold);
        conversationNodeHeader_->Hide();
        root->Add(nodeHeader_, 0, wxEXPAND | wxALL, FromDIP(3));

        auto* form = new wxFlexGridSizer(2, FromDIP(3), FromDIP(6));
        form->AddGrowableCol(1, 1);
        nodeSpeakerLabel_ = new wxStaticText(page, wxID_ANY, "Speaker:");
        form->Add(nodeSpeakerLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        nodeSpeaker_ = new wxComboBox(page, wxID_ANY);
        form->Add(nodeSpeaker_, 1, wxEXPAND);

        nodeListenerLabel_ = new wxStaticText(page, wxID_ANY, "Listener:");
        form->Add(nodeListenerLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        nodeListener_ = new wxComboBox(page, wxID_ANY);
        form->Add(nodeListener_, 1, wxEXPAND);

        nodeStrRef_ = addTextField(page, form, "Text StrRef:", 0, wxDefaultSize, &nodeStrRefLabel_);
        nodeStringType_ = addTextField(page, form, "Jade string type:", 0, wxDefaultSize, &nodeStringTypeLabel_);
        nodeLocalText_ = addResizableInspectorText(page, form, "Local text:",
            "NeoDLG local text", &nodeLocalTextLabel_, 0, 110);
        nodeResolvedText_ = addResizableInspectorText(page, form, "Resolved TLK text:",
            "NeoDLG resolved TLK text", &nodeResolvedTextLabel_, wxTE_READONLY, 110);
        nodeVo_ = addTextField(page, form, "Voice-over resref:", 0, wxDefaultSize, &nodeVoLabel_);
        nodeJadeSkippable_ = addCheckField(page, form, "Entry can be skipped", &nodeJadeSkippablePlaceholder_);
        nodeJadeSkippable_->SetToolTip(
            "Jade Empire Entry Skippable field. The runtime default is enabled when the field is absent.");
        nodeComment_ = addResizableInspectorText(page, form, "Designer comment:",
            "NeoDLG designer comment", &nodeCommentLabel_);
        root->Add(form, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(3));
        root->Add(new wxButton(page, ID_ApplyNode, "Apply Line Changes"), 0, wxALIGN_RIGHT | wxALL, FromDIP(3));
    }

    void buildScriptsPage(wxNotebook* book) {
        wxBoxSizer* root = nullptr;
        wxPanel* page = makeInspectorSection(book, "Scripts / Quest", root);
        auto* form = new wxFlexGridSizer(2, 8, 8);
        form->AddGrowableCol(1, 1);
        nodeScript1_ = addTextField(page, form, "Action script 1:", 0, wxDefaultSize, &nodeScript1Label_);
        nodeScript2_ = addTextField(page, form, "Action script 2:", 0, wxDefaultSize, &nodeScript2Label_);
        nodeScriptCamEntry_ = addTextField(page, form, "Entry-camera script:", 0, wxDefaultSize, &nodeScriptCamEntryLabel_);
        nodeCameraEntry_ = addTextField(page, form, "Entry camera tag:", 0, wxDefaultSize, &nodeCameraEntryLabel_);
        nodeScriptCamReplies_ = addTextField(page, form, "Replies-camera script:", 0, wxDefaultSize, &nodeScriptCamRepliesLabel_);
        nodeCameraReplies_ = addTextField(page, form, "Replies camera tag:", 0, wxDefaultSize, &nodeCameraRepliesLabel_);
        nodeQuest_ = addTextField(page, form, "Quest tag:", 0, wxDefaultSize, &nodeQuestLabel_);
        nodeQuestEntry_ = addTextField(page, form, "Quest entry:", 0, wxDefaultSize, &nodeQuestEntryLabel_);
        nodePlotIndex_ = addTextField(page, form, "Plot index:", 0, wxDefaultSize, &nodePlotIndexLabel_);
        nodePlotXp_ = addTextField(page, form, "Plot XP percentage:", 0, wxDefaultSize, &nodePlotXpLabel_);
        nodeActionStrA_ = addTextField(page, form, "Action string A:", 0, wxDefaultSize, &nodeActionStrALabel_);
        nodeActionStrB_ = addTextField(page, form, "Action string B:", 0, wxDefaultSize, &nodeActionStrBLabel_);
        root->Add(form, 0, wxEXPAND | wxALL, 10);

        actionParamHeading_ = new wxStaticText(page, wxID_ANY, "Action integer parameters");
        root->Add(actionParamHeading_, 0, wxLEFT | wxRIGHT | wxTOP, 10);
        actionParamFields_ = new IntegerParameterFields(page, "Script 1", "Script 2");
        actionParamFields_->SetName("NeoDLG action parameters");
        root->Add(actionParamFields_, 0, wxEXPAND | wxALL, 10);
        nodeCameraEntry_->SetToolTip(
            "Free-form Jade camera tag exposed to scripts; NeoDLG stores it in lowercase.");
        nodeCameraReplies_->SetToolTip(
            "Free-form Jade reply-camera tag exposed to scripts; NeoDLG stores it in lowercase.");
        root->Add(new wxButton(page, ID_ApplyScripts, "Apply Script / Quest Changes"), 0, wxALIGN_RIGHT | wxALL, 10);
    }

    void buildPresentationPage(wxNotebook* book) {
        wxBoxSizer* root = nullptr;
        wxPanel* page = makeInspectorSection(book, "Presentation", root);
        jadePresentationNote_ = new wxStaticText(
            page, wxID_ANY,
            "Jade Empire dialogue presentation is controlled by Entry camera scripts/tags and the Animations page. "
            "KotOR camera, fade, delay, sound, and post-processing fields do not belong to the Jade DLG runtime schema.");
        jadePresentationNote_->Wrap(FromDIP(520));
        jadePresentationNote_->Hide();
        root->Add(jadePresentationNote_, 0, wxEXPAND | wxALL, 10);
        auto* form = new wxFlexGridSizer(2, 8, 8);
        form->AddGrowableCol(1, 1);

        nodeSound_ = addTextField(page, form, "Sound resref:", 0, wxDefaultSize, &nodeSoundLabel_);
        nodeDelay_ = addTextField(page, form, "Delay:", 0, wxDefaultSize, &nodeDelayLabel_);
        nodeWaitFlags_ = addTextField(page, form, "Wait flags:", 0, wxDefaultSize, &nodeWaitFlagsLabel_);

        nodeCameraAngleLabel_ = new wxStaticText(page, wxID_ANY, "Camera angle:");
        form->Add(nodeCameraAngleLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        nodeCameraAngle_ = new wxChoice(page, wxID_ANY);
        populateIntegerChoice(nodeCameraAngle_, nodeCameraAngleValues_, kCameraAngleOptions,
                              "0", 0, "Unknown camera mode (preserve until changed)");
        form->Add(nodeCameraAngle_, 1, wxEXPAND);

        nodeCameraId_ = addTextField(page, form, "Camera ID:", 0, wxDefaultSize, &nodeCameraIdLabel_);
        nodeCamHeightOffset_ = addTextField(page, form, "Camera height offset:", 0, wxDefaultSize,
                                            &nodeCamHeightOffsetLabel_);
        nodeTarHeightOffset_ = addTextField(page, form, "Target height offset:", 0, wxDefaultSize,
                                             &nodeTarHeightOffsetLabel_);

        nodeCameraFovLabel_ = new wxStaticText(page, wxID_ANY, "Camera field of view:");
        form->Add(nodeCameraFovLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        auto* fovRow = new wxBoxSizer(wxHORIZONTAL);
        nodeCameraFovMode_ = new wxChoice(page, wxID_ANY);
        nodeCameraFovMode_->Append("Automatic");
        nodeCameraFovMode_->Append("Custom");
        nodeCameraFovMode_->SetSelection(0);
        fovRow->Add(nodeCameraFovMode_, 0, wxRIGHT, 6);
        nodeCameraFov_ = new wxTextCtrl(page, wxID_ANY);
        nodeCameraFov_->SetMinSize(FromDIP(wxSize(110, -1)));
        fovRow->Add(nodeCameraFov_, 1, wxEXPAND | wxRIGHT, 6);
        nodeCameraFovUnit_ = new wxStaticText(page, wxID_ANY, "degrees");
        fovRow->Add(nodeCameraFovUnit_, 0, wxALIGN_CENTER_VERTICAL);
        form->Add(fovRow, 1, wxEXPAND);

        nodeCameraAnimation_ = addTextField(page, form, "Camera animation:", 0, wxDefaultSize,
                                            &nodeCameraAnimationLabel_);
        nodeEmotion_ = addTextField(page, form, "Emotion:", 0, wxDefaultSize, &nodeEmotionLabel_);
        nodeFacialAnim_ = addTextField(page, form, "Facial animation:", 0, wxDefaultSize,
                                      &nodeFacialAnimLabel_);

        nodeCamVidEffectLabel_ = new wxStaticText(page, wxID_ANY, "Camera video effect:");
        form->Add(nodeCamVidEffectLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        nodeCamVidEffectPanel_ = new wxPanel(page);
        auto* videoEffectRow = new wxBoxSizer(wxHORIZONTAL);
        nodeCamVidEffectChoice_ = new wxChoice(nodeCamVidEffectPanel_, wxID_ANY);
        videoEffectRow->Add(nodeCamVidEffectChoice_, 1, wxEXPAND);
        nodeCamVidEffectPanel_->SetSizer(videoEffectRow);
        form->Add(nodeCamVidEffectPanel_, 1, wxEXPAND);

        nodeFadeTypeLabel_ = new wxStaticText(page, wxID_ANY, "Fade type:");
        form->Add(nodeFadeTypeLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        nodeFadeType_ = new wxChoice(page, wxID_ANY);
        populateIntegerChoice(nodeFadeType_, nodeFadeTypeValues_, kFadeTypeOptions,
                              "0", 0, "Unknown nonzero value (runtime treats as Fade in; preserve until changed)");
        form->Add(nodeFadeType_, 1, wxEXPAND);

        nodeFadeColorLabel_ = new wxStaticText(page, wxID_ANY, "Fade color:");
        form->Add(nodeFadeColorLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        auto* colorRow = new wxBoxSizer(wxHORIZONTAL);
        nodeFadeColorPicker_ = new wxColourPickerCtrl(page, wxID_ANY, *wxBLACK);
        colorRow->Add(nodeFadeColorPicker_, 0, wxRIGHT, 8);
        nodeFadeColorRLabel_ = new wxStaticText(page, wxID_ANY, "R");
        colorRow->Add(nodeFadeColorRLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
        nodeFadeColorR_ = new wxTextCtrl(page, wxID_ANY, "0", wxDefaultPosition, FromDIP(wxSize(72, -1)));
        colorRow->Add(nodeFadeColorR_, 0, wxRIGHT, 6);
        nodeFadeColorGLabel_ = new wxStaticText(page, wxID_ANY, "G");
        colorRow->Add(nodeFadeColorGLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
        nodeFadeColorG_ = new wxTextCtrl(page, wxID_ANY, "0", wxDefaultPosition, FromDIP(wxSize(72, -1)));
        colorRow->Add(nodeFadeColorG_, 0, wxRIGHT, 6);
        nodeFadeColorBLabel_ = new wxStaticText(page, wxID_ANY, "B");
        colorRow->Add(nodeFadeColorBLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 3);
        nodeFadeColorB_ = new wxTextCtrl(page, wxID_ANY, "0", wxDefaultPosition, FromDIP(wxSize(72, -1)));
        colorRow->Add(nodeFadeColorB_, 0);
        form->Add(colorRow, 1, wxEXPAND);

        nodeFadeDelay_ = addTextFieldWithUnit(page, form, "Fade delay:", "seconds",
                                               &nodeFadeDelayLabel_, &nodeFadeDelayUnit_);
        nodeFadeLength_ = addTextFieldWithUnit(page, form, "Fade length:", "seconds",
                                                &nodeFadeLengthLabel_, &nodeFadeLengthUnit_);

        nodeAlienRace_ = addTextField(page, form, "Alien-race node:", 0, wxDefaultSize, &nodeAlienRaceLabel_);
        nodeUnskippable_ = addCheckField(page, form, "Node is unskippable", &nodeUnskippablePlaceholder_);

        nodeCameraAngle_->SetToolTip(
            "0 uses the deterministic automatic camera sequence; 1-3 are calculated presets; 6 uses Camera ID.");
        nodeCameraId_->SetToolTip(
            "Raw CameraID from the area's GIT CameraList. NeoDLG does not resolve the current area yet.");
        nodeCamHeightOffset_->SetToolTip("Finite signed vertical camera offset.");
        nodeTarHeightOffset_->SetToolTip("Finite signed vertical target offset.");
        nodeCameraFov_->SetToolTip("Custom field of view in degrees. Use Automatic to store -1.");
        nodeFadeDelay_->SetToolTip("Seconds to wait before the fade starts. Must be zero or greater.");
        nodeFadeLength_->SetToolTip("Fade duration in seconds. Must be greater than zero.");

        root->Add(form, 1, wxEXPAND | wxALL, 10);
        presentationApplyButton_ = new wxButton(page, ID_ApplyPresentation, "Apply Presentation Changes");
        root->Add(presentationApplyButton_, 0, wxALIGN_RIGHT | wxALL, 10);

        nodeCameraAngle_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            updateCameraControls();
            queueContextualInspectorRefresh();
        });
        nodeCameraFovMode_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) {
            updateCameraControls();
            queueContextualInspectorRefresh();
        });
        nodeFadeColorPicker_->Bind(wxEVT_COLOURPICKER_CHANGED,
                                   [this](wxColourPickerEvent&) { updateFadeColorTextFromPicker(); });
        const auto updatePicker = [this](wxCommandEvent&) { updateFadeColorPickerFromText(); };
        nodeFadeColorR_->Bind(wxEVT_TEXT, updatePicker);
        nodeFadeColorG_->Bind(wxEVT_TEXT, updatePicker);
        nodeFadeColorB_->Bind(wxEVT_TEXT, updatePicker);
        updateCameraControls();
    }

    void updateCameraControls() {
        if (nodeCameraAngle_ && nodeCameraId_) {
            const std::string angle = selectedIntegerChoice(nodeCameraAngle_, nodeCameraAngleValues_, "camera angle");
            const bool knownNonPlaced = angle == "0" || angle == "1" || angle == "2" || angle == "3";
            nodeCameraId_->Enable(angle == "6" || !knownNonPlaced);
        }
        if (nodeCameraFovMode_ && nodeCameraFov_) {
            nodeCameraFov_->Enable(nodeCameraFovMode_->GetSelection() == 1);
        }
    }

    void populateCameraFov(const std::string& value, bool present) {
        loadedCameraFovPresent_ = present;
        loadedCameraFovRaw_ = value;
        nodeCameraFovMode_->Clear();
        nodeCameraFovMode_->Append("Automatic");
        nodeCameraFovMode_->Append("Custom");
        nodeCameraFov_->ChangeValue("55");

        if (!present || value.empty() || value == "-1") {
            nodeCameraFovMode_->SetSelection(0);
        } else {
            try {
                const float parsed = neogff::ParseFloatDecimal(value);
                if (std::isfinite(parsed) && parsed > 0.0f) {
                    nodeCameraFovMode_->SetSelection(1);
                    nodeCameraFov_->ChangeValue(wxui::toWx(value));
                } else {
                    nodeCameraFovMode_->Append(wxui::toWx("Existing invalid value " + value + " (preserve)"));
                    nodeCameraFovMode_->SetSelection(2);
                }
            } catch (const std::exception&) {
                nodeCameraFovMode_->Append(wxui::toWx("Existing invalid value " + value + " (preserve)"));
                nodeCameraFovMode_->SetSelection(2);
            }
        }
        updateCameraControls();
    }

    std::string cameraFovValue() const {
        if (!nodeCameraFovMode_) return "-1";
        switch (nodeCameraFovMode_->GetSelection()) {
        case 0:
            return "-1";
        case 1: {
            const float value = parseFiniteFloat(nodeCameraFov_, "Camera field of view");
            if (value <= 0.0f) {
                throw std::invalid_argument("Camera field of view must be greater than 0 degrees, or set to Automatic.");
            }
            return neogff::FormatNumber(value);
        }
        case 2:
            if (!loadedCameraFovPresent_) {
                throw std::invalid_argument("The preserved camera field-of-view value is unavailable.");
            }
            return loadedCameraFovRaw_;
        default:
            throw std::invalid_argument("Select a camera field-of-view mode.");
        }
    }

    void populateVideoEffect(DlgFlavor flavor, const std::string& value, bool present) {
        loadedCamVidEffectPresent_ = present;
        loadedCamVidEffectRaw_ = value.empty() ? "-1" : value;

        const bool kotor2 = flavor == DlgFlavor::Kotor2;
        const bool kotor1 = flavor == DlgFlavor::Kotor;
        nodeCamVidEffectChoice_->Show(kotor1 || kotor2);

        if (kotor2) {
            populateIntegerChoice(nodeCamVidEffectChoice_, nodeCamVidEffectValues_,
                                  kKotor2VideoEffectOptions, loadedCamVidEffectRaw_, -1,
                                  "Unknown VideoEffects.2da row (preserve until changed)");
            nodeCamVidEffectChoice_->SetToolTip(
                "KotOR II rows from videoeffects.2da. New values are limited to the listed rows.");
        } else if (kotor1) {
            populateIntegerChoice(nodeCamVidEffectChoice_, nodeCamVidEffectValues_,
                                  kKotorVideoEffectOptions, loadedCamVidEffectRaw_, -1,
                                  "Unknown VideoEffects.2da row (preserve until changed)");
            nodeCamVidEffectChoice_->SetToolTip(
                "KotOR rows from videoeffects.2da. New values are limited to None and rows 0 through 2.");
        } else {
            nodeCamVidEffectChoice_->Clear();
            nodeCamVidEffectValues_.clear();
        }

        nodeCamVidEffectPanel_->Layout();
        refreshInspectorLayouts();
    }

    std::int32_t cameraVideoEffectValue(DlgFlavor flavor) const {
        std::int32_t value = -1;
        if (flavor == DlgFlavor::Kotor || flavor == DlgFlavor::Kotor2) {
            value = neogff::ParseInt32Decimal(selectedIntegerChoice(
                nodeCamVidEffectChoice_, nodeCamVidEffectValues_, "camera video effect"));
        } else {
            return -1;
        }

        if (value < -1) {
            bool preserveExisting = false;
            if (loadedCamVidEffectPresent_) {
                try {
                    preserveExisting = value == neogff::ParseInt32Decimal(loadedCamVidEffectRaw_);
                } catch (const std::exception&) {
                    preserveExisting = false;
                }
            }
            if (!preserveExisting) {
                throw std::invalid_argument(
                    "Camera video effect must be -1 (None) or a non-negative VideoEffects.2da row.");
            }
        }
        return value;
    }

    void populateFadeColor(const std::string& value, bool present) {
        loadedFadeColorPresent_ = present;
        loadedFadeColorRaw_ = value;
        fadeColorEdited_ = false;
        neogff::GffVector3 components{};
        if (present) {
            try {
                components = neogff::ParseGffVector3Text(value);
            } catch (const std::exception&) {
                // Preserve malformed legacy data verbatim until the user edits it,
                // while presenting a safe color in the semantic controls.
                components = {};
            }
        }
        updatingFadeColor_ = true;
        nodeFadeColorR_->ChangeValue(wxui::toWx(neogff::FormatNumber(components[0])));
        nodeFadeColorG_->ChangeValue(wxui::toWx(neogff::FormatNumber(components[1])));
        nodeFadeColorB_->ChangeValue(wxui::toWx(neogff::FormatNumber(components[2])));
        updatingFadeColor_ = false;
        updateFadeColorPickerFromText();
        fadeColorEdited_ = false;
    }

    void updateFadeColorTextFromPicker() {
        if (updatingFadeColor_ || !nodeFadeColorPicker_) return;
        const wxColour color = nodeFadeColorPicker_->GetColour();
        updatingFadeColor_ = true;
        nodeFadeColorR_->ChangeValue(wxui::toWx(neogff::FormatNumber(static_cast<float>(color.Red()) / 255.0f)));
        nodeFadeColorG_->ChangeValue(wxui::toWx(neogff::FormatNumber(static_cast<float>(color.Green()) / 255.0f)));
        nodeFadeColorB_->ChangeValue(wxui::toWx(neogff::FormatNumber(static_cast<float>(color.Blue()) / 255.0f)));
        updatingFadeColor_ = false;
        fadeColorEdited_ = true;
    }

    void updateFadeColorPickerFromText() {
        if (updatingFadeColor_ || !nodeFadeColorPicker_) return;
        fadeColorEdited_ = true;
        try {
            const float red = parseFiniteFloat(nodeFadeColorR_, "Fade color red");
            const float green = parseFiniteFloat(nodeFadeColorG_, "Fade color green");
            const float blue = parseFiniteFloat(nodeFadeColorB_, "Fade color blue");
            const auto toByte = [](float component) {
                return static_cast<unsigned char>(std::lround(std::clamp(component, 0.0f, 1.0f) * 255.0f));
            };
            updatingFadeColor_ = true;
            nodeFadeColorPicker_->SetColour(wxColour(toByte(red), toByte(green), toByte(blue)));
            updatingFadeColor_ = false;
        } catch (const std::exception&) {
            updatingFadeColor_ = false;
        }
    }

    std::string fadeColorValue() const {
        if (loadedFadeColorPresent_ && !fadeColorEdited_) return loadedFadeColorRaw_;
        const float red = parseFiniteFloat(nodeFadeColorR_, "Fade color red");
        const float green = parseFiniteFloat(nodeFadeColorG_, "Fade color green");
        const float blue = parseFiniteFloat(nodeFadeColorB_, "Fade color blue");
        for (const float component : {red, green, blue}) {
            if (component < 0.0f || component > 1.0f) {
                throw std::invalid_argument("Fade color components must be between 0.0 and 1.0.");
            }
        }
        return neogff::FormatGffVector3Text(neogff::GffVector3{red, green, blue});
    }

    void buildLinkPage(wxNotebook* book) {
        wxBoxSizer* root = nullptr;
        wxPanel* page = makeInspectorSection(book, "Link / Conditions", root);
        linkHeader_ = new wxStaticText(page, wxID_ANY, "Select a linked node to edit its conditions.");
        wxFont bold = linkHeader_->GetFont();
        bold.SetWeight(wxFONTWEIGHT_BOLD);
        linkHeader_->SetFont(bold);
        root->Add(linkHeader_, 0, wxEXPAND | wxALL, 10);

        auto* form = new wxFlexGridSizer(2, 8, 8);
        form->AddGrowableCol(1, 1);
        linkActive1_ = addTextField(page, form, "Conditional script 1:", 0, wxDefaultSize, &linkActive1Label_);
        linkActive2_ = addTextField(page, form, "Conditional script 2:", 0, wxDefaultSize, &linkActive2Label_);
        linkLogic_ = addTextField(page, form, "Logic mode:", 0, wxDefaultSize, &linkLogicLabel_);
        linkParamStrA_ = addTextField(page, form, "Conditional string A:", 0, wxDefaultSize, &linkParamStrALabel_);
        linkParamStrB_ = addTextField(page, form, "Conditional string B:", 0, wxDefaultSize, &linkParamStrBLabel_);
        linkDesignerNumber_ = addTextField(page, form, "Designer number available to script:", 0,
                                           wxDefaultSize, &linkDesignerNumberLabel_);
        linkNot1_ = addCheckField(page, form, "Negate conditional 1", &linkNot1Placeholder_);
        linkNot2_ = addCheckField(page, form, "Negate conditional 2", &linkNot2Placeholder_);
        linkReverseCond_ = addCheckField(page, form, "Negate condition", &linkReverseCondPlaceholder_);
        root->Add(form, 0, wxEXPAND | wxALL, 10);

        linkDisplayInactive_ = new wxCheckBox(page, wxID_ANY, "Display inactive reply");
        linkDisplayInactive_->SetName("NeoDLG DisplayInactive");
        linkDisplayInactive_->SetToolTip(
            "Keep this Entry-to-Reply choice displayed when its condition is false.");
        linkDisplayInactive_->Hide();
        linkDisplayInactive_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            linkDisplayInactiveEdited_ = true;
        });

        linkParamHeading_ = new wxStaticText(page, wxID_ANY, "Conditional integer parameters");
        root->Add(linkParamHeading_, 0, wxLEFT | wxRIGHT | wxTOP, 10);
        linkParamFields_ = new IntegerParameterFields(page, "Conditional 1", "Conditional 2");
        linkParamFields_->SetName("NeoDLG link parameters");
        root->Add(linkParamFields_, 0, wxEXPAND | wxALL, 10);
        root->Add(new wxButton(page, ID_ApplyLink, "Apply Link Conditions"), 0, wxALIGN_RIGHT | wxALL, 10);
    }

    void buildAnimationsPage(wxNotebook* book) {
        wxBoxSizer* root = nullptr;
        wxPanel* page = makeInspectorSection(book, "Animations", root);
        animationSummary_ = new wxStaticText(page, wxID_ANY, "No animations");
        animationSummary_->SetName("NeoDLG animation summary");
        animationSummary_->Hide();
        animationList_ = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxLC_REPORT | wxLC_SINGLE_SEL);
        animationList_->SetName("NeoDLG animation list");
        animationList_->InsertColumn(0, "Participant");
        animationList_->InsertColumn(1, "Animation ID");
        animationList_->InsertColumn(2, "Emotion ID");
        animationList_->SetColumnWidth(0, FromDIP(220));
        animationList_->SetColumnWidth(1, FromDIP(130));
        animationList_->SetColumnWidth(2, FromDIP(130));
        animationList_->SetMinSize(FromDIP(wxSize(-1, 220)));
        root->Add(animationList_, 1, wxEXPAND | wxALL, 10);
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        animationAddButton_ = new wxButton(page, ID_AnimationAdd, "Add...");
        animationEditButton_ = new wxButton(page, ID_AnimationEdit, "Edit...");
        animationDeleteButton_ = new wxButton(page, ID_AnimationDelete, "Delete");
        buttons->Add(animationAddButton_, 0, wxRIGHT, 4);
        buttons->Add(animationEditButton_, 0, wxRIGHT, 4);
        buttons->Add(animationDeleteButton_, 0);
        root->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);

        const auto selectionChanged = [this](wxListEvent& event) {
            event.Skip();
            if (singlePanelActive_) refreshCompactAnimationActions();
        };
        animationList_->Bind(wxEVT_LIST_ITEM_SELECTED, selectionChanged);
        animationList_->Bind(wxEVT_LIST_ITEM_DESELECTED, selectionChanged);
        animationList_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& event) {
            if (!singlePanelActive_) { event.Skip(); return; }
            const long row = event.GetIndex();
            if (row < 0 || static_cast<std::size_t>(row) >= animationValues_.size()) return;
            animationList_->SetItemState(row, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                         wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            wxCommandEvent edit(wxEVT_BUTTON, ID_AnimationEdit);
            onAnimationEdit(edit);
        });

        animationList_->Bind(wxEVT_LIST_ITEM_RIGHT_CLICK, [this](wxListEvent& event) {
            const long row = event.GetIndex();
            if (row < 0 || static_cast<std::size_t>(row) >= animationValues_.size()) return;
            animationList_->SetItemState(row, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                         wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            const bool jadeReply = hasActiveDocument() && activeDocument().selectedNode &&
                dialogue().dialect() == DlgDialect::JadeEmpire &&
                activeDocument().selectedNode->kind == DlgNodeKind::Reply;
            wxMenu menu;
            auto* moveUp = menu.Append(ID_AnimationUp, "Move Animation Up");
            auto* moveDown = menu.Append(ID_AnimationDown, "Move Animation Down");
            moveUp->Enable(!jadeReply && row > 0);
            moveDown->Enable(!jadeReply &&
                             static_cast<std::size_t>(row + 1) < animationValues_.size());
            animationList_->PopupMenu(&menu);
        });
    }

    void buildRawPage(wxNotebook* parent) {
        auto* page = new wxPanel(parent);
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* filterRow = new wxBoxSizer(wxHORIZONTAL);
        filterRow->Add(new wxStaticText(page, wxID_ANY, "Filter GFF fields:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        rawFilter_ = new wxTextCtrl(page, wxID_ANY);
        rawFilter_->SetName("NeoDLG GFF filter");
        filterRow->Add(rawFilter_, 1);
        rawOptional_ = new wxCheckBox(page, wxID_ANY, "Show optional fields");
        rawOptional_->SetName("NeoDLG GFF optional fields");
        rawOptional_->SetValue(optionalFields_ && optionalFields_->GetValue());
        rawOptional_->SetToolTip("Reveal context-hidden fields, but not removed authoring fields. No stored data is deleted.");
        filterRow->Add(rawOptional_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        rawOptional_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
            setShowOptionalFields(rawOptional_->GetValue());
        });
        root->Add(filterRow, 0, wxEXPAND | wxBOTTOM, 6);

        rawTree_ = new wxTreeCtrl(page, ID_RawTree, wxDefaultPosition, wxDefaultSize,
                                  wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE);
        rawTree_->SetName("NeoDLG GFF tree");
        root->Add(rawTree_, 1, wxEXPAND);
        page->SetSizer(root);
        parent->AddPage(page, "GFF Tree", false);
        rawFilter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
            if (hasActiveDocument()) {
                activeDocument().rawFilterTerm = wxui::toStd(rawFilter_->GetValue());
            }
            refreshRawTree();
        });
    }

    std::string conversationTreeItemKey(const wxTreeItemId& item) const {
        if (!conversationTree_ || !item.IsOk()) return {};
        if (auto* data = dynamic_cast<ConversationTreeData*>(conversationTree_->GetItemData(item))) {
            return data->key;
        }
        return {};
    }

    std::string rawTreeItemKey(const wxTreeItemId& item) const {
        if (!rawTree_ || !item.IsOk()) return {};
        if (item == rawTree_->GetRootItem()) return "$root";
        if (auto* data = dynamic_cast<RawGffTreeItemData*>(rawTree_->GetItemData(item))) {
            return data->path();
        }
        return {};
    }

    void captureRenderedConversationTreeState() {
        if (!conversationTree_ || !hasActiveDocument() ||
            conversationTreeRenderedDocumentPage_ != activeDocument().tabPage) {
            return;
        }
        neotree::captureTreeViewState(
            *conversationTree_, activeDocument().conversationTreeState,
            [this](const wxTreeItemId& item) { return conversationTreeItemKey(item); });
    }

    void captureRenderedRawTreeState() {
        if (!rawTree_ || !hasActiveDocument() ||
            rawTreeRenderedDocumentPage_ != activeDocument().tabPage) {
            return;
        }
        neotree::captureTreeViewState(
            *rawTree_, activeDocument().rawTreeState,
            [this](const wxTreeItemId& item) { return rawTreeItemKey(item); });
    }

    void captureRenderedTreeStates() {
        captureRenderedConversationTreeState();
        captureRenderedRawTreeState();
    }

    void createDocumentTab(bool select) {
        if (select && hasActiveDocument()) captureRenderedTreeStates();
        DocumentTab tab;
        const std::size_t previous = activeDocumentIndex_;
        documents_.push_back(std::move(tab));
        const std::size_t index = documents_.size() - 1;
        tabSwitchInProgress_ = true;
        wxWindow* page = neotabs::addTabPage(documentTabs_, tabDisplayName(documents_.back()), false, select);
        tabSwitchInProgress_ = false;
        if (!page) {
            documents_.pop_back();
            activeDocumentIndex_ = previous;
            throw std::runtime_error("Unable to create a document tab.");
        }
        documents_.back().tabPage = page;
        if (select) {
            activeDocumentIndex_ = index;
            tabSwitchInProgress_ = true;
            neotabs::changeSelectionToPage(documentTabs_, page);
            tabSwitchInProgress_ = false;
        }
    }

    bool activeTabReusable() const {
        return hasActiveDocument() && documents_.size() == 1 && !model().loaded() && !model().dirty();
    }

    void ensureTabForOpen() {
        if (!hasActiveDocument()) createDocumentTab(true);
        else if (!activeTabReusable()) createDocumentTab(true);
    }

#if defined(__EMSCRIPTEN__)
    using BrowserImportCallback = std::function<void(neobrowser::BrowserImportLease)>;

    void requestBrowserImport(const std::string& title,
                              const std::string& accept,
                              bool multiple,
                              BrowserImportCallback callback) {
        wxWeakRef<NeoDLGPanelImpl> weakSelf(this);
        neobrowser::requestOpenFilesOwned(
            title, accept, multiple,
            [weakSelf, callback = std::move(callback)](
                neobrowser::OwnedOpenFilesResult result) mutable {
                if (!weakSelf || weakSelf->IsBeingDeleted()) return;
                auto* const frame = weakSelf.get();
                if (!result.error.empty()) {
                    wxMessageBox(wxui::toWx(result.error), "File Open Error",
                                 wxOK | wxICON_ERROR, frame);
                    return;
                }
                if (result.cancelled()) return;
                callback(std::move(result.import));
            });
    }

    static bool importOwnsPath(const neobrowser::BrowserImportLease& import,
                               const std::filesystem::path& path) {
        return std::find(import.paths().begin(), import.paths().end(), path) !=
               import.paths().end();
    }
#endif

    void selectDocumentTab(std::size_t index) {
        if (index >= documents_.size()) return;
        if (hasActiveDocument() && index != activeDocumentIndex_) captureRenderedTreeStates();
        tabSwitchInProgress_ = true;
        neotabs::changeSelectionToPage(documentTabs_, documents_[index].tabPage);
        tabSwitchInProgress_ = false;
        activeDocumentIndex_ = index;
        if (rawFilter_) rawFilter_->ChangeValue(wxui::toWx(activeDocument().rawFilterTerm));
        setWorkspaceView(activeDocument().workspaceView, false);
        refreshAll();
    }

    bool confirmCloseDocument(std::size_t index) {
        if (index >= documents_.size()) return true;
        if (documents_[index].saveInProgress) {
            wxui::showMessage(this, "Save in progress",
                              "Finish the browser save transaction before closing this tab.");
            return false;
        }
        if (!tabDirty(documents_[index])) return true;
        return wxui::confirm(this, "Close tab", neotabs::closePromptText(tabDisplayName(documents_[index])));
    }

    bool closeDocument(std::size_t index) {
        if (index >= documents_.size() || !confirmCloseDocument(index)) return false;
        wxWindow* page = documents_[index].tabPage;
        if (conversationTreeRenderedDocumentPage_ == page) conversationTreeRenderedDocumentPage_ = nullptr;
        if (rawTreeRenderedDocumentPage_ == page) rawTreeRenderedDocumentPage_ = nullptr;
        tabSwitchInProgress_ = true;
        const bool deleted = neotabs::deleteTabPage(documentTabs_, page);
        tabSwitchInProgress_ = false;
        if (!deleted) return false;
        documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(index));
        if (documents_.empty()) {
            activeDocumentIndex_ = neotabs::npos;
            createDocumentTab(true);
            setWorkspaceView(activeDocument().workspaceView, false);
            refreshAll();
            return true;
        }
        std::size_t selected = neotabs::findDocumentIndexForPage(documents_, neotabs::currentPage(documentTabs_));
        if (selected == neotabs::npos) selected = std::min(index, documents_.size() - 1);
        selectDocumentTab(selected);
        return true;
    }

    void updateDocumentTabTitle(DocumentTab& document) {
        neotabs::setTabLabel(documentTabs_, document.tabPage,
                             tabDisplayName(document), tabDirty(document));
    }

    void updateTabTitle() {
        if (!hasActiveDocument()) return;
        updateDocumentTabTitle(activeDocument());
    }

    void newDocument(DlgFlavor flavor) {
        try {
            if (!activeTabReusable()) createDocumentTab(true);
            DlgDocument document(model());
            document.create(flavor);
            activeDocument().logicalFilename.clear();
#if defined(__EMSCRIPTEN__)
            activeDocument().sourceImport.reset();
#endif
            activeDocument().untitledName = "Untitled " + flavorName(flavor) + " DLG";
            activeDocument().undo.clear();
            activeDocument().redo.clear();
            activeDocument().selectedNode.reset();
            activeDocument().selectedLink.reset();
            activeDocument().conversationTreeState.reset();
            activeDocument().rawTreeState.reset();
            activeDocument().rawFilterTerm.clear();
            if (conversationTreeRenderedDocumentPage_ == activeDocument().tabPage) conversationTreeRenderedDocumentPage_ = nullptr;
            if (rawTreeRenderedDocumentPage_ == activeDocument().tabPage) rawTreeRenderedDocumentPage_ = nullptr;
            if (rawFilter_) rawFilter_->ChangeValue(wxString{});
            setWorkspaceView(WorkspaceView::Conversation);
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void chooseAndOpenDlg(const std::filesystem::path& initialDirectory = {}) {
#if defined(__EMSCRIPTEN__)
        (void)initialDirectory;
        requestBrowserImport(
            "Open DLG", ".dlg", false,
            [this](neobrowser::BrowserImportLease import) {
                if (import.empty() || IsBeingDeleted()) return;
                try {
                    const std::filesystem::path selectedPath = import.paths().front();
                    openModelPath(selectedPath, std::move(import));
                } catch (const std::exception& ex) {
                    wxui::showError(this, ex);
                }
            });
#else
        const auto path = wxui::chooseOpenFile(this, "Open DLG", kDlgWildcard, initialDirectory);
        if (path) openModelPath(*path);
#endif
    }

    bool openModelPath(const std::filesystem::path& path) {
        if (path.empty()) return false;
        for (std::size_t i=0; i<documents_.size(); ++i) {
            if (neoshared::sameResourcePath(path, documentFilename(documents_[i]))) {
                selectDocumentTab(i); return true;
            }
        }


        auto candidate = std::make_unique<GffModel>();
        loadDlgModel(*candidate, path);
        if (!neoshared::sameGffResourceType(candidate->fileType(), "DLG ")) {
            throw std::invalid_argument("The selected file is not a DLG resource.");
        }

        ensureTabForOpen();
        activeDocument().model = std::move(candidate);
        activeDocument().logicalFilename = path;
        activeDocument().resourceIdentity.clear();
        activeDocument().sourceDescription.clear();
        activeDocument().protectedInputs.clear();
#if defined(__EMSCRIPTEN__)
        activeDocument().sourceImport.reset();
#endif
        activeDocument().undo.clear();
        activeDocument().redo.clear();
        activeDocument().selectedNode.reset();
        activeDocument().selectedLink.reset();
        activeDocument().conversationTreeState.reset();
        activeDocument().rawTreeState.reset();
        activeDocument().rawFilterTerm.clear();
        if (conversationTreeRenderedDocumentPage_ == activeDocument().tabPage) conversationTreeRenderedDocumentPage_ = nullptr;
        if (rawTreeRenderedDocumentPage_ == activeDocument().tabPage) rawTreeRenderedDocumentPage_ = nullptr;
        if (rawFilter_) rawFilter_->ChangeValue(wxString{});
        tryLoadResolvedTlkForPath(path);
        rememberRecentFile(path);
        neogames::resolver().inferFromOpenedPath(path);
        const bool semantic = dialogue().semanticallyEditable();
        setWorkspaceView(semantic ? WorkspaceView::Conversation : WorkspaceView::Raw);
        refreshAll();
        return true;
    }

#if defined(__EMSCRIPTEN__)
    bool openModelPath(const std::filesystem::path& path,
                       neobrowser::BrowserImportLease import) {
        if (!openModelPath(path)) return false;
        activeDocument().sourceImport = std::move(import);
        return true;
    }
#endif

    void onOpen(wxCommandEvent&) {
        try { chooseAndOpenDlg(); } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void onOpenRecent(wxCommandEvent& event) {
        if (event.GetId() == kClearRecentFilesId) {
            settings_.clearRecentFiles();
            rebuildRecentFilesMenu();
            return;
        }
        const int index = event.GetId() - kRecentFileBaseId;
        const auto files = settings_.recentFiles();
        if (index < 0 || static_cast<std::size_t>(index) >= files.size()) return;
        try { openModelPath(files[static_cast<std::size_t>(index)]); }
        catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void onSave(wxCommandEvent&) { save(false); }
    void onSaveAs(wxCommandEvent&) { save(true); }

    void onDialogueInformation(wxCommandEvent&) {
        const DialogueSummary summary = currentDialogueSummary();
        wxDialog dialog(this, wxID_ANY, "Dialogue Information", wxDefaultPosition,
                        wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* form = new wxFlexGridSizer(2, FromDIP(8), FromDIP(8));
        form->AddGrowableCol(1, 1);

        const auto addValue = [&](const wxString& label,
                                  const wxString& value,
                                  bool multiline) {
            form->Add(new wxStaticText(&dialog, wxID_ANY, label), 0,
                      wxALIGN_TOP | wxRIGHT, FromDIP(8));
            long style = wxTE_READONLY;
            if (multiline) style |= wxTE_MULTILINE | wxTE_WORDWRAP;
            auto* control = new wxTextCtrl(&dialog, wxID_ANY, value,
                                           wxDefaultPosition, wxDefaultSize, style);
            if (multiline) control->SetMinSize(FromDIP(wxSize(480, 58)));
            form->Add(control, 1, wxEXPAND);
        };

        addValue("File:", summary.file, true);
        addValue("Format:", summary.type, false);
        addValue("Contents:", summary.statistics, false);
        addValue("TLK:", summary.tlk, true);
        if (!summary.warning.empty()) addValue("TLK warning:", summary.warning, true);

        root->Add(form, 1, wxEXPAND | wxALL, FromDIP(12));
        root->Add(dialog.CreateStdDialogButtonSizer(wxOK), 0,
                  wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        dialog.SetSizerAndFit(root);
        wxui::configureResponsiveWindow(dialog, wxSize(720, 420), wxSize(520, 300));
        dialog.CentreOnParent();
        wxui::constrainWindowToDisplay(dialog);
        dialog.ShowModal();
    }

    bool save(bool saveAs, const std::filesystem::path& forcedTarget = {}) {
        if (!hasActiveDocument() || !model().loaded()) return false;
        if (activeDocument().saveInProgress || browserSaveActive_) return false;
        try {
            DocumentTab& document = activeDocument();
            std::filesystem::path target = forcedTarget.empty() ? documentFilename(document) : forcedTarget;
            if ((saveAs && forcedTarget.empty()) || target.empty()) {
                std::string name = target.empty() ? (document.resourceIdentity.empty() ? "new.dlg" : document.untitledName) : neosettings::pathToUtf8(target.filename());
                const auto chosen = wxui::chooseSaveFile(this, "Save DLG", kDlgWildcard, name);
                if (!chosen) return false;
                target = ensureDlgExtension(*chosen);
            }

            checkDestination(target);
            removeRetiredDlgFields(*document.model);
#if defined(__EMSCRIPTEN__)
            const bool wasDirty = document.model->dirty();
#endif
            saveDlgModel(*document.model, target);

#if defined(__EMSCRIPTEN__)
            document.saveInProgress = true;
            browserSaveActive_ = true;
            updateDocumentTabTitle(document);
            refreshHeader();
            Enable(false);

            wxWeakRef<NeoDLGPanelImpl> weakSelf(this);
            wxWindow* const targetPage = document.tabPage;
            neobrowser::requestDownloadFile(
                target,
                target.filename().string(),
                [weakSelf, targetPage, target, wasDirty](neobrowser::DownloadResult result) {
                    if (!weakSelf || weakSelf->IsBeingDeleted()) return;
                    auto* const frame = weakSelf.get();
                    frame->browserSaveActive_ = false;
                    frame->Enable(true);

                    const std::size_t index = neotabs::findDocumentIndexForPage(
                        frame->documents_, targetPage);
                    if (index == neotabs::npos) return;

                    DocumentTab& savedDocument = frame->documents_[index];
                    savedDocument.saveInProgress = false;
                    if (!result.error.empty() || result.cancelled()) {
                        savedDocument.model->gff().dirty(wasDirty);
                        frame->updateDocumentTabTitle(savedDocument);
                        if (index == frame->activeDocumentIndex_) frame->refreshHeader();
                        const std::string message = result.error.empty()
                            ? "The browser save transaction was cancelled."
                            : result.error;
                        wxMessageBox(wxui::toWx(message), "Save Failed",
                                     wxOK | wxICON_ERROR, frame);
                        return;
                    }

                    savedDocument.logicalFilename = target;
                    savedDocument.model->gff().dirty(false);
                    if (!frame->importOwnsPath(savedDocument.sourceImport, target)) {
                        savedDocument.sourceImport.reset();
                    }
                    frame->rememberRecentFile(target);
                    neogames::resolver().inferFromOpenedPath(target);
                    frame->updateDocumentTabTitle(savedDocument);
                    if (index == frame->activeDocumentIndex_) frame->refreshAll();

                    if (result.ready()) {
                        wxui::showMessage(
                            frame,
                            "Replacement download ready",
                            "The browser could not overwrite the original host file directly. "
                            "A replacement DLG is ready in the download panel; download it before closing this page.");
                    }
                });
            return true;
#else
            document.logicalFilename = target;
            document.model->gff().dirty(false);
            rememberRecentFile(target);
            neogames::resolver().inferFromOpenedPath(target);
            updateTabTitle();
            refreshHeader();
            return true;
#endif
        } catch (const std::exception& ex) {
            if (!forcedTarget.empty()) throw;
            wxui::showError(this, ex);
            return false;
        }
    }

    void tryLoadResolvedTlkForPath(const std::filesystem::path& path) {
        if (model().tlk().loaded()) return;
        const auto resolved = neogames::resolver().bestTlkForPath(path);
        if (!resolved || resolved->empty()) return;
        try {
            model().loadTlk(*resolved);
            settings_.setLastTlkPath(*resolved);
            activeDocument().tlkAutoLoadWarning.clear();
        } catch (const std::exception& ex) {
            activeDocument().tlkAutoLoadWarning = ex.what();
        }
    }

    void tryLoadCachedTlk() {
        if (!hasActiveDocument()) return;
#if defined(__EMSCRIPTEN__)
        // Browser-import paths are process-local and cannot be reused across page loads.
        settings_.clearLastTlkPath();
#else
        const auto path = settings_.lastTlkPath();
        if (!path || path->empty()) return;
        try { model().loadTlk(*path); }
        catch (const std::exception&) { settings_.clearLastTlkPath(); }
#endif
    }

    bool loadTlkFromPath(const std::filesystem::path& chosen, bool rememberPath = true) {
        try {
            model().loadTlk(chosen);
            if (rememberPath) settings_.setLastTlkPath(chosen);
            else settings_.clearLastTlkPath();
            activeDocument().tlkAutoLoadWarning.clear();
            refreshAll();
            return true;
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
            return false;
        }
    }

    void onOpenTlk(wxCommandEvent&) {
#if defined(__EMSCRIPTEN__)
        if (!hasActiveDocument()) return;
        wxWindow* const targetPage = activeDocument().tabPage;
        requestBrowserImport(
            "Open TLK", ".tlk", false,
            [this, targetPage](neobrowser::BrowserImportLease import) {
                if (import.empty() || IsBeingDeleted()) return;
                if (!hasActiveDocument() || activeDocument().tabPage != targetPage) {
                    wxui::showMessage(
                        this,
                        "TLK Load Cancelled",
                        "The active document changed while the TLK picker was open. Select the TLK again from the intended tab.");
                    return;
                }
                // TlkLookup owns all decoded strings after load, so this one-shot
                // import can be released as soon as the callback returns.
                loadTlkFromPath(import.paths().front(), false);
            });
#else
        const auto chosen = wxui::chooseOpenFile(this, "Open TLK", kTlkWildcard);
        if (!chosen) return;
        loadTlkFromPath(*chosen);
#endif
    }

    void onClearTlk(wxCommandEvent&) {
        if (!hasActiveDocument()) return;
        model().clearTlk();
        settings_.clearLastTlkPath();
        refreshAll();
    }

    void rebuildRecentFilesMenu() {
        if (recentFilesMenu_) neosettings::populateRecentFilesMenu(*recentFilesMenu_, settings_, kRecentFileBaseId, kClearRecentFilesId);
    }

    void rememberRecentFile(const std::filesystem::path& path) {
        settings_.addRecentFile(path);
        rebuildRecentFilesMenu();
    }

    template <typename Function>
    bool mutate(const std::string& description, Function&& function) {
        if (!hasActiveDocument() || !model().loaded()) return false;
        std::string snapshot;
        try {
            if (!model().gff().isGff4()) snapshot = model().toXml();
            try {
                function();
                removeRetiredDlgFields(model());
            } catch (...) {
                if (!snapshot.empty()) {
                    try {
                        importDlgModelXml(model(), snapshot);
                    } catch (...) {
                        // Preserve the original operation error. The next open/save
                        // action can still recover from the on-disk file.
                    }
                }
                throw;
            }
            if (!snapshot.empty()) {
                activeDocument().undo.push_back({description, std::move(snapshot)});
                if (activeDocument().undo.size() > kUndoLimit) activeDocument().undo.erase(activeDocument().undo.begin());
                activeDocument().redo.clear();
            }
            refreshAll();
            return true;
        } catch (const std::exception& ex) {
            refreshAll();
            wxui::showError(this, ex);
            return false;
        }
    }

    void onUndo(wxCommandEvent&) {
        if (!hasActiveDocument() || activeDocument().undo.empty()) return;
        try {
            UndoSnapshot snapshot = std::move(activeDocument().undo.back());
            activeDocument().undo.pop_back();
            activeDocument().redo.push_back({snapshot.description, model().toXml()});
            importDlgModelXml(model(), snapshot.xml);
            refreshAll();
        } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void onRedo(wxCommandEvent&) {
        if (!hasActiveDocument() || activeDocument().redo.empty()) return;
        try {
            UndoSnapshot snapshot = std::move(activeDocument().redo.back());
            activeDocument().redo.pop_back();
            activeDocument().undo.push_back({snapshot.description, model().toXml()});
            importDlgModelXml(model(), snapshot.xml);
            refreshAll();
        } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void onAddStartingEntry(wxCommandEvent&) {
        if (!dialogue().semanticallyEditable()) return;
        mutate("Add starting entry", [this]() {
            DlgNodeRef node = dialogue().addStartingEntry();
            activeDocument().selectedNode = node;
            activeDocument().selectedLink = dialogue().startingLinks().back();
        });
    }

    void onAddChild(wxCommandEvent&) {
        if (!activeDocument().selectedNode || !dialogue().semanticallyEditable()) return;
        const DlgNodeRef parent = *activeDocument().selectedNode;
        mutate("Add child dialogue node", [this, parent]() {
            DlgNodeRef child = dialogue().addChildNode(parent);
            activeDocument().selectedNode = child;
            const auto links = dialogue().outgoingLinks(parent);
            activeDocument().selectedLink = links.empty() ? std::optional<DlgLinkRef>{} : links.back();
        });
    }

    std::optional<DlgNodeRef> chooseExistingNode(DlgNodeKind kind, const wxString& title) {
        DlgDocument document = dialogue();
        std::vector<neodlggui::ExistingNodeChoice> choices;
        choices.reserve(document.nodeCount(kind));

        for (std::size_t i = 0; i < document.nodeCount(kind); ++i) {
            const DlgNodeRef ref{kind, i};
            const DlgTextValue textValue = document.text(ref);

            std::string visible = !textValue.localText.empty()
                ? textValue.localText
                : textValue.resolvedText;
            if (visible.empty() && textValue.strref != 0xFFFFFFFFu) {
                visible = "StrRef " + std::to_string(textValue.strref);
            }
            if (visible.empty()) visible = "(no text)";
            visible = singleLineText(std::move(visible));

            const std::string nodeId = document.nodeIdText(ref);
            const std::string speaker = document.speaker(ref);
            std::string search = std::to_string(i) + " " + nodeId + " " + speaker + " " +
                                 textValue.localText + " " + textValue.resolvedText;
            if (textValue.strref != 0xFFFFFFFFu) {
                search += " StrRef " + std::to_string(textValue.strref);
            }

            wxString visiblePreview = wxui::toWx(visible);
            constexpr std::size_t kNodePreviewCharacters = 360;
            if (visiblePreview.length() > kNodePreviewCharacters) {
                visiblePreview = visiblePreview.Left(kNodePreviewCharacters - 3) + "...";
            }

            choices.push_back({
                ref,
                wxui::toWx(std::to_string(i)),
                wxui::toWx(nodeId),
                wxui::toWx(speaker),
                std::move(visiblePreview),
                wxui::toWx(singleLineText(std::move(search))).Lower(),
            });
        }

        if (choices.empty()) {
            wxui::showMessage(this, "Link Existing Node", "There are no existing nodes of the required type.");
            return std::nullopt;
        }

        neodlggui::ExistingNodeDialog dialog(this, title, std::move(choices), darkMode_);
        if (dialog.ShowModal() != wxID_OK) return std::nullopt;
        return dialog.selectedNode();
    }

    void onLinkExisting(wxCommandEvent&) {
        if (!dialogue().semanticallyEditable()) return;
        if (!activeDocument().selectedNode) {
            const auto entry = chooseExistingNode(DlgNodeKind::Entry, "Link Existing Starting Entry");
            if (!entry) return;
            mutate("Link existing starting entry", [this, entry]() {
                activeDocument().selectedLink = dialogue().linkExistingStartingEntry(*entry);
                activeDocument().selectedNode = *entry;
            });
            return;
        }
        const DlgNodeRef parent = *activeDocument().selectedNode;
        const DlgNodeKind required = parent.kind == DlgNodeKind::Entry ? DlgNodeKind::Reply : DlgNodeKind::Entry;
        const auto child = chooseExistingNode(required, "Link Existing Child Node");
        if (!child) return;
        mutate("Link existing child node", [this, parent, child]() {
            activeDocument().selectedLink = dialogue().linkExistingChild(parent, *child);
            activeDocument().selectedNode = *child;
        });
    }

    void onDuplicateNode(wxCommandEvent&) {
        if (!activeDocument().selectedNode || !dialogue().semanticallyEditable()) return;
        const DlgNodeRef source = *activeDocument().selectedNode;
        const std::optional<DlgLinkRef> sourceLink = activeDocument().selectedLink;
        mutate("Duplicate dialogue node", [this, source, sourceLink]() {
            DlgDocument document = dialogue();
            const DlgNodeRef copy = document.duplicateNode(source);
            activeDocument().selectedNode = copy;
            activeDocument().selectedLink.reset();
            if (sourceLink) {
                if (sourceLink->owner == DlgLinkOwner::StartingList) {
                    DlgLinkRef added = document.linkExistingStartingEntry(copy);
                    while (added.position > sourceLink->position + 1) {
                        document.moveLink(added, -1);
                        --added.position;
                    }
                    document.copyLinkProperties(*sourceLink, added);
                    activeDocument().selectedLink = added;
                } else {
                    const DlgNodeRef parent{
                        sourceLink->owner == DlgLinkOwner::Entry ? DlgNodeKind::Entry : DlgNodeKind::Reply,
                        sourceLink->ownerIndex};
                    DlgLinkRef added = document.linkExistingChild(parent, copy);
                    while (added.position > sourceLink->position + 1) {
                        document.moveLink(added, -1);
                        --added.position;
                    }
                    document.copyLinkProperties(*sourceLink, added);
                    activeDocument().selectedLink = added;
                }
            }
        });
    }

    void onRemoveLink(wxCommandEvent&) {
        if (!activeDocument().selectedLink || !dialogue().semanticallyEditable()) return;
        if (!wxui::confirm(this, "Remove Link",
                           "Remove this occurrence from the conversation graph?\nThe target node will remain available if linked elsewhere.")) return;
        const DlgLinkRef ref = *activeDocument().selectedLink;
        mutate("Remove dialogue link", [this, ref]() {
            dialogue().removeLink(ref);
            activeDocument().selectedLink.reset();
        });
    }

    void onDeleteNode(wxCommandEvent&) {
        if (!activeDocument().selectedNode || !dialogue().semanticallyEditable()) return;
        const DlgNodeRef ref = *activeDocument().selectedNode;
        const DlgDocument document = dialogue();
        const std::size_t incoming = document.incomingLinks(ref).size();
        const std::size_t outgoing = document.outgoingLinks(ref).size();
        const std::string message =
            "Delete " + document.nodeKindName(ref.kind) + " " + std::to_string(ref.index) +
            " from the node list and remove all " + std::to_string(incoming) +
            " incoming links?\nIts " + std::to_string(outgoing) + " outgoing links will also be removed.";
        if (!wxui::confirm(this, "Delete Node Everywhere", message)) return;
        mutate("Delete dialogue node", [this, ref]() {
            dialogue().deleteNodeEverywhere(ref);
            activeDocument().selectedNode.reset();
            activeDocument().selectedLink.reset();
        });
    }

    void moveSelectedLink(int delta) {
        if (!activeDocument().selectedLink || !dialogue().semanticallyEditable()) return;
        DlgLinkRef ref = *activeDocument().selectedLink;
        const std::ptrdiff_t target = static_cast<std::ptrdiff_t>(ref.position) + delta;
        const std::size_t count = dialogue().linkCount(ref);
        if (target < 0 || target >= static_cast<std::ptrdiff_t>(count)) return;
        mutate(delta < 0 ? "Move dialogue choice up" : "Move dialogue choice down", [this, ref, delta, target]() mutable {
            dialogue().moveLink(ref, delta);
            ref.position = static_cast<std::size_t>(target);
            activeDocument().selectedLink = ref;
        });
    }

    void onConversationProperties(wxCommandEvent&) {
        if (!dialogue().semanticallyEditable()) return;
        ConversationPropertiesDialog dialog(this, dialogue());
        wxui::applyTheme(&dialog, darkMode_);
        if (dialog.ShowModal() != wxID_OK) return;
        mutate("Edit conversation properties", [this, &dialog]() {
            auto document = dialogue();
            dialog.apply(document);
        });
    }

    void onValidate(wxCommandEvent&) {
        if (!model().loaded()) return;
        ValidationDialog dialog(this, dialogue(), dialogue().validate());
        wxui::applyTheme(&dialog, darkMode_);
        if (dialog.ShowModal() != wxID_OK) return;
        const auto issue = dialog.selectedIssue();
        if (!issue) return;
        if (issue->node) selectSemanticNode(*issue->node, issue->link);
    }

    void onFindNext(wxCommandEvent&) {
        if (!dialogue().semanticallyEditable()) return;
        const std::string term = wxui::toStd(findText_->GetValue());
        if (term.empty()) return;
        if (term != lastSearchTerm_) {
            lastSearchTerm_ = term;
            searchResults_ = dialogue().search(term);
            searchIndex_ = 0;
        } else if (!searchResults_.empty()) {
            searchIndex_ = (searchIndex_ + 1) % searchResults_.size();
        }
        if (searchResults_.empty()) {
            wxui::showMessage(this, "Find", "No dialogue nodes matched the search text.");
            return;
        }
        selectSemanticNode(searchResults_[searchIndex_], std::nullopt);
        setModuleStatusText(wxString::Format("Match %zu of %zu", searchIndex_ + 1, searchResults_.size()), 1);
    }

    void onApplyNode(wxCommandEvent&) {
        if (!activeDocument().selectedNode) return;
        const DlgNodeRef ref = *activeDocument().selectedNode;
        mutate("Edit dialogue line", [this, ref]() {
            DlgDocument document = dialogue();
            DlgTextValue value = document.text(ref);
            value.strref = parseStrRef(nodeStrRef_->GetValue());
            value.stringType = nodeStringType_->IsEnabled()
                                   ? neogff::ParseUInt32Decimal(wxui::toStd(nodeStringType_->GetValue()))
                                   : value.stringType;
            value.localText = nodeLocalText_->IsEnabled() ? wxui::toStd(nodeLocalText_->GetValue()) : std::string{};
            document.setText(ref, value);

            const bool jade = document.dialect() == DlgDialect::JadeEmpire;
            if (jade) {
                if (ref.kind == DlgNodeKind::Entry) {
                    const auto tags = document.speakerTags();
                    const auto checkedParticipant = [&](wxComboBox* control,
                                                        const char* label,
                                                        const char* field,
                                                        std::int32_t fallback) {
                        const std::int32_t value = indexedComboValue(control, label);
                        std::int32_t stored = fallback;
                        try {
                            const std::string existing = document.nodeField(ref, field);
                            if (!existing.empty()) stored = neogff::ParseInt32Decimal(existing);
                        } catch (const std::exception&) {
                            stored = fallback;
                        }
                        const bool known = value == -1 || value == -2 ||
                                           (value >= 0 && static_cast<std::size_t>(value) < tags.size());
                        if (!known && value != stored) {
                            throw std::out_of_range(std::string(label) +
                                                    " must identify a TagList participant, -1, or -2.");
                        }
                        return value;
                    };
                    document.setNodeField(ref, "SpeakerIndex", FIELD_TYPE_INT,
                                          std::to_string(checkedParticipant(
                                              nodeSpeaker_, "Speaker participant", "SpeakerIndex", -1)));
                    document.setNodeField(ref, "ListenerIndex", FIELD_TYPE_INT,
                                          std::to_string(checkedParticipant(
                                              nodeListener_, "Listener participant", "ListenerIndex", -2)));
                    setOptionalNodeField(document, ref, "VoiceOver", FIELD_TYPE_CEXOSTRING,
                                         nodeVo_->GetValue(), false);
                    setOptionalNodeField(document, ref, "Skippable", FIELD_TYPE_BYTE,
                                         wxui::toWx(boolText(nodeJadeSkippable_)),
                                         document.hasNodeField(ref, "Skippable") || !nodeJadeSkippable_->GetValue());
                }
            } else {
                if (ref.kind == DlgNodeKind::Entry || document.hasNodeField(ref, "Speaker") ||
                    !nodeSpeaker_->GetValue().empty())
                    setOptionalNodeField(document, ref, "Speaker", FIELD_TYPE_CEXOSTRING, nodeSpeaker_->GetValue(), true);
                setOptionalNodeField(document, ref, "Listener", FIELD_TYPE_CEXOSTRING, nodeListener_->GetValue(), false);
                setOptionalNodeField(document, ref, "VO_ResRef", FIELD_TYPE_RESREF, nodeVo_->GetValue(), false);
                setOptionalNodeField(document, ref, "Comment", FIELD_TYPE_CEXOSTRING, nodeComment_->GetValue(), false);
            }
        });
    }

    void onApplyScripts(wxCommandEvent&) {
        refreshContextualInspector();
        if (!activeDocument().selectedNode) return;
        const DlgNodeRef ref = *activeDocument().selectedNode;
        mutate("Edit dialogue scripts", [this, ref]() {
            DlgDocument document = dialogue();
            const bool jade = document.dialect() == DlgDialect::JadeEmpire;
            setOptionalNodeField(document, ref, "Script", FIELD_TYPE_RESREF,
                                 nodeScript1_->GetValue(), false);

            if (jade) {
                if (ref.kind == DlgNodeKind::Entry) {
                    setOptionalNodeField(document, ref, "ScriptEntry", FIELD_TYPE_RESREF,
                                         nodeScript2_->GetValue(), false);
                    const auto setDefaultedCameraScript = [&](const char* field,
                                                              wxTextCtrl* control,
                                                              const char* runtimeDefault) {
                        const std::string text = trimAscii(wxui::toStd(control->GetValue()));
                        if (document.hasNodeField(ref, field) || text != runtimeDefault) {
                            document.setNodeField(ref, field, FIELD_TYPE_RESREF, text);
                        }
                    };
                    setDefaultedCameraScript("ScriptCamEntry", nodeScriptCamEntry_, "camscrentdef");
                    setOptionalNodeField(document, ref, "CameraEntry", FIELD_TYPE_CEXOSTRING,
                                         wxui::toWx(lowerAsciiValue(trimAscii(wxui::toStd(nodeCameraEntry_->GetValue())))),
                                         false);
                    setDefaultedCameraScript("ScriptCamReplies", nodeScriptCamReplies_, "camscrrepdef");
                    setOptionalNodeField(document, ref, "CameraReplies", FIELD_TYPE_CEXOSTRING,
                                         wxui::toWx(lowerAsciiValue(trimAscii(wxui::toStd(nodeCameraReplies_->GetValue())))),
                                         false);
                }
                return;
            }

            setOptionalNodeField(document, ref, "Script2", FIELD_TYPE_RESREF,
                                 nodeScript2_->GetValue(), false);
            setOptionalNodeField(document, ref, "Quest", FIELD_TYPE_CEXOSTRING,
                                 nodeQuest_->GetValue(), false);
            setOptionalNodeField(document, ref, "QuestEntry", FIELD_TYPE_DWORD,
                                 nodeQuestEntry_->GetValue(), false);
            setOptionalNodeField(document, ref, "PlotIndex", FIELD_TYPE_INT,
                                 nodePlotIndex_->GetValue(), false);
            setOptionalNodeField(document, ref, "PlotXPPercentage", FIELD_TYPE_FLOAT,
                                 nodePlotXp_->GetValue(), false);
            setOptionalNodeField(document, ref, "ActionParamStrA", FIELD_TYPE_CEXOSTRING,
                                 nodeActionStrA_->GetValue(), false);
            setOptionalNodeField(document, ref, "ActionParamStrB", FIELD_TYPE_CEXOSTRING,
                                 nodeActionStrB_->GetValue(), false);
            for (int i = 0; i < 5; ++i) {
                setOptionalNodeField(document, ref, "ActionParam" + std::to_string(i + 1), FIELD_TYPE_INT,
                                     actionParamFields_->GetCellValue(i, 0), false);
                setOptionalNodeField(document, ref, "ActionParam" + std::to_string(i + 1) + "b", FIELD_TYPE_INT,
                                     actionParamFields_->GetCellValue(i, 1), false);
            }
        });
    }

    void onApplyPresentation(wxCommandEvent&) {
        refreshContextualInspector();
        if (!activeDocument().selectedNode) return;
        const DlgNodeRef ref = *activeDocument().selectedNode;
        mutate("Edit dialogue presentation", [this, ref]() {
            DlgDocument document = dialogue();
            const bool jade = document.dialect() == DlgDialect::JadeEmpire;
            const DlgFlavor flavor = document.flavor();

            if (jade) return;

            setOptionalNodeField(document, ref, "Sound", FIELD_TYPE_RESREF, nodeSound_->GetValue(), false);
            setOptionalNodeField(document, ref, "Delay", FIELD_TYPE_DWORD, nodeDelay_->GetValue(), false);
            setOptionalNodeField(document, ref, "WaitFlags", FIELD_TYPE_DWORD, nodeWaitFlags_->GetValue(), false);

            if (!jade) {
                const std::string cameraAngle = selectedIntegerChoice(
                    nodeCameraAngle_, nodeCameraAngleValues_, "camera angle");
                setOptionalNodeField(document, ref, "CameraAngle", FIELD_TYPE_DWORD,
                                     wxui::toWx(cameraAngle), false);

                if (cameraAngle == "6") {
                    const std::int32_t cameraId = neogff::ParseInt32Decimal(
                        trimAscii(wxui::toStd(nodeCameraId_->GetValue())));
                    setOptionalNodeField(document, ref, "CameraID", FIELD_TYPE_INT,
                                         wxString::Format("%d", cameraId), true);
                } else if (nodeCameraId_->IsShown() && nodeCameraId_->IsModified()) {
                    // An inactive camera ID is preserved in either layout.
                    // The optional override permits an intentional edit only.
                    setOptionalNodeField(document, ref, "CameraID", FIELD_TYPE_INT,
                                         nodeCameraId_->GetValue(), false);
                }

                const auto setOptionalFloat = [&](const char* label,
                                                  wxTextCtrl* control,
                                                  const std::string& fieldName,
                                                  const std::function<void(float)>& validate) {
                    if (!control->IsShown()) return;
                    const auto value = parseOptionalFiniteFloat(control, fieldName);
                    if (!value) return;
                    validate(*value);
                    setOptionalNodeField(document, ref, label, FIELD_TYPE_FLOAT,
                                         wxui::toWx(neogff::FormatNumber(*value)), false);
                };

                setOptionalFloat("CamHeightOffset", nodeCamHeightOffset_, "Camera height offset",
                                 [](float) {});
                setOptionalFloat("TarHeightOffset", nodeTarHeightOffset_, "Target height offset",
                                 [](float) {});

                if (loadedCameraFovPresent_ || nodeCameraFovMode_->GetSelection() != 0) {
                    const auto fov = cameraFovValue();
                    if (!loadedCameraFovPresent_ || fov != loadedCameraFovRaw_ || nodeCameraFov_->IsModified())
                        setOptionalNodeField(document, ref, "CamFieldOfView", FIELD_TYPE_FLOAT,
                                             wxui::toWx(fov), true);
                }

                const std::int32_t videoEffect = cameraVideoEffectValue(flavor);
                if (loadedCamVidEffectPresent_ || videoEffect != -1) {
                    setOptionalNodeField(document, ref, "CamVidEffect", FIELD_TYPE_INT,
                                         wxString::Format("%d", videoEffect), true);
                }

                const std::string fadeType = selectedIntegerChoice(
                    nodeFadeType_, nodeFadeTypeValues_, "fade type");
                if (document.hasNodeField(ref, "FadeType") || fadeType != "0") {
                    setOptionalNodeField(document, ref, "FadeType", FIELD_TYPE_BYTE,
                                         wxui::toWx(fadeType), true);
                }

                if ((loadedFadeColorPresent_ || fadeColorEdited_) &&
                    nodeFadeColorPicker_->IsShown()) {
                    setOptionalNodeField(document, ref, "FadeColor", FIELD_TYPE_POSITION,
                                         wxui::toWx(fadeColorValue()), true);
                }

                setOptionalFloat("FadeDelay", nodeFadeDelay_, "Fade delay", [](float value) {
                    if (value < 0.0f) throw std::invalid_argument("Fade delay cannot be negative.");
                });
                setOptionalFloat("FadeLength", nodeFadeLength_, "Fade length", [](float value) {
                    if (value <= 0.0f) throw std::invalid_argument("Fade length must be greater than 0 seconds.");
                });
            }

            setOptionalNodeField(document, ref, "CameraAnimation", FIELD_TYPE_WORD, nodeCameraAnimation_->GetValue(), false);
            setOptionalNodeField(document, ref, "Emotion", FIELD_TYPE_INT, nodeEmotion_->GetValue(), false);
            setOptionalNodeField(document, ref, "FacialAnim", FIELD_TYPE_INT, nodeFacialAnim_->GetValue(), false);
            setOptionalNodeField(document, ref, "AlienRaceNode", FIELD_TYPE_INT, nodeAlienRace_->GetValue(), false);
            setOptionalNodeField(document, ref, "NodeUnskippable", FIELD_TYPE_INT,
                                 wxui::toWx(boolText(nodeUnskippable_)), document.hasNodeField(ref, "NodeUnskippable"));
        });
    }

    void onApplyLink(wxCommandEvent&) {
        refreshContextualInspector();
        if (!activeDocument().selectedLink) return;
        const DlgLinkRef ref = *activeDocument().selectedLink;
        mutate("Edit dialogue link conditions", [this, ref]() {
            DlgDocument document = dialogue();
            const bool jade = document.dialect() == DlgDialect::JadeEmpire;
            setOptionalLinkField(document, ref, "Active", FIELD_TYPE_RESREF,
                                 linkActive1_->GetValue(), false);

            if (jade) {
                setOptionalLinkField(document, ref, "DesignerNumber", FIELD_TYPE_INT,
                                     linkDesignerNumber_->GetValue(), false);
                setOptionalLinkField(document, ref, "ReverseCond", FIELD_TYPE_BYTE,
                                     wxui::toWx(boolText(linkReverseCond_)),
                                     document.hasLinkField(ref, "ReverseCond") || linkReverseCond_->GetValue());
                return;
            }

            // Do not normalize an unusual existing BYTE just by pressing Apply.
            if (linkDisplayInactive_->IsShown() && linkDisplayInactiveEdited_ &&
                inspectorReplyChoiceLink(document, activeDocument().selectedNode, ref)) {
                setOptionalLinkField(document, ref, "DisplayInactive", FIELD_TYPE_BYTE,
                                     wxui::toWx(boolText(linkDisplayInactive_)), true);
            }
            setOptionalLinkField(document, ref, "Active2", FIELD_TYPE_RESREF,
                                 linkActive2_->GetValue(), false);
            setOptionalLinkField(document, ref, "Logic", FIELD_TYPE_INT,
                                 linkLogic_->GetValue(), false);
            setOptionalLinkField(document, ref, "ParamStrA", FIELD_TYPE_CEXOSTRING,
                                 linkParamStrA_->GetValue(), false);
            setOptionalLinkField(document, ref, "ParamStrB", FIELD_TYPE_CEXOSTRING,
                                 linkParamStrB_->GetValue(), false);
            setOptionalLinkField(document, ref, "Not", FIELD_TYPE_BYTE,
                                 wxui::toWx(boolText(linkNot1_)), document.hasLinkField(ref, "Not"));
            setOptionalLinkField(document, ref, "Not2", FIELD_TYPE_BYTE,
                                 wxui::toWx(boolText(linkNot2_)), document.hasLinkField(ref, "Not2"));
            for (int i = 0; i < 5; ++i) {
                setOptionalLinkField(document, ref, "Param" + std::to_string(i + 1), FIELD_TYPE_INT,
                                     linkParamFields_->GetCellValue(i, 0), false);
                setOptionalLinkField(document, ref, "Param" + std::to_string(i + 1) + "b", FIELD_TYPE_INT,
                                     linkParamFields_->GetCellValue(i, 1), false);
            }
        });
    }

    void setOptionalNodeField(DlgDocument& document,
                              DlgNodeRef ref,
                              const std::string& label,
                              std::uint32_t type,
                              const wxString& value,
                              bool createEvenIfEmpty) {
        if (!inspectorAllowsNodeWrite(label)) return;
        const std::string text = wxui::toStd(value);
        if (!document.hasNodeField(ref, label) && text.empty() && !createEvenIfEmpty) return;
        document.setNodeField(ref, label, type,
                              text.empty() && type != FIELD_TYPE_CEXOSTRING && type != FIELD_TYPE_RESREF ? "0" : text);
    }

    void setOptionalLinkField(DlgDocument& document,
                              DlgLinkRef ref,
                              const std::string& label,
                              std::uint32_t type,
                              const wxString& value,
                              bool createEvenIfEmpty) {
        if (!inspectorAllowsLinkWrite(label)) return;
        const std::string text = wxui::toStd(value);
        if (!document.hasLinkField(ref, label) && text.empty() && !createEvenIfEmpty) return;
        document.setLinkField(ref, label, type,
                              text.empty() && type != FIELD_TYPE_CEXOSTRING && type != FIELD_TYPE_RESREF ? "0" : text);
    }

    long selectedAnimationRow() const {
        return animationList_ ? animationList_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED) : -1;
    }

    void onAnimationAdd(wxCommandEvent&) {
        if (!activeDocument().selectedNode) return;
        const DlgNodeRef ref = *activeDocument().selectedNode;
        const DlgDocument document = dialogue();
        const auto current = document.animations(ref);
        if (document.dialect() == DlgDialect::JadeEmpire &&
            ref.kind == DlgNodeKind::Reply && !current.empty()) {
            wxui::showMessage(this, "Dialogue Animation",
                              "A Jade Empire Reply stores one Animation/Emotion pair. Edit or delete the existing value.");
            return;
        }
        DlgAnimation initial;
        if (document.dialect() == DlgDialect::JadeEmpire && ref.kind == DlgNodeKind::Entry) {
            initial.participantIndex = -1;
        }
        AnimationEditDialog dialog(this, document.dialect() == DlgDialect::JadeEmpire, ref.kind,
                                   document.speakerTags(), initial);
        wxui::applyTheme(&dialog, darkMode_);
        if (dialog.ShowModal() != wxID_OK) return;
        mutate("Add dialogue animation", [this, ref, &dialog]() {
            auto values = dialogue().animations(ref);
            values.push_back(dialog.value());
            dialogue().replaceAnimations(ref, values);
        });
    }

    void onAnimationEdit(wxCommandEvent&) {
        if (!activeDocument().selectedNode) return;
        const long row = selectedAnimationRow();
        if (row < 0 || static_cast<std::size_t>(row) >= animationValues_.size()) return;
        const DlgNodeRef ref = *activeDocument().selectedNode;
        const DlgDocument document = dialogue();
        AnimationEditDialog dialog(this, document.dialect() == DlgDialect::JadeEmpire, ref.kind,
                                   document.speakerTags(),
                                   animationValues_[static_cast<std::size_t>(row)]);
        wxui::applyTheme(&dialog, darkMode_);
        if (dialog.ShowModal() != wxID_OK) return;
        mutate("Edit dialogue animation", [this, ref, row, &dialog]() {
            auto values = dialogue().animations(ref);
            values[static_cast<std::size_t>(row)] = dialog.value();
            dialogue().replaceAnimations(ref, values);
        });
    }

    void onAnimationDelete(wxCommandEvent&) {
        if (!activeDocument().selectedNode) return;
        const long row = selectedAnimationRow();
        if (row < 0 || static_cast<std::size_t>(row) >= animationValues_.size()) return;
        const DlgNodeRef ref = *activeDocument().selectedNode;
        mutate("Delete dialogue animation", [this, ref, row]() {
            auto values = dialogue().animations(ref);
            values.erase(values.begin() + row);
            dialogue().replaceAnimations(ref, values);
        });
    }

    void moveAnimation(int delta) {
        if (!activeDocument().selectedNode) return;
        const DlgNodeRef ref = *activeDocument().selectedNode;
        if (dialogue().dialect() == DlgDialect::JadeEmpire &&
            ref.kind == DlgNodeKind::Reply) {
            return;
        }
        const long row = selectedAnimationRow();
        const long target = row + delta;
        if (row < 0 || target < 0 ||
            static_cast<std::size_t>(target) >= animationValues_.size()) {
            return;
        }
        mutate("Move dialogue animation", [this, ref, row, target]() {
            auto values = dialogue().animations(ref);
            std::swap(values[static_cast<std::size_t>(row)],
                      values[static_cast<std::size_t>(target)]);
            dialogue().replaceAnimations(ref, values);
        });
        animationList_->SetItemState(target,
                                     wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                     wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    }

    void importFromPath(bool json, const std::filesystem::path& chosen) {
        mutate(json ? "Import JSON" : "Import XML", [this, chosen, json]() {
            const std::string source = readTextFile(chosen);
            importDlgModelXml(model(), json ? gffJsonToXml(source) : source);
            activeDocument().selectedNode.reset();
            activeDocument().selectedLink.reset();
        });
    }

    void onImport(bool json) {
#if defined(__EMSCRIPTEN__)
        if (!hasActiveDocument()) return;
        wxWindow* const targetPage = activeDocument().tabPage;
        requestBrowserImport(
            json ? "Import JSON" : "Import XML",
            json ? ".json" : ".xml",
            false,
            [this, targetPage, json](neobrowser::BrowserImportLease import) {
                if (import.empty() || IsBeingDeleted()) return;
                if (!hasActiveDocument() || activeDocument().tabPage != targetPage) {
                    wxui::showMessage(
                        this,
                        "Import Cancelled",
                        "The active document changed while the import picker was open. Start the import again from the intended tab.");
                    return;
                }
                importFromPath(json, import.paths().front());
            });
#else
        const auto chosen = wxui::chooseOpenFile(
            this,
            json ? "Import JSON" : "Import XML",
            json ? kJsonWildcard : kXmlWildcard);
        if (!chosen) return;
        importFromPath(json, *chosen);
#endif
    }

    void onExport(bool json) {
        if (!model().loaded() || model().gff().isGff4()) {
            wxui::showMessage(this, "Export", "Semantic XML/JSON export is available for classic GFF V3 DLG files.");
            return;
        }
        const std::filesystem::path sourcePath = documentFilename(activeDocument());
        const std::string stem = sourcePath.empty() ? "dialog" : neosettings::pathToUtf8(sourcePath.stem());
        const auto chosen = wxui::chooseSaveFile(this, json ? "Export JSON" : "Export XML",
                                                  json ? kJsonWildcard : kXmlWildcard,
                                                  stem + (json ? ".json" : ".xml"));
        if (!chosen) return;
        try {
            removeRetiredDlgFields(model());
            const std::string xml = model().toXml();
            checkOutput(*chosen, true);
            writeTextFile(*chosen, json ? gffXmlToJson(xml) : xml);
        } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void continueExportPatcherPackage(
        neodlg::patcher::DlgPatchMode patchMode,
        std::optional<std::filesystem::path> originalPath) {
        try {
            removeRetiredDlgFields(model());
            const std::filesystem::path sourcePath = documentFilename(activeDocument());
            std::string defaultName = sourcePath.empty()
                ? "modified.dlg"
                : neosettings::pathToUtf8(sourcePath.filename());
            const auto patchName = wxui::promptText(
                this,
                "Patch Target Filename",
                "DLG filename to patch in the user's install:",
                defaultName);
            if (!patchName || patchName->empty()) return;

            wxArrayString destinationChoices;
            destinationChoices.Add("Override folder");
            destinationChoices.Add("Module or archive inside the game directory...");
            wxSingleChoiceDialog destinationDialog(
                this,
                "Choose where TSLPatcher/HoloPatcher should install the patched DLG.\n\n"
                "Use Override for a loose DLG. Use a module/archive destination when the DLG must be patched inside a .mod, .rim, or .erf file.",
                "Patch Destination",
                destinationChoices);
            destinationDialog.SetSelection(0);
            if (destinationDialog.ShowModal() != wxID_OK) return;

            std::string destination = "override";
            if (destinationDialog.GetSelection() == 1) {
                const auto archivePath = wxui::promptText(
                    this,
                    "Module or Archive Destination",
                    "Path relative to the game directory, for example Modules\\101PER.mod:",
                    "Modules\\");
                if (!archivePath) return;
                destination = trimAscii(*archivePath);
                if (destination.empty()) {
                    throw std::runtime_error("The module/archive destination cannot be empty.");
                }
            }

            const auto output = wxui::choosePatcherOutput(this);
            if (!output) return;
            const bool writeToIni = output->writesToIni();

            neotsl::PatchProject project;
            if (patchMode == neodlg::patcher::DlgPatchMode::DynamicMerge) {
                if (!originalPath) {
                    throw std::runtime_error("A clean original DLG is required for dynamic merge mode.");
                }
                GffModel original;
                original.load(*originalPath);
                project = neodlg::patcher::diffDlgPatcher(
                    original.gff(),
                    model().gff(),
                    *patchName,
                    patchMode,
                    writeToIni,
                    writeToIni ? *originalPath : std::filesystem::path{},
                    destination);
            } else {
                project = neodlg::patcher::makeCompleteDlgReplacement(
                    model().gff(),
                    *patchName,
                    destination);
            }

            if (patchMode == neodlg::patcher::DlgPatchMode::DynamicMerge &&
                !project.unsupported.empty()) {
                try {
                    neotsl::throwIfUnsupported(project);
                } catch (const std::exception& ex) {
                    throw std::runtime_error(
                        std::string(ex.what()) +
                        "\n\nDynamic merge mode cannot represent this edit. "
                        "Choose Complete modified DLG replacement if a whole-file install is acceptable.");
                }
            }

            neotsl::throwIfUnsupported(project);

            if (!writeToIni) {
                std::vector<std::string> companionFiles;
                if (patchMode == neodlg::patcher::DlgPatchMode::DynamicMerge) {
                    companionFiles.push_back(*patchName);
                }
                wxui::showIniFragmentDialog(
                    this,
                    "DLG Patcher INI Fragment",
                    project,
                    companionFiles);
                return;
            }

            const auto report = neotsl::writePackageToIni(project, output->iniPath, true);

            const std::string detail = patchMode == neodlg::patcher::DlgPatchMode::DynamicMerge
                ? "The package uses TSLPatcher's dynamic ListIndex and 2DAMEMORY workflow. New Entry and Reply nodes receive their actual indexes during installation, and generated link fields are updated to those indexes automatically."
                : "The package installs the complete modified DLG and does not use dynamic ListIndex or 2DAMEMORY wiring. This mode is less merge-friendly when another mod replaces the same DLG.";
            wxui::showMessage(
                this,
                "TSL/HoloPatcher Package",
                std::string(report.mergedExisting ? "Merged the generated DLG instructions into:\n"
                                                  : "Created the installer INI:\n") +
                    neosettings::pathToUtf8(report.iniPath) +
                    "\n\nRequired package files were staged beside the selected INI.\n\n" + detail);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void onExportPatcherPackage(wxCommandEvent&) {
        if (!model().loaded() || model().gff().isGff4()) return;
        try {
            neodlggui::PatcherExportModeDialog modeDialog(this, darkMode_);
            if (modeDialog.ShowModal() != wxID_OK) return;

            const auto patchMode = modeDialog.selectedMode();
            if (patchMode != neodlg::patcher::DlgPatchMode::DynamicMerge) {
                continueExportPatcherPackage(patchMode, std::nullopt);
                return;
            }

#if defined(__EMSCRIPTEN__)
            wxWindow* const targetPage = activeDocument().tabPage;
            requestBrowserImport(
                "Select clean/unmodified DLG", ".dlg", false,
                [this, targetPage, patchMode](neobrowser::BrowserImportLease import) {
                    if (import.empty() || IsBeingDeleted()) return;
                    if (!hasActiveDocument() || activeDocument().tabPage != targetPage) {
                        wxui::showMessage(
                            this,
                            "Patcher Export Cancelled",
                            "The active document changed while the baseline picker was open. Start the export again from the intended tab.");
                        return;
                    }
                    continueExportPatcherPackage(patchMode, import.paths().front());
                });
#else
            const auto originalPath = wxui::chooseOpenFile(
                this,
                "Select clean/unmodified DLG",
                kDlgWildcard);
            if (!originalPath) return;
            continueExportPatcherPackage(patchMode, originalPath);
#endif
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void setWorkspaceView(WorkspaceView view, bool refresh = true) {
        if (!workspaceBook_ || workspaceBook_->GetPageCount() < 3) return;

        WorkspaceView semanticView = view;
        if (hasActiveDocument()) {
            if (view == WorkspaceView::Raw) {
                semanticView = activeDocument().semanticView;
            } else {
                activeDocument().semanticView = view;
            }
            activeDocument().workspaceView = view;
        } else if (view == WorkspaceView::Raw) {
            semanticView = singlePanelActive_
                ? WorkspaceView::SinglePanel
                : WorkspaceView::Conversation;
        }

        const bool singlePanel = semanticView == WorkspaceView::SinglePanel;
        setSemanticWorkspaceHost(singlePanel);
        setSinglePanelLayout(singlePanel);

        int page = 0;
        if (view == WorkspaceView::SinglePanel) page = 1;
        else if (view == WorkspaceView::Raw) page = 2;
        if (workspaceBook_->GetSelection() != page) workspaceBook_->ChangeSelection(page);

        if (conversationViewItem_) conversationViewItem_->Check(view == WorkspaceView::Conversation);
        if (singlePanelViewItem_) singlePanelViewItem_->Check(view == WorkspaceView::SinglePanel);
        if (rawViewItem_) rawViewItem_->Check(view == WorkspaceView::Raw);

        if (refresh) {
            if (view == WorkspaceView::Raw) refreshRawTree();
            else refreshConversationTree();
        }
    }

    void onWorkspacePageChanged(wxBookCtrlEvent& event) {
        // Notebook selection events propagate from nested notebooks. An
        // inspector subtab (e.g. Scripts / Quest == 1) is NOT a workspace page
        // (Single Panel == 1). Ignore it before touching view or document state.
        if (event.GetEventObject() != workspaceBook_) {
            event.Skip();
            return;
        }
        switch (event.GetSelection()) {
        case 0:
            setWorkspaceView(WorkspaceView::Conversation);
            break;
        case 1:
            setWorkspaceView(WorkspaceView::SinglePanel);
            break;
        case 2:
            setWorkspaceView(WorkspaceView::Raw);
            break;
        default:
            break;
        }
        event.Skip();
    }

    void refreshAll() {
        if (!hasActiveDocument()) return;
        // Hosts can edit the underlying shared GffModel directly.
        removeRetiredDlgFields(model());
        refreshHeader();
        updateTabTitle();
        refreshUndoMenu();
        if (activeDocument().workspaceView == WorkspaceView::Raw) refreshRawTree();
        else refreshConversationTree();
        refreshInspector();
    }

    void refreshHeader() {
        if (!hasActiveDocument() || !model().loaded()) {
            setModuleStatusText("Ready", 0);
            setModuleStatusText("No DLG loaded", 1);
            return;
        }

        wxString primary = activeDocument().saveInProgress
            ? "Saving..."
            : (model().dirty() ? "Modified" : "Saved");
        primary += " - ";
        primary += wxui::toWx(tabDisplayName(activeDocument()));
        setModuleStatusText(primary, 0);

        const wxString warning = wxui::toWx(activeDocument().tlkAutoLoadWarning);
        setModuleStatusText(warning.empty() ? wxString{} : "TLK warning: " + warning, 1);
    }

    void refreshUndoMenu() {
        if (!undoItem_ || !redoItem_ || !hasActiveDocument()) return;
        if (activeDocument().undo.empty()) {
            undoItem_->SetItemLabel("Undo\tCtrl-Z");
            undoItem_->Enable(false);
        } else {
            undoItem_->SetItemLabel("Undo " + wxui::toWx(activeDocument().undo.back().description) + "\tCtrl-Z");
            undoItem_->Enable(true);
        }
        if (activeDocument().redo.empty()) {
            redoItem_->SetItemLabel("Redo\tCtrl-Y");
            redoItem_->Enable(false);
        } else {
            redoItem_->SetItemLabel("Redo " + wxui::toWx(activeDocument().redo.back().description) + "\tCtrl-Y");
            redoItem_->Enable(true);
        }
    }

    wxTreeItemId conversationTreeItemForKey(const std::string& key) const {
        const auto found = conversationTreeItemsByKey_.find(key);
        return found == conversationTreeItemsByKey_.end() ? wxTreeItemId{} : found->second;
    }

    void refreshConversationTree() {
        if (!conversationTree_ || !hasActiveDocument()) return;
        if (conversationTreeRenderedDocumentPage_ == activeDocument().tabPage) {
            captureRenderedConversationTreeState();
        }

        treeRefreshInProgress_ = true;
        conversationTree_->Freeze();
        conversationTree_->DeleteAllItems();
        canonicalTreeItems_.clear();
        conversationTreeItemsByKey_.clear();

        constexpr const char* kRootKey = "conversation";
        constexpr const char* kStartsKey = "conversation/starts";
        constexpr const char* kUnreachableKey = "conversation/unreachable";
        const wxTreeItemId rootItem = conversationTree_->AddRoot(
            "Conversation", -1, -1,
            new ConversationTreeData(ConversationTreeKind::ConversationRoot,
                                     std::nullopt, std::nullopt, false, kRootKey));
        conversationTreeItemsByKey_[kRootKey] = rootItem;

        wxTreeItemId starts;
        if (!model().loaded()) {
            conversationTree_->AppendItem(rootItem, "Open or create a DLG to begin.");
        } else {
            DlgDocument document = dialogue();
            if (!document.semanticallyEditable()) {
                conversationTree_->AppendItem(
                    rootItem,
                    "This DLG schema is not supported by the conversation editor. Use GFF Tree view.");
            } else {
                starts = conversationTree_->AppendItem(
                    rootItem, "Starting Nodes", -1, -1,
                    new ConversationTreeData(ConversationTreeKind::Group,
                                             std::nullopt, std::nullopt, false, kStartsKey));
                conversationTreeItemsByKey_[kStartsKey] = starts;
                std::vector<DlgNodeRef> ancestry;
                for (DlgLinkRef ref : document.startingLinks()) {
                    appendLinkBranch(starts, ref, ancestry, 0);
                }

                const auto unreachable = document.unreachableNodes();
                if (!unreachable.empty()) {
                    const wxTreeItemId orphanGroup = conversationTree_->AppendItem(
                        rootItem, wxString::Format("Unreachable Nodes (%zu)", unreachable.size()),
                        -1, -1, new ConversationTreeData(ConversationTreeKind::Group,
                                                        std::nullopt, std::nullopt, false,
                                                        kUnreachableKey));
                    conversationTreeItemsByKey_[kUnreachableKey] = orphanGroup;
                    for (DlgNodeRef ref : unreachable) {
                        if (canonicalTreeItems_.count(ref)) continue;
                        const std::string itemKey = std::string(kUnreachableKey) + "/" + conversationNodeKey(ref);
                        const wxTreeItemId item = conversationTree_->AppendItem(
                            orphanGroup, wxui::toWx("[unreachable] " + document.nodeLabel(ref)),
                            -1, -1, new ConversationTreeData(ConversationTreeKind::Node, ref,
                                                            std::nullopt, false, itemKey));
                        conversationTreeItemsByKey_[itemKey] = item;
                        styleTreeNode(item, ref, true);
                        canonicalTreeItems_[ref] = item;
                        ancestry.clear();
                        ancestry.push_back(ref);
                        for (DlgLinkRef child : document.outgoingLinks(ref)) {
                            appendLinkBranch(item, child, ancestry, 1);
                        }
                    }
                }
            }
        }

        const neotree::TreeViewState& state = activeDocument().conversationTreeState;
        neotree::TreeRestoreResult restored;
        if (state.initialized) {
            restored = neotree::restoreTreeViewState(
                *conversationTree_, state,
                [this](const std::string& key) { return conversationTreeItemForKey(key); });
        } else {
            conversationTree_->Expand(rootItem);
            if (starts.IsOk()) conversationTree_->Expand(starts);
        }

        conversationTreeRenderedDocumentPage_ = activeDocument().tabPage;
        conversationTree_->Thaw();
        treeRefreshInProgress_ = false;

        if (!restored.selectionRestored && activeDocument().selectedNode) {
            selectSemanticNode(*activeDocument().selectedNode, activeDocument().selectedLink, false);
        }
    }

    void appendLinkBranch(const wxTreeItemId& parent,
                          DlgLinkRef linkRef,
                          std::vector<DlgNodeRef>& ancestry,
                          int depth) {
        DlgDocument document = dialogue();
        const auto target = document.targetOf(linkRef);
        if (!target) {
            const std::string itemKey = conversationLinkKey(linkRef);
            const wxTreeItemId invalid = conversationTree_->AppendItem(
                parent, "[invalid link]", -1, -1,
                new ConversationTreeData(ConversationTreeKind::InvalidLink, std::nullopt,
                                         linkRef, false, itemKey));
            conversationTreeItemsByKey_[itemKey] = invalid;
            conversationTree_->SetItemTextColour(invalid, darkMode_ ? wxColour(255, 130, 130) : wxColour(170, 0, 0));
            return;
        }

        const bool cycle = std::find(ancestry.begin(), ancestry.end(), *target) != ancestry.end();
        const bool reference = canonicalTreeItems_.count(*target) != 0;
        std::string label;
        if (cycle) label = "[cycle] ";
        else if (reference) label = "[link] ";
        label += document.nodeLabel(*target);
        const std::string condition = linkConditionSummary(document, linkRef);
        if (!condition.empty()) label += "  (" + condition + ")";

        const std::string itemKey = conversationLinkKey(linkRef);
        const wxTreeItemId item = conversationTree_->AppendItem(
            parent, wxui::toWx(label), -1, -1,
            new ConversationTreeData(ConversationTreeKind::Node, *target, linkRef,
                                     cycle || reference, itemKey));
        conversationTreeItemsByKey_[itemKey] = item;
        styleTreeNode(item, *target, false);
        if (cycle || reference || depth >= 512) return;
        canonicalTreeItems_[*target] = item;
        ancestry.push_back(*target);
        for (DlgLinkRef child : document.outgoingLinks(*target)) appendLinkBranch(item, child, ancestry, depth + 1);
        ancestry.pop_back();
    }

    void styleTreeNode(const wxTreeItemId& item, DlgNodeRef node, bool unreachable) {
        if (!item.IsOk()) return;
        if (unreachable) {
            conversationTree_->SetItemTextColour(
                item, darkMode_ ? wxColour(225, 150, 240) : wxColour(120, 40, 135));
            return;
        }
        if (node.kind == DlgNodeKind::Entry) {
            conversationTree_->SetItemTextColour(
                item, darkMode_ ? wxColour(255, 150, 150) : wxColour(175, 30, 30));
        } else {
            conversationTree_->SetItemTextColour(
                item, darkMode_ ? wxColour(145, 185, 255) : wxColour(35, 70, 175));
        }
    }

    void onTreeContextMenu(wxTreeEvent& event) {
        const wxTreeItemId item = event.GetItem();
        if (item.IsOk()) {
            conversationTree_->SelectItem(item);
            auto* data = dynamic_cast<ConversationTreeData*>(conversationTree_->GetItemData(item));
            if (data) {
                activeDocument().selectedNode = data->node;
                activeDocument().selectedLink = data->link;
                refreshInspector();
            }
        }

        wxMenu menu;
        if (!activeDocument().selectedNode) {
            menu.Append(ID_AddStartingEntry, "Add Starting Entry");
            menu.Append(ID_LinkExisting, "Link Existing Starting Entry...");
        } else {
            menu.Append(ID_AddChild, "Add Child Node");
            menu.Append(ID_LinkExisting, "Link Existing Child...");
            menu.Append(ID_DuplicateNode, "Duplicate Node");
            menu.AppendSeparator();
            wxMenuItem* removeLink = menu.Append(ID_RemoveLink, "Remove This Link");
            removeLink->Enable(activeDocument().selectedLink.has_value());
            menu.Append(ID_DeleteNode, "Delete Node Everywhere");
            menu.AppendSeparator();
            wxMenuItem* moveUp = menu.Append(ID_MoveLinkUp, "Move Choice Up");
            wxMenuItem* moveDown = menu.Append(ID_MoveLinkDown, "Move Choice Down");
            moveUp->Enable(activeDocument().selectedLink.has_value());
            moveDown->Enable(activeDocument().selectedLink.has_value());
        }
        PopupMenu(&menu);
    }

    void selectSemanticNode(DlgNodeRef node,
                            std::optional<DlgLinkRef> link,
                            bool updateInspector = true) {
        activeDocument().selectedNode = node;
        activeDocument().selectedLink = link;
        auto found = canonicalTreeItems_.find(node);
        if (found != canonicalTreeItems_.end() && found->second.IsOk()) {
            conversationTree_->SelectItem(found->second);
            conversationTree_->EnsureVisible(found->second);
        }
        if (updateInspector) refreshInspector();
    }

    void onTreeSelection(wxTreeEvent& event) {
        if (treeRefreshInProgress_) { event.Skip(); return; }
        auto* data = dynamic_cast<ConversationTreeData*>(conversationTree_->GetItemData(event.GetItem()));
        if (!data) { event.Skip(); return; }
        activeDocument().selectedNode = data->node;
        activeDocument().selectedLink = data->link;
        refreshInspector();
        event.Skip();
    }

    void onTreeActivated(wxTreeEvent& event) {
        auto* data = dynamic_cast<ConversationTreeData*>(conversationTree_->GetItemData(event.GetItem()));
        if (data && data->reference && data->node) selectSemanticNode(*data->node, data->link);
        event.Skip();
    }

    void refreshInspector() {
        const bool valid = hasActiveDocument() && model().loaded() &&
                           dialogue().semanticallyEditable() && activeDocument().selectedNode;
        enableInspector(valid);
        if (!valid) {
            setNodeHeader("Select a dialogue node.");
            linkHeader_->SetLabel("Select a linked node to edit its conditions.");
            clearInspectorControls();
            enableInspector(false);
            refreshContextualInspector();
            return;
        }

        const DlgDocument document = dialogue();
        const DlgNodeRef ref = *activeDocument().selectedNode;
        const bool jade = document.dialect() == DlgDialect::JadeEmpire;
        const bool jadeEntry = jade && ref.kind == DlgNodeKind::Entry;
        setNodeHeader(wxui::toWx(document.nodeLabel(ref, 200)));

        const auto parseStoredIndex = [&](const std::string& value, std::int32_t fallback) {
            if (value.empty()) return fallback;
            try {
                return neogff::ParseInt32Decimal(value);
            } catch (const std::exception&) {
                return fallback;
            }
        };

        nodeSpeaker_->Clear();
        nodeListener_->Clear();
        if (jadeEntry) {
            const auto tags = document.speakerTags();
            populateJadeParticipantCombo(
                nodeSpeaker_, tags,
                parseStoredIndex(document.nodeField(ref, "SpeakerIndex"), -1), false);
            populateJadeParticipantCombo(
                nodeListener_, tags,
                parseStoredIndex(document.nodeField(ref, "ListenerIndex"), -2), false);
        } else if (!jade) {
            nodeSpeaker_->ChangeValue(wxui::toWx(document.nodeField(ref, "Speaker")));
            nodeListener_->ChangeValue(wxui::toWx(document.nodeField(ref, "Listener")));
        }

        const DlgTextValue value = document.text(ref);
        nodeStrRef_->ChangeValue(value.strref == 0xFFFFFFFFu
                                     ? wxString("-1")
                                     : wxString::Format("%u", value.strref));
        nodeStringType_->ChangeValue(wxString::Format("%u", value.stringType));
        nodeLocalText_->ChangeValue(wxui::toWx(value.localText));
        nodeResolvedText_->ChangeValue(wxui::toWx(value.resolvedText));
        nodeVo_->ChangeValue(jadeEntry
                                 ? wxui::toWx(document.nodeField(ref, "VoiceOver"))
                                 : (!jade ? wxui::toWx(document.nodeField(ref, "VO_ResRef")) : wxString{}));
        nodeComment_->ChangeValue(!jade ? wxui::toWx(document.nodeField(ref, "Comment")) : wxString{});

        loadField(nodeScript1_, document.nodeField(ref, "Script"));
        if (jadeEntry) {
            loadField(nodeScript2_, document.nodeField(ref, "ScriptEntry"));
            const std::string entryCameraScript = document.nodeField(ref, "ScriptCamEntry");
            const std::string repliesCameraScript = document.nodeField(ref, "ScriptCamReplies");
            loadField(nodeScriptCamEntry_, entryCameraScript.empty() ? "camscrentdef" : entryCameraScript);
            loadField(nodeCameraEntry_, document.nodeField(ref, "CameraEntry"));
            loadField(nodeScriptCamReplies_, repliesCameraScript.empty() ? "camscrrepdef" : repliesCameraScript);
            loadField(nodeCameraReplies_, document.nodeField(ref, "CameraReplies"));
        } else if (!jade) {
            loadField(nodeScript2_, document.nodeField(ref, "Script2"));
        } else {
            loadField(nodeScript2_, "");
            loadField(nodeScriptCamEntry_, "");
            loadField(nodeCameraEntry_, "");
            loadField(nodeScriptCamReplies_, "");
            loadField(nodeCameraReplies_, "");
        }

        if (!jade) {
            loadField(nodeQuest_, document.nodeField(ref, "Quest"));
            loadField(nodeQuestEntry_, document.nodeField(ref, "QuestEntry"));
            loadField(nodePlotIndex_, document.nodeField(ref, "PlotIndex"));
            loadField(nodePlotXp_, document.nodeField(ref, "PlotXPPercentage"));
            loadField(nodeActionStrA_, document.nodeField(ref, "ActionParamStrA"));
            loadField(nodeActionStrB_, document.nodeField(ref, "ActionParamStrB"));
            for (int i = 0; i < 5; ++i) {
                actionParamFields_->SetCellValue(
                    i, 0, wxui::toWx(document.nodeField(ref, "ActionParam" + std::to_string(i + 1))));
                actionParamFields_->SetCellValue(
                    i, 1, wxui::toWx(document.nodeField(ref, "ActionParam" + std::to_string(i + 1) + "b")));
            }

            loadField(nodeSound_, document.nodeField(ref, "Sound"));
            loadField(nodeDelay_, document.nodeField(ref, "Delay"));
            loadField(nodeWaitFlags_, document.nodeField(ref, "WaitFlags"));
            populateIntegerChoice(nodeCameraAngle_, nodeCameraAngleValues_, kCameraAngleOptions,
                                  document.nodeField(ref, "CameraAngle"), 0,
                                  "Unknown camera mode (preserve until changed)");
            loadField(nodeCameraId_, document.nodeField(ref, "CameraID"));
            loadField(nodeCamHeightOffset_, document.nodeField(ref, "CamHeightOffset"));
            loadField(nodeTarHeightOffset_, document.nodeField(ref, "TarHeightOffset"));
            populateCameraFov(document.nodeField(ref, "CamFieldOfView"),
                              document.hasNodeField(ref, "CamFieldOfView"));
            loadField(nodeCameraAnimation_, document.nodeField(ref, "CameraAnimation"));
            loadField(nodeEmotion_, document.nodeField(ref, "Emotion"));
            loadField(nodeFacialAnim_, document.nodeField(ref, "FacialAnim"));
            populateVideoEffect(document.flavor(), document.nodeField(ref, "CamVidEffect"),
                                document.hasNodeField(ref, "CamVidEffect"));
            populateIntegerChoice(nodeFadeType_, nodeFadeTypeValues_, kFadeTypeOptions,
                                  document.nodeField(ref, "FadeType"), 0,
                                  "Unknown nonzero value (runtime treats as Fade in; preserve until changed)");
            populateFadeColor(document.nodeField(ref, "FadeColor"),
                              document.hasNodeField(ref, "FadeColor"));
            loadField(nodeFadeDelay_, document.nodeField(ref, "FadeDelay"));
            loadField(nodeFadeLength_, document.nodeField(ref, "FadeLength"));
            loadField(nodeAlienRace_, document.nodeField(ref, "AlienRaceNode"));
            setBoolControl(nodeUnskippable_, document.nodeField(ref, "NodeUnskippable"));
        } else {
            nodeJadeSkippable_->SetValue(
                !document.hasNodeField(ref, "Skippable") || document.nodeField(ref, "Skippable") != "0");
        }

        const bool hasLink = activeDocument().selectedLink && document.link(*activeDocument().selectedLink);
        enableLinkInspector(hasLink);
        if (hasLink) {
            const DlgLinkRef linkRef = *activeDocument().selectedLink;
            linkHeader_->SetLabel(wxui::toWx(
                "Link to " + document.nodeKindName(ref.kind) + " " + std::to_string(ref.index)));
            loadField(linkActive1_, document.linkField(linkRef, "Active"));
            if (jade) {
                loadField(linkDesignerNumber_, document.linkField(linkRef, "DesignerNumber"));
                setBoolControl(linkReverseCond_, document.linkField(linkRef, "ReverseCond"));
            } else {
                loadField(linkActive2_, document.linkField(linkRef, "Active2"));
                loadField(linkLogic_, document.linkField(linkRef, "Logic"));
                loadField(linkParamStrA_, document.linkField(linkRef, "ParamStrA"));
                loadField(linkParamStrB_, document.linkField(linkRef, "ParamStrB"));
                setBoolControl(linkNot1_, document.linkField(linkRef, "Not"));
                setBoolControl(linkNot2_, document.linkField(linkRef, "Not2"));
                for (int i = 0; i < 5; ++i) {
                    linkParamFields_->SetCellValue(
                        i, 0, wxui::toWx(document.linkField(linkRef, "Param" + std::to_string(i + 1))));
                    linkParamFields_->SetCellValue(
                        i, 1, wxui::toWx(document.linkField(linkRef, "Param" + std::to_string(i + 1) + "b")));
                }
            }
        } else {
            linkHeader_->SetLabel("This node is not selected through a specific link.");
            clearLinkControls();
        }
        linkDisplayInactiveEdited_ = false;
        inspectorFadeTypeEdited_ = false;
        if (hasLink) setBoolControl(linkDisplayInactive_,
            document.linkField(*activeDocument().selectedLink, "DisplayInactive"));
        else linkDisplayInactive_->SetValue(false);
        // This policy is read-only and is refreshed once per selection/model
        // change, not every mouse wheel, keystroke, font or viewport event.
        inspectorFlavor_ = inspectorFieldFlavor(document);
        refreshAnimationList();
        refreshContextualInspector();
    }

    void loadField(wxTextCtrl* control, const std::string& value) {
        if (control) control->ChangeValue(wxui::toWx(value));
    }

    void enableInspector(bool enabled) {
        for (wxWindow* window : nodeInspectorWindows()) if (window) window->Enable(enabled);
        // Navigation/scrollbars stay enabled even with no selected node. Only
        // data entry and Apply actions depend on the selection.
        for (int id : {ID_ApplyNode, ID_ApplyScripts, ID_ApplyPresentation}) {
            if (auto* button = FindWindow(id)) button->Enable(enabled);
        }
        if (!enabled) enableLinkInspector(false);
    }

    void enableLinkInspector(bool enabled) {
        for (wxWindow* window : linkInspectorWindows()) if (window) window->Enable(enabled);
        if (auto* button = FindWindow(ID_ApplyLink)) button->Enable(enabled);
    }

    std::vector<wxWindow*> nodeInspectorWindows() const {
        return {nodeSpeaker_, nodeListener_, nodeStrRef_, nodeStringType_, nodeLocalText_, nodeResolvedText_, nodeVo_,
                nodeJadeSkippable_, nodeComment_, nodeScript1_, nodeScript2_, nodeScriptCamEntry_, nodeCameraEntry_,
                nodeScriptCamReplies_, nodeCameraReplies_, nodeQuest_, nodeQuestEntry_, nodePlotIndex_, nodePlotXp_,
                nodeActionStrA_, nodeActionStrB_, actionParamFields_, nodeSound_, nodeDelay_, nodeWaitFlags_,
                nodeCameraAngle_, nodeCameraId_, nodeCamHeightOffset_, nodeTarHeightOffset_, nodeCameraFovMode_, nodeCameraFov_,
                nodeCameraAnimation_, nodeEmotion_, nodeFacialAnim_, nodeCamVidEffectPanel_, nodeFadeType_, nodeFadeColorPicker_,
                nodeFadeColorR_, nodeFadeColorG_, nodeFadeColorB_, nodeFadeDelay_, nodeFadeLength_, nodeAlienRace_,
                nodeUnskippable_, animationList_, animationAddButton_,
                animationEditButton_, animationDeleteButton_};
    }

    std::vector<wxWindow*> linkInspectorWindows() const {
        return {linkActive1_, linkActive2_, linkLogic_, linkParamStrA_, linkParamStrB_, linkDesignerNumber_,
                linkNot1_, linkNot2_, linkReverseCond_, linkDisplayInactive_, linkParamFields_};
    }

    void clearInspectorControls() {
        if (nodeSpeaker_) {
            nodeSpeaker_->Clear();
            nodeSpeaker_->ChangeValue("");
        }
        if (nodeListener_) {
            nodeListener_->Clear();
            nodeListener_->ChangeValue("");
        }
        for (wxTextCtrl* control : {nodeStrRef_, nodeStringType_, nodeLocalText_, nodeResolvedText_, nodeVo_, nodeComment_,
                                    nodeScript1_, nodeScript2_, nodeScriptCamEntry_, nodeCameraEntry_,
                                    nodeScriptCamReplies_, nodeCameraReplies_, nodeQuest_, nodeQuestEntry_, nodePlotIndex_,
                                    nodePlotXp_, nodeActionStrA_, nodeActionStrB_, nodeSound_, nodeDelay_, nodeWaitFlags_, nodeCameraId_,
                                    nodeCamHeightOffset_, nodeTarHeightOffset_, nodeCameraFov_, nodeCameraAnimation_, nodeEmotion_, nodeFacialAnim_,
                                    nodeFadeColorR_, nodeFadeColorG_, nodeFadeColorB_, nodeFadeDelay_, nodeFadeLength_, nodeAlienRace_}) {
            if (control) control->ChangeValue("");
        }
        if (nodeCameraAngle_) {
            populateIntegerChoice(nodeCameraAngle_, nodeCameraAngleValues_, kCameraAngleOptions,
                                  "0", 0, "Unknown camera mode (preserve until changed)");
        }
        if (nodeCameraFovMode_) {
            nodeCameraFovMode_->Clear();
            nodeCameraFovMode_->Append("Automatic");
            nodeCameraFovMode_->Append("Custom");
            nodeCameraFovMode_->SetSelection(0);
        }
        if (nodeCamVidEffectPanel_) populateVideoEffect(DlgFlavor::Kotor, "", false);
        if (nodeFadeType_) {
            populateIntegerChoice(nodeFadeType_, nodeFadeTypeValues_, kFadeTypeOptions,
                                  "0", 0, "Unknown nonzero value (runtime treats as Fade in; preserve until changed)");
        }
        if (nodeFadeColorPicker_) nodeFadeColorPicker_->SetColour(*wxBLACK);
        loadedCameraFovPresent_ = false;
        loadedCameraFovRaw_.clear();
        loadedCamVidEffectPresent_ = false;
        loadedCamVidEffectRaw_.clear();
        loadedFadeColorPresent_ = false;
        loadedFadeColorRaw_.clear();
        fadeColorEdited_ = false;
        updateCameraControls();
        for (wxCheckBox* check : {nodeJadeSkippable_, nodeUnskippable_}) {
            if (check) check->SetValue(false);
        }
        if (actionParamFields_) actionParamFields_->ClearValues();
        clearLinkControls();
        animationValues_.clear();
        if (animationList_) animationList_->DeleteAllItems();
        for (wxButton* button : {animationAddButton_, animationEditButton_, animationDeleteButton_}) {
            if (button) button->Enable(false);
        }
    }

    void clearLinkControls() {
        for (wxTextCtrl* control : {linkActive1_, linkActive2_, linkLogic_, linkParamStrA_, linkParamStrB_, linkDesignerNumber_})
            if (control) control->ChangeValue("");
        for (wxCheckBox* check : {linkNot1_, linkNot2_, linkReverseCond_}) if (check) check->SetValue(false);
        if (linkParamFields_) linkParamFields_->ClearValues();
    }

    void refreshAnimationList() {
        if (!animationList_) return;
        animationList_->DeleteAllItems();
        animationValues_.clear();

        const bool hasNode = hasActiveDocument() && activeDocument().selectedNode.has_value();
        if (!hasNode) {
            for (wxButton* button : {animationAddButton_, animationEditButton_, animationDeleteButton_}) {
                if (button) button->Enable(false);
            }
            return;
        }

        const DlgDocument document = dialogue();
        const DlgNodeRef ref = *activeDocument().selectedNode;
        animationValues_ = document.animations(ref);
        const bool jade = document.dialect() == DlgDialect::JadeEmpire;
        const bool jadeReply = jade && ref.kind == DlgNodeKind::Reply;
        const std::vector<std::string> tags = jade ? document.speakerTags() : std::vector<std::string>{};

        animationList_->SetColumnWidth(0, jadeReply ? 0 : FromDIP(220));
        animationList_->SetColumnWidth(1, FromDIP(150));
        animationList_->SetColumnWidth(2, jade ? FromDIP(130) : 0);
        // A document/dialect refresh resets the legacy widths just as before.
        // Keep them separately while the same list is using compact columns.
        if (animationCompact_) {
            for (int column = 0; column < 3; ++column)
                animationConversationWidths_[column] = animationList_->GetColumnWidth(column);
        }

        for (std::size_t i = 0; i < animationValues_.size(); ++i) {
            const DlgAnimation& animation = animationValues_[i];
            const std::string participant = jade
                ? (jadeReply ? std::string{} : jadeParticipantText(animation.participantIndex, tags, true))
                : animation.participant;
            const long row = animationList_->InsertItem(static_cast<long>(i), wxui::toWx(participant));
            animationList_->SetItem(row, 1, wxString::Format("%d", animation.animation));
            animationList_->SetItem(row, 2, wxString::Format("%d", animation.emotion));
        }

        if (jadeReply && !animationValues_.empty()) {
            animationList_->SetItemState(0, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
        }

        const bool hasSelection = !animationValues_.empty();
        if (animationAddButton_) animationAddButton_->Enable(!jadeReply);
        if (animationEditButton_) animationEditButton_->Enable(hasSelection);
        if (animationDeleteButton_) animationDeleteButton_->Enable(!jadeReply && hasSelection);
    }

    void refreshCompactAnimationActions() {
        if (!singlePanelActive_ || !animationList_) return;
        const bool hasNode = hasActiveDocument() && model().loaded() &&
            dialogue().semanticallyEditable() && activeDocument().selectedNode.has_value();
        const bool jadeReply = hasNode && dialogue().dialect() == DlgDialect::JadeEmpire &&
            activeDocument().selectedNode->kind == DlgNodeKind::Reply;
        const long row = selectedAnimationRow();
        const bool selected = hasNode && row >= 0 &&
            static_cast<std::size_t>(row) < animationValues_.size();
        animationAddButton_->Enable(hasNode && !jadeReply);
        animationEditButton_->Enable(selected);
        animationDeleteButton_->Enable(selected && !jadeReply);
    }

    void refreshAnimationPresentation() {
        if (!animationList_ || !animationSummary_) return;
        const bool hasNode = hasActiveDocument() && model().loaded() &&
            dialogue().semanticallyEditable() && activeDocument().selectedNode.has_value();
        const bool jade = hasNode && dialogue().dialect() == DlgDialect::JadeEmpire;
        const bool jadeReply = jade && activeDocument().selectedNode->kind == DlgNodeKind::Reply;
        const std::size_t count = animationValues_.size();
        const auto heading = [this](int column, const wxString& text) {
            wxListItem item;
            item.SetMask(wxLIST_MASK_TEXT);
            animationList_->GetColumn(column, item);
            if (item.GetText() != text) {
                item.SetMask(wxLIST_MASK_TEXT);
                item.SetText(text);
                animationList_->SetColumn(column, item);
            }
        };
        const auto exactButtons = [this](bool compact) {
            for (auto* button : {animationAddButton_, animationEditButton_, animationDeleteButton_}) {
                const long style = button->GetWindowStyleFlag();
                const long desired = compact ? style | wxBU_EXACTFIT : style & ~wxBU_EXACTFIT;
                if (style != desired) {
                    button->SetWindowStyleFlag(desired);
                    button->InvalidateBestSize();
                }
            }
        };
        if (!singlePanelActive_) {
            if (!animationCompact_) return;
            // Restore Conversation's full-height list, original headers,
            // column widths and action row without reloading any records.
            animationCompact_ = false;
            animationSummary_->Hide();
            animationList_->Show();
            animationList_->SetMinSize(FromDIP(wxSize(-1, 220)));
            animationList_->SetMaxSize(wxDefaultSize);
            animationList_->SetToolTip(wxString{});
            heading(1, "Animation ID");
            heading(2, "Emotion ID");
            for (int column = 0; column < 3; ++column)
                animationList_->SetColumnWidth(column, animationConversationWidths_[column]);
            exactButtons(false);
            for (auto* button : {animationAddButton_, animationEditButton_, animationDeleteButton_})
                button->Show();
            animationAddButton_->Enable(hasNode && !jadeReply);
            animationEditButton_->Enable(hasNode && count != 0);
            animationDeleteButton_->Enable(hasNode && !jadeReply && count != 0);
            animationList_->InvalidateBestSize();
            return;
        }
        if (!animationCompact_) {
            for (int column = 0; column < 3; ++column)
                animationConversationWidths_[column] = animationList_->GetColumnWidth(column);
            animationCompact_ = true;
        }
        exactButtons(true);
        const wxString summary = !hasNode ? wxString("Select a dialogue node") :
            count == 0 ? wxString("No animations") :
            count == 1 ? wxString("1 animation") : wxString::Format("%zu animations", count);
        if (animationSummary_->GetLabel() != summary) animationSummary_->SetLabel(summary);
        animationSummary_->Show();
        animationAddButton_->Show(!jadeReply);
        animationEditButton_->Show(count != 0);
        animationDeleteButton_->Show(!jadeReply && count != 0);
        animationList_->Show(count != 0);
        refreshCompactAnimationActions();
        if (count == 0) return; // No empty white list, header or reserved rows.

        heading(1, "Anim ID");
        heading(2, "Emotion");
        animationList_->SetToolTip("Double-click a row or press Enter to edit. "
                                   "Scroll the list for more animations.");
        const int padding = FromDIP(14);
        const auto textWidth = [this, padding](const wxString& text) {
            return animationList_->GetTextExtent(text).x + padding;
        };
        int participant = textWidth("Participant");
        int animation = textWidth("Anim ID");
        int emotion = textWidth("Emotion");
        // Numeric columns fit their actual values, not a fixed 130/150 DIP.
        // Keep every digit of unusual IDs; cap only the participant viewport.
        for (long row = 0; row < animationList_->GetItemCount(); ++row) {
            participant = std::max(participant, textWidth(animationList_->GetItemText(row, 0)));
            animation = std::max(animation, textWidth(animationList_->GetItemText(row, 1)));
            emotion = std::max(emotion, textWidth(animationList_->GetItemText(row, 2)));
        }
        participant = jadeReply ? 0 : std::min(participant, textWidth("abcdefghijklmnopqrstuvwx"));
        if (!jade) emotion = 0;
        const int available = std::max(1, singleInspector_->GetClientSize().x - FromDIP(36));
        const int verticalBar = count > 3 ? std::max(FromDIP(12),
            wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, animationList_)) : 0;
        const wxSize border = animationList_->GetWindowBorderSize();
        const int chrome = std::max(FromDIP(4), border.x) + verticalBar;
        // Give the participant only its needed width, reducing it first when
        // the inspector narrows. Native horizontal scrolling handles extremes.
        if (!jadeReply) participant = std::max(textWidth("Participant"),
            std::min(participant, available - animation - emotion - chrome));
        const std::array<int, 3> widths{participant, animation, emotion};
        for (int column = 0; column < 3; ++column)
            if (animationList_->GetColumnWidth(column) != widths[column])
                animationList_->SetColumnWidth(column, widths[column]);
        const int contentWidth = participant + animation + emotion;
        const int width = std::min(available, contentWidth + chrome);
        int rowHeight = animationList_->GetCharHeight() + FromDIP(6);
        int headerHeight = animationList_->GetCharHeight() + FromDIP(10);
        wxRect itemRect;
        const long top = animationList_->GetTopItem();
        if (top >= 0 && animationList_->GetItemRect(top, itemRect, wxLIST_RECT_BOUNDS)) {
            rowHeight = std::max(rowHeight, itemRect.height);
            // The first visible item is relative to the native list client,
            // unlike item zero which goes negative after scrolling.
            if (itemRect.y >= 0) headerHeight = std::max(headerHeight, itemRect.y);
        }
        const int horizontalBar = contentWidth + chrome > width ? std::max(FromDIP(12),
            wxSystemSettings::GetMetric(wxSYS_HSCROLL_Y, animationList_)) : 0;
        const int height = headerHeight + static_cast<int>(std::min<std::size_t>(count, 3)) * rowHeight +
            std::max(FromDIP(4), border.y) + horizontalBar;
        const wxSize size(width, height);
        if (animationList_->GetMinSize() != size) animationList_->SetMinSize(size);
        if (animationList_->GetMaxSize() != size) animationList_->SetMaxSize(size);
        animationList_->InvalidateBestSize();
    }

    void materializeRawTreeChildren(const wxTreeItemId& parentItem, const std::string& parentPath) {
        if (!rawTree_ || !parentItem.IsOk()) return;
        if (!rawTreeMaterializedPaths_.insert(parentPath).second) return;
        const auto found = rawTreeChildrenByParent_.find(parentPath);
        if (found == rawTreeChildrenByParent_.end()) return;

        for (const std::size_t rowIndex : found->second) {
            if (rowIndex >= rawRows_.size()) continue;
            const auto& row = rawRows_[rowIndex];
            const wxTreeItemId item = rawTree_->AppendItem(
                parentItem, wxui::toWx(gffTreeText(row)), -1, -1,
                new RawGffTreeItemData(row.path, static_cast<int>(rowIndex)));
            rawTreeRowItems_[rowIndex] = item;
            rawTreeItemsByPath_[row.path] = item;
            const auto grandchildren = rawTreeChildrenByParent_.find(row.path);
            if (grandchildren != rawTreeChildrenByParent_.end() && !grandchildren->second.empty()) {
                rawTree_->SetItemHasChildren(item, true);
            }
        }
    }

    wxTreeItemId ensureRawTreeItemForKey(const std::string& key) {
        if (!rawTree_) return {};
        const wxTreeItemId root = rawTree_->GetRootItem();
        if (key == "$root") return root;

        const auto existing = rawTreeItemsByPath_.find(key);
        if (existing != rawTreeItemsByPath_.end() && existing->second.IsOk()) {
            return existing->second;
        }

        const std::string parentPath = gffTreeParentPath(key);
        const wxTreeItemId parent = parentPath.empty()
            ? root
            : ensureRawTreeItemForKey(parentPath);
        if (!parent.IsOk()) return {};
        materializeRawTreeChildren(parent, parentPath);

        const auto created = rawTreeItemsByPath_.find(key);
        return created == rawTreeItemsByPath_.end() ? wxTreeItemId{} : created->second;
    }

    void refreshRawTree() {
        if (!rawTree_ || !hasActiveDocument()) return;
        if (rawTreeRenderedDocumentPage_ == activeDocument().tabPage) {
            captureRenderedRawTreeState();
        }

        wxWindowUpdateLocker updateLocker(rawTree_);
        rawRows_.clear();
        rawTreeChildrenByParent_.clear();
        rawTreeMaterializedPaths_.clear();
        rawTreeItemsByPath_.clear();
        rawTree_->DeleteAllItems();

        const std::string filter = lowerAscii(activeDocument().rawFilterTerm);
        if (model().loaded()) {
            const auto document = dialogue();
            const DlgTreeFieldVisibility visibility(document, showOptionalFields());
            for (const auto& row : model().rows()) {
                // Filter the display projection only. Keep original GFF paths,
                // list indices and model data; never renumber or delete fields.
                if (!visibility.visible(row.path)) continue;
                if (!filter.empty()) {
                    const std::string haystack = lowerAscii(
                        row.path + " " + row.label + " " + row.type + " " + row.value + " " + row.resolved);
                    if (haystack.find(filter) == std::string::npos) continue;
                }
                rawRows_.push_back(row);
            }
        }

        const std::string rootLabel = !model().loaded()
            ? std::string("No DLG loaded")
            : (documentFilename(activeDocument()).empty()
                   ? std::string("New DLG")
                   : neosettings::pathToUtf8(documentFilename(activeDocument())));
        const wxTreeItemId root = rawTree_->AddRoot(
            wxui::toWx(rootLabel), -1, -1, new RawGffTreeItemData(std::string{}, -1));
        rawTreeRowItems_.assign(rawRows_.size(), wxTreeItemId{});

        std::unordered_map<std::string, std::size_t> visibleRows;
        visibleRows.reserve(rawRows_.size());
        for (std::size_t i = 0; i < rawRows_.size(); ++i) visibleRows.emplace(rawRows_[i].path, i);
        for (std::size_t i = 0; i < rawRows_.size(); ++i) {
            std::string parentPath = gffTreeParentPath(rawRows_[i].path);
            while (!parentPath.empty() && visibleRows.find(parentPath) == visibleRows.end()) {
                parentPath = gffTreeParentPath(parentPath);
            }
            rawTreeChildrenByParent_[parentPath].push_back(i);
        }

        materializeRawTreeChildren(root, std::string{});
        const neotree::TreeViewState& state = activeDocument().rawTreeState;
        if (state.initialized) {
            neotree::restoreTreeViewState(
                *rawTree_, state,
                [this](const std::string& key) { return ensureRawTreeItemForKey(key); });
        } else {
            rawTree_->Expand(root);
        }
        rawTreeRenderedDocumentPage_ = activeDocument().tabPage;
    }

    void onRawTreeExpanding(wxTreeEvent& event) {
        const wxTreeItemId item = event.GetItem();
        std::string path;
        if (rawTree_ && item.IsOk()) {
            if (auto* data = dynamic_cast<RawGffTreeItemData*>(rawTree_->GetItemData(item))) path = data->path();
        }
        materializeRawTreeChildren(item, path);
        event.Skip();
    }

    void onRawTreeActivated(wxTreeEvent& event) {
        const wxTreeItemId item = event.GetItem();
        auto* data = (rawTree_ && item.IsOk())
            ? dynamic_cast<RawGffTreeItemData*>(rawTree_->GetItemData(item))
            : nullptr;
        if (!data || data->rowIndex() < 0 || data->rowIndex() >= static_cast<int>(rawRows_.size())) {
            if (rawTree_ && item.IsOk() && rawTree_->ItemHasChildren(item)) {
                if (rawTree_->IsExpanded(item)) rawTree_->Collapse(item);
                else rawTree_->Expand(item);
            }
            return;
        }

        const GffFieldRow row = rawRows_[static_cast<std::size_t>(data->rowIndex())];
        const auto document = dialogue();
        if (!DlgTreeFieldVisibility(document, showOptionalFields()).visible(row.path)) return;
        if (!row.editable) {
            if (rawTree_->ItemHasChildren(item)) {
                if (rawTree_->IsExpanded(item)) rawTree_->Collapse(item);
                else rawTree_->Expand(item);
            }
            return;
        }

        const auto value = wxui::promptText(this, "Edit GFF Value", row.label + " (" + row.type + "):", row.value);
        if (!value) return;
        mutate("Edit GFF value", [this, row, value]() { model().setValue(row.path, *value); });
    }

    bool showOptionalFields() const {
        return optionalFields_ && optionalFields_->GetValue();
    }

    void setShowOptionalFields(bool show) {
        if (optionalFields_) optionalFields_->SetValue(show);
        if (rawOptional_) rawOptional_->SetValue(show);
        refreshContextualInspector();
        if (hasActiveDocument()) refreshRawTree();
    }

    void queueContextualInspectorRefresh() {
        if (contextRefreshPending_ || IsBeingDeleted()) return;
        contextRefreshPending_ = true;
        CallAfter([this]() {
            contextRefreshPending_ = false;
            if (!IsBeingDeleted()) refreshContextualInspector();
        });
    }

    void refreshContextualInspector() {
        if (inspectorSections_.size() != 5 || !linkDisplayInactive_) return;
        const bool loaded = hasActiveDocument() && model().loaded();
        const auto kind = loaded && activeDocument().selectedNode
            ? activeDocument().selectedNode->kind : DlgNodeKind::Entry;
        updateDialectFieldVisibility(loaded ? dialogue().flavor() : DlgFlavor::Kotor, kind);
        if (loaded && activeDocument().selectedNode && dialogue().dialect() != DlgDialect::JadeEmpire)
            updateCameraControls();
    }

    void applyInspectorContextVisibility() {
        // Applicability is shared by both semantic presentations. Compact
        // sizing/reparenting remains exclusively a Single Panel concern.
        for (auto& section : inspectorSections_) section.singleHost->Show(true);
        if (auto* button = FindWindow(ID_ApplyLink)) button->Show(true);
        const auto pair = [this](wxWindow* label, wxWindow* field, bool show) {
            if (label) label->Show(show);
            if (field) {
                field->Show(show);
                inspectorFieldWindow(field)->Show(show);
            }
        };
        const auto windows = [](std::initializer_list<wxWindow*> fields, bool show) {
            for (auto* field : fields) if (field) field->Show(show);
        };
        const auto hasText = [](const wxTextCtrl* field) {
            wxString text = field->GetValue();
            text.Trim(true).Trim(false);
            return !text.empty() || field->IsModified();
        };

        const bool valid = hasActiveDocument() && model().loaded() &&
                           dialogue().semanticallyEditable() && activeDocument().selectedNode &&
                           dialogue().node(*activeDocument().selectedNode);
        if (!valid) {
            for (std::size_t i = 1; i < inspectorSections_.size(); ++i)
                inspectorSections_[i].singleHost->Hide();
            return;
        }
        const auto document = dialogue();
        const auto ref = *activeDocument().selectedNode;
        const bool jade = document.dialect() == DlgDialect::JadeEmpire;
        const bool optional = showOptionalFields();
        // No synthetic K1/K2 restriction is applied to Jade's own schema.
        const bool k2 = !jade && (inspectorFlavor_ == DlgFlavor::Kotor2 || optional);
        if (!jade) {
            pair(nodeScript2Label_, nodeScript2_, k2 || hasText(nodeScript2_));
            const auto parameters = inspectorParameterVisibility(k2, optional,
                hasText(nodeScript1_), hasText(nodeScript2_),
                {hasText(nodeActionStrA_) || actionParamFields_->HasPendingOrNonzeroValue(0),
                 hasText(nodeActionStrB_) || actionParamFields_->HasPendingOrNonzeroValue(1)});
            const bool first = parameters.first, second = parameters.second;
            pair(nodeActionStrALabel_, nodeActionStrA_, first);
            pair(nodeActionStrBLabel_, nodeActionStrB_, second);
            actionParamFields_->SetVisibleColumns(first, second);
            windows({actionParamHeading_, actionParamFields_}, first || second);
            pair(nodeQuestEntryLabel_, nodeQuestEntry_, optional || hasText(nodeQuest_) || hasText(nodeQuestEntry_));
            // Sound is verified in K1. Preserve populated/edited K2 legacy data;
            // reveal otherwise-unused legacy fields only through the override.
            pair(nodeSoundLabel_, nodeSound_, inspectorFlavor_ != DlgFlavor::Kotor2 ||
                                             optional || hasText(nodeSound_));
            pair(nodeEmotionLabel_, nodeEmotion_, k2 || hasText(nodeEmotion_));
            pair(nodeFacialAnimLabel_, nodeFacialAnim_, k2 || hasText(nodeFacialAnim_));
            pair(nodeAlienRaceLabel_, nodeAlienRace_, k2 || hasText(nodeAlienRace_));
            pair(nodeUnskippablePlaceholder_, nodeUnskippable_, k2 || nodeUnskippable_->GetValue());

            const std::string camera = selectedIntegerChoice(
                nodeCameraAngle_, nodeCameraAngleValues_, "camera angle");
            pair(nodeCameraIdLabel_, nodeCameraId_, camera == "6" || optional);
            windows({nodeCameraFov_, nodeCameraFovUnit_},
                    nodeCameraFovMode_->GetSelection() == 1 || optional);
            const std::string fade = selectedIntegerChoice(
                nodeFadeType_, nodeFadeTypeValues_, "fade type");
            // FadeType 0 means Fade OUT, not "disabled". Keep either direction's
            // authored detail fields visible; collapse only unconfigured rows.
            const bool authoredFade = document.hasNodeField(ref, "FadeColor") ||
                document.hasNodeField(ref, "FadeDelay") || document.hasNodeField(ref, "FadeLength");
            const bool fadeDetails = optional || authoredFade || fade != "0" ||
                inspectorFadeTypeEdited_ || fadeColorEdited_ ||
                nodeFadeDelay_->IsModified() || nodeFadeLength_->IsModified();
            windows({nodeFadeColorLabel_, nodeFadeColorPicker_, nodeFadeColorRLabel_, nodeFadeColorR_,
                     nodeFadeColorGLabel_, nodeFadeColorG_, nodeFadeColorBLabel_, nodeFadeColorB_,
                     nodeFadeDelayLabel_, nodeFadeDelay_, nodeFadeDelayUnit_,
                     nodeFadeLengthLabel_, nodeFadeLength_, nodeFadeLengthUnit_}, fadeDetails);
        } else {
            // The existing Jade Entry/Reply rules are unchanged. Don't reserve
            // an entire unified section solely for the not-applicable note.
            inspectorSections_[2].singleHost->Hide();
        }

        const auto link = activeDocument().selectedLink;
        const bool hasLink = link && document.link(*link) && document.targetOf(*link) == ref;
        if (!hasLink) {
            for (auto* field : linkInspectorWindows()) inspectorFieldWindow(field)->Hide();
            windows({linkActive1Label_, linkActive2Label_, linkLogicLabel_, linkParamStrALabel_,
                     linkParamStrBLabel_, linkDesignerNumberLabel_, linkNot1Placeholder_,
                     linkNot2Placeholder_, linkReverseCondPlaceholder_, linkParamHeading_}, false);
            if (auto* button = FindWindow(ID_ApplyLink)) button->Hide();
            linkHeader_->SetLabel("Select an incoming link to edit its conditions.");
            return;
        }
        if (!jade) {
            pair(linkActive2Label_, linkActive2_, k2 || hasText(linkActive2_));
            pair(linkLogicLabel_, linkLogic_, k2 || hasText(linkLogic_));
            pair(linkNot1Placeholder_, linkNot1_, k2 || linkNot1_->GetValue());
            pair(linkNot2Placeholder_, linkNot2_, k2 || linkNot2_->GetValue());
            const auto parameters = inspectorParameterVisibility(k2, optional,
                hasText(linkActive1_), hasText(linkActive2_),
                {hasText(linkParamStrA_) || linkParamFields_->HasPendingOrNonzeroValue(0),
                 hasText(linkParamStrB_) || linkParamFields_->HasPendingOrNonzeroValue(1)});
            const bool first = parameters.first, second = parameters.second;
            pair(linkParamStrALabel_, linkParamStrA_, first);
            pair(linkParamStrBLabel_, linkParamStrB_, second);
            linkParamFields_->SetVisibleColumns(first, second);
            windows({linkParamHeading_, linkParamFields_}, first || second);
            linkDisplayInactive_->Show(inspectorReplyChoiceLink(document, ref, link));
        }
    }

    bool inspectorAllowsNodeWrite(const std::string& label) const {
        if (isRemovedInspectorField(label)) return false;
        const std::initializer_list<std::pair<const char*, wxWindow*>> fields{
            {"Script2", nodeScript2_}, {"ActionParamStrA", nodeActionStrA_}, {"ActionParamStrB", nodeActionStrB_},
            {"QuestEntry", nodeQuestEntry_}, {"Sound", nodeSound_}, {"CameraID", nodeCameraId_},
            {"Emotion", nodeEmotion_}, {"FacialAnim", nodeFacialAnim_}, {"AlienRaceNode", nodeAlienRace_},
            {"NodeUnskippable", nodeUnskippable_}, {"FadeColor", nodeFadeColorPicker_},
            {"FadeDelay", nodeFadeDelay_}, {"FadeLength", nodeFadeLength_}
        };
        for (const auto& field : fields) if (label == field.first) return field.second->IsShown();
        for (int i = 1; i <= 5; ++i) {
            const std::string base = "ActionParam" + std::to_string(i);
            if (label == base) return actionParamFields_->IsShown() && actionParamFields_->ColumnShown(0);
            if (label == base + "b") return actionParamFields_->IsShown() && actionParamFields_->ColumnShown(1);
        }
        return true;
    }

    bool inspectorAllowsLinkWrite(const std::string& label) const {
        if (isRemovedInspectorField(label)) return false;
        const std::initializer_list<std::pair<const char*, wxWindow*>> fields{
            {"Active2", linkActive2_}, {"ParamStrA", linkParamStrA_}, {"ParamStrB", linkParamStrB_},
            {"Logic", linkLogic_}, {"Not", linkNot1_}, {"Not2", linkNot2_}, {"DisplayInactive", linkDisplayInactive_}
        };
        for (const auto& field : fields) if (label == field.first) return field.second->IsShown();
        for (int i = 1; i <= 5; ++i) {
            const std::string base = "Param" + std::to_string(i);
            if (label == base) return linkParamFields_->IsShown() && linkParamFields_->ColumnShown(0);
            if (label == base + "b") return linkParamFields_->IsShown() && linkParamFields_->ColumnShown(1);
        }
        return true;
    }

    void updateDialectFieldVisibility(DlgFlavor flavor, DlgNodeKind kind) {
        const bool jade = flavor == DlgFlavor::JadeEmpire;
        const bool jadeEntry = jade && kind == DlgNodeKind::Entry;
        linkDisplayInactive_->Hide();
        if (jade) {
            actionParamFields_->SetVisibleColumns(false, false);
            linkParamFields_->SetVisibleColumns(false, false);
        }

        const auto showPair = [](wxWindow* label, wxWindow* control, bool show) {
            if (label) label->Show(show);
            if (control) control->Show(show);
        };
        const auto showWindows = [](std::initializer_list<wxWindow*> windows, bool show) {
            for (wxWindow* window : windows) {
                if (window) window->Show(show);
            }
        };

        // Line fields. Jade Reply nodes contain Text only; participant, voice,
        // and skippable state belong to Jade Entry nodes.
        showPair(nodeSpeakerLabel_, nodeSpeaker_, !jade || jadeEntry);
        showPair(nodeListenerLabel_, nodeListener_, !jade || jadeEntry);
        showPair(nodeStrRefLabel_, nodeStrRef_, true);
        showPair(nodeStringTypeLabel_, nodeStringType_, jade);
        showPair(nodeLocalTextLabel_, nodeLocalText_, !jade);
        showPair(nodeResolvedTextLabel_, nodeResolvedText_, true);
        showPair(nodeVoLabel_, nodeVo_, !jade || jadeEntry);
        showPair(nodeJadeSkippablePlaceholder_, nodeJadeSkippable_, jadeEntry);
        showPair(nodeCommentLabel_, nodeComment_, !jade);

        if (nodeSpeakerLabel_) nodeSpeakerLabel_->SetLabel(jadeEntry ? "Speaker participant:" : "Speaker:");
        if (nodeListenerLabel_) nodeListenerLabel_->SetLabel(jadeEntry ? "Listener participant:" : "Listener:");
        if (nodeVoLabel_) nodeVoLabel_->SetLabel(jadeEntry ? "Voice-over ID:" : "Voice-over resref:");

        // Script fields. Jade Entry and Reply nodes have different schemas.
        showPair(nodeScript1Label_, nodeScript1_, true);
        showPair(nodeScript2Label_, nodeScript2_, !jade || jadeEntry);
        showPair(nodeScriptCamEntryLabel_, nodeScriptCamEntry_, jadeEntry);
        showPair(nodeCameraEntryLabel_, nodeCameraEntry_, jadeEntry);
        showPair(nodeScriptCamRepliesLabel_, nodeScriptCamReplies_, jadeEntry);
        showPair(nodeCameraRepliesLabel_, nodeCameraReplies_, jadeEntry);
        showPair(nodeQuestLabel_, nodeQuest_, !jade);
        showPair(nodeQuestEntryLabel_, nodeQuestEntry_, !jade);
        showPair(nodePlotIndexLabel_, nodePlotIndex_, !jade);
        showPair(nodePlotXpLabel_, nodePlotXp_, !jade);
        showPair(nodeActionStrALabel_, nodeActionStrA_, !jade);
        showPair(nodeActionStrBLabel_, nodeActionStrB_, !jade);
        showWindows({actionParamHeading_, actionParamFields_}, !jade);

        if (nodeScript1Label_) {
            nodeScript1Label_->SetLabel(jade ? (jadeEntry ? "Action script:" : "Reply script:")
                                             : "Action script 1:");
        }
        if (nodeScript2Label_) nodeScript2Label_->SetLabel(jadeEntry ? "Entry script:" : "Action script 2:");

        // KotOR presentation controls are not part of Jade's DLG runtime schema.
        showWindows({jadePresentationNote_}, jade);
        showPair(nodeSoundLabel_, nodeSound_, !jade);
        showPair(nodeDelayLabel_, nodeDelay_, !jade);
        showPair(nodeWaitFlagsLabel_, nodeWaitFlags_, !jade);
        showPair(nodeCameraAngleLabel_, nodeCameraAngle_, !jade);
        showPair(nodeCameraIdLabel_, nodeCameraId_, !jade);
        showPair(nodeCamHeightOffsetLabel_, nodeCamHeightOffset_, !jade);
        showPair(nodeTarHeightOffsetLabel_, nodeTarHeightOffset_, !jade);
        showWindows({nodeCameraFovLabel_, nodeCameraFovMode_, nodeCameraFov_, nodeCameraFovUnit_}, !jade);
        showPair(nodeCameraAnimationLabel_, nodeCameraAnimation_, !jade);
        showPair(nodeEmotionLabel_, nodeEmotion_, !jade);
        showPair(nodeFacialAnimLabel_, nodeFacialAnim_, !jade);
        showPair(nodeCamVidEffectLabel_, nodeCamVidEffectPanel_, !jade);
        showPair(nodeFadeTypeLabel_, nodeFadeType_, !jade);
        showWindows({nodeFadeColorLabel_, nodeFadeColorPicker_, nodeFadeColorRLabel_, nodeFadeColorR_,
                     nodeFadeColorGLabel_, nodeFadeColorG_, nodeFadeColorBLabel_, nodeFadeColorB_}, !jade);
        showWindows({nodeFadeDelayLabel_, nodeFadeDelay_, nodeFadeDelayUnit_,
                     nodeFadeLengthLabel_, nodeFadeLength_, nodeFadeLengthUnit_}, !jade);
        showPair(nodeAlienRaceLabel_, nodeAlienRace_, !jade);
        showPair(nodeUnskippablePlaceholder_, nodeUnskippable_, !jade);
        if (presentationApplyButton_) presentationApplyButton_->Show(!jade);

        // Jade links contain one condition, ReverseCond, DesignerNumber, and Index.
        showPair(linkActive1Label_, linkActive1_, true);
        showPair(linkActive2Label_, linkActive2_, !jade);
        showPair(linkLogicLabel_, linkLogic_, !jade);
        showPair(linkParamStrALabel_, linkParamStrA_, !jade);
        showPair(linkParamStrBLabel_, linkParamStrB_, !jade);
        showPair(linkNot1Placeholder_, linkNot1_, !jade);
        showPair(linkNot2Placeholder_, linkNot2_, !jade);
        showPair(linkDesignerNumberLabel_, linkDesignerNumber_, jade);
        showPair(linkReverseCondPlaceholder_, linkReverseCond_, jade);
        showWindows({linkParamHeading_, linkParamFields_}, !jade);
        if (linkActive1Label_) linkActive1Label_->SetLabel(jade ? "Condition script:" : "Conditional script 1:");

        setInspectorSectionTitle(0, jadeEntry ? "Entry Line" : (jade ? "Reply Line" : "Line"));
        setInspectorSectionTitle(1, jadeEntry ? "Scripts / Camera" : (jade ? "Reply Script" : "Scripts / Quest"));
        setInspectorSectionTitle(2, jade ? "Jade Presentation" : "Presentation");
        setInspectorSectionTitle(4, jadeEntry ? "Entry Animations" : (jade ? "Reply Animation" : "Animations"));
        applyInspectorContextVisibility();
        // Remove entire hidden rows, including unit sizers and their gaps.
        // This does not recreate controls or apply pending values.
        if (!singlePanelActive_) rebuildInspectorForms(false);
        refreshInspectorLayouts();
    }

    static std::string lowerAscii(std::string text) {
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return text;
    }

    void onDocumentTabChanged(wxAuiNotebookEvent& event) {
        if (tabSwitchInProgress_) { event.Skip(); return; }
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::pageForIndex(documentTabs_, event.GetSelection()));
        if (index != neotabs::npos) selectDocumentTab(index);
        event.Skip();
    }

    void onDocumentTabCloseRequested(wxAuiNotebookEvent& event) {
        event.Veto();
        const std::size_t index = neotabs::findDocumentIndexForPage(
            documents_, neotabs::pageForIndex(documentTabs_, event.GetSelection()));
        if (index != neotabs::npos) closeDocument(index);
    }

    void onCloseTab(wxCommandEvent&) {
        if (hasActiveDocument()) closeDocument(activeDocumentIndex_);
    }

    void onCloseOtherTabs(wxCommandEvent&) {
        if (!hasActiveDocument()) return;
        for (std::size_t i = documents_.size(); i-- > 0;) {
            if (i != activeDocumentIndex_ && !closeDocument(i)) return;
        }
    }

    void onNextTab(wxCommandEvent&) {
        if (documents_.size() < 2) return;
        selectDocumentTab((activeDocumentIndex_ + 1) % documents_.size());
    }

    void onPreviousTab(wxCommandEvent&) {
        if (documents_.size() < 2) return;
        selectDocumentTab(activeDocumentIndex_ == 0 ? documents_.size() - 1 : activeDocumentIndex_ - 1);
    }

    void onToggleDarkMode(wxCommandEvent&) {
        darkMode_ = darkModeItem_ && darkModeItem_->IsChecked();
        wxui::writeDarkMode(kAppName, darkMode_);
        applyDarkMode();
    }

    void applyDarkMode() {
        if (darkModeItem_) darkModeItem_->Check(darkMode_);
        wxui::applyTheme(this, darkMode_);
        if (hasActiveDocument() && activeDocument().workspaceView != WorkspaceView::Raw) {
            refreshConversationTree();
        }
        Refresh();
    }

    void applyFontScale() {
        neoview::applyFontScale(this, fontScale_);
        refreshInspectorLayouts();
        Layout();
    }

    void changeFontScaleSteps(int steps) {
        fontScale_ = neoview::steppedFontScale(fontScale_, steps);
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }

    void onIncreaseFontScale(wxCommandEvent&) { changeFontScaleSteps(1); }
    void onDecreaseFontScale(wxCommandEvent&) { changeFontScaleSteps(-1); }
    void onResetFontScale(wxCommandEvent&) {
        fontScale_ = neoview::kDefaultFontScale;
        settings_.setFontScale(fontScale_);
        applyFontScale();
    }



    neosettings::AppSettings settings_;
    std::vector<DocumentTab> documents_;
    std::size_t activeDocumentIndex_ = neotabs::npos;
    bool tabSwitchInProgress_ = false;
    bool treeRefreshInProgress_ = false;
    bool browserSaveActive_ = false;

    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;
    wxMenu* recentFilesMenu_ = nullptr;
    wxMenuItem* undoItem_ = nullptr;
    wxMenuItem* redoItem_ = nullptr;
    wxMenuItem* conversationViewItem_ = nullptr;
    wxMenuItem* singlePanelViewItem_ = nullptr;
    wxMenuItem* rawViewItem_ = nullptr;
    wxMenuItem* darkModeItem_ = nullptr;

    wxAuiNotebook* documentTabs_ = nullptr;
    wxNotebook* workspaceBook_ = nullptr;
    wxPanel* conversationWorkspacePage_ = nullptr;
    wxPanel* singlePanelWorkspacePage_ = nullptr;
    wxPanel* semanticWorkspace_ = nullptr;
    wxBoxSizer* conversationWorkspaceSizer_ = nullptr;
    wxBoxSizer* singlePanelWorkspaceSizer_ = nullptr;
    wxBoxSizer* semanticToolbarSizer_ = nullptr;
    std::vector<wxButton*> semanticToolbarButtons_;
    wxStaticText* findLabel_ = nullptr;
    wxButton* findNextButton_ = nullptr;

    wxTreeCtrl* conversationTree_ = nullptr;
    wxPanel* inspectorHost_ = nullptr;
    wxNotebook* inspectorBook_ = nullptr;
    wxScrolledWindow* singleInspector_ = nullptr;
    wxBoxSizer* singleInspectorSizer_ = nullptr;
    std::vector<InspectorSection> inspectorSections_;
    std::vector<CompactInspectorBand> compactInspectorBands_;
    std::vector<wxSizerItem*> compactInspectorRows_;
    bool singlePanelActive_ = false;
    wxCheckBox* optionalFields_ = nullptr;
    wxCheckBox* rawOptional_ = nullptr;
    DlgFlavor inspectorFlavor_ = DlgFlavor::Kotor;
    bool contextRefreshPending_ = false;
    bool inspectorFadeTypeEdited_ = false;
    bool inspectorLayoutRefreshPending_ = false;
    bool inspectorLayoutRefreshInProgress_ = false;
    wxSize inspectorClientSize_;
    wxScrolledWindow* wheelTarget_ = nullptr;
    int wheelRotation_ = 0;
    std::vector<ResizableInspectorText*> resizableInspectorText_;
    wxTextCtrl* findText_ = nullptr;
    std::map<DlgNodeRef, wxTreeItemId> canonicalTreeItems_;
    std::unordered_map<std::string, wxTreeItemId> conversationTreeItemsByKey_;
    wxWindow* conversationTreeRenderedDocumentPage_ = nullptr;
    std::string lastSearchTerm_;
    std::vector<DlgNodeRef> searchResults_;
    std::size_t searchIndex_ = 0;

    wxStaticText* nodeHeader_ = nullptr;
    wxTextCtrl* conversationNodeHeader_ = nullptr;
    struct InspectorFieldMetrics {
        wxWindow* control;
        wxString widthSample;
        wxSize conversationMin;
    };
    std::vector<InspectorFieldMetrics> inspectorFieldMetrics_;
    wxStaticText* nodeSpeakerLabel_ = nullptr;
    wxStaticText* nodeListenerLabel_ = nullptr;
    wxStaticText* nodeStrRefLabel_ = nullptr;
    wxStaticText* nodeStringTypeLabel_ = nullptr;
    wxStaticText* nodeLocalTextLabel_ = nullptr;
    wxStaticText* nodeResolvedTextLabel_ = nullptr;
    wxStaticText* nodeVoLabel_ = nullptr;
    wxStaticText* nodeJadeSkippablePlaceholder_ = nullptr;
    wxStaticText* nodeCommentLabel_ = nullptr;
    wxComboBox* nodeSpeaker_ = nullptr;
    wxComboBox* nodeListener_ = nullptr;
    wxTextCtrl* nodeStrRef_ = nullptr;
    wxTextCtrl* nodeStringType_ = nullptr;
    wxTextCtrl* nodeLocalText_ = nullptr;
    wxTextCtrl* nodeResolvedText_ = nullptr;
    wxTextCtrl* nodeVo_ = nullptr;
    wxCheckBox* nodeJadeSkippable_ = nullptr;
    wxTextCtrl* nodeComment_ = nullptr;

    wxStaticText* nodeScript1Label_ = nullptr;
    wxStaticText* nodeScript2Label_ = nullptr;
    wxStaticText* nodeScriptCamEntryLabel_ = nullptr;
    wxStaticText* nodeCameraEntryLabel_ = nullptr;
    wxStaticText* nodeScriptCamRepliesLabel_ = nullptr;
    wxStaticText* nodeCameraRepliesLabel_ = nullptr;
    wxStaticText* nodeQuestLabel_ = nullptr;
    wxStaticText* nodeQuestEntryLabel_ = nullptr;
    wxStaticText* nodePlotIndexLabel_ = nullptr;
    wxStaticText* nodePlotXpLabel_ = nullptr;
    wxStaticText* nodeActionStrALabel_ = nullptr;
    wxStaticText* nodeActionStrBLabel_ = nullptr;
    wxStaticText* actionParamHeading_ = nullptr;
    wxTextCtrl* nodeScript1_ = nullptr;
    wxTextCtrl* nodeScript2_ = nullptr;
    wxTextCtrl* nodeScriptCamEntry_ = nullptr;
    wxTextCtrl* nodeCameraEntry_ = nullptr;
    wxTextCtrl* nodeScriptCamReplies_ = nullptr;
    wxTextCtrl* nodeCameraReplies_ = nullptr;
    wxTextCtrl* nodeQuest_ = nullptr;
    wxTextCtrl* nodeQuestEntry_ = nullptr;
    wxTextCtrl* nodePlotIndex_ = nullptr;
    wxTextCtrl* nodePlotXp_ = nullptr;
    wxTextCtrl* nodeActionStrA_ = nullptr;
    wxTextCtrl* nodeActionStrB_ = nullptr;
    IntegerParameterFields* actionParamFields_ = nullptr;

    wxStaticText* jadePresentationNote_ = nullptr;
    wxButton* presentationApplyButton_ = nullptr;
    wxStaticText* nodeSoundLabel_ = nullptr;
    wxTextCtrl* nodeSound_ = nullptr;
    wxStaticText* nodeDelayLabel_ = nullptr;
    wxTextCtrl* nodeDelay_ = nullptr;
    wxStaticText* nodeWaitFlagsLabel_ = nullptr;
    wxTextCtrl* nodeWaitFlags_ = nullptr;

    wxStaticText* nodeCameraAngleLabel_ = nullptr;
    wxChoice* nodeCameraAngle_ = nullptr;
    std::vector<std::string> nodeCameraAngleValues_;
    wxStaticText* nodeCameraIdLabel_ = nullptr;
    wxTextCtrl* nodeCameraId_ = nullptr;
    wxStaticText* nodeCamHeightOffsetLabel_ = nullptr;
    wxTextCtrl* nodeCamHeightOffset_ = nullptr;
    wxStaticText* nodeTarHeightOffsetLabel_ = nullptr;
    wxTextCtrl* nodeTarHeightOffset_ = nullptr;
    wxStaticText* nodeCameraFovLabel_ = nullptr;
    wxChoice* nodeCameraFovMode_ = nullptr;
    wxTextCtrl* nodeCameraFov_ = nullptr;
    wxStaticText* nodeCameraFovUnit_ = nullptr;
    bool loadedCameraFovPresent_ = false;
    std::string loadedCameraFovRaw_;

    wxStaticText* nodeCameraAnimationLabel_ = nullptr;
    wxTextCtrl* nodeCameraAnimation_ = nullptr;
    wxStaticText* nodeEmotionLabel_ = nullptr;
    wxTextCtrl* nodeEmotion_ = nullptr;
    wxStaticText* nodeFacialAnimLabel_ = nullptr;
    wxTextCtrl* nodeFacialAnim_ = nullptr;
    wxStaticText* nodeCamVidEffectLabel_ = nullptr;
    wxPanel* nodeCamVidEffectPanel_ = nullptr;
    wxChoice* nodeCamVidEffectChoice_ = nullptr;
    std::vector<std::string> nodeCamVidEffectValues_;
    bool loadedCamVidEffectPresent_ = false;
    std::string loadedCamVidEffectRaw_;

    wxStaticText* nodeFadeTypeLabel_ = nullptr;
    wxChoice* nodeFadeType_ = nullptr;
    std::vector<std::string> nodeFadeTypeValues_;
    wxStaticText* nodeFadeColorLabel_ = nullptr;
    wxColourPickerCtrl* nodeFadeColorPicker_ = nullptr;
    wxStaticText* nodeFadeColorRLabel_ = nullptr;
    wxTextCtrl* nodeFadeColorR_ = nullptr;
    wxStaticText* nodeFadeColorGLabel_ = nullptr;
    wxTextCtrl* nodeFadeColorG_ = nullptr;
    wxStaticText* nodeFadeColorBLabel_ = nullptr;
    wxTextCtrl* nodeFadeColorB_ = nullptr;
    bool loadedFadeColorPresent_ = false;
    bool fadeColorEdited_ = false;
    bool updatingFadeColor_ = false;
    std::string loadedFadeColorRaw_;
    wxStaticText* nodeFadeDelayLabel_ = nullptr;
    wxTextCtrl* nodeFadeDelay_ = nullptr;
    wxStaticText* nodeFadeDelayUnit_ = nullptr;
    wxStaticText* nodeFadeLengthLabel_ = nullptr;
    wxTextCtrl* nodeFadeLength_ = nullptr;
    wxStaticText* nodeFadeLengthUnit_ = nullptr;

    wxStaticText* nodeAlienRaceLabel_ = nullptr;
    wxTextCtrl* nodeAlienRace_ = nullptr;
    wxStaticText* nodeUnskippablePlaceholder_ = nullptr;
    wxCheckBox* nodeUnskippable_ = nullptr;

    wxStaticText* linkHeader_ = nullptr;
    wxStaticText* linkActive1Label_ = nullptr;
    wxTextCtrl* linkActive1_ = nullptr;
    wxStaticText* linkActive2Label_ = nullptr;
    wxTextCtrl* linkActive2_ = nullptr;
    wxStaticText* linkLogicLabel_ = nullptr;
    wxTextCtrl* linkLogic_ = nullptr;
    wxStaticText* linkParamStrALabel_ = nullptr;
    wxTextCtrl* linkParamStrA_ = nullptr;
    wxStaticText* linkParamStrBLabel_ = nullptr;
    wxTextCtrl* linkParamStrB_ = nullptr;
    wxCheckBox* linkDisplayInactive_ = nullptr;
    bool linkDisplayInactiveEdited_ = false;
    wxStaticText* linkDesignerNumberLabel_ = nullptr;
    wxTextCtrl* linkDesignerNumber_ = nullptr;
    wxStaticText* linkNot1Placeholder_ = nullptr;
    wxCheckBox* linkNot1_ = nullptr;
    wxStaticText* linkNot2Placeholder_ = nullptr;
    wxCheckBox* linkNot2_ = nullptr;
    wxStaticText* linkReverseCondPlaceholder_ = nullptr;
    wxCheckBox* linkReverseCond_ = nullptr;
    wxStaticText* linkParamHeading_ = nullptr;
    IntegerParameterFields* linkParamFields_ = nullptr;

    wxStaticText* animationSummary_ = nullptr;
    wxListCtrl* animationList_ = nullptr;
    bool animationCompact_ = false;
    std::array<int, 3> animationConversationWidths_{};
    wxButton* animationAddButton_ = nullptr;
    wxButton* animationEditButton_ = nullptr;
    wxButton* animationDeleteButton_ = nullptr;
    std::vector<DlgAnimation> animationValues_;

    wxTextCtrl* rawFilter_ = nullptr;
    wxTreeCtrl* rawTree_ = nullptr;
    std::vector<GffFieldRow> rawRows_;
    std::vector<wxTreeItemId> rawTreeRowItems_;
    std::unordered_map<std::string, wxTreeItemId> rawTreeItemsByPath_;
    std::unordered_map<std::string, std::vector<std::size_t>> rawTreeChildrenByParent_;
    std::set<std::string> rawTreeMaterializedPaths_;
    wxWindow* rawTreeRenderedDocumentPage_ = nullptr;

    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    double fontScale_ = neoview::kDefaultFontScale;
    bool darkMode_ = false;
};


} // namespace

namespace neodlg::ui {
DLGEditorPanel* createEditorPanel(wxWindow* parent, neomodules::Context context) {
    return new NeoDLGPanelImpl(parent, std::move(context));
}
}
