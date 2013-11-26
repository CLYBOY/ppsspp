// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "base/logging.h"
#include "Common/ArmEmitter.h"
#include "Common/CPUDetect.h"
#include "Core/MIPS/ARM/ArmRegCacheFPU.h"
#include "Core/MIPS/ARM/ArmJit.h"

using namespace ArmGen;

ArmRegCacheFPU::ArmRegCacheFPU(MIPSState *mips) : mips_(mips), vr(mr + 32) {
	if (cpu_info.bNEON) {
		numARMFpuReg_ = 32;
	} else {
		numARMFpuReg_ = 16;
	}
}

void ArmRegCacheFPU::Init(ARMXEmitter *emitter, MIPSComp::ArmJitOptions *jo) {
	emit_ = emitter;
	jo_ = jo;
}

void ArmRegCacheFPU::Start(MIPSAnalyst::AnalysisResults &stats) {
	qTime_ = 0;
	for (int i = 0; i < numARMFpuReg_; i++) {
		ar[i].mipsReg = -1;
		ar[i].isDirty = false;
	}
	for (int i = 0; i < NUM_MIPSFPUREG; i++) {
		mr[i].loc = ML_MEM;
		mr[i].reg = INVALID_REG;
		mr[i].spillLock = false;
		mr[i].tempLock = false;
	}
	for (int i = 0; i < MAX_ARMQUADS; i++) {
		qr[i].isDirty = false;
		qr[i].mipsVec = -1;
		qr[i].sz = V_Invalid;
		memset(qr[i].vregs, 0xff, 4);
	}
}

const ARMReg *ArmRegCacheFPU::GetMIPSAllocationOrder(int &count) {
	// We reserve S0-S1 as scratch. Can afford two registers. Maybe even four, which could simplify some things.
	static const ARMReg allocationOrder[] = {
							S2,  S3,
		S4,  S5,  S6,  S7,
		S8,  S9,  S10, S11,
		S12, S13, S14, S15
	};

	// VFP mapping
	// VFPU registers and regular FP registers are mapped interchangably on top of the standard
	// 16 FPU registers.

	// NEON mapping
	// We map FPU and VFPU registers entirely separately. FPU is mapped to 12 of the bottom 16 S registers.
	// VFPU is mapped to the upper 48 regs, 32 of which can only be reached through NEON
	// (or D16-D31 as doubles, but not relevant).
	// Might consider shifting the split in the future, giving more regs to NEON allowing it to map more quads.
	
	// We should attempt to map scalars to low Q registers and wider things to high registers,
	// as the NEON instructions are all 2-vector or 4-vector, they don't do scalar, we want to be
	// able to use regular VFP instructions too.
	static const ARMReg allocationOrderNEON[] = {
		// Reserve four temp registers. Useful when building quads until we really figure out
		// how to do that best.
		S4,  S5,  S6,  S7,   // Q1
		S8,  S9,  S10, S11,  // Q2
		S12, S13, S14, S15,  // Q3
		// Q4-Q15 free for VFPU
	};

	if (jo_->useNEONVFPU) {
		count = sizeof(allocationOrderNEON) / sizeof(const int);
		return allocationOrderNEON;
	} else {
		count = sizeof(allocationOrder) / sizeof(const int);
		return allocationOrder;
	}
}

