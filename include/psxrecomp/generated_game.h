#pragma once
#include "r3000a.h"

// This function is emitted by r3000a_recomp into generated/game.cpp.
void run_recompiled(r3k::CpuState& s, r3k::Memory& mem, r3k::Gte& gte);
