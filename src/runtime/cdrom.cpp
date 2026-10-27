// CD-ROM controller (register level, as libcd drives it) reading the disc image in the game
// folder: commands and interrupts, sector reads at 1x/2x speed, the data FIFO / DMA3,
// XA-ADPCM decoding of streamed audio and CD-DA playback of the audio track.
#include "hw.h"
#include "log.h"
#include "config.h"
#include "discpack.h"
#include <libchdr/chd.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

// ---------------------------------------------------------------- disc image
// The disc is read straight from the user's .cue/.bin rip (one .bin per track, or a single
// .bin with both tracks). Older "game" folders made by prepare_disc.py (track01.bin,
// track02.bin, disc.txt) still work.
static FILE* g_t1 = nullptr;
static FILE* g_t2 = nullptr;
static long g_t1_off = 0;         // file offset (sectors) of track 1 INDEX 01
static long g_t2_off = 0;         // file offset (sectors) of track 2 INDEX 01
static int g_t1_sectors = 0;      // sectors in track 1
static int g_t2_start = 0;        // disc LBA of track 2 INDEX 01 (LBA 0 = 00:02:00)
static int g_t2_sectors = 0;

static int bcd2i(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
static uint8_t i2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int msf_to_lba(int m, int s, int f) { return (m * 60 + s) * 75 + f - 150; }
static void lba_to_msf(int lba, int* m, int* s, int* f) {
    lba += 150;
    *m = lba / 4500; *s = (lba / 75) % 60; *f = lba % 75;
}

static bool g_packed = false;     // reading from assets.dat instead of the .bin files

// .chd disc images (MAME CHD v5, as made by chdman createcd): frames of 2352 bytes + 96 bytes
// subcode, 8 frames per hunk, every track padded to a multiple of 4 frames; audio is big-endian.
struct ChdTrack { bool audio; int lba, frames, pregap; bool pregap_stored; int chd_frame; };
static const int CD_FRAME_SIZE_ = 2448;
static chd_file* g_chd = nullptr;
static std::vector<ChdTrack> g_chd_tracks;
static std::vector<uint8_t> g_chd_hunk;
static int g_chd_hunk_no = -1, g_chd_frames_per_hunk = 8;

static bool chd_sector(int lba, uint8_t* out, bool* audio) {
    for (auto& t : g_chd_tracks) {
        int first = t.lba - t.pregap;
        if (lba < first || lba >= t.lba + t.frames) continue;
        *audio = t.audio;
        int f;
        if (lba < t.lba) {
            if (!t.pregap_stored) { memset(out, 0, 2352); return true; }
            f = t.chd_frame + (lba - first);
        } else {
            f = t.chd_frame + (t.pregap_stored ? t.pregap : 0) + (lba - t.lba);
        }
        int h = f / g_chd_frames_per_hunk;
        if (h != g_chd_hunk_no) {
            if (chd_read(g_chd, (uint32_t)h, g_chd_hunk.data()) != CHDERR_NONE) { g_chd_hunk_no = -1; return false; }
            g_chd_hunk_no = h;
        }
        const uint8_t* src = &g_chd_hunk[(size_t)(f % g_chd_frames_per_hunk) * CD_FRAME_SIZE_];
        if (t.audio) for (int i = 0; i < 2352; i += 2) { out[i] = src[i + 1]; out[i + 1] = src[i]; }
        else memcpy(out, src, 2352);
        return true;
    }
    *audio = lba >= g_t1_sectors;
    memset(out, 0, 2352);
    return lba >= 0 && lba < g_t2_start + g_t2_sectors;
}

static bool open_chd(const std::string& path) {
    if (chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &g_chd) != CHDERR_NONE) return false;
    const chd_header* h = chd_get_header(g_chd);
    g_chd_frames_per_hunk = (int)(h->hunkbytes / CD_FRAME_SIZE_);
    g_chd_hunk.resize(h->hunkbytes);
    int chd_frame = 0, lba = 0;
    for (uint32_t i = 0;; i++) {
        char meta[256] = {};
        uint32_t len = 0, tag = 0;
        uint8_t flags = 0;
        if (chd_get_metadata(g_chd, CDROM_TRACK_METADATA2_TAG, i, meta, sizeof meta - 1, &len, &tag, &flags) != CHDERR_NONE) break;
        int num = 0, frames = 0, pregap = 0, postgap = 0;
        char type[32] = {}, sub[32] = {}, pgtype[32] = {}, pgsub[32] = {};
        if (sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &num, type, sub, &frames, &pregap, pgtype, pgsub, &postgap) < 5) break;
        ChdTrack t;
        t.audio = strcmp(type, "AUDIO") == 0;
        t.pregap_stored = pgtype[0] == 'V';
        t.pregap = pregap;
        t.frames = frames - (t.pregap_stored ? pregap : 0);
        t.chd_frame = chd_frame;
        if (i == 0) t.pregap = 0;          // track 1's pregap is the lead-in before LBA 0
        lba += t.pregap;
        t.lba = lba;
        lba += t.frames;
        chd_frame += (frames + 3) / 4 * 4;
        g_chd_tracks.push_back(t);
    }
    if (g_chd_tracks.empty() || g_chd_tracks[0].audio) { chd_close(g_chd); g_chd = nullptr; return false; }
    g_t1_sectors = g_chd_tracks[0].frames;
    if (g_chd_tracks.size() > 1) {
        g_t2_start = g_chd_tracks[1].lba;
        g_t2_sectors = g_chd_tracks.back().lba + g_chd_tracks.back().frames - g_t2_start;
    } else {
        g_t2_start = g_t1_sectors + 150;
        g_t2_sectors = 0;
    }
    return true;
}

