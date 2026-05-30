#include <iostream>
#include <cmath>
#include <vector>
#include <algorithm>
#include <SDL3/SDL.h>
#include <string>
#include <map>
#include <sstream>
#include <deque>
#include <random>
#include <chrono>
using namespace std;

const int SAMPLE_RATE = 44100;
const double AMPLITUDE = 12000;

double GLOBAL_TONE = 1.0;   // 0.1 = darker, 1.0 = normal, 5.0 = brighter
double GLOBAL_DELAY = 0.0;  // 0.0 ~ 0.8

struct notes {
    string name;
    double freq;
};

const notes notes_list[] = {
    {"C4",  261.63},{"C#4", 277.18},{"D4",  293.66},{"D#4", 311.13},{"E4",  329.63},{"F4",  349.23},{"F#4", 369.99},
    {"G4",  392.00},{"G#4", 415.30},{"A4",  440.00},{"A#4", 466.16},{"B4",  493.88},{"C5",  523.25},{"C#5", 554.37},
    {"D5",  587.33},{"D#5", 622.25},{"E5",  659.25},{"F5",  698.46},{"F#5", 739.99},{"G5",  783.99},{"G#5", 830.61},
    {"A5",  880.00},{"A#5", 932.33},{"B5",  987.77},
};

enum Instrument {
    GUITAR,
    PIANO,
    RHODES
};

// ── 播放已產生的 buffer ──────────────────────────────────────────────
void playBuffer(const vector<Sint16>& audio_data, int duration_ms) {
    SDL_AudioSpec spec;
    spec.format   = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq     = SAMPLE_RATE;

    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr
    );
    if (!stream) {
        cout << "Audio device error: " << SDL_GetError() << endl;
        return;
    }
    SDL_PutAudioStreamData(stream, audio_data.data(),
                           (int)(audio_data.size() * sizeof(Sint16)));
    SDL_ResumeAudioStreamDevice(stream);
    SDL_Delay(duration_ms);
    SDL_DestroyAudioStream(stream);
}

// ── Tone Control ─────────────────────────────────────────────────────
// GLOBAL_TONE < 1.0 : darker / smoother
// GLOBAL_TONE > 1.0 : brighter / more attack
void applyTone(vector<Sint16>& audio_data) {
    if (audio_data.empty()) return;

    double tone = clamp(GLOBAL_TONE, 0.1, 5.0);
    vector<Sint16> out(audio_data.size());

    double prev = audio_data[0];
    out[0] = audio_data[0];

    if (tone <= 1.0) {
        for (size_t i = 1; i < audio_data.size(); i++) {
            double current = audio_data[i];
            double filtered = tone * current + (1.0 - tone) * prev;
            prev = filtered;
            filtered = clamp(filtered, -32767.0, 32767.0);
            out[i] = (Sint16)filtered;
        }
    } else {
        double edge_gain = (tone - 1.0) * 0.35;
        for (size_t i = 1; i < audio_data.size(); i++) {
            double current = audio_data[i];
            double hp = current - prev;
            double boosted = current + edge_gain * hp;
            prev = current;
            boosted = clamp(boosted, -32767.0, 32767.0);
            out[i] = (Sint16)boosted;
        }
    }

    audio_data.swap(out);
}

// ── Delay Effect ─────────────────────────────────────────────────────
void applyDelay(vector<Sint16>& audio_data) {
    double delay_amt = clamp(GLOBAL_DELAY, 0.0, 0.8);
    if (delay_amt <= 0.0 || audio_data.empty()) return;

    const int delay_ms = 250;
    int delay_samples = SAMPLE_RATE * delay_ms / 1000;

    vector<Sint16> out = audio_data;

    for (size_t i = delay_samples; i < audio_data.size(); i++) {
        double mixed = (double)out[i] + (double)audio_data[i - delay_samples] * delay_amt;
        mixed = clamp(mixed, -32767.0, 32767.0);
        out[i] = (Sint16)mixed;
    }

    audio_data.swap(out);
}

// ════════════════════════════════════════════════════════════════════
// 演算法 1
// ════════════════════════════════════════════════════════════════════

