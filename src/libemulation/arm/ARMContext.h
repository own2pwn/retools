/*
 * ARMContext.h
 *
 *  Created on: Oct 10, 2015
 *      Author: anon
 */

#ifndef SRC_LIBDISASSEMBLY_ARM_ARMCONTEXT_H_
#define SRC_LIBDISASSEMBLY_ARM_ARMCONTEXT_H_

#include "arm/ARMArch.h"
#include <cstdint>

class ARMContext {
public:
	ARMContext();
	virtual ~ARMContext();

	void setRegister(Register::Core reg, uint32_t value);
	void getRegister(Register::Core reg, uint32_t &value);
	void setRegister(Register::Coproc reg, uint32_t value);
	void getRegister(Register::Coproc reg, uint32_t &value);
	void setRegister(Register::Single reg, uint32_t value);
	void getRegister(Register::Single reg, uint32_t &value);
	void setRegister(Register::Double reg, uint64_t value);
	void getRegister(Register::Double reg, uint64_t &value);
	void setRegister(Register::Quad reg, uint32_t value);
	void getRegister(Register::Quad reg, uint32_t &value);

	uint32_t readRegularRegister(unsigned regno);
	uint32_t readRmode(unsigned regno, unsigned n);
	uint32_t readSingleRegister(unsigned regno);
	uint64_t readDoubleRegister(unsigned regno);
	uint64_t readQuadRegister(unsigned regno);

	uint32_t readMemory(uintptr_t address, unsigned size);
	uint32_t writeRegularRegister(unsigned regno, uintptr_t value);
	uint32_t writeRmode(unsigned regno, unsigned size, uintptr_t value);
	uint32_t writeSingleRegister(unsigned regno, float value);
	uint32_t writeDoubleRegister(unsigned regno, double value);
	uint32_t writeQuadRegister(unsigned regno, uint64_t value);
	uint32_t writeMemory(uintptr_t address, unsigned size, uintptr_t value);
	uint32_t readElement(uintptr_t address, uintptr_t value, unsigned size);
	uint32_t writeElement(uintptr_t address, unsigned size, uintptr_t value, unsigned what___);

	void ALUWritePC(uint32_t address) {
		return;
	}

	// Return the value of PC used when storing, this may be +4 or +8.
	uint32_t PCStoreValue() {
		return readRegularRegister(15);
	}

	ARMMode CurrentInstrSet() {
	    return m_opcode_mode;
	}

	bool IsZero(unsigned i) {
		return i == 0;
	}

	bool InITBlock() {
	    return CurrentInstrSet() == InstrSet_Thumb && m_it_session.InITBlock();
	}

	bool LastInITBlock() {
	    return CurrentInstrSet() == InstrSet_Thumb && m_it_session.LastInITBlock();
	}

	bool CurrentModeIsHyp() {
		return m_hyp_mode;
	}

	ARMVariants ArchVersion() {
		return m_arm_isa;
	}

	// TODO: Implement.
	void BranchTo(uintptr_t address) {
		return;
	}

	void UNPREDICTABLE() {
		return;
	}

	void BranchWritePC(uintptr_t address) {
		if (CurrentInstrSet() == InstrSet_ARM) {
			if (ArchVersion() < 6 && (address & 3) != 0) {
				UNPREDICTABLE();
			}

			BranchTo(address & 0xfffffffc);
		} else if (CurrentInstrSet() == InstrSet_Jazelle) {
			BranchTo(address);
		} else {
			BranchTo(address & 1);
		}
	}

private:
    bool m_hyp_mode;
    ITSession m_it_session;
    ARMMode m_opcode_mode;
    ARMVariants m_arm_isa;
    apsr_t APSR;
    fpscr_t FPSCR;

    // Registers.
	uint32_t m_core_regs[Register::ARM_REG_CORE_MAX];
	uint32_t m_coproc_regs[Register::ARM_REG_COPROC_MAX];
	uint32_t m_single_regs[Register::ARM_REG_SINGLE_MAX];
	uint64_t m_double_regs[Register::ARM_REG_DOUBLE_MAX];
	uint64_t m_quad_regs[Register::ARM_REG_QUAD_MAX];
};

#endif /* SRC_LIBDISASSEMBLY_ARM_ARMCONTEXT_H_ */
