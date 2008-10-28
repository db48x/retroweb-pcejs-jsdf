/*****************************************************************************
 * pce                                                                       *
 *****************************************************************************/

/*****************************************************************************
 * File name:     src/arch/ibmpc/ibmpc.c                                     *
 * Created:       1999-04-16 by Hampa Hug <hampa@hampa.ch>                   *
 * Copyright:     (C) 1999-2008 Hampa Hug <hampa@hampa.ch>                   *
 *****************************************************************************/

/*****************************************************************************
 * This program is free software. You can redistribute it and / or modify it *
 * under the terms of the GNU General Public License version 2 as  published *
 * by the Free Software Foundation.                                          *
 *                                                                           *
 * This program is distributed in the hope  that  it  will  be  useful,  but *
 * WITHOUT  ANY   WARRANTY,   without   even   the   implied   warranty   of *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU  General *
 * Public License for more details.                                          *
 *****************************************************************************/

/* $Id$ */


#include "main.h"

#include <lib/iniram.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif


void pc_e86_hook (void *ext, unsigned char op1, unsigned char op2);

unsigned char pc_ppi_get_port_a (ibmpc_t *pc);
unsigned char pc_ppi_get_port_c (ibmpc_t *pc);
void pc_ppi_set_port_b (ibmpc_t *pc, unsigned char val);


static
void pc_setup_nvram (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t     *sct;
	const char    *fname;
	unsigned long base, size;

	pc->nvr = NULL;

	sct = ini_next_sct (ini, NULL, "nvram");
	if (sct == NULL) {
		return;
	}

	ini_get_string (sct, "file", &fname, NULL);
	ini_get_uint32 (sct, "base", &base, 0);
	ini_get_uint32 (sct, "size", &size, 65536);

	pce_log_tag (MSG_INF, "NVRAM:", "addr=0x%08lx size=%lu file=%s\n",
		base, size, (fname == NULL) ? "<>" : fname
	);

	pc->nvr = nvr_new (base, size);
	if (pc->nvr == NULL) {
		pce_log (MSG_ERR, "*** creating nvram failed\n");
		return;
	}

	nvr_set_endian (pc->nvr, 0);

	mem_add_blk (pc->mem, nvr_get_mem (pc->nvr), 0);

	if (fname != NULL) {
		if (nvr_set_fname (pc->nvr, fname)) {
			pce_log (MSG_ERR, "*** loading nvram failed (%s)\n", fname);
		}
	}
}

static
void pc_setup_cpu (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t  *sct;
	const char *model;

	sct = ini_next_sct (ini, NULL, "cpu");

	ini_get_string (sct, "model", &model,
		(par_cpu != NULL) ? par_cpu : "8086"
	);

	if (par_cpu != NULL) {
		model = par_cpu;
	}

	pce_log_tag (MSG_INF, "CPU:", "model=%s\n", model);

	pc->cpu = e86_new ();
	pc->cpu_model = PCE_CPU_8086;

	if (strcmp (model, "8086") == 0) {
		pc_set_cpu_model (pc, PCE_CPU_8086);
	}
	else if (strcmp (model, "8088") == 0) {
		pc_set_cpu_model (pc, PCE_CPU_8088);
	}
	else if ((strcmp (model, "v20") == 0) || (strcmp (model, "V20") == 0)) {
		pc_set_cpu_model (pc, PCE_CPU_V20);
	}
	else if ((strcmp (model, "v30") == 0) || (strcmp (model, "V30") == 0)) {
		pc_set_cpu_model (pc, PCE_CPU_V30);
	}
	else if ((strcmp (model, "80186") == 0) || (strcmp (model, "186") == 0)) {
		pc_set_cpu_model (pc, PCE_CPU_80186);
	}
	else if ((strcmp (model, "80188") == 0) || (strcmp (model, "188") == 0)) {
		pc_set_cpu_model (pc, PCE_CPU_80188);
	}
	else if ((strcmp (model, "80286") == 0) || (strcmp (model, "286") == 0)) {
		pc_set_cpu_model (pc, PCE_CPU_80286);
	}
	else {
		pce_log (MSG_ERR, "*** unknown cpu model (%s)\n", model);
	}

	e86_set_mem (pc->cpu, pc->mem,
		(e86_get_uint8_f) &mem_get_uint8,
		(e86_set_uint8_f) &mem_set_uint8,
		(e86_get_uint16_f) &mem_get_uint16_le,
		(e86_set_uint16_f) &mem_set_uint16_le
	);

	e86_set_prt (pc->cpu, pc->prt,
		(e86_get_uint8_f) &mem_get_uint8,
		(e86_set_uint8_f) &mem_set_uint8,
		(e86_get_uint16_f) &mem_get_uint16_le,
		(e86_set_uint16_f) &mem_set_uint16_le
	);

	if (pc->ram != NULL) {
		e86_set_ram (pc->cpu, pc->ram->data, pc->ram->size);
	}
	else {
		e86_set_ram (pc->cpu, NULL, 0);
	}

	pc->cpu->op_ext = pc;
	pc->cpu->op_hook = &pc_e86_hook;
}

