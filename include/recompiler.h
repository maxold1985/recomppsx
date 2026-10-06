#pragma once
#include "psxexe.h"
#include <string>
#include <cstddef>

struct RecompileOptions {
    RecompileOptions() : emit_comments(true), stop_on_unknown(false) {}
    bool emit_comments;
    bool stop_on_unknown;
};

std::string recompile_psx_exe_to_cpp(const PsxExeImage& exe, const RecompileOptions& opt = RecompileOptions());
bool recompile_psx_exe_to_split_files(const PsxExeImage& exe, const std::string& outputDir, std::size_t wordsPerPart = 4096, const RecompileOptions& opt = RecompileOptions());
