#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import math
import argparse
from PIL import Image, ImageDraw, ImageFont

W, H = 240, 240

def ensure_dir(p: str) -> None:
    os.makedirs(p, exist_ok=True)

def clamp(x: float, a: float, b: float) -> float:
    return a if x < a else b if x > b else x

def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t

def rgba(r, g, b, a=255):
    return (int(r), int(g), int(b), int(a))

def draw_circle(draw: ImageDraw.ImageDraw, cx, cy, r, fill, outline=None, width=1):
    box = [cx - r, cy - r, cx + r, cy + r]
    draw.ellipse(box, fill=fill, outline=outline, width=width)

def draw_line(draw: ImageDraw.ImageDraw, x0, y0, x1, y1, fill, width=2):
    draw.line((x0, y0, x1, y1), fill=fill, width=width)

def draw_orbit(draw: ImageDraw.ImageDraw, cx, cy, r, color):
    box = [cx - r, cy - r, cx + r, cy + r]
    draw.ellipse(box, outline=color, width=1)

def draw_label(draw: ImageDraw.ImageDraw, text: str):
    # Minimal text to keep size small.
    try:
        font = ImageFont.load_default()
    except Exception:
        font = None
    draw.text((6, 6), text, fill=rgba(220, 220, 220, 255), font=font)

def compose_alpha(base: Image.Image, overlay: Image.Image) -> Image.Image:
    return Image.alpha_composite(base, overlay)

def make_canvas() -> Image.Image:
    return Image.new("RGBA", (W, H), rgba(0, 0, 0, 255))

def shadow_cone_poly(src_cx, src_cy, occl_cx, occl_cy, occl_r, length=180, spread=1.25):
    # Build a simple shadow cone polygon behind the occluder.
    vx = occl_cx - src_cx
    vy = occl_cy - src_cy
    vlen = math.hypot(vx, vy) + 1e-6
    ux, uy = vx / vlen, vy / vlen

    # Perpendicular
    px, py = -uy, ux

    # Two tangency-ish points on occluder
    t = occl_r * spread
    a1 = (occl_cx + px * t, occl_cy + py * t)
    a2 = (occl_cx - px * t, occl_cy - py * t)

    # Far points behind occluder
    fx = occl_cx + ux * length
    fy = occl_cy + uy * length
    b1 = (fx + px * (t * 0.35), fy + py * (t * 0.35))
    b2 = (fx - px * (t * 0.35), fy - py * (t * 0.35))

    return [a1, a2, b2, b1]

def draw_shadow_cone(img: Image.Image, poly, alpha=90):
    overlay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(overlay)
    d.polygon(poly, fill=rgba(30, 30, 30, alpha))
    return compose_alpha(img, overlay)

def lit_moon_image(r: int, sun_dir_x: float) -> Image.Image:
    # Create a simple moon phase disk:
    # sun_dir_x = +1 means sun from left->right? Here we treat sun from LEFT,
    # so if sun_dir_x < 0, light comes from left side of the moon.
    # Implementation: draw full disk, then subtract a shifted dark disk.
    size = r * 2 + 2
    base = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(base)

    # Full disk (bright side base)
    d.ellipse((1, 1, size - 2, size - 2), fill=rgba(210, 210, 210, 255))

    # Dark overlay disk shift controls phase
    # shift in [-r, +r]
    shift = int(clamp(sun_dir_x, -1.0, 1.0) * r)
    dark = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    dd = ImageDraw.Draw(dark)
    dd.ellipse((1 + shift, 1, size - 2 + shift, size - 2), fill=rgba(0, 0, 0, 255))

    # Composite: keep base but apply dark as mask (simple)
    out = Image.alpha_composite(base, dark)
    return out

def paste_center(img: Image.Image, sprite: Image.Image, cx: int, cy: int):
    x = cx - sprite.size[0] // 2
    y = cy - sprite.size[1] // 2
    img.alpha_composite(sprite, (x, y))