static
void pc_setup_dma (ibmpc_t *pc, ini_sct_t *ini)
{
	unsigned long addr;
	ini_sct_t     *sct;
	mem_blk_t     *blk;

	sct = ini_next_sct (ini, NULL, "dmac");

	ini_get_uint32 (sct, "address", &addr, 0);

	pce_log_tag (MSG_INF, "DMAC:", "addr=0x%08x size=0x%04x\n", addr, 16);

	e8237_init (&pc->dma);

	blk = mem_blk_new (addr, 16, 0);
	if (blk == NULL) {
		pce_log (MSG_ERR, "*** allocating DMAC failed\n");
		return;
	}

	mem_blk_set_fct (blk, &pc->dma,
		e8237_get_uint8, e8237_get_uint16, e8237_get_uint32,
		e8237_set_uint8, e8237_set_uint16, e8237_set_uint32
	);

	mem_add_blk (pc->prt, blk, 1);

	/* This is a hack. HLDA should be connected to the CPU core. Instead,
	 * this will keep it permanently at high. */
	e8237_set_hlda (&pc->dma, 1);
}

static
void pc_setup_pic (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t     *sct;
	mem_blk_t     *blk;
	unsigned long addr;

	sct = ini_next_sct (ini, NULL, "pic");

	ini_get_uint32 (sct, "address", &addr, 0x0020);

	pce_log_tag (MSG_INF, "PIC:", "addr=0x%08x size=0x%04x\n", addr, 2);

	e8259_init (&pc->pic);

	blk = mem_blk_new (addr, 2, 0);
	if (blk == NULL) {
		return;
	}

	mem_blk_set_fct (blk, &pc->pic,
		e8259_get_uint8, e8259_get_uint16, e8259_get_uint32,
		e8259_set_uint8, e8259_set_uint16, e8259_set_uint32
	);

	mem_add_blk (pc->prt, blk, 1);

	e8259_set_int_fct (&pc->pic, pc->cpu, e86_irq);

	e86_set_inta_fct (pc->cpu, &pc->pic, e8259_inta);
}

static
void pc_setup_pit (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t     *sct;
	mem_blk_t     *blk;
	unsigned long addr;
	int           rt;

	sct = ini_next_sct (ini, NULL, "pit");

	ini_get_uint32 (sct, "address", &addr, 0x0040);
	ini_get_sint16 (sct, "realtime", &rt, 0);

	pce_log_tag (MSG_INF, "PIT:", "addr=0x%08lx size=0x%04x realtime=%d\n",
		addr, 4, (rt != 0)
	);

	e8253_init (&pc->pit);

	blk = mem_blk_new (addr, 4, 0);
	if (blk == NULL) {
		return;
	}

	mem_blk_set_fct (blk, &pc->pit,
		e8253_get_uint8, e8253_get_uint16, e8253_get_uint32,
		e8253_set_uint8, e8253_set_uint16, e8253_set_uint32
	);

	mem_add_blk (pc->prt, blk, 1);

	e8253_set_gate (&pc->pit, 0, 1);
	e8253_set_gate (&pc->pit, 1, 1);
	e8253_set_gate (&pc->pit, 2, 1);

	e8253_set_out_fct (&pc->pit, 0, &pc->pic, e8259_set_irq0);
	e8253_set_out_fct (&pc->pit, 1, &pc->dma, e8237_set_dreq0);

	pc->pit_real = (rt != 0);
	pc->pit_clk = 0;
	pc->pit_clkdiv = 0;
}

