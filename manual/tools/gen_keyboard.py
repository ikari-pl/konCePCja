#!/usr/bin/env python3
"""Generate a clean, geometrically perfect CPC6128 keyboard layout SVG.

Based on the traced outline at ~/Downloads/kb-outline.svg for overall
positioning, with all keys snapped to a precise grid.
"""

# Grid parameters (matching traced SVG approximate dimensions)
KEY_W = 17.0      # standard key width
KEY_H = 17.0      # standard key height
GAP = 0.8         # gap between keys
CORNER_R = 1.5    # key corner radius
STROKE_W = 0.8    # key outline stroke width

# Overall keyboard origin (matching trace)
OX = 85.0         # left edge of first key
OY = 206.0        # top edge of first row

# Section gaps
SECTION_GAP = 6.0  # gap between main keyboard and cursor/numpad sections

def key_x(col):
    """X position for column (in key units)."""
    return OX + col * (KEY_W + GAP)

def key_y(row):
    """Y position for row."""
    return OY + row * (KEY_H + GAP)

def rect_key(x, y, w, h, label="", sublabel=""):
    """Generate SVG for a rectangular key."""
    return (
        f'<rect x="{x:.2f}" y="{y:.2f}" width="{w:.2f}" height="{h:.2f}" '
        f'rx="{CORNER_R}" ry="{CORNER_R}" fill="none" stroke="black" '
        f'stroke-width="{STROKE_W}"/>'
    )

def l_shaped_return(x_top, y_top, w_top, x_bot, y_bot, w_bot, h_each):
    """Generate SVG for the L-shaped RETURN key.
    Top part is narrower (right-aligned with bottom).
    """
    # The L-shape: top-right portion + bottom full width
    r = CORNER_R
    x_right = x_bot + w_bot
    y_bottom = y_bot + h_each

    path = (
        f'M{x_top + r:.2f},{y_top:.2f} '
        f'L{x_right - r:.2f},{y_top:.2f} '
        f'Q{x_right:.2f},{y_top:.2f} {x_right:.2f},{y_top + r:.2f} '
        f'L{x_right:.2f},{y_bottom - r:.2f} '
        f'Q{x_right:.2f},{y_bottom:.2f} {x_right - r:.2f},{y_bottom:.2f} '
        f'L{x_bot + r:.2f},{y_bottom:.2f} '
        f'Q{x_bot:.2f},{y_bottom:.2f} {x_bot:.2f},{y_bottom - r:.2f} '
        f'L{x_bot:.2f},{y_bot + r:.2f} '
        f'Q{x_bot:.2f},{y_bot:.2f} {x_bot + r:.2f},{y_bot:.2f} '
        f'L{x_top:.2f},{y_bot:.2f} '
        f'L{x_top:.2f},{y_top + r:.2f} '
        f'Q{x_top:.2f},{y_top:.2f} {x_top + r:.2f},{y_top:.2f} Z'
    )
    return (
        f'<path d="{path}" fill="none" stroke="black" '
        f'stroke-width="{STROKE_W}"/>'
    )


