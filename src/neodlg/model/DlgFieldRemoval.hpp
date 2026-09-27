#pragma once

#include "../../core/AppModel.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace neodlg {

// NeoDLG's explicit removal policy, not a general GFF schema rule. GFF V3
// serializes at most 16 label bytes, including the legacy shortened spelling
// of RecordNoVOOverride. Do not match substrings such as RecordNoVO or Comment.
inline bool isRetiredDlgField(std::string_view label) {
    label = label.substr(0, 16);
    for (const auto name : {"LinkComment", "IsChild", "PostProcNode", "RecordVO",
                            "RecordNoVOOverri"}) {
        if (label == name) return true;
    }
    return false;
}

// Iterative ownership-tree traversal: dialogue graph edges are stored indices,
// not pointers, and must not be followed or renumbered. Remove every duplicate
// field occurrence regardless of its value/type, retaining all other field and
// list ordering. Removing a named struct/list also removes its owned contents.
inline std::size_t removeRetiredDlgFields(GffStruct& root) {
    std::size_t removed = 0;
    std::vector<GffStruct*> pending{&root};
    while (!pending.empty()) {
        GffStruct* structure = pending.back();
        pending.pop_back();
        auto& fields = structure->allFields();
        const auto end = std::remove_if(fields.begin(), fields.end(), [&](const auto& field) {
            if (!field || !isRetiredDlgField(field->GetLabel())) return false;
            ++removed;
            return true;
        });
        fields.erase(end, fields.end());
        for (auto& field : fields) {
            if (auto* child = dynamic_cast<GffStruct*>(field.get())) {
                pending.push_back(child);
            } else if (auto* list = dynamic_cast<GffList*>(field.get())) {
                for (auto& entry : list->allStructs()) {
                    if (entry) pending.push_back(entry.get());
                }
            }
        }
    }
    return removed;
}

inline bool isDlgResource(const GffModel& model) {
    auto type = model.fileType();
    while (!type.empty() && (type.back() == ' ' || type.back() == '\0')) type.pop_back();
    return type == "DLG";
}

// Cleanup changes the working document, never its on-disk source. Keep it dirty
// until an explicit save succeeds so closing cannot silently discard cleanup.
inline std::size_t removeRetiredDlgFields(GffModel& model) {
    if (!model.loaded() || !isDlgResource(model) || !model.gff().root()) return 0;
    const auto removed = removeRetiredDlgFields(*model.gff().root());
    if (removed) model.gff().dirty(true);
    return removed;
}

inline void loadDlgModel(GffModel& model, const std::filesystem::path& path) {
    model.load(path);
    removeRetiredDlgFields(model);
}

inline void importDlgModelXml(GffModel& model, const std::string& xml) {
    model.importXml(xml);
    removeRetiredDlgFields(model);
}

inline void saveDlgModel(GffModel& model, const std::filesystem::path& path = {}) {
    // Also catches fields introduced through a host's raw GffModel pointer.
    removeRetiredDlgFields(model);
    model.save(path);
}

} // namespace neodlg
