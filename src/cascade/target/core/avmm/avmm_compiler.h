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

#ifndef CASCADE_SRC_TARGET_CORE_AVMM_AVMM_COMPILER_H
#define CASCADE_SRC_TARGET_CORE_AVMM_AVMM_COMPILER_H

#include <cmath>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <stdint.h>
#include <string>
#include <vector>
#include "common/indstream.h"
#include "target/compiler.h"
#include "target/core_compiler.h"
#include "target/core/avmm/avmm_logic.h"
#include "target/core/avmm/rewrite.h"
#include "verilog/analyze/evaluate.h"
#include "verilog/analyze/module_info.h"
#include "verilog/ast/ast.h"

namespace cascade::avmm {

template <size_t M, size_t V, typename A, typename T>
class AvmmCompiler : public CoreCompiler {
  public:
    AvmmCompiler();
    ~AvmmCompiler() override = default;

    // Core Compiler Interface:
    void stop_compile(Engine::Id id) override;

  protected:
    // Avalon Memory Mapped Compiler Interface
    //
    // This method should perform whatever target-specific logic is necessary
    // to return an instance of an AvmmLogic. 
    virtual AvmmLogic<V,A,T>* build(Interface* interface, ModuleDeclaration* md, size_t slot) = 0;
    // This method should perform whatever target-specific logic is necessary
    // to stop any previous compilations and compile text to a device. This
    // method is called in a context where it holds the global lock on this
    // compiler. Implementations for which this may take a long time should
    // release this lock, but reaquire it before returning.  This method should
    // return true on success, and false on failure, say if stop_compile
    // interrupted a compilation.
    virtual bool compile(const std::string& text, std::mutex& lock) = 0;
    // This method should perform whatever target-specific logic is necessary
    // to stop the execution of any invocations of compile().
    virtual void stop_compile() = 0;

  private:
    // Compilation States:
    enum class State : uint8_t {
      FREE = 0,
      COMPILING,
      WAITING,
      STOPPED,
      CURRENT
    };
    // Slot Information:
    struct Slot {
      Engine::Id id;
      State state;
      std::string text;
    };

    // Program Management:
    std::mutex lock_;
    std::condition_variable cv_;
    std::vector<Slot> slots_;

    // Core Compiler Interface:
    AvmmLogic<V,A,T>* compile_logic(Engine::Id id, std::unique_ptr<ModuleDeclaration> md, Interface* interface) override;

    // Slot Management Helpers:
    int get_free() const;
    void release(size_t slot);
    void update();

