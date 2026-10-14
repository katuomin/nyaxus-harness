"""One-off: re-compress stored image attachments with the client normalizer's
rules (2048px long edge, smallest of jpeg 0.85 / jpeg 0.7 / lossless png that
fits 1.5 MiB), keeping any row that would not shrink. Run with the daemon
stopped; VACUUMs at the end to reclaim freed pages. Idempotent."""
import base64
import io
import os
import sqlite3

from PIL import Image, ImageOps

DB = "nyaxus.db"
MAX_LONG_EDGE = 2048
BUDGET = int(1.5 * 1024 * 1024)


def parse_data_url(content):
    if not content.startswith("data:"):
        return None
    header, _, payload = content.partition(",")
    if ";base64" not in header:
        return None
    mime = header[5:].split(";")[0]
    return mime, base64.b64decode(payload)


def has_transparency(img):
    if img.mode in ("RGBA", "LA"):
        alpha = img.getchannel("A")
    elif img.mode == "P" and "transparency" in img.info:
        alpha = img.convert("RGBA").getchannel("A")
    else:
        return False
    return alpha.getextrema()[0] < 255


def encode_ladder(img):
    """Same candidates the browser normalizer tries: jpeg 0.85, jpeg 0.7, png.
    Browser canvas flattens transparency to black when encoding jpeg, so do
    the same for alpha images."""
    candidates = []
    if has_transparency(img):
        background = Image.new("RGB", img.size, (0, 0, 0))
        rgba = img.convert("RGBA")
        background.paste(rgba, mask=rgba.getchannel("A"))
        rgb = background
    else:
        rgb = img.convert("RGB")
    for quality in (85, 70):
        buf = io.BytesIO()
        rgb.save(buf, "JPEG", quality=quality, optimize=True)
        candidates.append((buf.getvalue(), "image/jpeg"))
    buf = io.BytesIO()
    img.save(buf, "PNG", optimize=True)
    candidates.append((buf.getvalue(), "image/png"))
    return candidates


def normalize(data):
    """Return (payload, mime, width, height) under the budget, or raise."""
    img = Image.open(io.BytesIO(data))
    img = ImageOps.exif_transpose(img)
    if max(img.size) > MAX_LONG_EDGE:
        scale = MAX_LONG_EDGE / max(img.size)
        img = img.resize(
            (max(1, round(img.width * scale)), max(1, round(img.height * scale))),
            Image.LANCZOS,
        )
    for shrink in (1.0, 0.75, 0.55):
        if shrink == 1.0:
            surface = img
        else:
            surface = img.resize(
                (max(1, round(img.width * shrink)), max(1, round(img.height * shrink))),
                Image.LANCZOS,
            )
        best = None
        for payload, mime in encode_ladder(surface):
            if len(payload) <= BUDGET and (best is None or len(payload) < len(best[0])):
                best = (payload, mime)
        if best is not None:
            return best[0], best[1], surface.width, surface.height
    raise ValueError("no encoding fits the budget")


def main():
    file_before = os.path.getsize(DB)
    con = sqlite3.connect(DB)
    cur = con.cursor()
    rows = cur.execute(
        "SELECT id, name, mime_type, size, width, height, content "
        "FROM attachments WHERE kind = 'image'"
    ).fetchall()
    print(f"image attachments: {len(rows)}")
    total_saved = 0
    changed = 0
    for (aid, name, _mime, _size, _w, _h, content) in rows:
        parsed = parse_data_url(content or "")
        if not parsed:
            print(f"  skip {aid} {name}: not a base64 data url")
            continue
        _mime_in, data = parsed
        try:
            payload, mime_out, out_w, out_h = normalize(data)
        except Exception as error:
            print(f"  skip {aid} {name}: {error}")
            continue
        if len(payload) >= len(data):
            print(f"  keep {aid} {name}: {len(data) // 1024} kb already smaller "
                  f"than any re-encode ({len(payload) // 1024} kb)")
            continue
        data_url = "data:" + mime_out + ";base64," + base64.b64encode(payload).decode("ascii")
        cur.execute(
            "UPDATE attachments SET content = ?, size = ?, mime_type = ?, "
            "width = ?, height = ? WHERE id = ?",
            (data_url, len(payload), mime_out, out_w, out_h, aid),
        )
        saved = len(data) - len(payload)
        total_saved += saved
        changed += 1
        print(f"  {aid} {name}: {len(data) // 1024} kb -> {len(payload) // 1024} kb "
              f"({mime_out}, {out_w}x{out_h}), saved {saved // 1024} kb")
    con.commit()
    cur.execute("VACUUM")
    con.commit()
    con.close()
    file_after = os.path.getsize(DB)
    print(f"\nattachments changed: {changed}/{len(rows)}")
    print(f"encoded bytes saved: {total_saved / 1048576:.2f} MB")
    print(f"db file: {file_before / 1048576:.2f} MB -> {file_after / 1048576:.2f} MB "
          f"({(file_before - file_after) / 1048576:.2f} MB reclaimed)")


if __name__ == "__main__":
    main()
