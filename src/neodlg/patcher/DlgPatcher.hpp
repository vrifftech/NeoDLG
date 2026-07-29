#pragma once

#include "core/GFFFile.hpp"
#include "TslPatcher.hpp"

#include <filesystem>
#include <string>

namespace neodlg::patcher {

enum class DlgPatchMode {
    DynamicMerge,
    CompleteReplacement,
};

neotsl::PatchProject diffDlgPatcher(const neodlg::GffFile& original,
                                    neodlg::GffFile& modified,
                                    const std::string& patchFilename,
                                    DlgPatchMode mode = DlgPatchMode::DynamicMerge,
                                    bool packageOutput = true,
                                    const std::filesystem::path& baselineAsset = {},
                                    const std::string& destination = "override");

neotsl::PatchProject makeCompleteDlgReplacement(neodlg::GffFile& modified,
                                                  const std::string& patchFilename,
                                                  const std::string& destination = "override");


} // namespace neodlg::patcher
