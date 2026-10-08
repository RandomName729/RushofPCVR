"""Looks at a WAV file without a speaker: loudness over time, clipping, and a spectrogram picture.

  python wavstat.py <file.wav> [--png out.png] [--from s] [--to s] [--step s]
"""
import sys
import wave

import numpy as np


def load(path):
    with wave.open(path, "rb") as handle:
        channels = handle.getnchannels()
        rate = handle.getframerate()
        width = handle.getsampwidth()
        frames = handle.getnframes()
        raw = handle.readframes(frames)
    if width != 2:
        raise SystemExit(f"{path}: only 16-bit files are handled")
    data = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    return data.reshape(-1, channels), rate


def db(value):
    return 20.0 * np.log10(max(value, 1e-9))


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    path = args[0]
    option = lambda name, default=None: args[args.index(name) + 1] if name in args else default
    data, rate = load(path)
    start = float(option("--from", 0))
    end = float(option("--to", len(data) / rate))
    step = float(option("--step", max((end - start) / 24.0, 0.25)))
    data = data[int(start * rate):int(end * rate)]
    channels = data.shape[1]
    seconds = len(data) / rate
    print(f"{path}: {channels} ch, {rate} Hz, {seconds:.1f} s")
    if len(data) == 0:
        return

    peak = np.abs(data).max(axis=0)
    rms = np.sqrt((data ** 2).mean(axis=0))
    clipped = (np.abs(data) >= 0.999).sum(axis=0)
    for c in range(channels):
        print(f"  ch{c}: peak {db(peak[c]):6.1f} dB  rms {db(rms[c]):6.1f} dB  clipped {clipped[c]}")

    mono = data[:, :2].mean(axis=1) if channels >= 2 else data[:, 0]
    print("  loudness over time (dB rms of ch0+ch1, one column per %.2f s):" % step)
    window = int(step * rate)
    row = []
    for at in range(0, len(mono) - window + 1, window):
        chunk = mono[at:at + window]
        row.append(db(float(np.sqrt((chunk ** 2).mean()))))
    for at in range(0, len(row), 12):
        print("   %6.1fs  " % (start + at * step) + " ".join("%6.1f" % v for v in row[at:at + 12]))

    # Where the energy sits: a rough spectrum of the whole excerpt.
    size = 4096
    if len(mono) >= size:
        frames = [mono[i:i + size] * np.hanning(size) for i in range(0, len(mono) - size, size // 2)]
        spectrum = np.abs(np.fft.rfft(np.array(frames), axis=1)) ** 2
        mean = spectrum.mean(axis=0)
        freqs = np.fft.rfftfreq(size, 1.0 / rate)
        bands = [(20, 100), (100, 300), (300, 1000), (1000, 3000), (3000, 8000), (8000, 20000)]
        total = mean.sum() + 1e-20
        print("  energy by band: " + "  ".join(
            "%d-%dHz %4.1f%%" % (lo, hi, 100.0 * mean[(freqs >= lo) & (freqs < hi)].sum() / total)
            for lo, hi in bands))

    png = option("--png")
    if png:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        figure, axes = plt.subplots(2, 1, figsize=(14, 7), gridspec_kw={"height_ratios": [1, 3]})
        times = np.arange(len(mono)) / rate + start
        stride = max(len(mono) // 20000, 1)
        axes[0].plot(times[::stride], mono[::stride], linewidth=0.4)
        axes[0].set_xlim(times[0], times[-1])
        axes[0].set_ylim(-1, 1)
        axes[1].specgram(mono, NFFT=2048, Fs=rate, noverlap=1024, xextent=(times[0], times[-1]),
                         vmin=-120, vmax=-30, cmap="magma")
        axes[1].set_ylim(0, 12000)
        axes[1].set_xlabel("seconds")
        figure.tight_layout()
        figure.savefig(png, dpi=80)
        print(f"  wrote {png}")


if __name__ == "__main__":
    main()
