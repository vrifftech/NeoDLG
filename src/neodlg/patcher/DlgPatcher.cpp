#include "DlgPatcher.hpp"

#include "core/GffTypeNames.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace neodlg::patcher {
namespace {

using neotsl::IniSection;
using neotsl::PatchProject;
using neotsl::StagedAsset;

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string trim(std::string value) {
    const auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string appendPath(const std::string& parent, const std::string& child) {
    if (parent.empty()) return child;
    if (child.empty()) return parent;
    return parent + "\\" + child;
}

std::string normalizePatchDestination(std::string value) {
    value = trim(std::move(value));
    if (value.empty() || lowerAscii(value) == "override") return "override";

    std::replace(value.begin(), value.end(), '/', '\\');
    while (value.rfind(".\\", 0) == 0) value.erase(0, 2);
    if (value.empty()) return "override";
    if (value.front() == '\\' || (value.size() >= 2 && value[1] == ':')) {
        throw std::runtime_error(
            "The patch destination must be relative to the game directory: " + value);
    }

    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t end = value.find('\\', start);
        const std::string component = value.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        if (component == "..") {
            throw std::runtime_error(
                "The patch destination cannot leave the game directory: " + value);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return value;
}

std::string nextKey(const IniSection& section, const std::string& prefix) {
    std::size_t next = 0;
    const std::string wanted = lowerAscii(prefix);
    for (const auto& entry : section.entries) {
        const std::string key = lowerAscii(entry.key);
        if (key.rfind(wanted, 0) != 0) continue;
        const std::string suffix = key.substr(wanted.size());
        if (suffix.empty()) continue;

        std::size_t value = 0;
        bool numeric = true;
        for (const char ch : suffix) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) {
                numeric = false;
                break;
            }
            const std::size_t digit = static_cast<std::size_t>(ch - '0');
            if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10u) {
                numeric = false;
                break;
            }
            value = value * 10u + digit;
        }
        if (numeric && value >= next) next = value + 1u;
    }
    return prefix + std::to_string(next);
}

std::string uniqueSectionName(const PatchProject& project, const std::string& base) {
    std::string clean = neotsl::sanitizeSectionName(base);
    if (clean.empty()) clean = "field";
    for (std::size_t index = 0;; ++index) {
        const std::string candidate = clean + "_" + std::to_string(index);
        if (!project.findSection(candidate)) return candidate;
    }
}

bool isDlgType(const GffFile& file) {
    return lowerAscii(trim(file.filetype())) == "dlg";
}

const GffField* fieldByLabel(const GffStruct& structure, const std::string& label) {
    for (std::size_t index = 0; index < structure.count(); ++index) {
        const GffField* field = structure.GetField(index);
        if (field && field->GetLabel() == label) return field;
    }
    return nullptr;
}

const GffList* listByLabel(const GffStruct& structure, const std::string& label) {
    const GffField* field = fieldByLabel(structure, label);
    if (!field || field->fieldtype != FIELD_TYPE_LIST) return nullptr;
    return dynamic_cast<const GffList*>(field);
}

const GffList* rootList(const GffFile& file, const std::string& label) {
    const GffStruct* root = file.root();
    return root ? listByLabel(*root, label) : nullptr;
}

bool isJadeDlg(const GffFile& file) {
    const GffStruct* root = file.root();
    if (!root) return false;
    if (fieldByLabel(*root, "TagList") && !fieldByLabel(*root, "DelayEntry")) return true;

    std::vector<const GffStruct*> pending{root};
    while (!pending.empty()) {
        const GffStruct* structure = pending.back();
        pending.pop_back();
        for (std::size_t index = 0; index < structure->count(); ++index) {
            const GffField* field = structure->GetField(index);
            if (!field) continue;
            if (field->fieldtype == FIELD_TYPE_JADE_STRREF) return true;
            if (field->fieldtype == FIELD_TYPE_STRUCT) {
                pending.push_back(&dynamic_cast<const GffStruct&>(*field));
            } else if (field->fieldtype == FIELD_TYPE_LIST) {
                const auto& list = dynamic_cast<const GffList&>(*field);
                for (std::size_t item = 0; item < list.count(); ++item) {
                    if (const GffStruct* child = list.GetStruct(item)) pending.push_back(child);
                }
            }
        }
    }
    return false;
}

std::string strRefText(std::uint32_t value) {
    return value == 0xFFFFFFFFu ? std::string("-1") : std::to_string(value);
}

