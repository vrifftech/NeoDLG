#pragma once

#include "core/GFFFile.hpp"
#include "TslPatcher.hpp"

#include <filesystem>
#include <string>

namespace neodlg::patcher {

neotsl::PatchProject diffDlgPatcher(const neodlg::GffFile& original,
                                    const neodlg::GffFile& modified,
                                    const std::string& patchFilename,
                                    bool copyBaselineAsset = true,
                                    const std::filesystem::path& baselineAsset = {});

} // namespace neodlg::patcher
