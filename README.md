<img width="140" alt="image" src="https://github.com/user-attachments/assets/60679bdb-5642-43af-938e-c5f4182a3852" />

# suno-daw

> ⚠️ **Proof of Concept** — Expect bugs and limited functionality.

Control the **Suno API** from your DAW. Generate music, create covers, and add vocals to your tracks using Suno's AI-powered music generation.

## Platform Support

**Currently supported:** macOS (Apple Silicon & Intel)

**Future support:** Windows and Linux are planned for future releases.

## Features

- **Generate** — Create music from text prompts with style and title customization
- **Cover** — Transform recorded audio into a cover version with Suno AI
- **Add Vocals** — Add AI-generated vocals to your instrumental recordings
- **Library** — Generated tracks are automatically saved and organized for easy access

## Installation

### Pre-built Releases (Recommended)

Download the latest release from [GitHub Releases](https://github.com/audiohacking/suno-daw/releases):
- `.pkg` installer for easy installation
- `.zip` for manual installation

After installation, rescan plugins in your DAW.

### Requirements

- macOS (Apple Silicon or Intel)
- A Suno API key from [Suno API](https://docs.sunoapi.org/)

## Quick Start

1. **Get an API Key** — Sign up at [Suno API](https://docs.sunoapi.org/) to get your API key
2. **Load the Plugin** — Insert **AceForge-Suno** on any stereo track or bus in your DAW
3. **Configure** — Open the plugin UI, enter your API key, and click **Save**
4. **Start Creating** — Use Generate, Cover, or Add Vocals to create music with AI

### Usage Tips

- **Generate:** Enter a prompt describing the music you want, set a style and title, then click Generate
- **Cover/Vocals:** First enable Record mode and capture audio in your DAW, then use Cover or Add Vocals features
- **Library:** All generated audio is saved to `~/Library/Application Support/AceForgeSuno/Generations/` and can be dragged into your DAW

## Building from Source

For developers who want to build the plugin:

```bash
cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Copy the built plugins to your system plugin folders and rescan in your DAW.

For detailed build instructions and architecture documentation, see [DESIGN.md](DESIGN.md).

## License

Created by AudioHacking. Built with [JUCE](https://github.com/juce-framework/JUCE).

Uses the [Suno API](https://docs.sunoapi.org/).
