Import("env")  # type: ignore[name-defined]

from pathlib import Path
import re

PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))  # type: ignore[name-defined]
PIO_DIR = PROJECT_DIR / "pio"
INCLUDE_DIR = PROJECT_DIR / "include" / "generated"

# サイドセットのビット数と有効フラグを保持するグローバル変数
_sideset_bits = 0


def _sanitize_instruction(line: str) -> str:
    line = line.split(";", 1)[0]
    return line.strip()


def _normalize_program_name(name: str) -> str:
    return re.sub(r"[^0-9A-Za-z_]", "_", name)


def _encode_set(args: str) -> str:
    parts = [p.strip().lower() for p in args.split(",")]
    if len(parts) != 2:
        raise ValueError(f"Invalid set syntax: {args}")
    dst, value = parts
    return f"(uint16_t)pio_encode_set(pio_{dst}, {value})"


def _encode_mov(args: str) -> str:
    parts = [p.strip().lower() for p in args.split(",")]
    if len(parts) != 2:
        raise ValueError(f"Invalid mov syntax: {args}")
    dst, src = parts
    if src.startswith("~"):
        src_val = src[1:]
        if src_val in ("0", "null"):
            return f"(uint16_t)pio_encode_mov_not(pio_{dst}, pio_null)"
        return f"(uint16_t)pio_encode_mov_not(pio_{dst}, pio_{src_val})"

    return f"(uint16_t)pio_encode_mov(pio_{dst}, pio_{src})"


def _encode_push(args: str) -> str:
    mode = args.strip().lower()
    if mode == "noblock":
        return "(uint16_t)pio_encode_push(false, false)"
    if mode in ("block", ""):
        return "(uint16_t)pio_encode_push(false, true)"
    raise ValueError(f"Unsupported push mode: {args}")


def _encode_pull(args: str) -> str:
    mode = args.strip().lower()
    if mode == "noblock":
        return "(uint16_t)pio_encode_pull(false, false)"
    if mode in ("block", ""):
        return "(uint16_t)pio_encode_pull(false, true)"
    raise ValueError(f"Unsupported pull mode: {args}")


def _encode_jmp(args: str, labels: dict[str, int]) -> str:
    parts = [p.strip().lower() for p in re.split(r"[\s,]+", args.strip()) if p.strip()]
    if len(parts) == 1:
        target = parts[0]
        condition = None
    elif len(parts) == 2:
        condition = parts[0]
        target = parts[1]
    else:
        raise ValueError(f"Invalid jmp syntax: {args}")

    if target not in labels:
        raise ValueError(f"Unknown label for jmp: {target}")

    if not condition:
        return f"(uint16_t)pio_encode_jmp({labels[target]})"

    cond_map = {
        "!x": "pio_encode_jmp_not_x",
        "x--": "pio_encode_jmp_x_dec",
        "!y": "pio_encode_jmp_not_y",
        "y--": "pio_encode_jmp_y_dec",
        "x!=y": "pio_encode_jmp_x_ne_y",
        "pin": "pio_encode_jmp_pin",
        "!osre": "pio_encode_jmp_not_osre",
    }

    if condition not in cond_map:
        raise ValueError(f"Unsupported jmp condition: {condition}")

    return f"(uint16_t){cond_map[condition]}({labels[target]})"


def _encode_wait(args: str) -> str:
    parts = [p.strip().lower() for p in re.split(r"[\s,]+", args.strip()) if p.strip()]
    if len(parts) != 3:
        raise ValueError(f"Invalid wait syntax: {args}")
    polarity_str, src_str, index_str = parts
    polarity_c = "true" if polarity_str == "1" else "false"
    
    if src_str == "pin":
        return f"(uint16_t)pio_encode_wait_pin({polarity_c}, {index_str})"
    elif src_str == "gpio":
        return f"(uint16_t)pio_encode_wait_gpio({polarity_c}, {index_str})"
    elif src_str == "irq":
        return f"(uint16_t)pio_encode_wait_irq({polarity_c}, false, {index_str})"
    else:
        raise ValueError(f"Unsupported wait source: {src_str}")


def _encode_nop(_: str) -> str:
    return "(uint16_t)pio_encode_nop()"


def _encode_out(args: str) -> str:
    # 例: "x, 32" または "x 32"
    parts = [
        p.strip().lower() for p in re.split(r"[\s,]+", args.strip()) if p.strip()
    ]
    if len(parts) != 2:
        raise ValueError(f"Invalid out syntax: {args}")
    dst, bit_count_str = parts

    return f"(uint16_t)pio_encode_out(pio_{dst}, {bit_count_str})"


def _encode_in(args: str) -> str:
    # 例: "pins, 4"
    parts = [
        p.strip().lower() for p in re.split(r"[\s,]+", args.strip()) if p.strip()
    ]
    if len(parts) != 2:
        raise ValueError(f"Invalid in syntax: {args}")
    src, bit_count_str = parts

    return f"(uint16_t)pio_encode_in(pio_{src}, {bit_count_str})"


