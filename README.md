# NeoDLG

[![CI](https://github.com/vrifftech/NeoDLG/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoDLG/actions/workflows/ci.yml)

NeoDLG is a purpose-built conversation editor for BioWare `DLG` resources. It edits the dialogue graph directly instead of presenting a DLG as a generic table of GFF fields.

The semantic editor supports:

- Star Wars: Knights of the Old Republic dialogue graphs.
- Star Wars: Knights of the Old Republic II dialogue graphs, including second scripts, conditional parameters, node IDs, camera fields, VO flags, emotions, and post-processing fields.
- Jade Empire dialogue graphs, including `TagList` speakers, Jade string references, voice-over IDs, Jade scripts, animations, emotions, and reversed conditions.
- Optional TLK lookup for resolved dialogue text.
- A raw GFF view for unusual fields and unsupported DLG schemas.

The design retains the useful workflow of the classic DLGEditor—entries, replies, starting links, conditionals, scripts, animations, stunts, search, and orphan handling—while replacing its implementation-oriented tree editing with explicit node and link operations.

## Conversation editor

The main workspace has two views.

### Conversation

The conversation tree starts at `StartingList` and follows the actual Entry/Reply graph. It distinguishes:

- canonical nodes;
- additional links to an existing node;
- cycles;
- invalid links;
- nodes unreachable from any starting link.

Node operations are explicit:

- Add starting entry.
- Add alternating Entry/Reply child.
- Link an existing node without duplicating it.
- Duplicate a node as a new list entry.
- Remove only the selected link.
- Delete a node everywhere and repair affected indexes.
- Reorder sibling choices.

The inspector is divided by purpose rather than GFF layout:

- **Line** — speaker, listener, StrRef, local text, resolved TLK text, voice-over, and designer comment.
- **Scripts / Quest** — action scripts, camera scripts, quest fields, plot fields, strings, and integer parameters.
- **Presentation** — sound, delay, wait flags, cameras, animation-related fields, emotions, fades, post-processing, alien-race fields, and VO flags.
- **Link / Conditions** — condition scripts, negation, logic, string/integer parameters, link comments, Jade designer numbers, and reversed conditions.
- **Animations** — ordered animation records for the selected node.

**Dialogue > Conversation Properties** edits root conversation settings, KotOR stunt models, or Jade speaker tags. When Jade tags are reordered or removed, NeoDLG remaps `SpeakerIndex` references by tag instead of silently changing speakers.

**Dialogue > Validate Dialogue** reports invalid links, duplicate node IDs, missing starts, and unreachable nodes. Search covers visible text, TLK text, StrRefs, IDs, scripts, and other scalar node properties.

### Raw GFF

Raw GFF exposes the complete flattened document for fields not yet represented by the semantic inspector. The raw view is an advanced fallback, not the default editing model.

## Documents and interchange

NeoDLG supports multiple open documents, recent files, undo/redo snapshots, dark mode, scalable text, and the shared **Open Game Directory** registry from NeoShared.

Classic DLG documents can be imported or exported as hierarchical XML or JSON. NeoDLG can also create a DLG-aware TSLPatcher/HoloPatcher package from a clean original and a modified conversation.

## CLI

The command-line utility is `neodlg-cli`.

```text
neodlg-cli info <dlg> [--tlk dialog.tlk]
neodlg-cli dump <dlg> [filter-term] [--tlk dialog.tlk]
neodlg-cli search <dlg> <term> [--tlk dialog.tlk]
neodlg-cli export <dlg> <xml|json> <output>
neodlg-cli import <input-dlg> <output-dlg> <xml|json> <input-document>
neodlg-cli roundtrip <input-dlg> <output-dlg>
neodlg-cli diff-tslpatcher <original-dlg> <modified-dlg> <output-dir|fragment.ini> [options]
```

The low-level `set-value`, `add-field`, and `delete-field` commands remain available for automation and unusual fields.

## Repository layout

NeoDLG and NeoShared are independent repositories. Clone them as siblings:

```text
workspace/
  NeoShared/
  NeoDLG/
```

The build also accepts a lowercase sibling named `neoshared`. For another layout, pass the shared source path explicitly.

## Build

Linux:

```sh
./scripts/build.sh \
  --wx ON \
  --require-wx ON \
  --cli ON \
  --neoshared-root ../NeoShared \
  --jobs "$(nproc)"
```

Windows with the shared wxWidgets overlay:

```powershell
& ..\NeoShared\scripts\install-wxwidgets.ps1 `
  -VcpkgRoot C:\vcpkg `
  -Triplet x64-windows-static `
  -CleanAfterBuild

.\scripts\build.ps1 `
  -Clean `
  -Wx ON `
  -RequireWx ON `
  -Cli ON `
  -NeoSharedRoot ..\NeoShared `
  -VcpkgRoot C:\vcpkg `
  -VcpkgTriplet x64-windows-static `
  -Parallel ([Environment]::ProcessorCount)
```

Use `--wx OFF` or `-Wx OFF` for a core/CLI-only build.

## Platform icons

`resources/neodlg.svg` is the canonical artwork. Windows ICO, macOS ICNS, Linux PNG/scalable icons, and the wxWidgets fallback icon are derived from it.
