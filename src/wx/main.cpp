#include "core/AppModel.hpp"
#include "core/GffJson.hpp"
#include "neodlg/model/DlgDocument.hpp"
#include "neodlg/patcher/DlgPatcher.hpp"
#include "neodlg_icon.xpm"

#include "NeoDocumentTabs.hpp"
#include "NeoGameDirectoryMenu.hpp"
#include "NeoSettings.hpp"
#include "NeoViewState.hpp"
#include "NeoWxUi.hpp"
#include "TslPatcher.hpp"

#include <wx/aui/auibook.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/combobox.h>
#include <wx/clipbrd.h>
#include <wx/grid.h>
#include <wx/icon.h>
#include <wx/iconbndl.h>
#include <wx/listctrl.h>
#include <wx/notebook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/statline.h>
#include <wx/treectrl.h>
#include <wx/wupdlock.h>
#include <wx/wx.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
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

wxTextCtrl* addTextField(wxWindow* parent,
                         wxFlexGridSizer* form,
                         const wxString& label,
                         long style = 0,
                         const wxSize& minSize = wxDefaultSize) {
    form->Add(new wxStaticText(parent, wxID_ANY, label), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
    auto* control = new wxTextCtrl(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, style);
    if (minSize != wxDefaultSize) control->SetMinSize(minSize);
    form->Add(control, 1, wxEXPAND);
    return control;
}

wxCheckBox* addCheckField(wxWindow* parent, wxFlexGridSizer* form, const wxString& label) {
    form->AddSpacer(1);
    auto* control = new wxCheckBox(parent, wxID_ANY, label);
    form->Add(control, 0, wxALIGN_CENTER_VERTICAL);
    return control;
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

class AnimationEditDialog final : public wxDialog {
public:
    AnimationEditDialog(wxWindow* parent, bool jade, const DlgAnimation& initial)
        : wxDialog(parent, wxID_ANY, "Dialogue Animation", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          jade_(jade) {
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* form = new wxFlexGridSizer(2, 8, 8);
        form->AddGrowableCol(1, 1);

        participant_ = addTextField(this, form, "Participant:");
        participant_->ChangeValue(wxui::toWx(initial.participant));
        participant_->Enable(!jade_);

        animation_ = addTextField(this, form, "Animation ID:");
        animation_->ChangeValue(wxString::Format("%d", initial.animation));

        emotion_ = addTextField(this, form, "Emotion ID:");
        emotion_->ChangeValue(wxString::Format("%d", initial.emotion));
        emotion_->Enable(jade_);

        root->Add(form, 1, wxEXPAND | wxALL, 12);
        root->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);
        SetSizerAndFit(root);
        SetMinSize(FromDIP(wxSize(420, 220)));
    }

    DlgAnimation value() const {
        DlgAnimation result;
        result.participant = wxui::toStd(participant_->GetValue());
        result.animation = neogff::ParseInt32Decimal(wxui::toStd(animation_->GetValue()));
        result.emotion = neogff::ParseInt32Decimal(wxui::toStd(emotion_->GetValue()));
        return result;
    }

private:
    bool jade_ = false;
    wxTextCtrl* participant_ = nullptr;
    wxTextCtrl* animation_ = nullptr;
    wxTextCtrl* emotion_ = nullptr;
};

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
            addRootText(document, basics, form, "ConversationType", "Conversation type:");
            addRootText(document, basics, form, "ComputerType", "Computer type:");
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
            auto* tagPage = new wxPanel(book);
            auto* tagSizer = new wxBoxSizer(wxVERTICAL);
            tagSizer->Add(new wxStaticText(tagPage, wxID_ANY,
                                           "Jade Empire speaker indices refer to this TagList."),
                          0, wxLEFT | wxRIGHT | wxTOP, 10);
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
            book->AddPage(tagPage, "Speakers", true);
            add->Bind(wxEVT_BUTTON, &ConversationPropertiesDialog::onAddTag, this);
            edit->Bind(wxEVT_BUTTON, &ConversationPropertiesDialog::onEditTag, this);
            remove->Bind(wxEVT_BUTTON, &ConversationPropertiesDialog::onDeleteTag, this);
        }

        root->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        SetSizer(root);
        SetMinSize(FromDIP(wxSize(680, 540)));
        SetInitialSize(FromDIP(wxSize(760, 620)));
    }

    void apply(DlgDocument& document) const {
        if (jade_) {
            document.replaceSpeakerTags(tagValues_);
            return;
        }
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
            } else if (item.first == "ConversationType" || item.first == "NextNodeID" ||
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
    void addRootText(const DlgDocument& document,
                     wxWindow* page,
                     wxFlexGridSizer* form,
                     const std::string& field,
                     const wxString& label) {
        wxTextCtrl* control = addTextField(page, form, label);
        control->ChangeValue(wxui::toWx(document.rootField(field)));
        rootText_[field] = control;
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
        tagValues_.push_back(*tag);
        refreshTags();
    }

    void onEditTag(wxCommandEvent&) {
        const long row = selectedRow(tags_);
        if (row < 0 || static_cast<std::size_t>(row) >= tagValues_.size()) return;
        const auto tag = wxui::promptText(this, "Edit Speaker", "Speaker tag:", tagValues_[row]);
        if (!tag) return;
        tagValues_[row] = *tag;
        refreshTags();
        tags_->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }

    void onDeleteTag(wxCommandEvent&) {
        const long row = selectedRow(tags_);
        if (row < 0 || static_cast<std::size_t>(row) >= tagValues_.size()) return;
        tagValues_.erase(tagValues_.begin() + row);
        refreshTags();
    }

    bool jade_ = false;
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
        SetMinSize(FromDIP(wxSize(900, 520)));
        SetInitialSize(FromDIP(wxSize(980, 620)));
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
                         bool referenceOnly = false)
        : kind(itemKind), node(std::move(nodeRef)), link(std::move(linkRef)), reference(referenceOnly) {}

    ConversationTreeKind kind = ConversationTreeKind::Group;
    std::optional<DlgNodeRef> node;
    std::optional<DlgLinkRef> link;
    bool reference = false;
};

enum : int {
    ID_NewK1 = wxID_HIGHEST + 1,
    ID_NewK2,
    ID_NewJade,
    ID_Open,
    ID_Save,
    ID_SaveAs,
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
    ID_ViewRaw,
    ID_DarkMode,
    ID_FontIncrease,
    ID_FontDecrease,
    ID_FontReset,
    ID_DocumentTabs,
    ID_Workspace,
    ID_ConversationTree,
    ID_RawGrid,
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

constexpr int kRecentFileBaseId = wxID_HIGHEST + 1000;
constexpr int kClearRecentFilesId = kRecentFileBaseId + neosettings::kMaxRecentFiles;

class NeoDLGFrame final : public wxFrame {
public:
    NeoDLGFrame()
        : wxFrame(nullptr, wxID_ANY, "NeoDLG", wxDefaultPosition, wxDefaultSize),
          settings_(kAppName) {
        setApplicationIcon();
        buildMenus();
        buildWindow();
        wxui::createStatusBar(*this, 2);
        darkMode_ = wxui::readDarkMode(kAppName);
        fontScale_ = settings_.fontScale();
        fontScaleWheelFilter_.attach(this, [this](int steps) { changeFontScaleSteps(steps); });
        neoview::bindFontScaleDpiRefresh(this, [this]() { applyFontScale(); });
        createDocumentTab(true);
        tryLoadCachedTlk();
        applyDarkMode();
        SetMinSize(FromDIP(wxSize(980, 650)));
        SetInitialSize(FromDIP(wxSize(1420, 900)));
        settings_.restoreWindowPlacement(*this);
        refreshAll();
    }

