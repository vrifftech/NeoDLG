# NeoDLG

[![CI](https://github.com/vrifftech/NeoDLG/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoDLG/actions/workflows/ci.yml)

NeoDLG is a DLG-focused editor for BioWare conversation/dialog resources. It is layered on the shared GFF V3.2/V4 parser and editor backend, while keeping DLG-specific behavior in NeoDLG itself: `.dlg` defaults, native type `DLG ` for new documents, dialogue compatibility checks, and DLG-aware TSL/HoloPatcher export.

The GUI executable is named `NeoDLG`; the command-line utility is named `neodlg-cli`. NeoDLG still exposes the underlying raw GFF structure because DLG resources are GFF documents, but app-specific dialogue semantics stay outside the shared GFF and shared patcher libraries.

## GUI views

The GUI has two DLG/GFF element views:

- **Flat Grid View** shows the existing path/label/type/value table and remains the default for direct value editing.
- **Element Tree View** shows the same GFF elements as a hierarchical tree, similar to K-GFF's tree navigation, with expand/collapse commands in the View menu. Selecting a tree item also selects the corresponding model row so Add Field, Delete Selected, copy, paste, and value editing continue to use the same data model. Filtered trees keep placeholder parents when needed, and localized-string child entries are grouped under their owning field.

Choose **View > Flat Grid View** or **View > Element Tree View**. The selected view is remembered through the application's wxConfig settings and restored on the next launch.


## Dragon Age GFF V4 support

NeoDLG detects `GFF V4.0PC` and `GFF V4.1PC` headers and maps V4 numeric labels into readable tree paths where possible. Supported V4 scalar/list payloads include integers, floats, vectors, UTF-16 ECString values, DA2 TLK string references, structs, references, and lists. DA2 `TLK V0.5` files can be opened as GFF4 resources, and the optional TLK resolver can decode their Huffman-compressed UTF-16 string table for resolved StrRef previews. Large DA2 primitive lists, such as ARL terrain/visibility byte arrays, are represented compactly as raw primitive-list fields instead of being expanded into millions of GUI rows.

For GFF V4 files, NeoDLG currently preserves and rewrites the existing template schema and supports scalar value edits. Structural add/delete/rename operations are intentionally rejected for V4 files because V4 struct/field templates are compact schema descriptors rather than the V3.2 string-label tables. Use Neo2DA for spreadsheet-style GDA/G2DA row and column edits.

## CLI usage

```text
neodlg-cli info <dlg> [--tlk dialog.tlk]
neodlg-cli dump <dlg> [filter-term] [--tlk dialog.tlk]
neodlg-cli search <dlg> <term> [--tlk dialog.tlk]
neodlg-cli roundtrip <input-dlg> <output-dlg>
neodlg-cli new <output-dlg> [file-type]
neodlg-cli set-value <input-dlg> <output-dlg> <path> <value>
neodlg-cli add-field <input-dlg> <output-dlg> <parent-path|.> <label> <type> [value] [struct-type-id]
neodlg-cli delete-field <input-dlg> <output-dlg> <path>
```

Paths use GFF labels separated by backslashes. List entries are addressed by numeric index, for example:

```text
InventoryList\0\Item
```

Localized strings expose editable child paths:

```text
LocalizedName(strref)
LocalizedName(lang0)
```

Supported field types are:

```text
Byte, Char, Word, Short, DWORD, Int, DWORD64, Int64, Float, Double,
CExoString, CResRef, CExoLocString, Void, Struct, List, Orientation, Position, JadeStringRef
```

## Build

This repository consumes shared code from the separate `neoshared` repository. Clone the repositories as siblings:

```text
workspace/
  neoshared/
  NeoDLG/
```

CMake automatically detects `../neoshared`. For another layout, pass `--neoshared-root /path/to/neoshared` to `build.sh`, `-NeoSharedRoot C:\path\to\neoshared` to `build.ps1`, or set `NEOSHARED_ROOT` directly.


Linux GUI build:

```sh
./scripts/build.sh --wx ON --require-wx ON --jobs "$(nproc)"
```

Linux CLI/core-only build:

```sh
./scripts/build.sh --wx OFF --jobs "$(nproc)"
```

Windows GUI build with the shared, pinned wxWidgets 3.3.3 overlay:

```powershell
& ..\neoshared\scripts\install-wxwidgets.ps1 `
  -VcpkgRoot C:\vcpkg `
  -Triplet x64-windows-static `
  -CleanAfterBuild

.\scripts\build.ps1 `
  -Wx ON `
  -RequireWx ON `
  -VcpkgRoot C:\vcpkg `
  -VcpkgTriplet x64-windows-static `
  -Parallel ([Environment]::ProcessorCount)
```

Use `-Wx OFF` on Windows for a CLI/core-only build. The default build directory is `build/`.

## Tabular import/export

`neodlg-cli` can `search`, `export`, and `import` semantic XML and JSON. `info`, `dump`, and `search` also accept `--tlk dialog.tlk` to show resolved TLK text for CExoLocString StrRefs, Jade/DA2 string-reference fields, and obvious StrRef numeric fields. TLK loading is optional. Classic TLK `V3.0`/`V4.0` and DA2 GFF4 `TLK V0.5` are supported for lookup. The GUI remembers the last TLK opened with **Open optional TLK...** and attempts to auto-load it on the next launch so StrRefs resolve automatically, but files open and edit without one. XML and JSON use complete hierarchical typed GFF documents (`<gff3 type=...><struct id=...>...</struct></gff3>` for XML, and the analogous semantic JSON tree). CSV/TSV flattened GFF value-table import/export is intentionally not exposed because it does not preserve the semantic structure of GFF data. The GUI provides **Open optional TLK**, **Import XML**, **Import JSON**, **Export XML**, **Export JSON**, filter/search, and cell copy/paste actions for editable value rows. Hierarchical XML/JSON export is intentionally unfiltered so it remains a complete GFF document.

## TSLPatcher/HoloPatcher output

Generate DLG patcher instructions from an original clean DLG and a modified DLG:

```sh
neodlg-cli diff-tslpatcher original.dlg modified.dlg tslpatchdata --package --filename edited.dlg
neodlg-cli diff-tslpatcher original.dlg modified.dlg dlg_fragment.ini --fragment --filename edited.dlg
```

For native `DLG` files, `diff-tslpatcher` is DLG-aware by default. New root `EntryList` and `ReplyList` nodes are emitted as appended structs with `TypeId=ListIndex` and a generated `2DAMEMORY#=ListIndex` token. Added `StartingList`, `RepliesList`, and `EntriesList` link structs also use `TypeId=ListIndex`. Their added `Index` fields are initially created with a placeholder value and a generated `2DAMEMORY#=!FieldPath` token; the file section then emits token-to-token assignments such as `2DAMEMORY8=2DAMEMORY7` to reconnect the dialogue graph dynamically after TSLPatcher/HoloPatcher knows the final appended list indexes.

This avoids the fixed EntryList/ReplyList index problem produced by generic GFF compare output and removes most of the manual ChangeEdit cleanup normally required for compatible dialogue-branch insertion. Editable scalar and localized-string changes still become direct field assignments under `[GFFList]`; unrelated added fields still become `AddFieldN` sections. Deleted fields, type changes, shrinking lists, and structural operations that cannot be expressed safely are reported as unsupported by default.

Package mode stages the clean original DLG as the patcher baseline asset next to `changes.ini`. This mirrors the usual TSLPatcher/HoloPatcher workflow: the patcher modifies the user's existing file when present, or copies the staged clean file as the blueprint when needed.

TSLPatcher/HoloPatcher append new list structs; they do not insert them into the middle of a list. NeoDLG therefore emits warning comments when a modified dialogue choice appears inserted or reordered. The generated output preserves graph connectivity with dynamic tokens, but visual list ordering may still be append-order unless the installer backend supports true insertion.

Use `--generic-gff` to force the older low-level generic GFF compare output for diagnostics. Use `--dlg-aware` to explicitly request the default DLG-aware mode. Patcher generation accepts imported modified-side GFF data: `--modified-format xml|json|gff|kotor|native|auto` or a known native GFF extension alias such as `gff`, `utc`, `dlg`, `jrl`, `qst2`, `sto`, `fsm`, `cwa`, `cre`, `pla`, or `trg`; `diff-tslpatcher-import` accepts the same formats. XML/JSON are full hierarchical GFF documents; native GFF files can also be compared directly.

The GUI exposes the same package flow under `Export -> Export TSL/HoloPatcher Package...`: choose the clean original DLG, confirm the patch target filename, and choose the `tslpatchdata` package folder.

DLG-aware patch generation requires matching GFF V3 DLG documents. GFF V4 files remain available for native scalar editing but are not passed to stock GFF/DLG patch handlers.

## Shared game directories

The wxWidgets application exposes **File > Open Game Directory**. Its submenu lists every saved game install from the shared `neoshared` settings store; selecting an entry opens this application's supported-file dialog with that installation as the starting folder. **Manage Game Directories...** adds, renames, rescans, activates, or removes shared entries, and changes are visible in every Neo tool.

## Continuous integration

GitHub Actions checks out `vrifftech/neoshared` beside this repository, then builds the full wxWidgets application on Ubuntu 24.04 and Windows Server 2025 with Visual Studio 2026. Successful non-pull-request runs publish staged Linux and Windows artifacts.

The shared dependency defaults to `neoshared/main`. Set the repository Actions variable `NEOSHARED_REF` to a release tag or commit SHA to pin normal CI builds. A manual workflow run can override the ref, and the workflow accepts the `neoshared-updated` repository-dispatch event for cross-repository compatibility checks.
