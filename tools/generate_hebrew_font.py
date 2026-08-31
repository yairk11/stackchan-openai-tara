from pathlib import Path
from PIL import ImageFont

FONT_PATH = r"C:\Windows\Fonts\segoeui.ttf"
FONT_SIZE = 22

OUT = Path(__file__).resolve().parent.parent / "src" / "HebrewFont22.h"

CHARS = (
    "אבגדהוזחטיכלמנסעפצקרשת"
    "ךםןףץ"
    "0123456789"
    " .,!?():;+-/%"
)

font = ImageFont.truetype(FONT_PATH, FONT_SIZE)

glyphs = []

for ch in CHARS:
    bbox = font.getbbox(ch)

    x0, y0, x1, y1 = bbox
    width = max(1, x1 - x0)
    height = max(1, y1 - y0)

    mask = font.getmask(ch, mode="1")
    mask_width, mask_height = mask.size

    rows = []

    pixels = list(mask)

    for y in range(mask_height):
        row_bytes = []
        current = 0
        bit_count = 0

        for x in range(mask_width):
            value = pixels[y * mask_width + x]

            current <<= 1

            if value:
                current |= 1

            bit_count += 1

            if bit_count == 8:
                row_bytes.append(current)
                current = 0
                bit_count = 0

        if bit_count:
            current <<= (8 - bit_count)
            row_bytes.append(current)

        rows.extend(row_bytes)

    glyphs.append({
        "char": ch,
        "codepoint": ord(ch),
        "width": mask_width,
        "height": mask_height,
        "x_advance": int(round(font.getlength(ch))),
        "data": rows,
    })

with OUT.open("w", encoding="utf-8", newline="\n") as f:
    f.write("#pragma once\n\n")
    f.write("#include <Arduino.h>\n\n")
    f.write("struct TaraGlyph22 {\n")
    f.write("    uint16_t codepoint;\n")
    f.write("    uint8_t width;\n")
    f.write("    uint8_t height;\n")
    f.write("    uint8_t xAdvance;\n")
    f.write("    const uint8_t* bitmap;\n")
    f.write("};\n\n")

    for index, glyph in enumerate(glyphs):
        data = glyph["data"]

        f.write(
            f"static const uint8_t taraGlyph22Bitmap_{index}[] PROGMEM = {{\n"
        )

        for i in range(0, len(data), 16):
            chunk = data[i:i + 16]
            f.write(
                "    " +
                ", ".join(f"0x{value:02X}" for value in chunk) +
                ",\n"
            )

        f.write("};\n\n")

    f.write("static const TaraGlyph22 taraGlyph22[] = {\n")

    for index, glyph in enumerate(glyphs):
        f.write(
            "    {"
            f"0x{glyph['codepoint']:04X}, "
            f"{glyph['width']}, "
            f"{glyph['height']}, "
            f"{glyph['x_advance']}, "
            f"taraGlyph22Bitmap_{index}"
            "},\n"
        )

    f.write("};\n\n")

    f.write(
        "static constexpr size_t taraGlyph22Count = "
        "sizeof(taraGlyph22) / sizeof(taraGlyph22[0]);\n"
    )

print(f"Generated: {OUT}")
print(f"Glyphs: {len(glyphs)}")