// ── 演算法 1 吉他：Karplus-Strong ───────────────────────────────────
vector<Sint16> synth1_guitar(const vector<double>& freqs, int duration_ms) {
    int num_samples = SAMPLE_RATE * duration_ms / 1000;
    vector<Sint16> audio_data(num_samples, 0);

    mt19937 rng(42);
    uniform_real_distribution<double> dist(-1.0, 1.0);

    vector<deque<double>> buffers(freqs.size());
    for (int j = 0; j < (int)freqs.size(); j++) {
        int buf_len = (int)(SAMPLE_RATE / freqs[j]);
        for (int k = 0; k < buf_len; k++)
            buffers[j].push_back(dist(rng));
    }

    for (int i = 0; i < num_samples; i++) {
        double mixed = 0.0;
        for (int j = 0; j < (int)freqs.size(); j++) {
            double front = buffers[j].front();
            buffers[j].pop_front();
            double next = 0.9995 * 0.5 * (front + buffers[j].front());
            buffers[j].push_back(next);
            mixed += front;
        }
        audio_data[i] = (Sint16)(mixed / freqs.size() * AMPLITUDE);
    }
    return audio_data;
}

// ── 演算法 1 鋼琴：2-op FM Synthesis ────────────────────────────────
vector<Sint16> synth1_piano(const vector<double>& freqs, int duration_ms) {
    int num_samples = SAMPLE_RATE * duration_ms / 1000;
    vector<Sint16> audio_data(num_samples, 0);

    const double mod_ratio = 1.0;
    const double mod_index = 4.0;

    vector<double> carrier_phase(freqs.size(), 0.0);
    vector<double> mod_phase(freqs.size(), 0.0);

    for (int i = 0; i < num_samples; i++) {
        double t = (double)i / num_samples;
        double envelope;
        if (t < 0.01)
            envelope = t / 0.01;
        else
            envelope = exp(-4.0 * (t - 0.01));

        double mixed = 0.0;
        for (int j = 0; j < (int)freqs.size(); j++) {
            double mod_freq = freqs[j] * mod_ratio;
            double mod_out  = mod_index * sin(mod_phase[j]);
            double carrier  = sin(carrier_phase[j] + mod_out);
            mixed += carrier;

            carrier_phase[j] += 2.0 * M_PI * freqs[j] / SAMPLE_RATE;
            mod_phase[j]     += 2.0 * M_PI * mod_freq  / SAMPLE_RATE;
            if (carrier_phase[j] >= 2.0 * M_PI) carrier_phase[j] -= 2.0 * M_PI;
            if (mod_phase[j]     >= 2.0 * M_PI) mod_phase[j]     -= 2.0 * M_PI;
        }

        audio_data[i] = (Sint16)(mixed / freqs.size() * envelope * AMPLITUDE);
    }
    return audio_data;
}

// ── 演算法 1 Rhodes：Additive Synthesis ─────────────────────────────
vector<Sint16> synth1_rhodes(const vector<double>& freqs, int duration_ms) {
    int num_samples = SAMPLE_RATE * duration_ms / 1000;
    vector<Sint16> audio_data(num_samples, 0);

    mt19937 rng(42);
    uniform_real_distribution<double> dist(-1.0, 1.0);

    const int NUM_PARTIALS = 6;
    const double PARTIAL_RATIO[NUM_PARTIALS] = {1.0, 2.01, 3.91, 6.37, 9.4, 14.2};
    const double PARTIAL_AMP[NUM_PARTIALS]   = {1.0, 0.16, 0.22, 0.10, 0.035, 0.015};
    const double PARTIAL_DECAY[NUM_PARTIALS] = {1.0, 2.5, 18.0, 36.0, 58.0, 90.0};

    vector<vector<double>> phases(freqs.size(), vector<double>(NUM_PARTIALS, 0.0));
    vector<double> click_p1(freqs.size()), click_p2(freqs.size());
    for (size_t j = 0; j < freqs.size(); j++) {
        click_p1[j] = dist(rng) * M_PI;
        click_p2[j] = dist(rng) * M_PI;
    }

    const double trem_rate = 5.1, trem_depth = 0.03;

    for (int i = 0; i < num_samples; i++) {
        double t = (double)i / SAMPLE_RATE;
        double envelope = (t < 0.0025) ? t / 0.0025 : exp(-1.08 * (t - 0.0025));
        double trem_mix = 1.0 - exp(-3.0 * t);
        double trem = 1.0 - trem_mix * trem_depth * (0.5 + 0.5 * sin(2.0 * M_PI * trem_rate * t));

        double mixed = 0.0;
        for (size_t j = 0; j < freqs.size(); j++) {
            double note_mix = 0.0;
            for (int n = 0; n < NUM_PARTIALS; n++) {
                double mode_env = PARTIAL_AMP[n] * exp(-PARTIAL_DECAY[n] * t);
                double freq = freqs[j] * PARTIAL_RATIO[n];
                note_mix += mode_env * sin(phases[j][n]);
                phases[j][n] += 2.0 * M_PI * freq / SAMPLE_RATE;
                if (phases[j][n] >= 2.0 * M_PI) phases[j][n] -= 2.0 * M_PI;
            }
            double click = 0.05 * exp(-420.0 * t) *
                           (0.7 * sin(2.0 * M_PI * 3200.0 * t + click_p1[j]) +
                            0.4 * sin(2.0 * M_PI * 5400.0 * t + click_p2[j]));
            double attack = 0.10 * exp(-120.0 * t) *
                            (sin(2.0 * M_PI * 2400.0 * t) + 0.6 * sin(2.0 * M_PI * 4200.0 * t));
            double airy = 0.045 * sin(2.0 * M_PI * (freqs[j] * 1.003) * t) * exp(-2.2 * t);
            mixed += note_mix + click + attack + airy;
        }

        double sample = (mixed / max<size_t>(1, freqs.size())) * envelope * trem * AMPLITUDE * 0.62;
        sample = clamp(sample, -32767.0, 32767.0);
        audio_data[i] = (Sint16)sample;
    }
    return audio_data;
}

