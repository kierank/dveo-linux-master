/* dvbm_q3io.c
 *
 * Linux driver functions for Linear Systems Ltd. DVB Master Quad-1in3out.
 *
 * Copyright (C) 2007-2010 Linear Systems Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either Version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public Licence for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * Linear Systems can be contacted at <http://www.linsys.ca/>.
 *
 */

#include <linux/kernel.h> /* KERN_INFO */
#include <linux/module.h> /* THIS_MODULE */

#include <linux/fs.h> /* inode, file, file_operations */
#include <linux/sched.h> /* pt_regs */
#include <linux/pci.h> /* pci_resource_start () */
#include <linux/slab.h> /* kzalloc () */
#include <linux/list.h> /* INIT_LIST_HEAD () */
#include <linux/spinlock.h> /* spin_lock_init () */
#include <linux/init.h> /* __init */
#include <linux/errno.h> /* error codes */
#include <linux/interrupt.h> /* irqreturn_t */
#include <linux/device.h> /* device_create_file () */
#include <linux/mutex.h> /* mutex_init () */

#include <linux/bitops.h> /* set_bit () */

#include "asicore.h"
#include "../include/master.h"
#include "mdev.h"
#include "dvbm.h"
#include "mdma.h"
#include "dvbm_qio.h"
#include "plx9080.h"
#include "lsdma.h"

static const char dvbm_q3ioe_name[] = DVBM_NAME_Q3IOE;

/* static function prototypes */
static irqreturn_t IRQ_HANDLER (dvbm_q3io_irq_handler, irq, dev_id, regs);

static DEVICE_ATTR(blackburst_type,S_IRUGO|S_IWUSR,
	dvbm_qio_show_blackburst_type,dvbm_qio_store_blackburst_type);
static DEVICE_ATTR(uid,S_IRUGO,
	dvbm_qio_show_uid,NULL);

/**
 * dvbm_q3io_pci_probe - PCI insertion handler for a DVB Master Quad-1in3out
 * @pdev: PCI device
 *
 * Handle the insertion of a DVB Master Quad-1in3out.
 * Returns a negative error code on failure and 0 on success.
 **/

