/*
 * DISCLAIMER: This is an experimental implementation that DOES NOT WORK.
 * It was created to test the feasibility of emulating problematic x86 instructions in Wine
 * on Apple Silicon, but the application simply crashes when using this emulator,
 * exhibiting the same behavior as if not using the JIT at all.
 * This should be considered a failed experiment and is provided for reference only.
 */

#include <windows.h>
#include <cstdint>
#include <cstdio>      // For sprintf_s (safer alternative to wsprintfA)
#include <tlhelp32.h>
#include <winnt.h>    // For CONTEXT structure definitions
#include <atomic>     // For std::atomic

// --- Constants ---
// Opcodes are viewed as little-endian words (byte sequence in memory)
// ARPL AX, AX (Bytes: 63 D0)
constexpr uint16_t ARPL_OPCODE_CHECK = 0xD063; // WORD representation of 63 D0

// FCOMP ST(1) (Bytes: D8 DC)
constexpr uint16_t FCOMP_CHECK_OPCODE = 0xDCD8; // WORD representation of D8 DC

// FCOMP ST(0) (Bytes: D8 D8) - Patch target for FCOMP ST(1)
constexpr uint16_t FCOMP_ST0_OPCODE   = 0xD8D8; // WORD representation of D8 D8

// Two NOP instructions (Bytes: 90 90) - Patch target for ARPL
constexpr uint16_t NOP_2BYTES         = 0x9090; // WORD representation of 90 90

// For ZF flag in EFlags (bit 6)
constexpr DWORD EFLAGS_ZF_FLAG = 0x40;
// For RPL mask (bits 0-1) - Note: Not used in current patching logic, kept for context
constexpr uint16_t SELECTOR_RPL_MASK = 0x3;

// Global state
struct WineRosettaState {
    PVOID vehHandler = nullptr;
    std::atomic<LONG> patchesApplied{0}; // Use {} initialization
    std::atomic<LONG> arplFixed{0};      // Use {} initialization
    std::atomic<LONG> fcompFixed{0};     // Use {} initialization
    HANDLE hProcess = NULL;
    HANDLE hThread = NULL;
    DWORD processId = 0;
} g_state; // Use std::atomic for thread safety

// --- Function Prototypes ---
LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ExceptionInfo);
void OptimizeMemoryBlock(void* baseAddr, SIZE_T size);
DWORD WINAPI OptimizeThread(LPVOID param);
void SetupExceptionHandlerAndOptimizer();
BOOL CreateAndSetupProcess(const char* exePath);

// --- Implementation ---