ARMReg ArmRegCacheFPU::MapReg(MIPSReg mipsReg, int mapFlags) {
	// Let's see if it's already mapped. If so we just need to update the dirty flag.
	// We don't need to check for ML_NOINIT because we assume that anyone who maps
	// with that flag immediately writes a "known" value to the register.
	if (mr[mipsReg].loc == ML_ARMREG) {
		if (ar[mr[mipsReg].reg].mipsReg != mipsReg) {
			ERROR_LOG(JIT, "Reg mapping out of sync! MR %i", mipsReg);
		}
		if (mapFlags & MAP_DIRTY) {
			ar[mr[mipsReg].reg].isDirty = true;
		}
		//INFO_LOG(JIT, "Already mapped %i to %i", mipsReg, mr[mipsReg].reg);
		return (ARMReg)(mr[mipsReg].reg + S0);
	}

	// Okay, not mapped, so we need to allocate an ARM register.

	int allocCount;
	const ARMReg *allocOrder = GetMIPSAllocationOrder(allocCount);

allocate:
	for (int i = 0; i < allocCount; i++) {
		int reg = allocOrder[i] - S0;

		if (ar[reg].mipsReg == -1) {
			// That means it's free. Grab it, and load the value into it (if requested).
			ar[reg].isDirty = (mapFlags & MAP_DIRTY) ? true : false;
			if (!(mapFlags & MAP_NOINIT)) {
				if (mr[mipsReg].loc == ML_MEM && mipsReg < TEMP0) {
					emit_->VLDR((ARMReg)(reg + S0), CTXREG, GetMipsRegOffset(mipsReg));
				}
			}
			ar[reg].mipsReg = mipsReg;
			mr[mipsReg].loc = ML_ARMREG;
			mr[mipsReg].reg = reg;
			//INFO_LOG(JIT, "Mapped %i to %i", mipsReg, mr[mipsReg].reg);
			return (ARMReg)(reg + S0);
		}
	}

	// Still nothing. Let's spill a reg and goto 10.
	// TODO: Use age or something to choose which register to spill?
	// TODO: Spill dirty regs first? or opposite?
	int bestToSpill = -1;
	for (int i = 0; i < allocCount; i++) {
		int reg = allocOrder[i] - S0;
		if (ar[reg].mipsReg != -1 && (mr[ar[reg].mipsReg].spillLock || mr[ar[reg].mipsReg].tempLock))
			continue;
		bestToSpill = reg;
		break;
	}

	if (bestToSpill != -1) {
		FlushArmReg((ARMReg)(S0 + bestToSpill));
		goto allocate;
	}

	// Uh oh, we have all them spilllocked....
	ERROR_LOG(JIT, "Out of spillable registers at PC %08x!!!", mips_->pc);
	return INVALID_REG;
}

void ArmRegCacheFPU::MapInIn(MIPSReg rd, MIPSReg rs) {
	SpillLock(rd, rs);
	MapReg(rd);
	MapReg(rs);
	ReleaseSpillLock(rd);
	ReleaseSpillLock(rs);
}

void ArmRegCacheFPU::MapDirtyIn(MIPSReg rd, MIPSReg rs, bool avoidLoad) {
	SpillLock(rd, rs);
	bool overlap = avoidLoad && rd == rs;
	MapReg(rd, MAP_DIRTY | (overlap ? 0 : MAP_NOINIT));
	MapReg(rs);
	ReleaseSpillLock(rd);
	ReleaseSpillLock(rs);
}

void ArmRegCacheFPU::MapDirtyInIn(MIPSReg rd, MIPSReg rs, MIPSReg rt, bool avoidLoad) {
	SpillLock(rd, rs, rt);
	bool overlap = avoidLoad && (rd == rs || rd == rt);
	MapReg(rd, MAP_DIRTY | (overlap ? 0 : MAP_NOINIT));
	MapReg(rt);
	MapReg(rs);
	ReleaseSpillLock(rd);
	ReleaseSpillLock(rs);
	ReleaseSpillLock(rt);
}

void ArmRegCacheFPU::SpillLockV(const u8 *v, VectorSize sz) {
	for (int i = 0; i < GetNumVectorElements(sz); i++) {
		vr[v[i]].spillLock = true;
	}
}

void ArmRegCacheFPU::SpillLockV(int vec, VectorSize sz) {
	u8 v[4];
	GetVectorRegs(v, sz, vec);
	SpillLockV(v, sz);
}

void ArmRegCacheFPU::MapRegV(int vreg, int flags) {
	MapReg(vreg + 32, flags);
}

