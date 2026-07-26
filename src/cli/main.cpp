#include "core/AppModel.hpp"
#include "TabularData.hpp"
#include "core/GffJson.hpp"
#include "TslPatcher.hpp"
#include "DlgPatcher.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using neodlg::GffModel;

namespace {

void usage(std::ostream& out) {
    out << "NeoDLG DLG conversation editor CLI\n"
        << "\n"
        << "Usage:\n"
        << "  neodlg-cli info <dlg> [--tlk dialog.tlk]\n"
        << "  neodlg-cli dump <dlg> [filter-term] [--tlk dialog.tlk]\n"
        << "  neodlg-cli search <dlg> <term> [--tlk dialog.tlk]\n"
        << "  neodlg-cli export <dlg> <xml|json> <output>\n"
        << "  neodlg-cli import <input-dlg> <output-dlg> <xml|json> <input-document>\n"
        << "  neodlg-cli diff-tslpatcher <original-dlg> <modified-input> <output-dir|fragment.ini> [--modified-format xml|json|dlg|gff|kotor|native|auto] [--package|--fragment] [--filename name] [--allow-unsupported] [--generic-gff]\n"
        << "  neodlg-cli diff-tslpatcher-import <original-dlg> <modified-input> <xml|json|dlg|gff|kotor|native|auto> <output-dir|fragment.ini> [--package|--fragment] [--filename name] [--allow-unsupported] [--generic-gff]\n"
        << "  neodlg-cli roundtrip <input-dlg> <output-dlg>\n"
        << "  neodlg-cli new <output-dlg> [file-type]\n"
        << "  neodlg-cli set-value <input-dlg> <output-dlg> <path> <value>\n"
        << "  neodlg-cli add-field <input-dlg> <output-dlg> <parent-path|.> <label> <type> [value] [struct-type-id]\n"
        << "  neodlg-cli delete-field <input-dlg> <output-dlg> <path>\n"
        << "\n"
        << "Paths use GFF labels separated by backslashes, e.g. InventoryList\\0\\Item.\n"
        << "Use localized string suffixes such as FirstName(strref) or FirstName(lang0) with set-value.\n"
        << "Use --tlk dialog.tlk with info/dump/search to show resolved TLK text for StrRef fields.\n"
        << "Supported field types: ";
    const auto types = neodlg::supportedFieldTypeNames();
    for (std::size_t i = 0; i < types.size(); ++i) {
        if (i) out << ", ";
        out << types[i];
    }
    out << "\n";
}


struct PatchOutputOptions {
    bool package = true;
    bool allowUnsupported = false;
    bool genericGff = false;
    std::string patchFilename;
    std::string modifiedFormat = "auto";
};

PatchOutputOptions parsePatchOutputOptions(int argc, char** argv, int begin, const std::filesystem::path& original) {
    PatchOutputOptions options;
    options.patchFilename = neotsl::basenameForPatch(original);
    for (int i = begin; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--package") options.package = true;
        else if (arg == "--fragment") options.package = false;
        else if (arg == "--generic-gff") options.genericGff = true;
        else if (arg == "--dlg-aware") options.genericGff = false;
        else if (arg == "--filename") {
            if (i + 1 >= argc) throw std::runtime_error("--filename requires a value.");
            options.patchFilename = argv[++i];
        } else if (arg == "--allow-unsupported") options.allowUnsupported = true;
        else if (arg == "--modified-format" || arg == "--input-format") {
            if (i + 1 >= argc) throw std::runtime_error(arg + " requires a value.");
            options.modifiedFormat = argv[++i];
        }
        else throw std::runtime_error("Unknown diff-tslpatcher option: " + arg);
    }
    return options;
}

void writePatchOutput(const neotsl::PatchProject& project, const std::filesystem::path& output, const PatchOutputOptions& options) {
    if (!options.allowUnsupported) neotsl::throwIfUnsupported(project);
    else neotsl::printReport(project);
    if (options.package) neotsl::writePackage(project, output, true);
    else neotsl::writeFragment(project, output);
}

std::string normalizeGffTypeForCli(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool isDlgModel(const GffModel& model) {
    return normalizeGffTypeForCli(model.fileType()) == "dlg";
}

neotsl::PatchProject makePatcherProject(const GffModel& original,
                                        const GffModel& modified,
                                        const PatchOutputOptions& options,
                                        const std::filesystem::path& originalPath) {
    if (!options.genericGff && isDlgModel(original) && isDlgModel(modified)) {
        return neodlg::patcher::diffDlgPatcher(original.gff(), modified.gff(), options.patchFilename, options.package, originalPath);
    }
    return neotsl::diffGffFlatTable(original.toTable(), modified.toTable(), options.patchFilename, options.package, originalPath);
}

std::string normalizeParentPath(std::string path) {
    if (path == "." || path == "root" || path == "ROOT") return {};
    return path;
}

std::uint32_t parseStructTypeId(const std::string& text) {
    return neodlg::ParseUInt32Decimal(text);
}


struct PositionalAndTlk {
    std::vector<std::string> positional;
    std::optional<std::filesystem::path> tlk;
};

PositionalAndTlk splitTlkOption(int argc, char** argv, int begin) {
    PositionalAndTlk parsed;
    for (int i = begin; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--tlk") {
            if (i + 1 >= argc) throw std::runtime_error("--tlk requires a TLK filename.");
            parsed.tlk = std::filesystem::path(argv[++i]);
        } else {
            parsed.positional.push_back(arg);
        }
    }
    return parsed;
}

void applyOptionalTlk(GffModel& model, const std::optional<std::filesystem::path>& tlk) {
    if (tlk) model.loadTlk(*tlk);
}

void printInfo(const GffModel& model) {
    const auto rows = model.rows();
    std::size_t editable = 0;
    std::size_t containers = 0;
    for (const auto& row : rows) {
        if (row.editable) ++editable;
        if (row.type == "Struct" || row.type == "List") ++containers;
    }
    std::cout << "File: " << (model.filename().empty() ? std::string("(new)") : model.filename().string()) << "\n";
    std::cout << "Type: " << model.fileType() << "\n";
    std::cout << "Version: " << model.version() << "\n";
    std::cout << "Rows: " << rows.size() << "\n";
    std::cout << "Editable values: " << editable << "\n";
    std::cout << "Containers: " << containers << "\n";
    if (model.tlk().loaded()) {
        std::cout << "TLK: " << model.tlk().filename().string() << "\n";
        std::cout << "TLK entries: " << model.tlk().count() << "\n";
    }
}

void dumpRows(const GffModel& model, const std::string& filter = {}) {
    const auto table = filter.empty() ? model.toTable() : neotabular::filterRows(model.toTable(), filter);
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
        if (c) std::cout << '\t';
        std::cout << table.columns[c];
    }
    std::cout << '\n';
    for (const auto& row : table.rows) {
        for (std::size_t c = 0; c < row.size(); ++c) {
            if (c) std::cout << '\t';
            std::cout << row[c];
        }
        std::cout << '\n';
    }
}

