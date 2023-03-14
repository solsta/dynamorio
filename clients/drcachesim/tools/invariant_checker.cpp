/* **********************************************************
 * Copyright (c) 2017-2023 Google, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include "dr_api.h"
#include "invariant_checker.h"
#include "invariant_checker_create.h"
#include <algorithm>
#include <iostream>
#include <string.h>

analysis_tool_t *
invariant_checker_create(bool offline, unsigned int verbose)
{
    return new invariant_checker_t(offline, verbose, "");
}

invariant_checker_t::invariant_checker_t(bool offline, unsigned int verbose,
                                         std::string test_name,
                                         std::istream *serial_schedule_file,
                                         std::istream *cpu_schedule_file)
    : knob_offline_(offline)
    , knob_verbose_(verbose)
    , knob_test_name_(test_name)
    , serial_schedule_file_(serial_schedule_file)
    , cpu_schedule_file_(cpu_schedule_file)
{
    if (knob_test_name_ == "kernel_xfer_app" || knob_test_name_ == "rseq_app")
        has_annotations_ = true;
}

invariant_checker_t::~invariant_checker_t()
{
}

std::string
invariant_checker_t::initialize_stream(memtrace_stream_t *serial_stream)
{
    serial_stream_ = serial_stream;
    return "";
}

void
invariant_checker_t::report_if_false(per_shard_t *shard, bool condition,
                                     const std::string &invariant_name)
{
    if (!condition) {
        std::cerr << "Trace invariant failure in T" << shard->tid_ << " at ref # "
                  << shard->stream->get_record_ordinal() << ": " << invariant_name
                  << "\n";
        abort();
    }
}

bool
invariant_checker_t::parallel_shard_supported()
{
    return true;
}

void *
invariant_checker_t::parallel_shard_init_stream(int shard_index, void *worker_data,
                                                memtrace_stream_t *shard_stream)
{
    auto per_shard = std::unique_ptr<per_shard_t>(new per_shard_t);
    per_shard->stream = shard_stream;
    void *res = reinterpret_cast<void *>(per_shard.get());
    std::lock_guard<std::mutex> guard(shard_map_mutex_);
    shard_map_[shard_index] = std::move(per_shard);
    return res;
}

// We have no stream interface in invariant_checker_test unit tests.
// XXX: Could we refactor the test to use a reader that takes a vector?
void *
invariant_checker_t::parallel_shard_init(int shard_index, void *worker_data)
{
    return parallel_shard_init_stream(shard_index, worker_data, nullptr);
}

bool
invariant_checker_t::parallel_shard_exit(void *shard_data)
{
    return true;
}

std::string
invariant_checker_t::parallel_shard_error(void *shard_data)
{
    per_shard_t *shard = reinterpret_cast<per_shard_t *>(shard_data);
    return shard->error_;
}

bool
invariant_checker_t::parallel_shard_memref(void *shard_data, const memref_t &memref)
{
    per_shard_t *shard = reinterpret_cast<per_shard_t *>(shard_data);
    if (shard->tid_ == -1 && memref.data.tid != 0)
        shard->tid_ = memref.data.tid;
    // We check the memtrace_stream_t counts with our own, unless there was an
    // instr skip from the start where we cannot compare, or we're in a unit
    // test with no stream interface, or we're in serial mode (since we want
    // per-shard counts for error reporting; XXX: we could add our own global
    // counts to compare to the serial stream).
    ++shard->ref_count_;
    if (type_is_instr(memref.instr.type))
        ++shard->instr_count_;
    // XXX: We also can't verify counts with a skip invoked from the middle, but
    // we have no simple way to detect that here.
    if (shard->instr_count_ <= 1 && !shard->skipped_instrs_ && shard->stream != nullptr &&
        shard->stream->get_instruction_ordinal() > 1)
        shard->skipped_instrs_ = true;
    if (!shard->skipped_instrs_ && shard->stream != nullptr &&
        (shard->stream != serial_stream_ || shard_map_.size() == 1)) {
        report_if_false(shard, shard->ref_count_ == shard->stream->get_record_ordinal(),
                        "Stream record ordinal inaccurate");
        report_if_false(shard,
                        shard->instr_count_ == shard->stream->get_instruction_ordinal(),
                        "Stream instr ordinal inaccurate");
    }
#ifdef UNIX
    if (has_annotations_) {
        // Check conditions specific to the signal_invariants app, where it
        // has annotations in prefetch instructions telling us how many instrs
        // and/or memrefs until a signal should arrive.
        if ((shard->instrs_until_interrupt_ == 0 &&
             shard->memrefs_until_interrupt_ == -1) ||
            (shard->instrs_until_interrupt_ == -1 &&
             shard->memrefs_until_interrupt_ == 0) ||
            (shard->instrs_until_interrupt_ == 0 &&
             shard->memrefs_until_interrupt_ == 0)) {
            report_if_false(
                shard,
                (memref.marker.type == TRACE_TYPE_MARKER &&
                 memref.marker.marker_type == TRACE_MARKER_TYPE_KERNEL_EVENT) ||
                    // TODO i#3937: Online instr bundles currently violate this.
                    !knob_offline_,
                "Interruption marker mis-placed");
            shard->instrs_until_interrupt_ = -1;
            shard->memrefs_until_interrupt_ = -1;
        }
        if (shard->memrefs_until_interrupt_ >= 0 &&
            (memref.data.type == TRACE_TYPE_READ ||
             memref.data.type == TRACE_TYPE_WRITE)) {
            report_if_false(shard, shard->memrefs_until_interrupt_ != 0,
                            "Interruption marker too late");
            --shard->memrefs_until_interrupt_;
        }
        // Check that the signal delivery marker is immediately followed by the
        // app's signal handler, which we have annotated with "prefetcht0 [1]".
        if (memref.data.type == TRACE_TYPE_PREFETCHT0 && memref.data.addr == 1) {
            report_if_false(shard,
                            type_is_instr(shard->prev_entry_.instr.type) &&
                                shard->prev_prev_entry_.marker.type ==
                                    TRACE_TYPE_MARKER &&
                                shard->last_xfer_marker_.marker.marker_type ==
                                    TRACE_MARKER_TYPE_KERNEL_EVENT,
                            "Signal handler not immediately after signal marker");
            shard->app_handler_pc_ = shard->prev_entry_.instr.addr;
        }
        // Look for annotations where signal_invariants.c and rseq.c pass info to us on
        // what to check for.  We assume the app does not have prefetch instrs w/ low
        // addresses.
        if (memref.data.type == TRACE_TYPE_PREFETCHT2 && memref.data.addr < 1024) {
            shard->instrs_until_interrupt_ = static_cast<int>(memref.data.addr);
        }
        if (memref.data.type == TRACE_TYPE_PREFETCHT1 && memref.data.addr < 1024) {
            shard->memrefs_until_interrupt_ = static_cast<int>(memref.data.addr);
        }
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        shard->prev_entry_.marker.type == TRACE_TYPE_MARKER &&
        shard->prev_entry_.marker.marker_type == TRACE_MARKER_TYPE_RSEQ_ABORT) {
        // The rseq marker must be immediately prior to the kernel event marker.
        report_if_false(shard,
                        memref.marker.marker_type == TRACE_MARKER_TYPE_KERNEL_EVENT,
                        "Rseq marker not immediately prior to kernel marker");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_RSEQ_ABORT) {
        // Check that the rseq final instruction was not executed: that raw2trace
        // rolled it back.
        report_if_false(shard,
                        memref.marker.marker_value != shard->prev_instr_.instr.addr,
                        "Rseq post-abort instruction not rolled back");
    }
#endif

    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_FILETYPE) {
        shard->file_type_ = static_cast<offline_file_type_t>(memref.marker.marker_value);
        report_if_false(shard,
                        shard->stream == nullptr ||
                            shard->file_type_ == shard->stream->get_filetype(),
                        "Stream interface filetype != trace marker");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_INSTRUCTION_COUNT) {
        shard->found_instr_count_marker_ = true;
        report_if_false(shard,
                        memref.marker.marker_value >= shard->last_instr_count_marker_,
                        "Instr count markers not increasing");
        shard->last_instr_count_marker_ = memref.marker.marker_value;
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_CACHE_LINE_SIZE) {
        shard->found_cache_line_size_marker_ = true;
        report_if_false(shard,
                        shard->stream == nullptr ||
                            memref.marker.marker_value ==
                                shard->stream->get_cache_line_size(),
                        "Stream interface cache line size != trace marker");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_PAGE_SIZE) {
        shard->found_page_size_marker_ = true;
        report_if_false(shard,
                        shard->stream == nullptr ||
                            memref.marker.marker_value == shard->stream->get_page_size(),
                        "Stream interface page size != trace marker");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_VERSION) {
        report_if_false(shard,
                        shard->stream == nullptr ||
                            memref.marker.marker_value == shard->stream->get_version(),
                        "Stream interface version != trace marker");
    }

    // Invariant: each chunk's instruction count must be identical and equal to
    // the value in the top-level marker.
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_CHUNK_INSTR_COUNT) {
        shard->chunk_instr_count_ = memref.marker.marker_value;
        report_if_false(shard,
                        shard->stream == nullptr ||
                            shard->chunk_instr_count_ ==
                                shard->stream->get_chunk_instr_count(),
                        "Stream interface chunk instr count != trace marker");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_CHUNK_FOOTER) {
        report_if_false(shard,
                        shard->skipped_instrs_ ||
                            (shard->chunk_instr_count_ != 0 &&
                             shard->instr_count_ % shard->chunk_instr_count_ == 0),
                        "Chunk instruction counts are inconsistent");
    }

    // Invariant: a function marker should not appear between an instruction and its
    // memrefs or in the middle of a block (we assume elision is turned off and so a
    // callee entry will always be the top of a block).  (We don't check for other types
    // of markers b/c a virtual2physical one *could* appear in between.)
    if (shard->prev_entry_.marker.type == TRACE_TYPE_MARKER &&
        marker_type_is_function_marker(shard->prev_entry_.marker.marker_type)) {
        report_if_false(shard,
                        memref.data.type != TRACE_TYPE_READ &&
                            memref.data.type != TRACE_TYPE_WRITE &&
                            !type_is_prefetch(memref.data.type),
                        "Function marker misplaced between instr and memref");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        marker_type_is_function_marker(memref.marker.marker_type)) {
        report_if_false(shard, type_is_instr_branch(shard->prev_instr_.instr.type),
                        "Function marker should be after a branch");
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_FUNC_RETADDR) {
        report_if_false(shard, memref.marker.marker_value == shard->last_retaddr_,
                        "Function marker retaddr should match prior call");
    }

    if (memref.exit.type == TRACE_TYPE_THREAD_EXIT) {
        report_if_false(shard,
                        !TESTANY(OFFLINE_FILE_TYPE_FILTERED | OFFLINE_FILE_TYPE_IFILTERED,
                                 shard->file_type_) ||
                            shard->found_instr_count_marker_,
                        "Missing instr count markers");
        report_if_false(shard,
                        shard->found_cache_line_size_marker_ ||
                            (shard->skipped_instrs_ && shard->stream != nullptr &&
                             shard->stream->get_cache_line_size() > 0),
                        "Missing cache line marker");
        report_if_false(shard,
                        shard->found_page_size_marker_ ||
                            (shard->skipped_instrs_ && shard->stream != nullptr &&
                             shard->stream->get_page_size() > 0),
                        "Missing page size marker");
        if (knob_test_name_ == "filter_asm_instr_count") {
            static constexpr int ASM_INSTR_COUNT = 133;
            report_if_false(shard, shard->last_instr_count_marker_ == ASM_INSTR_COUNT,
                            "Incorrect instr count marker value");
        }
    }
    if (shard->prev_entry_.marker.type == TRACE_TYPE_MARKER &&
        shard->prev_entry_.marker.marker_type == TRACE_MARKER_TYPE_PHYSICAL_ADDRESS) {
        // A physical address marker must be immediately prior to its virtual marker.
        report_if_false(shard,
                        memref.marker.type == TRACE_TYPE_MARKER &&
                            memref.marker.marker_type ==
                                TRACE_MARKER_TYPE_VIRTUAL_ADDRESS,
                        "Physical addr marker not immediately prior to virtual marker");
        // We don't have the actual page size, but it is always at least 4K, so
        // make sure the bottom 12 bits are the same.
        report_if_false(shard,
                        (memref.marker.marker_value & 0xfff) ==
                            (shard->prev_entry_.marker.marker_value & 0xfff),
                        "Physical addr bottom 12 bits do not match virtual");
    }
    if (type_is_instr(memref.instr.type) ||
        memref.instr.type == TRACE_TYPE_PREFETCH_INSTR ||
        memref.instr.type == TRACE_TYPE_INSTR_NO_FETCH) {
        if (knob_verbose_ >= 3) {
            std::cerr << "::" << memref.data.pid << ":" << memref.data.tid << ":: "
                      << " @" << (void *)memref.instr.addr
                      << ((memref.instr.type == TRACE_TYPE_INSTR_NO_FETCH)
                              ? " non-fetched"
                              : "")
                      << " instr x" << memref.instr.size << "\n";
        }
#ifdef UNIX
        report_if_false(shard, shard->instrs_until_interrupt_ != 0,
                        "Interruption marker too late");
        if (shard->instrs_until_interrupt_ > 0)
            --shard->instrs_until_interrupt_;
#endif
        if (memref.instr.type == TRACE_TYPE_INSTR_DIRECT_CALL ||
            memref.instr.type == TRACE_TYPE_INSTR_INDIRECT_CALL) {
            shard->last_retaddr_ = memref.instr.addr + memref.instr.size;
        }
        // Invariant: offline traces guarantee that a branch target must immediately
        // follow the branch w/ no intervening thread switch.
        // If we did serial analyses only, we'd just track the previous instr in the
        // interleaved stream.  Here we look for headers indicating where an interleaved
        // stream *could* switch threads, so we're stricter than necessary.
        if (knob_offline_ && type_is_instr_branch(shard->prev_instr_.instr.type)) {
            report_if_false(shard,
                            !shard->saw_timestamp_but_no_instr_ ||
                                // The invariant is relaxed for a signal.
                                // prev_xfer_marker_ is cleared on an instr, so if set to
                                // non-sentinel it means it is immediately prior, in
                                // between prev_instr_ and memref.
                                shard->prev_xfer_marker_.marker.marker_type ==
                                    TRACE_MARKER_TYPE_KERNEL_EVENT,
                            "Branch target not immediately after branch");
        }
        // Invariant: non-explicit control flow (i.e., kernel-mediated) is indicated
        // by markers.
        bool have_cond_branch_target = false;
        addr_t cond_branch_target = 0;
        if (shard->prev_instr_.instr.addr != 0 /*first*/ &&
            type_is_instr_direct_branch(shard->prev_instr_.instr.type) &&
            // We do not bother to support legacy traces without encodings.
            TESTANY(OFFLINE_FILE_TYPE_ENCODINGS, shard->file_type_)) {
            addr_t trace_pc = shard->prev_instr_.instr.addr;
            if (shard->prev_instr_.instr.encoding_is_new)
                shard->branch_target_cache.erase(trace_pc);
            auto cached = shard->branch_target_cache.find(trace_pc);
            if (cached != shard->branch_target_cache.end()) {
                have_cond_branch_target = true;
                cond_branch_target = cached->second;
            } else {
                instr_t instr;
                instr_init(GLOBAL_DCONTEXT, &instr);
                const app_pc decode_pc =
                    const_cast<app_pc>(shard->prev_instr_.instr.encoding);
                const app_pc next_pc =
                    decode_from_copy(GLOBAL_DCONTEXT, decode_pc,
                                     reinterpret_cast<app_pc>(trace_pc), &instr);
                if (next_pc == nullptr || !opnd_is_pc(instr_get_target(&instr))) {
                    // Neither condition should happen but they could on an invalid
                    // encoding from raw2trace or the reader so we report an
                    // invariant rather than asserting.
                    report_if_false(shard, false, "Branch target is not decodeable");
                } else {
                    have_cond_branch_target = true;
                    cond_branch_target =
                        reinterpret_cast<addr_t>(opnd_get_pc(instr_get_target(&instr)));
                    shard->branch_target_cache[trace_pc] = cond_branch_target;
                }
                instr_free(GLOBAL_DCONTEXT, &instr);
            }
        }
        if (shard->prev_instr_.instr.addr != 0 /*first*/) {
            report_if_false(
                shard, // Filtered.
                TESTANY(OFFLINE_FILE_TYPE_FILTERED | OFFLINE_FILE_TYPE_IFILTERED,
                        shard->file_type_) ||
                    // Regular fall-through.
                    (shard->prev_instr_.instr.addr + shard->prev_instr_.instr.size ==
                     memref.instr.addr) ||
                    // Indirect branches we cannot check.
                    (type_is_instr_branch(shard->prev_instr_.instr.type) &&
                     !type_is_instr_direct_branch(shard->prev_instr_.instr.type)) ||
                    // Conditional fall-through hits the regular case above.
                    (type_is_instr_direct_branch(shard->prev_instr_.instr.type) &&
                     (!have_cond_branch_target ||
                      memref.instr.addr == cond_branch_target)) ||
                    // String loop.
                    (shard->prev_instr_.instr.addr == memref.instr.addr &&
                     (memref.instr.type == TRACE_TYPE_INSTR_NO_FETCH ||
                      // Online incorrectly marks the 1st string instr across a thread
                      // switch as fetched.
                      // TODO i#4915, #4948: Eliminate non-fetched and remove the
                      // underlying instrs altogether, which would fix this for us.
                      (!knob_offline_ && shard->saw_timestamp_but_no_instr_))) ||
                    // Kernel-mediated, but we can't tell if we had a thread swap.
                    (shard->prev_xfer_marker_.instr.tid != 0 &&
                     (shard->prev_xfer_marker_.marker.marker_type ==
                          TRACE_MARKER_TYPE_KERNEL_EVENT ||
                      shard->prev_xfer_marker_.marker.marker_type ==
                          TRACE_MARKER_TYPE_KERNEL_XFER)) ||
                    // We expect a gap on a window transition.
                    shard->window_transition_ ||
                    shard->prev_instr_.instr.type == TRACE_TYPE_INSTR_SYSENTER,
                "Non-explicit control flow has no marker");
            // XXX: If we had instr decoding we could check direct branch targets
            // and look for gaps after branches.
        }