int __init
dvbm_q3io_pci_probe (struct pci_dev *pdev)
{
	int err, i;
	unsigned int cap;
	struct master_dev *card;
	void __iomem *p;

	err = dvbm_pci_probe_generic (pdev);
	if (err < 0) {
		goto NO_PCI;
	}

	/* Initialize the driver_data pointer so that dvbm_q3io_pci_remove()
	 * doesn't try to free it if an error occurs */
	pci_set_drvdata (pdev, NULL);

	/* Allocate a board info structure */
	if ((card = (struct master_dev *)
		kzalloc (sizeof (*card), GFP_KERNEL)) == NULL) {
		err = -ENOMEM;
		goto NO_MEM;
	}

	/* Initialize the board info structure */
	/* LS DMA Controller */
	card->bridge_addr = ioremap (pci_resource_start (pdev, 3),
		pci_resource_len (pdev, 3));
	/* ASI Core */
	card->core.addr = ioremap (pci_resource_start (pdev, 2),
		pci_resource_len (pdev, 2));
	card->version = readl(card->core.addr + DVBM_QIO_FPGAID) & 0xffff;
	card->name = dvbm_q3ioe_name;
	card->id = pdev->device;
	card->irq = pdev->irq;
	card->irq_handler = dvbm_q3io_irq_handler;
	INIT_LIST_HEAD(&card->iface_list);
	card->capabilities = MASTER_CAP_BLACKBURST | MASTER_CAP_UID;
	/* Lock for ICSR */
	spin_lock_init (&card->irq_lock);
	/* Lock for IBSTR, IPSTR, FTR, PFLUT, TCSR, RCSR */
	spin_lock_init (&card->reg_lock);
	mutex_init (&card->users_mutex);
	card->parent = &pdev->dev;

	/* Print the firmware version */
	printk (KERN_INFO "%s: %s detected, firmware version %u.%u (0x%04X)\n",
		dvbm_driver_name, card->name,
		card->version >> 8, card->version & 0x00ff, card->version);

	/* Store the pointer to the board info structure
	 * in the PCI info structure */
	pci_set_drvdata (pdev, card);

	/* PLX */
	p = ioremap (pci_resource_start (pdev, 0),
		pci_resource_len (pdev, 0));

	/* Reset PCI 9056 */
	plx_reset_bridge(p);

	/* Setup the PCI 9056 */
	writel(PLX_INTCSR_PCIINT_ENABLE |
		PLX_INTCSR_PCILOCINT_ENABLE, p + PLX_INTCSR);

	/* Dummy read to flush PCI posted wires */
	readl(p + PLX_INTCSR);

	/* Unmap PLX */
	iounmap (p);

	/* Reset the FPGA */
	for (i = 0; i < 3; i++) {
		writel (DVBM_QIO_TCSR_TXRST,
			card->core.addr + DVBM_QIO_TCSR(i));
	}
	for (i = 3; i < 4; i++) {
		writel (DVBM_QIO_RCSR_RXRST,
			card->core.addr + DVBM_QIO_RCSR(i));
	}

	/* Setup the LS DMA Controller */
	writel (LSDMA_INTMSK_CH(0) | LSDMA_INTMSK_CH(1) | LSDMA_INTMSK_CH(2)
		| LSDMA_INTMSK_CH(3),
		card->bridge_addr + LSDMA_INTMSK);

	for (i = 0; i < 3; i++) {
		writel (LSDMA_CH_CSR_INTDONEENABLE |
			LSDMA_CH_CSR_INTSTOPENABLE,
			card->bridge_addr + LSDMA_CSR(i));
	}
	for (i = 3; i < 4; i++) {
		writel (LSDMA_CH_CSR_INTDONEENABLE |
			LSDMA_CH_CSR_INTSTOPENABLE |
			LSDMA_CH_CSR_DIRECTION,
			card->bridge_addr + LSDMA_CSR(i));
	}
	/* Dummy read to flush PCI posted writes */
	readl (card->bridge_addr + LSDMA_INTMSK);

	/* Register a DVB Master device */
	if ((err = dvbm_register (card)) < 0) {
		goto NO_DEV;
	}

	/* Add device attributes */
	if (card->capabilities & MASTER_CAP_BLACKBURST) {
		if ((err = device_create_file (card->dev,
			&dev_attr_blackburst_type)) < 0) {
			printk (KERN_WARNING
				"%s: unable to create file 'blackburst_type'\n",
				dvbm_driver_name);
		}
	}
	if (card->capabilities & MASTER_CAP_UID) {
		if ((err = device_create_file (card->dev,
			&dev_attr_uid)) < 0) {
			printk(KERN_WARNING
				"%s: Unable to create file 'uid'\n",
				card->name);
		}
	}

	/* Register a transmit interface */
	cap = ASI_CAP_TX_SETCLKSRC | ASI_CAP_TX_FIFOUNDERRUN |
		ASI_CAP_TX_DATA | ASI_CAP_TX_RXCLKSRC |
		ASI_CAP_TX_MAKE204 | ASI_CAP_TX_FINETUNING |
		ASI_CAP_TX_BYTECOUNTER |
		ASI_CAP_TX_LARGEIB |
		ASI_CAP_TX_INTERLEAVING |
		ASI_CAP_TX_TIMESTAMPS |
		ASI_CAP_TX_NULLPACKETS |
		ASI_CAP_TX_PTIMESTAMPS;
	for (i = 0; i < 3; i++) {
		if ((err = asi_register_iface (card,
			&lsdma_dma_ops,
			0,
			MASTER_DIRECTION_TX,
			&dvbm_qio_txfops,
			&dvbm_qio_txops,
			cap,
			4,
			ASI_CTL_TRANSPORT_DVB_ASI)) < 0) {
			goto NO_IFACE;
		}
	}

	/* Register a receive interface */
	cap = ASI_CAP_RX_SYNC | ASI_CAP_RX_INVSYNC | ASI_CAP_RX_CD |
		ASI_CAP_RX_MAKE188 |
		ASI_CAP_RX_BYTECOUNTER |
		ASI_CAP_RX_DATA |
		ASI_CAP_RX_PIDFILTER |
		ASI_CAP_RX_TIMESTAMPS |
		ASI_CAP_RX_PTIMESTAMPS |
		ASI_CAP_RX_NULLPACKETS;
	for (i = 3; i < 4; i++) {
		if ((err = asi_register_iface (card,
			&lsdma_dma_ops,
			0,
			MASTER_DIRECTION_RX,
			&dvbm_qio_rxfops,
			&dvbm_qio_rxops,
			cap,
			4,
			ASI_CTL_TRANSPORT_DVB_ASI)) < 0) {
			goto NO_IFACE;
		}
	}

	return 0;

NO_IFACE:
NO_DEV:
NO_MEM:
	dvbm_q3io_pci_remove (pdev);
NO_PCI:
	return err;
}

/**
 * dvbm_q3io_irq_handler - DVB Master Quad-1in3out interrupt service routine
 * @irq: interrupt number
 * @dev_id: pointer to the device data structure
 * @regs: processor context
 **/
static irqreturn_t
IRQ_HANDLER (dvbm_q3io_irq_handler, irq, dev_id, regs)

