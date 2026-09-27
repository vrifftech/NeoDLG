#pragma once

#include "DlgDocument.hpp"

#include <array>
#include <optional>
#include <string_view>

namespace neodlg {

// Inspector policy, not a schema validator. A K2 marker anywhere enables the
// K2 controls conservatively; absence is not proof that an optional field is
// invalid. This never changes DlgDocument::flavor(), serialization or defaults.
inline bool isK2NodeInspectorField(std::string_view label) {
    for (const auto name : {"Script2", "ActionParamStrA", "ActionParamStrB",
                            "NodeUnskippable", "AlienRaceNode", "Emotion", "FacialAnim"}) {
        if (label == name) return true;
    }
    for (int i = 1; i <= 5; ++i) {
        const std::string base = "ActionParam" + std::to_string(i);
        if (label == base || label == base + "b") return true;
    }
    return false;
}

inline bool isK2LinkInspectorField(std::string_view label) {
    for (const auto name : {"Active2", "ParamStrA", "ParamStrB", "Not", "Not2", "Logic"}) {
        if (label == name) return true;
    }
    for (int i = 1; i <= 5; ++i) {
        const std::string base = "Param" + std::to_string(i);
        if (label == base || label == base + "b") return true;
    }
    return false;
}

inline bool isRemovedSinglePanelField(std::string_view label) {
    // GFF3 labels are at most 16 bytes. Preserve the longer user-facing spelling
    // too; neither spelling is deleted or normalized by this display policy.
    for (const auto name : {"LinkComment", "IsChild", "PostProcNode", "RecordVO",
                            "RecordNoVOOverri", "RecordNoVOOverride"}) {
        if (label == name) return true;
    }
    return false;
}

inline bool hasK2LinkInspectorFields(const GffStruct* link) {
    if (!link) return false;
    for (const auto name : {"Active2", "ParamStrA", "ParamStrB", "Not", "Not2", "Logic"}) {
        if (link->GetFieldByLabel(name)) return true;
    }
    for (int i = 1; i <= 5; ++i) {
        const std::string base = "Param" + std::to_string(i);
        if (link->GetFieldByLabel(base) || link->GetFieldByLabel(base + "b")) return true;
    }
    return false;
}

inline bool hasK2NodeInspectorFields(const GffStruct* node) {
    if (!node) return false;
    for (const auto name : {"Script2", "ActionParamStrA", "ActionParamStrB",
                            "NodeUnskippable", "AlienRaceNode", "Emotion", "FacialAnim"}) {
        if (node->GetFieldByLabel(name)) return true;
    }
    for (int i = 1; i <= 5; ++i) {
        const std::string base = "ActionParam" + std::to_string(i);
        if (node->GetFieldByLabel(base) || node->GetFieldByLabel(base + "b")) return true;
    }
    return false;
}

inline DlgFlavor singlePanelFieldFlavor(const DlgDocument& document) {
    if (document.dialect() == DlgDialect::JadeEmpire) return DlgFlavor::JadeEmpire;
    if (document.flavor() == DlgFlavor::Kotor2) return DlgFlavor::Kotor2;
    const auto linkListHasK2 = [](const GffStruct* owner, const char* label) {
        const auto* list = owner
            ? dynamic_cast<const GffList*>(owner->GetFieldByLabel(label)) : nullptr;
        if (!list) return false;
        for (std::size_t i = 0; i < list->count(); ++i) {
            if (hasK2LinkInspectorFields(list->GetStruct(i))) return true;
        }
        return false;
    };
    if (linkListHasK2(document.root(), "StartingList")) return DlgFlavor::Kotor2;
    // Visit stored lists, not graph edges. Unreachable nodes, later replies,
    // cycles and even links with invalid target indices are handled once each.
    for (const auto kind : {DlgNodeKind::Entry, DlgNodeKind::Reply}) {
        const auto* list = document.nodeList(kind);
        if (!list) continue;
        for (std::size_t i = 0; i < list->count(); ++i) {
            const auto* node = list->GetStruct(i);
            if (hasK2NodeInspectorFields(node) ||
                linkListHasK2(node, kind == DlgNodeKind::Entry ? "RepliesList" : "EntriesList")) {
                return DlgFlavor::Kotor2;
            }
        }
    }
    return DlgFlavor::Kotor;
}

struct DlgParameterVisibility {
    bool first = false;
    bool second = false;
};

inline DlgParameterVisibility singlePanelParameterVisibility(
    bool k2Controls, bool showOptional, bool firstScript, bool secondScript,
    std::array<bool, 2> retainedOrPending) {
    // A first script is valid in K1 and does not imply parameter support. A
    // second script, existing unusual values or pending edits must stay usable
    // even before an inferred K1 document is saved with its K2 markers.
    return {(k2Controls && (showOptional || firstScript)) || retainedOrPending[0],
            (k2Controls && showOptional) || secondScript || retainedOrPending[1]};
}

inline bool singlePanelReplyChoiceLink(const DlgDocument& document,
                                      std::optional<DlgNodeRef> node,
                                      std::optional<DlgLinkRef> link) {
    if (document.dialect() != DlgDialect::Kotor || !node || !link ||
        node->kind != DlgNodeKind::Reply || link->owner != DlgLinkOwner::Entry ||
        !document.node(*node) || !document.link(*link)) return false;
    const auto target = document.targetOf(*link);
    return target && *target == *node;
}

} // namespace neodlg
