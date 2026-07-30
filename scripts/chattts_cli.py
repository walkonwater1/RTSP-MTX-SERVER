#!/usr/bin/env python3
"""
ChatTTS CLI wrapper for RTSP voice pipeline.
One-shot mode:      python3 chattts_cli.py --text "你好" --output out.wav
Server mode (fast):  python3 chattts_cli.py --server  (model loaded once, stdin→WAV)

Server protocol (text lines on stdin, one utterance per line):
  INPUT:  <output_path>|<text>
  OUTPUT: OK|<output_path> or ERR|<message>
"""

import argparse
import sys
import os
import time
import json
import warnings
warnings.filterwarnings("ignore")


def load_model():
    import torch
    import ChatTTS

    chat = ChatTTS.Chat()
    chat.load(compile=False)
    # Use a fixed speaker for consistent voice quality
    spk = chat.sample_random_speaker()
    return chat, spk


def synthesize(chat, spk, text, speed=1.0):
    """Returns (samples, sample_rate) as numpy array."""
    import numpy as np

    wavs = chat.infer(
        [text],
        use_decoder=True,
        params_infer_code={
            'spk_emb': spk,
            'temperature': 0.3,
            'top_P': 0.7,
            'top_K': 20,
        },
        params_refine_text={
            'prompt': '[oral_2][laugh_0][break_4]',
        },
        do_text_normalization=True,
    )
    return wavs[0], 24000  # ChatTTS outputs at 24kHz


def save_wav(audio, sample_rate, output_path, target_rate=16000):
    """Resample to target_rate and save as 16-bit mono WAV."""
    import numpy as np
    import wave

    # Normalize
    peak = np.max(np.abs(audio))
    if peak > 0:
        audio = audio / peak * 0.9

    # Resample to target rate
    if target_rate != sample_rate and len(audio) > 0:
        # Use simple linear interpolation (no scipy dependency)
        ratio = target_rate / sample_rate
        new_len = int(len(audio) * ratio)
        indices = np.linspace(0, len(audio) - 1, new_len)
        audio = np.interp(indices, np.arange(len(audio)), audio)

    # Convert to int16
    audio_int16 = (audio * 32767).astype(np.int16)

    # Write WAV
    with wave.open(output_path, 'wb') as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(target_rate)
        wf.writeframes(audio_int16.tobytes())

    return len(audio_int16), target_rate


# ── Server mode ─────────────────────────────────────────
def run_server():
    """Load model once, process text lines from stdin."""
    print("[ChatTTS] loading model...", file=sys.stderr)
    t0 = time.time()
    chat, spk = load_model()
    print(f"[ChatTTS] model loaded in {time.time()-t0:.1f}s, ready",
          file=sys.stderr, flush=True)
    print("READY", flush=True)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        if line == "EXIT":
            break

        # Format: output_path|text
        parts = line.split("|", 1)
        if len(parts) != 2:
            print(f"ERR|bad format: {line}", flush=True)
            continue

        output_path, text = parts
        text = text.strip()
        if not text:
            print(f"ERR|empty text", flush=True)
            continue

        try:
            t_start = time.time()
            audio, sr = synthesize(chat, spk, text)
            n_samples, out_sr = save_wav(audio, sr, output_path)
            dur = n_samples / out_sr
            t_ms = (time.time() - t_start) * 1000
            print(f"OK|{output_path}|{n_samples}|{dur:.1f}s|{t_ms:.0f}ms", flush=True)
        except Exception as e:
            print(f"ERR|{e}", flush=True)


# ── One-shot mode ────────────────────────────────────────
def run_oneshot(text, output_path, speed):
    print("[ChatTTS] loading model...", file=sys.stderr)
    chat, spk = load_model()
    audio, sr = synthesize(chat, spk, text, speed)
    n_samples, out_sr = save_wav(audio, sr, output_path)
    dur = n_samples / out_sr
    print(f"[ChatTTS] {n_samples} samples ({dur:.1f}s) → {output_path}",
          file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="ChatTTS CLI for RTSP server")
    parser.add_argument("--text", help="Text to synthesize")
    parser.add_argument("--output", help="Output WAV file path")
    parser.add_argument("--speed", type=float, default=1.0)
    parser.add_argument("--server", action="store_true",
                        help="Run in server mode (stdin protocol)")
    args = parser.parse_args()

    if args.server:
        run_server()
    elif args.text and args.output:
        run_oneshot(args.text, args.output, args.speed)
    else:
        parser.print_help()
        sys.exit(1)


if __name__ == "__main__":
    main()
