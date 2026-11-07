#include "ProcessPipe.h"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace fc
{
#if JUCE_WINDOWS
ProcessPipe::~ProcessPipe()
{
    kill();
    if (process != nullptr) { WaitForSingleObject ((HANDLE) process, 2000); CloseHandle ((HANDLE) process); }
    if (readPipe != nullptr) CloseHandle ((HANDLE) readPipe);
}

bool ProcessPipe::start (const juce::StringArray& args)
{
    juce::String cmd;
    for (auto a : args)
    {
        a = a.replace ("\"", "\\\"");
        if (a.isEmpty() || a.containsAnyOf (" \t")) a = a.quoted();
        cmd << a << ' ';
    }

    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof (sa);
    sa.bInheritHandle = TRUE;
    HANDLE rd = nullptr, wr = nullptr;
    if (! CreatePipe (&rd, &wr, &sa, 16 * 1024 * 1024)) return false;   // contiene piu' fotogrammi interi
    SetHandleInformation (rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si {};
    si.cb = sizeof (si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    PROCESS_INFORMATION pi {};
    auto wide = cmd.trimEnd();
    const bool ok = CreateProcessW (nullptr, const_cast<LPWSTR> (wide.toWideCharPointer()), nullptr, nullptr, TRUE,
                                    CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi) != FALSE;
    CloseHandle (wr);   // la scrittura resta solo a ffmpeg
    if (! ok) { CloseHandle (rd); return false; }
    CloseHandle (pi.hThread);
    readPipe = rd;
    process = pi.hProcess;
    return true;
}

int ProcessPipe::read (void* dest, int numBytes)
{
    int total = 0;
    auto* out = static_cast<char*> (dest);
    while (total < numBytes && readPipe != nullptr)
    {
        DWORD avail = 0;
        if (! PeekNamedPipe ((HANDLE) readPipe, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0)
        {
            // si aspetta sul processo, non su ReadFile: se ffmpeg viene terminato si esce subito anche se un altro
            // processo avesse ereditato la pipe
            if (WaitForSingleObject ((HANDLE) process, 1) == WAIT_OBJECT_0)
            {
                if (! PeekNamedPipe ((HANDLE) readPipe, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
            }
            continue;
        }
        DWORD got = 0;
        if (! ReadFile ((HANDLE) readPipe, out + total, (DWORD) juce::jmin<int> ((int) avail, numBytes - total), &got, nullptr)) break;
        total += (int) got;
    }
    return total;
}

void ProcessPipe::kill()
{
    if (process != nullptr) TerminateProcess ((HANDLE) process, 0);
}
#else
ProcessPipe::~ProcessPipe() { child.kill(); }
bool ProcessPipe::start (const juce::StringArray& args) { return child.start (args, juce::ChildProcess::wantStdOut); }
int ProcessPipe::read (void* dest, int numBytes)
{
    int total = 0;
    while (total < numBytes)
    {
        const int n = child.readProcessOutput (static_cast<char*> (dest) + total, numBytes - total);
        if (n <= 0) break;
        total += n;
    }
    return total;
}
void ProcessPipe::kill() { child.kill(); }
#endif
} // namespace fc
