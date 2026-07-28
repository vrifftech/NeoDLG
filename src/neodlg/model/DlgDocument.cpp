#include "neodlg/model/DlgDocument.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace neodlg {
namespace {

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string collapseWhitespace(std::string text) {
    bool inWhitespace = false;
    std::string output;
    output.reserve(text.size());
    for (unsigned char ch : text) {
        if (std::isspace(ch)) {
            if (!inWhitespace && !output.empty()) output.push_back(' ');
            inWhitespace = true;
        } else {
            output.push_back(static_cast<char>(ch));
            inWhitespace = false;
        }
    }
    while (!output.empty() && output.back() == ' ') output.pop_back();
    return output;
}

std::string ellipsize(std::string text, std::size_t maxLength) {
    text = collapseWhitespace(std::move(text));
    if (text.size() <= maxLength) return text;
    if (maxLength < 4) return text.substr(0, maxLength);
    text.resize(maxLength - 3);
    text += "...";
    return text;
}

std::uint32_t parseUnsigned(const std::string& value) {
    if (value.empty() || value == "-1") return 0xFFFFFFFFu;
    return ParseUInt32Decimal(value);
}


float parseFloatValue(const std::string& value) {
    if (value.empty()) return 0.0f;
    return ParseFloatDecimal(value);
}

GffField* findField(GffStruct& structure, const std::string& label) {
    return structure.GetFieldByLabel(label);
}

const GffField* findField(const GffStruct& structure, const std::string& label) {
    return structure.GetFieldByLabel(label);
}

GffList* findList(GffStruct& structure, const std::string& label) {
    return dynamic_cast<GffList*>(findField(structure, label));
}

const GffList* findList(const GffStruct& structure, const std::string& label) {
    return dynamic_cast<const GffList*>(findField(structure, label));
}

std::uint32_t integralValue(const GffField* field, std::uint32_t fallback = 0) {
    if (const auto* value = dynamic_cast<const GffByteField*>(field)) return value->value;
    if (const auto* value = dynamic_cast<const GffWordField*>(field)) return value->value;
    if (const auto* value = dynamic_cast<const GffUInt32Field*>(field)) return value->value;
    if (const auto* value = dynamic_cast<const GffIntField*>(field)) return static_cast<std::uint32_t>(value->value);
    return fallback;
}

std::int32_t signedValue(const GffField* field, std::int32_t fallback = 0) {
    if (const auto* value = dynamic_cast<const GffByteField*>(field)) return value->value;
    if (const auto* value = dynamic_cast<const GffWordField*>(field)) return value->value;
    if (const auto* value = dynamic_cast<const GffUInt32Field*>(field)) return static_cast<std::int32_t>(value->value);
    if (const auto* value = dynamic_cast<const GffIntField*>(field)) return value->value;
    return fallback;
}

std::string stringValue(const GffField* field) {
    if (!field) return {};
    return field->GetString();
}

void setExistingFieldValue(GffField& field, const std::string& value) {
    switch (field.fieldtype) {
    case FIELD_TYPE_BYTE:
        dynamic_cast<GffByteField&>(field).value = static_cast<std::uint8_t>(ParseUInt32Decimal(value.empty() ? "0" : value));
        return;
    case FIELD_TYPE_CHAR:
        dynamic_cast<GffCharField&>(field).value = value.empty() ? '\0' : value.front();
        return;
    case FIELD_TYPE_WORD:
        dynamic_cast<GffWordField&>(field).value = static_cast<std::uint16_t>(ParseUInt32Decimal(value.empty() ? "0" : value));
        return;
    case FIELD_TYPE_SHORT:
        dynamic_cast<GffShortField&>(field).value = static_cast<std::int16_t>(ParseInt32Decimal(value.empty() ? "0" : value));
        return;
    case FIELD_TYPE_DWORD:
        dynamic_cast<GffUInt32Field&>(field).value = ParseUInt32Decimal(value.empty() ? "0" : value);
        return;
    case FIELD_TYPE_INT:
        dynamic_cast<GffIntField&>(field).value = ParseInt32Decimal(value.empty() ? "0" : value);
        return;
    case FIELD_TYPE_DWORD64:
        dynamic_cast<GffUInt64Field&>(field).value = ParseUInt64Decimal(value.empty() ? "0" : value);
        return;
    case FIELD_TYPE_INT64:
        dynamic_cast<GffInt64Field&>(field).value = ParseInt64Decimal(value.empty() ? "0" : value);
        return;
    case FIELD_TYPE_FLOAT:
        dynamic_cast<GffFloatField&>(field).value = parseFloatValue(value);
        return;
    case FIELD_TYPE_DOUBLE:
        dynamic_cast<GffDoubleField&>(field).value = ParseDoubleDecimal(value.empty() ? "0" : value);
        return;
    case FIELD_TYPE_CEXOSTRING:
        dynamic_cast<GffExoStringField&>(field).SetString(value);
        return;
    case FIELD_TYPE_RESREF:
        dynamic_cast<GffResRefField&>(field).SetString(value);
        return;
    case FIELD_TYPE_JADE_STRREF: {
        auto& jade = dynamic_cast<GffJadeStringRefField&>(field);
        const std::size_t separator = value.find('|');
        if (separator == std::string::npos) {
            jade.strref = parseUnsigned(value);
        } else {
            jade.stringType = ParseUInt32Decimal(value.substr(0, separator));
            jade.strref = parseUnsigned(value.substr(separator + 1));
        }
        return;
    }
    default:
        throw std::invalid_argument("The selected semantic property has an unsupported GFF field type.");
    }
}

std::unique_ptr<GffField> makeField(const std::string& label,
                                    std::uint32_t fieldType,
                                    const std::string& value) {
    switch (fieldType) {
    case FIELD_TYPE_BYTE:
        return std::make_unique<GffByteField>(label, static_cast<std::uint8_t>(ParseUInt32Decimal(value.empty() ? "0" : value)));
    case FIELD_TYPE_WORD:
        return std::make_unique<GffWordField>(label, static_cast<std::uint16_t>(ParseUInt32Decimal(value.empty() ? "0" : value)));
    case FIELD_TYPE_DWORD:
        return std::make_unique<GffUInt32Field>(label, ParseUInt32Decimal(value.empty() ? "0" : value));
    case FIELD_TYPE_INT:
        return std::make_unique<GffIntField>(label, ParseInt32Decimal(value.empty() ? "0" : value));
    case FIELD_TYPE_FLOAT:
        return std::make_unique<GffFloatField>(label, parseFloatValue(value));
    case FIELD_TYPE_CEXOSTRING:
        return std::make_unique<GffExoStringField>(label, value);
    case FIELD_TYPE_RESREF:
        return std::make_unique<GffResRefField>(label, value);
    case FIELD_TYPE_CEXOLOCSTRING:
        return std::make_unique<GffLocalizedStringField>(label, parseUnsigned(value));
    case FIELD_TYPE_LIST:
        return std::make_unique<GffList>(label);
    case FIELD_TYPE_JADE_STRREF: {
        std::uint32_t stringType = 4;
        std::uint32_t strref = 0xFFFFFFFFu;
        const std::size_t separator = value.find('|');
        if (separator == std::string::npos) {
            strref = parseUnsigned(value);
        } else {
            stringType = ParseUInt32Decimal(value.substr(0, separator));
            strref = parseUnsigned(value.substr(separator + 1));
        }
        return std::make_unique<GffJadeStringRefField>(label, stringType, strref);
    }
    default:
        throw std::invalid_argument("Unsupported semantic DLG field type for " + label + ".");
    }
}

void setField(GffStruct& structure,
              const std::string& label,
              std::uint32_t fieldType,
              const std::string& value) {
    if (GffField* existing = structure.GetFieldByLabel(label)) {
        if (existing->fieldtype != fieldType) {
            throw std::invalid_argument("Field '" + label + "' has an unexpected type and was not overwritten.");
        }
        setExistingFieldValue(*existing, value);
        return;
    }
    structure.AddField(makeField(label, fieldType, value));
}

void clearList(GffStruct& structure, const std::string& label) {
    if (GffList* list = findList(structure, label)) list->allStructs().clear();
}

void clearStringField(GffStruct& structure, const std::string& label) {
    if (auto* field = structure.GetFieldByLabel(label)) {
        if (auto* text = dynamic_cast<GffExoStringField*>(field)) text->SetString({});
        else if (auto* resref = dynamic_cast<GffResRefField*>(field)) resref->SetString({});
    }
}

void setIntegralIfPresent(GffStruct& structure, const std::string& label, std::int64_t value) {
    if (auto* field = structure.GetFieldByLabel(label)) {
        switch (field->fieldtype) {
        case FIELD_TYPE_BYTE: dynamic_cast<GffByteField&>(*field).value = static_cast<std::uint8_t>(value); break;
        case FIELD_TYPE_WORD: dynamic_cast<GffWordField&>(*field).value = static_cast<std::uint16_t>(value); break;
        case FIELD_TYPE_DWORD: dynamic_cast<GffUInt32Field&>(*field).value = static_cast<std::uint32_t>(value); break;
        case FIELD_TYPE_INT: dynamic_cast<GffIntField&>(*field).value = static_cast<std::int32_t>(value); break;
        default: break;
        }
    }
}

struct NodeHasher {
    std::size_t operator()(const DlgNodeRef& ref) const noexcept {
        return (ref.index << 1u) ^ (ref.kind == DlgNodeKind::Reply ? 1u : 0u);
    }
};


} // namespace

