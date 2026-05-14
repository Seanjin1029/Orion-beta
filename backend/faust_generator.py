"""
FAUST code generator for the VST3 plugin path.
Calls Claude, returns FAUST DSP code — no file writing, code is compiled
inside the JUCE plugin via libfaust (future integration).
"""
import json
import re
from pathlib import Path
from typing import Optional

import anthropic

SYSTEM_PROMPT_PATH = Path(__file__).parent.parent / "system-prompts" / "faust_device.md"
MAX_RETRIES = 3


class FaustGenerator:
    def __init__(self, api_key: str):
        self.client = anthropic.Anthropic(api_key=api_key)
        self.system_prompt = SYSTEM_PROMPT_PATH.read_text()

    def generate(self, prompt: str) -> dict:
        """
        Generate a FAUST audio effect from a natural language prompt.
        Returns:
          {"status": "ok", "device_name": ..., "faust_code": ..., ...}
          {"status": "error", "message": ...}
        """
        last_error = None
        messages = [{"role": "user", "content": prompt}]

        for attempt in range(MAX_RETRIES):
            try:
                response = self.client.messages.create(
                    model="claude-opus-4-7",
                    max_tokens=4096,
                    system=self.system_prompt,
                    messages=messages,
                )
                raw = response.content[0].text.strip()
                parsed = self._parse_response(raw)

                if "error" in parsed:
                    return {"status": "error", "message": parsed["error"]}

                self._validate(parsed)

                return {
                    "status":      "ok",
                    "device_name": parsed["device_name"],
                    "device_type": parsed["device_type"],
                    "description": parsed["description"],
                    "faust_code":  parsed["faust_code"],
                    "dsp_params":  parsed["dsp_params"],
                }

            except (json.JSONDecodeError, KeyError, ValueError) as e:
                last_error = str(e)
                messages.append({"role": "assistant", "content": raw if 'raw' in dir() else ""})
                messages.append({
                    "role": "user",
                    "content": (
                        f"Your previous response had an error: {last_error}\n\n"
                        "Please fix it and respond with valid JSON only."
                    ),
                })
            except anthropic.APIError as e:
                return {"status": "error", "message": f"API error: {e}"}

        return {
            "status": "error",
            "message": f"Failed after {MAX_RETRIES} attempts. Last error: {last_error}",
        }

    def _parse_response(self, raw: str) -> dict:
        clean = re.sub(r"^```(?:json)?\n?", "", raw, flags=re.MULTILINE)
        clean = re.sub(r"\n?```$", "", clean, flags=re.MULTILINE)
        return json.loads(clean.strip())

    def _validate(self, device: dict):
        required = {"device_name", "device_type", "description", "faust_code", "dsp_params"}
        missing = required - device.keys()
        if missing:
            raise ValueError(f"Missing fields: {missing}")
        if len(device["faust_code"].strip()) < 20:
            raise ValueError("faust_code is empty or too short")
        if "process" not in device["faust_code"]:
            raise ValueError("faust_code must contain a 'process' definition")
        # Unknown dsp_params types are allowed — they fall back to passthrough
        # in the C++ engine until libfaust JIT is integrated.