std::string encodeLocalizedText(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (ch == '\r') {
            if (index + 1u < value.size() && value[index + 1u] == '\n') ++index;
            result += "<#LF#>";
        } else if (ch == '\n') {
            result += "<#LF#>";
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

bool containsLineBreak(const std::string& value) {
    return value.find('\r') != std::string::npos || value.find('\n') != std::string::npos;
}

bool localizedStringsEqual(const GffLocalizedStringField& lhs,
                           const GffLocalizedStringField& rhs) {
    if (lhs.substrings.size() != rhs.substrings.size()) return false;
    std::map<std::int32_t, std::string> left;
    std::map<std::int32_t, std::string> right;
    for (const auto& value : lhs.substrings) left[value.stringid] = value.GetString();
    for (const auto& value : rhs.substrings) right[value.stringid] = value.GetString();
    return left == right;
}

bool fieldDeepEqual(const GffField& lhs, const GffField& rhs);

bool structDeepEqual(const GffStruct& lhs, const GffStruct& rhs) {
    if (lhs.typeid_ != rhs.typeid_ || lhs.count() != rhs.count()) return false;
    for (std::size_t index = 0; index < lhs.count(); ++index) {
        const GffField* left = lhs.GetField(index);
        const GffField* right = rhs.GetField(index);
        if ((left == nullptr) != (right == nullptr)) return false;
        if (left && right && !fieldDeepEqual(*left, *right)) return false;
    }
    return true;
}

bool listDeepEqual(const GffList& lhs, const GffList& rhs) {
    if (lhs.count() != rhs.count()) return false;
    for (std::size_t index = 0; index < lhs.count(); ++index) {
        const GffStruct* left = lhs.GetStruct(index);
        const GffStruct* right = rhs.GetStruct(index);
        if ((left == nullptr) != (right == nullptr)) return false;
        if (left && right && !structDeepEqual(*left, *right)) return false;
    }
    return true;
}

bool fieldDeepEqual(const GffField& lhs, const GffField& rhs) {
    if (lhs.fieldtype != rhs.fieldtype || lhs.GetLabel() != rhs.GetLabel()) return false;
    switch (lhs.fieldtype) {
    case FIELD_TYPE_STRUCT:
        return structDeepEqual(dynamic_cast<const GffStruct&>(lhs),
                               dynamic_cast<const GffStruct&>(rhs));
    case FIELD_TYPE_LIST:
        return listDeepEqual(dynamic_cast<const GffList&>(lhs),
                             dynamic_cast<const GffList&>(rhs));
    case FIELD_TYPE_CEXOLOCSTRING: {
        const auto& left = dynamic_cast<const GffLocalizedStringField&>(lhs);
        const auto& right = dynamic_cast<const GffLocalizedStringField&>(rhs);
        return left.strref == right.strref && localizedStringsEqual(left, right);
    }
    default:
        return lhs.GetString() == rhs.GetString();
    }
}

bool hasDuplicateLabels(const GffStruct& structure) {
    std::set<std::string> labels;
    for (std::size_t index = 0; index < structure.count(); ++index) {
        const GffField* field = structure.GetField(index);
        if (field && !labels.insert(field->GetLabel()).second) return true;
    }
    return false;
}

std::vector<std::uint8_t> serializeGff(GffFile& file) {
    const bool wasDirty = file.dirty();
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::filesystem::path temporary = std::filesystem::temp_directory_path() /
        ("neodlg-patcher-" + std::to_string(stamp) + ".dlg");

    for (unsigned attempt = 0; std::filesystem::exists(temporary) && attempt < 100u; ++attempt) {
        temporary = std::filesystem::temp_directory_path() /
            ("neodlg-patcher-" + std::to_string(stamp) + "-" +
             std::to_string(attempt + 1u) + ".dlg");
    }

    try {
        file.SaveFile(temporary);
        file.dirty(wasDirty);

        std::ifstream input(temporary, std::ios::binary);
        if (!input) {
            throw std::runtime_error(
                "Unable to read the generated DLG package asset: " + temporary.string());
        }
        std::vector<std::uint8_t> data(
            (std::istreambuf_iterator<char>(input)),
            std::istreambuf_iterator<char>());
        if (!input.eof() && input.fail()) {
            throw std::runtime_error(
                "Unable to finish reading the generated DLG package asset: " + temporary.string());
        }

        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return data;
    } catch (...) {
        file.dirty(wasDirty);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

PatchProject makeWholeFileReplacement(GffFile& modified,
                                      const std::string& patchFilename,
                                      const std::string& destination) {
    PatchProject project;
    project.add("Settings", "FileExists", "1");
    project.add("Settings", "InstallerMode", "1");

    const std::string installSection = "NeoDLGFiles";
    project.add("InstallList", installSection, normalizePatchDestination(destination));
    project.add(installSection, "Replace0", patchFilename);
    project.add(patchFilename, "!SourceFile", patchFilename);
    project.add(patchFilename, "!SaveAs", patchFilename);

    StagedAsset asset;
    asset.targetName = patchFilename;
    asset.data = serializeGff(modified);
    project.assets.push_back(std::move(asset));
    return project;
}

std::string patchFieldType(const GffField& field) {
    switch (field.fieldtype) {
    case FIELD_TYPE_BYTE: return "Byte";
    case FIELD_TYPE_CHAR: return "Char";
    case FIELD_TYPE_WORD: return "Word";
    case FIELD_TYPE_SHORT: return "Short";
    case FIELD_TYPE_DWORD: return "DWORD";
    case FIELD_TYPE_DWORD64: return "DWORD64";
    case FIELD_TYPE_INT: return "Int";
    case FIELD_TYPE_INT64: return "Int64";
    case FIELD_TYPE_FLOAT: return "Float";
    case FIELD_TYPE_DOUBLE: return "Double";
    case FIELD_TYPE_CEXOSTRING: return "ExoString";
    case FIELD_TYPE_RESREF: return "ResRef";
    case FIELD_TYPE_CEXOLOCSTRING: return "ExoLocString";
    case FIELD_TYPE_STRUCT: return "Struct";
    case FIELD_TYPE_LIST: return "List";
    case FIELD_TYPE_ORIENTATION: return "Orientation";
    case FIELD_TYPE_POSITION: return "Position";
    default:
        throw std::runtime_error(
            "TSLPatcher cannot add GFF field type " + fieldTypeName(field.fieldtype) + ".");
    }
}

bool isScalarPatchable(const GffField& field) {
    return field.fieldtype != FIELD_TYPE_STRUCT &&
           field.fieldtype != FIELD_TYPE_LIST &&
           field.fieldtype != FIELD_TYPE_CEXOLOCSTRING &&
           field.fieldtype != FIELD_TYPE_VOID &&
           field.fieldtype != FIELD_TYPE_JADE_STRREF;
}

bool parseSizeTStrict(const std::string& text, std::size_t& out) {
    if (text.empty()) return false;
    std::size_t value = 0;
    for (const char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        const std::size_t digit = static_cast<std::size_t>(ch - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

std::optional<std::size_t> structIndexValue(const GffStruct& structure) {
    const GffField* indexField = fieldByLabel(structure, "Index");
    if (!indexField) return std::nullopt;
    std::string text = trim(indexField->GetString());
    if (!text.empty() && text.front() == '+') text.erase(text.begin());
    std::size_t value = 0;
    if (!parseSizeTStrict(text, value)) return std::nullopt;
    return value;
}

std::string targetRootForLinkList(const std::string& listLabel) {
    if (listLabel == "RepliesList") return "ReplyList";
    if (listLabel == "EntriesList" || listLabel == "StartingList") return "EntryList";
    return {};
}

bool isLinkList(const std::string& listLabel) {
    return listLabel == "RepliesList" ||
           listLabel == "EntriesList" ||
           listLabel == "StartingList";
}

struct DeferredAssignment {
    std::string pathToken;
    std::string valueToken;
};

struct EmitContext {
    PatchProject& project;
    std::string fileSection;
    std::size_t originalEntryCount = 0;
    std::size_t originalReplyCount = 0;
    std::map<std::string, std::string> rootIndexTokens;
    std::vector<DeferredAssignment> deferredAssignments;
    int nextToken = 1;
    bool changed = false;
};

std::string allocateToken(EmitContext& context) {
    return "2DAMEMORY" + std::to_string(context.nextToken++);
}

std::string rootTokenKey(const std::string& listName, std::size_t index) {
    return listName + ":" + std::to_string(index);
}

std::optional<std::string> tokenForRootTarget(const EmitContext& context,
                                              const std::string& rootList,
                                              std::size_t index) {
    const auto found = context.rootIndexTokens.find(rootTokenKey(rootList, index));
    if (found == context.rootIndexTokens.end()) return std::nullopt;
    return found->second;
}

void addNumberedEntry(PatchProject& project,
                      const std::string& sectionName,
                      const std::string& prefix,
                      const std::string& value) {
    IniSection& section = project.section(sectionName);
    section.entries.push_back({nextKey(section, prefix), value});
}

void addPlainEntry(PatchProject& project,
                   const std::string& sectionName,
                   const std::string& key,
                   const std::string& value) {
    project.section(sectionName).entries.push_back({key, value});
}

std::string addFieldSection(EmitContext& context,
                            const std::string& parentSection,
                            const std::string& baseName,
                            const GffField& field,
                            const std::optional<std::string>& path,
                            const std::string& label,
                            const std::optional<std::string>& typeId,
                            const std::optional<std::string>& value,
                            const std::optional<std::string>& listIndexToken,
                            const std::optional<std::string>& fieldPathToken) {
    const std::string sectionName = uniqueSectionName(context.project, baseName);
    addNumberedEntry(context.project, parentSection, "AddField", sectionName);
    context.project.add(sectionName, "FieldType", patchFieldType(field));
    if (path) context.project.add(sectionName, "Path", *path);
    context.project.add(sectionName, "Label", label);
    if (typeId) context.project.add(sectionName, "TypeId", *typeId);
    if (value) context.project.add(sectionName, "Value", *value);
    if (listIndexToken) context.project.add(sectionName, *listIndexToken, "ListIndex");
    if (fieldPathToken) context.project.add(sectionName, *fieldPathToken, "!FieldPath");
    context.changed = true;
    return sectionName;
}

std::string addLocStringSection(EmitContext& context,
                                const std::string& parentSection,
                                const std::string& baseName,
                                const GffLocalizedStringField& loc,
                                const std::optional<std::string>& path,
                                const std::string& label) {
    const std::string sectionName = uniqueSectionName(context.project, baseName);
    addNumberedEntry(context.project, parentSection, "AddField", sectionName);
    context.project.add(sectionName, "FieldType", "ExoLocString");
    if (path) context.project.add(sectionName, "Path", *path);
    context.project.add(sectionName, "Label", label);
    context.project.add(sectionName, "StrRef", strRefText(loc.strref));
    for (const auto& sub : loc.substrings) {
        context.project.add(
            sectionName,
            "lang" + std::to_string(sub.stringid),
            encodeLocalizedText(sub.GetString()));
    }
    context.changed = true;
    return sectionName;
}

struct StructEmitOptions {
    std::string listLabel;
    std::optional<std::string> path;
    std::optional<std::string> listIndexToken;
    std::string targetRootList;
    std::string baseName;
    bool listElement = false;
};

void emitField(EmitContext& context,
               const std::string& parentSection,
               const GffField& field,
               const std::optional<std::string>& path,
               const std::string& baseName,
               const std::optional<std::string>& forcedValue = std::nullopt,
               const std::optional<std::string>& fieldPathToken = std::nullopt);

void emitStruct(EmitContext& context,
                const std::string& parentSection,
                const GffStruct& structure,
                StructEmitOptions options) {
    std::optional<std::string> typeId;
    if (options.listElement) typeId = "ListIndex";
    else typeId = std::to_string(structure.typeid_);

    std::optional<std::string> dynamicTargetToken;
    if (!options.targetRootList.empty()) {
        const auto targetIndex = structIndexValue(structure);
        if (!targetIndex) {
            context.project.unsupported.push_back(
                "A link added to " + options.listLabel + " has no numeric Index field.");
        } else {
            dynamicTargetToken = tokenForRootTarget(
                context, options.targetRootList, *targetIndex);
            if (!dynamicTargetToken &&
                ((options.targetRootList == "EntryList" &&
                  *targetIndex >= context.originalEntryCount) ||
                 (options.targetRootList == "ReplyList" &&
                  *targetIndex >= context.originalReplyCount))) {
                context.project.unsupported.push_back(
                    "A new link references an untracked new " + options.targetRootList +
                    " node at local index " + std::to_string(*targetIndex) + ".");
            }
        }
    }

    const std::string sectionName = addFieldSection(
        context,
        parentSection,
        options.baseName,
        structure,
        options.path,
        structure.GetLabel(),
        typeId,
        std::nullopt,
        options.listIndexToken,
        std::nullopt);

    for (std::size_t index = 0; index < structure.count(); ++index) {
        const GffField* child = structure.GetField(index);
        if (!child) continue;

        if (dynamicTargetToken && child->GetLabel() == "Index" && isScalarPatchable(*child)) {
            const std::string pathToken = allocateToken(context);
            emitField(
                context,
                sectionName,
                *child,
                std::nullopt,
                options.baseName + "_Index",
                std::string("0"),
                pathToken);
            context.deferredAssignments.push_back({pathToken, *dynamicTargetToken});
        } else {
            emitField(
                context,
                sectionName,
                *child,
                std::nullopt,
                options.baseName + "_" + child->GetLabel());
        }
    }
}

void emitList(EmitContext& context,
              const std::string& parentSection,
              const GffList& list,
              const std::optional<std::string>& path,
              const std::string& baseName) {
    if (list.gff4CompactPrimitiveList) {
        context.project.unsupported.push_back(
            "TSLPatcher cannot add a compact GFF4 list: " + list.GetLabel());
        return;
    }

    const std::string sectionName = addFieldSection(
        context,
        parentSection,
        baseName,
        list,
        path,
        list.GetLabel(),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt);

    const std::string targetRoot = targetRootForLinkList(list.GetLabel());
    for (std::size_t index = 0; index < list.count(); ++index) {
        const GffStruct* structure = list.GetStruct(index);
        if (!structure) {
            context.project.unsupported.push_back(
                "A newly added list contains an empty structure slot: " + list.GetLabel());
            continue;
        }
        emitStruct(
            context,
            sectionName,
            *structure,
            StructEmitOptions{
                list.GetLabel(),
                std::nullopt,
                std::nullopt,
                targetRoot,
                baseName + "_" + list.GetLabel() + "_" + std::to_string(index),
                true});
    }
}

void emitField(EmitContext& context,
               const std::string& parentSection,
               const GffField& field,
               const std::optional<std::string>& path,
               const std::string& baseName,
               const std::optional<std::string>& forcedValue,
               const std::optional<std::string>& fieldPathToken) {
    try {
        switch (field.fieldtype) {
        case FIELD_TYPE_STRUCT:
            emitStruct(
                context,
                parentSection,
                dynamic_cast<const GffStruct&>(field),
                StructEmitOptions{
                    {}, path, std::nullopt, {}, baseName, false});
            break;
        case FIELD_TYPE_LIST:
            emitList(
                context,
                parentSection,
                dynamic_cast<const GffList&>(field),
                path,
                baseName);
            break;
        case FIELD_TYPE_CEXOLOCSTRING:
            addLocStringSection(
                context,
                parentSection,
                baseName,
                dynamic_cast<const GffLocalizedStringField&>(field),
                path,
                field.GetLabel());
            break;
        case FIELD_TYPE_VOID:
        case FIELD_TYPE_JADE_STRREF:
            context.project.unsupported.push_back(
                "TSLPatcher cannot add field " + appendPath(path.value_or(""), field.GetLabel()) +
                " with type " + fieldTypeName(field.fieldtype) + ".");
            break;
        default: {
            const std::string value = forcedValue.value_or(field.GetString());
            if (containsLineBreak(value)) {
                context.project.unsupported.push_back(
                    "A non-localized GFF value contains a line break: " +
                    appendPath(path.value_or(""), field.GetLabel()));
                break;
            }
            addFieldSection(
                context,
                parentSection,
                baseName,
                field,
                path,
                field.GetLabel(),
                std::nullopt,
                value,
                std::nullopt,
                fieldPathToken);
            break;
        }
        }
    } catch (const std::exception& error) {
        context.project.unsupported.push_back(error.what());
    }
}

void emitDirectScalarModification(EmitContext& context,
                                  const std::string& path,
                                  const GffField& modified) {
    if (!isScalarPatchable(modified)) {
        context.project.unsupported.push_back(
            "TSLPatcher cannot modify field " + path + " with type " +
            fieldTypeName(modified.fieldtype) + ".");
        return;
    }
    const std::string value = modified.GetString();
    if (containsLineBreak(value)) {
        context.project.unsupported.push_back(
            "A non-localized GFF value contains a line break: " + path);
        return;
    }
    addPlainEntry(context.project, context.fileSection, path, value);
    context.changed = true;
}

void emitDirectLocStringModifications(EmitContext& context,
                                      const std::string& path,
                                      const GffLocalizedStringField& original,
                                      const GffLocalizedStringField& modified) {
    if (original.strref != modified.strref) {
        addPlainEntry(
            context.project,
            context.fileSection,
            path + "(strref)",
            strRefText(modified.strref));
        context.changed = true;
    }

    std::map<std::int32_t, std::string> originalStrings;
    std::map<std::int32_t, std::string> modifiedStrings;
    for (const auto& sub : original.substrings) originalStrings[sub.stringid] = sub.GetString();
    for (const auto& sub : modified.substrings) modifiedStrings[sub.stringid] = sub.GetString();

    for (const auto& item : originalStrings) {
        if (modifiedStrings.find(item.first) == modifiedStrings.end()) {
            context.project.unsupported.push_back(
                "TSLPatcher cannot delete localized text " + path +
                " lang" + std::to_string(item.first) + ".");
        }
    }
    for (const auto& item : modifiedStrings) {
        const auto found = originalStrings.find(item.first);
        if (found == originalStrings.end() || found->second != item.second) {
            addPlainEntry(
                context.project,
                context.fileSection,
                path + "(lang" + std::to_string(item.first) + ")",
                encodeLocalizedText(item.second));
            context.changed = true;
        }
    }
}

void compareStructs(EmitContext& context,
                    const GffStruct& original,
                    const GffStruct& modified,
                    const std::string& path,
                    const std::string& dynamicLinkTargetRoot = {});

void compareLists(EmitContext& context,
                  const GffList& original,
                  const GffList& modified,
                  const std::string& path) {
    const std::string listLabel = modified.GetLabel();

    if (modified.count() < original.count()) {
        context.project.unsupported.push_back(
            "TSLPatcher cannot delete list structures from " + path + ".");
    }

    const std::size_t common = std::min(original.count(), modified.count());
    const std::string targetRoot = isLinkList(listLabel)
        ? targetRootForLinkList(listLabel)
        : std::string{};

    for (std::size_t index = 0; index < common; ++index) {
        const GffStruct* before = original.GetStruct(index);
        const GffStruct* after = modified.GetStruct(index);
        if (!before || !after) {
            if (before != after) {
                context.project.unsupported.push_back(
                    "A list structure slot changed between empty and populated at " +
                    appendPath(path, std::to_string(index)) + ".");
            }
            continue;
        }
        compareStructs(
            context,
            *before,
            *after,
            appendPath(path, std::to_string(index)),
            targetRoot);
    }

    // EntryList and ReplyList additions are emitted before all other changes so
    // their runtime-assigned indexes are available to every link token.
    if (listLabel == "EntryList" || listLabel == "ReplyList") return;

    for (std::size_t index = original.count(); index < modified.count(); ++index) {
        const GffStruct* structure = modified.GetStruct(index);
        if (!structure) {
            context.project.unsupported.push_back(
                "A newly added list structure is empty at " +
                appendPath(path, std::to_string(index)) + ".");
            continue;
        }
        emitStruct(
            context,
            context.fileSection,
            *structure,
            StructEmitOptions{
                listLabel,
                path,
                std::nullopt,
                targetRoot,
                "neodlg_add_" + neotsl::sanitizeSectionName(path) + "_" +
                    std::to_string(index),
                true});
    }
}

void compareStructs(EmitContext& context,
                    const GffStruct& original,
                    const GffStruct& modified,
                    const std::string& path,
                    const std::string& dynamicLinkTargetRoot) {
    if (original.typeid_ != modified.typeid_) {
        context.project.unsupported.push_back(
            "TSLPatcher cannot change a structure TypeId at " +
            (path.empty() ? std::string("root") : path) + ".");
        return;
    }
    if (hasDuplicateLabels(original) || hasDuplicateLabels(modified)) {
        if (!structDeepEqual(original, modified)) {
            context.project.unsupported.push_back(
                "A structure with duplicate field labels changed at " +
                (path.empty() ? std::string("root") : path) + ".");
        }
        return;
    }

    std::map<std::string, const GffField*> originalFields;
    for (std::size_t index = 0; index < original.count(); ++index) {
        if (const GffField* field = original.GetField(index)) {
            originalFields[field->GetLabel()] = field;
        }
    }

    std::set<std::string> seen;
    for (std::size_t index = 0; index < modified.count(); ++index) {
        const GffField* after = modified.GetField(index);
        if (!after) continue;
        const std::string label = after->GetLabel();
        seen.insert(label);
        const std::string childPath = appendPath(path, label);

        const auto found = originalFields.find(label);
        if (found == originalFields.end()) {
            emitField(
                context,
                context.fileSection,
                *after,
                path,
                "neodlg_add_" + neotsl::sanitizeSectionName(childPath));
            continue;
        }

        const GffField* before = found->second;
        if (!before || before->fieldtype != after->fieldtype) {
            context.project.unsupported.push_back(
                "TSLPatcher cannot change the GFF field type at " + childPath + ".");
            continue;
        }

        switch (after->fieldtype) {
        case FIELD_TYPE_STRUCT:
            compareStructs(
                context,
                dynamic_cast<const GffStruct&>(*before),
                dynamic_cast<const GffStruct&>(*after),
                childPath);
            break;
        case FIELD_TYPE_LIST:
            compareLists(
                context,
                dynamic_cast<const GffList&>(*before),
                dynamic_cast<const GffList&>(*after),
                childPath);
            break;
        case FIELD_TYPE_CEXOLOCSTRING:
            emitDirectLocStringModifications(
                context,
                childPath,
                dynamic_cast<const GffLocalizedStringField&>(*before),
                dynamic_cast<const GffLocalizedStringField&>(*after));
            break;
        default:
            if (!dynamicLinkTargetRoot.empty() && label == "Index") {
                const auto targetIndex = structIndexValue(modified);
                const auto token = targetIndex
                    ? tokenForRootTarget(context, dynamicLinkTargetRoot, *targetIndex)
                    : std::nullopt;
                if (token) {
                    if (before->GetString() != *token) {
                        addPlainEntry(context.project, context.fileSection, childPath, *token);
                        context.changed = true;
                    }
                } else if (before->GetString() != after->GetString()) {
                    emitDirectScalarModification(context, childPath, *after);
                }
            } else if (before->GetString() != after->GetString()) {
                emitDirectScalarModification(context, childPath, *after);
            }
            break;
        }
    }

    for (const auto& originalField : originalFields) {
        if (!seen.count(originalField.first)) {
            context.project.unsupported.push_back(
                "TSLPatcher cannot delete GFF field " +
                appendPath(path, originalField.first) + ".");
        }
    }
}

bool sameExistingRootNode(const GffStruct& original, const GffStruct& modified) {
    if (original.typeid_ != modified.typeid_) return false;
    const GffField* originalNodeId = fieldByLabel(original, "NodeID");
    const GffField* modifiedNodeId = fieldByLabel(modified, "NodeID");
    if (originalNodeId && modifiedNodeId) {
        return originalNodeId->GetString() == modifiedNodeId->GetString();
    }
    return true;
}

void verifyRootPrefix(EmitContext& context,
                      const std::string& listName,
                      const GffList* original,
                      const GffList* modified) {
    if (!original || !modified) {
        context.project.unsupported.push_back(
            "The DLG is missing required root list " + listName + ".");
        return;
    }
    if (modified->count() < original->count()) {
        context.project.unsupported.push_back(
            "TSLPatcher cannot delete nodes from " + listName + ".");
        return;
    }
    for (std::size_t index = 0; index < original->count(); ++index) {
        const GffStruct* before = original->GetStruct(index);
        const GffStruct* after = modified->GetStruct(index);
        if (!before || !after || !sameExistingRootNode(*before, *after)) {
            context.project.unsupported.push_back(
                listName + " was inserted into, removed from, or reordered before index " +
                std::to_string(index) + ". Dynamic merge export requires new root nodes to be appended.");
            return;
        }
    }
}

void allocateRootTokens(EmitContext& context,
                        const std::string& listName,
                        const GffList* original,
                        const GffList* modified) {
    if (!modified) return;
    const std::size_t originalCount = original ? original->count() : 0u;
    for (std::size_t index = originalCount; index < modified->count(); ++index) {
        context.rootIndexTokens[rootTokenKey(listName, index)] = allocateToken(context);
    }
}

void emitNewRootNodes(EmitContext& context,
                      const std::string& listName,
                      const GffList* original,
                      const GffList* modified) {
    if (!modified) return;
    const std::size_t originalCount = original ? original->count() : 0u;
    for (std::size_t index = originalCount; index < modified->count(); ++index) {
        const GffStruct* structure = modified->GetStruct(index);
        if (!structure) {
            context.project.unsupported.push_back(
                "A new " + listName + " node is empty at local index " +
                std::to_string(index) + ".");
            continue;
        }
        const auto token = tokenForRootTarget(context, listName, index);
        emitStruct(
            context,
            context.fileSection,
            *structure,
            StructEmitOptions{
                listName,
                listName,
                token,
                {},
                "neodlg_new_" + listName + "_" + std::to_string(index),
                true});
    }
}

void appendDeferredAssignments(EmitContext& context) {
    for (const auto& assignment : context.deferredAssignments) {
        addPlainEntry(
            context.project,
            context.fileSection,
            assignment.pathToken,
            assignment.valueToken);
        context.changed = true;
    }
}

PatchProject makeDynamicMergeProject(const GffFile& original,
                                     const GffFile& modified,
                                     const std::string& patchFilename,
                                     bool packageOutput,
                                     const std::filesystem::path& baselineAsset,
                                     const std::string& destination) {
    PatchProject project;
    project.add("GFFList", "File0", patchFilename);
    project.add(patchFilename, "!Destination", normalizePatchDestination(destination));
    if (packageOutput) {
        project.add(patchFilename, "!SourceFile", patchFilename);
        project.add(patchFilename, "!SaveAs", patchFilename);
        project.assets.push_back(StagedAsset{baselineAsset, patchFilename, {}});
    }

    EmitContext context{project, patchFilename, 0u, 0u, {}, {}, 1, false};
    const GffList* originalEntries = rootList(original, "EntryList");
    const GffList* modifiedEntries = rootList(modified, "EntryList");
    const GffList* originalReplies = rootList(original, "ReplyList");
    const GffList* modifiedReplies = rootList(modified, "ReplyList");

    context.originalEntryCount = originalEntries ? originalEntries->count() : 0u;
    context.originalReplyCount = originalReplies ? originalReplies->count() : 0u;

    verifyRootPrefix(context, "EntryList", originalEntries, modifiedEntries);
    verifyRootPrefix(context, "ReplyList", originalReplies, modifiedReplies);

    allocateRootTokens(context, "EntryList", originalEntries, modifiedEntries);
    allocateRootTokens(context, "ReplyList", originalReplies, modifiedReplies);

    // Node tokens must be created before links are compared or emitted.
    emitNewRootNodes(context, "EntryList", originalEntries, modifiedEntries);
    emitNewRootNodes(context, "ReplyList", originalReplies, modifiedReplies);

    compareStructs(context, *original.root(), *modified.root(), {});
    appendDeferredAssignments(context);

    if (!context.changed && project.unsupported.empty()) {
        throw std::runtime_error(
            "The original and modified DLG files contain no patchable differences.");
    }
    return project;
}

} // namespace

PatchProject makeCompleteDlgReplacement(GffFile& modified,
                                               const std::string& patchFilename,
                                               const std::string& destination) {
    if (!modified.root()) {
        throw std::runtime_error(
            "Complete-DLG replacement requires a loaded modified DLG file.");
    }
    if (!isDlgType(modified)) {
        throw std::runtime_error(
            "Complete-DLG replacement requires a native DLG file.");
    }
    if (modified.isGff4()) {
        throw std::runtime_error(
            "TSLPatcher/HoloPatcher output supports classic GFF DLG files only.");
    }
    if (isJadeDlg(modified)) {
        throw std::runtime_error(
            "The TSLPatcher format targets KotOR and KotOR II. Jade Empire DLG files must be distributed separately.");
    }
    if (patchFilename.empty() ||
        std::filesystem::path(patchFilename).filename() !=
            std::filesystem::path(patchFilename)) {
        throw std::runtime_error(
            "The patch target must be a filename without directory components.");
    }
    return makeWholeFileReplacement(modified, patchFilename, destination);
}

PatchProject diffDlgPatcher(const GffFile& original,
                            GffFile& modified,
                            const std::string& patchFilename,
                            DlgPatchMode mode,
                            bool packageOutput,
                            const std::filesystem::path& baselineAsset,
                            const std::string& destination) {
    if (!original.root() || !modified.root()) {
        throw std::runtime_error(
            "DLG patcher generation requires loaded original and modified DLG files.");
    }
    if (!isDlgType(original) || !isDlgType(modified)) {
        throw std::runtime_error(
            "DLG-aware patcher generation requires both files to have native file type DLG.");
    }
    if (original.isGff4() || modified.isGff4()) {
        throw std::runtime_error(
            "DLG-aware TSLPatcher/HoloPatcher output supports classic GFF DLG files only.");
    }
    if (isJadeDlg(original) || isJadeDlg(modified)) {
        throw std::runtime_error(
            "The TSLPatcher format targets KotOR and KotOR II. Jade Empire DLG files must be distributed separately.");
    }
    if (original.version() != modified.version()) {
        throw std::runtime_error("The original and modified DLG versions do not match.");
    }
    if (patchFilename.empty() ||
        std::filesystem::path(patchFilename).filename() !=
            std::filesystem::path(patchFilename)) {
        throw std::runtime_error(
            "The patch target must be a filename without directory components.");
    }
    if (structDeepEqual(*original.root(), *modified.root())) {
        throw std::runtime_error(
            "The original and modified DLG files contain no differences.");
    }

    if (mode == DlgPatchMode::CompleteReplacement) {
        if (!packageOutput) {
            throw std::runtime_error(
                "Complete-DLG replacement requires package output because a fragment cannot carry the replacement DLG file.");
        }
        return makeCompleteDlgReplacement(modified, patchFilename, destination);
    }

    if (packageOutput && baselineAsset.empty()) {
        throw std::runtime_error(
            "A clean baseline DLG is required when creating a dynamic merge package.");
    }

    return makeDynamicMergeProject(
        original,
        modified,
        patchFilename,
        packageOutput,
        baselineAsset,
        destination);
}


} // namespace neodlg::patcher
