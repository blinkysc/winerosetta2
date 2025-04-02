#include <windows.h>
#include <cstdint>
#include <tlhelp32.h>

// Target problematic instructions
constexpr uint16_t ARPL_OPCODE = 0xD063;
constexpr uint16_t FCOMP_OPCODE = 0xD8DC;
constexpr uint16_t FCOMP_ST0_OPCODE = 0xD8D8;
constexpr uint16_t NOP_2BYTES = 0x9090;

// For ZF flag
constexpr DWORD ZF_FLAG = 0x40;
constexpr DWORD RPL_MASK = 0x3;

// Global binary translator state
struct {
    // Memory protection hook data
    PVOID vehHandler;
    
    // Patch statistics
    volatile LONG patchesApplied;
    volatile LONG arplFixed;
    volatile LONG fcompFixed;
    
    // Process info
    HANDLE hProcess;
    HANDLE hThread;
    DWORD processId;
} g_state = {nullptr, 0, 0, 0, NULL, NULL, 0};

// Interrupt hook handler to intercept illegal instructions
LONG WINAPI VectoredHandler(EXCEPTION_POINTERS* ExceptionInfo) {
    // Only handle illegal instruction exceptions
    if (ExceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    
    ULONG_PTR faultAddr = reinterpret_cast<ULONG_PTR>(ExceptionInfo->ExceptionRecord->ExceptionAddress);
    
    // Check if it's a valid memory location
    if (!IsBadReadPtr(reinterpret_cast<void*>(faultAddr), 2)) {
        uint16_t opcode = *reinterpret_cast<uint16_t*>(faultAddr);
        
        // Handle ARPL
        if (opcode == ARPL_OPCODE) {
            // Call our ARPL emulation
            auto dest = &ExceptionInfo->ContextRecord->Eax;
            auto src = &ExceptionInfo->ContextRecord->Edx;
            
            uint16_t destVal = static_cast<uint16_t>(*dest);
            uint16_t srcVal = static_cast<uint16_t>(*src);
            
            if ((destVal & RPL_MASK) < (srcVal & RPL_MASK)) {
                // Set ZF
                ExceptionInfo->ContextRecord->EFlags |= ZF_FLAG;
                
                // Update destination
                destVal = (destVal & ~RPL_MASK) | (srcVal & RPL_MASK);
                *dest = (*dest & 0xFFFF0000) | destVal;
            } else {
                // Clear ZF
                ExceptionInfo->ContextRecord->EFlags &= ~ZF_FLAG;
            }
            
            // Skip the instruction
            ExceptionInfo->ContextRecord->Eip += 2;
            
            // Attempt to patch with NOPs for next time
            DWORD oldProtect;
            if (VirtualProtect(reinterpret_cast<void*>(faultAddr), 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                *reinterpret_cast<uint16_t*>(faultAddr) = NOP_2BYTES;
                VirtualProtect(reinterpret_cast<void*>(faultAddr), 2, oldProtect, &oldProtect);
                InterlockedIncrement(&g_state.arplFixed);
            }
            
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        
        // Handle FCOMP
        if (opcode == FCOMP_OPCODE) {
            // Replace with FCOMP ST0
            DWORD oldProtect;
            if (VirtualProtect(reinterpret_cast<void*>(faultAddr), 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                *reinterpret_cast<uint16_t*>(faultAddr) = FCOMP_ST0_OPCODE;
                VirtualProtect(reinterpret_cast<void*>(faultAddr), 2, oldProtect, &oldProtect);
                InterlockedIncrement(&g_state.fcompFixed);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    
    return EXCEPTION_CONTINUE_SEARCH;
}

// Optimize a memory block
void OptimizeMemoryBlock(void* baseAddr, SIZE_T size) {
    // Can't optimize NULL or tiny blocks
    if (!baseAddr || size < 2) {
        return;
    }
    
    // Skip non-executable memory
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(baseAddr, &mbi, sizeof(mbi)) == 0) {
        return;
    }
    
    if (!(mbi.Protect & PAGE_EXECUTE) && 
        !(mbi.Protect & PAGE_EXECUTE_READ) && 
        !(mbi.Protect & PAGE_EXECUTE_READWRITE) && 
        !(mbi.Protect & PAGE_EXECUTE_WRITECOPY)) {
        return;
    }
    
    // Make memory temporarily writable
    DWORD oldProtect;
    if (!VirtualProtect(baseAddr, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return;
    }
    
    // Scan for and fix problematic instructions
    uint8_t* start = static_cast<uint8_t*>(baseAddr);
    uint8_t* end = start + size - 1;  // Need at least 2 bytes for 16-bit opcodes
    
    for (uint8_t* p = start; p < end; p++) {
        uint16_t opcode = *reinterpret_cast<uint16_t*>(p);
        
        // Fix ARPL instructions by replacing with NOPs
        if (opcode == ARPL_OPCODE) {
            *reinterpret_cast<uint16_t*>(p) = NOP_2BYTES;
            InterlockedIncrement(&g_state.arplFixed);
            InterlockedIncrement(&g_state.patchesApplied);
        }
        // Fix FCOMP instruction
        else if (opcode == FCOMP_OPCODE) {
            *reinterpret_cast<uint16_t*>(p) = FCOMP_ST0_OPCODE;
            InterlockedIncrement(&g_state.fcompFixed);
            InterlockedIncrement(&g_state.patchesApplied);
        }
    }
    
    // Restore original protection
    VirtualProtect(baseAddr, size, oldProtect, &oldProtect);
    
    // Ensure CPU sees the changes
    FlushInstructionCache(GetCurrentProcess(), baseAddr, size);
}

// Worker thread for optimizing memory in the current process
DWORD WINAPI OptimizeThread(LPVOID param) {
    // Get a snapshot of all modules
    HANDLE hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (hModuleSnap == INVALID_HANDLE_VALUE) {
        return 1;
    }
    
    // Prepare module entry
    MODULEENTRY32 me32;
    me32.dwSize = sizeof(MODULEENTRY32);
    
    // Iterate through modules
    if (Module32First(hModuleSnap, &me32)) {
        do {
            // Optimize each module's code section
            OptimizeMemoryBlock(me32.modBaseAddr, me32.modBaseSize);
        } while (Module32Next(hModuleSnap, &me32));
    }
    
    // Clean up
    CloseHandle(hModuleSnap);
    
    return 0;
}

// Function to set up the exception handler for the child process
void SetupChildProcess() {
    // Install VEH handler for runtime exceptions
    g_state.vehHandler = AddVectoredExceptionHandler(1, VectoredHandler);
    
    // Start a thread to scan and optimize all code
    HANDLE hThread = CreateThread(NULL, 0, OptimizeThread, NULL, 0, NULL);
    if (hThread) {
        // Set to high priority for faster startup optimization
        SetThreadPriority(hThread, THREAD_PRIORITY_HIGHEST);
        CloseHandle(hThread);
    }
}

// Creates and sets up a new process from inside the current process
BOOL CreateAndSetupProcess(const char* exePath) {
    // Create process
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {0};
    
    // Create process with our process as the parent
    if (!CreateProcessA(exePath, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, 
                       NULL, NULL, &si, &pi)) {
        char msg[256];
        wsprintfA(msg, "Failed to create process: %s (Error %d)", exePath, GetLastError());
        MessageBoxA(NULL, msg, "WineRosetta Error", MB_OK | MB_ICONERROR);
        return FALSE;
    }
    
    // Store the process information
    g_state.hProcess = pi.hProcess;
    g_state.hThread = pi.hThread;
    g_state.processId = pi.dwProcessId;
    
    // Resume the process now
    ResumeThread(pi.hThread);
    
    return TRUE;
}

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Default target
    const char* exePath = ".\\wow.exe";
    
    // Use command line if provided
    if (lpCmdLine && *lpCmdLine) {
        exePath = lpCmdLine;
    }
    
    // Show startup message
    char msg[512];
    wsprintfA(msg, "WineRosetta is starting %s\n\nThis will apply compatibility patches for problematic x86 instructions.", exePath);
    MessageBoxA(NULL, msg, "WineRosetta", MB_OK | MB_ICONINFORMATION);
    
    // Set up our current process to handle exceptions
    SetupChildProcess();
    
    // Create and set up the target process
    if (!CreateAndSetupProcess(exePath)) {
        return 1;
    }
    
    // Wait for the child process to exit
    if (g_state.hProcess) {
        WaitForSingleObject(g_state.hProcess, INFINITE);
        
        // Get exit code
        DWORD exitCode = 0;
        GetExitCodeProcess(g_state.hProcess, &exitCode);
        
        // Display statistics
        wsprintfA(msg, "Process exited with code %d\n\nPatches applied: %d\nARPL instructions fixed: %d\nFCOMP instructions fixed: %d", 
                 exitCode, g_state.patchesApplied, g_state.arplFixed, g_state.fcompFixed);
        MessageBoxA(NULL, msg, "WineRosetta", MB_OK | MB_ICONINFORMATION);
        
        // Clean up
        CloseHandle(g_state.hProcess);
        CloseHandle(g_state.hThread);
    }
    
    // Remove exception handler
    if (g_state.vehHandler) {
        RemoveVectoredExceptionHandler(g_state.vehHandler);
    }
    
    return 0;
}
