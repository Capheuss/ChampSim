/*
 *    Copyright 2023 The ChampSim Contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*! @file
 *  ChampSim PIN tracer — single-pass multi-simpoint version.
 *
 *  Attaches once to a warmed-up server and records all simpoint trace
 *  windows in a single pass through the instruction stream. This ensures
 *  all skip values are relative to the same instruction origin, matching
 *  the BBV that was collected on the same warm server.
 *
 *  Usage:
 *    pin -pid <server_pid> -t champsim_tracer.so \
 *        -simpoints <simpoints_file> \
 *        -weights   <weights_file> \
 *        -output    <output_dir> \
 *        -benchmark <name> \
 *        -trace_len <N> \
 *        -interval  <N>
 *
 *  The simpoints file format is: <simpoint_index> <cluster_id>
 *  Output files are named: <output_dir>/<benchmark>_sp<N>_cluster<C>.champsimtrace.xz
 *  (compression is handled externally via named pipes in gen_champsim_traces.sh)
 *
 *  Changes from original single-trace version:
 *  - Accepts a simpoints file instead of single -s/-t/-o knobs
 *  - Maintains a sorted list of trace windows; advances through them in one pass
 *  - Opens/closes output files dynamically as instrCount enters/exits each window
 *  - instrCount is std::atomic<UINT64> for correct multi-thread counting
 *  - curr_instr is thread-local to prevent threads clobbering each other
 *  - PIN_Detach() called after the last window completes
 */

 #include <algorithm>
 #include <atomic>
 #include <fstream>
 #include <iostream>
 #include <sstream>
 #include <stdlib.h>
 #include <string.h>
 #include <string>
 #include <vector>
 
 #include "../../inc/trace_instruction.h"
 #include "pin.H"
 
 using trace_instr_format_t = input_instr;
 
 /* ================================================================== */
 // Simpoint window descriptor
 /* ================================================================== */
 
 struct SimPointWindow {
     UINT64 skip_start;    // first instruction to record (skip_index * interval)
     UINT64 skip_end;      // last instruction to record (skip_start + trace_len - 1)
     std::string outpath;  // output file path
     int sp_num;           // simpoint number (for logging)
     int cluster_id;
 };
 
 /* ================================================================== */
 // Global variables
 /* ================================================================== */
 
 std::atomic<UINT64> instrCount{0};
 std::atomic<bool> detach_fired{false};
 PIN_LOCK write_lock;
 
 // Sorted list of trace windows — sorted by skip_start ascending
 std::vector<SimPointWindow> windows;
 // Index of the currently active window (-1 = between windows)
 int current_window = -1;
 // Index of the next window we haven't started yet
 int next_window = 0;
 
 std::ofstream outfile;
 static TLS_KEY tls_key = INVALID_TLS_KEY;
 
 /* ===================================================================== */
 // Command line switches
 /* ===================================================================== */
 KNOB<std::string> KnobSimPointsFile(KNOB_MODE_WRITEONCE, "pintool",
     "simpoints", "", "path to SimPoint .simpoints file (<index> <cluster> per line)");
 KNOB<std::string> KnobWeightsFile(KNOB_MODE_WRITEONCE, "pintool",
     "weights", "", "path to SimPoint .weights file");
 KNOB<std::string> KnobOutputDir(KNOB_MODE_WRITEONCE, "pintool",
     "output", ".", "output directory for trace files");
 KNOB<std::string> KnobBenchmark(KNOB_MODE_WRITEONCE, "pintool",
     "benchmark", "trace", "benchmark name prefix for output files");
 KNOB<UINT64> KnobTraceLen(KNOB_MODE_WRITEONCE, "pintool",
     "trace_len", "250000000", "instructions to trace per simpoint");
 KNOB<UINT64> KnobInterval(KNOB_MODE_WRITEONCE, "pintool",
     "interval", "200000000", "interval size used during BBV collection");
 
 /* ===================================================================== */
 // Utilities
 /* ===================================================================== */
 
 INT32 Usage()
 {
   std::cout << "ChampSim multi-simpoint tracer." << std::endl
             << "Records all simpoint trace windows in a single pass." << std::endl << std::endl;
   std::cout << KNOB_BASE::StringKnobSummary() << std::endl;
   return -1;
 }
 
 static void OpenWindow(int idx)
 {
   const SimPointWindow& w = windows[idx];
   std::cout << "[tracer] Opening SP" << w.sp_num
             << " cluster=" << w.cluster_id
             << " skip=" << w.skip_start
             << " -> " << w.outpath << std::endl;
   outfile.open(w.outpath.c_str(), std::ios_base::binary | std::ios_base::trunc);
   if (!outfile) {
     std::cerr << "ERROR: Could not open output file: " << w.outpath << std::endl;
     PIN_ExitApplication(1);
   }
   current_window = idx;
 }
 
 static void CloseCurrentWindow()
 {
   if (outfile.is_open()) {
     std::cout << "[tracer] Closing SP" << windows[current_window].sp_num << std::endl;
     outfile.close();
   }
   current_window = -1;
   next_window++;
 }
 
 /* ===================================================================== */
 // Analysis routines
 /* ===================================================================== */
 
 void ResetCurrentInstruction(VOID* ip)
 {
   trace_instr_format_t* instr = static_cast<trace_instr_format_t*>(
       PIN_GetThreadData(tls_key, PIN_ThreadId()));
   *instr = {};
   instr->ip = (unsigned long long int)ip;
 }
 
 BOOL ShouldWrite()
 {
   UINT64 count = ++instrCount;
 
   PIN_GetLock(&write_lock, PIN_ThreadId() + 1);
 
   // Advance to next window if we've passed the current one
   if (current_window >= 0 && count > windows[current_window].skip_end) {
     CloseCurrentWindow();
   }
 
   // Open next window if we've reached its start
   if (current_window < 0 && next_window < (int)windows.size()
       && count >= windows[next_window].skip_start) {
     OpenWindow(next_window);
   }
 
   // Detach once all windows are done
   if (next_window >= (int)windows.size() && current_window < 0) {
     PIN_ReleaseLock(&write_lock);
     std::cout << "[tracer] All simpoints captured — detaching PIN." << std::endl;
     PIN_Detach();
     return FALSE;
   }
 
   BOOL should = (current_window >= 0);
   PIN_ReleaseLock(&write_lock);
   return should;
 }
 
 void WriteCurrentInstruction()
 {
   trace_instr_format_t* instr = static_cast<trace_instr_format_t*>(
       PIN_GetThreadData(tls_key, PIN_ThreadId()));
   typename decltype(outfile)::char_type buf[sizeof(trace_instr_format_t)];
   std::memcpy(buf, instr, sizeof(trace_instr_format_t));
 
   PIN_GetLock(&write_lock, PIN_ThreadId() + 1);
   if (outfile.is_open())
     outfile.write(buf, sizeof(trace_instr_format_t));
   PIN_ReleaseLock(&write_lock);
 }
 
 void BranchOrNot(UINT32 taken)
 {
   trace_instr_format_t* instr = static_cast<trace_instr_format_t*>(
       PIN_GetThreadData(tls_key, PIN_ThreadId()));
   instr->is_branch = 1;
   instr->branch_taken = taken;
 }
 
 void WriteSourceReg(UINT32 index, UINT32 r)
 {
   trace_instr_format_t* instr = static_cast<trace_instr_format_t*>(
       PIN_GetThreadData(tls_key, PIN_ThreadId()));
   if (index < NUM_INSTR_SOURCES)
     instr->source_registers[index] = (unsigned char)r;
 }
 
 void WriteDestReg(UINT32 index, UINT32 r)
 {
   trace_instr_format_t* instr = static_cast<trace_instr_format_t*>(
       PIN_GetThreadData(tls_key, PIN_ThreadId()));
   if (index < NUM_INSTR_DESTINATIONS)
     instr->destination_registers[index] = (unsigned char)r;
 }
 
 void WriteSourceMem(UINT32 index, ADDRINT addr)
 {
   trace_instr_format_t* instr = static_cast<trace_instr_format_t*>(
       PIN_GetThreadData(tls_key, PIN_ThreadId()));
   if (index < NUM_INSTR_SOURCES)
     instr->source_memory[index] = (unsigned long long int)addr;
 }
 
 void WriteDestMem(UINT32 index, ADDRINT addr)
 {
   trace_instr_format_t* instr = static_cast<trace_instr_format_t*>(
       PIN_GetThreadData(tls_key, PIN_ThreadId()));
   if (index < NUM_INSTR_DESTINATIONS)
     instr->destination_memory[index] = (unsigned long long int)addr;
 }
 
 /* ===================================================================== */
 // Instrumentation
 /* ===================================================================== */
 
 VOID Instruction(INS ins, VOID* v)
 {
   INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)ResetCurrentInstruction,
                  IARG_INST_PTR, IARG_END);
 
   if (INS_IsBranch(ins))
     INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)BranchOrNot,
                    IARG_BRANCH_TAKEN, IARG_END);
 
   UINT32 readRegCount = INS_MaxNumRRegs(ins);
   for (UINT32 i = 0; i < readRegCount && i < NUM_INSTR_SOURCES; i++)
     INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)WriteSourceReg,
                    IARG_UINT32, i, IARG_UINT32, INS_RegR(ins, i), IARG_END);
 
   UINT32 writeRegCount = INS_MaxNumWRegs(ins);
   for (UINT32 i = 0; i < writeRegCount && i < NUM_INSTR_DESTINATIONS; i++)
     INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)WriteDestReg,
                    IARG_UINT32, i, IARG_UINT32, INS_RegW(ins, i), IARG_END);
 
   UINT32 memOperands = INS_MemoryOperandCount(ins);
   UINT32 src_mem_idx = 0, dst_mem_idx = 0;
   for (UINT32 memOp = 0; memOp < memOperands; memOp++) {
     if (INS_MemoryOperandIsRead(ins, memOp) && src_mem_idx < NUM_INSTR_SOURCES)
       INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)WriteSourceMem,
                      IARG_UINT32, src_mem_idx++, IARG_MEMORYOP_EA, memOp, IARG_END);
     if (INS_MemoryOperandIsWritten(ins, memOp) && dst_mem_idx < NUM_INSTR_DESTINATIONS)
       INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)WriteDestMem,
                      IARG_UINT32, dst_mem_idx++, IARG_MEMORYOP_EA, memOp, IARG_END);
   }
 
   INS_InsertIfCall(ins, IPOINT_BEFORE, (AFUNPTR)ShouldWrite, IARG_END);
   INS_InsertThenCall(ins, IPOINT_BEFORE, (AFUNPTR)WriteCurrentInstruction, IARG_END);
 }
 
 VOID ThreadStart(THREADID tid, CONTEXT* ctxt, INT32 flags, VOID* v)
 {
   trace_instr_format_t* instr = new trace_instr_format_t();
   *instr = {};
   PIN_SetThreadData(tls_key, instr, tid);
 }
 
 VOID ThreadFini(THREADID tid, const CONTEXT* ctxt, INT32 code, VOID* v)
 {
   trace_instr_format_t* instr = static_cast<trace_instr_format_t*>(
       PIN_GetThreadData(tls_key, tid));
   delete instr;
   PIN_SetThreadData(tls_key, nullptr, tid);
 }
 
 VOID DetachCallback(VOID* v)
 {
   PIN_GetLock(&write_lock, 1);
   if (outfile.is_open())
     outfile.close();
   PIN_ReleaseLock(&write_lock);
 }
 
 VOID Fini(INT32 code, VOID* v)
 {
   PIN_GetLock(&write_lock, 1);
   if (outfile.is_open())
     outfile.close();
   PIN_ReleaseLock(&write_lock);
 }
 
 /* ===================================================================== */
 // Main
 /* ===================================================================== */
 
 int main(int argc, char* argv[])
 {
   if (PIN_Init(argc, argv))
     return Usage();
 
   // Validate required knobs
   if (KnobSimPointsFile.Value().empty()) {
     std::cerr << "ERROR: -simpoints is required." << std::endl;
     return Usage();
   }
 
   // Parse simpoints file: "<sp_index> <cluster_id>" per line
   std::ifstream sp_file(KnobSimPointsFile.Value());
   if (!sp_file) {
     std::cerr << "ERROR: Could not open simpoints file: "
               << KnobSimPointsFile.Value() << std::endl;
     return 1;
   }
 
   UINT64 trace_len = KnobTraceLen.Value();
   UINT64 interval  = KnobInterval.Value();
   std::string outdir    = KnobOutputDir.Value();
   std::string benchmark = KnobBenchmark.Value();
 
   int sp_num = 0;
   UINT64 sp_index; int cluster_id;
   while (sp_file >> sp_index >> cluster_id) {
     SimPointWindow w;
     w.skip_start = sp_index * interval;
     w.skip_end   = w.skip_start + trace_len - 1;
     w.sp_num     = sp_num;
     w.cluster_id = cluster_id;
     // Output path — note: compression handled externally via named pipes
     w.outpath = outdir + "/" + benchmark
                 + "_sp" + std::to_string(sp_num)
                 + "_cluster" + std::to_string(cluster_id)
                 + ".champsimtrace.xz";  // raw binary written here; gen_champsim_traces.sh renames and compresses
     windows.push_back(w);
     sp_num++;
   }
   sp_file.close();
 
   if (windows.empty()) {
     std::cerr << "ERROR: No simpoints found in: "
               << KnobSimPointsFile.Value() << std::endl;
     return 1;
   }
 
   // Sort windows by skip_start so we advance through them in order
   std::sort(windows.begin(), windows.end(),
     [](const SimPointWindow& a, const SimPointWindow& b) {
       return a.skip_start < b.skip_start;
     });
 
   std::cout << "[tracer] Loaded " << windows.size() << " simpoints:" << std::endl;
   for (const auto& w : windows)
     std::cout << "  SP" << w.sp_num << " cluster=" << w.cluster_id
               << " skip=" << w.skip_start << " end=" << w.skip_end
               << " -> " << w.outpath << std::endl;
 
   PIN_InitLock(&write_lock);
 
   tls_key = PIN_CreateThreadDataKey(nullptr);
   if (tls_key == INVALID_TLS_KEY) {
     std::cerr << "ERROR: Could not create TLS key." << std::endl;
     return 1;
   }
 
   INS_AddInstrumentFunction(Instruction, 0);
   PIN_AddThreadStartFunction(ThreadStart, 0);
   PIN_AddThreadFiniFunction(ThreadFini, 0);
   PIN_AddDetachFunction(DetachCallback, 0);
   PIN_AddFiniFunction(Fini, 0);
 
   PIN_StartProgram();
   return 0;
 }