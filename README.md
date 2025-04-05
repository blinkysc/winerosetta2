# WineRosetta2

A lightweight JIT (Just-In-Time) binary translator specifically designed to address issues with World of Warcraft Classic versions running in Wine on Apple Silicon Macs (M1, M2, M3, M4) by handling problematic x86 instructions that cause conflicts with Rosetta 2 translation. Despite these improvements, performance remains significantly limited, with users typically experiencing around 5 FPS, making gameplay still quite challenging.

## Introduction

This implementation functions as a simple JIT binary translator that targets specific problematic instructions. While the original design solely relied on a Vectored Exception Handler to catch and handle illegal instructions at runtime, this enhanced version adds proactive patching:

1. **JIT Binary Translation**: It identifies and translates problematic instructions on-the-fly
2. **Upfront Scanning**: It pre-patches known problematic instructions during module loading to avoid unnecessary exceptions
3. **Fallback Exception Handling**: It maintains a VEH handler to catch any instructions missed by the initial scan
4. **Memory Efficient**: Targeted approach that only modifies specific problematic instructions rather than performing full code translation

This approach is minimally invasive while providing the necessary fixes to run World of Warcraft Classic on Apple Silicon Macs through Wine.

## Overview

WineRosetta2 is a dynamic binary translator that targets specific problematic x86 instructions that cause issues when running World of Warcraft Classic through Wine on Apple Silicon Macs using Rosetta 2. It primarily fixes:

- `ARPL` (Adjust RPL Field of Selector) - replaces with NOPs
- `FCOMP` instructions - replaces with compatible alternatives

These instructions can cause crashes or illegal instruction exceptions when translated by Rosetta 2 on Apple Silicon processors.

The tool works by:
1. Pre-scanning executable memory to patch known problematic instructions
2. Installing a Vectored Exception Handler (VEH) to catch and emulate instructions that weren't patched ahead of time

## Usage

### Installation and Usage

1. Place `WineRosetta2.exe` in the same directory as your `wow.exe` executable
2. Simply run `WineRosetta2.exe` (no command-line arguments are needed)

The tool will automatically locate and launch the `wow.exe` file in the same directory. It does not accept any command-line arguments.

This is particularly useful for World of Warcraft Classic versions running on Apple Silicon Macs (M1-M4) through Wine, where Rosetta 2 translation issues can cause crashes.

### As a DLL

You can also compile WineRosetta2 as a DLL and inject it into an existing process:

1. Compile with the `BUILD_AS_DLL` flag
2. Use your preferred DLL injection method

## How It Works

WineRosetta2 uses two approaches to handle problematic instructions:

1. **Proactive Optimization**: On startup, it scans all loaded modules and patches problematic instructions.
2. **Reactive Handling**: It installs a Vectored Exception Handler to catch illegal instruction exceptions and emulate them at runtime.

## Building

The project can be built as either a standalone launcher or as a DLL:

- **Launcher**: Build without the `BUILD_AS_DLL` flag
- **DLL**: Build with the `BUILD_AS_DLL` flag

### Compilation Command

Use the following command to compile the executable on a Linux system with MinGW:

```
i686-w64-mingw32-g++ -o winerosetta2.exe winerosetta2.cpp -static -static-libgcc -static-libstdc++ -std=c++11 -Wall -O2
```

This will create a statically linked 32-bit Windows executable that can be used with Wine.

## Credits

This implementation is based on the work by [Lifeisawful](https://github.com/Lifeisawful/winerosetta), with modifications and optimizations.

## License

The original WineRosetta project by [Lifeisawful](https://github.com/Lifeisawful/winerosetta) is licensed under the MIT license.

## Technical Details

### ARPL Instruction

The ARPL instruction adjusts the Requested Privilege Level (RPL) bits of a segment selector. It's rarely used in modern applications but can cause issues in Wine. WineRosetta replaces it with NOPs or emulates it when encountered.

### FCOMP Instruction

Some variants of the FCOMP floating-point comparison instruction may cause compatibility issues. WineRosetta replaces these with more compatible variants.