static
void pc_setup_ppi (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t     *sct;
	mem_blk_t     *blk;
	unsigned long addr;
	unsigned      ram;

	sct = ini_next_sct (ini, NULL, "ppi");

	if (pc->ram != NULL) {
		ram = mem_blk_get_size (pc->ram) / 32;
	}
	else {
		ram = 1;
	}

	ini_get_uint32 (sct, "address", &addr, 0x0060);

	pce_log_tag (MSG_INF, "PPI:", "addr=0x%08x size=0x%04x\n", addr, 4);

	e8255_init (&pc->ppi);

	pc->ppi.port[0].read_ext = pc;
	pc->ppi.port[0].read = (void *) pc_ppi_get_port_a;
	pc->ppi.port[1].write_ext = pc;
	pc->ppi.port[1].write = (void *) pc_ppi_set_port_b;
	pc->ppi.port[2].read_ext = pc;
	pc->ppi.port[2].read = (void *) pc_ppi_get_port_c;

	pc->ppi_port_a[0] = 0x30 | 0x0c;
	pc->ppi_port_a[1] = 0;
	pc->ppi_port_b = 0;
	pc->ppi_port_c[0] = (ram & 0x0f);
	pc->ppi_port_c[1] = (ram >> 4) & 0x01;

	blk = mem_blk_new (addr, 4, 0);
	if (blk == NULL) {
		return;
	}

	mem_blk_set_fct (blk, &pc->ppi,
		e8255_get_uint8, e8255_get_uint16, e8255_get_uint32,
		e8255_set_uint8, e8255_set_uint16, e8255_set_uint32
	);

	mem_add_blk (pc->prt, blk, 1);
}

static
void pc_setup_terminal (ibmpc_t *pc, ini_sct_t *ini)
{
	const char *driver;
	ini_sct_t  *sct;

	pc->trm = NULL;

	sct = ini_next_sct (ini, NULL, "terminal");
	ini_get_string (sct, "driver", &driver, "null");

	if (par_terminal != NULL) {
		while ((sct != NULL) && (strcmp (par_terminal, driver) != 0)) {
			sct = ini_next_sct (ini, sct, "terminal");
			ini_get_string (sct, "driver", &driver, "null");
		}

		if (sct == NULL) {
			driver = par_terminal;
		}
	}

	pce_log_tag (MSG_INF, "TERM:", "driver=%s\n", driver);

	if (strcmp (driver, "x11") == 0) {
#ifdef PCE_X11_USE
		pc->trm = xt_new (sct);
		if (pc->trm == NULL) {
			pce_log (MSG_ERR, "*** setting up x11 terminal failed\n");
		}
#else
		pce_log (MSG_ERR, "*** terminal driver 'x11' not supported\n");
#endif
	}
	else if (strcmp (driver, "sdl") == 0) {
#ifdef PCE_SDL_USE
		pc->trm = sdl_new (sct);
		if (pc->trm == NULL) {
			pce_log (MSG_ERR, "*** setting up sdl terminal failed\n");
		}
#else
		pce_log (MSG_ERR, "*** terminal driver 'sdl' not supported\n");
#endif
	}
	else if (strcmp (driver, "null") == 0) {
		pc->trm = null_new (sct);
		if (pc->trm == NULL) {
			pce_log (MSG_ERR, "*** setting up null terminal failed\n");
		}
	}
	else if (strcmp (driver, "vt100") == 0) {
#ifdef PCE_VT100_USE
		pc->trm = vt100_new (sct, 0, 1);
		if (pc->trm == NULL) {
			pce_log (MSG_ERR, "*** setting up vt100 terminal failed\n");
		}
#else
		pce_log (MSG_ERR, "*** terminal driver 'vt100' not supported\n");
#endif
	}
	else {
		pce_log (MSG_ERR, "*** unknown terminal driver: %s\n", driver);
	}

	if (pc->trm == NULL) {
		pce_log (MSG_ERR, "*** no terminal found\n");
		return;
	}

	trm_set_key_fct (pc->trm, pc, pc_set_keycode);
	trm_set_msg_fct (pc->trm, pc, pc_set_msg, pc_get_msgul);
}

