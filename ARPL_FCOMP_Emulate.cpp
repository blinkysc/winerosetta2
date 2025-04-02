#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstdio> // For rename

// --- Constants ---
// Define opcodes as they appear as byte sequences in the file
// ARPL AX, AX (Bytes: 63 D0)
constexpr uint16_t ARPL_OPCODE_CHECK = 0xD063; // Check as little-endian WORD
constexpr uint8_t ARPL_BYTE_1 = 0x63;
constexpr uint8_t ARPL_BYTE_2 = 0xD0;

// FCOMP ST(1) (Bytes: D8 DC)
constexpr uint16_t FCOMP_CHECK_OPCODE = 0xDCD8; // Check as little-endian WORD
constexpr uint8_t FCOMP_BYTE_1 = 0xD8;
constexpr uint8_t FCOMP_BYTE_2 = 0xDC;

// FCOMP ST(0) (Bytes: D8 D8) - Patch target for FCOMP ST(1)
constexpr uint8_t FCOMP_ST0_BYTE_1 = 0xD8;
constexpr uint8_t FCOMP_ST0_BYTE_2 = 0xD8;

// Two NOP instructions (Bytes: 90 90) - Patch target for ARPL
constexpr uint8_t NOP_BYTE_1 = 0x90;
constexpr uint8_t NOP_BYTE_2 = 0x90;

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path_to_executable>" << std::endl;
        std::cerr << "Example: " << argv[0] << " wow.exe" << std::endl;
        std::cerr << "\nThis will create a backup (.bak) and patch the original file." << std::endl;
        return 1;
    }

    std::string input_filepath = argv[1];
    std::string backup_filepath = input_filepath + ".bak";
    std::cout << "Patching: " << input_filepath << std::endl;

    // 1. Create a backup
    // Note: On Windows, rename might fail if the backup already exists.
    // A more robust solution would check first or use CopyFile.
    // std::remove(backup_filepath.c_str()); // Try removing old backup first
    if (std::rename(input_filepath.c_str(), backup_filepath.c_str()) != 0) {
         // If rename fails (e.g., cross-device link), try copying
         std::ifstream src(input_filepath, std::ios::binary);
         if (!src) {
            std::cerr << "Error: Cannot open input file for backup: " << input_filepath << std::endl;
             return 1;
         }
         std::ofstream dst(backup_filepath, std::ios::binary);
         if (!dst) {
            std::cerr << "Error: Cannot create backup file: " << backup_filepath << std::endl;
             src.close();
             return 1;
         }
         dst << src.rdbuf();
         src.close();
         dst.close();
         std::cout << "Created backup (by copying): " << backup_filepath << std::endl;
    } else {
         std::cout << "Created backup (by renaming): " << backup_filepath << std::endl;
    }


    // 2. Open the backup file for reading
    std::ifstream infile(backup_filepath, std::ios::binary | std::ios::ate); // Open at end to get size
    if (!infile) {
        std::cerr << "Error: Cannot open backup file for reading: " << backup_filepath << std::endl;
        // Attempt to restore original filename if rename was used
        std::rename(backup_filepath.c_str(), input_filepath.c_str());
        return 1;
    }

    std::streamsize size = infile.tellg();
    infile.seekg(0, std::ios::beg); // Go back to the beginning

    // 3. Read the entire file into memory
    std::vector<uint8_t> buffer(size);
    if (!infile.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "Error: Failed to read file content into buffer." << std::endl;
        infile.close();
        std::rename(backup_filepath.c_str(), input_filepath.c_str()); // Restore original
        return 1;
    }
    infile.close(); // Close backup file

    // 4. Scan and patch the buffer
    long arpl_patches = 0;
    long fcomp_patches = 0;
    bool changed = false;

    // Iterate up to size-1 to allow reading 2 bytes
    for (size_t i = 0; (i + 1) < buffer.size(); ++i) {
        // Read potential 16-bit opcode from buffer (little-endian)
        uint16_t current_opcode = *reinterpret_cast<uint16_t*>(&buffer[i]);

        // Check for ARPL (Bytes 63 D0)
        if (current_opcode == ARPL_OPCODE_CHECK) {
            buffer[i] = NOP_BYTE_1;     // Patch first byte (63 -> 90)
            buffer[i + 1] = NOP_BYTE_2; // Patch second byte (D0 -> 90)
            arpl_patches++;
            changed = true;
            i++; // Skip the next byte since we processed two
        }
        // Check for FCOMP ST(1) (Bytes D8 DC)
        else if (current_opcode == FCOMP_CHECK_OPCODE) {
            buffer[i] = FCOMP_ST0_BYTE_1; // Patch first byte (D8 -> D8)
            buffer[i + 1] = FCOMP_ST0_BYTE_2; // Patch second byte (DC -> D8)
            fcomp_patches++;
            changed = true;
            i++; // Skip the next byte since we processed two
        }
        // No need to increment 'i' here if no match, loop does it
    }

    // 5. Write the potentially modified buffer back to the original filename
    if (changed) {
        std::ofstream outfile(input_filepath, std::ios::binary | std::ios::trunc); // Overwrite/create
        if (!outfile) {
            std::cerr << "Error: Cannot open output file for writing: " << input_filepath << std::endl;
            std::cerr << "Original file is preserved as: " << backup_filepath << std::endl;
            return 1;
        }
        if (!outfile.write(reinterpret_cast<const char*>(buffer.data()), buffer.size())) {
            std::cerr << "Error: Failed to write patched content to output file." << std::endl;
            outfile.close();
            std::cerr << "Original file is preserved as: " << backup_filepath << std::endl;
            return 1;
        }
        outfile.close(); // Ensure data is flushed
        std::cout << "Patching successful!" << std::endl;
        std::cout << "  ARPL instructions patched: " << arpl_patches << std::endl;
        std::cout << "  FCOMP instructions patched: " << fcomp_patches << std::endl;
    } else {
        std::cout << "No target instructions found. Restoring original file." << std::endl;
        // Restore original by renaming backup back
        if (std::rename(backup_filepath.c_str(), input_filepath.c_str()) != 0) {
            std::cerr << "Warning: Failed to automatically restore original file from backup." << std::endl;
            std::cerr << "Original file is still available as: " << backup_filepath << std::endl;
        } else {
            // Successfully renamed back, maybe remove the now-redundant backup? Optional.
            // std::remove(backup_filepath.c_str());
        }
    }

    return 0;
}