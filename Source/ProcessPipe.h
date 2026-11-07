#pragma once

#include <JuceHeader.h>

namespace fc
{
/** Processo figlio di cui si legge lo stdout ad alta velocita' (fotogrammi grezzi di ffmpeg).
    Su Windows juce::ChildProcess usa una pipe da 4 KB e dorme ~15 ms quando e' vuota: troppo lento per il video. */
class ProcessPipe
{
public:
    ProcessPipe() = default;
    ~ProcessPipe();

    bool start (const juce::StringArray& args);
    /** Legge fino a numBytes; si ferma prima solo se il processo finisce. Restituisce i byte letti. */
    int read (void* dest, int numBytes);
    /** Termina il processo (si puo' chiamare da un altro thread mentre read() aspetta). */
    void kill();

private:
#if JUCE_WINDOWS
    void* readPipe = nullptr;
    void* process = nullptr;
#else
    juce::ChildProcess child;
#endif
    JUCE_DECLARE_NON_COPYABLE (ProcessPipe)
};
} // namespace fc