{
	struct master_dev *card = dev_id;
	struct list_head *p = &card->iface_list;
	struct master_iface *iface;
	unsigned int dmaintsrc = readl (card->bridge_addr + LSDMA_INTSRC);
	unsigned int status, interrupting_iface = 0;
	int i;

	for (i = 0 ; i < 3; i++) {
		p = p->next;
		iface = list_entry(p, struct master_iface, list);

		if (dmaintsrc & LSDMA_INTSRC_CH(i)) {
			/* Read the interrupt type and clear it */
			spin_lock (&card->irq_lock);
			status = readl (card->bridge_addr + LSDMA_CSR(i));
			writel (status, card->bridge_addr + LSDMA_CSR(i));
			spin_unlock (&card->irq_lock);
			/* Increment the buffer pointer */
			if (status & LSDMA_CH_CSR_INTSRCBUFFER) {
				mdma_advance (iface->dma);
			}

			/* Flag end-of-chain */
			if (status & LSDMA_CH_CSR_INTSRCDONE) {
				set_bit (ASI_EVENT_TX_BUFFER_ORDER,
					&iface->events);
				set_bit (0, &iface->dma_done);
			}
			/* Flag DMA abort */
			if (status & LSDMA_CH_CSR_INTSRCSTOP) {
				set_bit (0, &iface->dma_done);
			}

			interrupting_iface |= 0x1 << i;
		}

		spin_lock (&card->irq_lock);
		status = readl (card->core.addr + DVBM_QIO_ICSR(i));
		writel (status, card->core.addr + DVBM_QIO_ICSR(i));

		if (status & DVBM_QIO_ICSR_TXUIS) {
			set_bit (ASI_EVENT_TX_FIFO_ORDER,
				&iface->events);
			interrupting_iface |= 0x1 << i;
		}
		if (status & DVBM_QIO_ICSR_TXDIS) {
			set_bit (ASI_EVENT_TX_DATA_ORDER,
				&iface->events);
			interrupting_iface |= 0x1 << i;
		}
		spin_unlock (&card->irq_lock);

		if (interrupting_iface & 0x1 << i) {
			wake_up (&iface->queue);
		}
	}

	for (i = 3; i < 4; i++) {
		p = p->next;
		iface = list_entry(p, struct master_iface, list);

		if (dmaintsrc & LSDMA_INTSRC_CH(i)) {
			struct master_dma *dma = iface->dma;

			/* Read the interrupt type and clear it */
			spin_lock (&card->irq_lock);
			status = readl (card->bridge_addr + LSDMA_CSR(i));
			writel (status, card->bridge_addr + LSDMA_CSR(i));
			spin_unlock (&card->irq_lock);

			/* Increment the buffer pointer */
			if (status & LSDMA_CH_CSR_INTSRCBUFFER) {
				mdma_advance (dma);
				if (mdma_rx_isempty (dma)) {
					set_bit (ASI_EVENT_RX_BUFFER_ORDER,
						&iface->events);
				}
			}

			/* Flag end-of-chain */
			if (status & LSDMA_CH_CSR_INTSRCDONE) {
				set_bit (0, &iface->dma_done);
			}

			/* Flag DMA abort */
			if (status & LSDMA_CH_CSR_INTSRCSTOP) {
				set_bit (0, &iface->dma_done);
			}

			interrupting_iface |= 0x2;
		}

		spin_lock (&card->irq_lock);
		status = readl (card->core.addr + DVBM_QIO_ICSR(i));
		writel (status, card->core.addr + DVBM_QIO_ICSR(i));

		if (status & DVBM_QIO_ICSR_RXCDIS) {
			set_bit (ASI_EVENT_RX_CARRIER_ORDER,
				&iface->events);
			interrupting_iface |= 0x2;
		}
		if (status & DVBM_QIO_ICSR_RXAOSIS) {
			set_bit (ASI_EVENT_RX_AOS_ORDER,
				&iface->events);
			interrupting_iface |= 0x2;
		}
		if (status & DVBM_QIO_ICSR_RXLOSIS) {
			set_bit (ASI_EVENT_RX_LOS_ORDER,
				&iface->events);
			interrupting_iface |= 0x2;
		}
		if (status & DVBM_QIO_ICSR_RXOIS) {
			set_bit (ASI_EVENT_RX_FIFO_ORDER,
				&iface->events);
			interrupting_iface |= 0x2;
		}
		if (status & DVBM_QIO_ICSR_RXDIS) {
			set_bit (ASI_EVENT_RX_DATA_ORDER,
				&iface->events);
			interrupting_iface |= 0x2;
		}

		if (interrupting_iface & 0x2) {
			wake_up (&iface->queue);
		}
		spin_unlock (&card->irq_lock);
	}

	/* Check and clear the source of the interrupt */
	if (interrupting_iface) {
		/* Dummy read to flush PCI posted writes */
		readl (card->bridge_addr + LSDMA_INTMSK);

		return IRQ_HANDLED;
	}
	return IRQ_NONE;
}

