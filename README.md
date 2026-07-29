# NeoDLG

[![CI](https://github.com/vrifftech/NeoDLG/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoDLG/actions/workflows/ci.yml)

NeoDLG is a purpose-built conversation editor for BioWare `DLG` resources. It edits the dialogue graph directly instead of presenting a DLG as a generic table of GFF fields.

The semantic editor supports:

- Star Wars: Knights of the Old Republic dialogue graphs.
- Star Wars: Knights of the Old Republic II dialogue graphs, including second scripts, conditional parameters, node IDs, camera fields, VO flags, emotions, and post-processing fields.
- Jade Empire dialogue graphs, including positional `TagList` participants, Jade string references, voice-over IDs, entry/reply camera scripts and tags, entry animation performers, reply animation/emotion pairs, skippable entries, designer numbers, and reversed conditions.
- Optional TLK lookup for resolved dialogue text.
- A structured GFF tree for unusual fields and unsupported DLG schemas.

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

The inspector is divided by purpose rather than GFF layout. It changes with the detected DLG dialect and node kind.

For KotOR and KotOR II:

- **Line** — speaker, listener, StrRef, local text, resolved TLK text, voice-over, and designer comment.
- **Scripts / Quest** — action scripts, quest fields, plot fields, strings, and integer parameters.
- **Presentation** — sound, delay, wait flags, cameras, animation-related fields, emotions, fades, post-processing, alien-race fields, and VO flags.
- **Link / Conditions** — condition scripts, negation, logic, string/integer parameters, and link comments.
- **Animations** — ordered animation records for the selected node.

For Jade Empire, only fields present in Jade's runtime DLG schema are shown:

- **Entry Line** — `SpeakerIndex`, `ListenerIndex`, Jade string type/StrRef, resolved TLK text, `VoiceOver`, and `Skippable`.
- **Entry Scripts** — `Script`, `ScriptEntry`, `ScriptCamEntry`, `CameraEntry`, `ScriptCamReplies`, and `CameraReplies`. Camera tags are stored lowercase, matching runtime comparison behavior.
- **Reply Line** — Jade string type/StrRef and resolved TLK text.
- **Reply Scripts** — the single Reply `Script` field.
- **Link / Conditions** — `Active`, `ReverseCond`, and `DesignerNumber`; KotOR's second condition, logic, parameter banks, and `IsChild` are hidden.
- **Entry Animations** — an ordered `AnimationList` whose `Index` selects a participant from the root `TagList`, independent of the animation row number.
- **Reply Animation** — the singular `Animation`/`Emotion` pair stored directly on the Reply; `65535` is the unset animation value.

KotOR-only presentation controls are hidden for Jade documents. The GFF Tree remains available for uncommon fields without presenting them as part of the Jade semantic schema.

For KotOR and KotOR II conversations, the Presentation page uses constrained runtime-aware controls:

- Camera angle is limited to Automatic, calculated presets 1–3, or Placed camera; an unknown value already present in a file remains preservable.
- Camera ID remains an exposed integer and is enabled for Placed camera mode. NeoDLG does not yet resolve IDs from the current area GIT.
- Camera and target height offsets accept finite signed decimal values.
- Camera field of view is either Automatic (`-1`) or a positive custom value in **degrees**.
- KotOR camera video effects are selected from rows `0`–`2` of `videoeffects.2da`, plus None (`-1`).
- KotOR II camera video effects are selected from rows `0`–`15` of `videoeffects.2da`, plus None (`-1`). Unknown existing rows remain preservable for both games.
- Fade type is Fade out (`0`) or Fade in (`1`); unknown existing nonzero values remain preservable.
- Fade color uses a picker and normalized red/green/blue values from `0.0` through `1.0`.
- Fade delay is labeled in **seconds** and must be zero or greater.
- Fade length is labeled in **seconds** and must be greater than zero.

The root conversation type is selected semantically as Normal, Computer, or Full conversation (disable the one-line bark shortcut). These KotOR-specific camera and fade controls are hidden for Jade Empire DLGs; unrepresented fields remain available in the GFF Tree.

**Dialogue > Conversation Properties** edits root conversation settings, KotOR stunt models, or Jade `EndConversation` and the ordered participant `TagList`. The Jade participant page supports add, edit, delete, and move up/down. When tags are renamed, reordered, or removed, NeoDLG remaps `SpeakerIndex`, `ListenerIndex`, and Entry-animation participant indexes by tag identity instead of silently retargeting actors.

**Dialogue > Validate Dialogue** reports invalid links, duplicate node IDs, missing starts, and unreachable nodes. Search covers visible text, TLK text, StrRefs, IDs, scripts, and other scalar node properties.

### GFF tree

The GFF tree exposes the complete document hierarchy for fields not yet represented by the semantic inspector. Double-click an editable value to change it. This structured tree is an advanced fallback, not the default dialogue-authoring model.

## Documents and interchange

NeoDLG supports multiple open documents, recent files, undo/redo snapshots, dark mode, scalable text, and the shared **Open Game Directory** registry from NeoShared.

Classic DLG documents can be imported or exported as hierarchical XML or JSON. NeoDLG can also create a DLG-aware TSLPatcher/HoloPatcher package from a clean original and a modified conversation.


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