// Reads the raw 2352-byte sector at disc LBA; returns false past the end.
static bool read_raw(int lba, uint8_t* out, bool* audio) {
    if (g_packed) {
        *audio = lba >= g_t1_sectors;
        if (lba < 0 || lba >= g_t2_start + g_t2_sectors) { memset(out, 0, 2352); return false; }
        return pack_read(lba, out);
    }
    if (g_chd) return chd_sector(lba, out, audio);
    if (lba >= 0 && lba < g_t1_sectors) {
        *audio = false;
        fseek(g_t1, (g_t1_off + lba) * 2352L, SEEK_SET);
        return fread(out, 1, 2352, g_t1) == 2352;
    }
    *audio = true;
    if (g_t2 && lba >= g_t2_start && lba < g_t2_start + g_t2_sectors) {
        fseek(g_t2, (g_t2_off + (lba - g_t2_start)) * 2352L, SEEK_SET);
        return fread(out, 1, 2352, g_t2) == 2352;
    }
    memset(out, 0, 2352);   // pregap
    return lba >= 0 && lba < g_t2_start + g_t2_sectors;
}

static long file_sectors(FILE* f) { fseek(f, 0, SEEK_END); return ftell(f) / 2352; }

static bool open_cue(const std::string& cue) {
    FILE* f = fopen(cue.c_str(), "r");
    if (!f) return false;
    std::string dir = cue.substr(0, cue.find_last_of("/\\") + 1);
    struct Track { std::string file; int idx0 = -1, idx1 = -1; bool audio = false; };
    std::vector<Track> tracks;
    std::string cur;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string l = line;
        size_t q1 = l.find('"'), q2 = l.rfind('"');
        if (l.find("FILE") != std::string::npos && q1 != std::string::npos && q2 > q1) cur = dir + l.substr(q1 + 1, q2 - q1 - 1);
        else if (l.find("TRACK") != std::string::npos) { Track t; t.file = cur; t.audio = l.find("AUDIO") != std::string::npos; tracks.push_back(t); }
        else if (l.find("INDEX") != std::string::npos && !tracks.empty()) {
            int n, m, s, fr;
            if (sscanf(strstr(line, "INDEX") + 5, "%d %d:%d:%d", &n, &m, &s, &fr) == 4) {
                int v = (m * 60 + s) * 75 + fr;
                if (n == 0) tracks.back().idx0 = v; else if (n == 1) tracks.back().idx1 = v;
            }
        }
    }
    fclose(f);
    if (tracks.empty() || tracks[0].audio) return false;
    g_t1 = fopen(tracks[0].file.c_str(), "rb");
    if (!g_t1) { fprintf(stderr, "[cd] cannot open %s\n", tracks[0].file.c_str()); return false; }
    g_t1_off = std::max(0, tracks[0].idx1);
    if (tracks.size() >= 2) {
        const Track& t = tracks[1];
        if (t.file == tracks[0].file) {
            // single .bin: track 1 ends where track 2's pregap (INDEX 00) begins
            g_t1_sectors = (t.idx0 >= 0 ? t.idx0 : t.idx1) - (int)g_t1_off;
            g_t2 = fopen(t.file.c_str(), "rb");
            g_t2_off = t.idx1;
            g_t2_start = t.idx1 - (int)g_t1_off;
            g_t2_sectors = (int)(file_sectors(g_t2) - g_t2_off);
        } else {
            g_t1_sectors = (int)(file_sectors(g_t1) - g_t1_off);
            g_t2 = fopen(t.file.c_str(), "rb");
            if (g_t2) {
                g_t2_off = t.idx1;
                g_t2_start = g_t1_sectors + t.idx1;
                g_t2_sectors = (int)(file_sectors(g_t2) - g_t2_off);
            }
        }
    } else {
        g_t1_sectors = (int)(file_sectors(g_t1) - g_t1_off);
        g_t2_start = g_t1_sectors + 150;
    }
    fprintf(stderr, "[cd] %s\n", cue.c_str());
    return true;
}