def gen_lunar_eclipse(out_dir: str, frames: int):
    ensure_dir(out_dir)
    sun = (45, 120, 18)
    earth = (120, 120, 22)

    # Moon moves through earth shadow on the right side
    moon_y = 120
    moon_r = 12
    x0, x1 = 175, 235  # right side travel

    for i in range(frames):
        t = i / (frames - 1)
        img = make_canvas()
        d = ImageDraw.Draw(img)

        draw_label(d, "LUNAR ECLIPSE")

        # Orbits / guide
        draw_line(d, sun[0] + sun[2], sun[1], earth[0] - earth[2], earth[1], fill=rgba(60, 60, 60, 255), width=1)

        # Bodies
        draw_circle(d, sun[0], sun[1], sun[2], fill=rgba(255, 190, 40, 255))
        draw_circle(d, earth[0], earth[1], earth[2], fill=rgba(60, 140, 255, 255))

        # Shadow cone behind Earth (to the right)
        poly = shadow_cone_poly(sun[0], sun[1], earth[0], earth[1], earth[2], length=150, spread=1.15)
        img = draw_shadow_cone(img, poly, alpha=120)
        # Rebind draw handle after alpha-composite (new image object returned).
        d = ImageDraw.Draw(img)

        # Moon position
        mx = int(lerp(x0, x1, t))
        # Determine darkness based on whether moon is inside the shadow wedge approx
        # Use a simple rule: darker near the center of travel.
        depth = 1.0 - abs(t - 0.5) / 0.5  # 0..1
        depth = clamp(depth, 0.0, 1.0)

        # Moon base
        moon_col = (210, 210, 210)
        # Apply eclipse tint (reddish) and darkening
        dark_factor = lerp(0.15, 1.0, 1.0 - depth)  # smaller at center
        red_tint = lerp(0.0, 60.0, depth)
        mr = int(moon_col[0] * dark_factor + red_tint)
        mg = int(moon_col[1] * dark_factor)
        mb = int(moon_col[2] * dark_factor)

        draw_circle(d, mx, moon_y, moon_r, fill=rgba(mr, mg, mb, 255))

        # Save
        img.convert("RGB").save(os.path.join(out_dir, f"frame_{i:04d}.png"), "PNG")

def gen_solar_eclipse(out_dir: str, frames: int):
    ensure_dir(out_dir)
    sun = (55, 120, 22)
    earth = (195, 120, 24)
    moon_r = 12

    for i in range(frames):
        t = i / (frames - 1)
        img = make_canvas()
        d = ImageDraw.Draw(img)

        draw_label(d, "SOLAR ECLIPSE")

        # Bodies
        draw_circle(d, sun[0], sun[1], sun[2], fill=rgba(255, 190, 40, 255))
        draw_circle(d, earth[0], earth[1], earth[2], fill=rgba(60, 140, 255, 255))

        # Moon passes between Sun and Earth
        mx = int(lerp(90, 150, t))
        my = 120

        # Shadow cone behind Moon (to the right, toward Earth)
        poly = shadow_cone_poly(sun[0], sun[1], mx, my, moon_r, length=140, spread=1.2)
        img = draw_shadow_cone(img, poly, alpha=110)
        d = ImageDraw.Draw(img)

        # Shadow spot on Earth surface (simple)
        # Project t to an angle on Earth rim.
        spot_angle = lerp(-0.25, 0.25, t) * math.pi
        sx = int(earth[0] - math.cos(spot_angle) * earth[2] * 0.95)
        sy = int(earth[1] + math.sin(spot_angle) * earth[2] * 0.35)
        draw_circle(d, sx, sy, 4, fill=rgba(20, 20, 20, 180))

        # Moon disk (dark)
        draw_circle(d, mx, my, moon_r, fill=rgba(90, 90, 90, 255))

        img.convert("RGB").save(os.path.join(out_dir, f"frame_{i:04d}.png"), "PNG")

