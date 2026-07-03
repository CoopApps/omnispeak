#!/usr/bin/env python3
"""
DOSBox-X Breakpoint Register Logger
====================================
Waits for a breakpoint to fire in the DOSBox-X Debugger window,
captures all register values, presses F5 to continue, and repeats.
Results are saved to an Excel spreadsheet.

Requirements:
    pip install pillow pytesseract openpyxl pandas
    apt install xdotool scrot tesseract-ocr

Usage:
    python3 bp_logger.py --output registers.xlsx --hits 100
    python3 bp_logger.py --output registers.xlsx --hits 100 --delay 0.3
"""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

try:
    import pandas as pd
    from PIL import Image
    import pytesseract
    import openpyxl
except ImportError as e:
    print(f"Missing dependency: {e}")
    print("Run: pip install pillow pytesseract openpyxl pandas")
    sys.exit(1)


WINDOW_TITLE = "DOSBox-X Debugger"

# Registers to extract and their hex widths
REGISTERS_32 = ["EAX", "EBX", "ECX", "EDX", "ESP", "EBP", "ESI", "EDI", "EIP"]
REGISTERS_16 = ["CS", "DS", "ES", "FS", "GS", "SS"]
FLAGS = ["C", "Z", "S", "A", "P", "T", "I"]


def find_window():
    """Return the window ID of the DOSBox-X Debugger window."""
    result = subprocess.run(
        ["xdotool", "search", "--name", WINDOW_TITLE],
        capture_output=True, text=True
    )
    ids = result.stdout.strip().splitlines()
    if not ids:
        raise RuntimeError(
            f'DOSBox-X Debugger window not found. '
            f'Make sure DOSBox-X is running with the debugger open (Alt+Pause).'
        )
    return ids[-1].strip()


def get_window_geometry(win_id):
    """Return (x, y, width, height) of a window."""
    result = subprocess.run(
        ["xdotool", "getwindowgeometry", "--shell", win_id],
        capture_output=True, text=True
    )
    vals = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            vals[k.strip()] = int(v.strip())
    return vals["X"], vals["Y"], vals["WIDTH"], vals["HEIGHT"]


def screenshot_window(win_id):
    """Capture the debugger window and return a PIL Image."""
    x, y, w, h = get_window_geometry(win_id)
    tmp = "/tmp/dosbox_dbg_cap.png"
    subprocess.run(
        ["scrot", "-a", f"{x},{y},{w},{h}", tmp],
        check=True
    )
    return Image.open(tmp)


def crop_register_section(img):
    """Top ~17% — Register Overview (EAX, EBX, flags, segments…)."""
    w, h = img.size
    return img.crop((0, 0, w, int(h * 0.17)))


def crop_data_section(img):
    """Next band ~17-38% — Data View (hex dump of memory segment)."""
    w, h = img.size
    return img.crop((0, int(h * 0.17), w, int(h * 0.38)))


def crop_code_section(img):
    """Middle band ~38-62% — Code Overview (disassembly around EIP)."""
    w, h = img.size
    return img.crop((0, int(h * 0.38), w, int(h * 0.62)))


def crop_output_section(img):
    """Bottom ~62-100% — DOSBox-X output / log."""
    w, h = img.size
    return img.crop((0, int(h * 0.62), w, h))


def ocr(img):
    """Run Tesseract OCR on an image and return the text."""
    # Upscale for better OCR accuracy on small debugger fonts
    scale = 3
    big = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
    return pytesseract.image_to_string(big, config="--psm 6")


def parse_registers(text):
    """Extract register values from OCR'd register section text."""
    row = {}

    for reg in REGISTERS_32:
        m = re.search(rf'\b{reg}\s*[=:]\s*([0-9A-Fa-f]{{1,8}})\b', text)
        row[reg] = m.group(1).upper() if m else ""

    for reg in REGISTERS_16:
        m = re.search(rf'\b{reg}\s*[=:]\s*([0-9A-Fa-f]{{1,4}})\b', text)
        row[reg] = m.group(1).upper() if m else ""

    # Flags: C0 Z1 S0 A0 P1 T1 I1 style
    for flag in FLAGS:
        m = re.search(rf'\b{flag}([01])\b', text)
        row[f"F_{flag}"] = int(m.group(1)) if m else ""

    return row


def parse_code_overview(text):
    """
    Extract all disassembly lines from the Code Overview section.
    Returns (current_instruction_str, full_listing_str).
    The first matched line is treated as the current instruction (at EIP).
    """
    lines = [l.strip() for l in text.splitlines() if l.strip()]
    parsed = []
    for line in lines:
        # e.g. "0868:00016572 89CA   mov edx,ecx"
        m = re.match(r'([0-9A-Fa-f]{4}:[0-9A-Fa-f]{8})\s+([0-9A-Fa-f ]+?)\s{2,}(.+)', line)
        if m:
            parsed.append(f"{m.group(1)}  {m.group(3).strip()}")
    current = parsed[0] if parsed else ""
    listing = "\n".join(parsed)
    return current, listing


def parse_data_view(text):
    """
    Extract hex dump lines from the Data View section.
    Each line looks like:  0000:00000000 na na na na ...
    Returns the raw cleaned text block.
    """
    lines = []
    for line in text.splitlines():
        line = line.strip()
        if re.match(r'[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}', line):
            lines.append(line)
    return "\n".join(lines)