// Vectored Exception Handler to catch and patch instructions at runtime
LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ExceptionInfo) {
    // Only handle illegal instruction exceptions
    if (ExceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH; // Pass on other exceptions
    }

    // Get faulting address and context (assuming 32-bit target)
    PCONTEXT pContext = ExceptionInfo->ContextRecord;
    ULONG_PTR faultAddr = static_cast<ULONG_PTR>(pContext->Eip); // Use Eip for 32-bit

    // Check if the address is readable (at least 2 bytes for the opcode)
    // Using VirtualQuery is generally preferred over IsBadReadPtr
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<LPCVOID>(faultAddr), &mbi, sizeof(mbi)) == 0 ||
        !(mbi.State & MEM_COMMIT) || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) {
         // Cannot read or not committed memory
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Ensure we can read at least 2 bytes within the allocated region
    if (faultAddr + 1 >= reinterpret_cast<ULONG_PTR>(mbi.BaseAddress) + mbi.RegionSize) {
        // Trying to read past the end of the region
        return EXCEPTION_CONTINUE_SEARCH;
    }


    uint16_t opcode = *reinterpret_cast<uint16_t*>(faultAddr);
    bool patched = false;
    DWORD oldProtect;

    // --- Handle ARPL (Specific form ARPL AX, AX -> Bytes 63 D0) ---
    if (opcode == ARPL_OPCODE_CHECK) {
        // Attempt to make the memory writable
        if (VirtualProtect(reinterpret_cast<void*>(faultAddr), 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *reinterpret_cast<uint16_t*>(faultAddr) = NOP_2BYTES; // Patch with NOPs
            // Restore original protection
            DWORD ignored; // Variable to receive the old protection value on the second call
            VirtualProtect(reinterpret_cast<void*>(faultAddr), 2, oldProtect, &ignored);
            // Flush instruction cache for the patched memory
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(faultAddr), 2);

            g_state.arplFixed++; // Increment atomic counter
            patched = true;

             // Since we NOP'd the instruction, advance EIP past the 2 bytes
            pContext->Eip += 2;
        }
    }
    // --- Handle FCOMP ST(1) (Bytes D8 DC) ---
    else if (opcode == FCOMP_CHECK_OPCODE) {
         // Attempt to make the memory writable
        if (VirtualProtect(reinterpret_cast<void*>(faultAddr), 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *reinterpret_cast<uint16_t*>(faultAddr) = FCOMP_ST0_OPCODE; // Patch with FCOMP ST(0)
             // Restore original protection
            DWORD ignored;
            VirtualProtect(reinterpret_cast<void*>(faultAddr), 2, oldProtect, &ignored);
             // Flush instruction cache
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(faultAddr), 2);

            g_state.fcompFixed++; // Increment atomic counter
            patched = true;

            // EIP does NOT need to be advanced here, because we replaced the
            // instruction in place. The CPU will execute the *new* instruction
            // at the same address upon continuation.
        }
    }

    if (patched) {
        g_state.patchesApplied++;
        return EXCEPTION_CONTINUE_EXECUTION; // Resume execution at (potentially advanced) Eip
    } else {
        // We didn't handle or couldn't patch this specific illegal instruction
        return EXCEPTION_CONTINUE_SEARCH; // Let other handlers or the OS deal with it
    }
}


