/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2009  PCSX2 Dev Team
 * 
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once
#include "VU.h"
#include "VUops.h"
#include "R5900.h"
#define vuRunCycles  (512*12)  // Cycles to run ExecuteBlockJIT() for (called from within recs)
#define vu0RunCycles (512*12)  // Cycles to run vu0 for whenever ExecuteBlock() is called
#define vu1RunCycles (3000000) // mVU1 uses this for inf loop detection on dev builds

// --------------------------------------------------------------------------------------
//  BaseCpuProvider
// --------------------------------------------------------------------------------------
//
// Design Note: This class is only partial C++ style.  It still relies on Alloc and Shutdown
// calls for memory and resource management.  This is because the underlying implementations
// of our CPU emulators don't have properly encapsulated objects yet -- if we allocate ram
// in a constructor, it won't get free'd if an exception occurs during object construction.
// Once we've resolved all the 'dangling pointers' and stuff in the recompilers, Alloc
// and Shutdown can be removed in favor of constructor/destructor syntax.
//
class BaseCpuProvider
{
protected:
	// allocation counter for multiple init/shutdown calls
	// (most or all implementations will need this!)
	int		m_AllocCount;

public:
	// this boolean indicates to some generic logging facilities if the VU's registers
	// are valid for logging or not. (see DisVU1Micro.cpp, etc)  [kinda hacky, might
	// be removed in the future]
	bool	IsInterpreter;

public:
	BaseCpuProvider()
	{
		m_AllocCount   = 0;
	}

	virtual ~BaseCpuProvider() throw()
	{
		if( m_AllocCount != 0 )
			Console.Warning( "Cleanup miscount detected on CPU provider.  Count=%d", m_AllocCount );
	}

	virtual const char* GetShortName() const=0;
	virtual wxString GetLongName() const=0;

	virtual void Allocate()=0;
	virtual void Shutdown()=0;
	virtual void Reset()=0;
	virtual void Execute(u32 cycles)=0;
	virtual void ExecuteBlock(bool startUp)=0; 

	virtual void Step()=0;
	virtual void Clear(u32 Addr, u32 Size)=0;

	// C++ Calling Conventions are unstable, and some compilers don't even allow us to take the
	// address of C++ methods.  We need to use a wrapper function to invoke the ExecuteBlock from
	// recompiled code.
	static void __fastcall ExecuteBlockJIT( BaseCpuProvider* cpu ) 
	{
		cpu->Execute(vuRunCycles);
	}
};

// --------------------------------------------------------------------------------------
//  BaseVUmicroCPU
// --------------------------------------------------------------------------------------
// Layer class for possible future implementation (currently is nothing more than a type-safe
// type define).
//
class BaseVUmicroCPU : public BaseCpuProvider {
protected:
	u32 m_lastEEcycles;
	int m_Idx;
	BaseVUmicroCPU() {
		m_Idx		   = 0;
		m_lastEEcycles = 0;
	}
	virtual ~BaseVUmicroCPU() throw() {}
public:

	// Called by the PS2 VM's event manager for every internal vertical sync (occurs at either
	// 50hz (pal) or 59.94hz (NTSC).
	//
	// Exceptions:
	//   This method is not allowed to throw exceptions, since exceptions may not propagate
	//   safely from the context of recompiled code stackframes.
	//
	// Thread Affinity:
	//   Called from the EEcore thread.  No locking is performed, so any necessary locks must
	//   be implemented by the CPU provider manually.
	//
	virtual void Vsync() throw() { }

	virtual void Step() {
		// Ideally this would fall back on interpretation for executing single instructions
		// for all CPU types, but due to VU complexities and large discrepancies between
		// clamping in recs and ints, it's not really worth bothering with yet.
	}

	// Execute VU for the number of VU cycles (recs might go over 0~30 cycles)
	//virtual void Execute(u32 cycles)=0;
	
	// Executes a Block based on static preset cycles
	virtual void ExecuteBlock(bool startUp=0) {
		const int vuRunning = m_Idx ? 0x100 : 1;
		if (!(VU0.VI[REG_VPU_STAT].UL & vuRunning)) return;
		if (startUp) { // Start Executing a microprogram
			Execute(vu0RunCycles); // Kick start VU
			// If the VU0 program didn't finish then we'll want to finish it up
			// pretty soon.  This fixes vmhacks in some games (Naruto Ultimate Ninja 2)
			if(VU0.VI[REG_VPU_STAT].UL & 0x1)
				cpuSetNextBranchDelta( 192 ); // fixme : ideally this should be higher, like 512 or so.
		}
		else {
			Execute(vu0RunCycles);
			DevCon.Warning("VU0 running when in BranchTest");
			// This helps keep the EE and VU0 in sync.
			// Check Silver Surfer. Currently has SPS varying with different branch deltas set below.
			if(VU0.VI[REG_VPU_STAT].UL & 0x1)
				cpuSetNextBranchDelta( 768 );
		}
	}

	// Executes a Block based on EE delta time
	/*virtual void ExecuteBlock(bool startUp=0) {
		const int vuRunning = m_Idx ? 0x100 : 1;
		const int c = 1024;
		if (!(VU0.VI[REG_VPU_STAT].UL & vuRunning)) return;
		if (startUp) {  // Start Executing a microprogram
			Execute(c); // Kick start VU
			m_lastEEcycles = cpuRegs.cycle + (c*2);
			if (VU0.VI[REG_VPU_STAT].UL & vuRunning)
				cpuSetNextBranchDelta(c*4); // Let VUs run behind EE instead of ahead		
		}
		else { // Continue Executing (VU roughly half the mhz of EE)
			s32 delta = (s32)(u32)(cpuRegs.cycle - m_lastEEcycles);
			if (delta > 0) {
				delta>>=1;
				DevCon.WriteLn("Event Test1: Running VU0 for %d cycles", delta);
				Execute(delta);
				m_lastEEcycles = cpuRegs.cycle;
				if (VU0.VI[REG_VPU_STAT].UL & vuRunning)
					cpuSetNextBranchDelta(c*2); // Let VUs run behind EE instead of in front
			}
			else {
				cpuSetNextBranchDelta(-delta);
				DevCon.WriteLn("Event Test2: Running VU0 for %d cycles", delta/2);
			}
		}
	}*/
};


// --------------------------------------------------------------------------------------
//  InterpVU0 / InterpVU1
// --------------------------------------------------------------------------------------
class InterpVU0 : public BaseVUmicroCPU
{
public:
	InterpVU0();
	virtual ~InterpVU0() throw() { Shutdown(); }

	const char* GetShortName() const	{ return "intVU0"; }
	wxString GetLongName() const		{ return L"VU0 Interpreter"; }

	void Allocate() { }
	void Shutdown() throw() { }
	void Reset() { }

	void Step();
	void Execute(u32 cycles);
	void Clear(u32 addr, u32 size) {}
};

class InterpVU1 : public BaseVUmicroCPU
{
public:
	InterpVU1();
	virtual ~InterpVU1() throw() { Shutdown(); }

	const char* GetShortName() const	{ return "intVU1"; }
	wxString GetLongName() const		{ return L"VU1 Interpreter"; }

	void Allocate() { }
	void Shutdown() throw() { }
	void Reset() { }

	void Step();
	void Execute(u32 cycles);
	void Clear(u32 addr, u32 size) {}
};

// --------------------------------------------------------------------------------------
//  recMicroVU0 / recMicroVU1
// --------------------------------------------------------------------------------------
class recMicroVU0 : public BaseVUmicroCPU
{
public:
	recMicroVU0();
	virtual ~recMicroVU0() throw()  { Shutdown(); }

	const char* GetShortName() const	{ return "mVU0"; }
	wxString GetLongName() const		{ return L"microVU0 Recompiler"; }

	void Allocate();
	void Shutdown() throw();

	void Reset();
	void Execute(u32 cycles);
	void Clear(u32 addr, u32 size);
	void Vsync() throw();
};

class recMicroVU1 : public BaseVUmicroCPU
{
public:
	recMicroVU1();
	virtual ~recMicroVU1() throw() { Shutdown(); }

	const char* GetShortName() const	{ return "mVU1"; }
	wxString GetLongName() const		{ return L"microVU1 Recompiler"; }

	void Allocate();
	void Shutdown() throw();
	void Reset();
	void Execute(u32 cycles);
	void Clear(u32 addr, u32 size);
	void Vsync() throw();
};

// --------------------------------------------------------------------------------------
//  recSuperVU0 / recSuperVU1
// --------------------------------------------------------------------------------------

class recSuperVU0 : public BaseVUmicroCPU 
{
public:
	recSuperVU0();

	const char* GetShortName() const	{ return "sVU0"; }
	wxString GetLongName() const		{ return L"SuperVU0 Recompiler"; }