void ArmRegCacheFPU::LoadToRegV(ARMReg armReg, int vreg) {
	if (vr[vreg].loc == ML_ARMREG) {
		emit_->VMOV(armReg, (ARMReg)(S0 + vr[vreg].reg));
	} else {
		MapRegV(vreg);
		emit_->VMOV(armReg, V(vreg));
	}
}

void ArmRegCacheFPU::MapRegsAndSpillLockV(int vec, VectorSize sz, int flags) {
	u8 v[4];
	GetVectorRegs(v, sz, vec);
	SpillLockV(v, sz);
	for (int i = 0; i < GetNumVectorElements(sz); i++) {
		MapRegV(v[i], flags);
	}
}

void ArmRegCacheFPU::MapRegsAndSpillLockV(const u8 *v, VectorSize sz, int flags) {
	SpillLockV(v, sz);
	for (int i = 0; i < GetNumVectorElements(sz); i++) {
		MapRegV(v[i], flags);
	}
}

void ArmRegCacheFPU::MapInInV(int vs, int vt) {
	SpillLockV(vs);
	SpillLockV(vt);
	MapRegV(vs);
	MapRegV(vt);
	ReleaseSpillLockV(vs);
	ReleaseSpillLockV(vt);
}

void ArmRegCacheFPU::MapDirtyInV(int vd, int vs, bool avoidLoad) {
	bool overlap = avoidLoad && (vd == vs);
	SpillLockV(vd);
	SpillLockV(vs);
	MapRegV(vd, MAP_DIRTY | (overlap ? 0 : MAP_NOINIT));
	MapRegV(vs);
	ReleaseSpillLockV(vd);
	ReleaseSpillLockV(vs);
}

void ArmRegCacheFPU::MapDirtyInInV(int vd, int vs, int vt, bool avoidLoad) {
	bool overlap = avoidLoad && ((vd == vs) || (vd == vt));
	SpillLockV(vd);
	SpillLockV(vs);
	SpillLockV(vt);
	MapRegV(vd, MAP_DIRTY | (overlap ? 0 : MAP_NOINIT));
	MapRegV(vs);
	MapRegV(vt);
	ReleaseSpillLockV(vd);
	ReleaseSpillLockV(vs);
	ReleaseSpillLockV(vt);
}

void ArmRegCacheFPU::FlushArmReg(ARMReg r) {
	if (r >= S0 && r <= S31) {
		int reg = r - S0;
		if (ar[reg].mipsReg == -1) {
			// Nothing to do, reg not mapped.
			return;
		}
		if (ar[reg].mipsReg != -1) {
			if (ar[reg].isDirty && mr[ar[reg].mipsReg].loc == ML_ARMREG)
			{
				//INFO_LOG(JIT, "Flushing ARM reg %i", reg);
				emit_->VSTR(r, CTXREG, GetMipsRegOffset(ar[reg].mipsReg));
			}
			// IMMs won't be in an ARM reg.
			mr[ar[reg].mipsReg].loc = ML_MEM;
			mr[ar[reg].mipsReg].reg = INVALID_REG;
		} else {
			ERROR_LOG(JIT, "Dirty but no mipsreg?");
		}
		ar[reg].isDirty = false;
		ar[reg].mipsReg = -1;
	} else if (r >= D0 && r <= D31) {
		// TODO: Convert to S regs and flush them individually.
	} else if (r >= Q0 && r <= Q15) {
		// TODO
	}
}