DlgDialect DlgDocument::dialect() const {
    if (!model_.loaded() || model_.fileType() != "DLG ") return DlgDialect::Unsupported;
    const GffStruct* rootStruct = root();
    if (!rootStruct || model_.gff().isGff4()) return DlgDialect::Unsupported;
    const GffList* entries = findList(*rootStruct, "EntryList");
    const GffList* replies = findList(*rootStruct, "ReplyList");
    const GffList* starts = findList(*rootStruct, "StartingList");
    if (!entries || !replies || !starts) return DlgDialect::Unsupported;

    const GffStruct* sample = entries->count() ? entries->GetStruct(0) : nullptr;
    if (sample) {
        if (const GffField* textField = sample->GetFieldByLabel("Text")) {
            if (textField->fieldtype == FIELD_TYPE_JADE_STRREF) return DlgDialect::JadeEmpire;
            if (textField->fieldtype == FIELD_TYPE_CEXOLOCSTRING) return DlgDialect::Kotor;
        }
    }
    if (rootStruct->GetFieldByLabel("TagList")) return DlgDialect::JadeEmpire;
    return DlgDialect::Kotor;
}

DlgFlavor DlgDocument::flavor() const {
    if (dialect() == DlgDialect::JadeEmpire) return DlgFlavor::JadeEmpire;
    const GffStruct* rootStruct = root();
    if (rootStruct && (rootStruct->GetFieldByLabel("AlienRaceOwner") || rootStruct->GetFieldByLabel("NextNodeID"))) {
        return DlgFlavor::Kotor2;
    }
    const GffList* entries = nodeList(DlgNodeKind::Entry);
    if (entries && entries->count()) {
        const GffStruct* first = entries->GetStruct(0);
        if (first && (first->GetFieldByLabel("ActionParam1") || first->GetFieldByLabel("Script2"))) {
            return DlgFlavor::Kotor2;
        }
    }
    return DlgFlavor::Kotor;
}

bool DlgDocument::semanticallyEditable() const {
    const DlgDialect value = dialect();
    return value == DlgDialect::Kotor || value == DlgDialect::JadeEmpire;
}

void DlgDocument::create(DlgFlavor selectedFlavor) {
    model_.newFile("DLG");
    GffStruct* rootStruct = root();
    if (!rootStruct) throw std::runtime_error("Unable to create the DLG root structure.");

    if (selectedFlavor == DlgFlavor::JadeEmpire) {
        rootStruct->AddField(std::make_unique<GffList>("TagList"));
        rootStruct->AddField(std::make_unique<GffList>("EntryList"));
        rootStruct->AddField(std::make_unique<GffList>("ReplyList"));
        rootStruct->AddField(std::make_unique<GffList>("StartingList"));
        markDirty();
        return;
    }

    rootStruct->AddField(std::make_unique<GffUInt32Field>("DelayEntry", 0));
    rootStruct->AddField(std::make_unique<GffUInt32Field>("DelayReply", 0));
    rootStruct->AddField(std::make_unique<GffUInt32Field>("NumWords", 0));
    rootStruct->AddField(std::make_unique<GffResRefField>("EndConversation", ""));
    rootStruct->AddField(std::make_unique<GffResRefField>("EndConverAbort", ""));
    rootStruct->AddField(std::make_unique<GffByteField>("Skippable", 1));
    rootStruct->AddField(std::make_unique<GffList>("StuntList"));
    rootStruct->AddField(std::make_unique<GffResRefField>("CameraModel", ""));
    rootStruct->AddField(std::make_unique<GffExoStringField>("VO_ID", ""));
    rootStruct->AddField(std::make_unique<GffIntField>("ConversationType", 0));
    rootStruct->AddField(std::make_unique<GffByteField>("ComputerType", 0));
    rootStruct->AddField(std::make_unique<GffByteField>("OldHitCheck", 0));
    rootStruct->AddField(std::make_unique<GffResRefField>("AmbientTrack", ""));
    rootStruct->AddField(std::make_unique<GffByteField>("UnequipItems", 0));
    rootStruct->AddField(std::make_unique<GffByteField>("AnimatedCut", 0));
    rootStruct->AddField(std::make_unique<GffByteField>("UnequipHItem", 0));
    rootStruct->AddField(std::make_unique<GffList>("EntryList"));
    rootStruct->AddField(std::make_unique<GffList>("ReplyList"));
    rootStruct->AddField(std::make_unique<GffList>("StartingList"));

    if (selectedFlavor == DlgFlavor::Kotor2) {
        rootStruct->AddField(std::make_unique<GffIntField>("NextNodeID", 0));
        rootStruct->AddField(std::make_unique<GffIntField>("PostProcOwner", 0));
        rootStruct->AddField(std::make_unique<GffIntField>("AlienRaceOwner", 0));
        rootStruct->AddField(std::make_unique<GffIntField>("RecordNoVO", 0));
    }
    markDirty();
}