// ISO 9660 file lookup on the data track (used to load the executable from the disc)
static bool sector_data(int lba, uint8_t* out) {
    uint8_t raw[2352];
    bool audio;
    if (!read_raw(lba, raw, &audio) || audio) return false;
    memcpy(out, raw + 24, 2048);
    return true;
}
bool cd_read_file(const char* name, std::vector<uint8_t>& out) {
    uint8_t s[2048];
    if (!sector_data(16, s) || memcmp(s + 1, "CD001", 5)) return false;
    uint32_t lba, size;
    memcpy(&lba, s + 158, 4); memcpy(&size, s + 166, 4);
    std::vector<uint8_t> dir(((size + 2047) / 2048) * 2048);
    for (uint32_t i = 0; i < dir.size() / 2048; i++) if (!sector_data(lba + i, dir.data() + i * 2048)) return false;
    for (size_t i = 0; i < size;) {
        uint8_t n = dir[i];
        if (!n) { i = (i / 2048 + 1) * 2048; continue; }
        std::string nm((const char*)&dir[i + 33], dir[i + 32]);
        nm = nm.substr(0, nm.find(';'));
        if (nm == name) {
            uint32_t flba, fsize;
            memcpy(&flba, &dir[i + 2], 4); memcpy(&fsize, &dir[i + 10], 4);
            out.resize(((fsize + 2047) / 2048) * 2048);
            for (uint32_t k = 0; k < out.size() / 2048; k++) if (!sector_data(flba + k, out.data() + k * 2048)) return false;
            out.resize(fsize);
            return true;
        }
        i += n;
    }
    return false;
}

// ---------------------------------------------------------------- controller state
struct Response { uint8_t irq; std::vector<uint8_t> bytes; int64_t delay; };

static uint8_t g_index = 0;
static uint8_t g_ie = 0x1f, g_if = 0;   // the BIOS boot leaves all CD interrupts enabled
static std::deque<uint8_t> g_param;
static std::vector<uint8_t> g_resp;
static size_t g_resp_pos = 0;
static std::deque<Response> g_pending;     // queued interrupts (one is visible at a time)
static bool g_busy = false;
static uint8_t g_stat = 0x02;              // motor on
static uint8_t g_mode = 0;
static int g_setloc = 0;                   // target LBA from Setloc
static bool g_setloc_pending = false;
static int g_loc = 0;                      // current read head LBA
static int g_sector_lba = -1;               // LBA of the sector in the buffer
int cd_sector_lba() { return g_sector_lba; }
static enum { IDLE, READING, PLAYING } g_state = IDLE;
static uint8_t g_filter_file = 0, g_filter_chan = 0;
static bool g_muted = false, g_adpcm_muted = false;
static uint8_t g_vol[4] = {0x80, 0, 0x80, 0};      // L->L, L->R, R->R, R->L (pending)
static uint8_t g_vol_applied[4] = {0x80, 0, 0x80, 0};
static uint8_t g_last_header[8];
static uint8_t g_last_sub[4];

// sector buffers: the most recently read sector, and the data FIFO it is loaded into
static uint8_t g_sector[2352];
static bool g_sector_ready = false;
static std::vector<uint8_t> g_data;
static size_t g_data_pos = 0;
static int g_play_track_end = 0;

// Data reads (not XA-ADPCM streaming, which movies and music use) can go faster than the real
// drive when fast_cd is on; the game only waits for sectors, it never times them.
static int64_t read_period() {
    int64_t p = (g_mode & 0x80) ? CPU_HZ / 150 : CPU_HZ / 75;
    if (g_cfg.fast_cd && !(g_mode & 0x40) && g_state != PLAYING) p /= 4;
    return p;
}

static void stat_bits() {
    g_stat &= ~0xe0u;
    if (g_state == READING) g_stat |= 0x20;
    if (g_state == PLAYING) g_stat |= 0x80;
    g_stat |= 0x02;
}

static void raise_if_ready() {
    if ((g_if & 7) == 0 && !g_pending.empty() && !sched_active(EV_CDROM)) {
        sched_in(EV_CDROM, std::max<int64_t>(1, g_pending.front().delay));
    }
}

static void queue_resp(uint8_t irq, std::vector<uint8_t> b, int64_t delay) {
    g_pending.push_back(Response{irq, std::move(b), delay});
    raise_if_ready();
}

static void deliver_next() {
    if (g_pending.empty() || (g_if & 7)) return;
    Response r = std::move(g_pending.front());
    g_pending.pop_front();
    g_resp = std::move(r.bytes);
    g_resp_pos = 0;
    g_if = (g_if & ~7u) | (r.irq & 7);
    g_busy = false;
    LOG("cd", "INT%d [%s]", r.irq, [&] { static char b[64]; b[0] = 0; for (auto x : g_resp) snprintf(b + strlen(b), 4, "%02x ", x); return b; }());
    if (g_ie & g_if & 0x1f) irq_raise(IRQ_CDROM);
}