	void Allocate();
	void Shutdown() throw();
	void Reset();
	void Execute(u32 cycles);
	void Clear(u32 Addr, u32 Size);
};

class recSuperVU1 : public BaseVUmicroCPU 
{
public:
	recSuperVU1();

	const char* GetShortName() const	{ return "sVU1"; }
	wxString GetLongName() const		{ return L"SuperVU1 Recompiler"; }

	void Allocate();
	void Shutdown() throw();
	void Reset();
	void Execute(u32 cycles);
	void Clear(u32 Addr, u32 Size);
};

extern BaseVUmicroCPU* CpuVU0;
extern BaseVUmicroCPU* CpuVU1;


/////////////////////////////////////////////////////////////////
// These functions initialize memory for both VUs.
//
void vuMicroMemAlloc();
void vuMicroMemShutdown();
void vuMicroMemReset();

/////////////////////////////////////////////////////////////////
// Everything else does stuff on a per-VU basis.
//
void iDumpVU0Registers();
void iDumpVU1Registers();


extern void (*VU0_LOWER_OPCODE[128])();
extern void (*VU0_UPPER_OPCODE[64])();

extern void (*VU0_UPPER_FD_00_TABLE[32])();
extern void (*VU0_UPPER_FD_01_TABLE[32])();
extern void (*VU0_UPPER_FD_10_TABLE[32])();
extern void (*VU0_UPPER_FD_11_TABLE[32])();

extern void (*VU0regs_LOWER_OPCODE[128])(_VURegsNum *VUregsn);
extern void (*VU0regs_UPPER_OPCODE[64])(_VURegsNum *VUregsn);

extern void (*VU0regs_UPPER_FD_00_TABLE[32])(_VURegsNum *VUregsn);
extern void (*VU0regs_UPPER_FD_01_TABLE[32])(_VURegsNum *VUregsn);
extern void (*VU0regs_UPPER_FD_10_TABLE[32])(_VURegsNum *VUregsn);
extern void (*VU0regs_UPPER_FD_11_TABLE[32])(_VURegsNum *VUregsn);

extern void (*VU1_LOWER_OPCODE[128])();
extern void (*VU1_UPPER_OPCODE[64])();

extern void (*VU1_UPPER_FD_00_TABLE[32])();
extern void (*VU1_UPPER_FD_01_TABLE[32])();
extern void (*VU1_UPPER_FD_10_TABLE[32])();
extern void (*VU1_UPPER_FD_11_TABLE[32])();

extern void (*VU1regs_LOWER_OPCODE[128])(_VURegsNum *VUregsn);
extern void (*VU1regs_UPPER_OPCODE[64])(_VURegsNum *VUregsn);

extern void (*VU1regs_UPPER_FD_00_TABLE[32])(_VURegsNum *VUregsn);
extern void (*VU1regs_UPPER_FD_01_TABLE[32])(_VURegsNum *VUregsn);
extern void (*VU1regs_UPPER_FD_10_TABLE[32])(_VURegsNum *VUregsn);
extern void (*VU1regs_UPPER_FD_11_TABLE[32])(_VURegsNum *VUregsn);

// VU0
extern void vu0ResetRegs();
extern void __fastcall vu0ExecMicro(u32 addr);
extern void vu0Exec(VURegs* VU);
extern void vu0Finish();
extern void recResetVU0( void );

// VU1
extern void vu1ResetRegs();
extern void __fastcall vu1ExecMicro(u32 addr);
extern void vu1Exec(VURegs* VU);

void VU0_UPPER_FD_00();
void VU0_UPPER_FD_01();
void VU0_UPPER_FD_10();
void VU0_UPPER_FD_11();

void VU0LowerOP();
void VU0LowerOP_T3_00();
void VU0LowerOP_T3_01();
void VU0LowerOP_T3_10();
void VU0LowerOP_T3_11();

void VU0unknown();

void VU1_UPPER_FD_00();
void VU1_UPPER_FD_01();
void VU1_UPPER_FD_10();
void VU1_UPPER_FD_11();

void VU1LowerOP();
void VU1LowerOP_T3_00();
void VU1LowerOP_T3_01();
void VU1LowerOP_T3_10();
void VU1LowerOP_T3_11();

void VU1unknown();

void VU0regs_UPPER_FD_00(_VURegsNum *VUregsn);
void VU0regs_UPPER_FD_01(_VURegsNum *VUregsn);
void VU0regs_UPPER_FD_10(_VURegsNum *VUregsn);
void VU0regs_UPPER_FD_11(_VURegsNum *VUregsn);

void VU0regsLowerOP(_VURegsNum *VUregsn);
void VU0regsLowerOP_T3_00(_VURegsNum *VUregsn);
void VU0regsLowerOP_T3_01(_VURegsNum *VUregsn);
void VU0regsLowerOP_T3_10(_VURegsNum *VUregsn);
void VU0regsLowerOP_T3_11(_VURegsNum *VUregsn);

void VU0regsunknown(_VURegsNum *VUregsn);

void VU1regs_UPPER_FD_00(_VURegsNum *VUregsn);
void VU1regs_UPPER_FD_01(_VURegsNum *VUregsn);
void VU1regs_UPPER_FD_10(_VURegsNum *VUregsn);
void VU1regs_UPPER_FD_11(_VURegsNum *VUregsn);

void VU1regsLowerOP(_VURegsNum *VUregsn);
void VU1regsLowerOP_T3_00(_VURegsNum *VUregsn);
void VU1regsLowerOP_T3_01(_VURegsNum *VUregsn);
void VU1regsLowerOP_T3_10(_VURegsNum *VUregsn);
void VU1regsLowerOP_T3_11(_VURegsNum *VUregsn);

void VU1regsunknown(_VURegsNum *VUregsn);

/*****************************************
   VU0 Micromode Upper instructions
*****************************************/

void VU0MI_ABS();
void VU0MI_ADD();
void VU0MI_ADDi();
void VU0MI_ADDq();
void VU0MI_ADDx();
void VU0MI_ADDy();
void VU0MI_ADDz();
void VU0MI_ADDw();
void VU0MI_ADDA();
void VU0MI_ADDAi();
void VU0MI_ADDAq();
void VU0MI_ADDAx();
void VU0MI_ADDAy();
void VU0MI_ADDAz();
void VU0MI_ADDAw();
void VU0MI_SUB();
void VU0MI_SUBi();
void VU0MI_SUBq();
void VU0MI_SUBx();
void VU0MI_SUBy();
void VU0MI_SUBz();
void VU0MI_SUBw();
void VU0MI_SUBA();
void VU0MI_SUBAi();
void VU0MI_SUBAq();
void VU0MI_SUBAx();
void VU0MI_SUBAy();
void VU0MI_SUBAz();
void VU0MI_SUBAw();
void VU0MI_MUL();
void VU0MI_MULi();
void VU0MI_MULq();
void VU0MI_MULx();
void VU0MI_MULy();
void VU0MI_MULz();
void VU0MI_MULw();
void VU0MI_MULA();
void VU0MI_MULAi();
void VU0MI_MULAq();
void VU0MI_MULAx();
void VU0MI_MULAy();
void VU0MI_MULAz();
void VU0MI_MULAw();
void VU0MI_MADD();
void VU0MI_MADDi();
void VU0MI_MADDq();
void VU0MI_MADDx();
void VU0MI_MADDy();
void VU0MI_MADDz();
void VU0MI_MADDw();
void VU0MI_MADDA();
void VU0MI_MADDAi();
void VU0MI_MADDAq();
void VU0MI_MADDAx();
void VU0MI_MADDAy();
void VU0MI_MADDAz();
void VU0MI_MADDAw();
void VU0MI_MSUB();
void VU0MI_MSUBi();
void VU0MI_MSUBq();
void VU0MI_MSUBx();
void VU0MI_MSUBy();
void VU0MI_MSUBz();
void VU0MI_MSUBw();
void VU0MI_MSUBA();
void VU0MI_MSUBAi();
void VU0MI_MSUBAq();
void VU0MI_MSUBAx();
void VU0MI_MSUBAy();
void VU0MI_MSUBAz();
void VU0MI_MSUBAw();
void VU0MI_MAX();
void VU0MI_MAXi();
void VU0MI_MAXx();
void VU0MI_MAXy();
void VU0MI_MAXz();
void VU0MI_MAXw();
void VU0MI_MINI();
void VU0MI_MINIi();
void VU0MI_MINIx();
void VU0MI_MINIy();
void VU0MI_MINIz();
void VU0MI_MINIw();
void VU0MI_OPMULA();
void VU0MI_OPMSUB();
void VU0MI_NOP();
void VU0MI_FTOI0();
void VU0MI_FTOI4();
void VU0MI_FTOI12();
void VU0MI_FTOI15();
void VU0MI_ITOF0();
void VU0MI_ITOF4();
void VU0MI_ITOF12();
void VU0MI_ITOF15();
void VU0MI_CLIP();