GffStruct* DlgDocument::root() { return model_.gff().root(); }
const GffStruct* DlgDocument::root() const { return model_.gff().root(); }

std::string DlgDocument::nodeListLabel(DlgNodeKind kind) {
    return kind == DlgNodeKind::Entry ? "EntryList" : "ReplyList";
}

std::string DlgDocument::childListLabel(DlgNodeKind kind) {
    return kind == DlgNodeKind::Entry ? "RepliesList" : "EntriesList";
}

DlgNodeKind DlgDocument::childKind(DlgNodeKind parent) {
    return parent == DlgNodeKind::Entry ? DlgNodeKind::Reply : DlgNodeKind::Entry;
}

DlgNodeKind DlgDocument::targetKind(DlgLinkOwner owner) {
    return owner == DlgLinkOwner::Entry ? DlgNodeKind::Reply : DlgNodeKind::Entry;
}

GffList* DlgDocument::nodeList(DlgNodeKind kind) {
    GffStruct* rootStruct = root();
    return rootStruct ? findList(*rootStruct, nodeListLabel(kind)) : nullptr;
}

const GffList* DlgDocument::nodeList(DlgNodeKind kind) const {
    const GffStruct* rootStruct = root();
    return rootStruct ? findList(*rootStruct, nodeListLabel(kind)) : nullptr;
}

GffStruct* DlgDocument::node(DlgNodeRef ref) {
    GffList* list = nodeList(ref.kind);
    return list && ref.index < list->count() ? list->GetStruct(ref.index) : nullptr;
}

const GffStruct* DlgDocument::node(DlgNodeRef ref) const {
    const GffList* list = nodeList(ref.kind);
    return list && ref.index < list->count() ? list->GetStruct(ref.index) : nullptr;
}

std::size_t DlgDocument::nodeCount(DlgNodeKind kind) const {
    const GffList* list = nodeList(kind);
    return list ? list->count() : 0;
}

GffList* DlgDocument::childLinkList(DlgNodeRef parent, bool createIfMissing) {
    GffStruct* structure = node(parent);
    if (!structure) return nullptr;
    if (GffList* list = findList(*structure, childListLabel(parent.kind))) return list;
    if (!createIfMissing) return nullptr;
    auto list = std::make_unique<GffList>(childListLabel(parent.kind));
    GffList* raw = list.get();
    structure->AddField(std::move(list));
    return raw;
}

const GffList* DlgDocument::childLinkList(DlgNodeRef parent) const {
    const GffStruct* structure = node(parent);
    return structure ? findList(*structure, childListLabel(parent.kind)) : nullptr;
}

GffList* DlgDocument::linkList(DlgLinkRef ref) {
    if (ref.owner == DlgLinkOwner::StartingList) {
        GffStruct* rootStruct = root();
        return rootStruct ? findList(*rootStruct, "StartingList") : nullptr;
    }
    return childLinkList(DlgNodeRef{ref.owner == DlgLinkOwner::Entry ? DlgNodeKind::Entry : DlgNodeKind::Reply,
                                    ref.ownerIndex}, false);
}

const GffList* DlgDocument::linkList(DlgLinkRef ref) const {
    if (ref.owner == DlgLinkOwner::StartingList) {
        const GffStruct* rootStruct = root();
        return rootStruct ? findList(*rootStruct, "StartingList") : nullptr;
    }
    return childLinkList(DlgNodeRef{ref.owner == DlgLinkOwner::Entry ? DlgNodeKind::Entry : DlgNodeKind::Reply,
                                    ref.ownerIndex});
}

GffStruct* DlgDocument::link(DlgLinkRef ref) {
    GffList* list = linkList(ref);
    return list && ref.position < list->count() ? list->GetStruct(ref.position) : nullptr;
}

const GffStruct* DlgDocument::link(DlgLinkRef ref) const {
    const GffList* list = linkList(ref);
    return list && ref.position < list->count() ? list->GetStruct(ref.position) : nullptr;
}

std::vector<DlgLinkRef> DlgDocument::startingLinks() const {
    std::vector<DlgLinkRef> result;
    const GffStruct* rootStruct = root();
    const GffList* list = rootStruct ? findList(*rootStruct, "StartingList") : nullptr;
    if (!list) return result;
    result.reserve(list->count());
    for (std::size_t i = 0; i < list->count(); ++i) result.push_back({DlgLinkOwner::StartingList, 0, i});
    return result;
}

std::vector<DlgLinkRef> DlgDocument::outgoingLinks(DlgNodeRef parent) const {
    std::vector<DlgLinkRef> result;
    const GffList* list = childLinkList(parent);
    if (!list) return result;
    result.reserve(list->count());
    const DlgLinkOwner owner = parent.kind == DlgNodeKind::Entry ? DlgLinkOwner::Entry : DlgLinkOwner::Reply;
    for (std::size_t i = 0; i < list->count(); ++i) result.push_back({owner, parent.index, i});
    return result;
}

std::optional<DlgNodeRef> DlgDocument::targetOf(DlgLinkRef ref) const {
    const GffStruct* linkStruct = link(ref);
    if (!linkStruct) return std::nullopt;
    const GffField* index = linkStruct->GetFieldByLabel("Index");
    if (!index) return std::nullopt;
    const std::size_t value = integralValue(index, std::numeric_limits<std::uint32_t>::max());
    const DlgNodeRef target{targetKind(ref.owner), value};
    if (!node(target)) return std::nullopt;
    return target;
}

std::vector<DlgLinkRef> DlgDocument::incomingLinks(DlgNodeRef target) const {
    std::vector<DlgLinkRef> result;
    auto collect = [&](const std::vector<DlgLinkRef>& links) {
        for (DlgLinkRef ref : links) {
            const auto candidate = targetOf(ref);
            if (candidate && *candidate == target) result.push_back(ref);
        }
    };
    if (target.kind == DlgNodeKind::Entry) collect(startingLinks());
    for (std::size_t i = 0; i < nodeCount(DlgNodeKind::Entry); ++i) {
        if (target.kind == DlgNodeKind::Reply) collect(outgoingLinks({DlgNodeKind::Entry, i}));
    }
    for (std::size_t i = 0; i < nodeCount(DlgNodeKind::Reply); ++i) {
        if (target.kind == DlgNodeKind::Entry) collect(outgoingLinks({DlgNodeKind::Reply, i}));
    }
    return result;
}

std::size_t DlgDocument::linkCount(DlgLinkRef ref) const {
    const GffList* list = linkList(ref);
    return list ? list->count() : 0;
}

