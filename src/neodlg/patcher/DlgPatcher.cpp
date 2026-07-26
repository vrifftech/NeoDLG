#include "DlgPatcher.hpp"

#include "core/GffTypeNames.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace neodlg::patcher {

using neotsl::IniSection;
using neotsl::PatchProject;
using neotsl::sanitizeSectionName;

namespace {

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string trim(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

[[maybe_unused]] bool iequals(const std::string& a, const std::string& b) {
    return lowerAscii(a) == lowerAscii(b);
}

std::string cellOrEmpty(const std::vector<std::string>& row, std::size_t col) {
    return col < row.size() ? row[col] : std::string();
}

std::size_t optionalColumn(const neotabular::Table& table, const std::string& name) {
    const std::string want = lowerAscii(name);
    for (std::size_t i = 0; i < table.columns.size(); ++i) {
        if (lowerAscii(table.columns[i]) == want) return i;
    }
    return table.columns.size();
}

std::size_t requiredColumn(const neotabular::Table& table, const std::string& name) {
    const std::size_t idx = optionalColumn(table, name);
    if (idx == table.columns.size()) throw std::runtime_error("Required table column is missing: " + name);
    return idx;
}

[[maybe_unused]] std::string baseNameNoExt(const std::string& patchFilename) {
    std::filesystem::path p(patchFilename);
    std::string stem = lowerAscii(p.stem().string());
    if (stem.empty()) stem = "file";
    return sanitizeSectionName(stem);
}

std::string uniqueSectionName(const PatchProject& project, const std::string& base) {
    std::string clean = sanitizeSectionName(base);
    if (clean.empty()) clean = "section";
    for (std::size_t i = 0;; ++i) {
        std::string candidate = clean + "_" + std::to_string(i);
        if (!project.findSection(candidate)) return candidate;
    }
}

std::string nextKey(const IniSection& section, const std::string& prefix) {
    std::size_t next = 0;
    const std::string want = lowerAscii(prefix);
    for (const auto& kv : section.entries) {
        const std::string key = lowerAscii(kv.key);
        if (key.rfind(want, 0) != 0) continue;
        const std::string suffix = key.substr(want.size());
        if (suffix.empty()) continue;
        bool ok = true;
        std::size_t value = 0;
        for (char ch : suffix) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) { ok = false; break; }
            value = value * 10 + static_cast<std::size_t>(ch - '0');
        }
        if (ok && value >= next) next = value + 1;
    }
    return prefix + std::to_string(next);
}

void addAssetIfRequested(PatchProject& project, bool copyBaselineAsset, const std::filesystem::path& baselineAsset, const std::string& patchFilename) {
    if (copyBaselineAsset && !baselineAsset.empty()) {
        project.assets.push_back({baselineAsset, patchFilename});
    }
}

void addNumberedEntry(PatchProject& project, const std::string& sectionName, const std::string& prefix, const std::string& value) {
    auto& section = project.section(sectionName);
    section.entries.push_back({nextKey(section, prefix), value});
}

void addPlainEntry(PatchProject& project, const std::string& sectionName, const std::string& key, const std::string& value) {
    project.section(sectionName).entries.push_back({key, value});
}

std::string gffPatchType(std::string type) {
    type = trim(type);
    const std::string key = lowerAscii(type);
    if (key == "cexostring") return "ExoString";
    if (key == "cresref") return "ResRef";
    if (key == "cexolocstring") return "ExoLocString";
    if (key == "cexolocstring strref") return "ExoLocString";
    if (key == "cexolocstring text") return "ExoLocString";
    return type;
}

[[maybe_unused]] bool gffTypeIsEditableText(const std::string& type) {
    const std::string key = lowerAscii(type);
    return key != "struct" && key != "list" && key != "void" && key != "cexolocstring";
}

[[maybe_unused]] bool truthy(std::string text) {
    text = lowerAscii(trim(std::move(text)));
    return text == "1" || text == "yes" || text == "true" || text == "editable";
}

[[maybe_unused]] std::string parentPathOf(const std::string& path) {
    const std::size_t pos = path.find_last_of('\\');
    if (pos == std::string::npos) return {};
    return path.substr(0, pos);
}

