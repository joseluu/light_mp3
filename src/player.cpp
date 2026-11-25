#define MINIMP3_IMPLEMENTATION
#include "minimp3_ex.h"
#include "player.h"
#include <mmsystem.h>
#include <cstring>

// ---------------------------------------------------------------------------
Player::Player()  = default;
Player::~Player() { Close(); }

// ---------------------------------------------------------------------------
// waveOut callback — called from audio thread when a buffer finishes.
// ---------------------------------------------------------------------------
void CALLBACK Player::WaveOutProc(HWAVEOUT, UINT uMsg,
    DWORD_PTR dwInstance, DWORD_PTR, DWORD_PTR)
{
    if (uMsg != WOM_DONE) return;
    Player* self = (Player*)dwInstance;
    if (self->m_event) SetEvent(self->m_event);   // only SetEvent is safe here
}

// ---------------------------------------------------------------------------
void Player::StartWorker() {
    if (m_worker.joinable()) return;
    if (!m_event) m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_quit = false;
    m_worker = std::thread([this] { WorkerLoop(); });
}

void Player::StopWorker() {
    if (m_worker.joinable()) {
        m_quit = true;
        SetEvent(m_event);
        m_worker.join();
    }
    if (m_event) { CloseHandle(m_event); m_event = nullptr; }
}

void Player::WorkerLoop() {
    while (true) {
        WaitForSingleObject(m_event, INFINITE);
        if (m_quit) break;
        std::lock_guard<std::mutex> lk(m_mtx);
        if (!m_playing || !m_hWaveOut) continue;
        for (int i = 0; i < NUM_BUFS; i++)
            if (m_headers[i].dwFlags & WHDR_DONE)
                FillBuffer(i);
    }
}

// ---------------------------------------------------------------------------
// Fill one waveOut buffer from the decoded PCM at m_playPos.
// ---------------------------------------------------------------------------
void Player::FillBuffer(int idx) {
    WAVEHDR& wh = m_headers[idx];
    if (wh.dwFlags & WHDR_PREPARED)
        waveOutUnprepareHeader(m_hWaveOut, &wh, sizeof(wh));

    size_t frameSamples = (size_t)BUF_SAMPLES * m_channels;
    auto& buf = m_bufData[idx];
    buf.resize(frameSamples);

    size_t srcOffset = m_playPos * m_channels;
    size_t totalPcm  = m_totalSamples * m_channels;
    size_t avail = 0;
    if (srcOffset < totalPcm)
        avail = totalPcm - srcOffset;
    size_t toCopy = (avail < frameSamples) ? avail : frameSamples;

    // Nothing left — signal end-of-track, do NOT submit another buffer.
    if (toCopy == 0) {
        wh = {};
        if (m_playing) {
            m_playing = false;
            if (m_notify)
                PostMessage(m_notify, WM_PLAYER_DONE, 0, 0);
        }
        return;
    }

    memcpy(buf.data(), m_pcm.data() + srcOffset, toCopy * sizeof(int16_t));
    if (toCopy < frameSamples)
        memset(buf.data() + toCopy, 0, (frameSamples - toCopy) * sizeof(int16_t));

    m_playPos += toCopy / m_channels;

    wh = {};
    wh.lpData         = (LPSTR)buf.data();
    wh.dwBufferLength = (DWORD)(frameSamples * sizeof(int16_t));
    waveOutPrepareHeader(m_hWaveOut, &wh, sizeof(wh));
    waveOutWrite(m_hWaveOut, &wh, sizeof(wh));
}