#ifdef UNIX
        // Ensure signal handlers return to the interruption point.
        if (shard->prev_xfer_marker_.marker.marker_type ==
            TRACE_MARKER_TYPE_KERNEL_XFER) {
            report_if_false(
                shard,
                ((memref.instr.addr == shard->prev_xfer_int_pc_.top() ||
                  // DR hands us a different address for sysenter than the
                  // resumption point.
                  shard->pre_signal_instr_.top().instr.type ==
                      TRACE_TYPE_INSTR_SYSENTER) &&
                 (
                     // Skip pre_signal_instr_ check if there was no such instr.
                     shard->pre_signal_instr_.top().instr.addr == 0 ||
                     memref.instr.addr == shard->pre_signal_instr_.top().instr.addr ||
                     // Asynch will go to the subsequent instr.
                     memref.instr.addr ==
                         shard->pre_signal_instr_.top().instr.addr +
                             shard->pre_signal_instr_.top().instr.size ||
                     // Too hard to figure out branch targets.  We have the
                     // prev_xfer_int_pc_ though.
                     type_is_instr_branch(shard->pre_signal_instr_.top().instr.type) ||
                     shard->pre_signal_instr_.top().instr.type ==
                         TRACE_TYPE_INSTR_SYSENTER)) ||
                    // Nested signal.  XXX: This only works for our annotated test
                    // signal_invariants where we know shard->app_handler_pc_.
                    memref.instr.addr == shard->app_handler_pc_ ||
                    // Marker for rseq abort handler.  Not as unique as a prefetch, but
                    // we need an instruction and not a data type.
                    memref.instr.type == TRACE_TYPE_INSTR_DIRECT_JUMP,
                "Signal handler return point incorrect");
            // We assume paired signal entry-exit (so no longjmp and no rseq
            // inside signal handlers).
            shard->prev_xfer_int_pc_.pop();
            shard->pre_signal_instr_.pop();
        }