def _encode_instruction(instr: str, labels: dict[str, int]) -> str:
    global _sideset_bits
    instr_clean = instr.strip()
    
    side_value = 0
    side_regex = re.search(r"\s+side\s+(?P<val>\d+)\s*$", instr_clean, re.IGNORECASE)
    if side_regex:
        if _sideset_bits == 0:
            raise ValueError(f"Found 'side' modifier but .side_set directive is missing: {instr}")
        side_value = int(side_regex.group("val"))
        instr_clean = re.sub(r"\s+side\s+\d+\s*$", "", instr_clean, flags=re.IGNORECASE).strip()

    match = re.match(r"^(?P<op>\w+)\s+(?P<args>.*)$", instr_clean)
    if not match:
        op = instr_clean.lower()
        args = ""
    else:
        op = match.group("op").lower()
        args = match.group("args").strip()

    base_encoded = ""
    if op == "set":
        base_encoded = _encode_set(args)
    elif op == "mov":
        base_encoded = _encode_mov(args)
    elif op == "jmp":
        base_encoded = _encode_jmp(args, labels)
    elif op == "push":
        base_encoded = _encode_push(args)
    elif op == "pull":
        base_encoded = _encode_pull(args)
    elif op == "nop":
        base_encoded = _encode_nop(args)
    elif op == "wait":
        base_encoded = _encode_wait(args)
    elif op == "out":
        base_encoded = _encode_out(args)
    elif op == "in":
        base_encoded = _encode_in(args)
    else:
        raise ValueError(f"Unsupported instruction: {instr}")

    if _sideset_bits > 0:
        shift_amount = 12
        side_mask = side_value << shift_amount
        return f"(uint16_t)((uint16_t)({base_encoded}) | 0x{side_mask:04x})"
        
    return f"(uint16_t)({base_encoded})"


def generate_pio_header(pio_path: Path) -> None:
    global _sideset_bits
    _sideset_bits = 0
    
    program_name = None
    instruction_lines: list[str] = []
    labels: dict[str, int] = {}
    wrap_target = None
    wrap = None

    for raw_line in pio_path.read_text(encoding="utf-8").splitlines():
        line = _sanitize_instruction(raw_line)
        if not line:
            continue

        if line.startswith(".program"):
            parts = line.split()
            if len(parts) != 2:
                raise ValueError(f"Invalid .program line in {pio_path}: {line}")
            program_name = _normalize_program_name(parts[1])
            continue

        if line.startswith(".side_set"):
            parts = line.split()
            if len(parts) < 2:
                raise ValueError(f"Invalid .side_set line in {pio_path}: {line}")
            _sideset_bits = int(parts[1])
            continue

        if line == ".wrap_target":
            wrap_target = len(instruction_lines)
            continue

        if line == ".wrap":
            if len(instruction_lines) == 0:
                raise ValueError(f".wrap cannot be first directive in {pio_path}")
            wrap = len(instruction_lines) - 1
            continue

        if line.endswith(":"):
            label = line[:-1].strip().lower()
            if not label:
                raise ValueError(f"Invalid empty label in {pio_path}")
            labels[label] = len(instruction_lines)
            continue

        instruction_lines.append(line)

    if not program_name:
        raise ValueError(f"Missing .program directive in {pio_path}")
    if wrap_target is None:
        raise ValueError(f"Missing .wrap_target directive in {pio_path}")
    if wrap is None:
        raise ValueError(f"Missing .wrap directive in {pio_path}")

    encoded_lines = [_encode_instruction(instr, labels) for instr in instruction_lines]

    output = []
    output.append("#pragma once")
    output.append("")
    output.append("// AUTO-GENERATED by scripts/generate_pio_headers.py from .pio source.")
    output.append("// DO NOT EDIT THIS FILE DIRECTLY.")
    output.append("")
    output.append("#if !PICO_NO_HARDWARE")
    output.append('#include "hardware/pio.h"')
    output.append("#endif")
    output.append("")
    output.append(f"#define {program_name}_wrap_target {wrap_target}")
    output.append(f"#define {program_name}_wrap {wrap}")
    output.append("")
    output.append(f"static const uint16_t {program_name}_program_instructions[] = {{")
    for line in encoded_lines:
        output.append(f"  {line},")
    output.append("};")
    output.append("")
    output.append(f"static const struct pio_program {program_name}_program = {{")
    output.append(f"  .instructions = {program_name}_program_instructions,")
    output.append(f"  .length = {len(encoded_lines)},")
    output.append("  .origin = -1,")
    output.append("};")
    output.append("")
    output.append(f"static inline pio_sm_config {program_name}_program_get_default_config(uint offset) {{")
    output.append("  pio_sm_config c = pio_get_default_sm_config();")
    output.append(f"  sm_config_set_wrap(&c, offset + {program_name}_wrap_target, offset + {program_name}_wrap);")
    
    if _sideset_bits > 0:
        output.append(f"  sm_config_set_sideset(&c, {_sideset_bits}, false, false);")
        
    output.append("  return c;")
    output.append("}")
    output.append("")

    pio_header_path = INCLUDE_DIR / f"{pio_path.stem}.pio.h"
    generated_text = "\n".join(output)

    if pio_header_path.exists():
        current_text = pio_header_path.read_text(encoding="utf-8")
        if current_text == generated_text:
            return

    pio_header_path.write_text(generated_text, encoding="utf-8")


for pio_file in PIO_DIR.glob("*.pio"):
    INCLUDE_DIR.mkdir(parents=True, exist_ok=True)
    generate_pio_header(pio_file)