// Scans and patches a block of memory proactively
void OptimizeMemoryBlock(void* baseAddr, SIZE_T size) {
    if (!baseAddr || size < 2) {
        return;
    }

    // Check if memory is executable
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(baseAddr, &mbi, sizeof(mbi)) == 0) {
        return; // Failed to query memory info
    }

    // Check for execute flags (more robustly)
    bool isExecutable = (mbi.Protect & PAGE_EXECUTE) ||
                        (mbi.Protect & PAGE_EXECUTE_READ) ||
                        (mbi.Protect & PAGE_EXECUTE_READWRITE) ||
                        (mbi.Protect & PAGE_EXECUTE_WRITECOPY);

    if (!isExecutable || !(mbi.State & MEM_COMMIT) || (mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) {
        return; // Not committed, executable or accessible
    }

    // Make memory temporarily writable
    DWORD oldProtect;
    // Check if already writable, otherwise try to make it writable
    bool needsRestore = !(mbi.Protect & PAGE_EXECUTE_READWRITE || mbi.Protect & PAGE_READWRITE || mbi.Protect & PAGE_WRITECOPY);
    bool canWrite = !needsRestore;

    if (needsRestore) {
        if (!VirtualProtect(baseAddr, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            // Couldn't make writable, skip.
            return;
        }
        canWrite = true;
    } else {
        oldProtect = mbi.Protect; // Store current protection if already writable
    }


    if (!canWrite) return; // Should not happen, but safety check


    uint8_t* start = static_cast<uint8_t*>(baseAddr);
    // Calculate end pointer carefully to avoid going out of bounds
    uint8_t* region_end = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    uint8_t* effective_end = start + size;
    uint8_t* limit = (effective_end < region_end) ? effective_end : region_end;

    // Ensure we don't read past the actual memory region limit by 1 byte for the opcode word
    if (limit > start) {
         limit--;
    } else {
        // Region is too small (0 or 1 byte)
         if (needsRestore) VirtualProtect(baseAddr, size, oldProtect, &oldProtect);
         return;
    }


    bool changed = false;

    for (uint8_t* p = start; p < limit; /* increment in loop */) {
        // Read potential 16-bit opcode carefully
        uint16_t opcode = *reinterpret_cast<uint16_t*>(p);
        bool instruction_patched = false;

        // Check for ARPL (Specific form 63 D0 -> ARPL AX, AX)
        if (opcode == ARPL_OPCODE_CHECK) {
            *reinterpret_cast<uint16_t*>(p) = NOP_2BYTES;
            g_state.arplFixed++;
            g_state.patchesApplied++;
            changed = true;
            instruction_patched = true;
            p += 2; // Advance by 2 bytes
        }
        // Check for FCOMP ST(1) (D8 DC)
        else if (opcode == FCOMP_CHECK_OPCODE) {
            *reinterpret_cast<uint16_t*>(p) = FCOMP_ST0_OPCODE; // Patch to FCOMP ST(0)
            g_state.fcompFixed++;
            g_state.patchesApplied++;
            changed = true;
            instruction_patched = true;
            p += 2; // Advance by 2 bytes
        }

        // If we didn't patch, advance by 1 byte to check next position
        if (!instruction_patched) {
            p++;
        }
    }

    // Restore original protection only if we changed it
    if (needsRestore) {
        DWORD ignored;
        VirtualProtect(baseAddr, size, oldProtect, &ignored);
    }

    // Flush cache if we made changes
    if (changed) {
        FlushInstructionCache(GetCurrentProcess(), baseAddr, size);
    }
}

// Worker thread for optimizing memory in the current process
DWORD WINAPI OptimizeThread(LPVOID param) {
    // Get snapshot of modules for the *current* process
    HANDLE hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (hModuleSnap == INVALID_HANDLE_VALUE) {
        return 1; // Failed to get snapshot
    }

    MODULEENTRY32 me32;
    me32.dwSize = sizeof(MODULEENTRY32);

    if (Module32First(hModuleSnap, &me32)) {
        do {
            // Optimize the code section of each loaded module
            OptimizeMemoryBlock(me32.modBaseAddr, me32.modBaseSize);
        } while (Module32Next(hModuleSnap, &me32));
    }

    CloseHandle(hModuleSnap);
    return 0;
}

// Sets up VEH and starts optimization thread *in the current process*
void SetupExceptionHandlerAndOptimizer() {
    // Install VEH handler (first chance handler)
    g_state.vehHandler = AddVectoredExceptionHandler(1, VectoredHandler);
    if (!g_state.vehHandler) {
        MessageBoxA(NULL, "Failed to install Vectored Exception Handler!", "WineRosetta Error", MB_OK | MB_ICONERROR);
    }

    // Start background optimization thread
    HANDLE hThread = CreateThread(NULL, 0, OptimizeThread, NULL, 0, NULL);
    if (hThread) {
        CloseHandle(hThread); // Close handle, thread continues running
    } else {
         MessageBoxA(NULL, "Failed to create optimization thread!", "WineRosetta Warning", MB_OK | MB_ICONWARNING);
    }
}

// Creates the target process (suspended), potentially sets it up, and resumes
BOOL CreateAndSetupProcess(const char* exePath) {
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };

    // Create a mutable buffer for the command line argument
    char cmdLine[MAX_PATH * 2]; // Allocate sufficient buffer
    strncpy_s(cmdLine, sizeof(cmdLine), exePath, _TRUNCATE);


    if (!CreateProcessA(
            NULL,          // Application name (use lpCommandLine instead)
            cmdLine,       // Command line (needs to be mutable)
            NULL,          // Process handle not inheritable
            NULL,          // Thread handle not inheritable
            FALSE,         // Set handle inheritance to FALSE
            CREATE_SUSPENDED, // Create suspended
            NULL,          // Use parent's environment block
            NULL,          // Use parent's starting directory
            &si,           // Pointer to STARTUPINFO structure
            &pi            // Pointer to PROCESS_INFORMATION structure
           ))
    {
        char msg[256];
        sprintf_s(msg, sizeof(msg), "Failed to create process: %s (Error %lu)", exePath, GetLastError());
        MessageBoxA(NULL, msg, "WineRosetta Error", MB_OK | MB_ICONERROR);
        return FALSE;
    }

    // Store process information globally
    g_state.hProcess = pi.hProcess;
    g_state.hThread = pi.hThread;
    g_state.processId = pi.dwProcessId;


    // Resume the main thread of the new process
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        char msg[256];
        sprintf_s(msg, sizeof(msg), "Failed to resume process thread (Error %lu)", GetLastError());
        MessageBoxA(NULL, msg, "WineRosetta Error", MB_OK | MB_ICONERROR);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        g_state.hProcess = NULL;
        g_state.hThread = NULL;
        return FALSE;
    }

    // Don't close pi.hProcess/hThread here, we need them later for WaitForSingleObject etc.
    // They will be closed at the end of WinMain.

    return TRUE;
}

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Determine target executable path
    const char* exePathCStr = ".\\wow.exe"; // Default target
    char actualExePath[MAX_PATH]; // Buffer to hold the final path

    if (lpCmdLine && *lpCmdLine) {
        // Handle potential quotes around the path
        if (lpCmdLine[0] == '"') {
            strncpy_s(actualExePath, sizeof(actualExePath), lpCmdLine + 1, _TRUNCATE);
            char* lastQuote = strrchr(actualExePath, '"');
            if (lastQuote) {
                *lastQuote = '\0'; // Null-terminate at the closing quote
            }
        } else {
            strncpy_s(actualExePath, sizeof(actualExePath), lpCmdLine, _TRUNCATE);
        }
        exePathCStr = actualExePath; // Point to the processed path
    } else {
         strncpy_s(actualExePath, sizeof(actualExePath), exePathCStr, _TRUNCATE);
    }


    char msg[512];
    sprintf_s(msg, sizeof(msg), "WineRosetta starting target:\n%s\n\nCompatibility patches for ARPL/FCOMP will be applied.", exePathCStr);
    MessageBoxA(NULL, msg, "WineRosetta", MB_OK | MB_ICONINFORMATION);

    // Set up exception handling and optimization for THIS process (the launcher)
    // The VEH *might* be inherited by the child depending on circumstances.
    SetupExceptionHandlerAndOptimizer();

    // Create and start the target process
    if (!CreateAndSetupProcess(exePathCStr)) {
        if (g_state.vehHandler) {
            RemoveVectoredExceptionHandler(g_state.vehHandler);
        }
        return 1;
    }

    // Wait for the target process to finish
    if (g_state.hProcess) {
        WaitForSingleObject(g_state.hProcess, INFINITE);

        DWORD exitCode = 0;
        GetExitCodeProcess(g_state.hProcess, &exitCode);

        // Display statistics (using atomic loads)
        sprintf_s(msg, sizeof(msg),
                  "Process exited with code %lu\n\n"
                  "Total patches (Runtime + Proactive): %ld\n"
                  "ARPL instructions fixed: %ld\n"
                  "FCOMP instructions fixed: %ld",
                  exitCode,
                  g_state.patchesApplied.load(std::memory_order_relaxed), // Use relaxed for final report
                  g_state.arplFixed.load(std::memory_order_relaxed),
                  g_state.fcompFixed.load(std::memory_order_relaxed));
        MessageBoxA(NULL, msg, "WineRosetta - Process Finished", MB_OK | MB_ICONINFORMATION);

        // Clean up process handles now that we're done with them
        CloseHandle(g_state.hProcess);
        CloseHandle(g_state.hThread);
        g_state.hProcess = NULL; // Nullify handles after closing
        g_state.hThread = NULL;
    }

    // Remove the exception handler before exiting the launcher
    if (g_state.vehHandler) {
        RemoveVectoredExceptionHandler(g_state.vehHandler);
        g_state.vehHandler = nullptr; // Nullify handle after removing
    }

    return 0;
}