// ---------------------------------------------------------------------------
// Parse a WAV file (PCM 8/16/24/32-bit or IEEE float 32/64-bit) into m_pcm.
bool Player::LoadWav(const std::vector<uint8_t>& d) {
    auto rd16 = [&](size_t o) { return (uint32_t)(d[o] | (d[o + 1] << 8)); };
    auto rd32 = [&](size_t o) { return (uint32_t)(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24)); };

    uint32_t format = 0, channels = 0, rate = 0, bits = 0;
    size_t dataOff = 0, dataLen = 0;
    bool haveFmt = false;

    size_t pos = 12;
    while (pos + 8 <= d.size()) {
        size_t sz = rd32(pos + 4);
        size_t body = pos + 8;
        if (!memcmp(d.data() + pos, "fmt ", 4) && sz >= 16 && body + 16 <= d.size()) {
            format   = rd16(body);
            channels = rd16(body + 2);
            rate     = rd32(body + 4);
            bits     = rd16(body + 14);
            if (format == 0xFFFE && sz >= 26 && body + 26 <= d.size())
                format = rd16(body + 24);   // WAVE_FORMAT_EXTENSIBLE sub-format
            haveFmt = true;
        } else if (!memcmp(d.data() + pos, "data", 4)) {
            dataOff = body;
            dataLen = (body + sz > d.size()) ? d.size() - body : sz;
            break;
        }
        pos = body + sz + (sz & 1);
    }
    if (!haveFmt || !dataOff || !channels || !rate) return false;

    bool isFloat = (format == 3);
    if (format != 1 && !isFloat) return false;
    if (isFloat ? (bits != 32 && bits != 64) : (bits != 8 && bits != 16 && bits != 24 && bits != 32))
        return false;

    size_t bps = bits / 8;
    size_t n = dataLen / bps;
    n -= n % channels;
    if (!n) return false;

    std::vector<int16_t> pcm(n);
    const uint8_t* p = d.data() + dataOff;
    for (size_t i = 0; i < n; i++, p += bps) {
        int32_t v;
        if (isFloat) {
            double f;
            if (bits == 32) { float x; memcpy(&x, p, 4); f = x; }
            else            { memcpy(&f, p, 8); }
            if (f > 1.0) f = 1.0; else if (f < -1.0) f = -1.0;
            v = (int32_t)(f * 32767.0);
        } else if (bits == 8)  v = ((int)p[0] - 128) << 8;
        else if (bits == 16)   v = (int16_t)(p[0] | (p[1] << 8));
        else if (bits == 24)   v = (int16_t)(p[1] | (p[2] << 8));
        else                   v = (int16_t)(p[2] | (p[3] << 8));
        pcm[i] = (int16_t)v;
    }

    m_sampleRate   = (int)rate;
    m_channels     = (int)channels;
    m_totalSamples = n / channels;
    m_pcm.swap(pcm);
    return true;
}

