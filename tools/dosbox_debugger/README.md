# DOSBox-X Breakpoint Register Logger

Automatically captures register state, disassembly, and memory data each time a breakpoint fires in the DOSBox-X Debugger, then presses F5 to continue. Results saved to Excel.

## Setup

```bash
pip install pillow pytesseract openpyxl pandas
sudo apt install xdotool scrot tesseract-ocr
```

## Usage

1. Open DOSBox-X and start your program
2. Open the debugger: **Alt+Pause**
3. Set your breakpoint in DOSBox-X (e.g. `BP 0868:00016572`)
4. Press F5 once in DOSBox-X to run until the first hit
5. Run the logger:

```bash
python3 bp_logger.py --output registers.xlsx --hits 200
```

The tool captures the current state, then loops: press F5 → wait for BP → capture → repeat.

Press **Ctrl+C** at any time — data collected so far is saved immediately.

## Options

| Flag | Default | Description |
|------|---------|-------------|
| `--output` | `bp_registers.xlsx` | Output Excel filename |
| `--hits` | `100` | Number of breakpoint hits to capture |
| `--delay` | `0.4` | Seconds to wait after F5 before polling for BP |
| `--screenshots` | off | Also save a PNG per hit in `<output>_screenshots/` |

## Excel Columns

| Column | Description |
|--------|-------------|
| Hit | Sequential hit number |
| Timestamp | Wall-clock time of capture |
| EIP | Instruction pointer |
| EAX–EDI | General-purpose 32-bit registers |
| CS DS ES FS GS SS | Segment registers |
| F_C F_Z F_S F_A F_P F_T F_I | CPU flags (0 or 1) |
| Instruction | Current instruction at EIP |
| Code_Listing | Full disassembly block visible in Code Overview |
| Data_View | Hex dump lines from Data View panel |
| Screenshot | Path to PNG screenshot (if --screenshots used) |
```

## Window Layout Assumed

The script assumes DOSBox-X Debugger panels in this order (top→bottom):
- **Register Overview** (~top 17%)
- **Data View** (~17–38%)
- **Code Overview** (~38–62%)
- **Output** (~62–100%)

If your window layout differs, adjust the crop percentages in `bp_logger.py`.
