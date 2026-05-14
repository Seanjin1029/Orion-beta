"""
M4L device generator — calls Claude, parses response, writes .amxd to disk.
"""
import json
import os
import re
import struct
import time
from pathlib import Path
from typing import Optional

import anthropic

SYSTEM_PROMPT_PATH = Path(__file__).parent.parent / "system-prompts" / "m4l_device.md"
MAX_RETRIES = 3

# Default output directory — Ableton User Library on Mac
DEFAULT_OUTPUT_DIR = Path.home() / "Music" / "Ableton" / "User Library" / "Presets" / "MIDI Effects" / "Max MIDI Effect"


def get_output_dir(device_type: str, custom_dir: Optional[Path] = None) -> Path:
    if custom_dir:
        return custom_dir
    base = Path.home() / "Music" / "Ableton" / "User Library" / "Presets"
    mapping = {
        "midi_effect": base / "MIDI Effects" / "Max MIDI Effect",
        "audio_effect": base / "Audio Effects" / "Max Audio Effect",
        "instrument": base / "Instruments" / "Max Instrument",
    }
    return mapping.get(device_type, base / "MIDI Effects" / "Max MIDI Effect")


class M4LGenerator:
    def __init__(self, api_key: str):
        self.client = anthropic.Anthropic(api_key=api_key)
        self.system_prompt = SYSTEM_PROMPT_PATH.read_text()

    def generate(self, prompt: str, output_dir: Optional[Path] = None) -> dict:
        """
        Generate an M4L device from a natural language prompt.
        Returns {"status": "ok", "path": str, "device_name": str} or {"status": "error", "message": str}
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

                self._validate_device(parsed)

                target_dir = output_dir or get_output_dir(parsed["device_type"])
                target_dir.mkdir(parents=True, exist_ok=True)
                out_path = self._write_amxd(parsed, target_dir)

                return {
                    "status": "ok",
                    "path": str(out_path),
                    "device_name": parsed["device_name"],
                    "device_type": parsed["device_type"],
                    "description": parsed["description"],
                }

            except (json.JSONDecodeError, KeyError, ValueError) as e:
                last_error = str(e)
                # Feed the error back to Claude for self-correction
                messages.append({"role": "assistant", "content": raw if 'raw' in dir() else ""})
                messages.append({
                    "role": "user",
                    "content": f"Your previous response had an error: {last_error}\n\nPlease fix it and respond with valid JSON only.",
                })
            except anthropic.APIError as e:
                return {"status": "error", "message": f"API error: {e}"}

        return {"status": "error", "message": f"Failed after {MAX_RETRIES} attempts. Last error: {last_error}"}

    def _parse_response(self, raw: str) -> dict:
        # Strip markdown code fences if Claude wrapped the JSON
        clean = re.sub(r"^```(?:json)?\n?", "", raw, flags=re.MULTILINE)
        clean = re.sub(r"\n?```$", "", clean, flags=re.MULTILINE)
        return json.loads(clean.strip())

    def _validate_device(self, device: dict):
        required_base = {"device_name", "device_type", "description"}
        missing = required_base - device.keys()
        if missing:
            raise ValueError(f"Missing fields: {missing}")
        valid_types = {"midi_effect", "audio_effect", "instrument"}
        if device["device_type"] not in valid_types:
            raise ValueError(f"Invalid device_type: {device['device_type']}")
        if device["device_type"] == "audio_effect":
            if "gen_code" not in device or len(device["gen_code"].strip()) < 10:
                raise ValueError("audio_effect requires a non-empty gen_code field")
        else:
            if "js_code" not in device or len(device["js_code"].strip()) < 10:
                raise ValueError("midi_effect/instrument requires a non-empty js_code field")

    # .amxd type codes — written into the binary header and the project.amxdtype field
    _AMXD_TYPE = {
        "midi_effect":  (b"mmmm", 1835887981),
        "audio_effect": (b"aaaa", 1633771873),
        "instrument":   (b"iiii", 1768515945),
    }

    def _write_amxd(self, device: dict, output_dir: Path) -> Path:
        """
        Write a valid .amxd binary container:
          [ampf][04 00 00 00][type_code 4B]
          [meta][04 00 00 00][00 00 00 00]
          [ptch][size LE   ][JSON patcher]
        """
        safe_name = re.sub(r'[^\w\s-]', '', device["device_name"]).strip().replace(' ', '_')
        out_path = output_dir / f"{safe_name}.amxd"

        type_code, amxd_type_int = self._AMXD_TYPE.get(
            device["device_type"], self._AMXD_TYPE["midi_effect"]
        )

        if device["device_type"] == "audio_effect":
            # Write .gendsp sidecar so Max can find the DSP code at runtime
            gendsp_path = output_dir / f"{safe_name}.gendsp"
            gendsp_path.write_text(device["gen_code"])
            patcher_json = self._build_maxpat_audio(device, safe_name, amxd_type_int)
        else:
            # Write .js sidecar for inspection
            (output_dir / f"{safe_name}.js").write_text(device["js_code"])
            patcher_json = self._build_maxpat(device, safe_name, amxd_type_int)

        ptch_bytes = json.dumps(patcher_json, indent="\t").encode("utf-8")
        ampf_chunk = b"ampf" + struct.pack("<I", 4) + type_code
        meta_chunk = b"meta" + struct.pack("<I", 4) + b"\x00\x00\x00\x00"
        ptch_chunk = b"ptch" + struct.pack("<I", len(ptch_bytes)) + ptch_bytes

        out_path.write_bytes(ampf_chunk + meta_chunk + ptch_chunk)
        return out_path

    def _build_maxpat_audio(self, device: dict, safe_name: str, amxd_type_int: int) -> dict:
        """Patcher for audio effects: plugin~ (stereo) → gen~ → plugout~ (stereo)."""
        mac_time = int(time.time()) - 978307200
        gendsp_name = safe_name  # gen~ loads <name>.gendsp from same directory

        boxes = [
            {
                "box": {
                    "fontname": "Arial Bold",
                    "fontsize": 10.0,
                    "id": "obj-in",
                    "maxclass": "newobj",
                    "numinlets": 0,
                    "numoutlets": 2,
                    "outlettype": ["signal", "signal"],
                    "patching_rect": [50.0, 50.0, 70.0, 20.0],
                    "text": "plugin~ 2",
                }
            },
            {
                "box": {
                    "fontname": "Arial Bold",
                    "fontsize": 10.0,
                    "id": "obj-gen",
                    "maxclass": "newobj",
                    "numinlets": 2,
                    "numoutlets": 2,
                    "outlettype": ["signal", "signal"],
                    "patching_rect": [50.0, 150.0, 150.0, 20.0],
                    "text": f"gen~ {gendsp_name}",
                }
            },
            {
                "box": {
                    "fontname": "Arial Bold",
                    "fontsize": 10.0,
                    "id": "obj-out",
                    "maxclass": "newobj",
                    "numinlets": 2,
                    "numoutlets": 0,
                    "patching_rect": [50.0, 250.0, 70.0, 20.0],
                    "text": "plugout~ 2",
                }
            },
        ]

        lines = [
            {"patchline": {"source": ["obj-in", 0], "destination": ["obj-gen", 0]}},
            {"patchline": {"source": ["obj-in", 1], "destination": ["obj-gen", 1]}},
            {"patchline": {"source": ["obj-gen", 0], "destination": ["obj-out", 0]}},
            {"patchline": {"source": ["obj-gen", 1], "destination": ["obj-out", 1]}},
        ]

        return self._make_patcher_shell(device, boxes, lines, amxd_type_int, mac_time)

    def _build_maxpat(self, device: dict, safe_name: str, amxd_type_int: int) -> dict:
        """Patcher for MIDI effects and instruments: notein → js (embedded) → noteout."""
        js_filename = f"{safe_name}.js"
        source_lines = [line + "\n" for line in device["js_code"].split("\n")]
        mac_time = int(time.time()) - 978307200

        boxes = [
            {
                "box": {
                    "fontname": "Arial Bold",
                    "fontsize": 10.0,
                    "id": "obj-in",
                    "maxclass": "newobj",
                    "numinlets": 0,
                    "numoutlets": 3,
                    "outlettype": ["int", "int", "int"],
                    "patching_rect": [50.0, 50.0, 55.0, 20.0],
                    "text": "notein",
                }
            },
            {
                "box": {
                    "fontname": "Arial Bold",
                    "fontsize": 10.0,
                    "id": "obj-js",
                    "maxclass": "newobj",
                    "numinlets": 3,
                    "numoutlets": 3,
                    "outlettype": ["int", "int", "int"],
                    "patching_rect": [50.0, 150.0, 200.0, 20.0],
                    "saved_object_attributes": {
                        "embed": 1,
                        "filename": js_filename,
                        "runtime_filename": js_filename,
                    },
                    "source": source_lines,
                    "text": f"js {js_filename}",
                }
            },
            {
                "box": {
                    "fontname": "Arial Bold",
                    "fontsize": 10.0,
                    "id": "obj-out",
                    "maxclass": "newobj",
                    "numinlets": 3,
                    "numoutlets": 0,
                    "patching_rect": [50.0, 250.0, 55.0, 20.0],
                    "text": "noteout",
                }
            },
        ]

        lines = []
        for i in range(3):
            lines.append({"patchline": {"source": ["obj-in", i], "destination": ["obj-js", i]}})
            lines.append({"patchline": {"source": ["obj-js", i], "destination": ["obj-out", i]}})

        return self._make_patcher_shell(device, boxes, lines, amxd_type_int, mac_time)

    def _make_patcher_shell(self, device: dict, boxes: list, lines: list,
                             amxd_type_int: int, mac_time: int) -> dict:
        """Shared patcher JSON wrapper used by all device types."""
        return {
            "patcher": {
                "fileversion": 1,
                "appversion": {
                    "major": 8, "minor": 1, "revision": 2,
                    "architecture": "x64", "modernui": 1,
                },
                "classnamespace": "box",
                "rect": [65.0, 100.0, 640.0, 480.0],
                "openrect": [0.0, 0.0, 0.0, 0.0],
                "bglocked": 0,
                "openinpresentation": 0,
                "default_fontsize": 10.0,
                "default_fontface": 0,
                "default_fontname": "Arial Bold",
                "gridonopen": 1,
                "gridsize": [8.0, 8.0],
                "gridsnaponopen": 1,
                "objectsnaponopen": 1,
                "statusbarvisible": 2,
                "toolbarvisible": 1,
                "lefttoolbarpinned": 0,
                "toptoolbarpinned": 0,
                "righttoolbarpinned": 0,
                "bottomtoolbarpinned": 0,
                "toolbars_unpinned_last_save": 0,
                "tallnewobj": 0,
                "boxanimatetime": 500,
                "enablehscroll": 1,
                "enablevscroll": 1,
                "devicewidth": 0.0,
                "description": device["description"],
                "digest": "",
                "tags": "",
                "style": "",
                "subpatcher_template": "",
                "assistshowspatchername": 0,
                "boxes": boxes,
                "lines": lines,
                "dependency_cache": [],
                "latency": 0,
                "project": {
                    "version": 1,
                    "creationdate": mac_time,
                    "modificationdate": mac_time,
                    "viewrect": [0.0, 0.0, 300.0, 500.0],
                    "autoorganize": 1,
                    "hideprojectwindow": 1,
                    "showdependencies": 1,
                    "autolocalize": 0,
                    "contents": {"patchers": {}},
                    "layout": {},
                    "searchpath": {},
                    "detailsvisible": 0,
                    "amxdtype": amxd_type_int,
                    "readonly": 0,
                    "devpathtype": 0,
                    "devpath": ".",
                    "sortmode": 0,
                    "viewmode": 0,
                },
                "autosave": 0,
            }
        }