// ════════════════════════════════════════════════════════════════════
// 演算法 2
// ════════════════════════════════════════════════════════════════════

// ── 演算法 2 吉他：Extended KS + Fractional Delay + Pick Position ───
vector<Sint16> synth2_guitar(const vector<double>& freqs, int duration_ms) {
    int num_samples = SAMPLE_RATE * duration_ms / 1000;
    vector<Sint16> audio_data(num_samples, 0);

    mt19937 rng(42);
    uniform_real_distribution<double> dist(-1.0, 1.0);

    struct Voice {
        vector<double> buf;
        int idx = 0;
        double frac = 0.0;
        double prev = 0.0;
    };

    const double feedback   = 0.997;
    const double damping    = 0.03;
    const double delay_comp = 1.15;

    vector<Voice> voices;
    voices.reserve(freqs.size());

    for (double f : freqs) {
        double delay = SAMPLE_RATE / f - delay_comp;
        int D = max(2, (int)floor(delay));
        double frac = delay - D;

        int size = D + 2;
        vector<double> noise(size);
        for (int k = 0; k < size; k++) noise[k] = dist(rng);

        int pick = max(1, (int)round(0.18 * size));
        vector<double> buf(size);
        for (int k = 0; k < size; k++)
            buf[k] = 0.5 * (noise[k] - noise[(k + pick) % size]);

        voices.push_back({buf, 0, frac, 0.0});
    }

    for (int i = 0; i < num_samples; i++) {
        double mixed = 0.0;
        for (auto& v : voices) {
            int n = (int)v.buf.size();
            double x0 = v.buf[v.idx];
            double x1 = v.buf[(v.idx + 1) % n];
            double current = (1.0 - v.frac) * x0 + v.frac * x1;
            double next = feedback * ((1.0 - damping) * current + damping * v.prev);
            v.prev = current;
            v.buf[v.idx] = next;
            v.idx = (v.idx + 1) % n;
            mixed += current;
        }
        double sample = (mixed / max<size_t>(1, voices.size())) * AMPLITUDE * 1.5;
        sample = clamp(sample, -32767.0, 32767.0);
        audio_data[i] = (Sint16)sample;
    }
    return audio_data;
}

