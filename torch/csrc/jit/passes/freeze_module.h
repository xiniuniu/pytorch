/** \brief This file defines passes to freeze pytorch module..
 *
 * The passes have python-bindings and can be invoked directly or as a part of
 * general optimization pipeline (details TBD).
 */
#pragma once

#include <torch/csrc/jit/ir.h>
#include <torch/csrc/jit/script/module.h>

/** \brief Freeze Module, i.e., Assume all atrribute are constants.
 *
 * constant propagate attribute and optimize module attributes agressively
 * inline into forward to enable optimizations.
 */

namespace torch {
namespace jit {

TORCH_API script::Module FreezeModule(const script::Module& module);

} // namespace jit
} // namespace torch
