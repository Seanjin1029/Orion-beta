"""
FastAPI server — receives generate requests from the JUCE plugin (target="vst")
and from the CLI test script (target="m4l").

Run with: uvicorn server:app --host 127.0.0.1 --port 8765
"""
from pathlib import Path
from typing import Literal, Optional

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel

from generator import M4LGenerator
from faust_generator import FaustGenerator

app = FastAPI(title="Ableton AI Plugin Backend", version="0.2.0")


class GenerateRequest(BaseModel):
    prompt: str
    api_key: str
    target: Literal["m4l", "vst"] = "m4l"
    output_dir: Optional[str] = None   # m4l only


class GenerateResponse(BaseModel):
    status: str
    device_name: Optional[str] = None
    device_type: Optional[str] = None
    description: Optional[str] = None
    # m4l fields
    path: Optional[str] = None
    # vst fields
    faust_code: Optional[str] = None
    dsp_params: Optional[dict] = None
    # error
    message: Optional[str] = None


@app.get("/health")
def health():
    return {"status": "ok"}


@app.post("/generate", response_model=GenerateResponse)
def generate(req: GenerateRequest):
    if not req.prompt.strip():
        raise HTTPException(status_code=400, detail="Prompt cannot be empty")
    if not req.api_key.strip():
        raise HTTPException(status_code=400, detail="API key required")

    if req.target == "vst":
        gen = FaustGenerator(api_key=req.api_key)
        result = gen.generate(prompt=req.prompt)
    else:
        output_dir = Path(req.output_dir) if req.output_dir else None
        gen = M4LGenerator(api_key=req.api_key)
        result = gen.generate(prompt=req.prompt, output_dir=output_dir)

    return GenerateResponse(**result)