static
int pc_setup_mda (ibmpc_t *pc, ini_sct_t *sct)
{
	pc->video = mda_new (pc->trm, sct);
	if (pc->video == NULL) {
		return (1);
	}

	mem_add_blk (pc->mem, pce_video_get_mem (pc->video), 0);
	mem_add_blk (pc->prt, pce_video_get_reg (pc->video), 0);

	pc->ppi_port_a[0] &= ~0x30;
	pc->ppi_port_a[0] |= 0x30;

	return (0);
}

static
int pc_setup_hgc (ibmpc_t *pc, ini_sct_t *sct)
{
	pc->video = hgc_new (pc->trm, sct);
	if (pc->video == NULL) {
		return (1);
	}

	mem_add_blk (pc->mem, pce_video_get_mem (pc->video), 0);
	mem_add_blk (pc->prt, pce_video_get_reg (pc->video), 0);

	pc->ppi_port_a[0] &= ~0x30;
	pc->ppi_port_a[0] |= 0x30;

	return (0);
}

static
int pc_setup_cga (ibmpc_t *pc, ini_sct_t *sct)
{
	pc->video = cga_new (pc->trm, sct);
	if (pc->video == NULL) {
		return (1);
	}

	mem_add_blk (pc->mem, pce_video_get_mem (pc->video), 0);
	mem_add_blk (pc->prt, pce_video_get_reg (pc->video), 0);

	pc->ppi_port_a[0] &= ~0x30;
	pc->ppi_port_a[0] |= 0x20;

	return (0);
}

static
int pc_setup_ega (ibmpc_t *pc, ini_sct_t *sct)
{
	pc->video = ega_new (pc->trm, sct);
	if (pc->video == NULL) {
		return (1);
	}

	mem_add_blk (pc->mem, pce_video_get_mem (pc->video), 0);
	mem_add_blk (pc->prt, pce_video_get_reg (pc->video), 0);

	pc->ppi_port_a[0] &= ~0x30;
	pc->ppi_port_a[0] |= 0x00;

	return (0);
}

static
int pc_setup_vga (ibmpc_t *pc, ini_sct_t *sct)
{
	pc->video = vga_new (pc->trm, sct);
	if (pc->video == NULL) {
		return (1);
	}

	mem_add_blk (pc->mem, pce_video_get_mem (pc->video), 0);
	mem_add_blk (pc->prt, pce_video_get_reg (pc->video), 0);

	pc->ppi_port_a[0] &= ~0x30;
	pc->ppi_port_a[0] |= 0x00;

	return (0);
}

static
void pc_setup_video (ibmpc_t *pc, ini_sct_t *ini)
{
	const char *dev;
	ini_sct_t  *sct;

	pc->video = NULL;

	sct = ini_next_sct (ini, NULL, "video");
	ini_get_string (sct, "device", &dev, "cga");

	if (par_video != NULL) {
		while ((sct != NULL) && (strcmp (par_video, dev) != 0)) {
			sct = ini_next_sct (ini, sct, "video");
			ini_get_string (sct, "device", &dev, "cga");
		}

		if (sct == NULL) {
			dev = par_video;
		}
	}

	pce_log_tag (MSG_INF, "VIDEO:", "device=%s\n", dev);

	if (strcmp (dev, "vga") == 0) {
		pc_setup_vga (pc, sct);
	}
	else if (strcmp (dev, "ega") == 0) {
		pc_setup_ega (pc, sct);
	}
	else if (strcmp (dev, "cga") == 0) {
		pc_setup_cga (pc, sct);
	}
	else if (strcmp (dev, "hgc") == 0) {
		pc_setup_hgc (pc, sct);
	}
	else if (strcmp (dev, "mda") == 0) {
		pc_setup_mda (pc, sct);
	}
	else {
		pce_log (MSG_ERR, "*** unknown video device (%s)\n", dev);
	}

	if (pc->video == NULL) {
		pce_log (MSG_ERR, "*** setting up video device failed\n");
	}

	if (pc->video != NULL) {
		if (pc->trm != NULL) {
			pce_video_set_terminal (pc->video, pc->trm);
		}
	}
}