void ArmRegCacheFPU::FlushR(MIPSReg r) {
	switch (mr[r].loc) {
	case ML_IMM:
		// IMM is always "dirty".
		// IMM is not allowed for FP (yet).
		ERROR_LOG(JIT, "Imm in FP register?");
		break;

	case ML_ARMREG:
		if (mr[r].reg == (int)INVALID_REG) {
			ERROR_LOG(JIT, "FlushR: MipsReg had bad ArmReg");
		}

		if (mr[r].reg >= Q0 && mr[r].reg <= Q15) {
			// This should happen rarely, but occasionally we need to flush a single stray
			// mipsreg that's been part of a quad.
			int quad = mr[r].reg - Q0;
			if (qr[quad].isDirty) {
				emit_->ADD(R0, CTXREG, GetMipsRegOffset(r));
				emit_->VST1_lane(F_32, (ARMReg)mr[r].reg, R0, mr[r].lane, true);
			}
		} else {
			if (ar[mr[r].reg].isDirty) {
				//INFO_LOG(JIT, "Flushing dirty reg %i", mr[r].reg);
				emit_->VSTR((ARMReg)(mr[r].reg + S0), CTXREG, GetMipsRegOffset(r));
				ar[mr[r].reg].isDirty = false;
			}
			ar[mr[r].reg].mipsReg = -1;
		}
		break;

	case ML_MEM:
		// Already there, nothing to do.
		break;

	default:
		//BAD
		break;
	}
	mr[r].loc = ML_MEM;
	mr[r].reg = (int)INVALID_REG;
}

void ArmRegCacheFPU::DiscardR(MIPSReg r) {
	switch (mr[r].loc) {
	case ML_IMM:
		// IMM is always "dirty".
		// IMM is not allowed for FP (yet).
		ERROR_LOG(JIT, "Imm in FP register?");
		break;
		 
	case ML_ARMREG:
		if (mr[r].reg == (int)INVALID_REG) {
			ERROR_LOG(JIT, "DiscardR: MipsReg had bad ArmReg");
		}
		// Note that we DO NOT write it back here. That's the whole point of Discard.
		ar[mr[r].reg].isDirty = false;
		ar[mr[r].reg].mipsReg = -1;
		break;

	case ML_MEM:
		// Already there, nothing to do.
		break;

	default:
		//BAD
		break;
	}
	mr[r].loc = ML_MEM;
	mr[r].reg = (int)INVALID_REG;
	mr[r].tempLock = false;
	mr[r].spillLock = false;
}


bool ArmRegCacheFPU::IsTempX(ARMReg r) const {
	return ar[r - S0].mipsReg >= TEMP0;
}

int ArmRegCacheFPU::GetTempR() {
	for (int r = TEMP0; r < TEMP0 + NUM_TEMPS; ++r) {
		if (mr[r].loc == ML_MEM && !mr[r].tempLock) {
			mr[r].tempLock = true;
			return r;
		}
	}

	ERROR_LOG(CPU, "Out of temp regs! Might need to DiscardR() some");
	_assert_msg_(JIT, 0, "Regcache ran out of temp regs, might need to DiscardR() some.");
	return -1;
}


void ArmRegCacheFPU::FlushAll() {
	// Discard temps!
	for (int i = TEMP0; i < TEMP0 + NUM_TEMPS; i++) {
		DiscardR(i);
	}

	// Flush quads!
	for (int i = 0; i < MAX_ARMQUADS; i++) {
		QFlush(i);
	}

	for (int i = 0; i < NUM_MIPSFPUREG; i++) {
		FlushR(i);
	} 
	// Sanity check
	for (int i = 0; i < numARMFpuReg_; i++) {
		if (ar[i].mipsReg != -1) {
			ERROR_LOG(JIT, "Flush fail: ar[%i].mipsReg=%i", i, ar[i].mipsReg);
		}
	}
}

int ArmRegCacheFPU::GetMipsRegOffset(MIPSReg r) {
	// These are offsets within the MIPSState structure. First there are the GPRS, then FPRS, then the "VFPURs", then the VFPU ctrls.
	if (r < 32 + 128 + NUM_TEMPS)
		return (r + 32) << 2;
	ERROR_LOG(JIT, "bad mips register %i, out of range", r);
	return 0;  // or what?
}

void ArmRegCacheFPU::SpillLock(MIPSReg r1, MIPSReg r2, MIPSReg r3, MIPSReg r4) {
	mr[r1].spillLock = true;
	if (r2 != -1) mr[r2].spillLock = true;
	if (r3 != -1) mr[r3].spillLock = true;
	if (r4 != -1) mr[r4].spillLock = true;
}

