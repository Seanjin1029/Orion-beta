#!/usr/bin/env python3
"""
Quick CLI test — runs the generator directly without the server.
Usage: python test_generate.py "your prompt here"
"""
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "backend"))
from generator import M4LGenerator

def main():
    prompt = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else "Create a simple arpeggiator that cycles through the notes of a major chord"
    api_key = os.environ.get("ANTHROPIC_API_KEY", "")
    if not api_key:
        print("Error: set ANTHROPIC_API_KEY environment variable")
        sys.exit(1)

    print(f"Prompt: {prompt}")
    print("Generating...")

    gen = M4LGenerator(api_key=api_key)
    # output_dir=None → goes directly to Ableton User Library
    result = gen.generate(prompt=prompt, output_dir=None)

    print(json.dumps(result, indent=2))
    if result["status"] == "ok":
        print(f"\nDevice written to: {result['path']}")

if __name__ == "__main__":
    main()
