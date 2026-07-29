#include "core/AppModel.hpp"
#include "core/GffJson.hpp"
#include "neodlg/model/DlgDocument.hpp"
#include "neodlg/model/DlgSemanticOptions.hpp"
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
#include <wx/clrpicker.h>
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
            auto* moveUp = new wxButton(tagPage, wxID_ANY, "Up");
            auto* moveDown = new wxButton(tagPage, wxID_ANY, "Down");
            buttons->Add(add, 0, wxRIGHT, 6);
            buttons->Add(edit, 0, wxRIGHT, 6);
            buttons->Add(remove, 0, wxRIGHT, 12);
            buttons->Add(moveUp, 0, wxRIGHT, 6);
            buttons->Add(moveDown, 0);
            tagSizer->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
            tagPage->SetSizer(tagSizer);
            book->AddPage(tagPage, "Participants", false);
            add->Bind(wxEVT_BUTTON, &ConversationPropertiesDialog::onAddTag, this);
            edit->Bind(wxEVT_BUTTON, &ConversationPropertiesDialog::onEditTag, this);
            remove->Bind(wxEVT_BUTTON, &ConversationPropertiesDialog::onDeleteTag, this);
            moveUp->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { moveTag(-1); });
            moveDown->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { moveTag(1); });
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
        tags_->SetItemState(target, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
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
                         bool referenceOnly = false)
        : kind(itemKind), node(std::move(nodeRef)), link(std::move(linkRef)), reference(referenceOnly) {}

    ConversationTreeKind kind = ConversationTreeKind::Group;
    std::optional<DlgNodeRef> node;
    std::optional<DlgLinkRef> link;
    bool reference = false;
};


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
        wxui::configureResponsiveWindow(*this, wxSize(1420, 900), wxSize(720, 480));
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
        rawViewItem_ = view->AppendRadioItem(ID_ViewRaw, "GFF Structure Tree");
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
                              "Conversation graph editing, link conditions, TLK text, scripts, animations, validation, and structured GFF access.");
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
        header->Add(infoRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

        auto* tlkRow = new wxBoxSizer(wxHORIZONTAL);
        tlkRow->Add(new wxStaticText(panel, wxID_ANY, "TLK:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        tlkText_ = new wxTextCtrl(panel, wxID_ANY, "none", wxDefaultPosition, wxDefaultSize, wxTE_READONLY);
        tlkRow->Add(tlkText_, 1, wxEXPAND | wxRIGHT, 8);
        tlkRow->Add(new wxButton(panel, ID_OpenTlk, "Choose..."), 0, wxRIGHT, 4);
        tlkRow->Add(new wxButton(panel, ID_ClearTlk, "Clear"), 0);
        header->Add(tlkRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);
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
        Bind(wxEVT_TREE_ITEM_ACTIVATED, &NeoDLGFrame::onRawTreeActivated, this, ID_RawTree);
        Bind(wxEVT_TREE_ITEM_EXPANDING, &NeoDLGFrame::onRawTreeExpanding, this, ID_RawTree);
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

        auto* toolbar = new wxWrapSizer(wxHORIZONTAL);
        const auto addToolbarButton = [&](int id, const wxString& label) {
            toolbar->Add(new wxButton(page, id, label), 0,
                         wxRIGHT | wxBOTTOM, FromDIP(4));
        };
        addToolbarButton(ID_AddStartingEntry, "Add Start Entry");
        addToolbarButton(ID_AddChild, "Add Child");
        addToolbarButton(ID_LinkExisting, "Link Existing...");
        addToolbarButton(ID_DuplicateNode, "Duplicate");
        addToolbarButton(ID_RemoveLink, "Remove Link");
        addToolbarButton(ID_DeleteNode, "Delete Node");
        addToolbarButton(ID_MoveLinkUp, "Up");
        addToolbarButton(ID_MoveLinkDown, "Down");
        root->Add(toolbar, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(2));

        auto* findRow = new wxBoxSizer(wxHORIZONTAL);
        findRow->Add(new wxStaticText(page, wxID_ANY, "Find:"), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
        findText_ = new wxTextCtrl(page, wxID_ANY);
        findText_->SetMinSize(FromDIP(wxSize(160, -1)));
        findRow->Add(findText_, 1, wxEXPAND | wxRIGHT, FromDIP(4));
        findRow->Add(new wxButton(page, ID_FindNext, "Next"), 0);
        root->Add(findRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));

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
        linePage_ = page;
        nodeHeader_ = new wxTextCtrl(page, wxID_ANY, "Select a dialogue node.", wxDefaultPosition,
                                     FromDIP(wxSize(-1, 92)),
                                     wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP | wxBORDER_NONE);
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

        nodeListenerLabel_ = new wxStaticText(page, wxID_ANY, "Listener:");
        form->Add(nodeListenerLabel_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 8);
        nodeListener_ = new wxComboBox(page, wxID_ANY);
        form->Add(nodeListener_, 1, wxEXPAND);

        nodeStrRef_ = addTextField(page, form, "Text StrRef:", 0, wxDefaultSize, &nodeStrRefLabel_);
        nodeStringType_ = addTextField(page, form, "Jade string type:", 0, wxDefaultSize, &nodeStringTypeLabel_);
        nodeLocalText_ = addTextField(page, form, "Local text:", wxTE_MULTILINE,
                                      FromDIP(wxSize(-1, 110)), &nodeLocalTextLabel_);
        nodeResolvedText_ = addTextField(page, form, "Resolved TLK text:", wxTE_MULTILINE | wxTE_READONLY,
                                         FromDIP(wxSize(-1, 110)), &nodeResolvedTextLabel_);
        nodeVo_ = addTextField(page, form, "Voice-over resref:", 0, wxDefaultSize, &nodeVoLabel_);
        nodeJadeSkippable_ = addCheckField(page, form, "Entry can be skipped", &nodeJadeSkippablePlaceholder_);
        nodeJadeSkippable_->SetToolTip(
            "Jade Empire Entry Skippable field. The runtime default is enabled when the field is absent.");
        nodeComment_ = addTextField(page, form, "Designer comment:", wxTE_MULTILINE,
                                    FromDIP(wxSize(-1, 80)), &nodeCommentLabel_);
        root->Add(form, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);
        root->Add(new wxButton(page, ID_ApplyNode, "Apply Line Changes"), 0, wxALIGN_RIGHT | wxALL, 10);
    }

    void buildScriptsPage(wxNotebook* book) {
        wxBoxSizer* root = nullptr;
        wxScrolledWindow* page = makeInspectorPage(book, "Scripts / Quest", root);
        scriptsPage_ = page;
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
        actionParamGrid_ = new wxGrid(page, wxID_ANY);
        actionParamGrid_->CreateGrid(5, 2);
        actionParamGrid_->SetColLabelValue(0, "Script 1");
        actionParamGrid_->SetColLabelValue(1, "Script 2");
        for (int row = 0; row < 5; ++row) actionParamGrid_->SetRowLabelValue(row, wxString::Format("Param %d", row + 1));
        actionParamGrid_->SetMinSize(FromDIP(wxSize(420, 190)));
        root->Add(actionParamGrid_, 0, wxEXPAND | wxALL, 10);
        nodeCameraEntry_->SetToolTip(
            "Free-form Jade camera tag exposed to scripts; NeoDLG stores it in lowercase.");
        nodeCameraReplies_->SetToolTip(
            "Free-form Jade reply-camera tag exposed to scripts; NeoDLG stores it in lowercase.");
        root->Add(new wxButton(page, ID_ApplyScripts, "Apply Script / Quest Changes"), 0, wxALIGN_RIGHT | wxALL, 10);
    }

    void buildPresentationPage(wxNotebook* book) {
        wxBoxSizer* root = nullptr;
        wxScrolledWindow* page = makeInspectorPage(book, "Presentation", root);
        presentationPage_ = page;
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

        nodePostProc_ = addTextField(page, form, "Post-process node:", 0, wxDefaultSize, &nodePostProcLabel_);
        nodeAlienRace_ = addTextField(page, form, "Alien-race node:", 0, wxDefaultSize, &nodeAlienRaceLabel_);
        nodeUnskippable_ = addCheckField(page, form, "Node is unskippable", &nodeUnskippablePlaceholder_);
        nodeRecordVo_ = addCheckField(page, form, "Record VO", &nodeRecordVoPlaceholder_);
        nodeRecordNoVoOverride_ = addCheckField(page, form, "No-VO override", &nodeRecordNoVoPlaceholder_);

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

        nodeCameraAngle_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { updateCameraControls(); });
        nodeCameraFovMode_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { updateCameraControls(); });
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
        if (presentationPage_) {
            presentationPage_->Layout();
            presentationPage_->FitInside();
        }
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
        wxScrolledWindow* page = makeInspectorPage(book, "Link / Conditions", root);
        linkPage_ = page;
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
        linkComment_ = addTextField(page, form, "Link comment:", wxTE_MULTILINE,
                                    FromDIP(wxSize(-1, 70)), &linkCommentLabel_);
        linkDesignerNumber_ = addTextField(page, form, "Designer number available to script:", 0,
                                           wxDefaultSize, &linkDesignerNumberLabel_);
        linkNot1_ = addCheckField(page, form, "Negate conditional 1", &linkNot1Placeholder_);
        linkNot2_ = addCheckField(page, form, "Negate conditional 2", &linkNot2Placeholder_);
        linkIsChild_ = addCheckField(page, form, "IsChild link", &linkIsChildPlaceholder_);
        linkReverseCond_ = addCheckField(page, form, "Negate condition", &linkReverseCondPlaceholder_);
        root->Add(form, 0, wxEXPAND | wxALL, 10);

        linkParamHeading_ = new wxStaticText(page, wxID_ANY, "Conditional integer parameters");
        root->Add(linkParamHeading_, 0, wxLEFT | wxRIGHT | wxTOP, 10);
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
        animationAddButton_ = new wxButton(page, ID_AnimationAdd, "Add...");
        animationEditButton_ = new wxButton(page, ID_AnimationEdit, "Edit...");
        animationDeleteButton_ = new wxButton(page, ID_AnimationDelete, "Delete");
        animationUpButton_ = new wxButton(page, ID_AnimationUp, "Up");
        animationDownButton_ = new wxButton(page, ID_AnimationDown, "Down");
        buttons->Add(animationAddButton_, 0, wxRIGHT, 4);
        buttons->Add(animationEditButton_, 0, wxRIGHT, 4);
        buttons->Add(animationDeleteButton_, 0, wxRIGHT, 12);
        buttons->Add(animationUpButton_, 0, wxRIGHT, 4);
        buttons->Add(animationDownButton_, 0);
        root->Add(buttons, 0, wxLEFT | wxRIGHT | wxBOTTOM, 10);
        page->SetSizer(root);
        book->AddPage(page, "Animations", false);
    }

    void buildRawPage(wxNotebook* parent) {
        auto* page = new wxPanel(parent);
        auto* root = new wxBoxSizer(wxVERTICAL);
        auto* filterRow = new wxBoxSizer(wxHORIZONTAL);
        filterRow->Add(new wxStaticText(page, wxID_ANY, "Filter GFF fields:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
        rawFilter_ = new wxTextCtrl(page, wxID_ANY);
        filterRow->Add(rawFilter_, 1);
        root->Add(filterRow, 0, wxEXPAND | wxBOTTOM, 6);

        rawTree_ = new wxTreeCtrl(page, ID_RawTree, wxDefaultPosition, wxDefaultSize,
                                  wxTR_HAS_BUTTONS | wxTR_LINES_AT_ROOT | wxTR_SINGLE);
        root->Add(rawTree_, 1, wxEXPAND);
        page->SetSizer(root);
        parent->AddPage(page, "GFF Tree", false);
        rawFilter_->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { refreshRawTree(); });
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
                } else if (cameraAngle == "0" || cameraAngle == "1" ||
                           cameraAngle == "2" || cameraAngle == "3") {
                    if (document.hasNodeField(ref, "CameraID")) {
                        document.setNodeField(ref, "CameraID", FIELD_TYPE_INT, "-1");
                    }
                } else if (document.hasNodeField(ref, "CameraID") || nodeCameraId_->IsModified()) {
                    setOptionalNodeField(document, ref, "CameraID", FIELD_TYPE_INT,
                                         nodeCameraId_->GetValue(), false);
                }

                const auto setOptionalFloat = [&](const char* label,
                                                  wxTextCtrl* control,
                                                  const std::string& fieldName,
                                                  const std::function<void(float)>& validate) {
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
                    setOptionalNodeField(document, ref, "CamFieldOfView", FIELD_TYPE_FLOAT,
                                         wxui::toWx(cameraFovValue()), true);
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

                if (loadedFadeColorPresent_ || fadeColorEdited_) {
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

            setOptionalLinkField(document, ref, "Active2", FIELD_TYPE_RESREF,
                                 linkActive2_->GetValue(), false);
            setOptionalLinkField(document, ref, "Logic", FIELD_TYPE_INT,
                                 linkLogic_->GetValue(), false);
            setOptionalLinkField(document, ref, "ParamStrA", FIELD_TYPE_CEXOSTRING,
                                 linkParamStrA_->GetValue(), false);
            setOptionalLinkField(document, ref, "ParamStrB", FIELD_TYPE_CEXOSTRING,
                                 linkParamStrB_->GetValue(), false);
            setOptionalLinkField(document, ref, "LinkComment", FIELD_TYPE_CEXOSTRING,
                                 linkComment_->GetValue(), false);
            setOptionalLinkField(document, ref, "Not", FIELD_TYPE_BYTE,
                                 wxui::toWx(boolText(linkNot1_)), document.hasLinkField(ref, "Not"));
            setOptionalLinkField(document, ref, "Not2", FIELD_TYPE_BYTE,
                                 wxui::toWx(boolText(linkNot2_)), document.hasLinkField(ref, "Not2"));
            setOptionalLinkField(document, ref, "IsChild", FIELD_TYPE_BYTE,
                                 wxui::toWx(boolText(linkIsChild_)), true);
            for (int i = 0; i < 5; ++i) {
                setOptionalLinkField(document, ref, "Param" + std::to_string(i + 1), FIELD_TYPE_INT,
                                     linkParamGrid_->GetCellValue(i, 0), false);
                setOptionalLinkField(document, ref, "Param" + std::to_string(i + 1) + "b", FIELD_TYPE_INT,
                                     linkParamGrid_->GetCellValue(i, 1), false);
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
        if (dialogue().dialect() == DlgDialect::JadeEmpire && ref.kind == DlgNodeKind::Reply) return;
        const long row = selectedAnimationRow();
        const long target = row + delta;
        if (row < 0 || target < 0 || static_cast<std::size_t>(target) >= animationValues_.size()) return;
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
            wxArrayString modeChoices;
            modeChoices.Add("Dynamic merge instructions (recommended)");
            modeChoices.Add("Complete modified DLG replacement");
            wxSingleChoiceDialog modeDialog(
                this,
                "Choose how the package should install this dialogue.\n\n"
                "Dynamic merge appends new Entry and Reply nodes at installation time and connects them with ListIndex and 2DAMEMORY tokens. It is merge-friendly, but requires original TSLPatcher 1.2.10b1 or a HoloPatcher release that supports the same dynamic GFF syntax. Changes such as deletion or root-list reordering are rejected.\n\n"
                "Complete replacement installs the entire modified DLG. It does not use dynamic tokens and works with patchers that cannot process TypeId=ListIndex, but it can conflict with another mod that replaces the same DLG.",
                "TSLPatcher Export Mode",
                modeChoices);
            modeDialog.SetSelection(0);
            if (modeDialog.ShowModal() != wxID_OK) return;

            const auto patchMode = modeDialog.GetSelection() == 0
                ? neodlg::patcher::DlgPatchMode::DynamicMerge
                : neodlg::patcher::DlgPatchMode::CompleteReplacement;

            std::optional<std::filesystem::path> originalPath;
            if (patchMode == neodlg::patcher::DlgPatchMode::DynamicMerge) {
                originalPath = wxui::chooseOpenFile(
                    this,
                    "Select clean/unmodified DLG",
                    kDlgWildcard);
                if (!originalPath) return;
            }

            std::string defaultName = model().filename().empty() ? "modified.dlg" : neosettings::pathToUtf8(model().filename().filename());
            const auto patchName = wxui::promptText(this, "Patch Target Filename",
                                                     "DLG filename to patch in the user's install:", defaultName);
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

            const auto outputDir = wxui::chooseDirectory(this, "Choose tslpatchdata package folder");
            if (!outputDir) return;
            neotsl::PatchProject project;
            if (patchMode == neodlg::patcher::DlgPatchMode::DynamicMerge) {
                GffModel original;
                original.load(*originalPath);
                project = neodlg::patcher::diffDlgPatcher(
                    original.gff(),
                    model().gff(),
                    *patchName,
                    patchMode,
                    true,
                    *originalPath,
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
            neotsl::writePackage(project, *outputDir, true);

            const std::string detail = patchMode == neodlg::patcher::DlgPatchMode::DynamicMerge
                ? "The package uses TSLPatcher's dynamic ListIndex and 2DAMEMORY workflow. New Entry and Reply nodes receive their actual indexes during installation, and generated link fields are updated to those indexes automatically."
                : "The package installs the complete modified DLG and does not use dynamic ListIndex or 2DAMEMORY wiring. This mode is less merge-friendly when another mod replaces the same DLG.";
            wxui::showMessage(
                this,
                "TSL/HoloPatcher Package",
                "Wrote changes.ini, info.rtf, and the required DLG file to:\n" +
                    neosettings::pathToUtf8(*outputDir) + "\n\n" + detail);
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
            else refreshRawTree();
        }
    }

    void onWorkspacePageChanged(wxBookCtrlEvent& event) {
        if (hasActiveDocument()) activeDocument().workspacePage = event.GetSelection();
        if (event.GetSelection() == 1) refreshRawTree();
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
        else refreshRawTree();
        refreshInspector();
    }

    void refreshHeader() {
        if (!hasActiveDocument() || !model().loaded()) {
            filePath_->ChangeValue("");
            typeText_->SetLabel("No DLG loaded");
            statsText_->SetLabel("");
            tlkText_->ChangeValue("none");
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
            statsText_->SetLabel("Use GFF Tree view");
        }
        tlkText_->ChangeValue(model().tlk().loaded() ? neosettings::pathToWx(model().tlk().filename()) : wxString("none"));
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
                "This DLG schema is not supported by the conversation editor. Use GFF Tree view.");
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
        const bool valid = hasActiveDocument() && model().loaded() &&
                           dialogue().semanticallyEditable() && activeDocument().selectedNode;
        enableInspector(valid);
        if (!valid) {
            updateDialectFieldVisibility(DlgFlavor::Kotor, DlgNodeKind::Entry);
            nodeHeader_->ChangeValue("Select a dialogue node.");
            linkHeader_->SetLabel("Select a linked node to edit its conditions.");
            clearInspectorControls();
            return;
        }

        const DlgDocument document = dialogue();
        const DlgNodeRef ref = *activeDocument().selectedNode;
        const bool jade = document.dialect() == DlgDialect::JadeEmpire;
        const bool jadeEntry = jade && ref.kind == DlgNodeKind::Entry;
        updateDialectFieldVisibility(document.flavor(), ref.kind);
        nodeHeader_->ChangeValue(wxui::toWx(document.nodeLabel(ref, 200)));

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
                actionParamGrid_->SetCellValue(
                    i, 0, wxui::toWx(document.nodeField(ref, "ActionParam" + std::to_string(i + 1))));
                actionParamGrid_->SetCellValue(
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
            loadField(nodePostProc_, document.nodeField(ref, "PostProcNode"));
            loadField(nodeAlienRace_, document.nodeField(ref, "AlienRaceNode"));
            setBoolControl(nodeUnskippable_, document.nodeField(ref, "NodeUnskippable"));
            setBoolControl(nodeRecordVo_, document.nodeField(ref, "RecordVO"));
            setBoolControl(nodeRecordNoVoOverride_, document.nodeField(ref, "RecordNoVOOverri"));
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
                loadField(linkComment_, document.linkField(linkRef, "LinkComment"));
                setBoolControl(linkNot1_, document.linkField(linkRef, "Not"));
                setBoolControl(linkNot2_, document.linkField(linkRef, "Not2"));
                setBoolControl(linkIsChild_, document.linkField(linkRef, "IsChild"));
                for (int i = 0; i < 5; ++i) {
                    linkParamGrid_->SetCellValue(
                        i, 0, wxui::toWx(document.linkField(linkRef, "Param" + std::to_string(i + 1))));
                    linkParamGrid_->SetCellValue(
                        i, 1, wxui::toWx(document.linkField(linkRef, "Param" + std::to_string(i + 1) + "b")));
                }
            }
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
        return {nodeSpeaker_, nodeListener_, nodeStrRef_, nodeStringType_, nodeLocalText_, nodeResolvedText_, nodeVo_,
                nodeJadeSkippable_, nodeComment_, nodeScript1_, nodeScript2_, nodeScriptCamEntry_, nodeCameraEntry_,
                nodeScriptCamReplies_, nodeCameraReplies_, nodeQuest_, nodeQuestEntry_, nodePlotIndex_, nodePlotXp_,
                nodeActionStrA_, nodeActionStrB_, actionParamGrid_, nodeSound_, nodeDelay_, nodeWaitFlags_,
                nodeCameraAngle_, nodeCameraId_, nodeCamHeightOffset_, nodeTarHeightOffset_, nodeCameraFovMode_, nodeCameraFov_,
                nodeCameraAnimation_, nodeEmotion_, nodeFacialAnim_, nodeCamVidEffectPanel_, nodeFadeType_, nodeFadeColorPicker_,
                nodeFadeColorR_, nodeFadeColorG_, nodeFadeColorB_, nodeFadeDelay_, nodeFadeLength_, nodePostProc_, nodeAlienRace_,
                nodeUnskippable_, nodeRecordVo_, nodeRecordNoVoOverride_, animationList_, animationAddButton_,
                animationEditButton_, animationDeleteButton_, animationUpButton_, animationDownButton_};
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
        if (nodeListener_) {
            nodeListener_->Clear();
            nodeListener_->ChangeValue("");
        }
        for (wxTextCtrl* control : {nodeStrRef_, nodeStringType_, nodeLocalText_, nodeResolvedText_, nodeVo_, nodeComment_,
                                    nodeScript1_, nodeScript2_, nodeScriptCamEntry_, nodeCameraEntry_,
                                    nodeScriptCamReplies_, nodeCameraReplies_, nodeQuest_, nodeQuestEntry_, nodePlotIndex_,
                                    nodePlotXp_, nodeActionStrA_, nodeActionStrB_, nodeSound_, nodeDelay_, nodeWaitFlags_, nodeCameraId_,
                                    nodeCamHeightOffset_, nodeTarHeightOffset_, nodeCameraFov_, nodeCameraAnimation_, nodeEmotion_, nodeFacialAnim_,
                                    nodeFadeColorR_, nodeFadeColorG_, nodeFadeColorB_, nodeFadeDelay_, nodeFadeLength_, nodePostProc_, nodeAlienRace_}) {
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
        for (wxCheckBox* check : {nodeJadeSkippable_, nodeUnskippable_, nodeRecordVo_, nodeRecordNoVoOverride_}) {
            if (check) check->SetValue(false);
        }
        if (actionParamGrid_) actionParamGrid_->ClearGrid();
        clearLinkControls();
        animationValues_.clear();
        if (animationList_) animationList_->DeleteAllItems();
        for (wxButton* button : {animationAddButton_, animationEditButton_, animationDeleteButton_,
                                 animationUpButton_, animationDownButton_}) {
            if (button) button->Enable(false);
        }
    }

    void clearLinkControls() {
        for (wxTextCtrl* control : {linkActive1_, linkActive2_, linkLogic_, linkParamStrA_, linkParamStrB_, linkComment_, linkDesignerNumber_})
            if (control) control->ChangeValue("");
        for (wxCheckBox* check : {linkNot1_, linkNot2_, linkIsChild_, linkReverseCond_}) if (check) check->SetValue(false);
        if (linkParamGrid_) linkParamGrid_->ClearGrid();
    }

    void refreshAnimationList() {
        if (!animationList_) return;
        animationList_->DeleteAllItems();
        animationValues_.clear();

        const bool hasNode = hasActiveDocument() && activeDocument().selectedNode.has_value();
        if (!hasNode) {
            for (wxButton* button : {animationAddButton_, animationEditButton_, animationDeleteButton_,
                                     animationUpButton_, animationDownButton_}) {
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
        if (animationUpButton_) animationUpButton_->Enable(!jadeReply && animationValues_.size() > 1);
        if (animationDownButton_) animationDownButton_->Enable(!jadeReply && animationValues_.size() > 1);
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
            const auto grandchildren = rawTreeChildrenByParent_.find(row.path);
            if (grandchildren != rawTreeChildrenByParent_.end() && !grandchildren->second.empty()) {
                rawTree_->SetItemHasChildren(item, true);
            }
        }
    }

    void refreshRawTree() {
        if (!rawTree_ || !hasActiveDocument()) return;
        wxWindowUpdateLocker updateLocker(rawTree_);
        rawRows_.clear();
        rawTreeChildrenByParent_.clear();
        rawTreeMaterializedPaths_.clear();
        rawTree_->DeleteAllItems();

        const std::string filter = rawFilter_ ? lowerAscii(wxui::toStd(rawFilter_->GetValue())) : std::string{};
        if (model().loaded()) {
            for (const auto& row : model().rows()) {
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
            : (model().filename().empty() ? std::string("New DLG") : neosettings::pathToUtf8(model().filename()));
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
        rawTree_->Expand(root);
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

    void updateDialectFieldVisibility(DlgFlavor flavor, DlgNodeKind kind) {
        const bool jade = flavor == DlgFlavor::JadeEmpire;
        const bool jadeEntry = jade && kind == DlgNodeKind::Entry;

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
        showWindows({actionParamHeading_, actionParamGrid_}, !jade);

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
        showPair(nodePostProcLabel_, nodePostProc_, !jade);
        showPair(nodeAlienRaceLabel_, nodeAlienRace_, !jade);
        showPair(nodeUnskippablePlaceholder_, nodeUnskippable_, !jade);
        showPair(nodeRecordVoPlaceholder_, nodeRecordVo_, !jade);
        showPair(nodeRecordNoVoPlaceholder_, nodeRecordNoVoOverride_, !jade);
        if (presentationApplyButton_) presentationApplyButton_->Show(!jade);

        // Jade links contain one condition, ReverseCond, DesignerNumber, and Index.
        showPair(linkActive1Label_, linkActive1_, true);
        showPair(linkActive2Label_, linkActive2_, !jade);
        showPair(linkLogicLabel_, linkLogic_, !jade);
        showPair(linkParamStrALabel_, linkParamStrA_, !jade);
        showPair(linkParamStrBLabel_, linkParamStrB_, !jade);
        showPair(linkCommentLabel_, linkComment_, !jade);
        showPair(linkNot1Placeholder_, linkNot1_, !jade);
        showPair(linkNot2Placeholder_, linkNot2_, !jade);
        showPair(linkIsChildPlaceholder_, linkIsChild_, !jade);
        showPair(linkDesignerNumberLabel_, linkDesignerNumber_, jade);
        showPair(linkReverseCondPlaceholder_, linkReverseCond_, jade);
        showWindows({linkParamHeading_, linkParamGrid_}, !jade);
        if (linkActive1Label_) linkActive1Label_->SetLabel(jade ? "Condition script:" : "Conditional script 1:");

        if (inspectorBook_ && inspectorBook_->GetPageCount() >= 5) {
            inspectorBook_->SetPageText(0, jadeEntry ? "Entry Line" : (jade ? "Reply Line" : "Line"));
            inspectorBook_->SetPageText(1, jadeEntry ? "Scripts / Camera" : (jade ? "Reply Script" : "Scripts / Quest"));
            inspectorBook_->SetPageText(2, jade ? "Jade Presentation" : "Presentation");
            inspectorBook_->SetPageText(4, jadeEntry ? "Entry Animations" : (jade ? "Reply Animation" : "Animations"));
        }

        if (linePage_) { linePage_->Layout(); linePage_->FitInside(); }
        if (scriptsPage_) { scriptsPage_->Layout(); scriptsPage_->FitInside(); }
        if (presentationPage_) { presentationPage_->Layout(); presentationPage_->FitInside(); }
        if (linkPage_) { linkPage_->Layout(); linkPage_->FitInside(); }
        if (inspectorBook_) inspectorBook_->Layout();
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
    wxTextCtrl* tlkText_ = nullptr;

    wxTreeCtrl* conversationTree_ = nullptr;
    wxNotebook* inspectorBook_ = nullptr;
    wxTextCtrl* findText_ = nullptr;
    std::map<DlgNodeRef, wxTreeItemId> canonicalTreeItems_;
    std::string lastSearchTerm_;
    std::vector<DlgNodeRef> searchResults_;
    std::size_t searchIndex_ = 0;

    wxTextCtrl* nodeHeader_ = nullptr;
    wxScrolledWindow* linePage_ = nullptr;
    wxScrolledWindow* scriptsPage_ = nullptr;
    wxScrolledWindow* presentationPage_ = nullptr;
    wxScrolledWindow* linkPage_ = nullptr;

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
    wxGrid* actionParamGrid_ = nullptr;

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

    wxStaticText* nodePostProcLabel_ = nullptr;
    wxTextCtrl* nodePostProc_ = nullptr;
    wxStaticText* nodeAlienRaceLabel_ = nullptr;
    wxTextCtrl* nodeAlienRace_ = nullptr;
    wxStaticText* nodeUnskippablePlaceholder_ = nullptr;
    wxCheckBox* nodeUnskippable_ = nullptr;
    wxStaticText* nodeRecordVoPlaceholder_ = nullptr;
    wxCheckBox* nodeRecordVo_ = nullptr;
    wxStaticText* nodeRecordNoVoPlaceholder_ = nullptr;
    wxCheckBox* nodeRecordNoVoOverride_ = nullptr;

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
    wxStaticText* linkCommentLabel_ = nullptr;
    wxTextCtrl* linkComment_ = nullptr;
    wxStaticText* linkDesignerNumberLabel_ = nullptr;
    wxTextCtrl* linkDesignerNumber_ = nullptr;
    wxStaticText* linkNot1Placeholder_ = nullptr;
    wxCheckBox* linkNot1_ = nullptr;
    wxStaticText* linkNot2Placeholder_ = nullptr;
    wxCheckBox* linkNot2_ = nullptr;
    wxStaticText* linkIsChildPlaceholder_ = nullptr;
    wxCheckBox* linkIsChild_ = nullptr;
    wxStaticText* linkReverseCondPlaceholder_ = nullptr;
    wxCheckBox* linkReverseCond_ = nullptr;
    wxStaticText* linkParamHeading_ = nullptr;
    wxGrid* linkParamGrid_ = nullptr;

    wxListCtrl* animationList_ = nullptr;
    wxButton* animationAddButton_ = nullptr;
    wxButton* animationEditButton_ = nullptr;
    wxButton* animationDeleteButton_ = nullptr;
    wxButton* animationUpButton_ = nullptr;
    wxButton* animationDownButton_ = nullptr;
    std::vector<DlgAnimation> animationValues_;

    wxTextCtrl* rawFilter_ = nullptr;
    wxTreeCtrl* rawTree_ = nullptr;
    std::vector<GffFieldRow> rawRows_;
    std::vector<wxTreeItemId> rawTreeRowItems_;
    std::unordered_map<std::string, std::vector<std::size_t>> rawTreeChildrenByParent_;
    std::set<std::string> rawTreeMaterializedPaths_;

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