/*****************************************
   VU0 Micromode Lower instructions
*****************************************/

void VU0MI_DIV();
void VU0MI_SQRT();
void VU0MI_RSQRT();
void VU0MI_IADD();
void VU0MI_IADDI();
void VU0MI_IADDIU();
void VU0MI_IAND();
void VU0MI_IOR();
void VU0MI_ISUB();
void VU0MI_ISUBIU();
void VU0MI_MOVE();
void VU0MI_MFIR();
void VU0MI_MTIR();
void VU0MI_MR32();
void VU0MI_LQ();
void VU0MI_LQD();
void VU0MI_LQI();
void VU0MI_SQ();
void VU0MI_SQD();
void VU0MI_SQI();
void VU0MI_ILW();
void VU0MI_ISW();
void VU0MI_ILWR();
void VU0MI_ISWR();
void VU0MI_LOI();
void VU0MI_RINIT();
void VU0MI_RGET();
void VU0MI_RNEXT();
void VU0MI_RXOR();
void VU0MI_WAITQ();
void VU0MI_FSAND();
void VU0MI_FSEQ();
void VU0MI_FSOR();
void VU0MI_FSSET();
void VU0MI_FMAND();
void VU0MI_FMEQ();
void VU0MI_FMOR();
void VU0MI_FCAND();
void VU0MI_FCEQ();
void VU0MI_FCOR();
void VU0MI_FCSET();
void VU0MI_FCGET();
void VU0MI_IBEQ();
void VU0MI_IBGEZ();
void VU0MI_IBGTZ();
void VU0MI_IBLEZ();
void VU0MI_IBLTZ();
void VU0MI_IBNE();
void VU0MI_B();
void VU0MI_BAL();
void VU0MI_JR();
void VU0MI_JALR();
void VU0MI_MFP();
void VU0MI_WAITP();
void VU0MI_ESADD();
void VU0MI_ERSADD();
void VU0MI_ELENG();
void VU0MI_ERLENG();
void VU0MI_EATANxy();
void VU0MI_EATANxz();
void VU0MI_ESUM();
void VU0MI_ERCPR();
void VU0MI_ESQRT();
void VU0MI_ERSQRT();
void VU0MI_ESIN(); 
void VU0MI_EATAN();
void VU0MI_EEXP();
void VU0MI_XGKICK();
void VU0MI_XTOP();
void VU0MI_XITOP();

/*****************************************
   VU1 Micromode Upper instructions
*****************************************/

void VU0regsMI_ABS(_VURegsNum *VUregsn);
void VU0regsMI_ADD(_VURegsNum *VUregsn);
void VU0regsMI_ADDi(_VURegsNum *VUregsn);
void VU0regsMI_ADDq(_VURegsNum *VUregsn);
void VU0regsMI_ADDx(_VURegsNum *VUregsn);
void VU0regsMI_ADDy(_VURegsNum *VUregsn);
void VU0regsMI_ADDz(_VURegsNum *VUregsn);
void VU0regsMI_ADDw(_VURegsNum *VUregsn);
void VU0regsMI_ADDA(_VURegsNum *VUregsn);
void VU0regsMI_ADDAi(_VURegsNum *VUregsn);
void VU0regsMI_ADDAq(_VURegsNum *VUregsn);
void VU0regsMI_ADDAx(_VURegsNum *VUregsn);
void VU0regsMI_ADDAy(_VURegsNum *VUregsn);
void VU0regsMI_ADDAz(_VURegsNum *VUregsn);
void VU0regsMI_ADDAw(_VURegsNum *VUregsn);
void VU0regsMI_SUB(_VURegsNum *VUregsn);
void VU0regsMI_SUBi(_VURegsNum *VUregsn);
void VU0regsMI_SUBq(_VURegsNum *VUregsn);
void VU0regsMI_SUBx(_VURegsNum *VUregsn);
void VU0regsMI_SUBy(_VURegsNum *VUregsn);
void VU0regsMI_SUBz(_VURegsNum *VUregsn);
void VU0regsMI_SUBw(_VURegsNum *VUregsn);
void VU0regsMI_SUBA(_VURegsNum *VUregsn);
void VU0regsMI_SUBAi(_VURegsNum *VUregsn);
void VU0regsMI_SUBAq(_VURegsNum *VUregsn);
void VU0regsMI_SUBAx(_VURegsNum *VUregsn);
void VU0regsMI_SUBAy(_VURegsNum *VUregsn);
void VU0regsMI_SUBAz(_VURegsNum *VUregsn);
void VU0regsMI_SUBAw(_VURegsNum *VUregsn);
void VU0regsMI_MUL(_VURegsNum *VUregsn);
void VU0regsMI_MULi(_VURegsNum *VUregsn);
void VU0regsMI_MULq(_VURegsNum *VUregsn);
void VU0regsMI_MULx(_VURegsNum *VUregsn);
void VU0regsMI_MULy(_VURegsNum *VUregsn);
void VU0regsMI_MULz(_VURegsNum *VUregsn);
void VU0regsMI_MULw(_VURegsNum *VUregsn);
void VU0regsMI_MULA(_VURegsNum *VUregsn);
void VU0regsMI_MULAi(_VURegsNum *VUregsn);
void VU0regsMI_MULAq(_VURegsNum *VUregsn);
void VU0regsMI_MULAx(_VURegsNum *VUregsn);
void VU0regsMI_MULAy(_VURegsNum *VUregsn);
void VU0regsMI_MULAz(_VURegsNum *VUregsn);
void VU0regsMI_MULAw(_VURegsNum *VUregsn);
void VU0regsMI_MADD(_VURegsNum *VUregsn);
void VU0regsMI_MADDi(_VURegsNum *VUregsn);
void VU0regsMI_MADDq(_VURegsNum *VUregsn);
void VU0regsMI_MADDx(_VURegsNum *VUregsn);
void VU0regsMI_MADDy(_VURegsNum *VUregsn);
void VU0regsMI_MADDz(_VURegsNum *VUregsn);
void VU0regsMI_MADDw(_VURegsNum *VUregsn);
void VU0regsMI_MADDA(_VURegsNum *VUregsn);
void VU0regsMI_MADDAi(_VURegsNum *VUregsn);
void VU0regsMI_MADDAq(_VURegsNum *VUregsn);
void VU0regsMI_MADDAx(_VURegsNum *VUregsn);
void VU0regsMI_MADDAy(_VURegsNum *VUregsn);
void VU0regsMI_MADDAz(_VURegsNum *VUregsn);
void VU0regsMI_MADDAw(_VURegsNum *VUregsn);
void VU0regsMI_MSUB(_VURegsNum *VUregsn);
void VU0regsMI_MSUBi(_VURegsNum *VUregsn);
void VU0regsMI_MSUBq(_VURegsNum *VUregsn);
void VU0regsMI_MSUBx(_VURegsNum *VUregsn);
void VU0regsMI_MSUBy(_VURegsNum *VUregsn);
void VU0regsMI_MSUBz(_VURegsNum *VUregsn);
void VU0regsMI_MSUBw(_VURegsNum *VUregsn);
void VU0regsMI_MSUBA(_VURegsNum *VUregsn);
void VU0regsMI_MSUBAi(_VURegsNum *VUregsn);
void VU0regsMI_MSUBAq(_VURegsNum *VUregsn);
void VU0regsMI_MSUBAx(_VURegsNum *VUregsn);
void VU0regsMI_MSUBAy(_VURegsNum *VUregsn);
void VU0regsMI_MSUBAz(_VURegsNum *VUregsn);
void VU0regsMI_MSUBAw(_VURegsNum *VUregsn);
void VU0regsMI_MAX(_VURegsNum *VUregsn);
void VU0regsMI_MAXi(_VURegsNum *VUregsn);
void VU0regsMI_MAXx(_VURegsNum *VUregsn);
void VU0regsMI_MAXy(_VURegsNum *VUregsn);
void VU0regsMI_MAXz(_VURegsNum *VUregsn);
void VU0regsMI_MAXw(_VURegsNum *VUregsn);
void VU0regsMI_MINI(_VURegsNum *VUregsn);
void VU0regsMI_MINIi(_VURegsNum *VUregsn);
void VU0regsMI_MINIx(_VURegsNum *VUregsn);
void VU0regsMI_MINIy(_VURegsNum *VUregsn);
void VU0regsMI_MINIz(_VURegsNum *VUregsn);
void VU0regsMI_MINIw(_VURegsNum *VUregsn);
void VU0regsMI_OPMULA(_VURegsNum *VUregsn);
void VU0regsMI_OPMSUB(_VURegsNum *VUregsn);
void VU0regsMI_NOP(_VURegsNum *VUregsn);
void VU0regsMI_FTOI0(_VURegsNum *VUregsn);
void VU0regsMI_FTOI4(_VURegsNum *VUregsn);
void VU0regsMI_FTOI12(_VURegsNum *VUregsn);
void VU0regsMI_FTOI15(_VURegsNum *VUregsn);
void VU0regsMI_ITOF0(_VURegsNum *VUregsn);
void VU0regsMI_ITOF4(_VURegsNum *VUregsn);
void VU0regsMI_ITOF12(_VURegsNum *VUregsn);
void VU0regsMI_ITOF15(_VURegsNum *VUregsn);
void VU0regsMI_CLIP(_VURegsNum *VUregsn);

