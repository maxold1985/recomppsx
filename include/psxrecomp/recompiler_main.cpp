#include "psxexe.h"
#include "recompiler.h"
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage:\n"
                  << "  r3000a_recomp <PS-X EXE> <output.cpp>\n"
                  << "  r3000a_recomp <PS-X EXE> --split <output_dir> [words_per_part]\n";
        return 1;
    }
    try {
        auto exe = load_psx_exe(argv[1]);
        if (std::string(argv[2]) == "--split") {
            if (argc < 4) { std::cerr << "missing output directory\n"; return 1; }
            std::size_t words = argc >= 5 ? std::stoul(argv[4]) : 4096u;
            if (!recompile_psx_exe_to_split_files(exe, argv[3], words)) {
                std::cerr << "split generation failed\n"; return 3;
            }
            std::cout << "generated split C++ in " << argv[3] << " (" << words << " words/part)\n";
        } else {
            auto cpp = recompile_psx_exe_to_cpp(exe);
            std::ofstream out(argv[2], std::ios::binary);
            out << cpp;
            std::cout << "generated " << argv[2] << "\n";
        }
        std::cout << "entry=" << std::hex << exe.initial_pc
                  << " load=" << exe.load_address
                  << " size=" << exe.load_size << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
}
