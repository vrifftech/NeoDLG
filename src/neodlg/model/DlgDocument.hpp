#pragma once

#include "core/AppModel.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neodlg {

enum class DlgDialect {
    Unsupported,
    Kotor,
    JadeEmpire,
};

enum class DlgFlavor {
    Kotor,
    Kotor2,
    JadeEmpire,
};

enum class DlgNodeKind {
    Entry,
    Reply,
};

struct DlgNodeRef {
    DlgNodeKind kind = DlgNodeKind::Entry;
    std::size_t index = 0;

    friend bool operator==(const DlgNodeRef& lhs, const DlgNodeRef& rhs) noexcept {
        return lhs.kind == rhs.kind && lhs.index == rhs.index;
    }
    friend bool operator!=(const DlgNodeRef& lhs, const DlgNodeRef& rhs) noexcept {
        return !(lhs == rhs);
    }
    friend bool operator<(const DlgNodeRef& lhs, const DlgNodeRef& rhs) noexcept {
        if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
        return lhs.index < rhs.index;
    }
};

enum class DlgLinkOwner {
    StartingList,
    Entry,
    Reply,
};

struct DlgLinkRef {
    DlgLinkOwner owner = DlgLinkOwner::StartingList;
    std::size_t ownerIndex = 0;
    std::size_t position = 0;

    friend bool operator==(const DlgLinkRef& lhs, const DlgLinkRef& rhs) noexcept {
        return lhs.owner == rhs.owner && lhs.ownerIndex == rhs.ownerIndex && lhs.position == rhs.position;
    }
};

struct DlgTextValue {
    std::uint32_t stringType = 4;
    std::uint32_t strref = 0xFFFFFFFFu;
    std::string localText;
    std::string resolvedText;
};

struct DlgAnimation {
    std::string participant;
    std::int32_t animation = 0;
    std::int32_t emotion = 0;
};

struct DlgStunt {
    std::string participant;
    std::string model;
};

enum class DlgIssueSeverity {
    Information,
    Warning,
    Error,
};

struct DlgIssue {
    DlgIssueSeverity severity = DlgIssueSeverity::Information;
    std::string message;
    std::optional<DlgNodeRef> node;
    std::optional<DlgLinkRef> link;
};

struct DlgStatistics {
    std::size_t entries = 0;
    std::size_t replies = 0;
    std::size_t startingLinks = 0;
    std::size_t totalLinks = 0;
    std::size_t reachableNodes = 0;
    std::size_t unreachableNodes = 0;
};

class DlgDocument {
public:
    explicit DlgDocument(GffModel& model) : model_(model) {}
    explicit DlgDocument(const GffModel& model) : model_(const_cast<GffModel&>(model)) {}

    DlgDialect dialect() const;
    DlgFlavor flavor() const;
    bool semanticallyEditable() const;
    void create(DlgFlavor flavor);

    GffStruct* root();
    const GffStruct* root() const;
    GffList* nodeList(DlgNodeKind kind);
    const GffList* nodeList(DlgNodeKind kind) const;
    GffStruct* node(DlgNodeRef ref);
    const GffStruct* node(DlgNodeRef ref) const;
    GffStruct* link(DlgLinkRef ref);
    const GffStruct* link(DlgLinkRef ref) const;

    std::size_t nodeCount(DlgNodeKind kind) const;
    std::vector<DlgLinkRef> startingLinks() const;
    std::vector<DlgLinkRef> outgoingLinks(DlgNodeRef parent) const;
    std::vector<DlgLinkRef> incomingLinks(DlgNodeRef target) const;
    std::optional<DlgNodeRef> targetOf(DlgLinkRef ref) const;
    std::size_t linkCount(DlgLinkRef ref) const;

    DlgTextValue text(DlgNodeRef ref) const;
    void setText(DlgNodeRef ref, const DlgTextValue& value);
    std::string speaker(DlgNodeRef ref) const;
    std::string nodeLabel(DlgNodeRef ref, std::size_t maxText = 100) const;
    std::string nodeKindName(DlgNodeKind kind) const;
    std::string nodeIdText(DlgNodeRef ref) const;

    bool hasNodeField(DlgNodeRef ref, const std::string& label) const;
    std::string nodeField(DlgNodeRef ref, const std::string& label) const;
    void setNodeField(DlgNodeRef ref,
                      const std::string& label,
                      std::uint32_t fieldType,
                      const std::string& value);
    void removeNodeField(DlgNodeRef ref, const std::string& label);

    bool hasLinkField(DlgLinkRef ref, const std::string& label) const;
    std::string linkField(DlgLinkRef ref, const std::string& label) const;
    void setLinkField(DlgLinkRef ref,
                      const std::string& label,
                      std::uint32_t fieldType,
                      const std::string& value);
    void removeLinkField(DlgLinkRef ref, const std::string& label);

    bool hasRootField(const std::string& label) const;
    std::string rootField(const std::string& label) const;
    void setRootField(const std::string& label,
                      std::uint32_t fieldType,
                      const std::string& value);
    void removeRootField(const std::string& label);

    DlgNodeRef addStartingEntry();
    DlgNodeRef addChildNode(DlgNodeRef parent);
    DlgLinkRef linkExistingStartingEntry(DlgNodeRef entry);
    DlgLinkRef linkExistingChild(DlgNodeRef parent, DlgNodeRef child);
    DlgNodeRef duplicateNode(DlgNodeRef source);
    void removeLink(DlgLinkRef ref);
    void moveLink(DlgLinkRef ref, int delta);
    void copyLinkProperties(DlgLinkRef source, DlgLinkRef destination);
    void deleteNodeEverywhere(DlgNodeRef ref);

    std::vector<DlgAnimation> animations(DlgNodeRef ref) const;
    void replaceAnimations(DlgNodeRef ref, const std::vector<DlgAnimation>& values);
    std::vector<DlgStunt> stunts() const;
    void replaceStunts(const std::vector<DlgStunt>& values);
    std::vector<std::string> speakerTags() const;
    void replaceSpeakerTags(const std::vector<std::string>& values);

    std::vector<DlgNodeRef> search(const std::string& term) const;
    std::vector<DlgNodeRef> unreachableNodes() const;
    std::vector<DlgIssue> validate() const;
    DlgStatistics statistics() const;

    void markDirty();

private:
    GffList* linkList(DlgLinkRef ref);
    const GffList* linkList(DlgLinkRef ref) const;
    GffList* childLinkList(DlgNodeRef parent, bool createIfMissing);
    const GffList* childLinkList(DlgNodeRef parent) const;

    std::unique_ptr<GffStruct> makeDefaultNode(DlgNodeKind kind) const;
    std::unique_ptr<GffStruct> makeDefaultLink(DlgLinkOwner owner, std::size_t targetIndex) const;
    void clearClonedNode(GffStruct& structure, DlgNodeKind kind) const;
    void assignFreshNodeId(GffStruct& structure);
    void reindexList(GffList& list) const;
    void normalizeAllLinkIndicesAfterDelete(DlgNodeRef deleted);

    static DlgNodeKind childKind(DlgNodeKind parent);
    static DlgNodeKind targetKind(DlgLinkOwner owner);
    static std::string nodeListLabel(DlgNodeKind kind);
    static std::string childListLabel(DlgNodeKind kind);

    GffModel& model_;
};

} // namespace neodlg
