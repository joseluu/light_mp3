#pragma once
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>

// Posted to notify window when playback reaches the end.
#define WM_PLAYER_DONE  (WM_APP + 1)

class Player {
public:
    Player();
    ~Player();

    bool Open(const std::wstring& path, HWND notifyWnd = nullptr);
    void Play();
    void Pause();   // toggles play/pause
    void Stop();
    void Close();
    void SeekTo(DWORD ms);

    DWORD GetPosition() const;  // current position in ms
    DWORD GetLength()   const;  // total length in ms

    bool IsPlaying() const { return m_playing; }
    bool IsOpen()    const { return m_open; }

private:
    // Decoded PCM (signed 16-bit interleaved)
    std::vector<int16_t> m_pcm;
    int      m_sampleRate = 0;
    int      m_channels   = 0;
    size_t   m_totalSamples = 0;  // per channel

    // waveOut state
    HWAVEOUT m_hWaveOut  = nullptr;
    bool     m_open      = false;
    bool     m_playing   = false;
    HWND     m_notify    = nullptr;

    // Playback position (in samples, per channel)
    volatile size_t m_playPos = 0;

    // Double-buffered waveOut
    static const int NUM_BUFS = 4;
    static const int BUF_SAMPLES = 4096; // samples per channel per buffer
    WAVEHDR  m_headers[NUM_BUFS] = {};
    std::vector<int16_t> m_bufData[NUM_BUFS];

    void FillBuffer(int idx);
    static void CALLBACK WaveOutProc(HWAVEOUT hwo, UINT uMsg,
        DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2);
};
