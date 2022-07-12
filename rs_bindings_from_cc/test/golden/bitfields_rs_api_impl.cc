// Part of the Crubit project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <cstddef>
#include <memory>

#include "rs_bindings_from_cc/support/cxx20_backports.h"
#include "rs_bindings_from_cc/support/offsetof.h"
#include "rs_bindings_from_cc/test/golden/bitfields.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-analysis"
extern "C" void __rust_thunk___ZN13WithBitfieldsC1Ev(
    struct WithBitfields* __this) {
  crubit::construct_at(std::forward<decltype(__this)>(__this));
}
extern "C" void __rust_thunk___ZN13WithBitfieldsC1ERKS_(
    struct WithBitfields* __this, const struct WithBitfields& __param_0) {
  crubit::construct_at(std::forward<decltype(__this)>(__this),
                       std::forward<decltype(__param_0)>(__param_0));
}
extern "C" void __rust_thunk___ZN13WithBitfieldsC1EOS_(
    struct WithBitfields* __this, struct WithBitfields&& __param_0) {
  crubit::construct_at(std::forward<decltype(__this)>(__this),
                       std::forward<decltype(__param_0)>(__param_0));
}
extern "C" void __rust_thunk___ZN13WithBitfieldsD1Ev(
    struct WithBitfields* __this) {
  std::destroy_at(std::forward<decltype(__this)>(__this));
}
extern "C" struct WithBitfields& __rust_thunk___ZN13WithBitfieldsaSERKS_(
    struct WithBitfields* __this, const struct WithBitfields& __param_0) {
  return __this->operator=(std::forward<decltype(__param_0)>(__param_0));
}
extern "C" struct WithBitfields& __rust_thunk___ZN13WithBitfieldsaSEOS_(
    struct WithBitfields* __this, struct WithBitfields&& __param_0) {
  return __this->operator=(std::forward<decltype(__param_0)>(__param_0));
}

static_assert(sizeof(struct WithBitfields) == 32);
static_assert(alignof(struct WithBitfields) == 4);
static_assert(CRUBIT_OFFSET_OF(f2, struct WithBitfields) == 4);
static_assert(CRUBIT_OFFSET_OF(f5, struct WithBitfields) == 20);
static_assert(CRUBIT_OFFSET_OF(f7, struct WithBitfields) == 27);

#pragma clang diagnostic pop