// This is actually pretty slow with all the 160 regs...
void ArmRegCacheFPU::ReleaseSpillLocksAndDiscardTemps() {
	for (int i = 0; i < NUM_MIPSFPUREG; i++)
		mr[i].spillLock = false;
	for (int i = TEMP0; i < TEMP0 + NUM_TEMPS; ++i)
		DiscardR(i);
}

ARMReg ArmRegCacheFPU::R(int mipsReg) {
	if (mr[mipsReg].loc == ML_ARMREG) {
		return (ARMReg)(mr[mipsReg].reg + S0);
	} else {
		if (mipsReg < 32) {
			ERROR_LOG(JIT, "FReg %i not in ARM reg. compilerPC = %08x : %s", mipsReg, compilerPC_, currentMIPS->DisasmAt(compilerPC_));
		} else if (mipsReg < 32 + 128) {
			ERROR_LOG(JIT, "VReg %i not in ARM reg. compilerPC = %08x : %s", mipsReg - 32, compilerPC_, currentMIPS->DisasmAt(compilerPC_));
		} else {
			ERROR_LOG(JIT, "Tempreg %i not in ARM reg. compilerPC = %08x : %s", mipsReg - 128 - 32, compilerPC_, currentMIPS->DisasmAt(compilerPC_));
		}
		return INVALID_REG;  // BAAAD
	}
}

inline ARMReg QuadAsD(int quad) {
	return (ARMReg)(D0 + quad * 2);
}

inline ARMReg QuadAsQ(int quad) {
	return (ARMReg)(Q0 + quad);
}

bool MappableQ(int quad) {
	return quad >= 4;
}

void ArmRegCacheFPU::QFlush(int quad) {
	if (!MappableQ(quad))
		return;

	ARMReg q = QuadAsQ(quad);
	if (qr[quad].isDirty) {
		// Unlike reads, when writing to the register file we need to be careful to write the correct
		// number of floats.

		// TODO: Optimize to multi-write when possible.
		switch (qr[quad].sz) {
		case V_Single:
			emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(qr[quad].vregs[0]), R1);
			emit_->VST1_lane(F_32, QuadAsQ(quad), R0, 0, true);
			break;
		case V_Pair:
			if (qr[quad].vregs[1] == qr[quad].vregs[0] + 1) {
				// Can combine, it's a column!
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(qr[quad].vregs[0]), R1);
				emit_->VST1(F_32, QuadAsD(quad), R0, 1, ALIGN_NONE);  // TODO: Allow ALIGN_64 when applicable
			} else {
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(qr[quad].vregs[0]), R1);
				emit_->VST1_lane(F_32, QuadAsQ(quad), R0, 0, true);
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(qr[quad].vregs[1]), R1);
				emit_->VST1_lane(F_32, QuadAsQ(quad), R0, 1, true);
			}
			break;
		case V_Triple:
			if (qr[quad].vregs[2] == qr[quad].vregs[1] + 1 && qr[quad].vregs[1] == qr[quad].vregs[0] + 1) {
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(qr[quad].vregs[0]), R1);
				emit_->VST1(F_32, QuadAsD(quad), R0, 1, ALIGN_NONE, REG_UPDATE);  // TODO: Allow ALIGN_64 when applicable
				emit_->VST1_lane(F_32, QuadAsQ(quad), R0, 2, true);
			} else {
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(qr[quad].vregs[0]), R1);
				emit_->VST1_lane(F_32, QuadAsQ(quad), R0, 0, true);
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(qr[quad].vregs[1]), R1);
				emit_->VST1_lane(F_32, QuadAsQ(quad), R0, 1, true);
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(qr[quad].vregs[2]), R1);
				emit_->VST1_lane(F_32, QuadAsQ(quad), R0, 2, true);
			}
			break;
		case V_Quad:
			if (qr[quad].vregs[3] == qr[quad].vregs[2] + 1 && qr[quad].vregs[2] == qr[quad].vregs[1] + 1 && qr[quad].vregs[1] == qr[quad].vregs[0] + 1) {
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(qr[quad].vregs[0]), R1);
				emit_->VST1(F_32, QuadAsD(quad), R0, 2, ALIGN_NONE);  // TODO: Allow ALIGN_64 when applicable
			} else {
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(qr[quad].vregs[0]), R1);
				emit_->VST1_lane(F_32, QuadAsQ(quad), R0, 0, true);
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(qr[quad].vregs[1]), R1);
				emit_->VST1_lane(F_32, QuadAsQ(quad), R0, 1, true);
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(qr[quad].vregs[2]), R1);
				emit_->VST1_lane(F_32, QuadAsQ(quad), R0, 2, true);
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(qr[quad].vregs[3]), R1);
				emit_->VST1_lane(F_32, QuadAsQ(quad), R0, 3, true);
			}
			break;
		}

		qr[quad].isDirty = false;
		qr[quad].mipsVec = -1;

		int n = GetNumVectorElements(qr[quad].sz);
		for (int i = 0; i < n; i++) {
			FPURegMIPS &m = mr[32 + qr[quad].vregs[i]];
			m.loc = ML_MEM;
			m.lane = -1;
		}
	}
}


