#pragma once

#include "neodlg/model/DlgDocument.hpp"
#include "neodlg/patcher/DlgPatcher.hpp"

#include "NeoWxUi.hpp"

#include <wx/listctrl.h>
#include <wx/radiobut.h>
#include <wx/srchctrl.h>
#include <wx/statline.h>
#include <wx/tokenzr.h>
#include <wx/wx.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neodlggui {

using neodlg::DlgNodeRef;

class PatcherExportModeDialog final : public wxDialog {
public:
    PatcherExportModeDialog(wxWindow* parent, bool darkMode)
        : wxDialog(parent, wxID_ANY, "TSLPatcher Export Mode", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          dynamicDescriptionText_(
              "Appends new Entry and Reply nodes at installation time and connects them with "
              "ListIndex and 2DAMEMORY tokens. This is merge-friendly, but it requires original "
              "TSLPatcher 1.2.10b1 or a HoloPatcher release that supports the same dynamic GFF "
              "syntax. Deletions and root-list reordering are rejected."),
          replacementDescriptionText_(
              "Installs the entire modified DLG. This mode does not use dynamic tokens and works "
              "with patchers that cannot process TypeId=ListIndex, but it can conflict with "
              "another mod that replaces the same DLG.") {
        auto* root = new wxBoxSizer(wxVERTICAL);
        root->Add(new wxStaticText(this, wxID_ANY,
                                   "Choose how this package should install the dialogue:"),
                  0, wxEXPAND | wxALL, 12);

        dynamicMode_ = new wxRadioButton(
            this, wxID_ANY, "Dynamic merge instructions (recommended)",
            wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        dynamicMode_->SetValue(true);
        root->Add(dynamicMode_, 0, wxEXPAND | wxLEFT | wxRIGHT, 12);

        dynamicDescription_ = new wxStaticText(this, wxID_ANY, dynamicDescriptionText_);
        root->Add(dynamicDescription_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        replacementMode_ = new wxRadioButton(
            this, wxID_ANY, "Complete modified DLG replacement");
        root->Add(replacementMode_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        replacementDescription_ = new wxStaticText(this, wxID_ANY, replacementDescriptionText_);
        root->Add(replacementDescription_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 12);

        auto* buttons = new wxStdDialogButtonSizer();
        buttons->AddButton(new wxButton(this, wxID_OK));
        buttons->AddButton(new wxButton(this, wxID_CANCEL));
        buttons->Realize();
        root->AddStretchSpacer(1);
        root->Add(new wxStaticLine(this), 0, wxEXPAND | wxTOP, 12);
        root->Add(buttons, 0, wxEXPAND | wxALL, 12);

        SetSizer(root);
        wxui::configureResponsiveWindow(*this, wxSize(680, 440), wxSize(480, 340));
        rewrapDescriptions();
        wxui::applyTheme(this, darkMode);

        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            rewrapDescriptions();
            event.Skip();
        });
    }

    neodlg::patcher::DlgPatchMode selectedMode() const noexcept {
        return replacementMode_ && replacementMode_->GetValue()
            ? neodlg::patcher::DlgPatchMode::CompleteReplacement
            : neodlg::patcher::DlgPatchMode::DynamicMerge;
    }

private:
    void rewrapDescriptions() {
        if (!dynamicDescription_ || !replacementDescription_) return;
        const int horizontalPadding = FromDIP(wxSize(60, 0)).GetWidth();
        const int width = std::max(FromDIP(wxSize(300, 0)).GetWidth(),
                                   GetClientSize().GetWidth() - horizontalPadding);

        dynamicDescription_->SetLabel(dynamicDescriptionText_);
        dynamicDescription_->Wrap(width);
        replacementDescription_->SetLabel(replacementDescriptionText_);
        replacementDescription_->Wrap(width);
        Layout();
    }

    wxRadioButton* dynamicMode_ = nullptr;
    wxRadioButton* replacementMode_ = nullptr;
    wxStaticText* dynamicDescription_ = nullptr;
    wxStaticText* replacementDescription_ = nullptr;
    wxString dynamicDescriptionText_;
    wxString replacementDescriptionText_;
};

struct ExistingNodeChoice {
    DlgNodeRef ref;
    wxString listIndex;
    wxString nodeId;
    wxString speaker;
    wxString visibleText;
    wxString searchText;
};

class ExistingNodeDialog final : public wxDialog {
public:
    ExistingNodeDialog(wxWindow* parent,
                       const wxString& title,
                       std::vector<ExistingNodeChoice> choices,
                       bool darkMode)
        : wxDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          choices_(std::move(choices)) {
        auto* root = new wxBoxSizer(wxVERTICAL);

        auto* instructions = new wxStaticText(
            this, wxID_ANY,
            "Search by dialogue text, list index, NodeID, speaker, or StrRef. "
            "Select a node and choose Link, or double-click it.");
        root->Add(instructions, 0, wxEXPAND | wxALL, 10);

        search_ = new wxSearchCtrl(
            this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
            wxTE_PROCESS_ENTER);
        search_->SetDescriptiveText("Search node text or index");
        search_->ShowCancelButton(true);
        root->Add(search_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

        list_ = new wxListCtrl(
            this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
            wxLC_REPORT | wxLC_SINGLE_SEL);
        list_->AppendColumn("List index", wxLIST_FORMAT_RIGHT);
        list_->AppendColumn("NodeID", wxLIST_FORMAT_RIGHT);
        list_->AppendColumn("Speaker");
        list_->AppendColumn("Text");
        root->Add(list_, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

        countLabel_ = new wxStaticText(this, wxID_ANY, wxEmptyString);
        root->Add(countLabel_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, 10);

        auto* buttons = new wxStdDialogButtonSizer();
        okButton_ = new wxButton(this, wxID_OK, "Link");
        okButton_->Enable(false);
        buttons->AddButton(okButton_);
        buttons->AddButton(new wxButton(this, wxID_CANCEL));
        buttons->Realize();
        root->Add(new wxStaticLine(this), 0, wxEXPAND | wxTOP, 10);
        root->Add(buttons, 0, wxEXPAND | wxALL, 10);

        SetSizer(root);
        wxui::configureResponsiveWindow(*this, wxSize(820, 540), wxSize(500, 340));
        instructions->Wrap(std::max(FromDIP(wxSize(300, 0)).GetWidth(),
                                    GetClientSize().GetWidth() - FromDIP(wxSize(40, 0)).GetWidth()));
        wxui::applyTheme(this, darkMode);

        search_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refreshList(); });
        search_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) {
            if (selectedNode()) EndModal(wxID_OK);
        });
        search_->Bind(wxEVT_SEARCH_CANCEL, [this](wxCommandEvent&) {
            search_->Clear();
            search_->SetFocus();
        });
        list_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent&) {
            updateOkButton();
        });
        list_->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) {
            updateOkButton();
        });
        list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent&) {
            if (selectedNode()) EndModal(wxID_OK);
        });
        const wxString instructionText = instructions->GetLabel();
        Bind(wxEVT_SIZE, [this, instructions, instructionText](wxSizeEvent& event) {
            instructions->SetLabel(instructionText);
            instructions->Wrap(std::max(
                FromDIP(wxSize(300, 0)).GetWidth(),
                GetClientSize().GetWidth() - FromDIP(wxSize(40, 0)).GetWidth()));
            resizeColumns();
            Layout();
            event.Skip();
        });

        refreshList();
        search_->SetFocus();
    }

    std::optional<DlgNodeRef> selectedNode() const {
        if (!list_) return std::nullopt;
        const long row = list_->GetNextItem(
            -1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
        if (row < 0 || static_cast<std::size_t>(row) >= visibleChoices_.size()) {
            return std::nullopt;
        }
        return choices_[visibleChoices_[static_cast<std::size_t>(row)]].ref;
    }

private:
    static wxString normalizedQuery(wxString value) {
        value.Trim(true);
        value.Trim(false);
        return value.Lower();
    }

    static bool matchesAllTokens(const wxString& haystack, const wxString& query) {
        wxStringTokenizer tokenizer(query, " \t\r\n");
        while (tokenizer.HasMoreTokens()) {
            if (haystack.Find(tokenizer.GetNextToken()) == wxNOT_FOUND) return false;
        }
        return true;
    }

    void refreshList() {
        if (!list_) return;
        const std::optional<DlgNodeRef> previousSelection = selectedNode();
        const wxString query = normalizedQuery(search_ ? search_->GetValue() : wxString{});

        std::vector<std::size_t> matches;
        if (query.empty()) {
            matches.reserve(choices_.size());
            for (std::size_t i = 0; i < choices_.size(); ++i) matches.push_back(i);
        } else {
            long numeric = 0;
            const bool numericQuery = query.ToLong(&numeric) && numeric >= 0;
            if (numericQuery) {
                for (std::size_t i = 0; i < choices_.size(); ++i) {
                    const auto& choice = choices_[i];
                    if (choice.ref.index == static_cast<std::size_t>(numeric) ||
                        choice.nodeId.Lower() == query) {
                        matches.push_back(i);
                    }
                }
            }

            if (matches.empty()) {
                for (std::size_t i = 0; i < choices_.size(); ++i) {
                    if (matchesAllTokens(choices_[i].searchText, query)) matches.push_back(i);
                }
            }
        }

        list_->Freeze();
        list_->DeleteAllItems();
        visibleChoices_ = matches;
        long rowToSelect = wxNOT_FOUND;
        for (std::size_t row = 0; row < visibleChoices_.size(); ++row) {
            const auto& choice = choices_[visibleChoices_[row]];
            const long item = list_->InsertItem(
                static_cast<long>(row), choice.listIndex);
            list_->SetItem(item, 1, choice.nodeId);
            list_->SetItem(item, 2, choice.speaker);
            list_->SetItem(item, 3, choice.visibleText);
            if (previousSelection && choice.ref == *previousSelection) {
                rowToSelect = item;
            }
        }
        resizeColumns();

        if (!visibleChoices_.empty()) {
            if (rowToSelect == wxNOT_FOUND) rowToSelect = 0;
            list_->SetItemState(
                rowToSelect,
                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->EnsureVisible(rowToSelect);
        }
        list_->Thaw();

        const std::string count = std::to_string(visibleChoices_.size()) + " of " +
                                  std::to_string(choices_.size()) + " nodes";
        countLabel_->SetLabel(wxui::toWx(count));
        updateOkButton();
    }

    void resizeColumns() {
        if (!list_) return;
        const int width = list_->GetClientSize().GetWidth();
        if (width <= 0) return;

        const int indexWidth = FromDIP(wxSize(74, 0)).GetWidth();
        const int nodeIdWidth = FromDIP(wxSize(84, 0)).GetWidth();
        const int speakerWidth = FromDIP(wxSize(140, 0)).GetWidth();
        const int textMinimum = FromDIP(wxSize(180, 0)).GetWidth();
        const int remaining = width - indexWidth - nodeIdWidth - speakerWidth -
                              FromDIP(wxSize(16, 0)).GetWidth();

        list_->SetColumnWidth(0, indexWidth);
        list_->SetColumnWidth(1, nodeIdWidth);
        list_->SetColumnWidth(2, speakerWidth);
        list_->SetColumnWidth(3, std::max(textMinimum, remaining));
    }

    void updateOkButton() {
        if (okButton_) okButton_->Enable(selectedNode().has_value());
    }

    std::vector<ExistingNodeChoice> choices_;
    std::vector<std::size_t> visibleChoices_;
    wxSearchCtrl* search_ = nullptr;
    wxListCtrl* list_ = nullptr;
    wxStaticText* countLabel_ = nullptr;
    wxButton* okButton_ = nullptr;
};


} // namespace neodlggui