static
void pc_setup_disks (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t *sct;
	disk_t    *dsk;

	pc->dsk = dsks_new();

	sct = NULL;
	while ((sct = ini_next_sct (ini, sct, "disk")) != NULL) {
		if (ini_get_disk (sct, &dsk)) {
			pce_log (MSG_ERR, "*** loading drive failed\n");
			continue;
		}

		if (dsk == NULL) {
			continue;
		}

		dsks_add_disk (pc->dsk, dsk);

		if (dsk_get_drive (dsk) < 0x80) {
			/* if floppy disk increase number of floppy disks in config word */
			if (pc->ppi_port_a[0] & 0x01) {
				pc->ppi_port_a[0] = (pc->ppi_port_a[0] + 0x40) & 0xff;
			}
			else {
				pc->ppi_port_a[0] |= 0x01;
			}

			pc->fd_cnt += 1;
		}
		else {
			pc->hd_cnt += 1;
		}
	}
}

static
void pc_setup_mouse (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t     *sct;
	unsigned long addr;
	unsigned      irq;

	sct = ini_next_sct (ini, NULL, "mouse");
	if (sct == NULL) {
		return;
	}

	if (ini_get_uint32 (sct, "address", &addr, 0x03f8)) {
		ini_get_uint32 (sct, "io", &addr, 0x03f8);
	}
	ini_get_uint16 (sct, "irq", &irq, 4);

	pce_log_tag (MSG_INF, "MOUSE:", "addr=0x%04lx irq=%u\n", addr, irq);

	pc->mse = mse_new (addr, sct);

	e8250_set_irq_fct (&pc->mse->uart, &pc->pic, e8259_get_irq_f (&pc->pic, irq));

	mem_add_blk (pc->prt, mse_get_reg (pc->mse), 0);

	pc->trm->mse_ext = pc->mse;
	pc->trm->set_mse = (void *) mse_set;
}

static
void pc_setup_parport (ibmpc_t *pc, ini_sct_t *ini)
{
	unsigned        i;
	unsigned long   addr;
	const char      *fname;
	ini_sct_t       *sct;
	static unsigned defbase[4] = { 0x378, 0x278, 0x3bc, 0x2bc };

	for (i = 0; i < 4; i++) {
		pc->parport[i] = NULL;
	}

	i = 0;
	sct = NULL;
	while ((sct = ini_next_sct (ini, sct, "parport")) != NULL) {
		if (i >= 4) {
			break;
		}

		if (ini_get_uint32 (sct, "address", &addr, defbase[i])) {
			ini_get_uint32 (sct, "io", &addr, defbase[i]);
		}
		ini_get_string (sct, "file", &fname, NULL);

		pce_log_tag (MSG_INF,
			"PARPORT:", "LPT%u addr=0x%04lx file=%s\n",
			i + 1, addr, (fname == NULL) ? "<none>" : fname
		);

		pc->parport[i] = parport_new (addr);
		if (pc->parport[i] == NULL) {
			pce_log (MSG_ERR,
				"*** parport setup failed [%04X -> %s]\n",
				addr, (fname == NULL) ? "<none>" : fname
			);
		}
		else {
			if (fname != NULL) {
				if (parport_set_fname (pc->parport[i], fname)) {
					pce_log (MSG_ERR,
						"*** can't open file (%s)\n",
						fname
					);
				}
			}

			mem_add_blk (pc->prt, parport_get_reg (pc->parport[i]), 0);

			i += 1;
		}
	}
}

