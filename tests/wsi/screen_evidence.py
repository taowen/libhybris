"""Verify the fixed window in two real screenshots, honoring embedded color profiles."""
import hashlib
import io
from pathlib import Path
import PIL
from PIL import Image, ImageCms

def verify_screen(directory):
    directory = Path(directory)
    colors = ((0, 255, 0), (255, 0, 0))
    pictures, expected, records = [], [], []
    for frame, color in zip((0, 7), colors):
        path = directory / f'screen-{frame}.png'
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
        raw = (directory / f'image-{frame}.rgba').read_bytes()
        if raw != bytes((*color, 255)) * (320 * 240):
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
    if len(points) != 320 * 240 or bounds[2] - bounds[0] != 320 or bounds[3] - bounds[1] != 240:
        raise ValueError(f'expected one complete 320x240 window; got {len(points)} pixels at {bounds}')
    return {'status': 'PASS', 'checker_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
            'pillow': PIL.__version__, 'littlecms': ImageCms.core.littlecms_version, 'screen_size': [width, height], 'window_bounds': bounds,
            'compared_pixels': len(points), 'frames': records}