std::string readTextFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("Unable to open input text file: " + file.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void writeTextFile(const std::filesystem::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Unable to open output text file: " + file.string());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) throw std::runtime_error("Unable to write output text file: " + file.string());
}


std::string lowerAsciiLocal(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string extensionImportFormat(const std::filesystem::path& path) {
    std::string ext = lowerAsciiLocal(path.extension().string());
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    if (ext == "xml" || ext == "json") return ext;
    return "native";
}

bool isNativeGffImportFormat(std::string formatName) {
    formatName = lowerAsciiLocal(std::move(formatName));
    return formatName == "native" || formatName == "kotor" || formatName == "gff" ||
           formatName == "utc" || formatName == "utd" || formatName == "ute" ||
           formatName == "uti" || formatName == "utm" || formatName == "utp" || formatName == "uts" ||
           formatName == "utt" || formatName == "utw" || formatName == "itp" || formatName == "bic" ||
           formatName == "btc" || formatName == "bti" || formatName == "btp" || formatName == "btt" ||
           formatName == "jrl" || formatName == "dlg" || formatName == "are" || formatName == "git" ||
           formatName == "ifo" || formatName == "pth" || formatName == "fac" || formatName == "gui" ||
           formatName == "sto" || formatName == "cwa" || formatName == "fsm" || formatName == "qst" ||
           formatName == "qst2" || formatName == "cre" || formatName == "pla" || formatName == "trg";
}

void loadGffFromImport(GffModel& model,
                            const std::filesystem::path& originalPath,
                            const std::filesystem::path& inputPath,
                            std::string formatName) {
    (void)originalPath;
    formatName = lowerAsciiLocal(std::move(formatName));
    if (formatName.empty() || formatName == "auto") formatName = extensionImportFormat(inputPath);

    if (isNativeGffImportFormat(formatName)) {
        model.load(inputPath);
        return;
    }

    const auto format = neotabular::parseFormat(formatName);
    if (format == neotabular::Format::Xml) {
        model.importXml(readTextFile(inputPath));
    } else if (format == neotabular::Format::Json) {
        model.importXml(neodlg::gffJsonToXml(readTextFile(inputPath)));
    } else {
        throw std::runtime_error("NeoDLG supports XML/JSON or native DLG/GFF import for patcher generation; CSV/TSV flattened imports are not supported.");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            usage(std::cerr);
            return 2;
        }

        const std::string command = argv[1];
        if (command == "help" || command == "--help" || command == "-h") {
            usage(std::cout);
            return 0;
        }

        if (command == "info") {
            const auto parsed = splitTlkOption(argc, argv, 2);
            if (parsed.positional.size() != 1) { usage(std::cerr); return 2; }
            GffModel model;
            model.load(parsed.positional[0]);
            applyOptionalTlk(model, parsed.tlk);
            printInfo(model);
            return 0;
        }

        if (command == "dump") {
            const auto parsed = splitTlkOption(argc, argv, 2);
            if (parsed.positional.size() < 1 || parsed.positional.size() > 2) { usage(std::cerr); return 2; }
            GffModel model;
            model.load(parsed.positional[0]);
            applyOptionalTlk(model, parsed.tlk);
            dumpRows(model, parsed.positional.size() == 2 ? parsed.positional[1] : std::string{});
            return 0;
        }

        if (command == "search") {
            const auto parsed = splitTlkOption(argc, argv, 2);
            if (parsed.positional.size() != 2) { usage(std::cerr); return 2; }
            GffModel model;
            model.load(parsed.positional[0]);
            applyOptionalTlk(model, parsed.tlk);
            dumpRows(model, parsed.positional[1]);
            return 0;
        }

        if (command == "export") {
            if (argc < 5 || argc > 6) { usage(std::cerr); return 2; }
            const auto format = neotabular::parseFormat(argv[3]);
            GffModel model;
            model.load(argv[2]);
            if (format == neotabular::Format::Xml) {
                if (argc == 6) {
                    throw std::invalid_argument("Hierarchical DLG XML export preserves hierarchy and does not support row filtering.");
                }
                writeTextFile(argv[4], model.toXml());
            } else if (format == neotabular::Format::Json) {
                if (argc == 6) {
                    throw std::invalid_argument("Semantic DLG JSON export preserves hierarchy and does not support row filtering.");
                }
                writeTextFile(argv[4], neodlg::gffXmlToJson(model.toXml()));
            } else {
                throw std::invalid_argument("NeoDLG exports only semantic XML or JSON. CSV/TSV flattened export is not supported for DLG files.");
            }
            return 0;
        }

        if (command == "import") {
            if (argc != 6) { usage(std::cerr); return 2; }
            const auto format = neotabular::parseFormat(argv[4]);
            GffModel model;
            if (format == neotabular::Format::Xml) {
                model.importXml(readTextFile(argv[5]));
            } else if (format == neotabular::Format::Json) {
                model.importXml(neodlg::gffJsonToXml(readTextFile(argv[5])));
            } else {
                throw std::invalid_argument("NeoDLG imports only semantic XML or JSON. CSV/TSV flattened import is not supported for DLG files.");
            }
            model.save(argv[3]);
            return 0;
        }


        if (command == "diff-tslpatcher" || command == "diff-tslpatcher-import") {
            if ((command == "diff-tslpatcher" && argc < 5) || (command == "diff-tslpatcher-import" && argc < 6)) { usage(std::cerr); return 2; }
            const fs::path originalPath = argv[2];
            const fs::path modifiedPath = argv[3];
            if (command == "diff-tslpatcher-import") {
                auto options = parsePatchOutputOptions(argc, argv, 6, originalPath);
                options.modifiedFormat = argv[4];
                GffModel original;
                original.load(originalPath);
                GffModel modified;
                loadGffFromImport(modified, originalPath, modifiedPath, options.modifiedFormat);
                auto project = makePatcherProject(original, modified, options, originalPath);
                writePatchOutput(project, argv[5], options);
                return 0;
            }
            const fs::path output = argv[4];
            const auto options = parsePatchOutputOptions(argc, argv, 5, originalPath);
            GffModel original;
            original.load(originalPath);
            GffModel modified;
                loadGffFromImport(modified, originalPath, modifiedPath, options.modifiedFormat);
            auto project = makePatcherProject(original, modified, options, originalPath);
            writePatchOutput(project, output, options);
            return 0;
        }

        if (command == "roundtrip") {
            if (argc != 4) { usage(std::cerr); return 2; }
            GffModel model;
            model.load(argv[2]);
            model.save(argv[3]);
            return 0;
        }

        if (command == "new") {
            if (argc < 3 || argc > 4) { usage(std::cerr); return 2; }
            const fs::path output = argv[2];
            const std::string type = argc == 4 ? std::string(argv[3]) : std::string("DLG ");
            GffModel model;
            model.newFile(type);
            model.save(output);
            return 0;
        }

        if (command == "set-value") {
            if (argc != 6) { usage(std::cerr); return 2; }
            GffModel model;
            model.load(argv[2]);
            model.setValue(argv[4], argv[5]);
            model.save(argv[3]);
            return 0;
        }

        if (command == "add-field") {
            if (argc < 7 || argc > 9) { usage(std::cerr); return 2; }
            GffModel model;
            model.load(argv[2]);
            const std::string value = argc >= 8 ? std::string(argv[7]) : std::string{};
            const std::uint32_t typeId = argc >= 9 ? parseStructTypeId(argv[8]) : 0u;
            model.addField(normalizeParentPath(argv[4]), argv[5], argv[6], value, typeId);
            model.save(argv[3]);
            return 0;
        }

        if (command == "delete-field") {
            if (argc != 5) { usage(std::cerr); return 2; }
            GffModel model;
            model.load(argv[2]);
            model.deleteField(argv[4]);
            model.save(argv[3]);
            return 0;
        }

        std::cerr << "Unknown command: " << command << "\n";
        usage(std::cerr);
        return 2;
    } catch (const std::exception& ex) {
        std::cerr << "NeoDLG error: " << ex.what() << "\n";
        return 1;
    }
}
