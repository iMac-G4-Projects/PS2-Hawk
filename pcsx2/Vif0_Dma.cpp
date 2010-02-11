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

#include "PrecompiledHeader.h"
#include "Common.h"
#include "Vif_Dma.h"
#include "VUmicro.h"
#include "newVif.h"

__forceinline void vif0FLUSH()
{
	int _cycles = VU0.cycle;
	// Run VU0 until finish, don't add cycles to EE
	// because its vif stalling not the EE core...
	vu0Finish();
	g_vifCycles += (VU0.cycle - _cycles) * BIAS;
}

bool VIF0transfer(u32 *data, int size, bool istag)
{
	int ret;
	int transferred = vif0.vifstalled ? vif0.irqoffset : 0; // irqoffset necessary to add up the right qws, or else will spin (spiderman)
	VIF_LOG("VIF0transfer: size %x (vif0.cmd %x)", size, vif0.cmd);

	vif0.stallontag = false;
	vif0.vifstalled = false;
	vif0.vifpacketsize = size;

	while (vif0.vifpacketsize > 0)
	{
		if (vif0.cmd)
		{
			vif0Regs->stat.VPS = VPS_TRANSFERRING; //Decompression has started

			ret = Vif0TransTLB[(vif0.cmd & 0x7f)](data);
			data += ret;
			vif0.vifpacketsize -= ret;
			if (vif0.cmd == 0) vif0Regs->stat.VPS = VPS_IDLE; //We are once again waiting for a new vifcode as the command has cleared
			continue;
		}

		if (vif0.tag.size != 0) Console.WriteLn("no vif0 cmd but tag size is left last cmd read %x", vif0Regs->code);

		// if interrupt and new cmd is NOT MARK
		if (vif0.irq) break;

		vif0.cmd = (data[0] >> 24);
		vif0Regs->code = data[0];

		vif0Regs->stat.VPS |= VPS_DECODING; //We need to set these (Onimusha needs it)

		if ((vif0.cmd & 0x60) == 0x60)
		{
			vif0UnpackSetup(data);
		}
		else
		{
			VIF_LOG("VIFtransfer: cmd %x, num %x, imm %x, size %x", vif0.cmd, (data[0] >> 16) & 0xff, data[0] & 0xffff, size);

			if ((vif0.cmd & 0x7f) > 0x4A)
			{
				if (!(vif0Regs->err.ME1))    //Ignore vifcode and tag mismatch error
				{
					Console.WriteLn("UNKNOWN VifCmd: %x", vif0.cmd);
					vif0Regs->stat.ER1 = true;
					vif0.irq++;
				}
				vif0.cmd = 0;
			}
			else
			{
				Vif0CMDTLB[(vif0.cmd & 0x7f)]();
			}
		}
		++data;
		--vif0.vifpacketsize;

		if ((vif0.cmd & 0x80))
		{
			vif0.cmd &= 0x7f;

			if (!(vif0Regs->err.MII)) //i bit on vifcode and not masked by VIF0_ERR
			{
				VIF_LOG("Interrupt on VIFcmd: %x (INTC_MASK = %x)", vif0.cmd, psHu32(INTC_MASK));

				++vif0.irq;

				if (istag && vif0.tag.size <= vif0.vifpacketsize) vif0.stallontag = true;

				if (vif0.tag.size == 0) break;
			}
		}
	} //End of Transfer loop

	transferred += size - vif0.vifpacketsize;
	g_vifCycles += (transferred >> 2) * BIAS; /* guessing */
	// use tag.size because some game doesn't like .cmd

	if (vif0.irq && (vif0.tag.size == 0))
	{
		vif0.vifstalled = true;

		if (((vif0Regs->code >> 24) & 0x7f) != 0x7) vif0Regs->stat.VIS = true;
		//else Console.WriteLn("VIF0 IRQ on MARK");

		// spiderman doesn't break on qw boundaries
		vif0.irqoffset = transferred % 4; // cannot lose the offset

		if (!istag)
		{
			transferred = transferred >> 2;
			vif0ch->madr += (transferred << 4);
			vif0ch->qwc -= transferred;
		}
		//else Console.WriteLn("Stall on vif0, FromSPR = %x, Vif0MADR = %x Sif0MADR = %x STADR = %x", psHu32(0x1000d010), vif0ch->madr, psHu32(0x1000c010), psHu32(DMAC_STADR));
		return false;
	}

	vif0Regs->stat.VPS = VPS_IDLE; //Vif goes idle as the stall happened between commands;
	if (vif0.cmd) vif0Regs->stat.VPS |= VPS_WAITING;  //Otherwise we wait for the data

	if (!istag)
	{
		transferred   = transferred >> 2;
		vif0ch->madr +=(transferred << 4);
		vif0ch->qwc  -= transferred;
	}

	return true;
}

bool  _VIF0chain()
{
	u32 *pMem;

	if ((vif0ch->qwc == 0) && !vif0.vifstalled) return true;

	pMem = (u32*)dmaGetAddr(vif0ch->madr);
	if (pMem == NULL)
	{
		vif0.cmd = 0;
		vif0.tag.size = 0;
		vif0ch->qwc = 0;
		return true;
	}

	if (vif0.vifstalled)
		return VIF0transfer(pMem + vif0.irqoffset, vif0ch->qwc * 4 - vif0.irqoffset, false);
	else
		return VIF0transfer(pMem, vif0ch->qwc * 4, false);
}