def is_at_breakpoint(win_id, prev_eip):
    """
    Detect whether execution has stopped at a breakpoint.
    Strategy: take a screenshot and check if EIP changed from last time,
    or simply always capture after a configurable delay (the user controls
    when BP fires by having already set it in DOSBox-X).
    """
    img = screenshot_window(win_id)
    reg_img = crop_register_section(img)
    text = ocr(reg_img)
    row = parse_registers(text)
    eip = row.get("EIP", "")
    stopped = eip != "" and eip != prev_eip
    return stopped, img, row, text


def press_f5(win_id):
    """Send F5 to the debugger window (Run / Continue)."""
    subprocess.run(["xdotool", "key", "--window", win_id, "F5"])


def save_excel(rows, output_path):
    """Write collected rows to Excel."""
    df = pd.DataFrame(rows)
    cols = (
        ["Hit", "Timestamp", "EIP"]
        + REGISTERS_32[:-1]  # EIP already first; skip duplicate
        + REGISTERS_16
        + [f"F_{f}" for f in FLAGS]
        + ["Instruction", "Code_Listing", "Data_View", "Screenshot"]
    )
    # Keep only columns that exist
    cols = [c for c in cols if c in df.columns]
    df = df[cols]

    with pd.ExcelWriter(output_path, engine="openpyxl") as writer:
        df.to_excel(writer, index=False, sheet_name="Breakpoints")
        ws = writer.sheets["Breakpoints"]

        # Auto-size columns
        for col_cells in ws.columns:
            max_len = max((len(str(c.value or "")) for c in col_cells), default=10)
            ws.column_dimensions[col_cells[0].column_letter].width = min(max_len + 2, 40)

        # Freeze top row
        ws.freeze_panes = "A2"

    print(f"\nSaved {len(rows)} rows to {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Log DOSBox-X register state each time a breakpoint fires."
    )
    parser.add_argument("--output", default="bp_registers.xlsx",
                        help="Output Excel file (default: bp_registers.xlsx)")
    parser.add_argument("--hits", type=int, default=100,
                        help="Number of breakpoint hits to capture (default: 100)")
    parser.add_argument("--delay", type=float, default=0.4,
                        help="Seconds to wait after F5 before checking for BP (default: 0.4)")
    parser.add_argument("--screenshots", action="store_true",
                        help="Save a PNG screenshot for each hit alongside the Excel file")
    args = parser.parse_args()

    output_path = Path(args.output)
    screenshot_dir = output_path.parent / (output_path.stem + "_screenshots")

    print(f"Looking for '{WINDOW_TITLE}' window...")
    win_id = find_window()
    print(f"Found window ID: {win_id}")
    print(f"Will capture {args.hits} breakpoint hits → {output_path}")
    print("Make sure your breakpoint is already set in DOSBox-X.")
    print("Press Ctrl+C at any time to stop and save what's been collected.\n")

    rows = []
    prev_eip = None

    # --- Initial capture: read state right now (execution is stopped at BP) ---
    print("Capturing initial state (hit 0)...")
    img = screenshot_window(win_id)
    row = parse_registers(ocr(crop_register_section(img)))
    current_instr, code_listing = parse_code_overview(ocr(crop_code_section(img)))
    data_view = parse_data_view(ocr(crop_data_section(img)))
    row["Hit"] = 0
    row["Timestamp"] = time.strftime("%H:%M:%S")
    row["Instruction"] = current_instr
    row["Code_Listing"] = code_listing
    row["Data_View"] = data_view
    if args.screenshots:
        screenshot_dir.mkdir(exist_ok=True)
        shot_path = screenshot_dir / "hit_0000.png"
        img.save(shot_path)
        row["Screenshot"] = str(shot_path)
    else:
        row["Screenshot"] = ""
    rows.append(row)
    prev_eip = row.get("EIP", "")
    print(f"  Hit 0: EIP={row.get('EIP','')}  {row.get('Instruction','')}")

    try:
        for hit in range(1, args.hits + 1):
            # Press F5 to continue execution
            press_f5(win_id)
            time.sleep(args.delay)

            # Poll until EIP changes (breakpoint fired again) or timeout
            waited = 0
            timeout = 30  # seconds max wait per hit
            captured_img = None
            while waited < timeout:
                img = screenshot_window(win_id)
                reg_text = ocr(crop_register_section(img))
                row = parse_registers(reg_text)
                eip = row.get("EIP", "")
                if eip and eip != prev_eip:
                    captured_img = img
                    break
                time.sleep(0.2)
                waited += 0.2
            else:
                # EIP didn't change — BP may not have fired yet, capture anyway
                captured_img = img

            current_instr, code_listing = parse_code_overview(ocr(crop_code_section(captured_img)))
            data_view = parse_data_view(ocr(crop_data_section(captured_img)))
            row["Hit"] = hit
            row["Timestamp"] = time.strftime("%H:%M:%S")
            row["Instruction"] = current_instr
            row["Code_Listing"] = code_listing
            row["Data_View"] = data_view

            if args.screenshots:
                screenshot_dir.mkdir(exist_ok=True)
                shot_path = screenshot_dir / f"hit_{hit:04d}.png"
                captured_img.save(shot_path)
                row["Screenshot"] = str(shot_path)
            else:
                row["Screenshot"] = ""

            rows.append(row)
            prev_eip = row.get("EIP", "")
            print(f"  Hit {hit:4d}: EIP={row.get('EIP','')}  {row.get('Instruction','')}")

            # Save incrementally every 10 hits so you don't lose data
            if hit % 10 == 0:
                save_excel(rows, output_path)

    except KeyboardInterrupt:
        print("\nStopped by user.")

    save_excel(rows, output_path)


if __name__ == "__main__":
    main()