    // Codegen Helpers:
    std::string get_text();
};

template <size_t M, size_t V, typename A, typename T>
inline AvmmCompiler<M,V,A,T>::AvmmCompiler() : CoreCompiler() {
  const auto num_slots = T(1) << M;
  slots_.resize(num_slots, {0, State::FREE, ""});
}

template <size_t M, size_t V, typename A, typename T>
inline void AvmmCompiler<M,V,A,T>::stop_compile(Engine::Id id) {
  std::lock_guard<std::mutex> lg(lock_);

  // Free any slot with this id which is in the compiling or waiting state. 
  auto stopped = false;
  auto need_new_lead = false;
  for (auto& s : slots_) {
    if (s.id == id) {
      switch (s.state) {
        case State::COMPILING:
          need_new_lead = true;
          // fallthrough
        case State::WAITING:
          stopped = true;
          s.state = State::STOPPED;
          break;
        default:
          break;
      } 
    }
  }
  // If nothing was stopped, we can return immediately.
  if (!stopped) {
    return;
  }
  // If we need a new compilation lead, find a waiting slot and promote it.
  if (need_new_lead) {
    for (auto& s : slots_) {
      if (s.state == State::WAITING) {
        s.state = State::COMPILING;
        break;
      }
    }
  }
  // Target-specific implementation of stop logic
  stop_compile();

  // Notify any waiting threads that the slot table has changed.
  cv_.notify_all();
}

template <size_t M, size_t V, typename A, typename T>
inline AvmmLogic<V,A,T>* AvmmCompiler<M,V,A,T>::compile_logic(Engine::Id id, std::unique_ptr<ModuleDeclaration> md, Interface* interface) {
  std::unique_lock<std::mutex> lg(lock_);
  ModuleInfo info(md.get());

  // Check for unsupported language features
  auto unsupported = false;
  if (info.uses_mixed_triggers()) {
    get_compiler()->error("Avmm backends do not currently support code with mixed triggers!");
    unsupported = true;
  } else if (!info.implied_latches().empty()) {
    get_compiler()->error("Avmm backends do not currently support the use of implied latches!");
    unsupported = true;
  } else if (info.uses_multiple_clocks()) {
    get_compiler()->error("Avmm backends do not currently support the use of multiple clocks!");
    unsupported = true;
  }
  if (unsupported) {
    return nullptr;
  }

  // Find a free slot 
  const auto slot = get_free();
  if (slot == -1) {
    get_compiler()->error("No remaining slots available on Avmm device");
    return nullptr;
  }
  
  // Register inputs, state, and outputs. Invoke these methods
  // lexicographically to ensure a deterministic variable table ordering. The
  // final invocation of index_tasks is lexicographic by construction, as it's
  // based on a recursive descent of the AST.
  auto* al = build(interface, md.get(), slot);
  std::map<VId, const Identifier*> is;
  for (auto* i : info.inputs()) {
    is.insert(std::make_pair(to_vid(i), i));
  }
  for (const auto& i : is) {
    al->set_input(i.second, i.first);
  }
  std::map<VId, const Identifier*> ss;
  for (auto* s : info.stateful()) {
    ss.insert(std::make_pair(to_vid(s), s));
  }
  for (const auto& s : ss) {
    al->set_state(s.second, s.first);
  }
  std::map<VId, const Identifier*> os;
  for (auto* o : info.outputs()) {
    os.insert(std::make_pair(to_vid(o), o));
  }
  for (const auto& o : os) {
    al->set_output(o.second, o.first);
  }
  al->index_tasks();
  // Check table and index sizes. If this program uses too much state, we won't
  // be able to uniquely name its elements using our current addressing scheme.

  const auto max_vars = T(1) << V;
  if (al->get_table()->size() >= max_vars) {
    std::stringstream ss;
    ss << "Avmm backends do not currently support more than " << max_vars << " entries in variable table";
    get_compiler()->error(ss.str());
    delete al;
    return nullptr;
  }

  // Downgrade any compilation slots to waiting slots, and stop any slots that
  // are working on this id.
  for (auto& s : slots_) {
    if (s.state == State::COMPILING) {
      s.state = State::WAITING;
    }
    if ((s.id == id) && (s.state == State::WAITING)) {
      s.state = State::STOPPED;
    }
  }
  // This slot is now the compile lead
  slots_[slot].id = id;
  slots_[slot].state = State::COMPILING;
  slots_[slot].text = Rewrite<M,V,A,T>().run(md.get(), slot, al->get_table(), al->open_loop_clock());
  // Enter into compilation state machine. Control will exit from this loop
  // either when compilation succeeds or is aborted.
  while (true) {
    switch (slots_[slot].state) {
      case State::COMPILING:
        if (compile(get_text(), lock_)) {
          update();
        }
        break;
      case State::WAITING:
        cv_.wait(lg);
        break;
      case State::STOPPED:
        slots_[slot].state = State::FREE;
        delete al;
        return nullptr;
      case State::CURRENT:
        al->set_callback([this, slot]{release(slot);});
        return al;
      default:
        // Control should never reach here
        assert(false);
        break;
    }
  }
}

template <size_t M, size_t V, typename A, typename T>
inline int AvmmCompiler<M,V,A,T>::get_free() const {
  for (size_t i = 0, ie = slots_.size(); i < ie; ++i) {
    if (slots_[i].state == State::FREE) {
      return i;
    }
  }
  return -1;
}

template <size_t M, size_t V, typename A, typename T>
inline void AvmmCompiler<M,V,A,T>::release(size_t slot) {
  // Return this slot to the pool if necessary. This method is only invoked on
  // successfully compiled cores, which means we don't have to worry about
  // transfering compilation ownership or invoking stop_compile.
  std::lock_guard<std::mutex> lg(lock_);
  assert(slots_[slot].state == State::CURRENT);
  slots_[slot].state = State::FREE;
  cv_.notify_all();
}

template <size_t M, size_t V, typename A, typename T>
inline void AvmmCompiler<M,V,A,T>::update() {
  for (auto& s : slots_) {
    if ((s.state == State::COMPILING) || (s.state == State::WAITING)) {
      s.state = State::CURRENT;
    }     
  }
  cv_.notify_all();
}

template <size_t M, size_t V, typename A, typename T>
inline std::string AvmmCompiler<M,V,A,T>::get_text() {
  std::stringstream ss;
  indstream os(ss);

  // Generate code for modules
  std::map<MId, std::string> text;
  for (size_t i = 0, ie = slots_.size(); i < ie; ++i) {
    if (slots_[i].state != State::FREE) {
      text.insert(std::make_pair(i, slots_[i].text));
    }
  }

  // Module Declarations
  for (const auto& s : text) {
    os << s.second << std::endl;
    os << std::endl;
  }

  // Top-level Module
  os << "module program_logic(" << std::endl;
  os.tab();
  os << "input wire clk," << std::endl;
  os << "input wire reset," << std::endl;
  os << std::endl;
  os << "input wire[" << (std::numeric_limits<A>::digits-1) << ":0]  s0_address," << std::endl;
  os << "input wire s0_read," << std::endl;
  os << "input wire s0_write," << std::endl;
  os << std::endl;
  os << "output wire[" << (std::numeric_limits<T>::digits-1) << ":0] s0_readdata," << std::endl;
  os << "input  wire[" << (std::numeric_limits<T>::digits-1) << ":0] s0_writedata," << std::endl;
  os << std::endl;
  os << "output wire s0_waitrequest" << std::endl;
  os.untab();
  os << ");" << std::endl;
  os.tab();

  os << "// Unpack address into module id and variable id" << std::endl;
  os << "wire[" << (M-1) << ":0] __mid = s0_address[" << (V+M-1) << ":" << V << "];" << std::endl;
  os << "wire[" << (V-1) << ":0] __vid = s0_address[" << (V-1) << ":0];" << std::endl;

  os << "// Module Instantiations:" << std::endl;
  for (const auto& s : text) {
    os << "wire[" << (std::numeric_limits<T>::digits-1) << ":0] m" << s.first << "_out;" << std::endl;
    os << "wire m" << s.first << "_wait;" << std::endl;
    os << "M" << s.first << " m" << s.first << "(" << std::endl;
    os.tab();
    os << ".__clk(clk)," << std::endl;
    os << ".__read((__mid == " << s.first << ") & s0_write)," << std::endl;
    os << ".__write((__mid == " << s.first << ") & s0_read)," << std::endl;
    os << ".__vid(__vid)," << std::endl;
    os << ".__in(s0_writedata)," << std::endl;
    os << ".__out(m" << s.first << "_out)," << std::endl;
    os << ".__wait(m" << s.first << "_wait)" << std::endl;
    os.untab();
    os << ");" << std::endl;
  }

  os << "// Output Demuxing:" << std::endl;
  os << "reg[" << (std::numeric_limits<T>::digits-1) << ":0] rd;" << std::endl;
  os << "reg wr;" << std::endl;
  os << "always @(*) begin" << std::endl;
  os.tab();
  os << "case (__mid)" << std::endl;
  os.tab();
  for (const auto& s : text) {
    os << s.first << ": begin rd = m" << s.first << "_out; wr = m" << s.first << "_wait; end" << std::endl;
  }
  os << "default: begin rd = 0; wr = 0; end" << std::endl;
  os.untab();
  os << "endcase" << std::endl;
  os.untab();
  os << "end" << std::endl;

  os << "// Output Logic:" << std::endl;
  os << "assign s0_waitrequest = (s0_read | s0_write) ? wr : 1'b1;" << std::endl;
  os << "assign s0_readdata = rd;" << std::endl;

  os.untab();
  os << "endmodule";

  return ss.str();
}

} // namespace cascade::avmm

#endif
