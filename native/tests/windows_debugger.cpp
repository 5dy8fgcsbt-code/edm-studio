// Development-only Windows debugger. Captures a worker's native exception
// before Windows Error Reporting can display a dialog; no external debugger DLL.
#include <windows.h>
#include <dbghelp.h>
#include <iostream>
#include <string>
#include <vector>
int wmain(int argc, wchar_t** argv) {
    if (argc < 2)
        return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::wstring command;
    for (int i = 1; i < argc; i++) {
        if (i > 1)
            command += L" ";
        command += L"\"" + std::wstring(argv[i]) + L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                        DEBUG_ONLY_THIS_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &pi)) {
        std::cerr << "CreateProcess failed " << GetLastError() << "\n";
        return 2;
    }
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    bool symbols = false, done = false, crashed = false;
    DWORD code = 0;
    while (!done) {
        DEBUG_EVENT e{};
        if (!WaitForDebugEvent(&e, 15000)) {
            std::cerr << "Debug timeout\n";
            TerminateProcess(pi.hProcess, 2);
            break;
        }
        DWORD status = DBG_CONTINUE;
        if (e.dwDebugEventCode == CREATE_PROCESS_DEBUG_EVENT) {
            symbols = SymInitializeW(pi.hProcess, nullptr, FALSE);
            SymLoadModuleExW(pi.hProcess, e.u.CreateProcessInfo.hFile, argv[1], nullptr,
                             reinterpret_cast<DWORD64>(e.u.CreateProcessInfo.lpBaseOfImage), 0, nullptr, 0);
            std::cerr << "Image base " << e.u.CreateProcessInfo.lpBaseOfImage << " symbols=" << symbols
                      << "\n";
            if (e.u.CreateProcessInfo.hFile)
                CloseHandle(e.u.CreateProcessInfo.hFile);
        }
        if (e.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT) {
            SymLoadModuleExW(pi.hProcess, e.u.LoadDll.hFile, nullptr, nullptr,
                             reinterpret_cast<DWORD64>(e.u.LoadDll.lpBaseOfDll), 0, nullptr, 0);
            if (e.u.LoadDll.hFile)
                CloseHandle(e.u.LoadDll.hFile);
        }
        if (e.dwDebugEventCode == EXCEPTION_DEBUG_EVENT) {
            auto& x = e.u.Exception;
            DWORD exception = x.ExceptionRecord.ExceptionCode;
            if (exception == EXCEPTION_BREAKPOINT)
                status = DBG_CONTINUE;
            else {
                status = DBG_EXCEPTION_NOT_HANDLED;
                if (exception == EXCEPTION_ACCESS_VIOLATION || !x.dwFirstChance) {
                    crashed = true;
                    std::cerr << "Exception 0x" << std::hex << exception << " at "
                              << x.ExceptionRecord.ExceptionAddress << " first=" << x.dwFirstChance << "\n";
                    HANDLE thread = OpenThread(THREAD_ALL_ACCESS, FALSE, e.dwThreadId);
                    CONTEXT context{};
                    context.ContextFlags = CONTEXT_FULL;
                    GetThreadContext(thread, &context);
                    STACKFRAME64 frame{};
                    frame.AddrPC.Offset = context.Rip;
                    frame.AddrPC.Mode = AddrModeFlat;
                    frame.AddrStack.Offset = context.Rsp;
                    frame.AddrStack.Mode = AddrModeFlat;
                    frame.AddrFrame.Offset = context.Rbp;
                    frame.AddrFrame.Mode = AddrModeFlat;
                    for (int depth = 0; depth < 40; depth++) {
                        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 1024]{};
                        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
                        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                        symbol->MaxNameLen = 1024;
                        DWORD64 displacement = 0;
                        std::cerr << "  0x" << std::hex << frame.AddrPC.Offset;
                        if (symbols && SymFromAddr(pi.hProcess, frame.AddrPC.Offset, &displacement, symbol))
                            std::cerr << " " << symbol->Name << " +0x" << displacement;
                        IMAGEHLP_LINE64 line{};
                        line.SizeOfStruct = sizeof(line);
                        DWORD lineDisplacement = 0;
                        if (SymGetLineFromAddr64(pi.hProcess, frame.AddrPC.Offset, &lineDisplacement, &line))
                            std::cerr << " " << line.FileName << ":" << std::dec << line.LineNumber;
                        std::cerr << "\n";
                        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, pi.hProcess, thread, &frame, &context,
                                         nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                            !frame.AddrPC.Offset)
                            break;
                    }
                    CloseHandle(thread);
                    TerminateProcess(pi.hProcess, exception);
                    status = DBG_CONTINUE;
                }
            }
        }
        if (e.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT) {
            code = e.u.ExitProcess.dwExitCode;
            done = true;
            std::cerr << "Exit 0x" << std::hex << code << "\n";
        }
        ContinueDebugEvent(e.dwProcessId, e.dwThreadId, status);
    }
    if (symbols)
        SymCleanup(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return crashed ? 3 : int(code);
}
