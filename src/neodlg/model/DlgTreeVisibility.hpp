#pragma once

#include "DlgFieldApplicability.hpp"

#include <charconv>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace neodlg {

// A read-only projection policy for NeoDLG's GFF tree, NOT a schema validator.
// Paths and list indices stay untouched. Unknown fields/structures stay visible;
// Optional fields are visible by default, even when empty or inactive. This
// does not bypass game/dialect or link-direction rules, or restore deleted fields.
class DlgTreeFieldVisibility {
public:
    explicit DlgTreeFieldVisibility(const DlgDocument& document, bool showOptional = true)
        : document_(document), flavor_(inspectorFieldFlavor(document)), optional_(showOptional) {}

    bool visible(std::string_view path) const {
        std::vector<std::string_view> parts;
        while (!path.empty()) {
            const auto separator = path.find('\\');
            parts.push_back(path.substr(0, separator));
            if (separator == std::string_view::npos) break;
            path.remove_prefix(separator + 1);
        }
        // Check every component: children of a hidden struct/list or a localized
        // field must not reappear at the root when search removes their parent.
        for (const auto part : parts)
            if (isRemovedInspectorField(fieldLabel(part))) return false;
        if (!document_.semanticallyEditable() || parts.size() < 3) return true;

        const auto index = parseIndex(parts[1]);
        if (!index) return true; // Preserve unfamiliar/duplicate container paths.
        if (parts[0] == "StartingList")
            return linkVisible({DlgLinkOwner::StartingList, 0, *index}, fieldLabel(parts[2]));
        const bool entry = parts[0] == "EntryList";
        if (!entry && parts[0] != "ReplyList") return true;
        const DlgNodeRef node{entry ? DlgNodeKind::Entry : DlgNodeKind::Reply, *index};
        if (!nodeVisible(node, fieldLabel(parts[2]))) return false;
        if (parts.size() >= 5 && parts[2] == (entry ? "RepliesList" : "EntriesList")) {
            const auto linkIndex = parseIndex(parts[3]);
            if (linkIndex)
                return linkVisible({entry ? DlgLinkOwner::Entry : DlgLinkOwner::Reply,
                                    *index, *linkIndex}, fieldLabel(parts[4]));
        }
        return true;
    }

private:
    static std::optional<std::size_t> parseIndex(std::string_view value) {
        if (value.empty()) return std::nullopt;
        std::size_t index{};
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), index);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
        return index;
    }

    static std::string_view fieldLabel(std::string_view part) {
        // AppModel uses Field[#N] for duplicates and Field(strref)/Field(langN)
        // for localized rows. Strip only these known suffixes, not label text.
        const auto loc = part.find('(');
        if (loc != std::string_view::npos && part.back() == ')') {
            const auto suffix = part.substr(loc);
            const auto language = suffix.size() > 6 && suffix.substr(0, 5) == "(lang"
                ? parseIndex(suffix.substr(5, suffix.size() - 6)) : std::nullopt;
            if (suffix == "(strref)" || language) part = part.substr(0, loc);
        }
        const auto duplicate = part.rfind("[#");
        if (duplicate != std::string_view::npos && part.back() == ']' &&
            parseIndex(part.substr(duplicate + 2, part.size() - duplicate - 3)))
            part = part.substr(0, duplicate);
        return part;
    }

    static bool oneOf(std::string_view label, std::initializer_list<std::string_view> names) {
        for (const auto name : names) if (label == name) return true;
        return false;
    }

    static std::string text(const GffStruct* owner, std::string_view label) {
        const auto* field = owner ? owner->GetFieldByLabel(std::string(label)) : nullptr;
        if (!field) return {};
        auto value = field->GetString();
        const auto start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return {};
        return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
    }

    static int parameterBank(std::string_view label, bool action) {
        const std::string prefix = action ? "ActionParam" : "Param";
        if (label == prefix + "StrA") return 0;
        if (label == prefix + "StrB") return 1;
        for (int i = 1; i <= 5; ++i) {
            const auto base = prefix + std::to_string(i);
            if (label == base) return 0;
            if (label == base + "b") return 1;
        }
        return -1;
    }

    DlgParameterVisibility parameters(const GffStruct* owner, bool action) const {
        const std::string prefix = action ? "ActionParam" : "Param";
        std::array<bool, 2> retained{
            !text(owner, prefix + "StrA").empty(), !text(owner, prefix + "StrB").empty()};
        for (int column = 0; column < 2; ++column) {
            for (int i = 1; i <= 5; ++i) {
                const auto value = text(owner, prefix + std::to_string(i) + (column ? "b" : ""));
                retained[static_cast<std::size_t>(column)] |= !value.empty() && value != "0";
            }
        }
        return inspectorParameterVisibility(flavor_ == DlgFlavor::Kotor2, optional_,
            !text(owner, action ? "Script" : "Active").empty(),
            !text(owner, action ? "Script2" : "Active2").empty(), retained);
    }

    bool nodeVisible(DlgNodeRef ref, std::string_view label) const {
        const auto* node = document_.node(ref);
        if (!node) return true;
        if (label == "Comment") return true; // Designer notes are retained in every dialect.
        const bool jade = document_.dialect() == DlgDialect::JadeEmpire;
        const bool jadeEntryField = oneOf(label, {"SpeakerIndex", "ListenerIndex", "VoiceOver", "Skippable",
            "ScriptEntry", "ScriptCamEntry", "CameraEntry", "ScriptCamReplies", "CameraReplies", "AnimationList"});
        if (jade) {
            if (jadeEntryField) return ref.kind == DlgNodeKind::Entry;
            if (label == "Animation" || label == "Emotion") return ref.kind == DlgNodeKind::Reply;
            if (isK2NodeInspectorField(label) || oneOf(label, {"Speaker", "Listener", "VO_ResRef", "AnimList",
                "Quest", "QuestEntry", "PlotIndex", "PlotXPPercentage", "Sound", "Delay", "WaitFlags",
                "CameraAngle", "CameraID", "CamHeightOffset", "TarHeightOffset", "CamFieldOfView",
                "CameraAnimation", "CamVidEffect", "FadeType", "FadeColor", "FadeDelay", "FadeLength"})) return false;
            return true;
        }
        if (jadeEntryField || label == "Animation") return false;
        const int bank = parameterBank(label, true);
        if (bank >= 0) {
            const auto shown = parameters(node, true);
            return bank == 0 ? shown.first : shown.second;
        }
        if (label == "CameraID") return optional_ || text(node, "CameraAngle") == "6";
        if (label == "CamFieldOfView") return optional_ || text(node, "CamFieldOfView") != "-1";
        if (label == "QuestEntry") return optional_ || !text(node, "Quest").empty() || !text(node, label).empty();
        if (label == "Sound" && flavor_ == DlgFlavor::Kotor2) return optional_ || !text(node, label).empty();
        // Presence of any K2 marker anywhere was considered in flavor_. Do not
        // infer Entry-only status for the common action/quest/camera/fade fields.
        if (isK2NodeInspectorField(label))
            return flavor_ == DlgFlavor::Kotor2 || !text(node, label).empty();
        return true;
    }

    bool linkVisible(DlgLinkRef ref, std::string_view label) const {
        const auto* link = document_.link(ref);
        if (!link) return true; // Do not hide unknown or malformed structures.
        if (document_.dialect() == DlgDialect::JadeEmpire)
            return !isK2LinkInspectorField(label) && label != "DisplayInactive";
        if (label == "DesignerNumber" || label == "ReverseCond") return false;
        if (label == "DisplayInactive")
            return inspectorReplyChoiceLink(document_, document_.targetOf(ref), ref);
        const int bank = parameterBank(label, false);
        if (bank >= 0) {
            const auto shown = parameters(link, false);
            return bank == 0 ? shown.first : shown.second;
        }
        if (isK2LinkInspectorField(label))
            return flavor_ == DlgFlavor::Kotor2 || !text(link, label).empty();
        return true;
    }

    const DlgDocument& document_;
    DlgFlavor flavor_;
    bool optional_;
};

} // namespace neodlg