static
void pc_setup_serport (ibmpc_t *pc, ini_sct_t *ini)
{
	unsigned      i;
	unsigned long addr;
	unsigned      irq;
	const char    *fname, *dname;
	const char    *chip;
	ini_sct_t     *sct;

	static unsigned long defbase[4] = { 0x3f8, 0x2f8, 0x3e8, 0x2e8 };
	static unsigned      defirq[4] = { 4, 3, 4, 3 };

	for (i = 0; i < 4; i++) {
		pc->serport[i] = NULL;
	}

	i = 0;
	sct = NULL;
	while ((sct = ini_next_sct (ini, sct, "serial")) != NULL) {
		if (i >= 4) {
			break;
		}

		if (ini_get_uint32 (sct, "address", &addr, defbase[i])) {
			ini_get_uint32 (sct, "io", &addr, defbase[i]);
		}
		ini_get_uint16 (sct, "irq", &irq, defirq[i]);
		ini_get_string (sct, "uart", &chip, "8250");
		ini_get_string (sct, "file", &fname, NULL);
		ini_get_string (sct, "device", &dname, NULL);

		pce_log_tag (MSG_INF,
			"SERPORT:", "COM%u addr=0x%04lx irq=%u uart=%s file=%s dev=%s\n",
			i + 1, addr, irq, chip,
			(fname == NULL) ? "<none>" : fname,
			(dname == NULL) ? "<none>" : dname
		);

		pc->serport[i] = ser_new (addr, 0);
		if (pc->serport[i] == NULL) {
			pce_log (MSG_ERR, "*** serial port setup failed [%04X/%u -> %s]\n",
				addr, irq, (fname == NULL) ? "<none>" : fname
			);
		}
		else {
			if (fname != NULL) {
				if (ser_set_fname (pc->serport[i], fname)) {
					pce_log (MSG_ERR, "*** can't open file (%s)\n", fname);
				}
			}

			if (dname != NULL) {
				if (ser_set_dname (pc->serport[i], dname)) {
					pce_log (MSG_ERR, "*** can't open device (%s)\n", dname);
				}
			}

			if (e8250_set_chip_str (&pc->serport[i]->uart, chip)) {
				pce_log (MSG_ERR, "*** unknown UART chip (%s)\n", chip);
			}

			e8250_set_irq_fct (&pc->serport[i]->uart,
				&pc->pic, e8259_get_irq_f (&pc->pic, irq)
			);

			mem_add_blk (pc->prt, ser_get_reg (pc->serport[i]), 0);

			i += 1;
		}
	}
}

static
void pc_setup_ems (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t *sct;
	mem_blk_t *mem;

	pc->ems = NULL;

	sct = ini_next_sct (ini, NULL, "ems");
	if (sct == NULL) {
		return;
	}

	pc->ems = ems_new (sct);
	if (pc->ems == NULL) {
		return;
	}

	mem = ems_get_mem (pc->ems);
	if (mem != NULL) {
		mem_add_blk (pc->mem, mem, 0);
	}
}

static
void pc_setup_xms (ibmpc_t *pc, ini_sct_t *ini)
{
	ini_sct_t *sct;
	mem_blk_t *mem;

	pc->xms = NULL;

	sct = ini_next_sct (ini, NULL, "xms");
	if (sct == NULL) {
		return;
	}

	pc->xms = xms_new (sct);
	if (pc->xms == NULL) {
		return;
	}

	mem = xms_get_umb_mem (pc->xms);
	if (mem != NULL) {
		mem_add_blk (pc->mem, mem, 0);
	}

	mem = xms_get_hma_mem (pc->xms);
	if (mem != NULL) {
		mem_add_blk (pc->mem, mem, 0);
	}
}