DlgTextValue DlgDocument::text(DlgNodeRef ref) const {
    DlgTextValue result;
    const GffStruct* structure = node(ref);
    if (!structure) return result;
    const GffField* field = structure->GetFieldByLabel("Text");
    if (const auto* loc = dynamic_cast<const GffLocalizedStringField*>(field)) {
        result.strref = loc->strref;
        result.localText = loc->GetStringById(0);
    } else if (const auto* jade = dynamic_cast<const GffJadeStringRefField*>(field)) {
        result.stringType = jade->stringType;
        result.strref = jade->strref;
    }
    if (model_.tlk().loaded() && result.strref != 0xFFFFFFFFu) {
        const auto resolved = model_.tlk().resolve(result.strref);
        if (resolved) result.resolvedText = *resolved;
    }
    return result;
}

void DlgDocument::setText(DlgNodeRef ref, const DlgTextValue& value) {
    GffStruct* structure = node(ref);
    if (!structure) throw std::out_of_range("Dialogue node does not exist.");
    if (dialect() == DlgDialect::JadeEmpire) {
        auto* jade = dynamic_cast<GffJadeStringRefField*>(structure->GetFieldByLabel("Text"));
        if (!jade) {
            structure->AddField(std::make_unique<GffJadeStringRefField>("Text", value.stringType, value.strref));
        } else {
            jade->stringType = value.stringType;
            jade->strref = value.strref;
        }
    } else {
        auto* loc = dynamic_cast<GffLocalizedStringField*>(structure->GetFieldByLabel("Text"));
        if (!loc) {
            auto created = std::make_unique<GffLocalizedStringField>("Text", value.strref);
            loc = created.get();
            structure->AddField(std::move(created));
        }
        loc->strref = value.strref;
        if (value.localText.empty()) {
            loc->DeleteStringByID(0);
        } else {
            loc->SetStringByID(0, value.localText);
        }
    }
    markDirty();
}

std::string DlgDocument::speaker(DlgNodeRef ref) const {
    const GffStruct* structure = node(ref);
    if (!structure) return {};
    if (dialect() == DlgDialect::JadeEmpire) {
        const std::int32_t index = signedValue(structure->GetFieldByLabel("SpeakerIndex"), -1);
        const GffStruct* rootStruct = root();
        const GffList* tags = rootStruct ? findList(*rootStruct, "TagList") : nullptr;
        if (tags && index >= 0 && static_cast<std::size_t>(index) < tags->count()) {
            return stringValue(tags->GetStruct(static_cast<std::size_t>(index))->GetFieldByLabel("Tag"));
        }
        return index >= 0 ? "Speaker " + std::to_string(index) : std::string{};
    }
    std::string value = stringValue(structure->GetFieldByLabel("Speaker"));
    if (value.empty() && ref.kind == DlgNodeKind::Reply) value = "Player";
    return value;
}

std::string DlgDocument::nodeKindName(DlgNodeKind kind) const {
    return kind == DlgNodeKind::Entry ? "Entry" : "Reply";
}

std::string DlgDocument::nodeIdText(DlgNodeRef ref) const {
    const GffStruct* structure = node(ref);
    if (!structure) return {};
    if (const GffField* id = structure->GetFieldByLabel("NodeID")) return id->GetString();
    return std::to_string(ref.index);
}

std::string DlgDocument::nodeLabel(DlgNodeRef ref, std::size_t maxText) const {
    const DlgTextValue nodeText = text(ref);
    std::string visible = !nodeText.localText.empty() ? nodeText.localText : nodeText.resolvedText;
    if (visible.empty() && nodeText.strref != 0xFFFFFFFFu) visible = "StrRef " + std::to_string(nodeText.strref);
    if (visible.empty()) visible = "(no text)";
    std::string result = nodeKindName(ref.kind) + " " + std::to_string(ref.index);
    const std::string who = speaker(ref);
    if (!who.empty()) result += " [" + who + "]";
    result += ": " + ellipsize(visible, maxText);
    return result;
}

bool DlgDocument::hasNodeField(DlgNodeRef ref, const std::string& label) const {
    const GffStruct* structure = node(ref);
    return structure && structure->GetFieldByLabel(label);
}

std::string DlgDocument::nodeField(DlgNodeRef ref, const std::string& label) const {
    const GffStruct* structure = node(ref);
    return structure ? stringValue(structure->GetFieldByLabel(label)) : std::string{};
}

void DlgDocument::setNodeField(DlgNodeRef ref,
                               const std::string& label,
                               std::uint32_t fieldType,
                               const std::string& value) {
    GffStruct* structure = node(ref);
    if (!structure) throw std::out_of_range("Dialogue node does not exist.");
    setField(*structure, label, fieldType, value);
    markDirty();
}

void DlgDocument::removeNodeField(DlgNodeRef ref, const std::string& label) {
    GffStruct* structure = node(ref);
    if (!structure || !structure->GetFieldByLabel(label)) return;
    structure->DeleteField(label);
    markDirty();
}

bool DlgDocument::hasLinkField(DlgLinkRef ref, const std::string& label) const {
    const GffStruct* structure = link(ref);
    return structure && structure->GetFieldByLabel(label);
}

std::string DlgDocument::linkField(DlgLinkRef ref, const std::string& label) const {
    const GffStruct* structure = link(ref);
    return structure ? stringValue(structure->GetFieldByLabel(label)) : std::string{};
}

void DlgDocument::setLinkField(DlgLinkRef ref,
                               const std::string& label,
                               std::uint32_t fieldType,
                               const std::string& value) {
    GffStruct* structure = link(ref);
    if (!structure) throw std::out_of_range("Dialogue link does not exist.");
    setField(*structure, label, fieldType, value);
    markDirty();
}

void DlgDocument::removeLinkField(DlgLinkRef ref, const std::string& label) {
    GffStruct* structure = link(ref);
    if (!structure || !structure->GetFieldByLabel(label)) return;
    structure->DeleteField(label);
    markDirty();
}

bool DlgDocument::hasRootField(const std::string& label) const {
    const GffStruct* structure = root();
    return structure && structure->GetFieldByLabel(label);
}

std::string DlgDocument::rootField(const std::string& label) const {
    const GffStruct* structure = root();
    return structure ? stringValue(structure->GetFieldByLabel(label)) : std::string{};
}

void DlgDocument::setRootField(const std::string& label,
                               std::uint32_t fieldType,
                               const std::string& value) {
    GffStruct* structure = root();
    if (!structure) throw std::runtime_error("DLG root structure is unavailable.");
    setField(*structure, label, fieldType, value);
    markDirty();
}

void DlgDocument::removeRootField(const std::string& label) {
    GffStruct* structure = root();
    if (!structure || !structure->GetFieldByLabel(label)) return;
    structure->DeleteField(label);
    markDirty();
}