[[maybe_unused]] std::string leafOf(const std::string& path) {
    const std::size_t pos = path.find_last_of('\\');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

[[maybe_unused]] std::string extractTypeId(const std::string& value) {
    const std::string key = "typeid=";
    const std::size_t pos = lowerAscii(value).find(key);
    if (pos == std::string::npos) return "0";
    std::size_t start = pos + key.size();
    std::size_t end = start;
    while (end < value.size() && std::isdigit(static_cast<unsigned char>(value[end]))) ++end;
    return end > start ? value.substr(start, end - start) : std::string("0");
}

[[maybe_unused]] std::string extractLocStrRef(const std::string& value) {
    const std::string key = "strref=";
    const std::string lower = lowerAscii(value);
    const std::size_t pos = lower.find(key);
    if (pos == std::string::npos) return "-1";
    std::size_t start = pos + key.size();
    std::size_t end = start;
    if (end < value.size() && value[end] == '-') ++end;
    while (end < value.size() && std::isdigit(static_cast<unsigned char>(value[end]))) ++end;
    return end > start ? value.substr(start, end - start) : std::string("-1");
}

[[maybe_unused]] std::string locLangKeyFromPath(const std::string& path) {
    const std::size_t marker = path.rfind("(lang");
    if (marker == std::string::npos || path.back() != ')') return {};
    return "lang" + path.substr(marker + 5, path.size() - marker - 6);
}


struct GffRow {
    std::string path;
    std::string label;
    std::string type;
    std::string editable;
    std::string value;
    std::size_t order = 0;
};

[[maybe_unused]] std::map<std::string, GffRow> gffRowsByPath(const neotabular::Table& table) {
    const std::size_t pathCol = requiredColumn(table, "Path");
    const std::size_t labelCol = optionalColumn(table, "Label");
    const std::size_t typeCol = optionalColumn(table, "Type");
    const std::size_t editableCol = optionalColumn(table, "Editable");
    const std::size_t valueCol = requiredColumn(table, "Value");
    std::map<std::string, GffRow> out;
    for (std::size_t i = 0; i < table.rows.size(); ++i) {
        const auto& row = table.rows[i];
        const std::string path = cellOrEmpty(row, pathCol);
        if (path.empty()) continue;
        out[path] = GffRow{path, cellOrEmpty(row, labelCol), cellOrEmpty(row, typeCol), cellOrEmpty(row, editableCol), cellOrEmpty(row, valueCol), i};
    }
    return out;
}

[[maybe_unused]] bool isLocStringChildType(const std::string& type) {
    const std::string key = lowerAscii(type);
    return key == "cexolocstring strref" || key == "cexolocstring text";
}

[[maybe_unused]] bool isLocStringChildPath(const std::string& path) {
    return path.size() > 8 && path.back() == ')' && path.find('(') != std::string::npos;
}

} // namespace