def generate():
    parts = []
    parts.append(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<svg xmlns="http://www.w3.org/2000/svg" '
        'viewBox="0 0 498.898 708.661">\n'
        '<g>'
    )

    # ── Horizontal separator lines ──
    parts.append(f'<line x1="55" y1="191.9" x2="439" y2="191.9" stroke="black" stroke-width="0.8"/>')
    parts.append(f'<line x1="55" y1="310.2" x2="439" y2="310.2" stroke="black" stroke-width="0.8"/>')

    # ── Function key label strip (row above main keyboard) ──
    # Small markers between y≈170 and y≈191
    fkey_y = 170.0
    fkey_h = 4.5
    fkey_labels = ["ESC", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "^"]
    for i, label in enumerate(fkey_labels):
        x = key_x(i)
        parts.append(
            f'<rect x="{x:.2f}" y="{fkey_y:.2f}" '
            f'width="{KEY_W:.2f}" height="{fkey_h:.2f}" '
            f'fill="none" stroke="black" stroke-width="0.4"/>'
        )

    # ══════════════════════════════════════════
    # MAIN KEYBOARD
    # ══════════════════════════════════════════

    # Row 0: Number row
    # ESC(1.5u), 1-0(10×1u), -(1u), ^(1u), CLR(1u), DEL(1u)
    row = 0
    col = 0.0
    # ESC - wider
    esc_w = KEY_W * 1.5 + GAP * 0.5
    parts.append(rect_key(key_x(0), key_y(row), esc_w, KEY_H))
    col = 1.5 + 0.5 * GAP / (KEY_W + GAP)  # skip ESC width

    # Number keys 1-0 and -, ^, CLR, DEL
    num_labels = ["1!", "2\"", "3#", "4$", "5%", "6&", "7'", "8(", "9)", "0_", "-=", "^£", "CLR", "DEL"]
    for i, label in enumerate(num_labels):
        x = OX + esc_w + GAP + i * (KEY_W + GAP)
        parts.append(rect_key(x, key_y(row), KEY_W, KEY_H))

    # Row 1: QWERTY row
    # TAB(1.75u), Q-P(10×1u), @(1u), [(1u), RETURN top part
    row = 1
    tab_w = KEY_W * 1.75 + GAP * 0.75
    parts.append(rect_key(key_x(0), key_y(row), tab_w, KEY_H))

    qwerty_labels = ["Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "@", "["]
    for i, label in enumerate(qwerty_labels):
        x = OX + tab_w + GAP + i * (KEY_W + GAP)
        parts.append(rect_key(x, key_y(row), KEY_W, KEY_H))

    # Row 2: ASDF row
    # CAPS LOCK(2u), A-L(9×1u), ;(1u), :(1u), ](1u)
    row = 2
    caps_w = KEY_W * 2.0 + GAP
    parts.append(rect_key(key_x(0), key_y(row), caps_w, KEY_H))

    asdf_labels = ["A", "S", "D", "F", "G", "H", "J", "K", "L", ";", ":", "]"]
    for i, label in enumerate(asdf_labels):
        x = OX + caps_w + GAP + i * (KEY_W + GAP)
        parts.append(rect_key(x, key_y(row), KEY_W, KEY_H))

    # RETURN key (L-shaped, spanning rows 1-2)
    # Top part: after [ key in row 1
    # Bottom part: after ] key in row 2, wider extending left
    ret_top_x = OX + tab_w + GAP + 12 * (KEY_W + GAP)
    ret_top_w = KEY_W * 1.25
    ret_bot_x = OX + caps_w + GAP + 12 * (KEY_W + GAP)
    ret_bot_w = ret_top_x + ret_top_w - ret_bot_x
    parts.append(l_shaped_return(
        ret_top_x, key_y(1), ret_top_w,
        ret_bot_x, key_y(2), ret_bot_w, KEY_H
    ))

    # Row 3: ZXCV row
    # SHIFT(2.25u), \(1u), Z-M(7×1u), ,(1u), .(1u), /(1u), SHIFT(2.25u)
    row = 3
    shift_w = KEY_W * 2.25 + GAP * 1.25
    parts.append(rect_key(key_x(0), key_y(row), shift_w, KEY_H))

    zxcv_labels = ["\\", "Z", "X", "C", "V", "B", "N", "M", ",", ".", "/"]
    for i, label in enumerate(zxcv_labels):
        x = OX + shift_w + GAP + i * (KEY_W + GAP)
        parts.append(rect_key(x, key_y(row), KEY_W, KEY_H))

    # Right SHIFT
    rshift_x = OX + shift_w + GAP + 11 * (KEY_W + GAP)
    parts.append(rect_key(rshift_x, key_y(row), shift_w, KEY_H))

    # Row 4: Bottom row
    # CTRL(1.5u), COPY(1.5u), SPACE(~8u), gap, then cursor/numpad area
    row = 4
    ctrl_w = KEY_W * 1.5 + GAP * 0.5
    parts.append(rect_key(key_x(0), key_y(row), ctrl_w, KEY_H))

    copy_x = OX + ctrl_w + GAP
    copy_w = ctrl_w
    parts.append(rect_key(copy_x, key_y(row), copy_w, KEY_H))

    space_x = copy_x + copy_w + GAP
    # Space bar extends to roughly where right shift ends
    space_w = rshift_x + shift_w - space_x
    parts.append(rect_key(space_x, key_y(row), space_w, KEY_H))

    # ══════════════════════════════════════════
    # CURSOR KEYS (right of DEL, rows 0-1 area)
    # ══════════════════════════════════════════
    cursor_ox = OX + esc_w + GAP + 14 * (KEY_W + GAP) + SECTION_GAP

    # Cursor Up
    parts.append(rect_key(cursor_ox + (KEY_W + GAP), key_y(0), KEY_W, KEY_H))
    # Cursor Left, Down, Right
    parts.append(rect_key(cursor_ox, key_y(1), KEY_W, KEY_H))
    parts.append(rect_key(cursor_ox + (KEY_W + GAP), key_y(1), KEY_W, KEY_H))
    parts.append(rect_key(cursor_ox + 2 * (KEY_W + GAP), key_y(1), KEY_W, KEY_H))
    # COPY key
    parts.append(rect_key(cursor_ox, key_y(0), KEY_W, KEY_H))

    # ══════════════════════════════════════════
    # NUMERIC KEYPAD (rightmost section)
    # ══════════════════════════════════════════
    np_ox = cursor_ox + 3 * (KEY_W + GAP) + SECTION_GAP

    # Numpad row 0: F7/7, F8/8, F9/9
    for i in range(3):
        parts.append(rect_key(np_ox + i * (KEY_W + GAP), key_y(0), KEY_W, KEY_H))

    # Numpad row 1: F4/4, F5/5, F6/6
    for i in range(3):
        parts.append(rect_key(np_ox + i * (KEY_W + GAP), key_y(1), KEY_W, KEY_H))

    # Numpad row 2: F1/1, F2/2, F3/3
    for i in range(3):
        parts.append(rect_key(np_ox + i * (KEY_W + GAP), key_y(2), KEY_W, KEY_H))

    # Numpad row 3: F0/0 (2u wide), F./. (1u)
    f0_w = KEY_W * 2 + GAP
    parts.append(rect_key(np_ox, key_y(3), f0_w, KEY_H))
    parts.append(rect_key(np_ox + f0_w + GAP, key_y(3), KEY_W, KEY_H))

    # ENTER key (tall, spanning rows 2-3, right of numpad)
    enter_x = np_ox + 3 * (KEY_W + GAP)
    enter_h = KEY_H * 2 + GAP
    parts.append(rect_key(enter_x, key_y(2), KEY_W, enter_h))

    # DEL key above ENTER (numpad row 0-1 right column)
    parts.append(rect_key(enter_x, key_y(0), KEY_W, KEY_H))
    # CLR key (numpad row 1 right column)  -- wait, DEL is row 0
    # Actually: numpad has 4 columns: 3 number + 1 operation
    # Let me add the 4th column for DEL
    parts.append(rect_key(enter_x, key_y(1), KEY_W, KEY_H))

    parts.append('</g>\n</svg>')
    return '\n'.join(parts)


if __name__ == '__main__':
    import os
    out = os.path.join(os.path.dirname(__file__), '..', 'images', 'keyboard-layout.svg')
    svg = generate()
    with open(out, 'w') as f:
        f.write(svg)
    print(f"Written to {out}")
