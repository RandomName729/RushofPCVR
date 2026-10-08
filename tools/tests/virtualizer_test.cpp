// Checks the head model and the 7.1 virtualizer with signals whose outcome is known:
//   virtualizer_test            prints level and delay between the ears for every speaker
//   virtualizer_test in.wav out.wav   renders an 8-channel 16-bit WAV to binaural stereo
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "core/libraries/audio/surround_virtualizer.h"

using Libraries::AudioOut::SurroundVirtualizer;

static double Rms(const std::vector<float>& v, int offset) {
    double sum = 0;
    size_t n = 0;
    for (size_t i = offset; i < v.size(); i += 2, ++n) sum += double(v[i]) * v[i];
    return std::sqrt(sum / n);
}

/// Lag of the right ear behind the left one, in samples, by cross-correlation.
static int Lag(const std::vector<float>& v) {
    const int frames = int(v.size() / 2);
    double best = -1e30;
    int best_lag = 0;
    for (int lag = -48; lag <= 48; ++lag) {
        double sum = 0;
        for (int i = 64; i < frames - 64; ++i) sum += double(v[i * 2]) * v[(i + lag) * 2 + 1];
        if (sum > best) {
            best = sum;
            best_lag = lag;
        }
    }
    return best_lag;
}

int main(int argc, char** argv) {
    const std::array<int, 8> layout = {0, 1, 2, 3, 4, 5, 6, 7};
    if (argc == 3) {
        std::FILE* in = std::fopen(argv[1], "rb");
        std::FILE* out = std::fopen(argv[2], "wb");
        if (!in || !out) return 1;
        unsigned char header[44];
        if (std::fread(header, 1, 44, in) != 44) return 1;
        const unsigned short two = 2;
        unsigned rate;
        std::memcpy(&rate, header + 24, 4);
        std::memcpy(header + 22, &two, 2);
        const unsigned byte_rate = rate * 4;
        const unsigned short align = 4;
        std::memcpy(header + 28, &byte_rate, 4);
        std::memcpy(header + 32, &align, 2);
        std::fwrite(header, 1, 44, out);
        SurroundVirtualizer virtualizer{layout, rate};
        std::vector<short> block(256 * 8);
        std::vector<float> stereo(256 * 2);
        std::vector<short> pcm(256 * 2);
        unsigned total = 0;
        size_t got;
        while ((got = std::fread(block.data(), 16, 256, in)) > 0) {
            virtualizer.Process(block.data(), unsigned(got), stereo.data());
            for (size_t i = 0; i < got * 2; ++i) {
                float v = stereo[i] < -1 ? -1 : stereo[i] > 1 ? 1 : stereo[i];
                pcm[i] = short(v * 32767);
            }
            std::fwrite(pcm.data(), 4, got, out);
            total += unsigned(got) * 4;
        }
        const unsigned riff = total + 36;
        std::fseek(out, 4, SEEK_SET);
        std::fwrite(&riff, 4, 1, out);
        std::fseek(out, 40, SEEK_SET);
        std::fwrite(&total, 4, 1, out);
        std::fclose(out);
        return 0;
    }

    const char* names[8] = {"front left",    "front right",    "centre",    "LFE",
                            "surround left", "surround right", "rear left", "rear right"};
    std::mt19937 random{1234};
    std::normal_distribution<float> noise{0.0f, 0.1f};
    for (int channel = 0; channel < 8; ++channel) {
        SurroundVirtualizer virtualizer{layout, 48000};
        const unsigned frames = 48000;
        std::vector<float> input(frames * 8, 0.0f), output(frames * 2);
        for (unsigned i = 0; i < frames; ++i) input[i * 8 + channel] = noise(random);
        for (unsigned at = 0; at < frames; at += 256) {
            virtualizer.Process(input.data() + at * 8, std::min(256u, frames - at),
                                output.data() + at * 2);
        }
        const double left = Rms(output, 0), right = Rms(output, 1);
        std::printf("%-14s left %6.1f dB  right %6.1f dB  (L-R %+5.1f dB)  right lags by %+d samples\n",
                    names[channel], 20 * std::log10(left / 0.1), 20 * std::log10(right / 0.1),
                    20 * std::log10(left / right), Lag(output));
    }
    return 0;
}
