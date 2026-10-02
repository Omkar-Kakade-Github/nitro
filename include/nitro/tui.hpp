#pragma once
#include "nitro/options.hpp"
#include <csignal>
namespace nitro {
extern volatile std::sig_atomic_t interrupted;
int run_tui(const Options& options);
} // namespace nitro