static void stop_reading() {
    g_state = IDLE;
    sched_cancel(EV_CDROM_READ);
    sched_cancel(EV_CDROM_AUDIO);
}

static void start_reading(bool play) {
    if (g_setloc_pending) { g_loc = g_setloc; g_setloc_pending = false; }
    g_state = play ? PLAYING : READING;
    stat_bits();
    // seek time: short, scaled a little with distance
    sched_in(play ? EV_CDROM_AUDIO : EV_CDROM_READ, read_period() + 20000);
}

// ---------------------------------------------------------------- XA-ADPCM
static const int k0[5] = {0, 60, 115, 98, 122};
static const int k1[5] = {0, 0, -52, -55, -60};
static int32_t g_xa_prev[2][2];
static std::vector<int16_t> g_xa_out;   // interleaved stereo at 44.1 kHz (resampled)
static double g_xa_phase = 0;
static int16_t g_xa_last[2] = {0, 0};

// High-quality resampling: Kaiser-windowed sinc, 32 taps, 512 phases, cut off just below the
// lower of the two Nyquist rates (so 18.9 kHz streams are not aliased either).
static const int kTaps = 32, kPhases = 512;
static std::vector<float> g_sinc;            // kPhases+1 rows of kTaps
static int g_sinc_rate = 0;
static std::vector<float> g_hq_hist[2];      // input history per channel
static double g_hq_pos = 0;                  // position of the next output, in input samples from hist start

static double bessel_i0(double x) {
    double s = 1, t = 1;
    for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}
static void build_sinc(int rate) {
    g_sinc_rate = rate;
    g_sinc.assign((size_t)(kPhases + 1) * kTaps, 0.f);
    double fc = 0.5 * std::min(1.0, 44100.0 / rate) * 0.92;   // cutoff, cycles per input sample
    const double beta = 8.0, i0b = bessel_i0(beta);
    for (int ph = 0; ph <= kPhases; ph++) {
        double frac = (double)ph / kPhases, sum = 0;
        float* row = &g_sinc[(size_t)ph * kTaps];
        for (int k = 0; k < kTaps; k++) {
            double x = (k - (kTaps / 2 - 1)) - frac;              // distance from the output point
            double sinc = x == 0 ? 2 * fc : std::sin(2 * M_PI * fc * x) / (M_PI * x);
            double r = x / (kTaps / 2.0);
            double win = std::fabs(r) >= 1 ? 0 : bessel_i0(beta * std::sqrt(1 - r * r)) / i0b;
            row[k] = (float)(sinc * win);
            sum += row[k];
        }
        for (int k = 0; k < kTaps; k++) row[k] = (float)(row[k] / sum);
    }
}
static void xa_resample_hq(const std::vector<int16_t>& lr, int rate) {
    if (rate != g_sinc_rate) build_sinc(rate);
    size_t n = lr.size() / 2;
    for (int ch = 0; ch < 2; ch++) {
        if (g_hq_hist[ch].empty()) g_hq_hist[ch].assign(kTaps, 0.f);
        for (size_t i = 0; i < n; i++) g_hq_hist[ch].push_back(lr[i * 2 + ch]);
    }
    const double step = (double)rate / 44100.0;
    size_t avail = g_hq_hist[0].size();
    while (g_hq_pos + kTaps < (double)avail) {
        size_t i0 = (size_t)g_hq_pos;
        double fr = g_hq_pos - i0;
        int ph = (int)(fr * kPhases + 0.5);
        const float* row = &g_sinc[(size_t)ph * kTaps];
        for (int ch = 0; ch < 2; ch++) {
            const float* h = &g_hq_hist[ch][i0];
            float acc = 0;
            for (int k = 0; k < kTaps; k++) acc += h[k] * row[k];
            g_xa_out.push_back((int16_t)std::clamp(acc, -32768.f, 32767.f));
        }
        g_hq_pos += step;
    }
    // drop consumed history
    size_t drop = std::min((size_t)g_hq_pos, avail);
    for (int ch = 0; ch < 2; ch++) g_hq_hist[ch].erase(g_hq_hist[ch].begin(), g_hq_hist[ch].begin() + drop);
    g_hq_pos -= (double)drop;
}

static void xa_resample_push(const std::vector<int16_t>& lr, int rate) {
    if (g_cfg.hq_audio) { xa_resample_hq(lr, rate); return; }
    // linear resampling to 44100 Hz
    double step = (double)rate / 44100.0;
    size_t n = lr.size() / 2;
    double pos = g_xa_phase;
    while (pos < (double)n) {
        size_t i = (size_t)pos;
        double fr = pos - i;
        for (int ch = 0; ch < 2; ch++) {
            int a = i == 0 ? g_xa_last[ch] : lr[(i - 1) * 2 + ch];
            int b = lr[i * 2 + ch];
            g_xa_out.push_back((int16_t)(a + (b - a) * fr));
        }
        pos += step;
    }
    g_xa_phase = pos - (double)n;
    g_xa_last[0] = lr[(n - 1) * 2];
    g_xa_last[1] = lr[(n - 1) * 2 + 1];
}

