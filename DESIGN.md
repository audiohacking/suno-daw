# AceForge-Suno — Design and implementation notes

This document describes how the plugin is built and how it uses the Suno API. Use it when resuming work in a new session (e.g. Cursor) so the design and data flow are clear.

---

## 1. Repo layout (suno-daw)

When this code lives in **github.com/audiohacking/suno-daw**, the root is the plugin project:

```
suno-daw/
├── CMakeLists.txt          # Root: add_subdirectory(plugin)
├── SunoClient/             # HTTP client for Suno API (macOS: NSURLSession)
│   ├── SunoClient.hpp
│   └── SunoClientMac.mm
├── plugin/
│   ├── CMakeLists.txt      # JUCE FetchContent, SunoClient lib, juce_add_plugin(AceForgeSuno)
│   ├── PluginProcessor.h
│   ├── PluginProcessor.cpp
│   ├── PluginEditor.h
│   └── PluginEditor.cpp
├── .github/workflows/
│   └── build-and-release.yml
├── README.md
└── DESIGN.md               # This file
```

No AceForge or AceStudioReverse code is used; this is a Suno-only fork.

---

## 2. Suno API usage

- **Base URL:** `https://api.sunoapi.org`
- **Auth:** `Authorization: Bearer <API_KEY>` on all requests.
- **Endpoints used:**
  - `GET /api/v1/generate/credit` — check API key / credits.
  - `POST /api/v1/generate` — text-to-music (prompt, style, title, model, instrumental, etc.). Returns `taskId`. We poll for completion (no webhook).
  - `POST /api/v1/generate/upload-cover` — same params plus `uploadUrl` (audio we uploaded). Used for “Cover (from recorded)”.
  - `POST /api/v1/generate/add-vocals` — `uploadUrl` (instrumental) + prompt/style/title. Used for “Add Vocals (from recorded)”.
  - `GET /api/v1/generate/record-info?taskId=...` — poll task status; response has `status` and `sunoData[].audioUrl`. We consider `SUCCESS`/`success` as done and fetch the first `audioUrl`.
  - `POST /api/file-stream-upload` (multipart) — upload WAV bytes; response gives `fileUrl` (or `downloadUrl`) used as `uploadUrl` for cover/add-vocals.

Models: V4, V4_5, V4_5PLUS, V4_5ALL, V5 (exposed in UI as V4, V4.5, V4.5 Plus, V4.5 All, V5).

---

## 3. Processor (AceForgeSunoAudioProcessor)

- **State:** `Idle` | `Submitting` | `Running` | `Succeeded` | `Failed`. Only one job at a time; UI disables Generate/Cover/Add Vocals while busy.
- **API key:** Stored in plugin state (`getStateInformation` / `setStateInformation`) so it persists across sessions. On load, `setApiKey` is called and `checkCredits()` runs to set “Suno: connected”.
- **Recording (transport-driven):** Recording follows the DAW transport. In `processBlock`, `getPlayHead()->getPosition()->getIsPlaying()` is read. When transport goes from stopped to playing, a new segment capture starts (current buffer cleared). When transport goes from playing to stopped, the current buffer is pushed as a **recorded segment** (if at least ~1 s). Segments are stored in `segments_`; each has `buffer`, `sampleRate`, and optional `trimStartSamples` / `trimEndSamples`. The user selects one segment in the UI for Cover or Add Vocals; encoding uses `encodeSegmentAsWav(segmentIndex)` with trim applied.
- **Generate flow:** UI calls `startGenerate(prompt, style, title, customMode, instrumental, modelIndex)`. A detached thread: `checkCredits()` → `startGenerate(GenerateParams)` → poll `getTaskStatus(taskId)` until status is SUCCESS or failure → `fetchAudio(audioUrls[0])` → put bytes in `pendingWavBytes_` and `triggerAsyncUpdate()`.
- **Upload-Cover flow:** `startUploadCover(...)` checks that a segment is selected (`hasSelectedSegment()`), then thread: `encodeSegmentAsWav(jobSegmentIndex_)` → `uploadAudio(wavBytes, "recorded.wav")` → `startUploadCover(uploadUrl, GenerateParams)` → same poll/fetch/pendingWavBytes_/triggerAsyncUpdate.
- **Add-Vocals flow:** Same idea: selected segment → `encodeSegmentAsWav(jobSegmentIndex_)` → upload → `startAddVocals(AddVocalsParams)` → poll → fetch → pendingWavBytes_ → triggerAsyncUpdate.
- **API test:** "Test API" runs `startTestApi()`: check credits, then a minimal `startGenerate` (short prompt), poll, fetch audio, set `pendingIsTest_` and `triggerAsyncUpdate()`. `handleAsyncUpdate()` plays the audio and shows "API test passed" without saving to the library.
- **Message-thread completion:** `handleAsyncUpdate()` decodes the WAV (JUCE `AudioFormatManager` + `MemoryInputStream`), converts to stereo float, calls `pushSamplesToPlayback()` (resampling if needed). If not a test, saves a copy to the library as `suno_YYYYMMDD_HHMMSS.wav`. Sets state to `Succeeded`.
- **Playback:** Same double-buffer + `AbstractFifo` pattern as the AceForge Bridge: message thread writes into one of two buffers and flips; audio thread in `processBlock` checks `pendingPlaybackReady_`, copies the active buffer into the playback fifo, then reads from the fifo into the output. No playback position UI; playback runs until the buffer is consumed.
- **Host BPM:** In `processBlock`, `getPlayHead()->getPosition()->getBpm()` is read when available and stored in `hostBpm_`; the editor shows it as “BPM: 120.0” or “BPM: —”.