/*****************************************
   VU0 Micromode Lower instructions
*****************************************/

void VU0regsMI_DIV(_VURegsNum *VUregsn);
void VU0regsMI_SQRT(_VURegsNum *VUregsn);
void VU0regsMI_RSQRT(_VURegsNum *VUregsn);
void VU0regsMI_IADD(_VURegsNum *VUregsn);
void VU0regsMI_IADDI(_VURegsNum *VUregsn);
void VU0regsMI_IADDIU(_VURegsNum *VUregsn);
void VU0regsMI_IAND(_VURegsNum *VUregsn);
void VU0regsMI_IOR(_VURegsNum *VUregsn);
void VU0regsMI_ISUB(_VURegsNum *VUregsn);
void VU0regsMI_ISUBIU(_VURegsNum *VUregsn);
void VU0regsMI_MOVE(_VURegsNum *VUregsn);
void VU0regsMI_MFIR(_VURegsNum *VUregsn);
void VU0regsMI_MTIR(_VURegsNum *VUregsn);
void VU0regsMI_MR32(_VURegsNum *VUregsn);
void VU0regsMI_LQ(_VURegsNum *VUregsn);
void VU0regsMI_LQD(_VURegsNum *VUregsn);
void VU0regsMI_LQI(_VURegsNum *VUregsn);
void VU0regsMI_SQ(_VURegsNum *VUregsn);
void VU0regsMI_SQD(_VURegsNum *VUregsn);
void VU0regsMI_SQI(_VURegsNum *VUregsn);
void VU0regsMI_ILW(_VURegsNum *VUregsn);
void VU0regsMI_ISW(_VURegsNum *VUregsn);
void VU0regsMI_ILWR(_VURegsNum *VUregsn);
void VU0regsMI_ISWR(_VURegsNum *VUregsn);
void VU0regsMI_LOI(_VURegsNum *VUregsn);
void VU0regsMI_RINIT(_VURegsNum *VUregsn);
void VU0regsMI_RGET(_VURegsNum *VUregsn);
void VU0regsMI_RNEXT(_VURegsNum *VUregsn);
void VU0regsMI_RXOR(_VURegsNum *VUregsn);
void VU0regsMI_WAITQ(_VURegsNum *VUregsn);
void VU0regsMI_FSAND(_VURegsNum *VUregsn);
void VU0regsMI_FSEQ(_VURegsNum *VUregsn);
void VU0regsMI_FSOR(_VURegsNum *VUregsn);
void VU0regsMI_FSSET(_VURegsNum *VUregsn);
void VU0regsMI_FMAND(_VURegsNum *VUregsn);
void VU0regsMI_FMEQ(_VURegsNum *VUregsn);
void VU0regsMI_FMOR(_VURegsNum *VUregsn);
void VU0regsMI_FCAND(_VURegsNum *VUregsn);
void VU0regsMI_FCEQ(_VURegsNum *VUregsn);
void VU0regsMI_FCOR(_VURegsNum *VUregsn);
void VU0regsMI_FCSET(_VURegsNum *VUregsn);
void VU0regsMI_FCGET(_VURegsNum *VUregsn);
void VU0regsMI_IBEQ(_VURegsNum *VUregsn);
void VU0regsMI_IBGEZ(_VURegsNum *VUregsn);
void VU0regsMI_IBGTZ(_VURegsNum *VUregsn);
void VU0regsMI_IBLTZ(_VURegsNum *VUregsn);
void VU0regsMI_IBLEZ(_VURegsNum *VUregsn);
void VU0regsMI_IBNE(_VURegsNum *VUregsn);
void VU0regsMI_B(_VURegsNum *VUregsn);
void VU0regsMI_BAL(_VURegsNum *VUregsn);
void VU0regsMI_JR(_VURegsNum *VUregsn);
void VU0regsMI_JALR(_VURegsNum *VUregsn);
void VU0regsMI_MFP(_VURegsNum *VUregsn);
void VU0regsMI_WAITP(_VURegsNum *VUregsn);
void VU0regsMI_ESADD(_VURegsNum *VUregsn);
void VU0regsMI_ERSADD(_VURegsNum *VUregsn);
void VU0regsMI_ELENG(_VURegsNum *VUregsn);
void VU0regsMI_ERLENG(_VURegsNum *VUregsn);
void VU0regsMI_EATANxy(_VURegsNum *VUregsn);
void VU0regsMI_EATANxz(_VURegsNum *VUregsn);
void VU0regsMI_ESUM(_VURegsNum *VUregsn);
void VU0regsMI_ERCPR(_VURegsNum *VUregsn);
void VU0regsMI_ESQRT(_VURegsNum *VUregsn);
void VU0regsMI_ERSQRT(_VURegsNum *VUregsn);
void VU0regsMI_ESIN(_VURegsNum *VUregsn); 
void VU0regsMI_EATAN(_VURegsNum *VUregsn);
void VU0regsMI_EEXP(_VURegsNum *VUregsn);
void VU0regsMI_XGKICK(_VURegsNum *VUregsn);
void VU0regsMI_XTOP(_VURegsNum *VUregsn);
void VU0regsMI_XITOP(_VURegsNum *VUregsn);

/*****************************************
   VU1 Micromode Upper instructions
*****************************************/

void VU1MI_ABS();
void VU1MI_ADD();
void VU1MI_ADDi();
void VU1MI_ADDq();
void VU1MI_ADDx();
void VU1MI_ADDy();
void VU1MI_ADDz();
void VU1MI_ADDw();
void VU1MI_ADDA();
void VU1MI_ADDAi();
void VU1MI_ADDAq();
void VU1MI_ADDAx();
void VU1MI_ADDAy();
void VU1MI_ADDAz();
void VU1MI_ADDAw();
void VU1MI_SUB();
void VU1MI_SUBi();
void VU1MI_SUBq();
void VU1MI_SUBx();
void VU1MI_SUBy();
void VU1MI_SUBz();
void VU1MI_SUBw();
void VU1MI_SUBA();
void VU1MI_SUBAi();
void VU1MI_SUBAq();
void VU1MI_SUBAx();
void VU1MI_SUBAy();
void VU1MI_SUBAz();
void VU1MI_SUBAw();
void VU1MI_MUL();
void VU1MI_MULi();
void VU1MI_MULq();
void VU1MI_MULx();
void VU1MI_MULy();
void VU1MI_MULz();
void VU1MI_MULw();
void VU1MI_MULA();
void VU1MI_MULAi();
void VU1MI_MULAq();
void VU1MI_MULAx();
void VU1MI_MULAy();
void VU1MI_MULAz();
void VU1MI_MULAw();
void VU1MI_MADD();
void VU1MI_MADDi();
void VU1MI_MADDq();
void VU1MI_MADDx();
void VU1MI_MADDy();
void VU1MI_MADDz();
void VU1MI_MADDw();
void VU1MI_MADDA();
void VU1MI_MADDAi();
void VU1MI_MADDAq();
void VU1MI_MADDAx();
void VU1MI_MADDAy();
void VU1MI_MADDAz();
void VU1MI_MADDAw();
void VU1MI_MSUB();
void VU1MI_MSUBi();
void VU1MI_MSUBq();
void VU1MI_MSUBx();
void VU1MI_MSUBy();
void VU1MI_MSUBz();
void VU1MI_MSUBw();
void VU1MI_MSUBA();
void VU1MI_MSUBAi();
void VU1MI_MSUBAq();
void VU1MI_MSUBAx();
void VU1MI_MSUBAy();
void VU1MI_MSUBAz();
void VU1MI_MSUBAw();
void VU1MI_MAX();
void VU1MI_MAXi();
void VU1MI_MAXx();
void VU1MI_MAXy();
void VU1MI_MAXz();
void VU1MI_MAXw();
void VU1MI_MINI();
void VU1MI_MINIi();
void VU1MI_MINIx();
void VU1MI_MINIy();
void VU1MI_MINIz();
void VU1MI_MINIw();
void VU1MI_OPMULA();
void VU1MI_OPMSUB();
void VU1MI_NOP();
void VU1MI_FTOI0();
void VU1MI_FTOI4();
void VU1MI_FTOI12();
void VU1MI_FTOI15();
void VU1MI_ITOF0();
void VU1MI_ITOF4();
void VU1MI_ITOF12();
void VU1MI_ITOF15();
void VU1MI_CLIP();