// ── 演算法 2 鋼琴：Additive Synthesis + Inharmonicity ───────────────
vector<Sint16> synth2_piano(const vector<double>& freqs, int duration_ms) {
    int num_samples = SAMPLE_RATE * duration_ms / 1000;
    vector<Sint16> audio_data(num_samples, 0);

    const vector<double> harmonics_amp = {1.0, 0.5, 0.25, 0.15, 0.08, 0.04, 0.02};
    const double B = 0.0003;
    const int num_harmonics = harmonics_amp.size();

    double total_amp = 0.0;
    for (double a : harmonics_amp) total_amp += a;

    vector<vector<double>> phases(freqs.size(), vector<double>(num_harmonics, 0.0));

    for (int i = 0; i < num_samples; i++) {
        double t = (double)i / num_samples;
        double envelope = (t < 0.005) ? t / 0.005 : exp(-3.5 * (t - 0.005));

        double mixed = 0.0;
        for (int j = 0; j < (int)freqs.size(); j++) {
            double note_mix = 0.0;
            for (int n = 0; n < num_harmonics; n++) {
                int harmonic_n = n + 1;
                double inharmonic_freq = freqs[j] * harmonic_n
                                       * sqrt(1.0 + B * harmonic_n * harmonic_n);
                note_mix += harmonics_amp[n] * sin(phases[j][n]);
                phases[j][n] += 2.0 * M_PI * inharmonic_freq / SAMPLE_RATE;
                if (phases[j][n] >= 2.0 * M_PI) phases[j][n] -= 2.0 * M_PI;
            }
            mixed += note_mix / total_amp;
        }
        audio_data[i] = (Sint16)(mixed / freqs.size() * envelope * AMPLITUDE);
    }
    return audio_data;
}

// ── 演算法 2 Rhodes：Modal Synthesis（音叉物理模態）────────────────
const int NUM_TINE_MODES = 6;
const double TINE_FREQ_RATIO[NUM_TINE_MODES] = {1.0, 6.25, 17.5, 34.4, 57.0, 85.0};
const double TINE_AMP[NUM_TINE_MODES]        = {1.0, 0.25, 0.08, 0.03, 0.01, 0.005};
const double TINE_DECAY[NUM_TINE_MODES]      = {1.2, 8.0, 20.0, 35.0, 50.0, 70.0};

vector<Sint16> synth2_rhodes(const vector<double>& freqs, int duration_ms) {
    int num_samples = SAMPLE_RATE * duration_ms / 1000;
    vector<Sint16> audio_data(num_samples, 0);

    double total_amp = 0.0;
    for (int n = 0; n < NUM_TINE_MODES; n++) total_amp += TINE_AMP[n];

    vector<vector<double>> phases(freqs.size(), vector<double>(NUM_TINE_MODES, 0.0));

    for (int i = 0; i < num_samples; i++) {
        double t = (double)i / SAMPLE_RATE;
        double envelope = (t < 0.003) ? t / 0.003 : exp(-1.2 * (t - 0.003));

        double mixed = 0.0;
        for (int j = 0; j < (int)freqs.size(); j++) {
            double note_mix = 0.0;
            for (int n = 0; n < NUM_TINE_MODES; n++) {
                double mode_env  = TINE_AMP[n] * exp(-TINE_DECAY[n] * t);
                double mode_freq = freqs[j] * TINE_FREQ_RATIO[n];
                note_mix += mode_env * sin(phases[j][n]);
                phases[j][n] += 2.0 * M_PI * mode_freq / SAMPLE_RATE;
                if (phases[j][n] >= 2.0 * M_PI) phases[j][n] -= 2.0 * M_PI;
            }
            mixed += note_mix / total_amp;
        }
        audio_data[i] = (Sint16)(mixed / freqs.size() * envelope * AMPLITUDE);
    }
    return audio_data;
}

// ════════════════════════════════════════════════════════════════════
// 聲音合成入口
// ════════════════════════════════════════════════════════════════════
vector<Sint16> synthesize(
    const vector<double>& freqs,
    Instrument inst,
    int synth_choice,
    int duration_ms
) {
    vector<Sint16> result;

    if (inst == GUITAR)
        result = (synth_choice == 1) ? synth1_guitar(freqs, duration_ms)
                                     : synth2_guitar(freqs, duration_ms);
    else if (inst == PIANO)
        result = (synth_choice == 1) ? synth1_piano(freqs, duration_ms)
                                     : synth2_piano(freqs, duration_ms);
    else
        result = (synth_choice == 1) ? synth1_rhodes(freqs, duration_ms)
                                    : synth2_rhodes(freqs, duration_ms);

    applyTone(result);
    applyDelay(result);
    return result;
}

