"""Builds the app icons from assets/source-icon.webp (needs Pillow).

The source already has the look the app uses: a transparent background, a
white ring and the green plant. Only the ring and the plant are opaque; the
inside of the ring is transparent too, so the taskbar colour shows through.
This script keeps the source's own colours and alpha, snaps the WebP's
near-opaque alpha (253-254) to fully opaque, drops the near-invisible
compression haze, crops to the ring, centres it on a transparent square and
writes:

  assets/AppIcon.png          512 px (Flet window icon on Linux, packaging)
  assets/AppIcon.ico          16-256 px (Windows window, taskbar and .exe)
  assets/AppIcon-<n>.png      48 / 128 / 256 px (Linux .desktop, top bar)

Run from the app folder:  python tools/make_icon.py
"""

from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"
MARGIN = 0.02          # transparent border, fraction of the side
HAZE_ALPHA = 12        # below this a pixel is compression noise, made fully transparent
OPAQUE_ALPHA = 245     # at or above this a pixel is solid (the WebP stores 253-254)


def cleaned(image: Image.Image) -> Image.Image:
    alpha = image.getchannel("A").point(lambda a: 0 if a < HAZE_ALPHA else 255 if a >= OPAQUE_ALPHA else a)
    out = image.copy()
    out.putalpha(alpha)
    return out


def squared(image: Image.Image) -> Image.Image:
    ring = image.crop(image.getchannel("A").getbbox())
    side = round(max(ring.size) / (1 - 2 * MARGIN))
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(ring, ((side - ring.width) // 2, (side - ring.height) // 2))
    return canvas


def main() -> None:
    icon = squared(cleaned(Image.open(ASSETS / "source-icon.webp").convert("RGBA")))
    big = icon.resize((512, 512), Image.LANCZOS)
    big.save(ASSETS / "AppIcon.png")
    for size in (48, 128, 256):
        icon.resize((size, size), Image.LANCZOS).save(ASSETS / f"AppIcon-{size}.png")
    big.save(ASSETS / "AppIcon.ico", sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    print(f"icons written to {ASSETS}")


if __name__ == "__main__":
    main()