/*****************************************
   VU1 Micromode Lower instructions
*****************************************/

void VU1MI_DIV();
void VU1MI_SQRT();
void VU1MI_RSQRT();
void VU1MI_IADD();
void VU1MI_IADDI();
void VU1MI_IADDIU();
void VU1MI_IAND();
void VU1MI_IOR();
void VU1MI_ISUB();
void VU1MI_ISUBIU();
void VU1MI_MOVE();
void VU1MI_MFIR();
void VU1MI_MTIR();
void VU1MI_MR32();
void VU1MI_LQ();
void VU1MI_LQD();
void VU1MI_LQI();
void VU1MI_SQ();
void VU1MI_SQD();
void VU1MI_SQI();
void VU1MI_ILW();
void VU1MI_ISW();
void VU1MI_ILWR();
void VU1MI_ISWR();
void VU1MI_LOI();
void VU1MI_RINIT();
void VU1MI_RGET();
void VU1MI_RNEXT();
void VU1MI_RXOR();
void VU1MI_WAITQ();
void VU1MI_FSAND();
void VU1MI_FSEQ();
void VU1MI_FSOR();
void VU1MI_FSSET();
void VU1MI_FMAND();
void VU1MI_FMEQ();
void VU1MI_FMOR();
void VU1MI_FCAND();
void VU1MI_FCEQ();
void VU1MI_FCOR();
void VU1MI_FCSET();
void VU1MI_FCGET();
void VU1MI_IBEQ();
void VU1MI_IBGEZ();
void VU1MI_IBGTZ();
void VU1MI_IBLTZ();
void VU1MI_IBLEZ();
void VU1MI_IBNE();
void VU1MI_B();
void VU1MI_BAL();
void VU1MI_JR();
void VU1MI_JALR();
void VU1MI_MFP();
void VU1MI_WAITP();
void VU1MI_ESADD();
void VU1MI_ERSADD();
void VU1MI_ELENG();
void VU1MI_ERLENG();
void VU1MI_EATANxy();
void VU1MI_EATANxz();
void VU1MI_ESUM();
void VU1MI_ERCPR();
void VU1MI_ESQRT();
void VU1MI_ERSQRT();
void VU1MI_ESIN(); 
void VU1MI_EATAN();
void VU1MI_EEXP();
void VU1MI_XGKICK();
void VU1MI_XTOP();
void VU1MI_XITOP();

/*****************************************
   VU1 Micromode Upper instructions
*****************************************/

void VU1regsMI_ABS(_VURegsNum *VUregsn);
void VU1regsMI_ADD(_VURegsNum *VUregsn);
void VU1regsMI_ADDi(_VURegsNum *VUregsn);
void VU1regsMI_ADDq(_VURegsNum *VUregsn);
void VU1regsMI_ADDx(_VURegsNum *VUregsn);
void VU1regsMI_ADDy(_VURegsNum *VUregsn);
void VU1regsMI_ADDz(_VURegsNum *VUregsn);
void VU1regsMI_ADDw(_VURegsNum *VUregsn);
void VU1regsMI_ADDA(_VURegsNum *VUregsn);
void VU1regsMI_ADDAi(_VURegsNum *VUregsn);
void VU1regsMI_ADDAq(_VURegsNum *VUregsn);
void VU1regsMI_ADDAx(_VURegsNum *VUregsn);
void VU1regsMI_ADDAy(_VURegsNum *VUregsn);
void VU1regsMI_ADDAz(_VURegsNum *VUregsn);
void VU1regsMI_ADDAw(_VURegsNum *VUregsn);
void VU1regsMI_SUB(_VURegsNum *VUregsn);
void VU1regsMI_SUBi(_VURegsNum *VUregsn);
void VU1regsMI_SUBq(_VURegsNum *VUregsn);
void VU1regsMI_SUBx(_VURegsNum *VUregsn);
void VU1regsMI_SUBy(_VURegsNum *VUregsn);
void VU1regsMI_SUBz(_VURegsNum *VUregsn);
void VU1regsMI_SUBw(_VURegsNum *VUregsn);
void VU1regsMI_SUBA(_VURegsNum *VUregsn);
void VU1regsMI_SUBAi(_VURegsNum *VUregsn);
void VU1regsMI_SUBAq(_VURegsNum *VUregsn);
void VU1regsMI_SUBAx(_VURegsNum *VUregsn);
void VU1regsMI_SUBAy(_VURegsNum *VUregsn);
void VU1regsMI_SUBAz(_VURegsNum *VUregsn);
void VU1regsMI_SUBAw(_VURegsNum *VUregsn);
void VU1regsMI_MUL(_VURegsNum *VUregsn);
void VU1regsMI_MULi(_VURegsNum *VUregsn);
void VU1regsMI_MULq(_VURegsNum *VUregsn);
void VU1regsMI_MULx(_VURegsNum *VUregsn);
void VU1regsMI_MULy(_VURegsNum *VUregsn);
void VU1regsMI_MULz(_VURegsNum *VUregsn);
void VU1regsMI_MULw(_VURegsNum *VUregsn);
void VU1regsMI_MULA(_VURegsNum *VUregsn);
void VU1regsMI_MULAi(_VURegsNum *VUregsn);
void VU1regsMI_MULAq(_VURegsNum *VUregsn);
void VU1regsMI_MULAx(_VURegsNum *VUregsn);
void VU1regsMI_MULAy(_VURegsNum *VUregsn);
void VU1regsMI_MULAz(_VURegsNum *VUregsn);
void VU1regsMI_MULAw(_VURegsNum *VUregsn);
void VU1regsMI_MADD(_VURegsNum *VUregsn);
void VU1regsMI_MADDi(_VURegsNum *VUregsn);
void VU1regsMI_MADDq(_VURegsNum *VUregsn);
void VU1regsMI_MADDx(_VURegsNum *VUregsn);
void VU1regsMI_MADDy(_VURegsNum *VUregsn);
void VU1regsMI_MADDz(_VURegsNum *VUregsn);
void VU1regsMI_MADDw(_VURegsNum *VUregsn);
void VU1regsMI_MADDA(_VURegsNum *VUregsn);
void VU1regsMI_MADDAi(_VURegsNum *VUregsn);
void VU1regsMI_MADDAq(_VURegsNum *VUregsn);
void VU1regsMI_MADDAx(_VURegsNum *VUregsn);
void VU1regsMI_MADDAy(_VURegsNum *VUregsn);
void VU1regsMI_MADDAz(_VURegsNum *VUregsn);
void VU1regsMI_MADDAw(_VURegsNum *VUregsn);
void VU1regsMI_MSUB(_VURegsNum *VUregsn);
void VU1regsMI_MSUBi(_VURegsNum *VUregsn);
void VU1regsMI_MSUBq(_VURegsNum *VUregsn);
void VU1regsMI_MSUBx(_VURegsNum *VUregsn);
void VU1regsMI_MSUBy(_VURegsNum *VUregsn);
void VU1regsMI_MSUBz(_VURegsNum *VUregsn);
void VU1regsMI_MSUBw(_VURegsNum *VUregsn);
void VU1regsMI_MSUBA(_VURegsNum *VUregsn);
void VU1regsMI_MSUBAi(_VURegsNum *VUregsn);
void VU1regsMI_MSUBAq(_VURegsNum *VUregsn);
void VU1regsMI_MSUBAx(_VURegsNum *VUregsn);
void VU1regsMI_MSUBAy(_VURegsNum *VUregsn);
void VU1regsMI_MSUBAz(_VURegsNum *VUregsn);
void VU1regsMI_MSUBAw(_VURegsNum *VUregsn);
void VU1regsMI_MAX(_VURegsNum *VUregsn);
void VU1regsMI_MAXi(_VURegsNum *VUregsn);
void VU1regsMI_MAXx(_VURegsNum *VUregsn);
void VU1regsMI_MAXy(_VURegsNum *VUregsn);
void VU1regsMI_MAXz(_VURegsNum *VUregsn);
void VU1regsMI_MAXw(_VURegsNum *VUregsn);
void VU1regsMI_MINI(_VURegsNum *VUregsn);
void VU1regsMI_MINIi(_VURegsNum *VUregsn);
void VU1regsMI_MINIx(_VURegsNum *VUregsn);
void VU1regsMI_MINIy(_VURegsNum *VUregsn);
void VU1regsMI_MINIz(_VURegsNum *VUregsn);
void VU1regsMI_MINIw(_VURegsNum *VUregsn);
void VU1regsMI_OPMULA(_VURegsNum *VUregsn);
void VU1regsMI_OPMSUB(_VURegsNum *VUregsn);
void VU1regsMI_NOP(_VURegsNum *VUregsn);
void VU1regsMI_FTOI0(_VURegsNum *VUregsn);
void VU1regsMI_FTOI4(_VURegsNum *VUregsn);
void VU1regsMI_FTOI12(_VURegsNum *VUregsn);
void VU1regsMI_FTOI15(_VURegsNum *VUregsn);
void VU1regsMI_ITOF0(_VURegsNum *VUregsn);
void VU1regsMI_ITOF4(_VURegsNum *VUregsn);
void VU1regsMI_ITOF12(_VURegsNum *VUregsn);
void VU1regsMI_ITOF15(_VURegsNum *VUregsn);
void VU1regsMI_CLIP(_VURegsNum *VUregsn);

