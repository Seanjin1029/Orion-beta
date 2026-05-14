# orion is not the actual name

Generates Max for Live devices from natural language prompts via Claude API.

## Architecture

```
JUCE VST3/AU plugin (C++)
    ↓ HTTP POST /generate
Python FastAPI server
    ↓ Anthropic API
Claude (claude-opus-4-7)
    ↓ parsed JSON
.amxd file written to Ableton User Library
    ↓ drag into rack
Ableton Live
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
