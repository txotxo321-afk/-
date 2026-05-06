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
using namespace std;

const int SAMPLE_RATE = 44100;
const double AMPLITUDE = 12000;

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
    ELECTRIC_GUITAR,
    CLASSICAL_GUITAR,
    PIANO,
    ELECTRIC_PIANO
};

struct Harmonics {
    vector<double> ratios;
    vector<int> multipliers;
    double total_ratio;

    Harmonics(vector<double> r, vector<int> m)
        : ratios(r), multipliers(m), total_ratio(0.0) {
        for (double x : ratios) total_ratio += x;
    }
};

const Harmonics HARMONICS_TABLE[] = {
    // ELECTRIC_GUITAR
    {{1.0, 0.6, 0.8, 0.3, 0.5, 0.2}, {1, 2, 3, 4, 5, 6}},
    // CLASSICAL_GUITAR
    {{1.0, 0.4, 0.2, 0.08, 0.03},    {1, 2, 3, 4, 5}},
    // PIANO
    {{1.0, 0.4, 0.3, 0.2, 0.15, 0.1, 0.05}, {1, 2, 3, 4, 5, 6, 7}},
    // ELECTRIC_PIANO
    {{1.0, 0.3, 0.15, 0.25, 0.1, 0.05},     {1, 2, 3, 4, 6, 8}},
};

double getInstrumentValue(Instrument inst, double base_phase) {
    const Harmonics& h = HARMONICS_TABLE[inst];
    double total = 0.0;
    for (int i = 0; i < (int)h.multipliers.size(); i++)
        total += h.ratios[i] * sin(base_phase * h.multipliers[i]);
    return total / h.total_ratio;
}

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
        cout << "開啟音效裝置失敗: " << SDL_GetError() << endl;
        return;
    }
    SDL_PutAudioStreamData(stream, audio_data.data(),
                           (int)(audio_data.size() * sizeof(Sint16)));
    SDL_ResumeAudioStreamDevice(stream);
    SDL_Delay(duration_ms);
    SDL_DestroyAudioStream(stream);
}

// ── 演算法 1：Additive Synthesis ────────────────────────────────────
// 原理：把基頻和各泛音的正弦波依比例疊加，用指數衰減模擬音量消失
// 資料結構：每個頻率維護一個 phase（double array）
// 特性：音色由係數直接決定，可控性高但比較「人工」
vector<Sint16> additive_chord(const vector<double>& freqs, Instrument inst, int duration_ms) {
    int num_samples = SAMPLE_RATE * duration_ms / 1000;
    vector<Sint16> audio_data(num_samples);
    vector<double> phases(freqs.size(), 0.0);

    for (int i = 0; i < num_samples; i++) {
        // 指數衰減：模擬撥弦後音量漸漸消失
        double envelope = exp(-3.0 * i / num_samples);
        double mixed = 0.0;

        for (int j = 0; j < (int)freqs.size(); j++) {
            mixed += getInstrumentValue(inst, phases[j]);
            phases[j] += 2.0 * M_PI * freqs[j] / SAMPLE_RATE;
            if (phases[j] >= 2.0 * M_PI) phases[j] -= 2.0 * M_PI;
        }

        audio_data[i] = (Sint16)(mixed / freqs.size() * envelope * AMPLITUDE);
    }
    return audio_data;
}

// ── 演算法 2：Karplus-Strong ─────────────────────────────────────────
// 資料結構：每個頻率一條 deque 當 circular buffer
// 特性：物理模擬，音色自然，衰減不需要額外 envelope
vector<Sint16> karplus_strong_chord(const vector<double>& freqs, int duration_ms) {
    int num_samples = SAMPLE_RATE * duration_ms / 1000;
    vector<Sint16> audio_data(num_samples, 0);

    mt19937 rng(42);
    uniform_real_distribution<double> dist(-1.0, 1.0);

    // 每個頻率初始化自己的 delay buffer，長度決定音高
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
            // 低通濾波 + 衰減：0.996 越接近 1 聲音越長
            double next = 0.996 * 0.5 * (front + buffers[j].front());
            buffers[j].push_back(next);
            mixed += front;
        }
        audio_data[i] = (Sint16)(mixed / freqs.size() * AMPLITUDE);
    }
    return audio_data;
}

map<string, vector<int>> chordIntervals = {
    {"b2",{1}},{"2",{2}},{"b3",{3}},{"3",{4}},{"4",{5}},{"b5",{6}},{"5",{7}},{"b6",{8}},{"6",{9}},{"b7",{10}},{"7",{11}},
    {"b9",{13}},{"9",{14}},{"#9",{15}},{"11",{17}},{"#11",{18}},{"b13",{20}},{"13",{21}},{"#13",{22}},
    {"M",{0,4,7}},{"m",{0,3,7}},{"dim",{0,3,6}},{"aug",{0,4,8}},
    {"M7",{0,4,7,11}},{"7",{0,4,7,10}},{"m7",{0,3,7,10}},{"dim7",{0,3,6,9}},
    {"M9",{0,4,7,11,14}},{"9",{0,4,7,10,14}},{"m9",{0,3,7,10,14}},{"dim9",{0,3,6,9,14}},
    {"M11",{0,4,7,11,14,17}},{"11",{0,4,7,10,14,17}},{"m11",{0,3,7,10,14,17}},{"dim11",{0,3,6,9,14,17}},
    {"M13",{0,4,7,11,14,17,21}},{"13",{0,4,7,10,14,17,21}},{"m13",{0,3,7,10,14,17,21}},{"dim13",{0,3,6,9,14,17,21}}
};
//我預期的代號輸入是C4_M_9,11_5
void chordnote(vector<double>& freqs, string chord_name) {
    vector<string> parts, add, omit;
    stringstream ss(chord_name);
    string part;
    while (getline(ss, part, '_')) parts.push_back(part);

    int bass = 0;
    for (int i = 0; i < 12; i++) {
        if (notes_list[i].name == parts[0]) { bass = i; break; }
    }

    for (int interval : chordIntervals[parts[1]])
        freqs.push_back(notes_list[bass + interval].freq);

    if (parts.size() >= 3) {
        stringstream ssadd(parts[2]);
        while (getline(ssadd, part, ',')) add.push_back(part);
        for (auto& a : add)
            freqs.push_back(notes_list[bass + chordIntervals[a][0]].freq);

        if (parts.size() == 4) {
            stringstream ssomit(parts[3]);
            while (getline(ssomit, part, ',')) omit.push_back(part);
            for (auto& o : omit) {
                double target = notes_list[bass + chordIntervals[o][0]].freq;
                freqs.erase(remove(freqs.begin(), freqs.end(), target), freqs.end());
            }
        }
    }
}

int main() {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        cout << "SDL 初始化失敗: " << SDL_GetError() << endl;
        return 1;
    }

    string name;
    cin >> name;
    vector<double> freqs;
    chordnote(freqs, name);
    for (double f : freqs) cout << f << endl;
    auto buf_add = additive_chord(freqs, CLASSICAL_GUITAR, 2000);
    playBuffer(buf_add, 2000);

    SDL_Delay(500);

    auto buf_ks = karplus_strong_chord(freqs, 2000);
    playBuffer(buf_ks, 2000);

    SDL_Quit();
    return 0;
}