/*****************************************
   VU1 Micromode Lower instructions
*****************************************/

void VU1regsMI_DIV(_VURegsNum *VUregsn);
void VU1regsMI_SQRT(_VURegsNum *VUregsn);
void VU1regsMI_RSQRT(_VURegsNum *VUregsn);
void VU1regsMI_IADD(_VURegsNum *VUregsn);
void VU1regsMI_IADDI(_VURegsNum *VUregsn);
void VU1regsMI_IADDIU(_VURegsNum *VUregsn);
void VU1regsMI_IAND(_VURegsNum *VUregsn);
void VU1regsMI_IOR(_VURegsNum *VUregsn);
void VU1regsMI_ISUB(_VURegsNum *VUregsn);
void VU1regsMI_ISUBIU(_VURegsNum *VUregsn);
void VU1regsMI_MOVE(_VURegsNum *VUregsn);
void VU1regsMI_MFIR(_VURegsNum *VUregsn);
void VU1regsMI_MTIR(_VURegsNum *VUregsn);
void VU1regsMI_MR32(_VURegsNum *VUregsn);
void VU1regsMI_LQ(_VURegsNum *VUregsn);
void VU1regsMI_LQD(_VURegsNum *VUregsn);
void VU1regsMI_LQI(_VURegsNum *VUregsn);
void VU1regsMI_SQ(_VURegsNum *VUregsn);
void VU1regsMI_SQD(_VURegsNum *VUregsn);
void VU1regsMI_SQI(_VURegsNum *VUregsn);
void VU1regsMI_ILW(_VURegsNum *VUregsn);
void VU1regsMI_ISW(_VURegsNum *VUregsn);
void VU1regsMI_ILWR(_VURegsNum *VUregsn);
void VU1regsMI_ISWR(_VURegsNum *VUregsn);
void VU1regsMI_LOI(_VURegsNum *VUregsn);
void VU1regsMI_RINIT(_VURegsNum *VUregsn);
void VU1regsMI_RGET(_VURegsNum *VUregsn);
void VU1regsMI_RNEXT(_VURegsNum *VUregsn);
void VU1regsMI_RXOR(_VURegsNum *VUregsn);
void VU1regsMI_WAITQ(_VURegsNum *VUregsn);
void VU1regsMI_FSAND(_VURegsNum *VUregsn);
void VU1regsMI_FSEQ(_VURegsNum *VUregsn);
void VU1regsMI_FSOR(_VURegsNum *VUregsn);
void VU1regsMI_FSSET(_VURegsNum *VUregsn);
void VU1regsMI_FMAND(_VURegsNum *VUregsn);
void VU1regsMI_FMEQ(_VURegsNum *VUregsn);
void VU1regsMI_FMOR(_VURegsNum *VUregsn);
void VU1regsMI_FCAND(_VURegsNum *VUregsn);
void VU1regsMI_FCEQ(_VURegsNum *VUregsn);
void VU1regsMI_FCOR(_VURegsNum *VUregsn);
void VU1regsMI_FCSET(_VURegsNum *VUregsn);
void VU1regsMI_FCGET(_VURegsNum *VUregsn);
void VU1regsMI_IBEQ(_VURegsNum *VUregsn);
void VU1regsMI_IBGEZ(_VURegsNum *VUregsn);
void VU1regsMI_IBGTZ(_VURegsNum *VUregsn);
void VU1regsMI_IBLTZ(_VURegsNum *VUregsn);
void VU1regsMI_IBLEZ(_VURegsNum *VUregsn);
void VU1regsMI_IBNE(_VURegsNum *VUregsn);
void VU1regsMI_B(_VURegsNum *VUregsn);
void VU1regsMI_BAL(_VURegsNum *VUregsn);
void VU1regsMI_JR(_VURegsNum *VUregsn);
void VU1regsMI_JALR(_VURegsNum *VUregsn);
void VU1regsMI_MFP(_VURegsNum *VUregsn);
void VU1regsMI_WAITP(_VURegsNum *VUregsn);
void VU1regsMI_ESADD(_VURegsNum *VUregsn);
void VU1regsMI_ERSADD(_VURegsNum *VUregsn);
void VU1regsMI_ELENG(_VURegsNum *VUregsn);
void VU1regsMI_ERLENG(_VURegsNum *VUregsn);
void VU1regsMI_EATANxy(_VURegsNum *VUregsn);
void VU1regsMI_EATANxz(_VURegsNum *VUregsn);
void VU1regsMI_ESUM(_VURegsNum *VUregsn);
void VU1regsMI_ERCPR(_VURegsNum *VUregsn);
void VU1regsMI_ESQRT(_VURegsNum *VUregsn);
void VU1regsMI_ERSQRT(_VURegsNum *VUregsn);
void VU1regsMI_ESIN(_VURegsNum *VUregsn); 
void VU1regsMI_EATAN(_VURegsNum *VUregsn);
void VU1regsMI_EEXP(_VURegsNum *VUregsn);
void VU1regsMI_XGKICK(_VURegsNum *VUregsn);
void VU1regsMI_XTOP(_VURegsNum *VUregsn);
void VU1regsMI_XITOP(_VURegsNum *VUregsn);

/*****************************************
   VU Micromode Tables/Opcodes defs macros
*****************************************/


#define _vuTables(VU, PREFIX) \
 \