// ---------------------------------------------------------------------------
bool Player::Open(const std::wstring& path, HWND notifyWnd) {
    Close();
    m_notify = notifyWnd;

    // Decode entire MP3 to PCM using minimp3.
    mp3dec_t mp3d;
    mp3dec_file_info_t info = {};
    mp3dec_init(&mp3d);

    // Read file into memory (minimp3_ex needs a path; use its built-in loader).
    // Convert wstring path to narrow for minimp3 on Windows.
    // minimp3_ex on Windows uses fopen, so we'll read the file ourselves.
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        MessageBoxW(notifyWnd, L"Cannot open file.", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> fileData(fsize);
    fread(fileData.data(), 1, fsize, f);
    fclose(f);

    // WAV (RIFF/WAVE) files are converted to 16-bit PCM directly.
    if (fileData.size() >= 12 && !memcmp(fileData.data(), "RIFF", 4) &&
        !memcmp(fileData.data() + 8, "WAVE", 4)) {
        if (!LoadWav(fileData)) {
            MessageBoxW(notifyWnd, L"Unsupported or invalid WAV file.", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }
        m_playPos = 0;
        m_open    = true;
        m_playing = false;
        return true;
    }

    // Decode from memory buffer.
    int result = mp3dec_load_buf(&mp3d, fileData.data(), fileData.size(), &info, nullptr, nullptr);
    if (result || !info.samples || !info.channels) {
        if (info.buffer) free(info.buffer);
        MessageBoxW(notifyWnd, L"Failed to decode MP3.", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

    m_sampleRate  = info.hz;
    m_channels    = info.channels;
    m_totalSamples = info.samples / info.channels;

    // Copy decoded PCM into our own buffer and free minimp3's allocation.
    m_pcm.assign(info.buffer, info.buffer + info.samples);
    free(info.buffer);

    m_playPos = 0;
    m_open    = true;
    m_playing = false;
    return true;
}

// ---------------------------------------------------------------------------
void Player::Play() {
    if (!m_open) return;
    if (m_playing) return;

    // Open waveOut device.
    if (!m_hWaveOut) {
        WAVEFORMATEX wfx = {};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = (WORD)m_channels;
        wfx.nSamplesPerSec  = (DWORD)m_sampleRate;
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

        MMRESULT mr = waveOutOpen(&m_hWaveOut, WAVE_MAPPER, &wfx,
            (DWORD_PTR)WaveOutProc, (DWORD_PTR)this, CALLBACK_FUNCTION);
        if (mr != MMSYSERR_NOERROR) {
            MessageBoxW(m_notify, L"Cannot open audio device.",
                        L"Error", MB_OK | MB_ICONERROR);
            return;
        }
    }

    StartWorker();
    std::lock_guard<std::mutex> lk(m_mtx);
    m_playing = true;

    // Prime all buffers.
    for (int i = 0; i < NUM_BUFS; i++)
        FillBuffer(i);
}

// ---------------------------------------------------------------------------
void Player::Pause() {
    if (!m_open) return;
    if (m_playing) {
        m_playing = false;
        if (m_hWaveOut) waveOutPause(m_hWaveOut);
    } else {
        if (m_hWaveOut) {
            m_playing = true;
            waveOutRestart(m_hWaveOut);
        } else {
            Play();
        }
    }
}

// ---------------------------------------------------------------------------
void Player::Stop() {
    if (!m_open) return;
    m_playing = false;
    StopWorker();       // no more refills once the thread is gone
    if (m_hWaveOut) {
        waveOutReset(m_hWaveOut);
        for (int i = 0; i < NUM_BUFS; i++) {
            if (m_headers[i].dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(m_hWaveOut, &m_headers[i], sizeof(WAVEHDR));
            m_headers[i] = {};
        }
        waveOutClose(m_hWaveOut);
        m_hWaveOut = nullptr;
    }
    m_playPos = 0;
}

// ---------------------------------------------------------------------------
void Player::Close() {
    m_playing = false;
    StopWorker();
    if (m_hWaveOut) {
        waveOutReset(m_hWaveOut);
        for (int i = 0; i < NUM_BUFS; i++) {
            if (m_headers[i].dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(m_hWaveOut, &m_headers[i], sizeof(WAVEHDR));
            m_headers[i] = {};
        }
        waveOutClose(m_hWaveOut);
        m_hWaveOut = nullptr;
    }
    m_pcm.clear();
    m_pcm.shrink_to_fit();
    m_totalSamples = 0;
    m_sampleRate   = 0;
    m_channels     = 0;
    m_playPos      = 0;
    m_open         = false;
}

// ---------------------------------------------------------------------------
void Player::SeekTo(DWORD ms) {
    if (!m_open) return;
    size_t target = (size_t)((double)ms / 1000.0 * m_sampleRate);
    if (target > m_totalSamples) target = m_totalSamples;

    std::lock_guard<std::mutex> lk(m_mtx);
    bool wasPlaying = m_playing;
    m_playing = false;
    m_playPos = target;

    if (m_hWaveOut) {
        waveOutReset(m_hWaveOut);
        if (wasPlaying) {
            m_playing = true;
            for (int i = 0; i < NUM_BUFS; i++)
                FillBuffer(i);
        }
    }
}

// ---------------------------------------------------------------------------
DWORD Player::GetPosition() const {
    if (!m_open || m_sampleRate == 0) return 0;
    return (DWORD)((double)m_playPos * 1000.0 / m_sampleRate);
}

DWORD Player::GetLength() const {
    if (!m_open || m_sampleRate == 0) return 0;
    return (DWORD)((double)m_totalSamples * 1000.0 / m_sampleRate);
}
