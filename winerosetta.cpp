#include <windows.h>
#include <cstdint>
#include <cstdio>

// Target opcodes
constexpr uint16_t ARPL_OPCODE = 0xD063;
constexpr uint16_t FCOMP_OPCODE = 0xD8DC;
constexpr uint16_t FCOMP_ST0_OPCODE = 0xD8D8;

// For ZF flag
constexpr DWORD ZF_FLAG = 0x40;
constexpr DWORD RPL_MASK = 0x3;

// Statistics for debugging
struct PatchStats {
    volatile LONG totalExceptions;
    volatile LONG arplHandled;
    volatile LONG fcompHandled;
    volatile LONG patchesMade;
} g_stats = {0};

// Function to safely patch memory - no exception handling, just direct approach
bool PatchInstruction(void* address, uint16_t newOpcode) {
    DWORD oldProtect;
    
    // Try to change memory protection
    if (!VirtualProtect(address, 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    
    // Write the new opcode
    *reinterpret_cast<uint16_t*>(address) = newOpcode;
    
    // Restore protection
    VirtualProtect(address, 2, oldProtect, &oldProtect);
    
    // Flush instruction cache to ensure the CPU sees the change
    FlushInstructionCache(GetCurrentProcess(), address, 2);
    
    // Record successful patch
    InterlockedIncrement(&g_stats.patchesMade);
    
    return true;
}

// Check if a memory address is valid and accessible
bool IsAddressAccessible(void* address, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    
    // Check if memory is committed and accessible
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    
    // Check if it's readable
    if ((mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD)) {
        return false;
    }
    
    // Check if memory block is large enough
    uintptr_t blockEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    uintptr_t requestedEnd = reinterpret_cast<uintptr_t>(address) + size;
    if (requestedEnd > blockEnd) {
        return false;
    }
    
    return true;
}

// Helper function to safely read memory
bool SafeReadMemory(void* address, void* buffer, size_t size) {
    if (!IsAddressAccessible(address, size)) {
        return false;
    }
    
    memcpy(buffer, address, size);
    return true;
}

// Our primary vectored exception handler
LONG WINAPI VectoredHandler(struct _EXCEPTION_POINTERS* ExceptionInfo) {
    // Count all exceptions for statistics
    InterlockedIncrement(&g_stats.totalExceptions);
    
    // Only handle illegal instruction exceptions
    if (ExceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    
    // Get the context and address
    auto context = ExceptionInfo->ContextRecord;
    void* instructionAddress = ExceptionInfo->ExceptionRecord->ExceptionAddress;
    
    // Safely read the opcode
    uint16_t opcode = 0;
    if (!SafeReadMemory(instructionAddress, &opcode, sizeof(opcode))) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    
    // Handle ARPL instruction
    if (opcode == ARPL_OPCODE) {
        InterlockedIncrement(&g_stats.arplHandled);
        
        // Extract operands
        uint16_t dest = static_cast<uint16_t>(context->Eax);
        uint16_t src = static_cast<uint16_t>(context->Edx);
        
        // Implement ARPL logic
        /*
        IF RPL bits(0,1) of DEST < RPL bits(0,1) of SRC
        THEN
            ZF := 1;
            RPL bits(0,1) of DEST := RPL bits(0,1) of SRC;
        ELSE
            ZF := 0;
        FI;
        */
        if ((dest & RPL_MASK) < (src & RPL_MASK)) {
            context->EFlags |= ZF_FLAG;  // Set ZF
            dest = (dest & ~RPL_MASK) | (src & RPL_MASK);
            context->Eax = (context->Eax & 0xFFFF0000) | dest;
        } else {
            context->EFlags &= ~ZF_FLAG; // Clear ZF
        }
        
        // Skip the instruction
        context->Eip += 2;
        
        // Try to patch this location for next time, but don't worry if it fails
        PatchInstruction(instructionAddress, 0x9090); // NOP, NOP
        
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    
    // Handle FCOMP instruction
    if (opcode == FCOMP_OPCODE) {
        InterlockedIncrement(&g_stats.fcompHandled);
        
        // Try to patch the instruction in-place
        if (PatchInstruction(instructionAddress, FCOMP_ST0_OPCODE)) {
            // Successfully patched, let CPU re-execute
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        
        // If patching failed, continue search for another handler
        return EXCEPTION_CONTINUE_SEARCH;
    }
    
    // Not our instruction
    return EXCEPTION_CONTINUE_SEARCH;
}

// Optional debug function to print to a log file
void WriteDebugLog(const char* format, ...) {
#ifdef DEBUG_OUTPUT
    FILE* f = fopen("winerosetta_debug.log", "a");
    if (f) {
        va_list args;
        va_start(args, format);
        vfprintf(f, format, args);
        va_end(args);
        fclose(f);
    }
#endif
}

BOOL WINAPI DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    static PVOID exceptionHandler = NULL;
    
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            // Disable thread attach/detach notifications
            DisableThreadLibraryCalls(hModule);
            
            // Register our exception handler with highest priority (1)
            exceptionHandler = AddVectoredExceptionHandler(1, VectoredHandler);
            
            // For debugging in development
            WriteDebugLog("WineRosetta DLL loaded, handler: %p\n", exceptionHandler);
            
            break;
        }
            
        case DLL_PROCESS_DETACH: {
            // Remove exception handler if it was registered
            if (exceptionHandler) {
                RemoveVectoredExceptionHandler(exceptionHandler);
                exceptionHandler = NULL;
                
                WriteDebugLog("WineRosetta unloaded. Stats: Total=%ld, ARPL=%ld, FCOMP=%ld, Patches=%ld\n",
                             g_stats.totalExceptions, g_stats.arplHandled, 
                             g_stats.fcompHandled, g_stats.patchesMade);
            }
            break;
        }
    }
    
    return TRUE;
}
