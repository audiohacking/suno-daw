# suno-daw

Experimental AU/VST3 plugin that controls the **Suno API** from your DAW. Insert it on the master (or any bus), set your API key, and generate or transform music using Suno’s Music Generation APIs.

## What it does

- **Generate** — Text-to-music: prompt + style + title → new track (plays back and is saved to the plugin library).
- **Cover** — Record audio in the DAW, then “Cover (from recorded)”: uploads the recording and asks Suno for a cover; result is played and saved.
- **Add Vocals** — Record an instrumental, then “Add Vocals (from recorded)”: Suno adds vocals; result is played and saved.
- **Library** — All generated/cover/vocal results are saved as WAVs under `~/Library/Application Support/AceForgeSuno/Generations/`. You can drag files into the DAW, “Insert into DAW” (Logic), or “Reveal in Finder”.
- **Host BPM** — When the host provides tempo, it’s shown in the UI (for reference; Suno API does not take BPM in the current implementation).

## Requirements

- **macOS** (Apple Silicon or Intel).
- **Suno API key** from [Suno API](https://docs.sunoapi.org/) (or your provider). Enter it in the plugin UI and click **Save**.

## Build (local)

From the **repo root** (suno-daw):

```bash
cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Then copy the built plugins to your system:

- **AU:** `build/plugin/AceForgeSuno_artefacts/Release/AU/AceForge-Suno.component` → `~/Library/Audio/Plug-Ins/Components/`
- **VST3:** `build/plugin/AceForgeSuno_artefacts/Release/VST3/AceForge-Suno.vst3` → `~/Library/Audio/Plug-Ins/VST3/`

Rescan plugins in your DAW.

### Build installer (.pkg and zip)

After building, run the installer script from the repo root to produce a zip and a macOS `.pkg`:

```bash
./scripts/build-installer-pkg.sh [--sign-plugins] [--version 0.1.0]
```

- **Output:** `release-artefacts/AceForgeSuno-macOS-AU-VST3.zip` and `release-artefacts/AceForgeSuno-macOS-Installer.pkg`
- Use `--sign-plugins` to ad-hoc sign the plugin bundles (recommended for local testing).
- Install: open the `.pkg` in Finder, or `sudo installer -pkg release-artefacts/AceForgeSuno-macOS-Installer.pkg -target /`

If Xcode is not available, use Unix Makefiles: `-G "Unix Makefiles"` and set `CMAKE_C_COMPILER` / `CMAKE_CXX_COMPILER` to your clang path.

## Install (pre-built)

- **Releases:** See [Releases](https://github.com/audiohacking/suno-daw/releases). Download the zip or the `.pkg` installer.
- **Artifacts:** On push to `main` / `master` or manual “Run workflow”, the Actions run produces a **workflow artifact** with the zip and installer.

## API key and usage

1. Get a Suno API key (see [Suno API docs](https://docs.sunoapi.org/)).
2. Insert **AceForge-Suno** on a stereo bus (e.g. master).
3. Open the plugin UI → enter the API key → click **Save**. Status should show “Suno: connected”.
4. **Record:** Enable **Record**, play your project (or feed audio through the bus). Disable Record when done.
5. **Generate:** Fill Prompt / Style / Title, choose Model (e.g. V4.5 All), then **Generate**.
6. **Cover / Add Vocals:** After recording, use **Cover (from recorded)** or **Add Vocals (from recorded)** with your prompt/style.

## Docs for developers

- **[DESIGN.md](DESIGN.md)** — Architecture, Suno API usage, recording, library, state, and UI. Use this when continuing development in a new session (e.g. in Cursor).

## License and credits

- Plugin: AudioHacking.
- Uses [JUCE](https://github.com/juce-framework/JUCE).
- Suno API: [docs.sunoapi.org](https://docs.sunoapi.org/).