static void xa_decode(const uint8_t* sec) {
    const uint8_t* sub = sec + 16;
    uint8_t ci = sub[3];
    bool stereo = (ci & 3) == 1;
    int rate = (ci & 0x0c) ? 18900 : 37800;
    bool bits8 = (ci & 0x30) == 0x10;
    const uint8_t* data = sec + 24;
    std::vector<int16_t> mono[2];
    for (int g = 0; g < 18; g++) {
        const uint8_t* grp = data + g * 128;
        int units = bits8 ? 4 : 8;
        for (int u = 0; u < units; u++) {
            uint8_t hdr = bits8 ? grp[4 + u] : grp[4 + u];
            int shift = hdr & 0x0f;
            int filt = (hdr >> 4) & 3;
            if (shift > 12) shift = 9;
            int ch = stereo ? (u & 1) : 0;
            for (int j = 0; j < 28; j++) {
                int32_t s;
                if (bits8) {
                    s = (int8_t)grp[16 + j * 4 + u];
                    s = (s << 8) >> shift;
                } else {
                    uint8_t b = grp[16 + j * 4 + (u >> 1)];
                    int nib = (u & 1) ? (b >> 4) : (b & 15);
                    s = (int16_t)(nib << 12) >> shift;
                }
                int32_t& p1 = g_xa_prev[ch][0];
                int32_t& p2 = g_xa_prev[ch][1];
                s += (p1 * k0[filt] + p2 * k1[filt] + 32) >> 6;
                s = std::clamp(s, -32768, 32767);
                p2 = p1; p1 = s;
                mono[ch].push_back((int16_t)s);
            }
        }
    }
    std::vector<int16_t> lr;
    if (stereo) {
        size_t n = std::min(mono[0].size(), mono[1].size());
        lr.resize(n * 2);
        for (size_t i = 0; i < n; i++) { lr[i * 2] = mono[0][i]; lr[i * 2 + 1] = mono[1][i]; }
    } else {
        lr.resize(mono[0].size() * 2);
        for (size_t i = 0; i < mono[0].size(); i++) lr[i * 2] = lr[i * 2 + 1] = mono[0][i];
    }
    if (lr.empty()) return;
    g_xa_out.clear();
    xa_resample_push(lr, rate);
    if (!g_muted && !g_adpcm_muted) {
        // apply the CD volume matrix
        for (size_t i = 0; i < g_xa_out.size(); i += 2) {
            int l = g_xa_out[i], r = g_xa_out[i + 1];
            int ol = (l * g_vol_applied[0] + r * g_vol_applied[3]) >> 7;
            int orr = (l * g_vol_applied[1] + r * g_vol_applied[2]) >> 7;
            g_xa_out[i] = (int16_t)std::clamp(ol, -32768, 32767);
            g_xa_out[i + 1] = (int16_t)std::clamp(orr, -32768, 32767);
        }
        spu_cd_audio(g_xa_out.data(), (int)g_xa_out.size() / 2, 44100);
    }
}

// ---------------------------------------------------------------- sector reading
static void read_sector_event() {
    if (g_state != READING) return;
    sched_in(EV_CDROM_READ, read_period());
    bool audio = false;
    uint8_t raw[2352];
    int lba = g_loc++;
    if (!read_raw(lba, raw, &audio)) {
        LOG("cd", "read past end of disc (lba %d)", lba);
        stop_reading();
        queue_resp(5, {(uint8_t)(g_stat | 1), 0x04}, 2000);
        return;
    }
    if (audio) {
        if (g_mode & 1) {   // CDDA reads as data
            memcpy(g_sector, raw, 2352);
        }
        return;
    }
    memcpy(g_last_header, raw + 12, 4);
    memcpy(g_last_header + 4, raw + 16, 4);
    const uint8_t* sub = raw + 16;
    bool form2 = (sub[2] & 0x20) != 0;
    bool is_audio = (sub[2] & 0x04) != 0;
    bool realtime = (sub[2] & 0x40) != 0;
    // XA-ADPCM sectors go to the decoder, not to the host
    if ((g_mode & 0x40) && is_audio && realtime && form2) {
        if (!(g_mode & 0x08) || (sub[0] == g_filter_file && sub[1] == g_filter_chan)) xa_decode(raw);
        return;
    }
    if ((g_mode & 0x08) && (g_mode & 0x40) && is_audio) return;   // filtered-out audio
    memcpy(g_sector, raw, 2352);
    g_sector_lba = lba;
    g_sector_ready = true;
    // the previous INT1 may not have been acknowledged yet; the newest sector wins
    for (auto it = g_pending.begin(); it != g_pending.end();) {
        if (it->irq == 1) it = g_pending.erase(it); else ++it;
    }
    queue_resp(1, {g_stat}, 1);
}