ibmpc_t *pc_new (ini_sct_t *ini)
{
	unsigned  i;
	ibmpc_t   *pc;

	pc = malloc (sizeof (ibmpc_t));
	if (pc == NULL) {
		return (NULL);
	}

	pc->cfg = ini;

	pc->key_i = 0;
	pc->key_j = 0;

	pc->bootdrive = 128;

	ini_get_bool (ini, "rtc", &pc->support_rtc, 1);

	pc->fd_cnt = 0;
	pc->hd_cnt = 0;

	pc->brk = 0;
	pc->pause = 0;
	pc->clk_cnt = 0;

	bps_init (&pc->bps);

	for (i = 0; i < 4; i++) {
		pc->clk_div[i] = 0;
	}

	pc->mem = mem_new();
	pc->prt = mem_new();

	ini_get_ram (pc->mem, ini, &pc->ram);
	ini_get_rom (pc->mem, ini);

	pc_setup_nvram (pc, ini);
	pc_setup_cpu (pc, ini);
	pc_setup_dma (pc, ini);
	pc_setup_pic (pc, ini);
	pc_setup_pit (pc, ini);
	pc_setup_ppi (pc, ini);

	pc_setup_terminal (pc, ini);

	if (pc->trm != NULL) {
		pc_setup_video (pc, ini);
	}

	pc_setup_disks (pc, ini);
	pc_setup_mouse (pc, ini);
	pc_setup_serport (pc, ini);
	pc_setup_parport (pc, ini);
	pc_setup_ems (pc, ini);
	pc_setup_xms (pc, ini);

	pce_load_mem_ini (pc->mem, ini);

	return (pc);
}

void pc_del_xms (ibmpc_t *pc)
{
	xms_del (pc->xms);
	pc->xms = NULL;
}

void pc_del_ems (ibmpc_t *pc)
{
	ems_del (pc->ems);
	pc->ems = NULL;
}

void pc_del_mouse (ibmpc_t *pc)
{
	if (pc->mse != NULL) {
		mse_del (pc->mse);
		pc->mse = NULL;
	}
}

void pc_del_parport (ibmpc_t *pc)
{
	unsigned i;

	for (i = 0; i < 4; i++) {
		if (pc->parport[i] != NULL) {
			parport_del (pc->parport[i]);
		}
	}
}

void pc_del_serport (ibmpc_t *pc)
{
	unsigned i;

	for (i = 0; i < 4; i++) {
		if (pc->serport[i] != NULL) {
			ser_del (pc->serport[i]);
		}
	}
}

void pc_del (ibmpc_t *pc)
{
	if (pc == NULL) {
		return;
	}

	bps_free (&pc->bps);

	pc_del_xms (pc);
	pc_del_ems (pc);
	pc_del_parport (pc);
	pc_del_serport (pc);
	pc_del_mouse (pc);

	dsks_del (pc->dsk);

	pce_video_del (pc->video);

	trm_del (pc->trm);

	e8237_free (&pc->dma);
	e8255_free (&pc->ppi);
	e8253_free (&pc->pit);
	e8259_free (&pc->pic);
	e86_del (pc->cpu);

	nvr_del (pc->nvr);
	mem_del (pc->mem);
	mem_del (pc->prt);

	ini_sct_del (pc->cfg);

	free (pc);
}

#ifdef HAVE_GETTIMEOFDAY
static
void pc_clock_pit (ibmpc_t *pc, unsigned n)
{
	unsigned long  clk0, clk1;
	struct timeval tv;

	if (pc->pit_real == 0) {
		e8253_clock (&pc->pit, n);
		return;
	}

	pc->pit_clkdiv += n;

	if (pc->pit_clkdiv < 256) {
		return;
	}

	pc->pit_clkdiv &= 255;

	if (gettimeofday (&tv, NULL)) {
		e8253_clock (&pc->pit, n);
		return;
	}

	clk0 = (1000000UL * tv.tv_sec + tv.tv_usec) & 0xffffffffUL;
	clk0 += clk0 / 8;
	clk1 = (clk0 - pc->pit_clk) & 0xffffffffUL;

	pc->pit_clk = clk0;

	if (clk1 > 1190000UL) {
		clk1 = 1190000UL;
	}

	e8253_clock (&pc->pit, clk1);
}
#endif