ARMReg ArmRegCacheFPU::QMapReg(int vreg, VectorSize sz, int flags) {
	qTime_++;

	u8 vregs[4];
	u8 regsQuad[4];
	GetVectorRegs(vregs, sz, vreg);
	GetVectorRegs(regsQuad, V_Quad, vreg);   // Same but as quad.

	// Let's check if they are all mapped in a quad somewhere.
	// Later we can check for possible transposes as well.
	int n = GetNumVectorElements(sz);
	for (int q = 0; q < 16; q++) {
		if (!MappableQ(q))
			continue;
		
		// Compare subregs to what's already in this register.
		int matchCount = 0;
		for (int i = 0; i < n; i++) {
			if (qr[q].vregs[i] == vregs[i])
				matchCount++;
			else
				break;
		}

		// Old was shorter!
		if (matchCount > 0 && matchCount < n) {
			// OK, let's extend by loading the missing elements.
			for (int i = matchCount; i < n; i++) {
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[i]), R1);
				emit_->VLD1_lane(F_32, QuadAsQ(q), R0, i, true);
				qr[q].vregs[i] = vregs[i];
			}
			matchCount = n;
		} else if (matchCount == n) {
			if (qr[q].sz > sz) {
				// Discard overshooting elements for now
				for (int j = n; j < GetNumVectorElements(qr[q].sz); j++) {
					FlushV(qr[q].vregs[j]);
				}
			}
		}

		// TODO: Check for longer too.
		// We have this already! Just check that it isn't longer
		// than necessary. We need to wipe stray extra regs if we intend to write (only! TODO - but must then wipe when changing to dirty)
		// If we only return a D, might not actually need to wipe the upper two.
		// But for now, let's keep it simple.
		/*
		for (int j = n; j < 4; j++) {
			if (ar[r + j].mipsReg == regsQuad[j]) {
				FlushR(ar[r + j].mipsReg);
			}
		}*/

		// TODO: Check for other types of overlap and do the clever thing.

		if (matchCount == n) {
			if (flags & MAP_DIRTY) {
				qr[q].isDirty = true;
			}
			qr[q].sz = sz;
			switch (sz) {
			case V_Single:
			case V_Pair:
				return QuadAsD(q);
			case V_Triple:
			case V_Quad:
				return QuadAsQ(q);
			}
		}
	}

retry:
	// No match, not mapped yet.
	// Search for a free quad. A quad is free if the first register in it is free.
	int quad = -1;
	for (int q = 0; q < 16; q++) {
		if (!MappableQ(q))
			continue;

		if (qr[q].mipsVec == INVALID_REG) {
			// Oh yeah!
			quad = q;
			break;
		}
	}

	if (quad != -1) {
		goto gotQuad;
	}

	// Flush something, anything!
	// TODO

	ERROR_LOG(JIT, "Failed finding a free quad. Zapping one and continuing.");
	goto retry;