// ════════════════════════════════════════════════════════════════════
// Chord 解析
// ════════════════════════════════════════════════════════════════════
map<string, vector<int>> chordIntervals = {
    {"b2",{1}},{"2",{2}},{"b3",{3}},{"3",{4}},{"4",{5}},{"b5",{6}},{"5",{7}},{"b6",{8}},{"6",{9}},{"b7",{10}},{"7",{11}},
    {"b9",{13}},{"9",{14}},{"#9",{15}},{"11",{17}},{"#11",{18}},{"b13",{20}},{"13",{21}},{"#13",{22}},
    {"M",{0,4,7}},{"m",{0,3,7}},{"dim",{0,3,6}},{"aug",{0,4,8}},
    {"M7",{0,4,7,11}},{"7",{0,4,7,10}},{"m7",{0,3,7,10}},{"dim7",{0,3,6,9}},
    {"M9",{0,4,7,11,14}},{"9",{0,4,7,10,14}},{"m9",{0,3,7,10,14}},{"dim9",{0,3,6,9,14}},
    {"M11",{0,4,7,11,14,17}},{"11",{0,4,7,10,14,17}},{"m11",{0,3,7,10,14,17}},{"dim11",{0,3,6,9,14,17}},
    {"M13",{0,4,7,11,14,17,21}},{"13",{0,4,7,10,14,17,21}},{"m13",{0,3,7,10,14,17,21}},{"dim13",{0,3,6,9,14,17,21}}
};