#endif
        shard->prev_instr_ = memref;
        shard->saw_kernel_xfer_after_prev_instr_ = false;
        // Clear prev_xfer_marker_ on an instr (not a memref which could come between an
        // instr and a kernel-mediated far-away instr) to ensure it's *immediately*
        // prior (i#3937).
        shard->prev_xfer_marker_.marker.marker_type = TRACE_MARKER_TYPE_VERSION;
        shard->saw_timestamp_but_no_instr_ = false;
        // Clear window transitions on instrs.
        shard->window_transition_ = false;
    } else if (knob_verbose_ >= 3) {
        std::cerr << "::" << memref.data.pid << ":" << memref.data.tid << ":: "
                  << " type " << memref.instr.type << "\n";
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_TIMESTAMP) {
        shard->last_timestamp_ = memref.marker.marker_value;
        shard->saw_timestamp_but_no_instr_ = true;
        if (knob_verbose_ >= 3) {
            std::cerr << "::" << memref.data.pid << ":" << memref.data.tid << ":: "
                      << " timestamp " << memref.marker.marker_value << "\n";
        }
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_CPU_ID) {
        shard->sched_.emplace_back(shard->tid_, shard->last_timestamp_,
                                   memref.marker.marker_value, shard->instr_count_);
        shard->cpu2sched_[memref.marker.marker_value].emplace_back(
            shard->tid_, shard->last_timestamp_, memref.marker.marker_value,
            shard->instr_count_);
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        // Ignore timestamp, etc. markers which show up at signal delivery boundaries
        // b/c the tracer does a buffer flush there.
        (memref.marker.marker_type == TRACE_MARKER_TYPE_KERNEL_EVENT ||
         memref.marker.marker_type == TRACE_MARKER_TYPE_KERNEL_XFER)) {
        if (knob_verbose_ >= 3) {
            std::cerr << "::" << memref.data.pid << ":" << memref.data.tid << ":: "
                      << "marker type " << memref.marker.marker_type << " value 0x"
                      << std::hex << memref.marker.marker_value << std::dec << "\n";
        }
#ifdef UNIX
        if (memref.marker.marker_type == TRACE_MARKER_TYPE_KERNEL_EVENT)
            shard->prev_xfer_int_pc_.push(memref.marker.marker_value);
        report_if_false(shard, memref.marker.marker_value != 0,
                        "Kernel event marker value missing");
        if (memref.marker.marker_type == TRACE_MARKER_TYPE_KERNEL_EVENT &&
            // XXX: Handle the back-to-back signals case where the second
            // signal arrives just after the return from the first without
            // any intervening instrs. The return point of the second one
            // would be the pc in the kernel xfer marker of the first.
            shard->prev_xfer_marker_.marker.marker_type !=
                TRACE_MARKER_TYPE_KERNEL_XFER) {
            if (shard->saw_kernel_xfer_after_prev_instr_) {
                // We have nested signals without an intervening app instr.
                // Push an empty instr to mean that this shouldn't be used.
                shard->pre_signal_instr_.push({});
            } else {
                shard->saw_kernel_xfer_after_prev_instr_ = true;
                // If there was a kernel xfer marker at the very beginning
                // of the trace, we may still push an empty instr here.
                shard->pre_signal_instr_.push(shard->prev_instr_);
            }
        }
#endif
        shard->prev_xfer_marker_ = memref;
        shard->last_xfer_marker_ = memref;
    }
    if (memref.marker.type == TRACE_TYPE_MARKER &&
        memref.marker.marker_type == TRACE_MARKER_TYPE_WINDOW_ID) {
        if (shard->last_window_ != memref.marker.marker_value)
            shard->window_transition_ = true;
        shard->last_window_ = memref.marker.marker_value;
    }

