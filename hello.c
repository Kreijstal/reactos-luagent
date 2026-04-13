/*
 * Serial Shell for ReactOS LiveCD (hello.exe)
 *
 * A lightweight remote shell over COM2, giving headless command-line
 * access to a ReactOS LiveCD from the host machine.
 *
 * Build:
 *   x86_64-w64-mingw32-gcc -o hello.exe hello.c -mconsole
 *
 * Test under Wine:
 *   printf "echo hi\r\nwhoami\r\nexit\r\n" | wine hello.exe --stdio
 *
 * Deploy on LiveCD:
 *   1. Add to the ISO file list:
 *        echo "reactos/system32/hello.exe=/tmp/hello.exe" \
 *          >> build/boot/livecd.Debug.lst
 *   2. Patch userinit.c to launch hello.exe at startup (see below).
 *   3. ninja livecd
 *
 * QEMU (COM1=debug, COM2=shell):
 *   qemu-system-x86_64 -m 512 -cdrom build/livecd.iso -boot d \
 *     -serial file:serial.log -serial pty -enable-kvm -display none
 *
 * Connect:
 *   screen /dev/pts/N 115200
 *   # or: cat /dev/pts/N & printf "whoami\r\n" > /dev/pts/N
 *
 * Userinit autorun hack (not committed):
 *   In base/system/userinit/userinit.c, top of wWinMain():
 *     STARTUPINFOW ssi; PROCESS_INFORMATION spi;
 *     WCHAR szCmd[] = L"hello.exe";
 *     ZeroMemory(&ssi, sizeof(ssi)); ZeroMemory(&spi, sizeof(spi));
 *     ssi.cb = sizeof(ssi);
 *     CreateProcessW(NULL, szCmd, NULL, NULL, FALSE,
 *                    CREATE_NEW_CONSOLE, NULL, NULL, &ssi, &spi);
 *   This only works on a pure LiveCD boot (no installed OS on disk).
 *   With an NTFS install present, the on-disk userinit runs instead;
 *   launch hello.exe manually: D:\reactos\system32\hello.exe
 *
 * Architecture:
 *   Transport (tx_write/tx_read1) is decoupled from the REPL, so the
 *   shell logic can be tested with --stdio under Wine without COM2.
 *
 * Builtins: echo, cd, pwd, set, exit
 * External: CreateProcess directly, fallback to cmd.exe /c (10s timeout)
 *
 * Known issues:
 *   - KVM may not deliver COM2 RX interrupts (IRQ 3). Short ReadFile
 *     timeouts (500ms) work around this by polling instead of blocking.
 *   - Double prompt ($ $) on \r\n input — cosmetic, from treating
 *     \r and \n as independent line terminators.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ---- Transport layer ---- */

static HANDLE hIn  = INVALID_HANDLE_VALUE;
static HANDLE hOut = INVALID_HANDLE_VALUE;

static void tx_write(const char *s)
{
    DWORD written;
    WriteFile(hOut, s, (DWORD)strlen(s), &written, NULL);
}

static void tx_writebuf(const char *buf, DWORD len)
{
    DWORD written;
    WriteFile(hOut, buf, len, &written, NULL);
}

static BOOL tx_read1(char *c)
{
    DWORD nread;
    DWORD ticks = 0;
    for (;;) {
        if (ReadFile(hIn, c, 1, &nread, NULL) && nread == 1)
            return TRUE;
        ticks++;
        if (ticks % 5 == 0)
            OutputDebugStringA("serial-shell: waiting for input\n");
    }
}

/* ---- REPL ---- */

/*
 * Read one line. \r and \n each terminate independently.
 * \r\n produces one line + one empty return (skipped by caller).
 */
static int repl_readline(char *buf, int max)
{
    int pos = 0;

    while (pos < max - 1) {
        char c;
        if (!tx_read1(&c))
            continue;

        if (c == '\r' || c == '\n') {
            buf[pos] = '\0';
            if (pos > 0)
                tx_write("\r\n");
            return pos;
        }

        if (c == '\b' || c == 0x7f) {
            if (pos > 0) {
                pos--;
                tx_write("\b \b");
            }
            continue;
        }

        buf[pos] = c;
        tx_writebuf(&c, 1);
        pos++;
    }
    buf[pos] = '\0';
    return pos;
}

static void pipe_output(HANDLE hRd)
{
    char buf[256];
    DWORD br;
    while (ReadFile(hRd, buf, sizeof(buf), &br, NULL) && br > 0) {
        DWORD i, start = 0;
        for (i = 0; i < br; i++) {
            if (buf[i] == '\n') {
                if (i > start)
                    tx_writebuf(&buf[start], i - start);
                tx_write("\r\n");
                start = i + 1;
            }
        }
        if (start < br)
            tx_writebuf(&buf[start], br - start);
    }
}