---

## 4. Editor (AceForgeSunoAudioProcessorEditor)

- **API key:** Text editor (password-style) + “Save” button → `setApiKey()` and status label shows connection.
- **Params:** Prompt, Style, Title (text); Model (combo: V4 … V5); Instrumental (toggle).
- **Actions:** Generate, Cover (from recorded), Add Vocals (from recorded). Cover/Add Vocals use the selected segment and are disabled when no segment is selected or when a job is running.
- **Library:** List of `getLibraryEntries()` (WAVs in `~/Library/Application Support/AceForgeSuno/Generations/`), sorted by modification time. Refresh, drag row to DAW timeline, double-click copies path; “Insert into DAW” opens Logic with the file; “Reveal in Finder” opens the folder.
- **Timer:** ~4 Hz to update status label, BPM label, segments list, selection sync, trim sliders, and button states from the processor.

---

## 5. Library directory

- Path: `~/Library/Application Support/AceForgeSuno/Generations/`
- Files: `suno_YYYYMMDD_HHMMSS.wav` (and any older naming if we change it). No separate metadata file; list is built by scanning `*.wav` and using filename and modification time for the list model.

---

## 6. CI (GitHub Actions)

- **File:** `.github/workflows/build-and-release.yml`
- **Triggers:** Push to `main`/`master`, release published, or workflow_dispatch (optional `release_tag` input).
- **Steps:** Checkout → CMake (Xcode, arm64) → Build → Find `AceForge-Suno.component` and `AceForge-Suno.vst3` → Zip + codesign (ad-hoc or `MACOS_SIGNING_IDENTITY`) → Build installer .pkg → Optionally codesign pkg (`MACOS_INSTALLER_SIGNING_IDENTITY`) → Upload to release (if tag) or as workflow artifact.
- **Artifacts:** `AceForgeSuno-macOS-AU-VST3.zip`, `AceForgeSuno-macOS-Installer.pkg`.

---

## 7. Pushing this code to github.com/audiohacking/suno-daw

This tree is designed so it can be the **root** of the **suno-daw** repo.

**Option A — Subtree push from AceStudioReverse**

If the code lives under `ml-bridge-suno/` in AceStudioReverse and you want to push that folder as the root of suno-daw:

```bash
# In AceStudioReverse repo
git remote add suno-daw https://github.com/audiohacking/suno-daw.git   # once
git add ml-bridge-suno/
git commit -m "Add AceForge-Suno plugin for suno-daw"
git subtree push --prefix=ml-bridge-suno suno-daw main
```

The **suno-daw** repo will then have at root: `CMakeLists.txt`, `SunoClient/`, `plugin/`, `.github/`, `README.md`, `DESIGN.md`. Clone suno-daw and build from that root with the same CMake commands (from repo root, no `ml-bridge-suno` prefix).

**Option B — Copy into a clone of suno-daw**

1. Clone: `git clone https://github.com/audiohacking/suno-daw.git && cd suno-daw`
2. Copy contents of `ml-bridge-suno/` from AceStudioReverse into the clone (so CMakeLists.txt, SunoClient, plugin, .github, README.md, DESIGN.md are at the root of the clone).
3. `git add . && git commit -m "AceForge-Suno plugin and CI" && git push origin main`

After either option, GitHub Actions will run on push and produce the plugin zip and installer as artifacts (or attach them to a release if you use a tag).

---

## 8. Possible next steps

- **Upload base URL:** If `api.sunoapi.org` file upload fails, try the alternate upload host from Suno docs (e.g. `sunoapiorg.redpandaai.co`) and make it configurable.
- **Extra Suno params:** Expose `negativeTags`, `vocalGender`, `styleWeight`, `weirdnessConstraint`, `audioWeight` in the UI and pass them through to `GenerateParams` / `AddVocalsParams`.
- **BPM in API:** If Suno adds a BPM/tempo parameter, pass `getHostBpm()` into the generate request.
- **Playback control:** Play/stop or loop for the current playback buffer.
- **Longer recordings:** Cap or trim recorded buffer size to avoid huge WAV uploads (e.g. max 5 minutes).