std::unique_ptr<GffStruct> DlgDocument::makeDefaultNode(DlgNodeKind kind) const {
    const GffList* list = nodeList(kind);
    if (list && list->count()) {
        std::unique_ptr<GffField> clonedField = list->GetStruct(0)->Clone();
        auto* clonedStruct = dynamic_cast<GffStruct*>(clonedField.release());
        if (!clonedStruct) throw std::runtime_error("Unable to clone a DLG node template.");
        std::unique_ptr<GffStruct> result(clonedStruct);
        clearClonedNode(*result, kind);
        return result;
    }

    auto result = std::make_unique<GffStruct>();
    if (dialect() == DlgDialect::JadeEmpire || flavor() == DlgFlavor::JadeEmpire) {
        if (kind == DlgNodeKind::Entry) {
            result->AddField(std::make_unique<GffList>("RepliesList"));
            result->AddField(std::make_unique<GffIntField>("SpeakerIndex", 0));
            result->AddField(std::make_unique<GffJadeStringRefField>("Text", 4, 0xFFFFFFFFu));
            result->AddField(std::make_unique<GffList>("AnimationList"));
            result->AddField(std::make_unique<GffResRefField>("Script", ""));
            result->AddField(std::make_unique<GffExoStringField>("VoiceOver", ""));
        } else {
            result->AddField(std::make_unique<GffList>("EntriesList"));
            result->AddField(std::make_unique<GffJadeStringRefField>("Text", 4, 0xFFFFFFFFu));
        }
        return result;
    }

    result->AddField(std::make_unique<GffExoStringField>("Speaker", ""));
    result->AddField(std::make_unique<GffExoStringField>("Listener", ""));
    result->AddField(std::make_unique<GffList>("AnimList"));
    result->AddField(std::make_unique<GffLocalizedStringField>("Text", 0xFFFFFFFFu));
    result->AddField(std::make_unique<GffResRefField>("VO_ResRef", ""));
    result->AddField(std::make_unique<GffResRefField>("Script", ""));
    result->AddField(std::make_unique<GffResRefField>("Sound", ""));
    result->AddField(std::make_unique<GffExoStringField>("Comment", ""));
    result->AddField(std::make_unique<GffExoStringField>("Quest", ""));
    result->AddField(std::make_unique<GffIntField>("PlotIndex", -1));
    result->AddField(std::make_unique<GffFloatField>("PlotXPPercentage", 1.0f));
    result->AddField(std::make_unique<GffUInt32Field>("WaitFlags", 0));
    result->AddField(std::make_unique<GffUInt32Field>("CameraAngle", 0));
    result->AddField(std::make_unique<GffUInt32Field>("Delay", 0xFFFFFFFFu));
    result->AddField(std::make_unique<GffList>(childListLabel(kind)));

    if (flavor() == DlgFlavor::Kotor2) {
        result->AddField(std::make_unique<GffResRefField>("Script2", ""));
        for (int i = 1; i <= 5; ++i) {
            result->AddField(std::make_unique<GffIntField>("ActionParam" + std::to_string(i), 0));
            result->AddField(std::make_unique<GffIntField>("ActionParam" + std::to_string(i) + "b", 0));
        }
        result->AddField(std::make_unique<GffExoStringField>("ActionParamStrA", ""));
        result->AddField(std::make_unique<GffExoStringField>("ActionParamStrB", ""));
        result->AddField(std::make_unique<GffIntField>("NodeUnskippable", 0));
        result->AddField(std::make_unique<GffIntField>("PostProcNode", 0));
        result->AddField(std::make_unique<GffIntField>("AlienRaceNode", 0));
        result->AddField(std::make_unique<GffIntField>("Emotion", 0));
        result->AddField(std::make_unique<GffIntField>("RecordVO", 0));
        result->AddField(std::make_unique<GffIntField>("RecordNoVOOverri", 0));
        result->AddField(std::make_unique<GffIntField>("FacialAnim", 0));
        result->AddField(std::make_unique<GffIntField>("CameraID", 0));
        result->AddField(std::make_unique<GffIntField>("CamVidEffect", -1));
        result->AddField(std::make_unique<GffByteField>("FadeType", 0));
        result->AddField(std::make_unique<GffIntField>("NodeID", 0));
        result->AddField(std::make_unique<GffByteField>("VOTextChanged", 0));
        result->AddField(std::make_unique<GffByteField>("Changed", 0));
    }
    return result;
}

void DlgDocument::clearClonedNode(GffStruct& structure, DlgNodeKind kind) const {
    for (const char* label : {"Speaker", "Listener", "VO_ResRef", "Script", "Script2", "Sound", "Comment", "Quest",
                              "ActionParamStrA", "ActionParamStrB", "VoiceOver", "ScriptCamEntry", "ScriptCamReplies", "ScriptEntry"}) {
        clearStringField(structure, label);
    }
    clearList(structure, childListLabel(kind));
    clearList(structure, "AnimList");
    clearList(structure, "AnimationList");

    if (auto* loc = dynamic_cast<GffLocalizedStringField*>(structure.GetFieldByLabel("Text"))) {
        loc->strref = 0xFFFFFFFFu;
        loc->substrings.clear();
        loc->stringcount = 0;
    }
    if (auto* jade = dynamic_cast<GffJadeStringRefField*>(structure.GetFieldByLabel("Text"))) {
        jade->stringType = 4;
        jade->strref = 0xFFFFFFFFu;
    }

    for (int i = 1; i <= 5; ++i) {
        setIntegralIfPresent(structure, "ActionParam" + std::to_string(i), 0);
        setIntegralIfPresent(structure, "ActionParam" + std::to_string(i) + "b", 0);
    }
    for (const char* label : {"NodeUnskippable", "PostProcNode", "AlienRaceNode", "Emotion", "RecordVO", "RecordNoVOOverri",
                              "RecordNoOverri", "FacialAnim", "CameraID", "CameraAngle", "FadeType", "WaitFlags", "SoundExists",
                              "VOTextChanged", "Changed", "SpeakerIndex"}) {
        setIntegralIfPresent(structure, label, 0);
    }
    setIntegralIfPresent(structure, "CamVidEffect", -1);
    setIntegralIfPresent(structure, "PlotIndex", -1);
    setIntegralIfPresent(structure, "QuestEntry", 0);
    setIntegralIfPresent(structure, "Delay", 0xFFFFFFFFu);
    if (auto* plot = dynamic_cast<GffFloatField*>(structure.GetFieldByLabel("PlotXPPercentage"))) plot->value = 1.0f;
}

std::unique_ptr<GffStruct> DlgDocument::makeDefaultLink(DlgLinkOwner owner, std::size_t targetIndex) const {
    auto result = std::make_unique<GffStruct>();
    result->AddField(std::make_unique<GffUInt32Field>("Index", static_cast<std::uint32_t>(targetIndex)));
    if (dialect() == DlgDialect::JadeEmpire) {
        result->AddField(std::make_unique<GffResRefField>("Active", ""));
        result->AddField(std::make_unique<GffIntField>("DesignerNumber", 0));
        result->AddField(std::make_unique<GffByteField>("ReverseCond", 0));
        return result;
    }

    result->AddField(std::make_unique<GffResRefField>("Active", ""));
    result->AddField(std::make_unique<GffByteField>("IsChild", 0));
    if (flavor() == DlgFlavor::Kotor2) {
        result->AddField(std::make_unique<GffResRefField>("Active2", ""));
        for (int i = 1; i <= 5; ++i) {
            result->AddField(std::make_unique<GffIntField>("Param" + std::to_string(i), 0));
            result->AddField(std::make_unique<GffIntField>("Param" + std::to_string(i) + "b", 0));
        }
        result->AddField(std::make_unique<GffByteField>("Not", 0));
        result->AddField(std::make_unique<GffByteField>("Not2", 0));
        result->AddField(std::make_unique<GffIntField>("Logic", 0));
        result->AddField(std::make_unique<GffExoStringField>("ParamStrA", ""));
        result->AddField(std::make_unique<GffExoStringField>("ParamStrB", ""));
    }
    (void)owner;
    return result;
}