def gen_moon_orbit(out_dir: str, frames: int):
    ensure_dir(out_dir)
    earth = (120, 120, 26)
    orbit_r = 70
    moon_r = 12
    sun_dir = (-1.0, 0.0)  # sun from left

    for i in range(frames):
        t = i / (frames - 1)
        ang = t * 2.0 * math.pi

        img = make_canvas()
        d = ImageDraw.Draw(img)

        draw_label(d, "MOON ORBIT")

        draw_orbit(d, earth[0], earth[1], orbit_r, rgba(60, 60, 60, 255))

        # Earth
        draw_circle(d, earth[0], earth[1], earth[2], fill=rgba(60, 140, 255, 255))

        # Moon position
        mx = int(earth[0] + orbit_r * math.cos(ang))
        my = int(earth[1] + orbit_r * math.sin(ang))

        # Moon phase: compute sun direction relative to moon center
        # Light side faces sun (left), so use sign of (sun_dir dot (moon->sun)).
        # Approx use x position only: if moon is on right, it's full; left -> new.
        # Convert to [-1, +1]
        phase = clamp((earth[0] - mx) / float(orbit_r), -1.0, 1.0)  # +1 near left (new), -1 right (full)
        # We want shift control where negative means light from left
        sprite = lit_moon_image(moon_r, -phase)
        paste_center(img, sprite, mx, my)

        img.convert("RGB").save(os.path.join(out_dir, f"frame_{i:04d}.png"), "PNG")