void pc_clock (ibmpc_t *pc)
{
	unsigned long n;

	n = e86_get_delay (pc->cpu);

	e86_clock (pc->cpu, n);

	pce_video_clock0 (pc->video, n, 4);

	pc->clk_cnt += n;
	pc->clk_div[0] += n;

	if (pc->clk_div[0] >= 32) {
		e8237_clock (&pc->dma, 1);
		e8259_clock (&pc->pic);

		pce_video_clock1 (pc->video, 0);

#ifdef HAVE_GETTIMEOFDAY
		if (pc->pit_real) {
			pc_clock_pit (pc, 8 * (pc->clk_div[0] / 32));
		}
		else {
			e8253_clock (&pc->pit, 8 * (pc->clk_div[0] / 32));
		}
#else
		e8253_clock (&pc->pit, 8 * (pc->clk_div[0] / 32));
#endif

		pc->clk_div[1] += pc->clk_div[0] & ~0x1fUL;

		pc->clk_div[0] &= 0x1f;

		if (pc->clk_div[1] >= 4096) {
			unsigned      i;
			unsigned long clk;

			clk = pc->clk_div[1] & ~4095UL;

			trm_check (pc->trm);

			for (i = 0; i < 4; i++) {
				if (pc->serport[i] != NULL) {
					ser_clock (pc->serport[i], clk);
				}
			}

			if (pc->key_i < pc->key_j) {
				pc->ppi_port_a[1] = pc->key_buf[pc->key_i];
				e8259_set_irq1 (&pc->pic, 1);
				pc->key_i += 1;

				if (pc->key_i == pc->key_j) {
					pc->key_i = 0;
					pc->key_j = 0;
				}
			}

			pc->clk_div[1] &= 4095;
		}
	}
}

void pc_screenshot (ibmpc_t *pc, const char *fname)
{
	static unsigned i = 0;
	char            tmp[256];
	FILE            *fp;

	if (fname == NULL) {
		sprintf (tmp, "snap%04u.dat", i++);
		fname = tmp;
	}

	fp = fopen (fname, "wb");
	if (fp == NULL) {
		return;
	}

	pce_video_screenshot (pc->video, fp, 0);

	fclose (fp);
}

int pc_set_cpu_model (ibmpc_t *pc, unsigned model)
{
	switch (model) {
		case PCE_CPU_8086:
		case PCE_CPU_8088:
			e86_enable_86 (pc->cpu);
			break;

		case PCE_CPU_V20:
		case PCE_CPU_V30:
			e86_enable_v30 (pc->cpu);
			break;

		case PCE_CPU_80186:
		case PCE_CPU_80188:
			e86_enable_186 (pc->cpu);
			break;

		case PCE_CPU_80286:
			e86_enable_286 (pc->cpu);
			break;

		default:
			return (1);
	}

	pc->cpu_model = model;

	return (0);
}

void pc_set_bootdrive (ibmpc_t *pc, unsigned drv)
{
	pc->bootdrive = drv;
}

unsigned pc_get_bootdrive (ibmpc_t *pc)
{
	return (pc->bootdrive);
}

void pc_break (ibmpc_t *pc, unsigned char val)
{
	if (pc == NULL) {
		pc = par_pc;
	}

	if ((val == PCE_BRK_STOP) || (val == PCE_BRK_ABORT)) {
		pc->brk = val;
	}
}

unsigned char pc_ppi_get_port_a (ibmpc_t *pc)
{
	if (pc->ppi_port_b & 0x80) {
		return (pc->ppi_port_a[0]);
	}
	else {
		return (pc->ppi_port_a[1]);
	}
}

unsigned char pc_ppi_get_port_c (ibmpc_t *pc)
{
	if (pc->ppi_port_b & 0x04) {
		return (pc->ppi_port_c[1]);
	}
	else {
		return (pc->ppi_port_c[0]);
	}
}

void pc_ppi_set_port_b (ibmpc_t *pc, unsigned char val)
{
	pc->ppi_port_b = val;

	e8253_set_gate (&pc->pit, 2, val & 0x01);
}

void pc_set_keycode (ibmpc_t *pc, unsigned char val)
{
	if (pc->key_j > 255) {
		return;
	}

	pc->key_buf[pc->key_j] = val;
	pc->key_j += 1;

#if 0
	if (((e8259_get_isr (&pc->pic) | e8259_get_irr (&pc->pic)) & 0x01) == 0) {
		pc->ppi_port_a[1] = pc->key_buf[pc->key_i];
		pc->key_i += 1;

		if (pc->key_i == pc->key_j) {
			pc->key_i = 0;
			pc->key_j = 0;
		}

		e8259_set_irq1 (&pc->pic, 1);
	}
#endif
}
