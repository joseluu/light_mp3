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
    if (!self->m_playing) return;

    // Find which buffer just finished and refill it.
    for (int i = 0; i < NUM_BUFS; i++) {
        if (self->m_headers[i].dwFlags & WHDR_DONE) {
            self->FillBuffer(i);
        }
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

    if (toCopy > 0)
        memcpy(buf.data(), m_pcm.data() + srcOffset, toCopy * sizeof(int16_t));
    if (toCopy < frameSamples)
        memset(buf.data() + toCopy, 0, (frameSamples - toCopy) * sizeof(int16_t));

    m_playPos += toCopy / m_channels;

    wh = {};
    wh.lpData         = (LPSTR)buf.data();
    wh.dwBufferLength = (DWORD)(frameSamples * sizeof(int16_t));
    waveOutPrepareHeader(m_hWaveOut, &wh, sizeof(wh));
    waveOutWrite(m_hWaveOut, &wh, sizeof(wh));

    // If we've played past the end, notify the UI.
    if (toCopy == 0 && m_playing) {
        m_playing = false;
        if (m_notify)
            PostMessage(m_notify, WM_PLAYER_DONE, 0, 0);
    }
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
    if (m_hWaveOut) {
        waveOutReset(m_hWaveOut);
    }
    m_playPos = 0;
}

// ---------------------------------------------------------------------------
void Player::Close() {
    m_playing = false;
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
    m_playPos = target;

    // If currently playing, reset waveOut and re-prime buffers.
    if (m_playing && m_hWaveOut) {
        waveOutReset(m_hWaveOut);
        for (int i = 0; i < NUM_BUFS; i++)
            FillBuffer(i);
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