bool _chainVIF0()
{
    tDMA_TAG *ptag;
    
	ptag = dmaGetAddr(vif0ch->tadr); //Set memory pointer to TADR

	if (!(vif0ch->transfer("Vif0 Tag", ptag))) return false;

	vif0ch->madr = ptag[1]._u32;		// MADR = ADDR field + SPR
    g_vifCycles += 1; 				// Increase the QW read for the tag

	VIF_LOG("dmaChain %8.8x_%8.8x size=%d, id=%d, madr=%lx, tadr=%lx",
	        ptag[1]._u32, ptag[0]._u32, vif0ch->qwc, ptag->ID, vif0ch->madr, vif0ch->tadr);

	// Transfer dma tag if tte is set
	if (vif0ch->chcr.TTE)
	{
	    bool ret;

		if (vif0.vifstalled)
			ret = VIF0transfer((u32*)ptag + (2 + vif0.irqoffset), 2 - vif0.irqoffset, true);  //Transfer Tag on stall
		else
			ret = VIF0transfer((u32*)ptag + 2, 2, true);  //Transfer Tag

		if (!(ret)) return false;        //IRQ set by VIFTransfer
	}

	vif0.done |= hwDmacSrcChainWithStack(vif0ch, ptag->ID);

	VIF_LOG("dmaChain %8.8x_%8.8x size=%d, id=%d, madr=%lx, tadr=%lx",
	        ptag[1]._u32, ptag[0]._u32, vif0ch->qwc, ptag->ID, vif0ch->madr, vif0ch->tadr);

	_VIF0chain();											   //Transfers the data set by the switch

	if (vif0ch->chcr.TIE && ptag->IRQ)  //Check TIE bit of CHCR and IRQ bit of tag
	{
		VIF_LOG("dmaIrq Set\n");
		vif0.done = true; //End Transfer
	}
	
	return vif0.done;
}

void vif0Interrupt()
{
	g_vifCycles = 0; //Reset the cycle count, Wouldn't reset on stall if put lower down.
	VIF_LOG("vif0Interrupt: %8.8x", cpuRegs.cycle);

	if (vif0.irq && (vif0.tag.size == 0))
	{
		vif0Regs->stat.INT = true;
		hwIntcIrq(VIF0intc);
		--vif0.irq;

		if (vif0Regs->stat.test(VIF0_STAT_VSS | VIF0_STAT_VIS | VIF0_STAT_VFS))
		{
			vif0Regs->stat.FQC = 0;
			vif0ch->chcr.STR = false;
			return;
		}

		if (vif0ch->qwc > 0 || vif0.irqoffset > 0)
		{
			if (vif0.stallontag)
				_chainVIF0();
			else
				_VIF0chain();

			CPU_INT(DMAC_VIF0, /*g_vifCycles*/ VifCycleVoodoo);
			return;
		}
	}

	if (!vif0ch->chcr.STR) Console.WriteLn("Vif0 running when CHCR = %x", vif0ch->chcr._u32);

	if ((vif0ch->chcr.MOD == CHAIN_MODE) && (!vif0.done) && (!vif0.vifstalled))
	{

		if (!(dmacRegs->ctrl.DMAE))
		{
			Console.WriteLn("vif0 dma masked");
			return;
		}

		if (vif0ch->qwc > 0)
			_VIF0chain();
		else
			_chainVIF0();

		CPU_INT(DMAC_VIF0, /*g_vifCycles*/ VifCycleVoodoo);
		return;
	}

	if (vif0ch->qwc > 0) Console.WriteLn("VIF0 Ending with QWC left");
	if (vif0.cmd != 0) Console.WriteLn("vif0.cmd still set %x", vif0.cmd);

	vif0ch->chcr.STR = false;
	hwDmacIrq(DMAC_VIF0);
	vif0Regs->stat.FQC = 0;
}

void dmaVIF0()
{
	VIF_LOG("dmaVIF0 chcr = %lx, madr = %lx, qwc  = %lx\n"
	        "        tadr = %lx, asr0 = %lx, asr1 = %lx\n",
	        vif0ch->chcr._u32, vif0ch->madr, vif0ch->qwc,
	        vif0ch->tadr, vif0ch->asr0, vif0ch->asr1);

	g_vifCycles = 0;

	vif0Regs->stat.FQC = 0x8; // FQC=8

	if (!(vif0ch->chcr.MOD & 0x1) || vif0ch->qwc > 0)   // Normal Mode
	{
		if (!_VIF0chain())
		{
			Console.WriteLn(L"Stall on normal vif0 " + vif0Regs->stat.desc());

			vif0.vifstalled = true;
			return;
		}

		vif0.done = true;
		CPU_INT(DMAC_VIF0, /*g_vifCycles*/ VifCycleVoodoo);
		return;
	}

	// Chain Mode
	vif0.done = false;
	CPU_INT(DMAC_VIF0, 0);
}
