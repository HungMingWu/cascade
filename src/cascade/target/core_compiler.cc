// Copyright 2017-2019 VMware, Inc.
// SPDX-License-Identifier: BSD-2-Clause
//
// The BSD-2 license (the License) set forth below applies to all parts of the
// Cascade project.  You may not use this file except in compliance with the
// License.
//
// BSD-2 License
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS AS IS AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "target/core_compiler.h"

#include <sstream>
#include "target/compiler.h"
#include "verilog/analyze/evaluate.h"
#include "verilog/analyze/module_info.h"
#include "verilog/ast/ast.h"

using namespace std;

namespace cascade {

CoreCompiler::CoreCompiler() {
  set_compiler(nullptr);
}

CoreCompiler& CoreCompiler::set_compiler(Compiler* c) {
  compiler_ = c;
  return *this;
}

Core* CoreCompiler::compile(Engine::Id id, ModuleDeclaration* md, Interface* interface) {
  const auto* delay = md->get_attrs()->get<Number>("__delay");
  if (delay != nullptr) {
    this_thread::sleep_for(chrono::seconds(delay->get_val().to_uint()));
  }
  if (md->get_attrs()->find("__state_safe_int")) {
    get_compiler()->schedule_state_safe_interrupt([]{});
  }

  const auto* std = md->get_attrs()->get<String>("__std");
  if (std->eq("clock")) {
    return compile_clock(id, std::unique_ptr<ModuleDeclaration>(md), interface);
  } else if (std->eq("gpio")) {
    return compile_gpio(id, std::unique_ptr<ModuleDeclaration>(md), interface);
  } else if (std->eq("led")) {
    return compile_led(id, std::unique_ptr<ModuleDeclaration>(md), interface);
  } else if (std->eq("logic")) {
    return compile_logic(id, std::unique_ptr<ModuleDeclaration>(md), interface);
  } else if (std->eq("pad")) {
    return compile_pad(id, std::unique_ptr<ModuleDeclaration>(md), interface);
  } else if (std->eq("reset")) {
    return compile_reset(id, std::unique_ptr<ModuleDeclaration>(md), interface);
  } else {
    return compile_custom(id, std::unique_ptr<ModuleDeclaration>(md), interface);
  }
}

void CoreCompiler::stop_async() {
  // Does nothing.
}

Clock* CoreCompiler::compile_clock(Engine::Id id, std::unique_ptr<ModuleDeclaration> md, Interface* interface) {
  (void) id;
  (void) interface;
  get_compiler()->error("No compiler support available for modules of type clock");
  return nullptr;
}

Custom* CoreCompiler::compile_custom(Engine::Id id, std::unique_ptr<ModuleDeclaration> md, Interface* interface) {
  (void) id;
  (void) interface;

  const auto* std = md->get_attrs()->get<String>("__std");
  assert(std != nullptr);
  get_compiler()->error("No compiler support available for custom modules of type " + std->get_readable_val());

  return nullptr;
}

Gpio* CoreCompiler::compile_gpio(Engine::Id id, std::unique_ptr<ModuleDeclaration> md, Interface* interface) {
  (void) id;
  (void) interface;
  get_compiler()->error("No compiler support available for modules of type gpio");
  return nullptr;
}

Led* CoreCompiler::compile_led(Engine::Id id, std::unique_ptr<ModuleDeclaration> md, Interface* interface) {
  (void) id;
  (void) interface;
  get_compiler()->error("No compiler support available for modules of type led");
  return nullptr;
}

Pad* CoreCompiler::compile_pad(Engine::Id id, std::unique_ptr<ModuleDeclaration> md, Interface* interface) {
  (void) id;
  (void) interface;
  get_compiler()->error("No compiler support available for modules of type pad");
  return nullptr;
}

Reset* CoreCompiler::compile_reset(Engine::Id id, std::unique_ptr<ModuleDeclaration> md, Interface* interface) {
  (void) id;
  (void) interface;
  get_compiler()->error("No compiler support available for modules of type reset");
  return nullptr;
}

Logic* CoreCompiler::compile_logic(Engine::Id id, std::unique_ptr<ModuleDeclaration> md, Interface* interface) {
  (void) id;
  (void) interface;
  get_compiler()->error("No compiler support available for modules of type logic");
  return nullptr;
} 

Compiler* CoreCompiler::get_compiler() {
  assert(compiler_ != nullptr);
  return compiler_;
}

MId CoreCompiler::to_mid(const Identifier* id) const {
  return to_vid(id);
}

VId CoreCompiler::to_vid(const Identifier* id) const {
  VId res;
  stringstream ss(id->front_ids()->get_readable_sid().substr(3));
  ss >> res;
  return res;
}

bool CoreCompiler::check_io(const ModuleDeclaration* md, size_t is, size_t os) const {
  if (is == 0) {
    if (ModuleInfo(md).inputs().size() != 0) {
      return false;
    }
  } else {
    if ((ModuleInfo(md).inputs().size() == 1) && (Evaluate().get_width(*ModuleInfo(md).inputs().begin()) > is)) {
      return false;
    } else if (ModuleInfo(md).inputs().size() > 1) {
      return false;
    }
  }
  if (os == 0) {
    if (ModuleInfo(md).outputs().size() != 0) {
      return false;
    }
  } else {
    if ((ModuleInfo(md).outputs().size() == 1) && (Evaluate().get_width(*ModuleInfo(md).outputs().begin()) > os)) {
      return false;
    } else if (ModuleInfo(md).outputs().size() > 1) {
      return false;
    }
  }
  return true;
}

} // namespace cascade