static void run_cmd(const char *line)
{
    SECURITY_ATTRIBUTES sa;
    HANDLE hRd, hWr, hNul;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char cmd[600];

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hRd, &hWr, &sa, 0)) {
        tx_write("CreatePipe failed\r\n");
        return;
    }
    SetHandleInformation(hRd, HANDLE_FLAG_INHERIT, 0);

    hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ,
                       &sa, OPEN_EXISTING, 0, NULL);

    ZeroMemory(&si, sizeof(si));
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = hWr;
    si.hStdError  = hWr;
    si.hStdInput  = hNul;

    ZeroMemory(&pi, sizeof(pi));

    /* Try running directly, fall back to cmd.exe /c */
    _snprintf(cmd, sizeof(cmd), "%s", line);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        _snprintf(cmd, sizeof(cmd), "cmd.exe /c %s", line);
        if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            char err[256];
            _snprintf(err, sizeof(err), "exec failed (%lu): %s\r\n",
                      GetLastError(), line);
            tx_write(err);
            CloseHandle(hWr);
            CloseHandle(hNul);
            CloseHandle(hRd);
            return;
        }
    }

    CloseHandle(hWr);
    CloseHandle(hNul);

    /* Drain pipe and wait for process concurrently.
     * Use a thread to drain stdout so the pipe doesn't fill up
     * and block the child, while we wait on the process handle. */
    {
        HANDLE hDrain = CreateThread(NULL, 0,
            (LPTHREAD_START_ROUTINE)pipe_output, hRd, 0, NULL);

        if (WaitForSingleObject(pi.hProcess, 10000) == WAIT_TIMEOUT) {
            tx_write("\r\n[timeout - killed]\r\n");
            TerminateProcess(pi.hProcess, 1);
        }
        /* Process exited (or killed) — pipe write end is now closed,
         * so pipe_output's ReadFile will return FALSE and the thread exits. */
        if (hDrain) {
            WaitForSingleObject(hDrain, 3000);
            CloseHandle(hDrain);
        }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hRd);
}

/* Handle shell builtins. Returns TRUE if handled. */
static BOOL handle_builtin(const char *line)
{
    if (_strnicmp(line, "echo ", 5) == 0) {
        tx_write(line + 5);
        tx_write("\r\n");
        return TRUE;
    }
    if (_stricmp(line, "echo") == 0) {
        tx_write("\r\n");
        return TRUE;
    }
    if (_strnicmp(line, "cd ", 3) == 0) {
        if (!SetCurrentDirectoryA(line + 3)) {
            char err[256];
            _snprintf(err, sizeof(err), "cd failed (%lu)\r\n", GetLastError());
            tx_write(err);
        }
        return TRUE;
    }
    if (_stricmp(line, "cd") == 0 || _stricmp(line, "pwd") == 0) {
        char cwd[MAX_PATH];
        if (GetCurrentDirectoryA(sizeof(cwd), cwd)) {
            tx_write(cwd);
            tx_write("\r\n");
        }
        return TRUE;
    }
    if (_strnicmp(line, "set ", 4) == 0) {
        char *eq = strchr(line + 4, '=');
        if (eq) {
            *eq = '\0';
            SetEnvironmentVariableA(line + 4, eq + 1);
            *eq = '=';
        }
        return TRUE;
    }
    return FALSE;
}

static void repl_loop(void)
{
    char line[512];
    tx_write("\r\n=== ReactOS Serial Shell ===\r\n");

    for (;;) {
        tx_write("$ ");
        int n = repl_readline(line, sizeof(line));
        if (n == 0)
            continue;
        if (_stricmp(line, "exit") == 0 || _stricmp(line, "quit") == 0)
            break;
        if (!handle_builtin(line))
            run_cmd(line);
    }
    tx_write("Bye!\r\n");
}

/* ---- Transport init ---- */

static BOOL init_com2(void)
{
    HANDLE h = CreateFileA("\\\\.\\COM2",
                           GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;

    DCB dcb;
    ZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    GetCommState(h, &dcb);
    dcb.BaudRate = CBR_115200;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity   = NOPARITY;
    dcb.fBinary  = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    SetCommState(h, &dcb);

    COMMTIMEOUTS to;
    ZeroMemory(&to, sizeof(to));
    to.ReadIntervalTimeout        = 100;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant   = 500;
    SetCommTimeouts(h, &to);

    hIn = hOut = h;
    return TRUE;
}

static void init_stdio(void)
{
    hIn  = GetStdHandle(STD_INPUT_HANDLE);
    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--stdio") == 0) {
        init_stdio();
    } else if (!init_com2()) {
        OutputDebugStringA("serial-shell: Failed to open COM2, using stdio\n");
        init_stdio();
    } else {
        OutputDebugStringA("serial-shell: ready on COM2\n");
    }

    repl_loop();

    if (hIn == hOut && hIn != INVALID_HANDLE_VALUE)
        CloseHandle(hIn);
    return 0;
}
