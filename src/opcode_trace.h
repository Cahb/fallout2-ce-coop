#ifndef FALLOUT_OPCODE_TRACE_H_
#define FALLOUT_OPCODE_TRACE_H_

namespace fallout {

// Script-opcode tracing, a consumer of the interpreter's public hook seam
// (interpreter.h). It answers "WHICH script did that?" — the question this
// codebase makes expensive, because the answer lives in compiled .int bytecode
// inside master.dat rather than in anything you can grep.
//
//   F2_TRACE_OPCODE=all                    every opcode
//   F2_TRACE_OPCODE=add_obj_to_inven       substring match on the opcode name
//   F2_TRACE_OPCODE=inven,give_exp_points  comma-separated, any match wins
//
// Installs NOTHING when the variable is unset, so the interpreter keeps its
// original dispatch path and every golden stays byte-identical.
void opcodeTraceInstall();

// Script-language name for an opcode ("op_add_obj_to_inven"), or nullptr.
const char* opcodeGetName(int opcode);

} // namespace fallout

#endif /* FALLOUT_OPCODE_TRACE_H_ */
