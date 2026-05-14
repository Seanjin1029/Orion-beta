# orion is not the actual name

Generates Max for Live devices from natural language prompts via Claude API.

## Architecture

```
  ┌─────────────────────────────────────────────────────┐
  │                   Ableton Live                      │
  │  ┌──────────────────────────────────────────────┐   │
  │  │            JUCE AU Plugin                    │   │
  │  │                                              │   │
  │  │  PluginEditor (UI thread)                    │   │
  │  │  ├── Prompt input → startGeneration()        │   │
  │  │  ├── Code viewer (editable, Compile button)  │   │
  │  │  ├── Layer list (version history)            │   │
  │  │  └── Status label                            │   │
  │  │                                              │   │
  │  │  PluginProcessor (audio thread)              │   │
  │  │  ├── FAUST DSP (LLVM JIT) ← hot-swapped      │   │
  │  │  └── C++ fallback DSP (if JIT fails)         │   │
  │  └──────────────────────────────────────────────┘   │
  └─────────────────────────────────────────────────────┘
           │ HTTP POST (prompt + API key)
           ▼
  ┌─────────────────────────┐
  │   Python Backend        │
  │   FastAPI on :8765      │
  │   └── Calls Claude API  │
  │       returns FAUST code│
  └─────────────────────────┘
```
## Generation Flow

```
  1. User types a prompt → hits Enter
  2. GenerationThread POSTs to the Python backend
  3. Backend sends the prompt to Claude, gets back a FAUST program
  4. Thread JIT-compiles the FAUST code via libfaust + LLVM into a native DSP
  instance
  5. The new DSP is handed to the audio thread lock-free via an atomic pointer
  swap
  6. If JIT fails, the C++ fallback engine kicks in (saturation, delay, reverb,
  etc.)
```

## Project Structure

```
ableton-ai-plugin/
├── backend/
│   ├── server.py          # FastAPI HTTP server (JUCE ↔ Python IPC)
│   ├── generator.py       # M4L device generator + Claude integration
│   └── requirements.txt
├── juce-plugin/           # C++ JUCE plugin (VST3/AU shell)
├── system-prompts/
│   └── m4l_device.md      # System prompt for M4L JS generation
├── scripts/
│   └── test_generate.py   # CLI test without the server
└── examples/
    └── generated/         # Test output devices land here
```

## Key Files
```
  ┌────────────────────────────┬─────────────────────────────────────────┐
  │            File            │                  Role                   │
  ├────────────────────────────┼─────────────────────────────────────────┤
  │ PluginProcessor.cpp        │ Audio processing, threading, JIT, state │
  ├────────────────────────────┼─────────────────────────────────────────┤
  │ PluginEditor.cpp           │ All UI, layout, user interaction        │
  ├────────────────────────────┼─────────────────────────────────────────┤
  │ backend/server.py          │ FastAPI server, routes requests         │
  ├────────────────────────────┼─────────────────────────────────────────┤
  │ backend/generator.py       │ Claude API call, prompt construction    │
  ├────────────────────────────┼─────────────────────────────────────────┤
  │ backend/faust_generator.py │ FAUST-specific generation logic         │
  ├────────────────────────────┼─────────────────────────────────────────┤
  │ CMakeLists.txt             │ Build: JUCE + libfaust linkage          │
  └────────────────────────────┴─────────────────────────────────────────┘
```

## Quick Start (backend only)

```bash
cd backend
pip install -r requirements.txt
export ANTHROPIC_API_KEY=sk-ant-...

# Test generation directly
cd ../scripts
python test_generate.py "create a MIDI arpeggiator that plays major chords"

# Or run the server
cd ../backend
uvicorn server:app --host 127.0.0.1 --port 8765
```

## API

### POST /generate
```json
{
  "prompt": "create a random velocity randomizer",
  "api_key": "sk-ant-...",
  "output_dir": null  // optional — defaults to Ableton User Library
}
```

Response:
```json
{
  "status": "ok",
  "path": "/Users/.../User Library/Presets/MIDI Effects/...",
  "device_name": "Velocity Randomizer",
  "device_type": "midi_effect",
  "description": "Randomizes MIDI note velocity"
}
```

## Device Types

- `midi_effect` — MIDI pitch/velocity/timing manipulation
- `audio_effect` — parameter control, LFOs (control-rate only)
- `instrument` — MIDI-driven synthesizer control

## Requirements

- Python 3.11+
- Ableton Live **Suite** (for Max for Live support)
- Anthropic API key
- JUCE 7+ (for building the VST shell)
- deps/faust/lib/libfaustwithllvm.a — a 295MB static library that bundles the 
  FAUST compiler and LLVM backend. It's excluded from git (too large); anyone
  building from source needs to download it separately.