// ---------------------------------------------------------------- CD-DA
static void play_event() {
    if (g_state != PLAYING) return;
    sched_in(EV_CDROM_AUDIO, CPU_HZ / 75);
    bool audio = false;
    uint8_t raw[2352];
    int lba = g_loc++;
    if (!read_raw(lba, raw, &audio) || (g_play_track_end && lba >= g_play_track_end)) {
        if (g_mode & 2) {   // auto pause at end of track
            stop_reading();
            queue_resp(4, {g_stat}, 1000);
        } else {
            stop_reading();
        }
        return;
    }
    if (audio && !g_muted) {
        int16_t lr[588 * 2];
        memcpy(lr, raw, sizeof lr);
        for (int i = 0; i < 588; i++) {
            int l = lr[i * 2], r = lr[i * 2 + 1];
            lr[i * 2] = (int16_t)std::clamp((l * g_vol_applied[0] + r * g_vol_applied[3]) >> 7, -32768, 32767);
            lr[i * 2 + 1] = (int16_t)std::clamp((l * g_vol_applied[1] + r * g_vol_applied[2]) >> 7, -32768, 32767);
        }
        spu_cd_audio(lr, 588, 44100);
    }
    if ((g_mode & 4) && (lba % 10) == 0) {   // report mode
        int m, s, f;
        lba_to_msf(lba, &m, &s, &f);
        int tl = lba - g_t2_start;
        int tm, ts, tf;
        lba_to_msf(tl - 150 + 150, &tm, &ts, &tf);
        queue_resp(1, {g_stat, 0x02, 0x01, i2bcd(m), i2bcd(s), i2bcd(f), 0, 0}, 1);
    }
}

void cd_event(int ev) {
    if (ev == EV_CDROM) { sched_cancel(EV_CDROM); deliver_next(); return; }
    if (ev == EV_CDROM_READ) { sched_cancel(EV_CDROM_READ); read_sector_event(); return; }
    if (ev == EV_CDROM_AUDIO) { sched_cancel(EV_CDROM_AUDIO); play_event(); return; }
}

// ---------------------------------------------------------------- commands
static const int64_t T_ACK = 25000;          // first response (INT3)
static const int64_t T_SECOND = 120000;      // second response for Pause/Init/Seek/GetID/...

