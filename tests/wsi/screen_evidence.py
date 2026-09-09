"""Verify three window sizes in real screenshots, honoring embedded color profiles."""
import hashlib
import io
from pathlib import Path
import PIL
from PIL import Image, ImageCms

def verify_epoch(directory, epoch, extent, first_alpha=255):
    directory = Path(directory)
    colors = ((0, 255, 0), (255, 0, 0))
    pictures, expected, records = [], [], []
    for frame, color in zip((0, 7), colors):
        path = directory / f'screen-{epoch}-{frame}.png'
        picture = Image.open(path)
        icc = picture.info.get('icc_profile')
        if icc:
            profile = ImageCms.ImageCmsProfile(io.BytesIO(icc))
            encoded = ImageCms.profileToProfile(Image.new('RGB', (1, 1), color),
                ImageCms.createProfile('sRGB'), profile, outputMode='RGB').getpixel((0, 0))
            name = ImageCms.getProfileName(profile).strip()
        elif 'srgb' in picture.info:
            encoded, name = color, 'PNG sRGB'
        else:
            raise ValueError('screenshot has no supported embedded color profile')
        pictures.append(picture.convert('RGB'))
        expected.append(encoded)
        raw = (directory / f'image-{epoch}-{frame}.rgba').read_bytes()
        if raw != bytes((*color, first_alpha if frame == 0 else 255)) * (extent[0] * extent[1]):
            raise ValueError(f'frame {frame}: raw image is not the expected complete RGBA pattern')
        records.append({'frame': frame, 'screenshot_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'image_sha256': hashlib.sha256(raw).hexdigest(), 'color_profile': name,
            'icc_sha256': hashlib.sha256(icc).hexdigest() if icc else None,
            'expected_screenshot_rgb': encoded})
    if pictures[0].size != pictures[1].size:
        raise ValueError('screenshot dimensions changed')
    width, height = pictures[0].size
    points = [(i % width, i // width) for i, (before, after) in
              enumerate(zip(pictures[0].getdata(), pictures[1].getdata()))
              if before == expected[0] and after == expected[1]]
    if not points: raise ValueError('no expected green-to-red window transition')
    bounds = [min(x for x, _ in points), min(y for _, y in points),
              max(x for x, _ in points) + 1, max(y for _, y in points) + 1]
    if len(points) != extent[0] * extent[1] or bounds[2] - bounds[0] != extent[0] or bounds[3] - bounds[1] != extent[1]:
        raise ValueError(f'epoch {epoch}: expected one complete {extent} window; got {len(points)} pixels at {bounds}')
    return {'epoch': epoch, 'extent': extent, 'screen_size': [width, height],
            'window_bounds': bounds, 'compared_pixels': len(points), 'frames': records}

def verify_screen(directory):
    # Fixed independent expectations: do not accept the dimensions the probe
    # reports as proof that it actually resized the displayed window.
    epochs = [verify_epoch(directory, epoch, extent, first_alpha=0) for epoch, extent in
              enumerate(((320, 240), (448, 288), (256, 192)))]
    return {'status': 'PASS', 'checker_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'pillow': PIL.__version__, 'littlecms': ImageCms.core.littlecms_version,
            'epochs': epochs, 'compared_pixels': sum(item['compared_pixels'] for item in epochs)}