void chordnote(vector<double>& freqs, const string& chord_name) {
    vector<string> parts, add, omit;
    stringstream ss(chord_name);
    string part;
    while (getline(ss, part, '_')) parts.push_back(part);

    if (parts.size() < 2) return;

    int bass = 0;
    for (int i = 0; i < 12; i++) {
        if (notes_list[i].name == parts[0]) { bass = i; break; }
    }

    auto nNotes = (int)(sizeof(notes_list) / sizeof(notes_list[0]));

    for (int interval : chordIntervals[parts[1]]) {
        int idx = bass + interval;
        if (0 <= idx && idx < nNotes)
            freqs.push_back(notes_list[idx].freq);
    }

    if (parts.size() >= 3) {
        stringstream ssadd(parts[2]);
        while (getline(ssadd, part, ',')) add.push_back(part);
        for (auto& a : add) {
            int idx = bass + chordIntervals[a][0];
            if (0 <= idx && idx < nNotes)
                freqs.push_back(notes_list[idx].freq);
        }

        if (parts.size() == 4) {
            stringstream ssomit(parts[3]);
            while (getline(ssomit, part, ',')) omit.push_back(part);
            for (auto& o : omit) {
                int idx = bass + chordIntervals[o][0];
                if (0 <= idx && idx < nNotes) {
                    double target = notes_list[idx].freq;
                    freqs.erase(remove(freqs.begin(), freqs.end(), target), freqs.end());
                }
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════════
// 循環播放
// ════════════════════════════════════════════════════════════════════
void playLoop(const vector<string>& chord_names, Instrument inst,
              int synth_choice, int bpm, int bars) {

    int bar_ms = (int)(60000.0 / bpm * 4.0);

    cout << "Playing " << bars << " bars..." << endl;

    for (int bar = 0; bar < bars; bar++) {
        int chord_idx = bar % (int)chord_names.size();
        const string& name = chord_names[chord_idx];

        vector<double> freqs;
        chordnote(freqs, name);

        cout << "Bar " << bar + 1 << ": " << name << endl;

        vector<Sint16> buf = synthesize(freqs, inst, synth_choice, bar_ms);
        playBuffer(buf, bar_ms);
    }

    cout << "Done" << endl;
}

// ════════════════════════════════════════════════════════════════════
// Main
// ════════════════════════════════════════════════════════════════════
int main() {

    if (!SDL_Init(SDL_INIT_AUDIO)) {
        cout << "SDL 初始化失敗: "
             << SDL_GetError()
             << endl;
        return 1;
    }

    int play_or_test;

    cout << "0: Benchmark  1: Play Loop" << endl;
    cin >> play_or_test;

    if (play_or_test == 0) {

        int how_many_chords;
        cout << "Number of chords: ";
        cin >> how_many_chords;

        vector<string> chord_names;
        for (int i = 0; i < how_many_chords; i++) {
            string chord_name;
            cout << "Chord(ex:C4_M7_11_5) " << i + 1 << ": ";
            cin >> chord_name;
            chord_names.push_back(chord_name);
        }

        int inst_choice;
        cout << "Instrument (0: Guitar, 1: Piano, 2: Rhodes): ";
        cin >> inst_choice;

        int bpm;
        cout << "BPM: ";
        cin >> bpm;

        int bars;
        cout << "Bars: ";
        cin >> bars;

        cout << "Tone (0.1 ~ 5.0): ";
        cin >> GLOBAL_TONE;

        cout << "Delay (0.0 ~ 0.8): ";
        cin >> GLOBAL_DELAY;

        Instrument inst =
            (inst_choice == 0) ? GUITAR :
            (inst_choice == 1) ? PIANO  : RHODES;

        int bar_ms = (int)(60000.0 / bpm * 4.0);

        auto benchmark = [&](int synth_choice) {
            using clock = std::chrono::high_resolution_clock;
            const int TEST_TIMES = 3;

            double total_ms = 0.0;
            long long checksum = 0;
            size_t peak_memory = 0;

            for (int test = 0; test < TEST_TIMES; test++) {
                auto start = clock::now();

                for (int bar = 0; bar < bars; bar++) {
                    const string& name = chord_names[bar % chord_names.size()];

                    vector<double> freqs;
                    chordnote(freqs, name);

                    vector<Sint16> buf = synthesize(freqs, inst, synth_choice, bar_ms);

                    for (Sint16 s : buf)
                        checksum += s;

                    size_t memory_used = 0;
                    memory_used += buf.size() * sizeof(Sint16);
                    memory_used += freqs.size() * sizeof(double);

                    if (inst == GUITAR) {
                        for (double f : freqs) {
                            memory_used += (size_t)(SAMPLE_RATE / f) * sizeof(double);
                        }
                    }
                    else if (inst == PIANO) {
                        int harmonics = (synth_choice == 1) ? 2 : 7;
                        memory_used += freqs.size() * harmonics * sizeof(double);
                    }
                    else {
                        int modes = 6;
                        memory_used += freqs.size() * modes * sizeof(double);
                    }

                    peak_memory = max(peak_memory, memory_used);
                }

                auto end = clock::now();
                double ms = std::chrono::duration<double, std::milli>(end - start).count();
                total_ms += ms;

                cout << "Run " << test + 1 << ": " << ms << " ms" << endl;
            }

            cout << endl;
            cout << "=== Synth " << synth_choice << " Result ===" << endl;
            cout << "Average time: " << total_ms / TEST_TIMES << " ms" << endl;
            cout << "Average per bar: " << total_ms / TEST_TIMES / bars << " ms" << endl;
            cout << "Estimated peak memory: " << peak_memory / 1024.0 << " KB" << endl;
            cout << "Checksum: " << checksum << endl << endl;
        };

        cout << endl;
        cout << "===== Benchmark Start =====" << endl << endl;
        benchmark(1);
        benchmark(2);
        cout << "===== Benchmark End =====" << endl;
    }

    else if (play_or_test == 1) {

        int how_many_chords;
        cout << "Number of chords: ";
        cin >> how_many_chords;

        vector<string> chord_names;
        for (int i = 0; i < how_many_chords; i++) {
            string chord_name;
            cout << "Chord(ex:C4_M7_11_5) " << i + 1 << ": ";
            cin >> chord_name;
            chord_names.push_back(chord_name);
        }

        int inst_choice;
        cout << "Instrument (0: Guitar, 1: Piano, 2: Rhodes): ";
        cin >> inst_choice;

        int bpm;
        cout << "BPM: ";
        cin >> bpm;

        int bars;
        cout << "Bars: ";
        cin >> bars;

        int synth_choice;
        cout << "Synth algorithm (1 or 2): ";
        cin >> synth_choice;

        cout << "Tone (0.1 ~ 5.0): ";
        cin >> GLOBAL_TONE;

        cout << "Delay (0.0 ~ 0.8): ";
        cin >> GLOBAL_DELAY;

        Instrument inst =
            (inst_choice == 0) ? GUITAR :
            (inst_choice == 1) ? PIANO  : RHODES;

        playLoop(
            chord_names,
            inst,
            synth_choice,
            bpm,
            bars
        );
    }

    SDL_Quit();
    return 0;
}