#ifdef UNIX
    shard->prev_prev_entry_ = shard->prev_entry_;
#endif
    shard->prev_entry_ = memref;

    return true;
}

bool
invariant_checker_t::process_memref(const memref_t &memref)
{
    per_shard_t *per_shard;
    const auto &lookup = shard_map_.find(memref.data.tid);
    if (lookup == shard_map_.end()) {
        auto per_shard_unique = std::unique_ptr<per_shard_t>(new per_shard_t);
        per_shard = per_shard_unique.get();
        per_shard->stream = serial_stream_;
        shard_map_[memref.data.tid] = std::move(per_shard_unique);
    } else
        per_shard = lookup->second.get();
    if (!parallel_shard_memref(reinterpret_cast<void *>(per_shard), memref)) {
        error_string_ = per_shard->error_;
        return false;
    }
    return true;
}

void
invariant_checker_t::check_schedule_data()
{
    if (serial_schedule_file_ == nullptr && cpu_schedule_file_ == nullptr)
        return;
    // Check that the scheduling data in the files written by raw2trace match
    // the data in the trace.
    per_shard_t global;
    // Use a synthetic stream object to allow report_if_false to work normally.
    auto stream = std::unique_ptr<memtrace_stream_t>(
        new default_memtrace_stream_t(&global.ref_count_));
    global.stream = stream.get();
    std::vector<schedule_entry_t> serial;
    std::unordered_map<uint64_t, std::vector<schedule_entry_t>> cpu2sched;
    for (auto &shard_keyval : shard_map_) {
        serial.insert(serial.end(), shard_keyval.second->sched_.begin(),
                      shard_keyval.second->sched_.end());
        for (auto &keyval : shard_keyval.second->cpu2sched_) {
            auto &vec = cpu2sched[keyval.first];
            vec.insert(vec.end(), keyval.second.begin(), keyval.second.end());
        }
    }
    std::sort(serial.begin(), serial.end(),
              [](const schedule_entry_t &l, const schedule_entry_t &r) {
                  return l.timestamp < r.timestamp;
              });
    if (serial_schedule_file_ != nullptr) {
        schedule_entry_t next(0, 0, 0, 0);
        while (
            serial_schedule_file_->read(reinterpret_cast<char *>(&next), sizeof(next))) {
            report_if_false(&global,
                            memcmp(&serial[static_cast<size_t>(global.ref_count_)], &next,
                                   sizeof(next)) == 0,
                            "Serial schedule entry does not match trace");
            ++global.ref_count_;
        }
        report_if_false(&global, global.ref_count_ == serial.size(),
                        "Serial schedule entry count does not match trace");
    }
    if (cpu_schedule_file_ == nullptr)
        return;
    std::unordered_map<uint64_t, uint64_t> cpu2idx;
    for (auto &keyval : cpu2sched) {
        std::sort(keyval.second.begin(), keyval.second.end(),
                  [](const schedule_entry_t &l, const schedule_entry_t &r) {
                      return l.timestamp < r.timestamp;
                  });
        cpu2idx[keyval.first] = 0;
    }
    // The zipfile reader will form a continuous stream from all elements in the
    // archive.  We figure out which cpu each one is from on the fly.
    schedule_entry_t next(0, 0, 0, 0);
    while (cpu_schedule_file_->read(reinterpret_cast<char *>(&next), sizeof(next))) {
        global.ref_count_ = next.instr_count;
        global.tid_ = next.thread;
        report_if_false(
            &global,
            memcmp(&cpu2sched[next.cpu][static_cast<size_t>(cpu2idx[next.cpu])], &next,
                   sizeof(next)) == 0,
            "Cpu schedule entry does not match trace");
        ++cpu2idx[next.cpu];
    }
    for (auto &keyval : cpu2sched) {
        global.ref_count_ = 0;
        global.tid_ = keyval.first;
        report_if_false(&global, cpu2idx[keyval.first] == keyval.second.size(),
                        "Cpu schedule entry count does not match trace");
    }
}

bool
invariant_checker_t::print_results()
{
    check_schedule_data();
    std::cerr << "Trace invariant checks passed\n";
    return true;
}
