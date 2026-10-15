#include "DlgAudioPreview.hpp"

#include <neoshared/PathUtf8.hpp>
#include <neoshared/game/Installation.hpp>

#include <algorithm>
#include <type_traits>
#include <vector>

namespace neodlg {
namespace {
std::string folded(std::string value) {
    for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return value;
}
std::string audioName(const std::filesystem::path& path) {
    return folded(neoshared::pathToUtf8(path.stem()));
}
} // namespace

DlgAudioPreview::AudioIndex::AudioIndex(const std::filesystem::path& root) {
    key.open(neoshared::game::findExistingPathCaseInsensitive(root / "chitin.key"));
    for (const auto& resource : key.resources()) {
        if (resource.extension == "wav" || resource.extension == "mp3")
            sources.emplace(folded(resource.resref), resource.index);
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const auto extension = folded(neoshared::pathToUtf8(entry.path().extension()));
        if (entry.is_regular_file() && (extension == ".wav" || extension == ".mp3"))
            files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) sources.insert_or_assign(audioName(file), file);
}

std::shared_ptr<const neoshared::audio::PcmData>
DlgAudioPreview::AudioIndex::decode(const std::string& resref) const {
    return std::visit([this](const auto& source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, std::filesystem::path>) {
            return neoshared::audio::decodeFile(source);
        } else {
            return neoshared::audio::decodeBytes(neoshared::bif::readResourceDocument(
                key, loose, {neoshared::bif::ResourceSource::KeyBif, source}).bytes);
        }
    }, sources.at(audioName(neoshared::pathFromUtf8(resref))));
}

void DlgAudioPreview::play(const std::filesystem::path& root, const std::string& resref) {
    auto& index = indexes_.try_emplace(root, root).first->second;
    playback_.load(index.decode(resref));
    playback_.play();
}

void DlgAudioPreview::play(const neoshared::ResourceDocument& resource) {
    playback_.load(neoshared::audio::decodeBytes(resource.bytes));
    playback_.play();
}

} // namespace neodlg
