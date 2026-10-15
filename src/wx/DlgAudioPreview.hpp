#pragma once

#include <neoshared/audio/Playback.hpp>
#include <neoshared/bif/ResourceDocument.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <variant>

namespace neodlg {

class DlgAudioPreview {
public:
    void play(const std::filesystem::path& root, const std::string& resref);
    void play(const neoshared::ResourceDocument& resource);

private:
    struct AudioIndex {
        explicit AudioIndex(const std::filesystem::path& root);
        std::shared_ptr<const neoshared::audio::PcmData> decode(const std::string& resref) const;
        neoshared::bif::KeyBifArchive key;
        neoshared::bif::LooseArchiveCatalog loose;
        std::map<std::string, std::variant<std::filesystem::path, std::size_t>> sources;
    };
    std::map<std::filesystem::path, AudioIndex> indexes_;
    neoshared::audio::AudioPlayback playback_;
};

} // namespace neodlg