namespace {

struct DlgDeferredAssignment {
    std::string pathToken;
    std::string valueToken;
};

struct DlgEmitContext {
    PatchProject& project;
    std::string fileSection;
    std::size_t originalEntryCount = 0;
    std::size_t originalReplyCount = 0;
    std::map<std::string, std::string> rootIndexTokens;
    std::vector<DlgDeferredAssignment> deferredAssignments;
    int nextToken = 1;
};

std::string dlgToken(DlgEmitContext& ctx) {
    return "2DAMEMORY" + std::to_string(ctx.nextToken++);
}

std::string dlgRootKey(const std::string& rootList, std::size_t index) {
    return rootList + ":" + std::to_string(index);
}

std::string strRefText(std::uint32_t value) {
    return value == 0xFFFFFFFFu ? std::string("-1") : std::to_string(value);
}

bool isDlgType(const neodlg::GffFile& file) {
    return lowerAscii(trim(file.filetype())) == "dlg";
}

const neodlg::GffField* dlgStructField(const neodlg::GffStruct& structure, const std::string& label) {
    for (std::size_t i = 0; i < structure.count(); ++i) {
        const neodlg::GffField* field = structure.GetField(i);
        if (field && field->GetLabel() == label) return field;
    }
    return nullptr;
}

const neodlg::GffList* dlgListField(const neodlg::GffStruct& structure, const std::string& label) {
    const auto* field = dlgStructField(structure, label);
    if (!field || field->fieldtype != neodlg::FIELD_TYPE_LIST) return nullptr;
    return dynamic_cast<const neodlg::GffList*>(field);
}

const neodlg::GffList* dlgRootList(const neodlg::GffFile& file, const std::string& label) {
    const auto* root = file.root();
    return root ? dlgListField(*root, label) : nullptr;
}

bool parseSizeTStrict(const std::string& text, std::size_t& out) {
    if (text.empty()) return false;
    std::size_t value = 0;
    for (char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        const std::size_t digit = static_cast<std::size_t>(ch - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10u) return false;
        value = value * 10u + digit;
    }
    out = value;
    return true;
}

std::optional<std::size_t> dlgStructIndexValue(const neodlg::GffStruct& structure) {
    const neodlg::GffField* indexField = dlgStructField(structure, "Index");
    if (!indexField) return std::nullopt;
    std::string text = trim(indexField->GetString());
    if (!text.empty() && text.front() == '+') text.erase(text.begin());
    std::size_t value = 0;
    if (!parseSizeTStrict(text, value)) return std::nullopt;
    return value;
}

std::string appendPath(const std::string& parent, const std::string& child) {
    if (parent.empty()) return child;
    if (child.empty()) return parent;
    return parent + "\\" + child;
}

std::string patcherFieldType(const neodlg::GffField& field) {
    return gffPatchType(neodlg::fieldTypeName(field.fieldtype));
}

bool isScalarPatchable(const neodlg::GffField& field) {
    return field.fieldtype != neodlg::FIELD_TYPE_STRUCT &&
           field.fieldtype != neodlg::FIELD_TYPE_LIST &&
           field.fieldtype != neodlg::FIELD_TYPE_VOID;
}

void appendHex(std::ostringstream& out, const neodlg::ByteBuffer& data) {
    static constexpr char hex[] = "0123456789ABCDEF";
    for (std::uint8_t byte : data) {
        out << hex[(byte >> 4u) & 0x0Fu] << hex[byte & 0x0Fu];
    }
}

void appendFieldSignature(std::ostringstream& out, const neodlg::GffField& field);

void appendStructSignature(std::ostringstream& out, const neodlg::GffStruct& structure) {
    out << "S(" << structure.typeid_ << "){";
    for (std::size_t i = 0; i < structure.count(); ++i) {
        const neodlg::GffField* field = structure.GetField(i);
        if (!field) continue;
        appendFieldSignature(out, *field);
        out << ';';
    }
    out << '}';
}

void appendListSignature(std::ostringstream& out, const neodlg::GffList& list) {
    out << "L[";
    for (std::size_t i = 0; i < list.count(); ++i) {
        const neodlg::GffStruct* structure = list.GetStruct(i);
        if (!structure) continue;
        appendStructSignature(out, *structure);
        out << ';';
    }
    out << ']';
}

void appendLocStringSignature(std::ostringstream& out, const neodlg::GffLocalizedStringField& loc) {
    out << "loc:" << strRefText(loc.strref) << ':';
    std::vector<std::pair<std::int32_t, std::string>> strings;
    for (const auto& sub : loc.substrings) strings.push_back({sub.stringid, sub.GetString()});
    std::sort(strings.begin(), strings.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& item : strings) out << item.first << '=' << item.second << '|';
}

void appendFieldSignature(std::ostringstream& out, const neodlg::GffField& field) {
    out << field.GetLabel() << ':' << field.fieldtype << '=';
    switch (field.fieldtype) {
    case neodlg::FIELD_TYPE_STRUCT:
        appendStructSignature(out, dynamic_cast<const neodlg::GffStruct&>(field));
        break;
    case neodlg::FIELD_TYPE_LIST:
        appendListSignature(out, dynamic_cast<const neodlg::GffList&>(field));
        break;
    case neodlg::FIELD_TYPE_CEXOLOCSTRING:
        appendLocStringSignature(out, dynamic_cast<const neodlg::GffLocalizedStringField&>(field));
        break;
    case neodlg::FIELD_TYPE_VOID:
        appendHex(out, dynamic_cast<const neodlg::GffVoidField&>(field).data);
        break;
    default:
        out << field.GetString();
        break;
    }
}

std::string dlgStructSignature(const neodlg::GffStruct& structure) {
    std::ostringstream out;
    appendStructSignature(out, structure);
    return out.str();
}

std::string linkTargetRootForList(const std::string& listLabel) {
    if (listLabel == "RepliesList") return "ReplyList";
    if (listLabel == "EntriesList" || listLabel == "StartingList") return "EntryList";
    return {};
}

bool isDlgLinkListLabel(const std::string& listLabel) {
    return listLabel == "RepliesList" || listLabel == "EntriesList" || listLabel == "StartingList";
}

bool shouldUseListIndexTypeId(const std::string& listLabel) {
    return listLabel == "EntryList" || listLabel == "ReplyList" ||
           listLabel == "RepliesList" || listLabel == "EntriesList" ||
           listLabel == "StartingList";
}

std::optional<std::string> tokenForDlgRootTarget(const DlgEmitContext& ctx, const std::string& rootList, std::size_t index) {
    const auto found = ctx.rootIndexTokens.find(dlgRootKey(rootList, index));
    if (found == ctx.rootIndexTokens.end()) return std::nullopt;
    return found->second;
}

std::string addGffAddFieldSection(DlgEmitContext& ctx,
                                  const std::string& parentSection,
                                  const std::string& baseName,
                                  const neodlg::GffField& field,
                                  const std::optional<std::string>& path,
                                  const std::string& label,
                                  const std::optional<std::string>& typeId,
                                  const std::optional<std::string>& value,
                                  const std::optional<std::string>& listIndexToken,
                                  const std::optional<std::string>& fieldPathToken) {
    const std::string sectionName = uniqueSectionName(ctx.project, baseName);
    addNumberedEntry(ctx.project, parentSection, "AddField", sectionName);
    ctx.project.add(sectionName, "FieldType", patcherFieldType(field));
    if (path) ctx.project.add(sectionName, "Path", *path);
    ctx.project.add(sectionName, "Label", label);
    if (typeId) ctx.project.add(sectionName, "TypeId", *typeId);
    if (value) ctx.project.add(sectionName, "Value", *value);
    if (listIndexToken) ctx.project.add(sectionName, *listIndexToken, "ListIndex");
    if (fieldPathToken) ctx.project.add(sectionName, *fieldPathToken, "!FieldPath");
    return sectionName;
}

std::string addLocStringAddFieldSection(DlgEmitContext& ctx,
                                        const std::string& parentSection,
                                        const std::string& baseName,
                                        const neodlg::GffLocalizedStringField& loc,
                                        const std::optional<std::string>& path,
                                        const std::string& label) {
    const std::string sectionName = uniqueSectionName(ctx.project, baseName);
    addNumberedEntry(ctx.project, parentSection, "AddField", sectionName);
    ctx.project.add(sectionName, "FieldType", "ExoLocString");
    if (path) ctx.project.add(sectionName, "Path", *path);
    ctx.project.add(sectionName, "Label", label);
    ctx.project.add(sectionName, "StrRef", strRefText(loc.strref));
    for (const auto& sub : loc.substrings) {
        ctx.project.add(sectionName, "lang" + std::to_string(sub.stringid), sub.GetString());
    }
    return sectionName;
}

struct DlgEmitStructOptions {
    std::string listLabel;
    std::optional<std::string> path;
    std::optional<std::string> listIndexToken;
    std::string targetRootList;
    std::string baseName;
};

void emitDlgField(DlgEmitContext& ctx,
                  const std::string& parentSection,
                  const neodlg::GffField& field,
                  const std::optional<std::string>& path,
                  const std::string& baseName,
                  const std::optional<std::string>& forcedValue = std::nullopt,
                  const std::optional<std::string>& fieldPathToken = std::nullopt);

void emitDlgStruct(DlgEmitContext& ctx,
                   const std::string& parentSection,
                   const neodlg::GffStruct& structure,
                   DlgEmitStructOptions options) {
    std::string typeId = std::to_string(structure.typeid_);
    if (shouldUseListIndexTypeId(options.listLabel)) typeId = "ListIndex";

    std::optional<std::string> dynamicIndexValueToken;
    if (!options.targetRootList.empty()) {
        const auto targetIndex = dlgStructIndexValue(structure);
        if (targetIndex) {
            dynamicIndexValueToken = tokenForDlgRootTarget(ctx, options.targetRootList, *targetIndex);
            if (!dynamicIndexValueToken &&
                ((options.targetRootList == "EntryList" && *targetIndex >= ctx.originalEntryCount) ||
                 (options.targetRootList == "ReplyList" && *targetIndex >= ctx.originalReplyCount))) {
                ctx.project.unsupported.push_back("DLG link in " + options.listLabel + " references new " + options.targetRootList +
                                                  " index " + std::to_string(*targetIndex) + " but no dynamic token was allocated for it.");
            }
        } else {
            ctx.project.warnings.push_back("DLG link added to " + options.listLabel + " has no numeric Index field; it was emitted without dynamic relinking.");
        }
    }

    const std::string sectionName = addGffAddFieldSection(ctx,
                                                          parentSection,
                                                          options.baseName,
                                                          structure,
                                                          options.path,
                                                          structure.GetLabel(),
                                                          typeId,
                                                          std::nullopt,
                                                          options.listIndexToken,
                                                          std::nullopt);

    for (std::size_t i = 0; i < structure.count(); ++i) {
        const neodlg::GffField* child = structure.GetField(i);
        if (!child) continue;
        if (dynamicIndexValueToken && child->GetLabel() == "Index" && isScalarPatchable(*child)) {
            const std::string pathToken = dlgToken(ctx);
            emitDlgField(ctx, sectionName, *child, std::nullopt, options.baseName + "_index", std::string("0"), pathToken);
            ctx.deferredAssignments.push_back({pathToken, *dynamicIndexValueToken});
        } else {
            emitDlgField(ctx, sectionName, *child, std::nullopt, options.baseName + "_" + child->GetLabel());
        }
    }
}

void emitDlgList(DlgEmitContext& ctx,
                 const std::string& parentSection,
                 const neodlg::GffList& list,
                 const std::optional<std::string>& path,
                 const std::string& baseName) {
    if (list.gff4CompactPrimitiveList) {
        ctx.project.unsupported.push_back("DLG patcher output cannot add compact primitive GFF4 list: " + list.GetLabel());
        return;
    }
    const std::string sectionName = addGffAddFieldSection(ctx,
                                                          parentSection,
                                                          baseName,
                                                          list,
                                                          path,
                                                          list.GetLabel(),
                                                          std::nullopt,
                                                          std::nullopt,
                                                          std::nullopt,
                                                          std::nullopt);
    const std::string targetRoot = linkTargetRootForList(list.GetLabel());
    for (std::size_t i = 0; i < list.count(); ++i) {
        const neodlg::GffStruct* structure = list.GetStruct(i);
        if (!structure) continue;
        emitDlgStruct(ctx, sectionName, *structure,
                      DlgEmitStructOptions{list.GetLabel(), std::nullopt, std::nullopt, targetRoot,
                                           baseName + "_" + list.GetLabel() + "_" + std::to_string(i)});
    }
}

void emitDlgField(DlgEmitContext& ctx,
                  const std::string& parentSection,
                  const neodlg::GffField& field,
                  const std::optional<std::string>& path,
                  const std::string& baseName,
                  const std::optional<std::string>& forcedValue,
                  const std::optional<std::string>& fieldPathToken) {
    switch (field.fieldtype) {
    case neodlg::FIELD_TYPE_STRUCT:
        emitDlgStruct(ctx, parentSection, dynamic_cast<const neodlg::GffStruct&>(field),
                      DlgEmitStructOptions{std::string{}, path, std::nullopt, std::string{}, baseName});
        break;
    case neodlg::FIELD_TYPE_LIST:
        emitDlgList(ctx, parentSection, dynamic_cast<const neodlg::GffList&>(field), path, baseName);
        break;
    case neodlg::FIELD_TYPE_CEXOLOCSTRING:
        addLocStringAddFieldSection(ctx,
                                    parentSection,
                                    baseName,
                                    dynamic_cast<const neodlg::GffLocalizedStringField&>(field),
                                    path,
                                    field.GetLabel());
        break;
    case neodlg::FIELD_TYPE_VOID:
        ctx.project.unsupported.push_back("DLG patcher output cannot add VOID field: " + (path ? *path : field.GetLabel()));
        break;
    default:
        addGffAddFieldSection(ctx,
                              parentSection,
                              baseName,
                              field,
                              path,
                              field.GetLabel(),
                              std::nullopt,
                              forcedValue ? forcedValue : std::optional<std::string>(field.GetString()),
                              std::nullopt,
                              fieldPathToken);
        break;
    }
}

void emitDirectScalarModification(DlgEmitContext& ctx, const std::string& path, const neodlg::GffField& modified) {
    if (modified.fieldtype == neodlg::FIELD_TYPE_CEXOLOCSTRING ||
        modified.fieldtype == neodlg::FIELD_TYPE_STRUCT ||
        modified.fieldtype == neodlg::FIELD_TYPE_LIST) {
        return;
    }
    if (modified.fieldtype == neodlg::FIELD_TYPE_VOID) {
        ctx.project.unsupported.push_back("DLG modified VOID field is not emitted as a patcher value: " + path);
        return;
    }
    addPlainEntry(ctx.project, ctx.fileSection, path, modified.GetString());
}

void emitDirectLocStringModifications(DlgEmitContext& ctx,
                                      const std::string& path,
                                      const neodlg::GffLocalizedStringField& original,
                                      const neodlg::GffLocalizedStringField& modified) {
    if (original.strref != modified.strref) {
        addPlainEntry(ctx.project, ctx.fileSection, path + "(strref)", strRefText(modified.strref));
    }

    std::map<std::int32_t, std::string> origStrings;
    for (const auto& sub : original.substrings) origStrings[sub.stringid] = sub.GetString();
    std::map<std::int32_t, std::string> modStrings;
    for (const auto& sub : modified.substrings) modStrings[sub.stringid] = sub.GetString();

    for (const auto& item : origStrings) {
        if (modStrings.find(item.first) == modStrings.end()) {
            ctx.project.unsupported.push_back("DLG localized-string substring deletion is not emitted: " + path + " lang" + std::to_string(item.first));
        }
    }
    for (const auto& item : modStrings) {
        const auto found = origStrings.find(item.first);
        if (found == origStrings.end() || found->second != item.second) {
            addPlainEntry(ctx.project, ctx.fileSection, path + "(lang" + std::to_string(item.first) + ")", item.second);
        }
    }
}

bool targetIndexIsNewForRoot(const DlgEmitContext& ctx, const std::string& rootList, std::size_t targetIndex) {
    if (rootList == "EntryList") return targetIndex >= ctx.originalEntryCount;
    if (rootList == "ReplyList") return targetIndex >= ctx.originalReplyCount;
    return false;
}

std::vector<std::pair<std::size_t, const neodlg::GffStruct*>> findAddedLinkStructs(const neodlg::GffList* original,
                                                                                  const neodlg::GffList& modified) {
    std::map<std::string, std::size_t> remainingOriginal;
    if (original) {
        for (std::size_t i = 0; i < original->count(); ++i) {
            const neodlg::GffStruct* structure = original->GetStruct(i);
            if (structure) ++remainingOriginal[dlgStructSignature(*structure)];
        }
    }

    std::vector<std::pair<std::size_t, const neodlg::GffStruct*>> out;
    for (std::size_t i = 0; i < modified.count(); ++i) {
        const neodlg::GffStruct* structure = modified.GetStruct(i);
        if (!structure) continue;
        const std::string sig = dlgStructSignature(*structure);
        auto found = remainingOriginal.find(sig);
        if (found != remainingOriginal.end() && found->second > 0) {
            --found->second;
            continue;
        }
        out.push_back({i, structure});
    }
    return out;
}

void emitLinkListAdditions(DlgEmitContext& ctx,
                           const std::string& listPath,
                           const std::string& listLabel,
                           const neodlg::GffList* original,
                           const neodlg::GffList& modified) {
    if (original && modified.count() < original->count()) {
        ctx.project.unsupported.push_back("DLG link list shrank; deletions are not emitted: " + listPath);
    }
    const std::string targetRoot = linkTargetRootForList(listLabel);
    for (const auto& item : findAddedLinkStructs(original, modified)) {
        const std::size_t modifiedIndex = item.first;
        const neodlg::GffStruct* structure = item.second;
        if (!structure) continue;
        const auto targetIndex = dlgStructIndexValue(*structure);
        const bool targetIsNew = targetIndex && targetIndexIsNewForRoot(ctx, targetRoot, *targetIndex);
        const bool appending = !original || modifiedIndex >= original->count();
        if (!appending) {
            ctx.project.warnings.push_back("DLG link in " + listPath + " was inserted or reordered in the modified file; TSLPatcher/HoloPatcher will append it instead.");
        }
        if (!targetIsNew && !appending) {
            ctx.project.unsupported.push_back("DLG existing link appears changed or reordered and does not point at a newly added node: " + listPath + "\\" + std::to_string(modifiedIndex));
            continue;
        }
        emitDlgStruct(ctx, ctx.fileSection, *structure,
                      DlgEmitStructOptions{listLabel, listPath, std::nullopt, targetRoot,
                                           "dlg_link_" + sanitizeSectionName(listPath) + "_" + std::to_string(modifiedIndex)});
    }
}

void compareDlgFields(DlgEmitContext& ctx,
                      const neodlg::GffStruct& original,
                      const neodlg::GffStruct& modified,
                      const std::string& path);

void compareDlgLists(DlgEmitContext& ctx,
                     const neodlg::GffList& original,
                     const neodlg::GffList& modified,
                     const std::string& path) {
    const std::string listLabel = modified.GetLabel();
    if (listLabel == "EntryList" || listLabel == "ReplyList") {
        if (modified.count() < original.count()) {
            ctx.project.unsupported.push_back("DLG root node list shrank; deletions are not emitted: " + path);
        }
        const std::size_t common = std::min(original.count(), modified.count());
        for (std::size_t i = 0; i < common; ++i) {
            const auto* origStruct = original.GetStruct(i);
            const auto* modStruct = modified.GetStruct(i);
            if (origStruct && modStruct) compareDlgFields(ctx, *origStruct, *modStruct, appendPath(path, std::to_string(i)));
        }
        return;
    }

    if (isDlgLinkListLabel(listLabel)) {
        emitLinkListAdditions(ctx, path, listLabel, &original, modified);
        return;
    }

    if (modified.count() < original.count()) {
        ctx.project.unsupported.push_back("GFF list shrank; deletions are not emitted: " + path);
    }
    const std::size_t common = std::min(original.count(), modified.count());
    for (std::size_t i = 0; i < common; ++i) {
        const auto* origStruct = original.GetStruct(i);
        const auto* modStruct = modified.GetStruct(i);
        if (origStruct && modStruct) compareDlgFields(ctx, *origStruct, *modStruct, appendPath(path, std::to_string(i)));
    }
    for (std::size_t i = original.count(); i < modified.count(); ++i) {
        const auto* modStruct = modified.GetStruct(i);
        if (!modStruct) continue;
        emitDlgStruct(ctx, ctx.fileSection, *modStruct,
                      DlgEmitStructOptions{listLabel, path, std::nullopt, linkTargetRootForList(listLabel),
                                           "dlg_added_list_struct_" + sanitizeSectionName(path) + "_" + std::to_string(i)});
    }
}

void compareDlgFields(DlgEmitContext& ctx,
                      const neodlg::GffStruct& original,
                      const neodlg::GffStruct& modified,
                      const std::string& path) {
    std::map<std::string, const neodlg::GffField*> originalByLabel;
    for (std::size_t i = 0; i < original.count(); ++i) {
        const neodlg::GffField* field = original.GetField(i);
        if (field) originalByLabel[field->GetLabel()] = field;
    }

    std::set<std::string> seenModified;
    for (std::size_t i = 0; i < modified.count(); ++i) {
        const neodlg::GffField* modField = modified.GetField(i);
        if (!modField) continue;
        const std::string label = modField->GetLabel();
        seenModified.insert(label);
        const std::string childPath = appendPath(path, label);
        const auto found = originalByLabel.find(label);
        if (found == originalByLabel.end()) {
            if (modField->fieldtype == neodlg::FIELD_TYPE_LIST && isDlgLinkListLabel(label)) {
                emitDlgField(ctx, ctx.fileSection, *modField, path, "dlg_added_" + sanitizeSectionName(childPath));
            } else {
                emitDlgField(ctx, ctx.fileSection, *modField, path, "dlg_added_" + sanitizeSectionName(childPath));
            }
            continue;
        }

        const neodlg::GffField* origField = found->second;
        if (!origField) continue;
        if (origField->fieldtype != modField->fieldtype) {
            ctx.project.unsupported.push_back("GFF field type changed and was not emitted: " + childPath + " (" +
                                              neodlg::fieldTypeName(origField->fieldtype) + " -> " +
                                              neodlg::fieldTypeName(modField->fieldtype) + ")");
            continue;
        }

        switch (modField->fieldtype) {
        case neodlg::FIELD_TYPE_STRUCT:
            compareDlgFields(ctx,
                             dynamic_cast<const neodlg::GffStruct&>(*origField),
                             dynamic_cast<const neodlg::GffStruct&>(*modField),
                             childPath);
            break;
        case neodlg::FIELD_TYPE_LIST:
            compareDlgLists(ctx,
                            dynamic_cast<const neodlg::GffList&>(*origField),
                            dynamic_cast<const neodlg::GffList&>(*modField),
                            childPath);
            break;
        case neodlg::FIELD_TYPE_CEXOLOCSTRING:
            emitDirectLocStringModifications(ctx,
                                             childPath,
                                             dynamic_cast<const neodlg::GffLocalizedStringField&>(*origField),
                                             dynamic_cast<const neodlg::GffLocalizedStringField&>(*modField));
            break;
        default:
            if (origField->GetString() != modField->GetString()) emitDirectScalarModification(ctx, childPath, *modField);
            break;
        }
    }

    for (const auto& item : originalByLabel) {
        if (seenModified.find(item.first) == seenModified.end()) {
            ctx.project.unsupported.push_back("GFF field deletion is not emitted: " + appendPath(path, item.first));
        }
    }
}

void allocateRootNodeTokens(DlgEmitContext& ctx, const std::string& listName, const neodlg::GffList* original, const neodlg::GffList* modified) {
    const std::size_t originalCount = original ? original->count() : 0u;
    const std::size_t modifiedCount = modified ? modified->count() : 0u;
    for (std::size_t i = originalCount; i < modifiedCount; ++i) {
        ctx.rootIndexTokens[dlgRootKey(listName, i)] = dlgToken(ctx);
    }
}

void emitNewRootNodes(DlgEmitContext& ctx, const std::string& listName, const neodlg::GffList* original, const neodlg::GffList* modified) {
    if (!modified) return;
    const std::size_t originalCount = original ? original->count() : 0u;
    if (modified->count() < originalCount) return;
    for (std::size_t i = originalCount; i < modified->count(); ++i) {
        const neodlg::GffStruct* structure = modified->GetStruct(i);
        if (!structure) continue;
        const auto token = tokenForDlgRootTarget(ctx, listName, i);
        emitDlgStruct(ctx, ctx.fileSection, *structure,
                      DlgEmitStructOptions{listName, listName, token, std::string{},
                                           "dlg_new_" + sanitizeSectionName(listName) + "_" + std::to_string(i)});
    }
}

void appendDeferredDlgAssignments(DlgEmitContext& ctx) {
    for (const auto& item : ctx.deferredAssignments) {
        addPlainEntry(ctx.project, ctx.fileSection, item.pathToken, item.valueToken);
    }
}

void addDlgAwarenessWarnings(PatchProject& project) {
    project.warnings.push_back("DLG-aware patcher output stores new EntryList/ReplyList indexes in 2DAMEMORY tokens and rewrites added link Index fields after creation.");
    project.warnings.push_back("TSLPatcher/HoloPatcher append new list structs; inserted or reordered dialogue choices may appear at the end of their parent link list.");
}

} // namespace