    void openStartupFile(const std::filesystem::path& path) {
        if (path.empty()) return;
        try {
            openModelPath(path);
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

private:
    struct UndoSnapshot {
        std::string description;
        std::string xml;
    };

    struct DocumentTab {
        std::unique_ptr<GffModel> model = std::make_unique<GffModel>();
        std::string untitledName = "Untitled DLG";
        std::string tlkAutoLoadWarning;
        wxWindow* tabPage = nullptr;
        int workspacePage = 0;
        std::optional<DlgNodeRef> selectedNode;
        std::optional<DlgLinkRef> selectedLink;
        std::vector<UndoSnapshot> undo;
        std::vector<UndoSnapshot> redo;
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

    bool tabDirty(const DocumentTab& tab) const { return tab.model && tab.model->dirty(); }

    std::string tabDisplayName(const DocumentTab& tab) const {
        return neotabs::displayNameForPath(tab.model ? tab.model->filename() : std::filesystem::path{}, tab.untitledName);
    }

    void setApplicationIcon() {
        wxIconBundle bundle;
#if defined(__WXMSW__)
        wxIcon windowsIcon("neodlg", wxBITMAP_TYPE_ICO_RESOURCE);
        if (windowsIcon.IsOk()) bundle.AddIcon(windowsIcon);
#endif
        wxIcon fallback(neodlg_icon_xpm);
        if (fallback.IsOk()) bundle.AddIcon(fallback);
        if (bundle.GetIconCount() > 0) SetIcons(bundle);
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
        file->Append(ID_OpenTlk, "Open optional &TLK...");
        file->Append(ID_ClearTlk, "Clear TLK");
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
        file->Append(wxID_EXIT, "E&xit");

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
        exportMenu->Append(ID_ExportPatcher, "Export TSL/HoloPatcher Package...");

        auto* view = new wxMenu;
        conversationViewItem_ = view->AppendRadioItem(ID_ViewConversation, "Conversation Editor");
        rawViewItem_ = view->AppendRadioItem(ID_ViewRaw, "Raw GFF Values");
        conversationViewItem_->Check(true);
        view->AppendSeparator();
        darkModeItem_ = view->AppendCheckItem(ID_DarkMode, "Dark Mode");
        view->AppendSeparator();
        view->Append(ID_FontIncrease, "Increase Text Size\tCtrl++");
        view->Append(ID_FontDecrease, "Decrease Text Size\tCtrl+-");
        view->Append(ID_FontReset, "Reset Text Size\tCtrl+0");

        auto* help = new wxMenu;
        help->Append(wxID_ABOUT, "&About NeoDLG");

        auto* bar = new wxMenuBar;
        bar->Append(file, "&File");
        bar->Append(edit, "&Edit");
        bar->Append(dialogueMenu, "&Dialogue");
        bar->Append(importMenu, "&Import");
        bar->Append(exportMenu, "E&xport");
        bar->Append(view, "&View");
        bar->Append(help, "&Help");
        SetMenuBar(bar);

        Bind(wxEVT_MENU, [this](wxCommandEvent&) { newDocument(DlgFlavor::Kotor); }, ID_NewK1);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { newDocument(DlgFlavor::Kotor2); }, ID_NewK2);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { newDocument(DlgFlavor::JadeEmpire); }, ID_NewJade);
        Bind(wxEVT_MENU, &NeoDLGFrame::onOpen, this, ID_Open);
        Bind(wxEVT_MENU, &NeoDLGFrame::onOpenRecent, this, kRecentFileBaseId, kClearRecentFilesId);
        Bind(wxEVT_MENU, &NeoDLGFrame::onSave, this, ID_Save);
        Bind(wxEVT_MENU, &NeoDLGFrame::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_MENU, &NeoDLGFrame::onOpenTlk, this, ID_OpenTlk);
        Bind(wxEVT_MENU, &NeoDLGFrame::onClearTlk, this, ID_ClearTlk);
        Bind(wxEVT_MENU, &NeoDLGFrame::onCloseTab, this, ID_CloseTab);
        Bind(wxEVT_MENU, &NeoDLGFrame::onCloseOtherTabs, this, ID_CloseOtherTabs);
        Bind(wxEVT_MENU, &NeoDLGFrame::onNextTab, this, ID_NextTab);
        Bind(wxEVT_MENU, &NeoDLGFrame::onPreviousTab, this, ID_PreviousTab);
        Bind(wxEVT_MENU, &NeoDLGFrame::onUndo, this, ID_Undo);
        Bind(wxEVT_MENU, &NeoDLGFrame::onRedo, this, ID_Redo);
        Bind(wxEVT_MENU, &NeoDLGFrame::onAddStartingEntry, this, ID_AddStartingEntry);
        Bind(wxEVT_MENU, &NeoDLGFrame::onAddChild, this, ID_AddChild);
        Bind(wxEVT_MENU, &NeoDLGFrame::onLinkExisting, this, ID_LinkExisting);
        Bind(wxEVT_MENU, &NeoDLGFrame::onDuplicateNode, this, ID_DuplicateNode);
        Bind(wxEVT_MENU, &NeoDLGFrame::onRemoveLink, this, ID_RemoveLink);
        Bind(wxEVT_MENU, &NeoDLGFrame::onDeleteNode, this, ID_DeleteNode);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { moveSelectedLink(-1); }, ID_MoveLinkUp);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { moveSelectedLink(1); }, ID_MoveLinkDown);
        Bind(wxEVT_MENU, &NeoDLGFrame::onFindNext, this, ID_FindNext);
        Bind(wxEVT_MENU, &NeoDLGFrame::onConversationProperties, this, ID_ConversationProperties);
        Bind(wxEVT_MENU, &NeoDLGFrame::onValidate, this, ID_Validate);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(false); }, ID_ImportXml);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onImport(true); }, ID_ImportJson);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(false); }, ID_ExportXml);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { onExport(true); }, ID_ExportJson);
        Bind(wxEVT_MENU, &NeoDLGFrame::onExportPatcherPackage, this, ID_ExportPatcher);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setWorkspacePage(0); }, ID_ViewConversation);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { setWorkspacePage(1); }, ID_ViewRaw);
        Bind(wxEVT_MENU, &NeoDLGFrame::onToggleDarkMode, this, ID_DarkMode);
        Bind(wxEVT_MENU, &NeoDLGFrame::onIncreaseFontScale, this, ID_FontIncrease);
        Bind(wxEVT_MENU, &NeoDLGFrame::onDecreaseFontScale, this, ID_FontDecrease);
        Bind(wxEVT_MENU, &NeoDLGFrame::onResetFontScale, this, ID_FontReset);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            wxui::showMessage(this, "About NeoDLG",
                              "NeoDLG v2.0.0\nPurpose-built BioWare conversation editor\n\n"
                              "Conversation graph editing, link conditions, TLK text, scripts, animations, validation, and raw GFF access.");
        }, wxID_ABOUT);
    }

    void buildWindow() {
        auto* panel = new wxPanel(this);
        auto* root = new wxBoxSizer(wxVERTICAL);

        documentTabs_ = new wxAuiNotebook(panel, ID_DocumentTabs, wxDefaultPosition, wxDefaultSize,
                                          wxAUI_NB_TOP | wxAUI_NB_TAB_MOVE | wxAUI_NB_CLOSE_ON_ACTIVE_TAB |
                                              wxAUI_NB_SCROLL_BUTTONS);
        neotabs::configureDocumentTabStrip(documentTabs_);
        root->Add(documentTabs_, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

        auto* header = new wxStaticBoxSizer(wxVERTICAL, panel, "Dialogue");
        auto* fileRow = new wxBoxSizer(wxHORIZONTAL);
        fileRow->Add(new wxStaticText(panel, wxID_ANY, "File:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        filePath_ = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        fileRow->Add(filePath_, 1, wxEXPAND | wxRIGHT, 8);
        fileRow->Add(new wxButton(panel, ID_Open, "Open..."), 0, wxRIGHT, 4);
        fileRow->Add(new wxButton(panel, ID_Save, "Save"), 0, wxRIGHT, 4);
        fileRow->Add(new wxButton(panel, ID_SaveAs, "Save As..."), 0);
        header->Add(fileRow, 0, wxEXPAND | wxALL, 8);

        auto* infoRow = new wxBoxSizer(wxHORIZONTAL);
        typeText_ = new wxStaticText(panel, wxID_ANY, "No DLG loaded");
        infoRow->Add(typeText_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
        statsText_ = new wxStaticText(panel, wxID_ANY, wxEmptyString);
        infoRow->Add(statsText_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 16);
        infoRow->AddStretchSpacer(1);
        infoRow->Add(new wxStaticText(panel, wxID_ANY, "TLK:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        tlkText_ = new wxStaticText(panel, wxID_ANY, "none");
        infoRow->Add(tlkText_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        infoRow->Add(new wxButton(panel, ID_OpenTlk, "Choose..."), 0, wxRIGHT, 4);
        infoRow->Add(new wxButton(panel, ID_ClearTlk, "Clear"), 0);
        header->Add(infoRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
        root->Add(header, 0, wxEXPAND | wxALL, 8);

        workspaceBook_ = new wxNotebook(panel, ID_Workspace);
        buildConversationPage(workspaceBook_);
        buildRawPage(workspaceBook_);
        root->Add(workspaceBook_, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        panel->SetSizer(root);

        Bind(wxEVT_BUTTON, &NeoDLGFrame::onOpen, this, ID_Open);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onSave, this, ID_Save);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onSaveAs, this, ID_SaveAs);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onOpenTlk, this, ID_OpenTlk);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onClearTlk, this, ID_ClearTlk);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onAddStartingEntry, this, ID_AddStartingEntry);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onAddChild, this, ID_AddChild);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onLinkExisting, this, ID_LinkExisting);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onDuplicateNode, this, ID_DuplicateNode);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onRemoveLink, this, ID_RemoveLink);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onDeleteNode, this, ID_DeleteNode);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { moveSelectedLink(-1); }, ID_MoveLinkUp);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { moveSelectedLink(1); }, ID_MoveLinkDown);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onFindNext, this, ID_FindNext);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onApplyNode, this, ID_ApplyNode);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onApplyScripts, this, ID_ApplyScripts);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onApplyPresentation, this, ID_ApplyPresentation);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onApplyLink, this, ID_ApplyLink);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onAnimationAdd, this, ID_AnimationAdd);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onAnimationEdit, this, ID_AnimationEdit);
        Bind(wxEVT_BUTTON, &NeoDLGFrame::onAnimationDelete, this, ID_AnimationDelete);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { moveAnimation(-1); }, ID_AnimationUp);
        Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { moveAnimation(1); }, ID_AnimationDown);
        Bind(wxEVT_GRID_CELL_CHANGED, &NeoDLGFrame::onRawCellChanged, this, ID_RawGrid);
        Bind(wxEVT_TREE_SEL_CHANGED, &NeoDLGFrame::onTreeSelection, this, ID_ConversationTree);
        Bind(wxEVT_TREE_ITEM_ACTIVATED, &NeoDLGFrame::onTreeActivated, this, ID_ConversationTree);
        Bind(wxEVT_TREE_ITEM_MENU, &NeoDLGFrame::onTreeContextMenu, this, ID_ConversationTree);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CHANGED, &NeoDLGFrame::onDocumentTabChanged, this);
        documentTabs_->Bind(wxEVT_AUINOTEBOOK_PAGE_CLOSE, &NeoDLGFrame::onDocumentTabCloseRequested, this);
        workspaceBook_->Bind(wxEVT_NOTEBOOK_PAGE_CHANGED, &NeoDLGFrame::onWorkspacePageChanged, this);
        Bind(wxEVT_CLOSE_WINDOW, &NeoDLGFrame::onClose, this);
    }

    void buildConversationPage(wxNotebook* parent) {
        auto* page = new wxPanel(parent);
        auto* root = new wxBoxSizer(wxVERTICAL);

        auto* toolbar = new wxBoxSizer(wxHORIZONTAL);
        toolbar->Add(new wxButton(page, ID_AddStartingEntry, "Add Start Entry"), 0, wxRIGHT, 4);
        toolbar->Add(new wxButton(page, ID_AddChild, "Add Child"), 0, wxRIGHT, 4);
        toolbar->Add(new wxButton(page, ID_LinkExisting, "Link Existing..."), 0, wxRIGHT, 4);
        toolbar->Add(new wxButton(page, ID_DuplicateNode, "Duplicate"), 0, wxRIGHT, 4);
        toolbar->Add(new wxButton(page, ID_RemoveLink, "Remove Link"), 0, wxRIGHT, 4);
        toolbar->Add(new wxButton(page, ID_DeleteNode, "Delete Node"), 0, wxRIGHT, 4);
        toolbar->Add(new wxButton(page, ID_MoveLinkUp, "Up"), 0, wxRIGHT, 4);
        toolbar->Add(new wxButton(page, ID_MoveLinkDown, "Down"), 0, wxRIGHT, 12);
        toolbar->AddStretchSpacer(1);
        toolbar->Add(new wxStaticText(page, wxID_ANY, "Find:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        findText_ = new wxTextCtrl(page, wxID_ANY);
        findText_->SetMinSize(FromDIP(wxSize(240, -1)));
        toolbar->Add(findText_, 0, wxRIGHT, 4);
        toolbar->Add(new wxButton(page, ID_FindNext, "Next"), 0);
        root->Add(toolbar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

        auto* splitter = new wxSplitterWindow(page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                               wxSP_LIVE_UPDATE | wxSP_3D);
        conversationTree_ = new wxTreeCtrl(splitter, ID_ConversationTree, wxDefaultPosition, wxDefaultSize,
                                            wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE);
        inspectorBook_ = new wxNotebook(splitter, wxID_ANY);
        buildNodePage(inspectorBook_);
        buildScriptsPage(inspectorBook_);
        buildPresentationPage(inspectorBook_);
        buildLinkPage(inspectorBook_);
        buildAnimationsPage(inspectorBook_);
        splitter->SplitVertically(conversationTree_, inspectorBook_, FromDIP(520));
        splitter->SetMinimumPaneSize(FromDIP(280));
        splitter->SetSashGravity(0.38);
        root->Add(splitter, 1, wxEXPAND);

        page->SetSizer(root);
        parent->AddPage(page, "Conversation", true);
    }

    wxScrolledWindow* makeInspectorPage(wxNotebook* book, const wxString& title, wxBoxSizer*& root) {
        auto* page = new wxScrolledWindow(book, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        page->SetScrollRate(0, FromDIP(10));
        root = new wxBoxSizer(wxVERTICAL);
        page->SetSizer(root);
        book->AddPage(page, title, false);
        return page;
    }

    void buildNodePage(wxNotebook* book) {
        wxBoxSizer* root = nullptr;
        wxScrolledWindow* page = makeInspectorPage(book, "Line", root);
        nodeHeader_ = new wxStaticText(page, wxID_ANY, "Select a dialogue node.");
        wxFont bold = nodeHeader_->GetFont();
        bold.SetWeight(wxFONTWEIGHT_BOLD);
        nodeHeader_->SetFont(bold);
        root->Add(nodeHeader_, 0, wxEXPAND | wxALL, 10);

        auto* form = new wxFlexGridSizer(2, 8, 8);
        form->AddGrowableCol(1, 1);
        nodeSpeakerLabel_ = new wxStaticText(page, wxID_ANY, "Speaker:");
        form->Add(nodeSpeakerLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        nodeSpeaker_ = new wxComboBox(page, wxID_ANY);
        form->Add(nodeSpeaker_, 1, wxEXPAND);
        nodeListener_ = addTextField(page, form, "Listener:");
        nodeStrRef_ = addTextField(page, form, "Text StrRef:");
        nodeStringType_ = addTextField(page, form, "Jade string type:");
        nodeLocalText_ = addTextField(page, form, "Local text:", wxTE_MULTILINE, FromDIP(wxSize(-1, 110)));
        nodeResolvedText_ = addTextField(page, form, "Resolved TLK text:", wxTE_MULTILINE | wxTE_READONLY,
                                         FromDIP(wxSize(-1, 110)));
        nodeVo_ = addTextField(page, form, "Voice-over resref/ID:");
        nodeComment_ = addTextField(page, form, "Designer comment:", wxTE_MULTILINE, FromDIP(wxSize(-1, 80)));
        root->Add(form, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        root->Add(new wxButton(page, ID_ApplyNode, "Apply Line Changes"), 0, wxALIGN_RIGHT | wxALL, 10);
    }

    void buildScriptsPage(wxNotebook* book) {
        wxBoxSizer* root = nullptr;
        wxScrolledWindow* page = makeInspectorPage(book, "Scripts / Quest", root);
        auto* form = new wxFlexGridSizer(2, 8, 8);
        form->AddGrowableCol(1, 1);
        nodeScript1_ = addTextField(page, form, "Action script 1:");
        nodeScript2_ = addTextField(page, form, "Action script 2 / entry script:");
        nodeScriptCamEntry_ = addTextField(page, form, "Camera-entry script:");
        nodeScriptCamReplies_ = addTextField(page, form, "Camera-replies script:");
        nodeQuest_ = addTextField(page, form, "Quest tag:");
        nodeQuestEntry_ = addTextField(page, form, "Quest entry:");
        nodePlotIndex_ = addTextField(page, form, "Plot index:");
        nodePlotXp_ = addTextField(page, form, "Plot XP percentage:");
        nodeActionStrA_ = addTextField(page, form, "Action string A:");
        nodeActionStrB_ = addTextField(page, form, "Action string B:");
        root->Add(form, 0, wxEXPAND | wxALL, 10);

        root->Add(new wxStaticText(page, wxID_ANY, "Action integer parameters"), 0, wxLEFT | wxRIGHT | wxTOP, 10);
        actionParamGrid_ = new wxGrid(page, wxID_ANY);
        actionParamGrid_->CreateGrid(5, 2);
        actionParamGrid_->SetColLabelValue(0, "Script 1");
        actionParamGrid_->SetColLabelValue(1, "Script 2");
        for (int row = 0; row < 5; ++row) actionParamGrid_->SetRowLabelValue(row, wxString::Format("Param %d", row + 1));
        actionParamGrid_->SetMinSize(FromDIP(wxSize(420, 190)));
        root->Add(actionParamGrid_, 0, wxEXPAND | wxALL, 10);
        root->Add(new wxButton(page, ID_ApplyScripts, "Apply Script / Quest Changes"), 0, wxALIGN_RIGHT | wxALL, 10);
    }

    void buildPresentationPage(wxNotebook* book) {
        wxBoxSizer* root = nullptr;
        wxScrolledWindow* page = makeInspectorPage(book, "Presentation", root);
        auto* form = new wxFlexGridSizer(2, 8, 8);
        form->AddGrowableCol(1, 1);
        nodeSound_ = addTextField(page, form, "Sound resref:");
        nodeDelay_ = addTextField(page, form, "Delay:");
        nodeWaitFlags_ = addTextField(page, form, "Wait flags:");
        nodeCameraAngle_ = addTextField(page, form, "Camera angle:");
        nodeCameraId_ = addTextField(page, form, "Camera ID:");
        nodeCameraAnimation_ = addTextField(page, form, "Camera animation:");
        nodeEmotion_ = addTextField(page, form, "Emotion:");
        nodeFacialAnim_ = addTextField(page, form, "Facial animation:");
        nodeCamVidEffect_ = addTextField(page, form, "Camera video effect:");
        nodeFadeType_ = addTextField(page, form, "Fade type:");
        nodePostProc_ = addTextField(page, form, "Post-process node:");
        nodeAlienRace_ = addTextField(page, form, "Alien-race node:");
        nodeUnskippable_ = addCheckField(page, form, "Node is unskippable");
        nodeRecordVo_ = addCheckField(page, form, "Record VO");
        nodeRecordNoVoOverride_ = addCheckField(page, form, "No-VO override");
        root->Add(form, 1, wxEXPAND | wxALL, 10);
        root->Add(new wxButton(page, ID_ApplyPresentation, "Apply Presentation Changes"), 0, wxALIGN_RIGHT | wxALL, 10);
    }

    void buildLinkPage(wxNotebook* book) {
        wxBoxSizer* root = nullptr;
        wxScrolledWindow* page = makeInspectorPage(book, "Link / Conditions", root);
        linkHeader_ = new wxStaticText(page, wxID_ANY, "Select a linked node to edit its conditions.");
        wxFont bold = linkHeader_->GetFont();
        bold.SetWeight(wxFONTWEIGHT_BOLD);
        linkHeader_->SetFont(bold);
        root->Add(linkHeader_, 0, wxEXPAND | wxALL, 10);

        auto* form = new wxFlexGridSizer(2, 8, 8);
        form->AddGrowableCol(1, 1);
        linkActive1_ = addTextField(page, form, "Conditional script 1:");
        linkActive2_ = addTextField(page, form, "Conditional script 2:");
        linkLogic_ = addTextField(page, form, "Logic mode:");
        linkParamStrA_ = addTextField(page, form, "Conditional string A:");
        linkParamStrB_ = addTextField(page, form, "Conditional string B:");
        linkComment_ = addTextField(page, form, "Link comment:", wxTE_MULTILINE, FromDIP(wxSize(-1, 70)));
        linkDesignerNumber_ = addTextField(page, form, "Jade designer number:");
        linkNot1_ = addCheckField(page, form, "Negate conditional 1");
        linkNot2_ = addCheckField(page, form, "Negate conditional 2");
        linkIsChild_ = addCheckField(page, form, "IsChild link");
        linkReverseCond_ = addCheckField(page, form, "Reverse Jade condition");
        root->Add(form, 0, wxEXPAND | wxALL, 10);

        root->Add(new wxStaticText(page, wxID_ANY, "Conditional integer parameters"), 0, wxLEFT | wxRIGHT | wxTOP, 10);
        linkParamGrid_ = new wxGrid(page, wxID_ANY);
        linkParamGrid_->CreateGrid(5, 2);
        linkParamGrid_->SetColLabelValue(0, "Conditional 1");
        linkParamGrid_->SetColLabelValue(1, "Conditional 2");
        for (int row = 0; row < 5; ++row) linkParamGrid_->SetRowLabelValue(row, wxString::Format("Param %d", row + 1));
        linkParamGrid_->SetMinSize(FromDIP(wxSize(420, 190)));
        root->Add(linkParamGrid_, 0, wxEXPAND | wxALL, 10);
        root->Add(new wxButton(page, ID_ApplyLink, "Apply Link Conditions"), 0, wxALIGN_RIGHT | wxALL, 10);
    }

    void buildAnimationsPage(wxNotebook* book) {
        auto* page = new wxPanel(book);
        auto* root = new wxBoxSizer(wxVERTICAL);
        animationList_ = new wxListCtrl(page, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                        wxLC_REPORT | wxLC_SINGLE_SEL);
        animationList_->InsertColumn(0, "Participant");
        animationList_->InsertColumn(1, "Animation ID");
        animationList_->InsertColumn(2, "Emotion ID");
        animationList_->SetColumnWidth(0, FromDIP(220));
        animationList_->SetColumnWidth(1, FromDIP(130));
        animationList_->SetColumnWidth(2, FromDIP(130));
        root->Add(animationList_, 1, wxEXPAND | wxALL, 10);
        auto* buttons = new wxBoxSizer(wxHORIZONTAL);
        buttons->Add(new wxButton(page, ID_AnimationAdd, "Add..."), 0, wxRIGHT, 4);
        buttons->Add(new wxButton(page, ID_AnimationEdit, "Edit..."), 0, wxRIGHT, 4);
        buttons->Add(new wxButton(page, ID_AnimationDelete, "Delete"), 0, wxRIGHT, 12);
        buttons->Add(new wxButton(page, ID_AnimationUp, "Up"), 0, wxRIGHT, 4);
        buttons->Add(new wxButton(page, ID_AnimationDown, "Down"), 0);
        root->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        page->SetSizer(root);
        book->AddPage(page, "Animations", false);
    }

    void buildRawPage(wxNotebook* parent) {
        auto* page = new wxPanel(parent);
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* filterRow = new wxBoxSizer(wxHORIZONTAL);
        filterRow->Add(new wxStaticText(page, wxID_ANY, "Filter raw fields:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        rawFilter_ = new wxTextCtrl(page, wxID_ANY);
        filterRow->Add(rawFilter_, 1);
        root->Add(filterRow, 0, wxEXPAND | wxBOTTOM, 6);

        rawGrid_ = new wxGrid(page, ID_RawGrid);
        rawGrid_->CreateGrid(0, 4);
        rawGrid_->SetColLabelValue(0, "Path");
        rawGrid_->SetColLabelValue(1, "Type");
        rawGrid_->SetColLabelValue(2, "Value");
        rawGrid_->SetColLabelValue(3, "Resolved");
        rawGrid_->SetColSize(0, FromDIP(420));
        rawGrid_->SetColSize(1, FromDIP(150));
        rawGrid_->SetColSize(2, FromDIP(380));
        rawGrid_->SetColSize(3, FromDIP(420));
        rawGrid_->EnableEditing(true);
        for (int column : {0, 1, 3}) {
            auto* attr = new wxGridCellAttr;
            attr->SetReadOnly(true);
            rawGrid_->SetColAttr(column, attr);
        }
        root->Add(rawGrid_, 1, wxEXPAND);
        page->SetSizer(root);
        parent->AddPage(page, "Raw GFF", false);
        rawFilter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refreshRawGrid(); });
    }

    void createDocumentTab(bool select) {
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

    void selectDocumentTab(std::size_t index) {
        if (index >= documents_.size()) return;
        tabSwitchInProgress_ = true;
        neotabs::changeSelectionToPage(documentTabs_, documents_[index].tabPage);
        tabSwitchInProgress_ = false;
        activeDocumentIndex_ = index;
        setWorkspacePage(activeDocument().workspacePage, false);
        refreshAll();
    }

    bool confirmCloseDocument(std::size_t index) {
        if (index >= documents_.size() || !tabDirty(documents_[index])) return true;
        return wxui::confirm(this, "Close tab", neotabs::closePromptText(tabDisplayName(documents_[index])));
    }

    bool closeDocument(std::size_t index) {
        if (index >= documents_.size() || !confirmCloseDocument(index)) return false;
        wxWindow* page = documents_[index].tabPage;
        tabSwitchInProgress_ = true;
        const bool deleted = neotabs::deleteTabPage(documentTabs_, page);
        tabSwitchInProgress_ = false;
        if (!deleted) return false;
        documents_.erase(documents_.begin() + static_cast<std::ptrdiff_t>(index));
        if (documents_.empty()) {
            activeDocumentIndex_ = neotabs::npos;
            createDocumentTab(true);
            refreshAll();
            return true;
        }
        std::size_t selected = neotabs::findDocumentIndexForPage(documents_, neotabs::currentPage(documentTabs_));
        if (selected == neotabs::npos) selected = std::min(index, documents_.size() - 1);
        selectDocumentTab(selected);
        return true;
    }

    void updateTabTitle() {
        if (!hasActiveDocument()) return;
        neotabs::setTabLabel(documentTabs_, activeDocument().tabPage,
                             tabDisplayName(activeDocument()), tabDirty(activeDocument()));
    }

    void newDocument(DlgFlavor flavor) {
        try {
            if (!activeTabReusable()) createDocumentTab(true);
            DlgDocument document(model());
            document.create(flavor);
            activeDocument().untitledName = "Untitled " + flavorName(flavor) + " DLG";
            activeDocument().undo.clear();
            activeDocument().redo.clear();
            activeDocument().selectedNode.reset();
            activeDocument().selectedLink.reset();
            setWorkspacePage(0);
            refreshAll();
        } catch (const std::exception& ex) {
            wxui::showError(this, ex);
        }
    }

    void chooseAndOpenDlg(const std::filesystem::path& initialDirectory = {}) {
        const auto path = wxui::chooseOpenFile(this, "Open DLG", kDlgWildcard, initialDirectory);
        if (path) openModelPath(*path);
    }

    bool openModelPath(const std::filesystem::path& path) {
        if (path.empty()) return false;
        ensureTabForOpen();

        auto candidate = std::make_unique<GffModel>();
        candidate->load(path);
        if (candidate->fileType() != "DLG ") {
            throw std::invalid_argument("The selected file is not a DLG resource.");
        }

        activeDocument().model = std::move(candidate);
        activeDocument().undo.clear();
        activeDocument().redo.clear();
        activeDocument().selectedNode.reset();
        activeDocument().selectedLink.reset();
        tryLoadResolvedTlkForPath(path);
        rememberRecentFile(path);
        neogames::resolver().inferFromOpenedPath(path);
        const bool semantic = dialogue().semanticallyEditable();
        setWorkspacePage(semantic ? 0 : 1);
        refreshAll();
        return true;
    }

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

    bool save(bool saveAs) {
        if (!hasActiveDocument() || !model().loaded()) return false;
        try {
            std::filesystem::path target = model().filename();
            if (saveAs || target.empty()) {
                std::string name = target.empty() ? "new.dlg" : neosettings::pathToUtf8(target.filename());
                const auto chosen = wxui::chooseSaveFile(this, "Save DLG", kDlgWildcard, name);
                if (!chosen) return false;
                target = ensureDlgExtension(*chosen);
            }
            model().save(target);
            rememberRecentFile(target);
            neogames::resolver().inferFromOpenedPath(target);
            updateTabTitle();
            refreshHeader();
            return true;
        } catch (const std::exception& ex) {
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
        const auto path = settings_.lastTlkPath();
        if (!path || path->empty()) return;
        try { model().loadTlk(*path); }
        catch (const std::exception&) { settings_.clearLastTlkPath(); }
    }

    void onOpenTlk(wxCommandEvent&) {
        const auto chosen = wxui::chooseOpenFile(this, "Open TLK", kTlkWildcard);
        if (!chosen) return;
        try {
            model().loadTlk(*chosen);
            settings_.setLastTlkPath(*chosen);
            refreshAll();
        } catch (const std::exception& ex) { wxui::showError(this, ex); }
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
            } catch (...) {
                if (!snapshot.empty()) {
                    try {
                        model().importXml(snapshot);
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
            model().importXml(snapshot.xml);
            refreshAll();
        } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void onRedo(wxCommandEvent&) {
        if (!hasActiveDocument() || activeDocument().redo.empty()) return;
        try {
            UndoSnapshot snapshot = std::move(activeDocument().redo.back());
            activeDocument().redo.pop_back();
            activeDocument().undo.push_back({snapshot.description, model().toXml()});
            model().importXml(snapshot.xml);
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
        wxArrayString choices;
        std::vector<DlgNodeRef> refs;
        for (std::size_t i = 0; i < document.nodeCount(kind); ++i) {
            DlgNodeRef ref{kind, i};
            refs.push_back(ref);
            choices.Add(wxui::toWx(document.nodeLabel(ref, 120)));
        }
        if (refs.empty()) {
            wxui::showMessage(this, "Link Existing Node", "There are no existing nodes of the required type.");
            return std::nullopt;
        }
        wxSingleChoiceDialog dialog(this, "Choose the node to link:", title, choices);
        wxui::applyTheme(&dialog, darkMode_);
        if (dialog.ShowModal() != wxID_OK) return std::nullopt;
        const int selection = dialog.GetSelection();
        if (selection < 0 || static_cast<std::size_t>(selection) >= refs.size()) return std::nullopt;
        return refs[static_cast<std::size_t>(selection)];
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
        SetStatusText(wxString::Format("Match %zu of %zu", searchIndex_ + 1, searchResults_.size()), 1);
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
                wxString speakerValue = nodeSpeaker_->GetValue();
                const int separator = speakerValue.Find(':');
                if (separator != wxNOT_FOUND) speakerValue = speakerValue.Left(static_cast<std::size_t>(separator));
                setOptionalNodeField(document, ref, "SpeakerIndex", FIELD_TYPE_INT, speakerValue, true);
                setOptionalNodeField(document, ref, "VoiceOver", FIELD_TYPE_CEXOSTRING, nodeVo_->GetValue(), false);
            } else {
                if (ref.kind == DlgNodeKind::Entry || document.hasNodeField(ref, "Speaker"))
                    setOptionalNodeField(document, ref, "Speaker", FIELD_TYPE_CEXOSTRING, nodeSpeaker_->GetValue(), true);
                setOptionalNodeField(document, ref, "Listener", FIELD_TYPE_CEXOSTRING, nodeListener_->GetValue(), false);
                setOptionalNodeField(document, ref, "VO_ResRef", FIELD_TYPE_RESREF, nodeVo_->GetValue(), false);
                setOptionalNodeField(document, ref, "Comment", FIELD_TYPE_CEXOSTRING, nodeComment_->GetValue(), false);
            }
        });
    }

    void onApplyScripts(wxCommandEvent&) {
        if (!activeDocument().selectedNode) return;
        const DlgNodeRef ref = *activeDocument().selectedNode;
        mutate("Edit dialogue scripts", [this, ref]() {
            DlgDocument document = dialogue();
            const bool jade = document.dialect() == DlgDialect::JadeEmpire;
            setOptionalNodeField(document, ref, "Script", FIELD_TYPE_RESREF, nodeScript1_->GetValue(), false);
            setOptionalNodeField(document, ref, jade ? "ScriptEntry" : "Script2", FIELD_TYPE_RESREF, nodeScript2_->GetValue(), false);
            setOptionalNodeField(document, ref, "ScriptCamEntry", FIELD_TYPE_RESREF, nodeScriptCamEntry_->GetValue(), false);
            setOptionalNodeField(document, ref, "ScriptCamReplies", FIELD_TYPE_RESREF, nodeScriptCamReplies_->GetValue(), false);
            setOptionalNodeField(document, ref, "Quest", FIELD_TYPE_CEXOSTRING, nodeQuest_->GetValue(), false);
            setOptionalNodeField(document, ref, "QuestEntry", FIELD_TYPE_DWORD, nodeQuestEntry_->GetValue(), false);
            setOptionalNodeField(document, ref, "PlotIndex", FIELD_TYPE_INT, nodePlotIndex_->GetValue(), false);
            setOptionalNodeField(document, ref, "PlotXPPercentage", FIELD_TYPE_FLOAT, nodePlotXp_->GetValue(), false);
            setOptionalNodeField(document, ref, "ActionParamStrA", FIELD_TYPE_CEXOSTRING, nodeActionStrA_->GetValue(), false);
            setOptionalNodeField(document, ref, "ActionParamStrB", FIELD_TYPE_CEXOSTRING, nodeActionStrB_->GetValue(), false);
            for (int i = 0; i < 5; ++i) {
                setOptionalNodeField(document, ref, "ActionParam" + std::to_string(i + 1), FIELD_TYPE_INT,
                                     actionParamGrid_->GetCellValue(i, 0), false);
                setOptionalNodeField(document, ref, "ActionParam" + std::to_string(i + 1) + "b", FIELD_TYPE_INT,
                                     actionParamGrid_->GetCellValue(i, 1), false);
            }
        });
    }

    void onApplyPresentation(wxCommandEvent&) {
        if (!activeDocument().selectedNode) return;
        const DlgNodeRef ref = *activeDocument().selectedNode;
        mutate("Edit dialogue presentation", [this, ref]() {
            DlgDocument document = dialogue();
            setOptionalNodeField(document, ref, "Sound", FIELD_TYPE_RESREF, nodeSound_->GetValue(), false);
            setOptionalNodeField(document, ref, "Delay", FIELD_TYPE_DWORD, nodeDelay_->GetValue(), false);
            setOptionalNodeField(document, ref, "WaitFlags", FIELD_TYPE_DWORD, nodeWaitFlags_->GetValue(), false);
            setOptionalNodeField(document, ref, "CameraAngle", FIELD_TYPE_DWORD, nodeCameraAngle_->GetValue(), false);
            setOptionalNodeField(document, ref, "CameraID", FIELD_TYPE_INT, nodeCameraId_->GetValue(), false);
            setOptionalNodeField(document, ref, "CameraAnimation", FIELD_TYPE_WORD, nodeCameraAnimation_->GetValue(), false);
            setOptionalNodeField(document, ref, "Emotion", FIELD_TYPE_INT, nodeEmotion_->GetValue(), false);
            setOptionalNodeField(document, ref, "FacialAnim", FIELD_TYPE_INT, nodeFacialAnim_->GetValue(), false);
            setOptionalNodeField(document, ref, "CamVidEffect", FIELD_TYPE_INT, nodeCamVidEffect_->GetValue(), false);
            setOptionalNodeField(document, ref, "FadeType", FIELD_TYPE_BYTE, nodeFadeType_->GetValue(), false);
            setOptionalNodeField(document, ref, "PostProcNode", FIELD_TYPE_INT, nodePostProc_->GetValue(), false);
            setOptionalNodeField(document, ref, "AlienRaceNode", FIELD_TYPE_INT, nodeAlienRace_->GetValue(), false);
            setOptionalNodeField(document, ref, "NodeUnskippable", FIELD_TYPE_INT,
                                 wxui::toWx(boolText(nodeUnskippable_)), document.hasNodeField(ref, "NodeUnskippable"));
            setOptionalNodeField(document, ref, "RecordVO", FIELD_TYPE_INT,
                                 wxui::toWx(boolText(nodeRecordVo_)), document.hasNodeField(ref, "RecordVO"));
            setOptionalNodeField(document, ref, "RecordNoVOOverri", FIELD_TYPE_INT,
                                 wxui::toWx(boolText(nodeRecordNoVoOverride_)), document.hasNodeField(ref, "RecordNoVOOverri"));
        });
    }

    void onApplyLink(wxCommandEvent&) {
        if (!activeDocument().selectedLink) return;
        const DlgLinkRef ref = *activeDocument().selectedLink;
        mutate("Edit dialogue link conditions", [this, ref]() {
            DlgDocument document = dialogue();
            const bool jade = document.dialect() == DlgDialect::JadeEmpire;
            setOptionalLinkField(document, ref, "Active", FIELD_TYPE_RESREF, linkActive1_->GetValue(), false);
            setOptionalLinkField(document, ref, "Active2", FIELD_TYPE_RESREF, linkActive2_->GetValue(), false);
            setOptionalLinkField(document, ref, "Logic", FIELD_TYPE_INT, linkLogic_->GetValue(), false);
            setOptionalLinkField(document, ref, "ParamStrA", FIELD_TYPE_CEXOSTRING, linkParamStrA_->GetValue(), false);
            setOptionalLinkField(document, ref, "ParamStrB", FIELD_TYPE_CEXOSTRING, linkParamStrB_->GetValue(), false);
            setOptionalLinkField(document, ref, "LinkComment", FIELD_TYPE_CEXOSTRING, linkComment_->GetValue(), false);
            setOptionalLinkField(document, ref, "DesignerNumber", FIELD_TYPE_INT, linkDesignerNumber_->GetValue(), false);
            if (!jade) {
                setOptionalLinkField(document, ref, "Not", FIELD_TYPE_BYTE, wxui::toWx(boolText(linkNot1_)), document.hasLinkField(ref, "Not"));
                setOptionalLinkField(document, ref, "Not2", FIELD_TYPE_BYTE, wxui::toWx(boolText(linkNot2_)), document.hasLinkField(ref, "Not2"));
                setOptionalLinkField(document, ref, "IsChild", FIELD_TYPE_BYTE, wxui::toWx(boolText(linkIsChild_)), true);
                for (int i = 0; i < 5; ++i) {
                    setOptionalLinkField(document, ref, "Param" + std::to_string(i + 1), FIELD_TYPE_INT,
                                         linkParamGrid_->GetCellValue(i, 0), false);
                    setOptionalLinkField(document, ref, "Param" + std::to_string(i + 1) + "b", FIELD_TYPE_INT,
                                         linkParamGrid_->GetCellValue(i, 1), false);
                }
            } else {
                setOptionalLinkField(document, ref, "ReverseCond", FIELD_TYPE_BYTE,
                                     wxui::toWx(boolText(linkReverseCond_)), true);
            }
        });
    }

    void setOptionalNodeField(DlgDocument& document,
                              DlgNodeRef ref,
                              const std::string& label,
                              std::uint32_t type,
                              const wxString& value,
                              bool createEvenIfEmpty) {
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
        AnimationEditDialog dialog(this, dialogue().dialect() == DlgDialect::JadeEmpire, {});
        wxui::applyTheme(&dialog, darkMode_);
        if (dialog.ShowModal() != wxID_OK) return;
        const DlgNodeRef ref = *activeDocument().selectedNode;
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
        AnimationEditDialog dialog(this, dialogue().dialect() == DlgDialect::JadeEmpire,
                                   animationValues_[static_cast<std::size_t>(row)]);
        wxui::applyTheme(&dialog, darkMode_);
        if (dialog.ShowModal() != wxID_OK) return;
        const DlgNodeRef ref = *activeDocument().selectedNode;
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
        const long row = selectedAnimationRow();
        const long target = row + delta;
        if (row < 0 || target < 0 || static_cast<std::size_t>(target) >= animationValues_.size()) return;
        const DlgNodeRef ref = *activeDocument().selectedNode;
        mutate("Move dialogue animation", [this, ref, row, target]() {
            auto values = dialogue().animations(ref);
            std::swap(values[static_cast<std::size_t>(row)], values[static_cast<std::size_t>(target)]);
            dialogue().replaceAnimations(ref, values);
        });
        animationList_->SetItemState(target, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }

    void onImport(bool json) {
        const auto chosen = wxui::chooseOpenFile(this, json ? "Import JSON" : "Import XML",
                                                  json ? kJsonWildcard : kXmlWildcard);
        if (!chosen) return;
        mutate(json ? "Import JSON" : "Import XML", [this, chosen, json]() {
            const std::string source = readTextFile(*chosen);
            model().importXml(json ? gffJsonToXml(source) : source);
            activeDocument().selectedNode.reset();
            activeDocument().selectedLink.reset();
        });
    }

    void onExport(bool json) {
        if (!model().loaded() || model().gff().isGff4()) {
            wxui::showMessage(this, "Export", "Semantic XML/JSON export is available for classic GFF V3 DLG files.");
            return;
        }
        const std::string stem = model().filename().empty() ? "dialog" : neosettings::pathToUtf8(model().filename().stem());
        const auto chosen = wxui::chooseSaveFile(this, json ? "Export JSON" : "Export XML",
                                                  json ? kJsonWildcard : kXmlWildcard,
                                                  stem + (json ? ".json" : ".xml"));
        if (!chosen) return;
        try {
            const std::string xml = model().toXml();
            writeTextFile(*chosen, json ? gffXmlToJson(xml) : xml);
        } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void onExportPatcherPackage(wxCommandEvent&) {
        if (!model().loaded() || model().gff().isGff4()) return;
        try {
            const auto originalPath = wxui::chooseOpenFile(this, "Select clean/unmodified DLG", kDlgWildcard);
            if (!originalPath) return;
            std::string defaultName = model().filename().empty() ? "modified.dlg" : neosettings::pathToUtf8(model().filename().filename());
            const auto patchName = wxui::promptText(this, "Patch Target Filename",
                                                     "DLG filename to patch in the user's install:", defaultName);
            if (!patchName || patchName->empty()) return;
            const auto outputDir = wxui::chooseDirectory(this, "Choose tslpatchdata package folder");
            if (!outputDir) return;
            GffModel original;
            original.load(*originalPath);
            auto project = neodlg::patcher::diffDlgPatcher(original.gff(), model().gff(), *patchName, true, *originalPath);
            neotsl::throwIfUnsupported(project);
            neotsl::writePackage(project, *outputDir, true);
            wxui::showMessage(this, "TSL/HoloPatcher Package",
                              "Wrote changes.ini and the clean baseline DLG to:\n" + neosettings::pathToUtf8(*outputDir));
        } catch (const std::exception& ex) { wxui::showError(this, ex); }
    }

    void setWorkspacePage(int page, bool refresh = true) {
        if (!workspaceBook_ || page < 0 || page >= static_cast<int>(workspaceBook_->GetPageCount())) return;
        if (hasActiveDocument()) activeDocument().workspacePage = page;
        if (workspaceBook_->GetSelection() != page) workspaceBook_->ChangeSelection(page);
        if (conversationViewItem_) conversationViewItem_->Check(page == 0);
        if (rawViewItem_) rawViewItem_->Check(page == 1);
        if (refresh) {
            if (page == 0) refreshConversationTree();
            else refreshRawGrid();
        }
    }

    void onWorkspacePageChanged(wxBookCtrlEvent& event) {
        if (hasActiveDocument()) activeDocument().workspacePage = event.GetSelection();
        if (event.GetSelection() == 1) refreshRawGrid();
        if (conversationViewItem_) conversationViewItem_->Check(event.GetSelection() == 0);
        if (rawViewItem_) rawViewItem_->Check(event.GetSelection() == 1);
        event.Skip();
    }

    void refreshAll() {
        if (!hasActiveDocument()) return;
        refreshHeader();
        updateTabTitle();
        refreshUndoMenu();
        if (activeDocument().workspacePage == 0) refreshConversationTree();
        else refreshRawGrid();
        refreshInspector();
    }

    void refreshHeader() {
        if (!hasActiveDocument() || !model().loaded()) {
            filePath_->ChangeValue("");
            typeText_->SetLabel("No DLG loaded");
            statsText_->SetLabel("");
            tlkText_->SetLabel("none");
            SetStatusText("Ready", 0);
            SetStatusText("", 1);
            return;
        }
        filePath_->ChangeValue(neosettings::pathToWx(model().filename()));
        DlgDocument document = dialogue();
        const std::string type = trimHeader(model().fileType()) + " " + trimHeader(model().version()) +
                                 " - " + dialectName(document.dialect());
        typeText_->SetLabel(wxui::toWx(type));
        if (document.semanticallyEditable()) {
            const DlgStatistics stats = document.statistics();
            statsText_->SetLabel(wxui::toWx(std::to_string(stats.entries) + " entries, " +
                                                     std::to_string(stats.replies) + " replies, " +
                                                     std::to_string(stats.totalLinks) + " links"));
        } else {
            statsText_->SetLabel("Use Raw GFF view");
        }
        tlkText_->SetLabel(model().tlk().loaded() ? neosettings::pathToWx(model().tlk().filename()) : wxString("none"));
        SetStatusText(model().dirty() ? "Modified" : "Saved", 0);
        SetStatusText(activeDocument().tlkAutoLoadWarning.empty() ? wxString{} : wxui::toWx(activeDocument().tlkAutoLoadWarning), 1);
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

    void refreshConversationTree() {
        treeRefreshInProgress_ = true;
        conversationTree_->Freeze();
        conversationTree_->DeleteAllItems();
        canonicalTreeItems_.clear();

        const wxTreeItemId rootItem = conversationTree_->AddRoot("Conversation",
            -1, -1, new ConversationTreeData(ConversationTreeKind::ConversationRoot));
        if (!model().loaded()) {
            conversationTree_->AppendItem(rootItem, "Open or create a DLG to begin.");
            conversationTree_->Expand(rootItem);
            conversationTree_->Thaw();
            treeRefreshInProgress_ = false;
            return;
        }

        DlgDocument document = dialogue();
        if (!document.semanticallyEditable()) {
            conversationTree_->AppendItem(rootItem,
                "This DLG schema is not supported by the conversation editor. Use Raw GFF view.");
            conversationTree_->Expand(rootItem);
            conversationTree_->Thaw();
            treeRefreshInProgress_ = false;
            return;
        }

        const wxTreeItemId starts = conversationTree_->AppendItem(rootItem, "Starting Nodes",
            -1, -1, new ConversationTreeData(ConversationTreeKind::Group));
        std::vector<DlgNodeRef> ancestry;
        for (DlgLinkRef ref : document.startingLinks()) appendLinkBranch(starts, ref, ancestry, 0);

        const auto unreachable = document.unreachableNodes();
        if (!unreachable.empty()) {
            const wxTreeItemId orphanGroup = conversationTree_->AppendItem(
                rootItem, wxString::Format("Unreachable Nodes (%zu)", unreachable.size()),
                -1, -1, new ConversationTreeData(ConversationTreeKind::Group));
            for (DlgNodeRef ref : unreachable) {
                if (canonicalTreeItems_.count(ref)) continue;
                const wxTreeItemId item = conversationTree_->AppendItem(
                    orphanGroup, wxui::toWx("[unreachable] " + document.nodeLabel(ref)),
                    -1, -1, new ConversationTreeData(ConversationTreeKind::Node, ref, std::nullopt, false));
                styleTreeNode(item, ref, true);
                canonicalTreeItems_[ref] = item;
                ancestry.clear();
                ancestry.push_back(ref);
                for (DlgLinkRef child : document.outgoingLinks(ref)) appendLinkBranch(item, child, ancestry, 1);
            }
        }

        conversationTree_->Expand(rootItem);
        conversationTree_->Expand(starts);
        conversationTree_->Thaw();
        treeRefreshInProgress_ = false;

        if (activeDocument().selectedNode) selectSemanticNode(*activeDocument().selectedNode, activeDocument().selectedLink, false);
    }

    void appendLinkBranch(const wxTreeItemId& parent,
                          DlgLinkRef linkRef,
                          std::vector<DlgNodeRef>& ancestry,
                          int depth) {
        DlgDocument document = dialogue();
        const auto target = document.targetOf(linkRef);
        if (!target) {
            const wxTreeItemId invalid = conversationTree_->AppendItem(
                parent, "[invalid link]", -1, -1,
                new ConversationTreeData(ConversationTreeKind::InvalidLink, std::nullopt, linkRef));
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

        const wxTreeItemId item = conversationTree_->AppendItem(
            parent, wxui::toWx(label), -1, -1,
            new ConversationTreeData(ConversationTreeKind::Node, *target, linkRef, cycle || reference));
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
        const bool valid = hasActiveDocument() && model().loaded() && dialogue().semanticallyEditable() && activeDocument().selectedNode;
        enableInspector(valid);
        if (!valid) {
            nodeHeader_->SetLabel("Select a dialogue node.");
            linkHeader_->SetLabel("Select a linked node to edit its conditions.");
            clearInspectorControls();
            return;
        }

        const DlgDocument document = dialogue();
        const DlgNodeRef ref = *activeDocument().selectedNode;
        const bool jade = document.dialect() == DlgDialect::JadeEmpire;
        nodeHeader_->SetLabel(wxui::toWx(document.nodeLabel(ref, 200)));
        nodeSpeakerLabel_->SetLabel(jade ? "Speaker:" : "Speaker:");
        nodeSpeaker_->Clear();
        if (jade) {
            const auto tags = document.speakerTags();
            for (std::size_t i = 0; i < tags.size(); ++i) {
                nodeSpeaker_->Append(wxString::Format("%zu: ", i) + wxui::toWx(tags[i]));
            }
            const std::string selected = document.nodeField(ref, "SpeakerIndex");
            try {
                const std::int32_t index = neogff::ParseInt32Decimal(selected.empty() ? "-1" : selected);
                if (index >= 0 && static_cast<std::size_t>(index) < tags.size()) {
                    nodeSpeaker_->SetSelection(index);
                } else {
                    nodeSpeaker_->ChangeValue(wxui::toWx(selected));
                }
            } catch (const std::exception&) {
                nodeSpeaker_->ChangeValue(wxui::toWx(selected));
            }
        } else {
            nodeSpeaker_->ChangeValue(wxui::toWx(document.nodeField(ref, "Speaker")));
        }
        nodeSpeaker_->Enable(jade || ref.kind == DlgNodeKind::Entry || document.hasNodeField(ref, "Speaker"));
        nodeListener_->ChangeValue(wxui::toWx(document.nodeField(ref, "Listener")));
        nodeListener_->Enable(!jade);
        const DlgTextValue value = document.text(ref);
        nodeStrRef_->ChangeValue(value.strref == 0xFFFFFFFFu ? wxString("-1") : wxString::Format("%u", value.strref));
        nodeStringType_->ChangeValue(wxString::Format("%u", value.stringType));
        nodeStringType_->Enable(jade);
        nodeLocalText_->ChangeValue(wxui::toWx(value.localText));
        nodeLocalText_->Enable(!jade);
        nodeResolvedText_->ChangeValue(wxui::toWx(value.resolvedText));
        nodeVo_->ChangeValue(wxui::toWx(document.nodeField(ref, jade ? "VoiceOver" : "VO_ResRef")));
        nodeComment_->ChangeValue(wxui::toWx(document.nodeField(ref, "Comment")));
        nodeComment_->Enable(!jade);

        loadField(nodeScript1_, document.nodeField(ref, "Script"));
        loadField(nodeScript2_, document.nodeField(ref, jade ? "ScriptEntry" : "Script2"));
        loadField(nodeScriptCamEntry_, document.nodeField(ref, "ScriptCamEntry"));
        loadField(nodeScriptCamReplies_, document.nodeField(ref, "ScriptCamReplies"));
        nodeScriptCamEntry_->Enable(jade || document.hasNodeField(ref, "ScriptCamEntry"));
        nodeScriptCamReplies_->Enable(jade || document.hasNodeField(ref, "ScriptCamReplies"));
        loadField(nodeQuest_, document.nodeField(ref, "Quest"));
        loadField(nodeQuestEntry_, document.nodeField(ref, "QuestEntry"));
        loadField(nodePlotIndex_, document.nodeField(ref, "PlotIndex"));
        loadField(nodePlotXp_, document.nodeField(ref, "PlotXPPercentage"));
        loadField(nodeActionStrA_, document.nodeField(ref, "ActionParamStrA"));
        loadField(nodeActionStrB_, document.nodeField(ref, "ActionParamStrB"));
        for (int i = 0; i < 5; ++i) {
            actionParamGrid_->SetCellValue(i, 0, wxui::toWx(document.nodeField(ref, "ActionParam" + std::to_string(i + 1))));
            actionParamGrid_->SetCellValue(i, 1, wxui::toWx(document.nodeField(ref, "ActionParam" + std::to_string(i + 1) + "b")));
        }
        actionParamGrid_->Enable(!jade);

        loadField(nodeSound_, document.nodeField(ref, "Sound"));
        loadField(nodeDelay_, document.nodeField(ref, "Delay"));
        loadField(nodeWaitFlags_, document.nodeField(ref, "WaitFlags"));
        loadField(nodeCameraAngle_, document.nodeField(ref, "CameraAngle"));
        loadField(nodeCameraId_, document.nodeField(ref, "CameraID"));
        loadField(nodeCameraAnimation_, document.nodeField(ref, "CameraAnimation"));
        loadField(nodeEmotion_, document.nodeField(ref, "Emotion"));
        loadField(nodeFacialAnim_, document.nodeField(ref, "FacialAnim"));
        loadField(nodeCamVidEffect_, document.nodeField(ref, "CamVidEffect"));
        loadField(nodeFadeType_, document.nodeField(ref, "FadeType"));
        loadField(nodePostProc_, document.nodeField(ref, "PostProcNode"));
        loadField(nodeAlienRace_, document.nodeField(ref, "AlienRaceNode"));
        setBoolControl(nodeUnskippable_, document.nodeField(ref, "NodeUnskippable"));
        setBoolControl(nodeRecordVo_, document.nodeField(ref, "RecordVO"));
        setBoolControl(nodeRecordNoVoOverride_, document.nodeField(ref, "RecordNoVOOverri"));

        const bool hasLink = activeDocument().selectedLink && document.link(*activeDocument().selectedLink);
        enableLinkInspector(hasLink);
        if (hasLink) {
            const DlgLinkRef linkRef = *activeDocument().selectedLink;
            linkHeader_->SetLabel(wxui::toWx("Link to " + document.nodeKindName(ref.kind) + " " + std::to_string(ref.index)));
            loadField(linkActive1_, document.linkField(linkRef, "Active"));
            loadField(linkActive2_, document.linkField(linkRef, "Active2"));
            loadField(linkLogic_, document.linkField(linkRef, "Logic"));
            loadField(linkParamStrA_, document.linkField(linkRef, "ParamStrA"));
            loadField(linkParamStrB_, document.linkField(linkRef, "ParamStrB"));
            loadField(linkComment_, document.linkField(linkRef, "LinkComment"));
            loadField(linkDesignerNumber_, document.linkField(linkRef, "DesignerNumber"));
            setBoolControl(linkNot1_, document.linkField(linkRef, "Not"));
            setBoolControl(linkNot2_, document.linkField(linkRef, "Not2"));
            setBoolControl(linkIsChild_, document.linkField(linkRef, "IsChild"));
            setBoolControl(linkReverseCond_, document.linkField(linkRef, "ReverseCond"));
            for (int i = 0; i < 5; ++i) {
                linkParamGrid_->SetCellValue(i, 0, wxui::toWx(document.linkField(linkRef, "Param" + std::to_string(i + 1))));
                linkParamGrid_->SetCellValue(i, 1, wxui::toWx(document.linkField(linkRef, "Param" + std::to_string(i + 1) + "b")));
            }
            linkActive2_->Enable(!jade);
            linkLogic_->Enable(!jade);
            linkParamStrA_->Enable(!jade);
            linkParamStrB_->Enable(!jade);
            linkComment_->Enable(!jade || document.hasLinkField(linkRef, "LinkComment"));
            linkDesignerNumber_->Enable(jade);
            linkNot1_->Enable(!jade);
            linkNot2_->Enable(!jade);
            linkIsChild_->Enable(!jade);
            linkReverseCond_->Enable(jade);
            linkParamGrid_->Enable(!jade);
        } else {
            linkHeader_->SetLabel("This node is not selected through a specific link.");
            clearLinkControls();
        }
        refreshAnimationList();
    }

    void loadField(wxTextCtrl* control, const std::string& value) {
        if (control) control->ChangeValue(wxui::toWx(value));
    }

    void enableInspector(bool enabled) {
        for (wxWindow* window : nodeInspectorWindows()) if (window) window->Enable(enabled);
        inspectorBook_->Enable(enabled);
    }

    void enableLinkInspector(bool enabled) {
        for (wxWindow* window : linkInspectorWindows()) if (window) window->Enable(enabled);
    }

    std::vector<wxWindow*> nodeInspectorWindows() const {
        return {nodeSpeaker_, nodeListener_, nodeStrRef_, nodeStringType_, nodeLocalText_, nodeResolvedText_, nodeVo_, nodeComment_,
                nodeScript1_, nodeScript2_, nodeScriptCamEntry_, nodeScriptCamReplies_, nodeQuest_, nodeQuestEntry_, nodePlotIndex_, nodePlotXp_,
                nodeActionStrA_, nodeActionStrB_, actionParamGrid_, nodeSound_, nodeDelay_, nodeWaitFlags_, nodeCameraAngle_, nodeCameraId_,
                nodeCameraAnimation_, nodeEmotion_, nodeFacialAnim_, nodeCamVidEffect_, nodeFadeType_, nodePostProc_, nodeAlienRace_,
                nodeUnskippable_, nodeRecordVo_, nodeRecordNoVoOverride_, animationList_};
    }

    std::vector<wxWindow*> linkInspectorWindows() const {
        return {linkActive1_, linkActive2_, linkLogic_, linkParamStrA_, linkParamStrB_, linkComment_, linkDesignerNumber_,
                linkNot1_, linkNot2_, linkIsChild_, linkReverseCond_, linkParamGrid_};
    }

    void clearInspectorControls() {
        if (nodeSpeaker_) {
            nodeSpeaker_->Clear();
            nodeSpeaker_->ChangeValue("");
        }
        for (wxTextCtrl* control : {nodeListener_, nodeStrRef_, nodeStringType_, nodeLocalText_, nodeResolvedText_, nodeVo_, nodeComment_,
                                    nodeScript1_, nodeScript2_, nodeScriptCamEntry_, nodeScriptCamReplies_, nodeQuest_, nodeQuestEntry_, nodePlotIndex_,
                                    nodePlotXp_, nodeActionStrA_, nodeActionStrB_, nodeSound_, nodeDelay_, nodeWaitFlags_, nodeCameraAngle_, nodeCameraId_,
                                    nodeCameraAnimation_, nodeEmotion_, nodeFacialAnim_, nodeCamVidEffect_, nodeFadeType_, nodePostProc_, nodeAlienRace_}) {
            if (control) control->ChangeValue("");
        }
        for (wxCheckBox* check : {nodeUnskippable_, nodeRecordVo_, nodeRecordNoVoOverride_}) if (check) check->SetValue(false);
        if (actionParamGrid_) actionParamGrid_->ClearGrid();
        clearLinkControls();
        animationValues_.clear();
        if (animationList_) animationList_->DeleteAllItems();
    }

    void clearLinkControls() {
        for (wxTextCtrl* control : {linkActive1_, linkActive2_, linkLogic_, linkParamStrA_, linkParamStrB_, linkComment_, linkDesignerNumber_})
            if (control) control->ChangeValue("");
        for (wxCheckBox* check : {linkNot1_, linkNot2_, linkIsChild_, linkReverseCond_}) if (check) check->SetValue(false);
        if (linkParamGrid_) linkParamGrid_->ClearGrid();
    }

    void refreshAnimationList() {
        animationList_->DeleteAllItems();
        animationValues_.clear();
        if (!activeDocument().selectedNode) return;
        const DlgDocument document = dialogue();
        animationValues_ = document.animations(*activeDocument().selectedNode);
        const bool jade = document.dialect() == DlgDialect::JadeEmpire;
        animationList_->SetColumnWidth(0, jade ? 0 : FromDIP(220));
        animationList_->SetColumnWidth(2, jade ? FromDIP(130) : 0);
        for (std::size_t i = 0; i < animationValues_.size(); ++i) {
            const long row = animationList_->InsertItem(static_cast<long>(i), wxui::toWx(animationValues_[i].participant));
            animationList_->SetItem(row, 1, wxString::Format("%d", animationValues_[i].animation));
            animationList_->SetItem(row, 2, wxString::Format("%d", animationValues_[i].emotion));
        }
    }

    void refreshRawGrid() {
        if (!rawGrid_ || !hasActiveDocument()) return;
        rawRefreshInProgress_ = true;
        std::vector<GffFieldRow> rows;
        if (model().loaded()) rows = model().rows();
        const std::string filter = lowerAscii(wxui::toStd(rawFilter_->GetValue()));
        rawRows_.clear();
        for (const auto& row : rows) {
            if (!filter.empty()) {
                const std::string haystack = lowerAscii(row.path + " " + row.label + " " + row.type + " " + row.value + " " + row.resolved);
                if (haystack.find(filter) == std::string::npos) continue;
            }
            rawRows_.push_back(row);
        }

        wxGridUpdateLocker lock(rawGrid_);
        const int current = rawGrid_->GetNumberRows();
        const int required = static_cast<int>(rawRows_.size());
        if (current < required) rawGrid_->AppendRows(required - current);
        else if (current > required) rawGrid_->DeleteRows(0, current - required);
        for (int row = 0; row < required; ++row) {
            const GffFieldRow& value = rawRows_[static_cast<std::size_t>(row)];
            rawGrid_->SetCellValue(row, 0, wxui::toWx(value.path));
            rawGrid_->SetCellValue(row, 1, wxui::toWx(value.type));
            rawGrid_->SetCellValue(row, 2, wxui::toWx(value.value));
            rawGrid_->SetCellValue(row, 3, wxui::toWx(value.resolved));
        }
        rawGrid_->ForceRefresh();
        rawRefreshInProgress_ = false;
    }

    void onRawCellChanged(wxGridEvent& event) {
        if (rawRefreshInProgress_ || event.GetCol() != 2) { event.Skip(); return; }
        const int row = event.GetRow();
        if (row < 0 || static_cast<std::size_t>(row) >= rawRows_.size()) { event.Skip(); return; }
        const GffFieldRow item = rawRows_[static_cast<std::size_t>(row)];
        if (!item.editable) {
            rawGrid_->SetCellValue(row, 2, wxui::toWx(item.value));
            event.Skip();
            return;
        }
        const std::string value = wxui::toStd(rawGrid_->GetCellValue(row, 2));
        mutate("Edit raw GFF value", [this, item, value]() { model().setValue(item.path, value); });
        event.Skip();
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
        if (hasActiveDocument() && activeDocument().workspacePage == 0) refreshConversationTree();
        Refresh();
    }

    void applyFontScale() {
        neoview::applyFontScale(this, fontScale_);
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

    void onClose(wxCloseEvent& event) {
        for (std::size_t i = 0; i < documents_.size(); ++i) {
            if (!confirmCloseDocument(i)) { event.Veto(); return; }
        }
        settings_.saveWindowPlacement(*this);
        event.Skip();
    }

    neosettings::AppSettings settings_;
    std::vector<DocumentTab> documents_;
    std::size_t activeDocumentIndex_ = neotabs::npos;
    bool tabSwitchInProgress_ = false;
    bool treeRefreshInProgress_ = false;
    bool rawRefreshInProgress_ = false;

    std::unique_ptr<neogames::OpenGameDirectoryMenu> gameDirectoryMenu_;
    wxMenu* recentFilesMenu_ = nullptr;
    wxMenuItem* undoItem_ = nullptr;
    wxMenuItem* redoItem_ = nullptr;
    wxMenuItem* conversationViewItem_ = nullptr;
    wxMenuItem* rawViewItem_ = nullptr;
    wxMenuItem* darkModeItem_ = nullptr;

    wxAuiNotebook* documentTabs_ = nullptr;
    wxNotebook* workspaceBook_ = nullptr;
    wxTextCtrl* filePath_ = nullptr;
    wxStaticText* typeText_ = nullptr;
    wxStaticText* statsText_ = nullptr;
    wxStaticText* tlkText_ = nullptr;

    wxTreeCtrl* conversationTree_ = nullptr;
    wxNotebook* inspectorBook_ = nullptr;
    wxTextCtrl* findText_ = nullptr;
    std::map<DlgNodeRef, wxTreeItemId> canonicalTreeItems_;
    std::string lastSearchTerm_;
    std::vector<DlgNodeRef> searchResults_;
    std::size_t searchIndex_ = 0;

    wxStaticText* nodeHeader_ = nullptr;
    wxStaticText* nodeSpeakerLabel_ = nullptr;
    wxComboBox* nodeSpeaker_ = nullptr;
    wxTextCtrl* nodeListener_ = nullptr;
    wxTextCtrl* nodeStrRef_ = nullptr;
    wxTextCtrl* nodeStringType_ = nullptr;
    wxTextCtrl* nodeLocalText_ = nullptr;
    wxTextCtrl* nodeResolvedText_ = nullptr;
    wxTextCtrl* nodeVo_ = nullptr;
    wxTextCtrl* nodeComment_ = nullptr;

    wxTextCtrl* nodeScript1_ = nullptr;
    wxTextCtrl* nodeScript2_ = nullptr;
    wxTextCtrl* nodeScriptCamEntry_ = nullptr;
    wxTextCtrl* nodeScriptCamReplies_ = nullptr;
    wxTextCtrl* nodeQuest_ = nullptr;
    wxTextCtrl* nodeQuestEntry_ = nullptr;
    wxTextCtrl* nodePlotIndex_ = nullptr;
    wxTextCtrl* nodePlotXp_ = nullptr;
    wxTextCtrl* nodeActionStrA_ = nullptr;
    wxTextCtrl* nodeActionStrB_ = nullptr;
    wxGrid* actionParamGrid_ = nullptr;

    wxTextCtrl* nodeSound_ = nullptr;
    wxTextCtrl* nodeDelay_ = nullptr;
    wxTextCtrl* nodeWaitFlags_ = nullptr;
    wxTextCtrl* nodeCameraAngle_ = nullptr;
    wxTextCtrl* nodeCameraId_ = nullptr;
    wxTextCtrl* nodeCameraAnimation_ = nullptr;
    wxTextCtrl* nodeEmotion_ = nullptr;
    wxTextCtrl* nodeFacialAnim_ = nullptr;
    wxTextCtrl* nodeCamVidEffect_ = nullptr;
    wxTextCtrl* nodeFadeType_ = nullptr;
    wxTextCtrl* nodePostProc_ = nullptr;
    wxTextCtrl* nodeAlienRace_ = nullptr;
    wxCheckBox* nodeUnskippable_ = nullptr;
    wxCheckBox* nodeRecordVo_ = nullptr;
    wxCheckBox* nodeRecordNoVoOverride_ = nullptr;

    wxStaticText* linkHeader_ = nullptr;
    wxTextCtrl* linkActive1_ = nullptr;
    wxTextCtrl* linkActive2_ = nullptr;
    wxTextCtrl* linkLogic_ = nullptr;
    wxTextCtrl* linkParamStrA_ = nullptr;
    wxTextCtrl* linkParamStrB_ = nullptr;
    wxTextCtrl* linkComment_ = nullptr;
    wxTextCtrl* linkDesignerNumber_ = nullptr;
    wxCheckBox* linkNot1_ = nullptr;
    wxCheckBox* linkNot2_ = nullptr;
    wxCheckBox* linkIsChild_ = nullptr;
    wxCheckBox* linkReverseCond_ = nullptr;
    wxGrid* linkParamGrid_ = nullptr;

    wxListCtrl* animationList_ = nullptr;
    std::vector<DlgAnimation> animationValues_;

    wxTextCtrl* rawFilter_ = nullptr;
    wxGrid* rawGrid_ = nullptr;
    std::vector<GffFieldRow> rawRows_;

    neoview::FontScaleWheelFilter fontScaleWheelFilter_;
    double fontScale_ = neoview::kDefaultFontScale;
    bool darkMode_ = false;
};

class NeoDLGApp final : public wxApp {
public:
    bool OnInit() override {
        auto* frame = new NeoDLGFrame();
        frame->Show(true);
        if (argc > 1) frame->openStartupFile(neosettings::pathFromWx(wxString(argv[1])));
        return true;
    }
};

} // namespace

wxIMPLEMENT_APP(NeoDLGApp);
