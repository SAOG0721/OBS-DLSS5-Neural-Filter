"""Offline output validation only; PyAV is isolated under .deps/test-python."""
from pathlib import Path
import json
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '.deps' / 'test-python'))
import av
import numpy as np
from PIL import Image

root = Path(sys.argv[1]).resolve()
width, height = map(int, (root / 'dimensions.txt').read_text().split())

def decode(name):
    container = av.open(str(root / f'{name}.mkv'))
    stream = container.streams.video[0]
    frames = 0
    selected = None
    previous = None
    for frame in container.decode(video=0):
        assert previous is None or frame.pts > previous, 'Non-monotonic video timestamps'
        previous = frame.pts
        if frames == 60:
            selected = frame.to_ndarray(format='rgb24')
        frames += 1
    assert selected is not None and frames >= 250, 'Recording is too short'
    info = {'codec': stream.codec_context.name, 'width': stream.width, 'height': stream.height,
            'rate': str(stream.average_rate), 'decodedFrames': frames, 'timestampsMonotonic': True}
    container.close()
    Image.fromarray(selected).save(root / f'{name}-decoded.png')
    return selected.astype(np.float32), info

def reference(name):
    raw = np.fromfile(root / f'{name}.rgba16f', dtype=np.float16).reshape(height, width, 4).astype(np.float32)
    rgb = np.clip(raw[:, :, :3], 0, 1)
    rgb = np.where(rgb <= .0031308, 12.92 * rgb, 1.055 * np.power(rgb, 1/2.4) - .055) * 255
    return rgb, raw[:, :, 3]

original, original_info = decode('original')
neural, neural_info = decode('neural')
assert original.shape == neural.shape == (height, width, 3), 'Recording dimensions changed'
ref_original, alpha = reference('original')
ref_neural, _ = reference('processed')
visible = alpha > .99
expected_delta = (ref_neural-ref_original)[visible].reshape(-1)
recorded_delta = (neural-original)[visible].reshape(-1)
report = {'original': original_info, 'neural': neural_info,
          'changedPixels': int(np.count_nonzero(np.any(original != neural, axis=2))),
          'originalReferenceMAE': float(np.mean(np.abs(original[visible]-ref_original[visible]))),
          'neuralToOriginalMAE': float(np.mean(np.abs(neural[visible]-ref_original[visible]))),
          'neuralReferenceMAE': float(np.mean(np.abs(neural[visible]-ref_neural[visible]))),
          'deltaCorrelation': float(np.corrcoef(expected_delta, recorded_delta)[0, 1]),
          'scope': '5-second static-pattern OBS/NVENC recordings; not gameplay or long-duration acceptance'}
assert report['changedPixels'] > width*height*.01, 'Recording does not contain a visible output difference'
assert report['deltaCorrelation'] > .5, 'Recorded changes do not agree with the actual GPU NR correction'
assert report['neuralReferenceMAE'] < report['neuralToOriginalMAE'], 'Recording does not match the processed reference better'
(root / 'recording-validation.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
print(json.dumps(report, indent=2))