void (*PREFIX##_LOWER_OPCODE[128])() = { \
	PREFIX##MI_LQ    , PREFIX##MI_SQ    , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_ILW   , PREFIX##MI_ISW   , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_IADDIU, PREFIX##MI_ISUBIU, PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_FCEQ  , PREFIX##MI_FCSET , PREFIX##MI_FCAND, PREFIX##MI_FCOR, /* 0x10 */ \
	PREFIX##MI_FSEQ  , PREFIX##MI_FSSET , PREFIX##MI_FSAND, PREFIX##MI_FSOR, \
	PREFIX##MI_FMEQ  , PREFIX##unknown  , PREFIX##MI_FMAND, PREFIX##MI_FMOR, \
	PREFIX##MI_FCGET , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_B     , PREFIX##MI_BAL   , PREFIX##unknown , PREFIX##unknown, /* 0x20 */  \
	PREFIX##MI_JR    , PREFIX##MI_JALR  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_IBEQ  , PREFIX##MI_IBNE  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_IBLTZ , PREFIX##MI_IBGTZ , PREFIX##MI_IBLEZ, PREFIX##MI_IBGEZ, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x30 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##LowerOP  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x40*/  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x50 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x60 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x70 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
}; \
 \
 void (*PREFIX##LowerOP_T3_00_OPCODE[32])() = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_MOVE  , PREFIX##MI_LQI   , PREFIX##MI_DIV  , PREFIX##MI_MTIR,  \
	PREFIX##MI_RNEXT , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##MI_MFP   , PREFIX##MI_XTOP , PREFIX##MI_XGKICK,  \
	PREFIX##MI_ESADD , PREFIX##MI_EATANxy, PREFIX##MI_ESQRT, PREFIX##MI_ESIN,  \
}; \
 \
 void (*PREFIX##LowerOP_T3_01_OPCODE[32])() = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_MR32  , PREFIX##MI_SQI   , PREFIX##MI_SQRT , PREFIX##MI_MFIR,  \
	PREFIX##MI_RGET  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##MI_XITOP, PREFIX##unknown,  \
	PREFIX##MI_ERSADD, PREFIX##MI_EATANxz, PREFIX##MI_ERSQRT, PREFIX##MI_EATAN, \
}; \
 \
 void (*PREFIX##LowerOP_T3_10_OPCODE[32])() = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##MI_LQD   , PREFIX##MI_RSQRT, PREFIX##MI_ILWR,  \
	PREFIX##MI_RINIT , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_ELENG , PREFIX##MI_ESUM  , PREFIX##MI_ERCPR, PREFIX##MI_EEXP,  \
}; \
 \
 void (*PREFIX##LowerOP_T3_11_OPCODE[32])() = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##MI_SQD   , PREFIX##MI_WAITQ, PREFIX##MI_ISWR,  \
	PREFIX##MI_RXOR  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_ERLENG, PREFIX##unknown  , PREFIX##MI_WAITP, PREFIX##unknown,  \
}; \
 \
 void (*PREFIX##LowerOP_OPCODE[64])() = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x20 */  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_IADD  , PREFIX##MI_ISUB  , PREFIX##MI_IADDI, PREFIX##unknown, /* 0x30 */ \
	PREFIX##MI_IAND  , PREFIX##MI_IOR   , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##LowerOP_T3_00, PREFIX##LowerOP_T3_01, PREFIX##LowerOP_T3_10, PREFIX##LowerOP_T3_11,  \
}; \
 \
 void (*PREFIX##_UPPER_OPCODE[64])() = { \
	PREFIX##MI_ADDx  , PREFIX##MI_ADDy  , PREFIX##MI_ADDz  , PREFIX##MI_ADDw, \
	PREFIX##MI_SUBx  , PREFIX##MI_SUBy  , PREFIX##MI_SUBz  , PREFIX##MI_SUBw, \
	PREFIX##MI_MADDx , PREFIX##MI_MADDy , PREFIX##MI_MADDz , PREFIX##MI_MADDw, \
	PREFIX##MI_MSUBx , PREFIX##MI_MSUBy , PREFIX##MI_MSUBz , PREFIX##MI_MSUBw, \
	PREFIX##MI_MAXx  , PREFIX##MI_MAXy  , PREFIX##MI_MAXz  , PREFIX##MI_MAXw,  /* 0x10 */  \
	PREFIX##MI_MINIx , PREFIX##MI_MINIy , PREFIX##MI_MINIz , PREFIX##MI_MINIw, \
	PREFIX##MI_MULx  , PREFIX##MI_MULy  , PREFIX##MI_MULz  , PREFIX##MI_MULw, \
	PREFIX##MI_MULq  , PREFIX##MI_MAXi  , PREFIX##MI_MULi  , PREFIX##MI_MINIi, \
	PREFIX##MI_ADDq  , PREFIX##MI_MADDq , PREFIX##MI_ADDi  , PREFIX##MI_MADDi, /* 0x20 */ \
	PREFIX##MI_SUBq  , PREFIX##MI_MSUBq , PREFIX##MI_SUBi  , PREFIX##MI_MSUBi, \
	PREFIX##MI_ADD   , PREFIX##MI_MADD  , PREFIX##MI_MUL   , PREFIX##MI_MAX, \
	PREFIX##MI_SUB   , PREFIX##MI_MSUB  , PREFIX##MI_OPMSUB, PREFIX##MI_MINI, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown,  /* 0x30 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown, \
	PREFIX##_UPPER_FD_00, PREFIX##_UPPER_FD_01, PREFIX##_UPPER_FD_10, PREFIX##_UPPER_FD_11,  \
}; \
 \
 void (*PREFIX##_UPPER_FD_00_TABLE[32])() = { \
	PREFIX##MI_ADDAx, PREFIX##MI_SUBAx , PREFIX##MI_MADDAx, PREFIX##MI_MSUBAx, \
	PREFIX##MI_ITOF0, PREFIX##MI_FTOI0, PREFIX##MI_MULAx , PREFIX##MI_MULAq , \
	PREFIX##MI_ADDAq, PREFIX##MI_SUBAq, PREFIX##MI_ADDA  , PREFIX##MI_SUBA  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
 void (*PREFIX##_UPPER_FD_01_TABLE[32])() = { \
	PREFIX##MI_ADDAy , PREFIX##MI_SUBAy  , PREFIX##MI_MADDAy, PREFIX##MI_MSUBAy, \
	PREFIX##MI_ITOF4 , PREFIX##MI_FTOI4 , PREFIX##MI_MULAy , PREFIX##MI_ABS   , \
	PREFIX##MI_MADDAq, PREFIX##MI_MSUBAq, PREFIX##MI_MADDA , PREFIX##MI_MSUBA , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
 void (*PREFIX##_UPPER_FD_10_TABLE[32])() = { \
	PREFIX##MI_ADDAz , PREFIX##MI_SUBAz  , PREFIX##MI_MADDAz, PREFIX##MI_MSUBAz, \
	PREFIX##MI_ITOF12, PREFIX##MI_FTOI12, PREFIX##MI_MULAz , PREFIX##MI_MULAi , \
	PREFIX##MI_ADDAi, PREFIX##MI_SUBAi , PREFIX##MI_MULA  , PREFIX##MI_OPMULA, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
 void (*PREFIX##_UPPER_FD_11_TABLE[32])() = { \
	PREFIX##MI_ADDAw , PREFIX##MI_SUBAw  , PREFIX##MI_MADDAw, PREFIX##MI_MSUBAw, \
	PREFIX##MI_ITOF15, PREFIX##MI_FTOI15, PREFIX##MI_MULAw , PREFIX##MI_CLIP  , \
	PREFIX##MI_MADDAi, PREFIX##MI_MSUBAi, PREFIX##unknown  , PREFIX##MI_NOP   , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
 \
 \
 void PREFIX##_UPPER_FD_00() { \
 PREFIX##_UPPER_FD_00_TABLE[(VU.code >> 6) & 0x1f ](); \
} \
 \
 void PREFIX##_UPPER_FD_01() { \
 PREFIX##_UPPER_FD_01_TABLE[(VU.code >> 6) & 0x1f](); \
} \
 \
 void PREFIX##_UPPER_FD_10() { \
 PREFIX##_UPPER_FD_10_TABLE[(VU.code >> 6) & 0x1f](); \
} \
 \
 void PREFIX##_UPPER_FD_11() { \
 PREFIX##_UPPER_FD_11_TABLE[(VU.code >> 6) & 0x1f](); \
} \
 \
 void PREFIX##LowerOP() { \
 PREFIX##LowerOP_OPCODE[VU.code & 0x3f](); \
} \
 \
 void PREFIX##LowerOP_T3_00() { \
 PREFIX##LowerOP_T3_00_OPCODE[(VU.code >> 6) & 0x1f](); \
} \
 \
 void PREFIX##LowerOP_T3_01() { \
 PREFIX##LowerOP_T3_01_OPCODE[(VU.code >> 6) & 0x1f](); \
} \
 \
 void PREFIX##LowerOP_T3_10() { \
 PREFIX##LowerOP_T3_10_OPCODE[(VU.code >> 6) & 0x1f](); \
} \
 \
 void PREFIX##LowerOP_T3_11() { \
 PREFIX##LowerOP_T3_11_OPCODE[(VU.code >> 6) & 0x1f](); \
}

#define _vuRegsTables(VU, PREFIX) \
 \