void DlgDocument::assignFreshNodeId(GffStruct& structure) {
    GffStruct* rootStruct = root();
    if (!rootStruct) return;
    auto* next = dynamic_cast<GffIntField*>(rootStruct->GetFieldByLabel("NextNodeID"));
    if (!next) return;
    setField(structure, "NodeID", FIELD_TYPE_INT, std::to_string(next->value));
    ++next->value;
}

void DlgDocument::reindexList(GffList& list) const {
    for (std::size_t i = 0; i < list.count(); ++i) {
        if (GffStruct* structure = list.GetStruct(i)) structure->typeid_ = static_cast<std::uint32_t>(i);
    }
}

DlgNodeRef DlgDocument::addStartingEntry() {
    GffList* entries = nodeList(DlgNodeKind::Entry);
    GffStruct* rootStruct = root();
    GffList* starts = rootStruct ? findList(*rootStruct, "StartingList") : nullptr;
    if (!entries || !starts) throw std::runtime_error("The DLG does not contain the required EntryList and StartingList.");
    auto created = makeDefaultNode(DlgNodeKind::Entry);
    created->typeid_ = static_cast<std::uint32_t>(entries->count());
    assignFreshNodeId(*created);
    const DlgNodeRef ref{DlgNodeKind::Entry, entries->count()};
    entries->AddStruct(std::move(created));
    auto linkStruct = makeDefaultLink(DlgLinkOwner::StartingList, ref.index);
    linkStruct->typeid_ = static_cast<std::uint32_t>(starts->count());
    starts->AddStruct(std::move(linkStruct));
    markDirty();
    return ref;
}

DlgNodeRef DlgDocument::addChildNode(DlgNodeRef parent) {
    if (!node(parent)) throw std::out_of_range("Parent dialogue node does not exist.");
    const DlgNodeKind kind = childKind(parent.kind);
    GffList* nodes = nodeList(kind);
    GffList* links = childLinkList(parent, true);
    if (!nodes || !links) throw std::runtime_error("The DLG is missing a required node or child-link list.");
    auto created = makeDefaultNode(kind);
    created->typeid_ = static_cast<std::uint32_t>(nodes->count());
    assignFreshNodeId(*created);
    const DlgNodeRef ref{kind, nodes->count()};
    nodes->AddStruct(std::move(created));
    const DlgLinkOwner owner = parent.kind == DlgNodeKind::Entry ? DlgLinkOwner::Entry : DlgLinkOwner::Reply;
    auto linkStruct = makeDefaultLink(owner, ref.index);
    linkStruct->typeid_ = static_cast<std::uint32_t>(links->count());
    links->AddStruct(std::move(linkStruct));
    markDirty();
    return ref;
}

DlgLinkRef DlgDocument::linkExistingStartingEntry(DlgNodeRef entry) {
    if (entry.kind != DlgNodeKind::Entry || !node(entry)) throw std::invalid_argument("StartingList can only link to an existing Entry.");
    GffStruct* rootStruct = root();
    GffList* starts = rootStruct ? findList(*rootStruct, "StartingList") : nullptr;
    if (!starts) throw std::runtime_error("The DLG does not contain StartingList.");
    auto linkStruct = makeDefaultLink(DlgLinkOwner::StartingList, entry.index);
    linkStruct->typeid_ = static_cast<std::uint32_t>(starts->count());
    const DlgLinkRef ref{DlgLinkOwner::StartingList, 0, starts->count()};
    starts->AddStruct(std::move(linkStruct));
    markDirty();
    return ref;
}

DlgLinkRef DlgDocument::linkExistingChild(DlgNodeRef parent, DlgNodeRef child) {
    if (!node(parent) || !node(child)) throw std::invalid_argument("Both the parent and child dialogue nodes must exist.");
    if (child.kind != childKind(parent.kind)) throw std::invalid_argument("Dialogue links must alternate between Entry and Reply nodes.");
    GffList* links = childLinkList(parent, true);
    if (!links) throw std::runtime_error("Unable to create the child-link list.");
    const DlgLinkOwner owner = parent.kind == DlgNodeKind::Entry ? DlgLinkOwner::Entry : DlgLinkOwner::Reply;
    auto linkStruct = makeDefaultLink(owner, child.index);
    linkStruct->typeid_ = static_cast<std::uint32_t>(links->count());
    const DlgLinkRef ref{owner, parent.index, links->count()};
    links->AddStruct(std::move(linkStruct));
    markDirty();
    return ref;
}

DlgNodeRef DlgDocument::duplicateNode(DlgNodeRef source) {
    const GffStruct* original = node(source);
    GffList* list = nodeList(source.kind);
    if (!original || !list) throw std::out_of_range("Dialogue node does not exist.");
    std::unique_ptr<GffField> clonedField = original->Clone();
    auto* clonedStruct = dynamic_cast<GffStruct*>(clonedField.release());
    if (!clonedStruct) throw std::runtime_error("Unable to duplicate the dialogue node.");
    std::unique_ptr<GffStruct> clone(clonedStruct);
    clone->typeid_ = static_cast<std::uint32_t>(list->count());
    assignFreshNodeId(*clone);
    const DlgNodeRef result{source.kind, list->count()};
    list->AddStruct(std::move(clone));
    markDirty();
    return result;
}

void DlgDocument::removeLink(DlgLinkRef ref) {
    GffList* list = linkList(ref);
    if (!list || ref.position >= list->count()) throw std::out_of_range("Dialogue link does not exist.");
    list->allStructs().erase(list->allStructs().begin() + static_cast<std::ptrdiff_t>(ref.position));
    reindexList(*list);
    markDirty();
}

void DlgDocument::moveLink(DlgLinkRef ref, int delta) {
    GffList* list = linkList(ref);
    if (!list || ref.position >= list->count()) throw std::out_of_range("Dialogue link does not exist.");
    const std::ptrdiff_t target = static_cast<std::ptrdiff_t>(ref.position) + delta;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(list->count())) return;
    std::swap(list->allStructs()[ref.position], list->allStructs()[static_cast<std::size_t>(target)]);
    reindexList(*list);
    markDirty();
}