static void command(uint8_t cmd) {
    std::vector<uint8_t> p(g_param.begin(), g_param.end());
    g_param.clear();
    g_busy = true;
    LOG("cd", "cmd %02x [%zu params%s]", cmd, p.size(), p.empty() ? "" : [&] { static char b[64]; b[0] = ':'; b[1] = 0; for (auto x : p) snprintf(b + strlen(b), 5, " %02x", x); return b; }());
    stat_bits();
    switch (cmd) {
        case 0x01: queue_resp(3, {g_stat}, T_ACK); g_stat &= ~0x10u; break;    // Getstat
        case 0x02:   // Setloc
            if (p.size() >= 3) {
                g_setloc = msf_to_lba(bcd2i(p[0]), bcd2i(p[1]), bcd2i(p[2]));
                g_setloc_pending = true;
            }
            queue_resp(3, {g_stat}, T_ACK);
            break;
        case 0x03: {   // Play [track]
            int track = p.empty() ? 0 : bcd2i(p[0]);
            if (track == 2) { g_setloc = g_t2_start; g_setloc_pending = true; }
            else if (track == 1) { g_setloc = 0; g_setloc_pending = true; }
            g_play_track_end = 0;
            if (g_setloc_pending ? g_setloc >= g_t2_start : g_loc >= g_t2_start) g_play_track_end = g_t2_start + g_t2_sectors;
            stop_reading();
            start_reading(true);
            stat_bits();
            queue_resp(3, {g_stat}, T_ACK);
            break;
        }
        case 0x04: case 0x05: queue_resp(3, {g_stat}, T_ACK); break;           // Forward / Backward
        case 0x06: case 0x1b:   // ReadN / ReadS
            stop_reading();
            start_reading(false);
            stat_bits();
            queue_resp(3, {g_stat}, T_ACK);
            break;
        case 0x07: g_stat |= 2; queue_resp(3, {g_stat}, T_ACK); queue_resp(2, {g_stat}, T_SECOND); break;   // MotorOn
        case 0x08: {   // Stop
            uint8_t s = g_stat;
            stop_reading();
            stat_bits();
            queue_resp(3, {s}, T_ACK);
            queue_resp(2, {g_stat}, T_SECOND);
            break;
        }
        case 0x09: {   // Pause
            uint8_t s = g_stat;
            stop_reading();
            stat_bits();
            queue_resp(3, {s}, T_ACK);
            queue_resp(2, {g_stat}, T_SECOND / 2);
            break;
        }
        case 0x0a:     // Init
            stop_reading();
            g_mode = 0x20;
            g_muted = false;
            stat_bits();
            queue_resp(3, {g_stat}, T_ACK);
            queue_resp(2, {g_stat}, T_SECOND);
            break;
        case 0x0b: g_muted = true; queue_resp(3, {g_stat}, T_ACK); break;     // Mute
        case 0x0c: g_muted = false; queue_resp(3, {g_stat}, T_ACK); break;    // Demute
        case 0x0d:     // Setfilter
            if (p.size() >= 2) { g_filter_file = p[0]; g_filter_chan = p[1]; }
            queue_resp(3, {g_stat}, T_ACK);
            break;
        case 0x0e:     // Setmode
            if (!p.empty()) g_mode = p[0];
            queue_resp(3, {g_stat}, T_ACK);
            break;
        case 0x0f: queue_resp(3, {g_stat, g_mode, 0, g_filter_file, g_filter_chan}, T_ACK); break;   // Getparam
        case 0x10:     // GetlocL
            queue_resp(3, std::vector<uint8_t>(g_last_header, g_last_header + 8), T_ACK);
            break;
        case 0x11: {   // GetlocP
            int lba = std::max(0, g_loc - 1);
            int m, s, f, tm, ts, tf;
            lba_to_msf(lba, &m, &s, &f);
            int track = lba >= g_t2_start ? 2 : 1;
            int rel = track == 2 ? lba - g_t2_start : lba;
            tm = rel / 4500; ts = (rel / 75) % 60; tf = rel % 75;
            queue_resp(3, {i2bcd(track), 1, i2bcd(tm), i2bcd(ts), i2bcd(tf), i2bcd(m), i2bcd(s), i2bcd(f)}, T_ACK);
            break;
        }
        case 0x12: queue_resp(3, {g_stat}, T_ACK); break;     // SetSession
        case 0x13: queue_resp(3, {g_stat, 0x01, 0x02}, T_ACK); break;   // GetTN
        case 0x14: {   // GetTD
            int t = p.empty() ? 0 : bcd2i(p[0]);
            int lba = t == 0 ? g_t2_start + g_t2_sectors : (t == 1 ? 0 : g_t2_start);
            int m, s, f;
            lba_to_msf(lba, &m, &s, &f);
            if (t > 2) queue_resp(5, {(uint8_t)(g_stat | 1), 0x10}, T_ACK);
            else queue_resp(3, {g_stat, i2bcd(m), i2bcd(s)}, T_ACK);
            break;
        }
        case 0x15: case 0x16:   // SeekL / SeekP
            stop_reading();
            if (g_setloc_pending) { g_loc = g_setloc; g_setloc_pending = false; }
            stat_bits();
            queue_resp(3, {g_stat}, T_ACK);
            queue_resp(2, {g_stat}, T_SECOND);
            break;
        case 0x19: {   // Test
            uint8_t sub = p.empty() ? 0 : p[0];
            if (sub == 0x20) queue_resp(3, {0x94, 0x09, 0x19, 0xc0}, T_ACK);
            else queue_resp(3, {g_stat}, T_ACK);
            break;
        }
        case 0x1a:     // GetID: licensed, NTSC-U ("SCEA")
            queue_resp(3, {g_stat}, T_ACK);
            queue_resp(2, {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'}, T_SECOND);
            break;
        case 0x1e:     // ReadTOC
            queue_resp(3, {g_stat}, T_ACK);
            queue_resp(2, {g_stat}, T_SECOND * 4);
            break;
        default:
            LOG("cd", "unknown command %02x", cmd);
            queue_resp(5, {(uint8_t)(g_stat | 1), 0x40}, T_ACK);
            break;
    }
}

// ---------------------------------------------------------------- registers
static uint8_t cd_read_impl(int reg);
uint8_t cd_read(int reg) {
    uint8_t v = cd_read_impl(reg);
    LOG("cdreg", "read reg%d.%d -> %02x", reg, g_index, v);
    return v;
}
static uint8_t cd_read_impl(int reg) {
    switch (reg) {
        case 0: {
            uint8_t s = g_index & 3;
            s |= g_param.empty() ? 0x08 : 0;
            s |= g_param.size() < 16 ? 0x10 : 0;
            s |= g_resp_pos < g_resp.size() ? 0x20 : 0;
            s |= g_data_pos < g_data.size() ? 0x40 : 0;
            s |= g_busy ? 0x80 : 0;
            return s;
        }
        case 1: {
            if (g_resp_pos < g_resp.size()) return g_resp[g_resp_pos++];
            return 0;
        }
        case 2: return g_data_pos < g_data.size() ? g_data[g_data_pos++] : 0;
        case 3:
            if (g_index & 1) return (uint8_t)(g_if | 0xe0);
            return (uint8_t)(g_ie | 0xe0);
    }
    return 0;
}

void cd_write(int reg, uint8_t v) {
    LOG("cdreg", "write reg%d.%d = %02x", reg, g_index, v);
    switch (reg) {
        case 0: g_index = v & 3; return;
        case 1:
            if (g_index == 0) command(v);
            else if (g_index == 3) g_vol[2] = v;     // R->R
            return;
        case 2:
            if (g_index == 0) { if (g_param.size() < 16) g_param.push_back(v); }
            else if (g_index == 1) { g_ie = v & 0x1f; if (g_ie & g_if & 0x1f) irq_raise(IRQ_CDROM); }
            else if (g_index == 2) g_vol[0] = v;     // L->L
            else g_vol[3] = v;                       // R->L
            return;
        case 3:
            if (g_index == 0) {
                if (v & 0x80) {   // BFRD: load the sector buffer into the data FIFO
                    if (g_sector_ready) {
                        if (g_mode & 0x20) g_data.assign(g_sector + 12, g_sector + 12 + 0x924);
                        else g_data.assign(g_sector + 24, g_sector + 24 + 0x800);
                        g_data_pos = 0;
                        g_sector_ready = false;
                    }
                } else {
                    g_data.clear();
                    g_data_pos = 0;
                }
            } else if (g_index == 1) {
                g_if &= ~(v & 0x1f);
                if (v & 0x40) g_param.clear();
                if ((g_if & 7) == 0) raise_if_ready();
            } else if (g_index == 2) {
                g_vol[1] = v;   // L->R
            } else {
                g_adpcm_muted = v & 1;
                if (v & 0x20) memcpy(g_vol_applied, g_vol, 4);
            }
            return;
    }
}

uint32_t cd_dma_read() {
    uint32_t w = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = g_data_pos < g_data.size() ? g_data[g_data_pos++] : 0;
        w |= (uint32_t)b << (8 * i);
    }
    return w;
}

bool cd_init_pack(const char* path, std::string* err) {
    DiscLayout lay;
    if (!pack_open(path, &lay, err)) return false;
    g_t1_sectors = lay.t1_sectors;
    g_t2_start = lay.t2_start;
    g_t2_sectors = lay.t2_sectors;
    g_packed = true;
    fprintf(stderr, "[cd] packed disc %s: data track %d sectors, audio track %d sectors at LBA %d\n", path, g_t1_sectors, g_t2_sectors, g_t2_start);
    return true;
}

// Packs the disc opened by cd_init (the .cue/.bin) into one file
bool cd_make_pack(const char* out_path, void (*progress)(int, int), std::string* err) {
    if (!g_t1 && !g_chd) { *err = "No disc image is open."; return false; }
    DiscLayout lay;
    lay.t1_sectors = g_t1_sectors;
    lay.t2_start = g_t2_start;
    lay.t2_sectors = g_t2_sectors;
    return pack_build(out_path, lay, read_raw, progress, err);
}

void cd_close_image() {
    if (g_chd) chd_close(g_chd);
    g_chd = nullptr;
    g_chd_tracks.clear();
    g_chd_hunk_no = -1;
    if (g_t1) fclose(g_t1);
    if (g_t2) fclose(g_t2);
    g_t1 = g_t2 = nullptr;
}

bool cd_init(const char* path) {
    std::string p = path;
    std::string ext = p.size() > 4 ? p.substr(p.size() - 4) : "";
    for (auto& ch : ext) ch = (char)tolower(ch);
    if (ext == ".chd") {
        if (!open_chd(p)) return false;
        fprintf(stderr, "[cd] %s: data track %d sectors, audio %d sectors at LBA %d\n", path, g_t1_sectors, g_t2_sectors, g_t2_start);
        return true;
    }
    if (p.size() > 4 && (p.substr(p.size() - 4) == ".cue" || p.substr(p.size() - 4) == ".CUE")) {
        if (!open_cue(p)) return false;
    } else {
        std::string d = p;
        if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
        g_t1 = fopen((d + "track01.bin").c_str(), "rb");
        if (!g_t1) return false;
        g_t2 = fopen((d + "track02.bin").c_str(), "rb");
        g_t1_sectors = (int)file_sectors(g_t1);
        g_t2_start = g_t1_sectors + 150;
        FILE* info = fopen((d + "disc.txt").c_str(), "r");
        if (info) {
            char line[128];
            while (fgets(line, sizeof line, info)) sscanf(line, "track2_start=%d", &g_t2_start);
            fclose(info);
        }
        if (g_t2) g_t2_sectors = (int)file_sectors(g_t2);
    }
    fprintf(stderr, "[cd] data track %d sectors, audio track %d sectors at LBA %d\n", g_t1_sectors, g_t2_sectors, g_t2_start);
    return true;
}