void (*PREFIX##_LOWER_OPCODE[128])(_VURegsNum *VUregsn) = { \
	PREFIX##MI_LQ    , PREFIX##MI_SQ    , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_ILW   , PREFIX##MI_ISW   , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_IADDIU, PREFIX##MI_ISUBIU, PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_FCEQ  , PREFIX##MI_FCSET , PREFIX##MI_FCAND, PREFIX##MI_FCOR, /* 0x10 */ \
	PREFIX##MI_FSEQ  , PREFIX##MI_FSSET , PREFIX##MI_FSAND, PREFIX##MI_FSOR, \
	PREFIX##MI_FMEQ  , PREFIX##unknown  , PREFIX##MI_FMAND, PREFIX##MI_FMOR, \
	PREFIX##MI_FCGET , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_B     , PREFIX##MI_BAL   , PREFIX##unknown , PREFIX##unknown, /* 0x20 */  \
	PREFIX##MI_JR    , PREFIX##MI_JALR  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_IBEQ  , PREFIX##MI_IBNE  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##MI_IBLTZ , PREFIX##MI_IBGTZ , PREFIX##MI_IBLEZ, PREFIX##MI_IBGEZ, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x30 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##LowerOP  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x40*/  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x50 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x60 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x70 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
}; \
 \
 void (*PREFIX##LowerOP_T3_00_OPCODE[32])(_VURegsNum *VUregsn) = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_MOVE  , PREFIX##MI_LQI   , PREFIX##MI_DIV  , PREFIX##MI_MTIR,  \
	PREFIX##MI_RNEXT , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##MI_MFP   , PREFIX##MI_XTOP , PREFIX##MI_XGKICK,  \
	PREFIX##MI_ESADD , PREFIX##MI_EATANxy, PREFIX##MI_ESQRT, PREFIX##MI_ESIN,  \
}; \
 \
 void (*PREFIX##LowerOP_T3_01_OPCODE[32])(_VURegsNum *VUregsn) = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_MR32  , PREFIX##MI_SQI   , PREFIX##MI_SQRT , PREFIX##MI_MFIR,  \
	PREFIX##MI_RGET  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##MI_XITOP, PREFIX##unknown,  \
	PREFIX##MI_ERSADD, PREFIX##MI_EATANxz, PREFIX##MI_ERSQRT, PREFIX##MI_EATAN, \
}; \
 \
 void (*PREFIX##LowerOP_T3_10_OPCODE[32])(_VURegsNum *VUregsn) = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##MI_LQD   , PREFIX##MI_RSQRT, PREFIX##MI_ILWR,  \
	PREFIX##MI_RINIT , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_ELENG , PREFIX##MI_ESUM  , PREFIX##MI_ERCPR, PREFIX##MI_EEXP,  \
}; \
 \
 void (*PREFIX##LowerOP_T3_11_OPCODE[32])(_VURegsNum *VUregsn) = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##MI_SQD   , PREFIX##MI_WAITQ, PREFIX##MI_ISWR,  \
	PREFIX##MI_RXOR  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_ERLENG, PREFIX##unknown  , PREFIX##MI_WAITP, PREFIX##unknown,  \
}; \
 \
 void (*PREFIX##LowerOP_OPCODE[64])(_VURegsNum *VUregsn) = { \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x10 */  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown, /* 0x20 */  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##MI_IADD  , PREFIX##MI_ISUB  , PREFIX##MI_IADDI, PREFIX##unknown, /* 0x30 */ \
	PREFIX##MI_IAND  , PREFIX##MI_IOR   , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown , PREFIX##unknown,  \
	PREFIX##LowerOP_T3_00, PREFIX##LowerOP_T3_01, PREFIX##LowerOP_T3_10, PREFIX##LowerOP_T3_11,  \
}; \
 \
 void (*PREFIX##_UPPER_OPCODE[64])(_VURegsNum *VUregsn) = { \
	PREFIX##MI_ADDx  , PREFIX##MI_ADDy  , PREFIX##MI_ADDz  , PREFIX##MI_ADDw, \
	PREFIX##MI_SUBx  , PREFIX##MI_SUBy  , PREFIX##MI_SUBz  , PREFIX##MI_SUBw, \
	PREFIX##MI_MADDx , PREFIX##MI_MADDy , PREFIX##MI_MADDz , PREFIX##MI_MADDw, \
	PREFIX##MI_MSUBx , PREFIX##MI_MSUBy , PREFIX##MI_MSUBz , PREFIX##MI_MSUBw, \
	PREFIX##MI_MAXx  , PREFIX##MI_MAXy  , PREFIX##MI_MAXz  , PREFIX##MI_MAXw,  /* 0x10 */  \
	PREFIX##MI_MINIx , PREFIX##MI_MINIy , PREFIX##MI_MINIz , PREFIX##MI_MINIw, \
	PREFIX##MI_MULx  , PREFIX##MI_MULy  , PREFIX##MI_MULz  , PREFIX##MI_MULw, \
	PREFIX##MI_MULq  , PREFIX##MI_MAXi  , PREFIX##MI_MULi  , PREFIX##MI_MINIi, \
	PREFIX##MI_ADDq  , PREFIX##MI_MADDq , PREFIX##MI_ADDi  , PREFIX##MI_MADDi, /* 0x20 */ \
	PREFIX##MI_SUBq  , PREFIX##MI_MSUBq , PREFIX##MI_SUBi  , PREFIX##MI_MSUBi, \
	PREFIX##MI_ADD   , PREFIX##MI_MADD  , PREFIX##MI_MUL   , PREFIX##MI_MAX, \
	PREFIX##MI_SUB   , PREFIX##MI_MSUB  , PREFIX##MI_OPMSUB, PREFIX##MI_MINI, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown,  /* 0x30 */ \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown, \
	PREFIX##_UPPER_FD_00, PREFIX##_UPPER_FD_01, PREFIX##_UPPER_FD_10, PREFIX##_UPPER_FD_11,  \
}; \
 \
 void (*PREFIX##_UPPER_FD_00_TABLE[32])(_VURegsNum *VUregsn) = { \
	PREFIX##MI_ADDAx, PREFIX##MI_SUBAx , PREFIX##MI_MADDAx, PREFIX##MI_MSUBAx, \
	PREFIX##MI_ITOF0, PREFIX##MI_FTOI0, PREFIX##MI_MULAx , PREFIX##MI_MULAq , \
	PREFIX##MI_ADDAq, PREFIX##MI_SUBAq, PREFIX##MI_ADDA  , PREFIX##MI_SUBA  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown , PREFIX##unknown , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
 void (*PREFIX##_UPPER_FD_01_TABLE[32])(_VURegsNum *VUregsn) = { \
	PREFIX##MI_ADDAy , PREFIX##MI_SUBAy  , PREFIX##MI_MADDAy, PREFIX##MI_MSUBAy, \
	PREFIX##MI_ITOF4 , PREFIX##MI_FTOI4 , PREFIX##MI_MULAy , PREFIX##MI_ABS   , \
	PREFIX##MI_MADDAq, PREFIX##MI_MSUBAq, PREFIX##MI_MADDA , PREFIX##MI_MSUBA , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
 void (*PREFIX##_UPPER_FD_10_TABLE[32])(_VURegsNum *VUregsn) = { \
	PREFIX##MI_ADDAz , PREFIX##MI_SUBAz  , PREFIX##MI_MADDAz, PREFIX##MI_MSUBAz, \
	PREFIX##MI_ITOF12, PREFIX##MI_FTOI12, PREFIX##MI_MULAz , PREFIX##MI_MULAi , \
	PREFIX##MI_ADDAi, PREFIX##MI_SUBAi , PREFIX##MI_MULA  , PREFIX##MI_OPMULA, \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
 void (*PREFIX##_UPPER_FD_11_TABLE[32])(_VURegsNum *VUregsn) = { \
	PREFIX##MI_ADDAw , PREFIX##MI_SUBAw  , PREFIX##MI_MADDAw, PREFIX##MI_MSUBAw, \
	PREFIX##MI_ITOF15, PREFIX##MI_FTOI15, PREFIX##MI_MULAw , PREFIX##MI_CLIP  , \
	PREFIX##MI_MADDAi, PREFIX##MI_MSUBAi, PREFIX##unknown  , PREFIX##MI_NOP   , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
	PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , PREFIX##unknown  , \
}; \
 \
 \
 \
 void PREFIX##_UPPER_FD_00(_VURegsNum *VUregsn) { \
 PREFIX##_UPPER_FD_00_TABLE[(VU.code >> 6) & 0x1f ](VUregsn); \
} \
 \
 void PREFIX##_UPPER_FD_01(_VURegsNum *VUregsn) { \
 PREFIX##_UPPER_FD_01_TABLE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 void PREFIX##_UPPER_FD_10(_VURegsNum *VUregsn) { \
 PREFIX##_UPPER_FD_10_TABLE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 void PREFIX##_UPPER_FD_11(_VURegsNum *VUregsn) { \
 PREFIX##_UPPER_FD_11_TABLE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 void PREFIX##LowerOP(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_OPCODE[VU.code & 0x3f](VUregsn); \
} \
 \
 void PREFIX##LowerOP_T3_00(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_T3_00_OPCODE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 void PREFIX##LowerOP_T3_01(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_T3_01_OPCODE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 void PREFIX##LowerOP_T3_10(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_T3_10_OPCODE[(VU.code >> 6) & 0x1f](VUregsn); \
} \
 \
 void PREFIX##LowerOP_T3_11(_VURegsNum *VUregsn) { \
 PREFIX##LowerOP_T3_11_OPCODE[(VU.code >> 6) & 0x1f](VUregsn); \
}


#ifdef VUM_LOG

#define IdebugUPPER(VU) \
	VUM_LOG("%s", dis##VU##MicroUF(VU.code, VU.VI[REG_TPC].UL));
#define IdebugLOWER(VU) \
	VUM_LOG("%s", dis##VU##MicroLF(VU.code, VU.VI[REG_TPC].UL));
#define _vuExecMicroDebug(VU) \
	VUM_LOG("_vuExecMicro: %8.8x", VU.VI[REG_TPC].UL);

#else

#define IdebugUPPER(VU)
#define IdebugLOWER(VU)
#define _vuExecMicroDebug(VU)

#endif