gotQuad:
	// If parts of our register are elsewhere, we need to flush them before we reload
	// in a new location.
	// This may be problematic if inputs overlap, say:
	// vdot S700, R000, C000
	// It might still work by accident...
	if (flags & MAP_DIRTY) {
		for (int i = 0; i < n; i++) {
			FlushV(vregs[i]);
		}
	}

	// Flush out anything left behind in our new fresh quad.
	QFlush(quad);

	qr[quad].sz = sz;
	qr[quad].mipsVec = vreg;

	if (!(flags & MAP_NOINIT)) {
		// Okay, now we will try to load the whole thing in one go. This is possible
		// if it's a column and easy if it's a single.
		switch (sz) {
		case V_Single:
			emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[0]), R1);
			emit_->VLD1_lane(F_32, QuadAsQ(quad), R0, 0, true);
			break;
		case V_Pair:
			if (vregs[1] == vregs[0] + 1) {
				// Can combine, it's a column!
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[0]), R1);
				emit_->VLD1(F_32, QuadAsD(quad), R0, 1, ALIGN_NONE);  // TODO: Allow ALIGN_64 when applicable
			} else {
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[0]), R1);
				emit_->VLD1_lane(F_32, QuadAsQ(quad), R0, 0, true);
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[1]), R1);
				emit_->VLD1_lane(F_32, QuadAsQ(quad), R0, 1, true);
			}
			break;
		case V_Triple:
			if (vregs[1] == vregs[0] + 1) {
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[0]), R1);
				emit_->VLD1(F_32, QuadAsD(quad), R0, 1, ALIGN_NONE, REG_UPDATE);  // TODO: Allow ALIGN_64 when applicable
				emit_->VLD1_lane(F_32, QuadAsQ(quad), R0, 2, true);
			} else {
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[0]), R1);
				emit_->VLD1_lane(F_32, QuadAsQ(quad), R0, 0, true);
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[1]), R1);
				emit_->VLD1_lane(F_32, QuadAsQ(quad), R0, 1, true);
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[2]), R1);
				emit_->VLD1_lane(F_32, QuadAsQ(quad), R0, 2, true);
			}
			break;
		case V_Quad:
			if (vregs[3] == vregs[2] + 1 && vregs[2] == vregs[1] + 1 && vregs[1] == vregs[0] + 1) {
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[0]), R1);
				emit_->VLD1(F_32, QuadAsD(quad), R0, 2, ALIGN_NONE);  // TODO: Allow ALIGN_64 when applicable
			} else {
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[0]), R1);
				emit_->VLD1_lane(F_32, QuadAsQ(quad), R0, 0, true);
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[1]), R1);
				emit_->VLD1_lane(F_32, QuadAsQ(quad), R0, 1, true);
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[2]), R1);
				emit_->VLD1_lane(F_32, QuadAsQ(quad), R0, 2, true);
				emit_->ADDI2R(R0, CTXREG, GetMipsRegOffsetV(vregs[3]), R1);
				emit_->VLD1_lane(F_32, QuadAsQ(quad), R0, 3, true);
			}
			break;
		}
	}

	// OK, let's fill out the arrays to confirm that we have grabbed these registers.
	for (int i = 0; i < n; i++) {
		int mipsReg = 32 + vregs[i];
		mr[mipsReg].loc = ML_ARMREG;
		mr[mipsReg].reg = QuadAsQ(quad);
		mr[mipsReg].lane = i;
		qr[quad].vregs[i] = vregs[i];
		qr[quad].isDirty = (flags & MAP_DIRTY) != 0;
	}

	return QuadAsQ(quad);
}

ARMReg ArmRegCacheFPU::QAllocTemp() {
	// TODO
	return Q15;
}
