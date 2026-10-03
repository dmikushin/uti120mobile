"""False-colour palettes as 256-entry RGB lookup tables."""

import numpy as np

_STOPS = {
    "grey": [(0.0, (0, 0, 0)), (1.0, (255, 255, 255))],
    "ironbow": [(0.0, (0, 0, 0)), (0.15, (32, 0, 96)), (0.35, (128, 0, 160)),
                (0.55, (220, 40, 60)), (0.75, (250, 140, 0)), (0.9, (255, 220, 40)),
                (1.0, (255, 255, 230))],
    "rainbow": [(0.0, (0, 0, 128)), (0.2, (0, 0, 255)), (0.4, (0, 255, 255)),
                (0.6, (255, 255, 0)), (0.8, (255, 0, 0)), (1.0, (128, 0, 0))],
}

NAMES = tuple(_STOPS)


def lut(name: str) -> np.ndarray:
    stops = _STOPS[name]
    x = np.linspace(0.0, 1.0, 256)
    pos = [s[0] for s in stops]
    return np.stack([np.interp(x, pos, [s[1][c] for s in stops]) for c in range(3)],
                    axis=1).round().astype(np.uint8)


def colorize(norm: np.ndarray, name: str) -> np.ndarray:
    """norm: float array in [0, 1] -> uint8 RGB image."""
    return lut(name)[(norm * 255).round().astype(np.uint8)]