PatchProject diffDlgPatcher(const neodlg::GffFile& original,
                            const neodlg::GffFile& modified,
                            const std::string& patchFilename,
                            bool copyBaselineAsset,
                            const std::filesystem::path& baselineAsset) {
    if (!original.root() || !modified.root()) {
        throw std::runtime_error("DLG patcher generation requires loaded original and modified DLG files.");
    }
    if (!isDlgType(original) || !isDlgType(modified)) {
        throw std::runtime_error("DLG-aware patcher generation requires both files to have native file type DLG.");
    }
    if (original.isGff4() || modified.isGff4()) {
        throw std::runtime_error(
            "DLG-aware TSLPatcher/HoloPatcher output supports canonical GFF3 DLG files only; GFF4 documents are not valid GFFList patch targets.");
    }
    if (original.version() != modified.version()) {
        throw std::runtime_error("The original and modified DLG versions do not match.");
    }

    PatchProject project;
    project.add("GFFList", "File0", patchFilename);
    project.section(patchFilename);
    addAssetIfRequested(project, copyBaselineAsset, baselineAsset, patchFilename);
    addDlgAwarenessWarnings(project);

    DlgEmitContext ctx{project, patchFilename, 0u, 0u, {}, {}, 1};
    const neodlg::GffList* origEntries = dlgRootList(original, "EntryList");
    const neodlg::GffList* modEntries = dlgRootList(modified, "EntryList");
    const neodlg::GffList* origReplies = dlgRootList(original, "ReplyList");
    const neodlg::GffList* modReplies = dlgRootList(modified, "ReplyList");
    ctx.originalEntryCount = origEntries ? origEntries->count() : 0u;
    ctx.originalReplyCount = origReplies ? origReplies->count() : 0u;

    if (!modEntries) project.unsupported.push_back("Modified DLG has no EntryList; output may not be useful.");
    if (!modReplies) project.unsupported.push_back("Modified DLG has no ReplyList; output may not be useful.");
    if (modEntries && origEntries && modEntries->count() < origEntries->count()) {
        project.unsupported.push_back("Modified DLG EntryList has fewer nodes than the original; node deletion is not emitted.");
    }
    if (modReplies && origReplies && modReplies->count() < origReplies->count()) {
        project.unsupported.push_back("Modified DLG ReplyList has fewer nodes than the original; node deletion is not emitted.");
    }

    allocateRootNodeTokens(ctx, "EntryList", origEntries, modEntries);
    allocateRootNodeTokens(ctx, "ReplyList", origReplies, modReplies);

    emitNewRootNodes(ctx, "EntryList", origEntries, modEntries);
    emitNewRootNodes(ctx, "ReplyList", origReplies, modReplies);
    compareDlgFields(ctx, *original.root(), *modified.root(), std::string{});
    appendDeferredDlgAssignments(ctx);

    return project;
}


} // namespace neodlg::patcher