def gen_earth_rotation(out_dir: str, frames: int):
    ensure_dir(out_dir)
    earth = (120, 120, 56)
    sun_from_left = True

    # Pre-draw a simple "continent band" texture to shift
    tex_w, tex_h = 200, 120
    tex = Image.new("RGBA", (tex_w, tex_h), (0, 0, 0, 0))
    td = ImageDraw.Draw(tex)
    # Simple blobs
    td.ellipse((20, 30, 80, 90), fill=rgba(40, 170, 90, 255))
    td.ellipse((90, 20, 150, 70), fill=rgba(40, 170, 90, 255))
    td.ellipse((130, 60, 185, 110), fill=rgba(40, 170, 90, 255))

    for i in range(frames):
        t = i / (frames - 1)
        img = make_canvas()
        d = ImageDraw.Draw(img)

        draw_label(d, "EARTH ROTATION")

        # Earth base
        draw_circle(d, earth[0], earth[1], earth[2], fill=rgba(60, 140, 255, 255))

        # Day/night shading overlay (terminator)
        overlay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        od = ImageDraw.Draw(overlay)

        # Create a half-plane dark mask on the right if sun from left
        if sun_from_left:
            od.rectangle((earth[0], earth[1] - earth[2] - 2, earth[0] + earth[2] + 2, earth[1] + earth[2] + 2),
                         fill=rgba(0, 0, 0, 110))

        # Clip dark mask to earth circle via alpha mask
        mask = Image.new("L", (W, H), 0)
        md = ImageDraw.Draw(mask)
        md.ellipse((earth[0]-earth[2], earth[1]-earth[2], earth[0]+earth[2], earth[1]+earth[2]), fill=255)
        overlay.putalpha(Image.composite(overlay.split()[-1], Image.new("L", (W, H), 0), mask))
        img = compose_alpha(img, overlay)

        # Continents (shift texture to simulate rotation)
        shift = int(t * tex_w)
        tex_shifted = Image.new("RGBA", (tex_w, tex_h), (0, 0, 0, 0))
        tex_shifted.alpha_composite(tex, (-shift, 0))
        tex_shifted.alpha_composite(tex, (tex_w - shift, 0))

        # Paste continents onto earth with circular mask
        cont = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        cont.alpha_composite(tex_shifted, (earth[0] - tex_w // 2, earth[1] - tex_h // 2))

        cont_mask = Image.new("L", (W, H), 0)
        cmd = ImageDraw.Draw(cont_mask)
        cmd.ellipse((earth[0]-earth[2], earth[1]-earth[2], earth[0]+earth[2], earth[1]+earth[2]), fill=180)

        # Apply mask
        cont.putalpha(cont_mask)
        img = compose_alpha(img, cont)

        # City marker dot rotates around
        ang = t * 2 * math.pi
        mx = int(earth[0] + (earth[2] * 0.75) * math.cos(ang))
        my = int(earth[1] + (earth[2] * 0.30) * math.sin(ang))
        ImageDraw.Draw(img).ellipse((mx-3, my-3, mx+3, my+3), fill=rgba(255, 80, 80, 255))

        img.convert("RGB").save(os.path.join(out_dir, f"frame_{i:04d}.png"), "PNG")

def gen_year_seasons(out_dir: str, frames: int):
    ensure_dir(out_dir)
    sun = (120, 120, 20)
    orbit_r = 78
    earth_r = 12
    moon_r = 5
    moon_orbit_r = 18

    # Fixed axis direction (points to upper-right)
    axis_ang = -math.pi / 4.0

    for i in range(frames):
        t = i / (frames - 1)
        ang = t * 2.0 * math.pi

        img = make_canvas()
        d = ImageDraw.Draw(img)

        draw_label(d, "YEAR + SEASONS")

        # Sun
        draw_circle(d, sun[0], sun[1], sun[2], fill=rgba(255, 190, 40, 255))

        # Earth orbit guide
        draw_orbit(d, sun[0], sun[1], orbit_r, rgba(50, 50, 50, 255))

        # Earth position
        ex = int(sun[0] + orbit_r * math.cos(ang))
        ey = int(sun[1] + orbit_r * math.sin(ang))

        # Earth
        draw_circle(d, ex, ey, earth_r, fill=rgba(60, 140, 255, 255))

        # Axis line (fixed in space, not rotating with orbit)
        ax_len = earth_r + 10
        x0 = ex - math.cos(axis_ang) * ax_len
        y0 = ey - math.sin(axis_ang) * ax_len
        x1 = ex + math.cos(axis_ang) * ax_len
        y1 = ey + math.sin(axis_ang) * ax_len
        draw_line(d, x0, y0, x1, y1, fill=rgba(220, 220, 220, 255), width=2)

        # Moon orbit around Earth (faster)
        mang = ang * 4.0
        mx = int(ex + moon_orbit_r * math.cos(mang))
        my = int(ey + moon_orbit_r * math.sin(mang))
        draw_circle(d, mx, my, moon_r, fill=rgba(200, 200, 200, 255))

        # Minimal season markers (4 corners around sun)
        # Keep as tiny letters to avoid size bloat.
        try:
            font = ImageFont.load_default()
        except Exception:
            font = None
        d.text((W-42, 10),  "SPR", fill=rgba(180, 220, 180, 255), font=font)
        d.text((W-42, H-18), "SUM", fill=rgba(220, 220, 160, 255), font=font)
        d.text((10, H-18),   "AUT", fill=rgba(220, 180, 140, 255), font=font)
        d.text((10, 10),     "WIN", fill=rgba(180, 200, 230, 255), font=font)

        img.convert("RGB").save(os.path.join(out_dir, f"frame_{i:04d}.png"), "PNG")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="out_astro", help="Output root directory")
    ap.add_argument("--fps", type=int, default=10)
    ap.add_argument("--seconds_short", type=int, default=6)
    ap.add_argument("--seconds_long", type=int, default=8)
    args = ap.parse_args()

    out = args.out
    ensure_dir(out)

    frames_short = args.fps * args.seconds_short  # 60 by default
    frames_long  = args.fps * args.seconds_long   # 80 by default

    gen_lunar_eclipse(os.path.join(out, "lunar_eclipse"), frames_short)
    gen_solar_eclipse(os.path.join(out, "solar_eclipse"), frames_short)
    gen_moon_orbit(os.path.join(out, "moon_orbit"), frames_short)
    gen_earth_rotation(os.path.join(out, "earth_rotation"), frames_short)
    gen_year_seasons(os.path.join(out, "year_seasons"), frames_long)

    print("PNG frames generated under:", out)

if __name__ == "__main__":
    main()