void DlgDocument::copyLinkProperties(DlgLinkRef source, DlgLinkRef destination) {
    const GffStruct* sourceStruct = link(source);
    GffStruct* destinationStruct = link(destination);
    GffList* destinationList = linkList(destination);
    if (!sourceStruct || !destinationStruct || !destinationList) {
        throw std::out_of_range("Dialogue link does not exist.");
    }

    const std::uint32_t destinationIndex = integralValue(
        destinationStruct->GetFieldByLabel("Index"), std::numeric_limits<std::uint32_t>::max());
    std::unique_ptr<GffField> clonedField = sourceStruct->Clone();
    auto* clonedStruct = dynamic_cast<GffStruct*>(clonedField.release());
    if (!clonedStruct) throw std::runtime_error("Unable to duplicate dialogue link properties.");
    std::unique_ptr<GffStruct> clone(clonedStruct);
    clone->typeid_ = static_cast<std::uint32_t>(destination.position);
    setField(*clone, "Index", FIELD_TYPE_DWORD, std::to_string(destinationIndex));
    destinationList->allStructs()[destination.position] = std::move(clone);
    markDirty();
}

void DlgDocument::normalizeAllLinkIndicesAfterDelete(DlgNodeRef deleted) {
    auto adjustList = [&](GffList* list, DlgNodeKind targetType) {
        if (!list || targetType != deleted.kind) return;
        auto& values = list->allStructs();
        values.erase(std::remove_if(values.begin(), values.end(), [&](const std::unique_ptr<GffStruct>& item) {
            if (!item) return true;
            const std::uint32_t index = integralValue(item->GetFieldByLabel("Index"), 0xFFFFFFFFu);
            return index == deleted.index;
        }), values.end());
        for (auto& item : values) {
            if (!item) continue;
            auto* index = dynamic_cast<GffUInt32Field*>(item->GetFieldByLabel("Index"));
            if (index && index->value > deleted.index) --index->value;
        }
        reindexList(*list);
    };

    GffStruct* rootStruct = root();
    adjustList(rootStruct ? findList(*rootStruct, "StartingList") : nullptr, DlgNodeKind::Entry);
    GffList* entries = nodeList(DlgNodeKind::Entry);
    if (entries) {
        for (auto& entry : entries->allStructs()) {
            if (entry) adjustList(findList(*entry, "RepliesList"), DlgNodeKind::Reply);
        }
    }
    GffList* replies = nodeList(DlgNodeKind::Reply);
    if (replies) {
        for (auto& reply : replies->allStructs()) {
            if (reply) adjustList(findList(*reply, "EntriesList"), DlgNodeKind::Entry);
        }
    }
}

void DlgDocument::deleteNodeEverywhere(DlgNodeRef ref) {
    GffList* list = nodeList(ref.kind);
    if (!list || ref.index >= list->count()) throw std::out_of_range("Dialogue node does not exist.");
    normalizeAllLinkIndicesAfterDelete(ref);
    list->allStructs().erase(list->allStructs().begin() + static_cast<std::ptrdiff_t>(ref.index));
    reindexList(*list);
    markDirty();
}

std::vector<DlgAnimation> DlgDocument::animations(DlgNodeRef ref) const {
    std::vector<DlgAnimation> result;
    const GffStruct* structure = node(ref);
    if (!structure) return result;
    const char* label = dialect() == DlgDialect::JadeEmpire ? "AnimationList" : "AnimList";
    const GffList* list = findList(*structure, label);
    if (!list) return result;
    result.reserve(list->count());
    for (const auto& item : list->allStructs()) {
        if (!item) continue;
        DlgAnimation animation;
        animation.participant = stringValue(item->GetFieldByLabel("Participant"));
        animation.animation = signedValue(item->GetFieldByLabel("Animation"));
        animation.emotion = signedValue(item->GetFieldByLabel("Emotion"));
        result.push_back(std::move(animation));
    }
    return result;
}

void DlgDocument::replaceAnimations(DlgNodeRef ref, const std::vector<DlgAnimation>& values) {
    GffStruct* structure = node(ref);
    if (!structure) throw std::out_of_range("Dialogue node does not exist.");
    const bool jade = dialect() == DlgDialect::JadeEmpire;
    const std::string label = jade ? "AnimationList" : "AnimList";
    GffList* list = findList(*structure, label);
    if (!list) {
        auto created = std::make_unique<GffList>(label);
        list = created.get();
        structure->AddField(std::move(created));
    }
    list->allStructs().clear();
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto item = std::make_unique<GffStruct>();
        item->typeid_ = static_cast<std::uint32_t>(i);
        if (jade) {
            item->AddField(std::make_unique<GffIntField>("Index", static_cast<std::int32_t>(i)));
            item->AddField(std::make_unique<GffWordField>("Animation", static_cast<std::uint16_t>(values[i].animation)));
            item->AddField(std::make_unique<GffWordField>("Emotion", static_cast<std::uint16_t>(values[i].emotion)));
        } else {
            item->AddField(std::make_unique<GffExoStringField>("Participant", values[i].participant));
            item->AddField(std::make_unique<GffWordField>("Animation", static_cast<std::uint16_t>(values[i].animation)));
        }
        list->AddStruct(std::move(item));
    }
    markDirty();
}

std::vector<DlgStunt> DlgDocument::stunts() const {
    std::vector<DlgStunt> result;
    const GffStruct* rootStruct = root();
    const GffList* list = rootStruct ? findList(*rootStruct, "StuntList") : nullptr;
    if (!list) return result;
    result.reserve(list->count());
    for (const auto& item : list->allStructs()) {
        if (!item) continue;
        result.push_back({stringValue(item->GetFieldByLabel("Participant")),
                          stringValue(item->GetFieldByLabel("StuntModel"))});
    }
    return result;
}

void DlgDocument::replaceStunts(const std::vector<DlgStunt>& values) {
    GffStruct* rootStruct = root();
    if (!rootStruct) throw std::runtime_error("DLG root structure is unavailable.");
    GffList* list = findList(*rootStruct, "StuntList");
    if (!list) {
        auto created = std::make_unique<GffList>("StuntList");
        list = created.get();
        rootStruct->AddField(std::move(created));
    }
    list->allStructs().clear();
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto item = std::make_unique<GffStruct>();
        item->typeid_ = static_cast<std::uint32_t>(i);
        item->AddField(std::make_unique<GffExoStringField>("Participant", values[i].participant));
        item->AddField(std::make_unique<GffResRefField>("StuntModel", values[i].model));
        list->AddStruct(std::move(item));
    }
    markDirty();
}

std::vector<std::string> DlgDocument::speakerTags() const {
    std::vector<std::string> result;
    const GffStruct* rootStruct = root();
    const GffList* list = rootStruct ? findList(*rootStruct, "TagList") : nullptr;
    if (!list) return result;
    result.reserve(list->count());
    for (const auto& item : list->allStructs()) {
        result.push_back(item ? stringValue(item->GetFieldByLabel("Tag")) : std::string{});
    }
    return result;
}

void DlgDocument::replaceSpeakerTags(const std::vector<std::string>& values) {
    GffStruct* rootStruct = root();
    if (!rootStruct) throw std::runtime_error("DLG root structure is unavailable.");

    const std::vector<std::string> previous = speakerTags();
    GffList* list = findList(*rootStruct, "TagList");
    if (!list) {
        auto created = std::make_unique<GffList>("TagList");
        list = created.get();
        rootStruct->AddField(std::move(created));
    }
    list->allStructs().clear();
    for (std::size_t i = 0; i < values.size(); ++i) {
        auto item = std::make_unique<GffStruct>();
        item->typeid_ = static_cast<std::uint32_t>(i);
        item->AddField(std::make_unique<GffExoStringField>("Tag", values[i]));
        list->AddStruct(std::move(item));
    }

    // Jade nodes refer to TagList by position. Preserve those references by
    // matching the previous tag text to its new position; deleted speakers
    // become unassigned instead of silently pointing at a different tag.
    GffList* entries = nodeList(DlgNodeKind::Entry);
    if (entries) {
        for (auto& entry : entries->allStructs()) {
            if (!entry) continue;
            auto* indexField = dynamic_cast<GffIntField*>(entry->GetFieldByLabel("SpeakerIndex"));
            if (!indexField) continue;
            const std::int32_t oldIndex = indexField->value;
            if (oldIndex < 0 || static_cast<std::size_t>(oldIndex) >= previous.size()) continue;
            const std::string& oldTag = previous[static_cast<std::size_t>(oldIndex)];
            const auto found = std::find(values.begin(), values.end(), oldTag);
            indexField->value = found == values.end()
                                    ? -1
                                    : static_cast<std::int32_t>(std::distance(values.begin(), found));
        }
    }
    markDirty();
}

std::vector<DlgNodeRef> DlgDocument::search(const std::string& term) const {
    const std::string needle = lowerAscii(collapseWhitespace(term));
    std::vector<DlgNodeRef> result;
    if (needle.empty()) return result;
    for (DlgNodeKind kind : {DlgNodeKind::Entry, DlgNodeKind::Reply}) {
        for (std::size_t index = 0; index < nodeCount(kind); ++index) {
            const DlgNodeRef ref{kind, index};
            const GffStruct* structure = node(ref);
            std::string haystack = nodeLabel(ref, 100000) + " " + nodeIdText(ref);
            if (structure) {
                for (const auto& field : structure->allFields()) {
                    if (!field || field->fieldtype == FIELD_TYPE_LIST || field->fieldtype == FIELD_TYPE_STRUCT || field->fieldtype == FIELD_TYPE_VOID) continue;
                    haystack += " " + field->GetLabel() + " " + field->GetString();
                }
            }
            if (lowerAscii(haystack).find(needle) != std::string::npos) result.push_back(ref);
        }
    }
    return result;
}

std::vector<DlgNodeRef> DlgDocument::unreachableNodes() const {
    std::unordered_set<DlgNodeRef, NodeHasher> reachable;
    std::deque<DlgNodeRef> queue;
    for (DlgLinkRef start : startingLinks()) {
        const auto target = targetOf(start);
        if (target && reachable.insert(*target).second) queue.push_back(*target);
    }
    while (!queue.empty()) {
        const DlgNodeRef current = queue.front();
        queue.pop_front();
        for (DlgLinkRef child : outgoingLinks(current)) {
            const auto target = targetOf(child);
            if (target && reachable.insert(*target).second) queue.push_back(*target);
        }
    }
    std::vector<DlgNodeRef> result;
    for (DlgNodeKind kind : {DlgNodeKind::Entry, DlgNodeKind::Reply}) {
        for (std::size_t index = 0; index < nodeCount(kind); ++index) {
            const DlgNodeRef ref{kind, index};
            if (!reachable.count(ref)) result.push_back(ref);
        }
    }
    return result;
}

std::vector<DlgIssue> DlgDocument::validate() const {
    std::vector<DlgIssue> issues;
    if (!semanticallyEditable()) {
        issues.push_back({DlgIssueSeverity::Error,
                          "This file is not a supported classic DLG V3 conversation graph. Use Raw GFF view for low-level access.",
                          std::nullopt,
                          std::nullopt});
        return issues;
    }
    if (startingLinks().empty()) {
        issues.push_back({DlgIssueSeverity::Warning, "The conversation has no starting links.", std::nullopt, std::nullopt});
    }

    auto inspectLinks = [&](const std::vector<DlgLinkRef>& links) {
        for (DlgLinkRef ref : links) {
            const GffStruct* structure = link(ref);
            if (!structure || !structure->GetFieldByLabel("Index")) {
                issues.push_back({DlgIssueSeverity::Error, "A dialogue link has no Index field.", std::nullopt, ref});
                continue;
            }
            if (!targetOf(ref)) {
                issues.push_back({DlgIssueSeverity::Error,
                                  "A dialogue link points outside its target node list.",
                                  std::nullopt,
                                  ref});
            }
        }
    };
    inspectLinks(startingLinks());
    for (DlgNodeKind kind : {DlgNodeKind::Entry, DlgNodeKind::Reply}) {
        for (std::size_t i = 0; i < nodeCount(kind); ++i) inspectLinks(outgoingLinks({kind, i}));
    }

    const auto unreachable = unreachableNodes();
    for (DlgNodeRef ref : unreachable) {
        issues.push_back({DlgIssueSeverity::Warning,
                          nodeKindName(ref.kind) + " " + std::to_string(ref.index) + " is unreachable from StartingList.",
                          ref,
                          std::nullopt});
    }

    std::unordered_map<std::int32_t, DlgNodeRef> nodeIds;
    for (DlgNodeKind kind : {DlgNodeKind::Entry, DlgNodeKind::Reply}) {
        for (std::size_t i = 0; i < nodeCount(kind); ++i) {
            const DlgNodeRef ref{kind, i};
            const GffStruct* structure = node(ref);
            if (!structure) continue;
            if (const GffField* idField = structure->GetFieldByLabel("NodeID")) {
                const std::int32_t id = signedValue(idField, -1);
                const auto insertion = nodeIds.emplace(id, ref);
                if (!insertion.second) {
                    issues.push_back({DlgIssueSeverity::Warning,
                                      "NodeID " + std::to_string(id) + " is used by more than one node.",
                                      ref,
                                      std::nullopt});
                }
            }
        }
    }

    std::stable_sort(issues.begin(), issues.end(), [](const DlgIssue& lhs, const DlgIssue& rhs) {
        return static_cast<int>(lhs.severity) > static_cast<int>(rhs.severity);
    });
    return issues;
}

DlgStatistics DlgDocument::statistics() const {
    DlgStatistics result;
    result.entries = nodeCount(DlgNodeKind::Entry);
    result.replies = nodeCount(DlgNodeKind::Reply);
    result.startingLinks = startingLinks().size();
    result.totalLinks = result.startingLinks;
    for (DlgNodeKind kind : {DlgNodeKind::Entry, DlgNodeKind::Reply}) {
        for (std::size_t i = 0; i < nodeCount(kind); ++i) result.totalLinks += outgoingLinks({kind, i}).size();
    }
    result.unreachableNodes = unreachableNodes().size();
    result.reachableNodes = result.entries + result.replies - result.unreachableNodes;
    return result;
}

void DlgDocument::markDirty() {
    model_.gff().dirty(true);
}

} // namespace neodlg
