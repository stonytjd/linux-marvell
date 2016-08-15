/*
 * Driver for the Aardvark PCIe controller, used on Marvell Armada
 * 3700.
 *
 * Copyright (C) 2016 Marvell
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/phy/phy.h>
#include <linux/of_gpio.h>

/* PCIe core registers */
#define PCIE_CORE_CMD_STATUS_REG				0x4
#define     PCIE_CORE_CMD_IO_ACCESS_EN				BIT(0)
#define     PCIE_CORE_CMD_MEM_ACCESS_EN				BIT(1)
#define     PCIE_CORE_CMD_MEM_IO_REQ_EN				BIT(2)
#define PCIE_CORE_DEV_CTRL_STATS_REG				0xc8
#define     PCIE_CORE_DEV_CTRL_STATS_RELAX_ORDER_DISABLE	(0 << 4)
#define     PCIE_CORE_DEV_CTRL_STATS_MAX_PAYLOAD_SZ_SHIFT	5
#define     PCIE_CORE_DEV_CTRL_STATS_MAX_PAYLOAD_SZ		0x2
#define     PCIE_CORE_DEV_CTRL_STATS_SNOOP_DISABLE		(0 << 11)
#define     PCIE_CORE_DEV_CTRL_STATS_MAX_RD_REQ_SIZE_SHIFT	12
#define     PCIE_CORE_DEV_CTRL_STATS_MAX_RD_REQ_SZ		0x2
#define     PCIE_CORE_MPS_UNIT_BYTE				128
#define PCIE_CORE_LINK_CTRL_STAT_REG				0xd0
#define     PCIE_CORE_LINK_L0S_ENTRY				BIT(0)
#define     PCIE_CORE_LINK_TRAINING				BIT(5)
#define     PCIE_CORE_LINK_WIDTH_SHIFT				20
#define PCIE_CORE_ERR_CAPCTL_REG				0x118
#define     PCIE_CORE_ERR_CAPCTL_ECRC_CHK_TX			BIT(5)
#define     PCIE_CORE_ERR_CAPCTL_ECRC_CHK_TX_EN			BIT(6)
#define     PCIE_CORE_ERR_CAPCTL_ECRC_CHCK			BIT(7)
#define     PCIE_CORE_ERR_CAPCTL_ECRC_CHCK_RCV			BIT(8)

/* PIO registers base address and register offsets */
#define PIO_BASE_ADDR				0x4000
#define PIO_CTRL				(PIO_BASE_ADDR + 0x0)
#define   PIO_CTRL_TYPE_MASK			GENMASK(3, 0)
#define   PIO_CTRL_ADDR_WIN_DISABLE		BIT(24)
#define PIO_STAT				(PIO_BASE_ADDR + 0x4)
#define   PIO_COMPLETION_STATUS_SHIFT		7
#define   PIO_COMPLETION_STATUS_MASK		GENMASK(9, 7)
#define   PIO_COMPLETION_STATUS_OK		0
#define   PIO_COMPLETION_STATUS_UR		1
#define   PIO_COMPLETION_STATUS_CRS		2
#define   PIO_COMPLETION_STATUS_CA		4
#define   PIO_NON_POSTED_REQ			BIT(0)
#define PIO_ADDR_LS				(PIO_BASE_ADDR + 0x8)
#define PIO_ADDR_MS				(PIO_BASE_ADDR + 0xc)
#define PIO_WR_DATA				(PIO_BASE_ADDR + 0x10)
#define PIO_WR_DATA_STRB			(PIO_BASE_ADDR + 0x14)
#define PIO_RD_DATA				(PIO_BASE_ADDR + 0x18)
#define PIO_START				(PIO_BASE_ADDR + 0x1c)
#define PIO_ISR					(PIO_BASE_ADDR + 0x20)
#define PIO_ISRM				(PIO_BASE_ADDR + 0x24)

/* Aardvark Control registers */
#define CONTROL_BASE_ADDR			0x4800
#define PCIE_CORE_CTRL0_REG			(CONTROL_BASE_ADDR + 0x0)
#define     PCIE_GEN_SEL_MSK			0x3
#define     PCIE_GEN_SEL_SHIFT			0x0
#define     SPEED_GEN_1				0
#define     SPEED_GEN_2				1
#define     SPEED_GEN_3				2
#define     IS_RC_MSK				1
#define     IS_RC_SHIFT				2
#define     LANE_CNT_MSK			0x18
#define     LANE_CNT_SHIFT			0x3
#define     LANE_COUNT_1			(0 << LANE_CNT_SHIFT)
#define     LANE_COUNT_2			(1 << LANE_CNT_SHIFT)
#define     LANE_COUNT_4			(2 << LANE_CNT_SHIFT)
#define     LANE_COUNT_8			(3 << LANE_CNT_SHIFT)
#define     LINK_TRAINING_EN			BIT(6)
#define     LEGACY_INTA				BIT(28)
#define     LEGACY_INTB				BIT(29)
#define     LEGACY_INTC				BIT(30)
#define     LEGACY_INTD				BIT(31)
#define PCIE_CORE_CTRL1_REG			(CONTROL_BASE_ADDR + 0x4)
#define     HOT_RESET_GEN			BIT(0)
#define PCIE_CORE_CTRL2_REG			(CONTROL_BASE_ADDR + 0x8)
#define     PCIE_CORE_CTRL2_RESERVED		0x7
#define     PCIE_CORE_CTRL2_TD_ENABLE		BIT(4)
#define     PCIE_CORE_CTRL2_STRICT_ORDER_ENABLE	BIT(5)
#define     PCIE_CORE_CTRL2_OB_WIN_ENABLE	BIT(6)
#define     PCIE_CORE_CTRL2_MSI_ENABLE		BIT(10)
#define PCIE_PHY_REF_CLOCK			(CONTROL_BASE_ADDR + 0x14)
#define     PCIE_PHY_CTRL_OFF			16
#define     PCIE_PHY_BUF_CTRL_OFF		0
#define     PCIE_PHY_BUF_CTRL_INIT_VAL		0x1342
#define PCIE_ISR0_REG				(CONTROL_BASE_ADDR + 0x40)
#define PCIE_ISR0_MASK_REG			(CONTROL_BASE_ADDR + 0x44)
#define     PCIE_ISR0_MSI_INT_PENDING		BIT(24)
#define     PCIE_ISR0_INTX_ASSERT(val)		BIT(16 + (val))
#define     PCIE_ISR0_INTX_DEASSERT(val)	BIT(20 + (val))
#define	    PCIE_ISR0_ALL_MASK			GENMASK(26, 0)
#define PCIE_ISR1_REG				(CONTROL_BASE_ADDR + 0x48)
#define PCIE_ISR1_MASK_REG			(CONTROL_BASE_ADDR + 0x4C)
#define     PCIE_ISR1_POWER_STATE_CHANGE	BIT(4)
#define     PCIE_ISR1_FLUSH			BIT(5)
#define     PCIE_ISR1_INTX_ASSERT(val)		BIT(8 + (val))
#define     PCIE_ISR1_ALL_MASK			GENMASK(11, 4)
#define PCIE_MSI_ADDR_LOW_REG			(CONTROL_BASE_ADDR + 0x50)
#define PCIE_MSI_ADDR_HIGH_REG			(CONTROL_BASE_ADDR + 0x54)
#define PCIE_MSI_STATUS_REG			(CONTROL_BASE_ADDR + 0x58)
#define PCIE_MSI_MASK_REG			(CONTROL_BASE_ADDR + 0x5C)
#define PCIE_MSI_PAYLOAD_REG			(CONTROL_BASE_ADDR + 0x9C)

/* LMI registers base address and register offsets */
#define LMI_BASE_ADDR				0x6000
#define CFG_REG					(LMI_BASE_ADDR + 0x0)
#define     LTSSM_SHIFT				24
#define     LTSSM_MASK				0x3f
#define     LTSSM_L0				0x10
#define     RC_BAR_CONFIG			0x300

/* PCIe core controller registers */
#define CTRL_CORE_BASE_ADDR			0x18000
#define CTRL_CONFIG_REG				(CTRL_CORE_BASE_ADDR + 0x0)
#define     CTRL_MODE_SHIFT			0x0
#define     CTRL_MODE_MASK			0x1
#define     PCIE_CORE_MODE_DIRECT		0x0
#define     PCIE_CORE_MODE_COMMAND		0x1

/* PCIe Central Interrupts Registers */
#define CENTRAL_INT_BASE_ADDR			0x1b000
#define HOST_CTRL_INT_STATUS_REG		(CENTRAL_INT_BASE_ADDR + 0x0)
#define HOST_CTRL_INT_MASK_REG			(CENTRAL_INT_BASE_ADDR + 0x4)
#define     PCIE_IRQ_MSI_STATUS_INT		BIT(1)
#define     PCIE_IRQ_DMA_INT			BIT(4)
#define     PCIE_IRQ_OB_DXFERDONE		BIT(6)
#define     PCIE_IRQ_OB_RXFERDONE		BIT(7)
#define     PCIE_IRQ_CORE_INT			BIT(16)
#define     PCIE_IRQ_CORE_INT_PIO		BIT(17)
#define     PCIE_IRQ_DPMU_INT			BIT(18)
#define     PCIE_IRQ_PCIE_MIS_INT		BIT(19)
#define     PCIE_IRQ_ALL_MASK			0xfff0fb
#define     PCIE_IRQ_ENABLE_INTS_MASK		PCIE_IRQ_CORE_INT

/* Transaction types */
#define PCIE_CONFIG_RD_TYPE0			0x8
#define PCIE_CONFIG_RD_TYPE1			0x9
#define PCIE_CONFIG_WR_TYPE0			0xa
#define PCIE_CONFIG_WR_TYPE1			0xb

/* PCI_BDF shifts 8bit, so we need extra 4bit shift */
#define PCIE_BDF(dev)				(dev << 4)
#define PCIE_CONF_BUS(bus)			(((bus) & 0xff) << 20)
#define PCIE_CONF_DEV(dev)			(((dev) & 0x1f) << 15)
#define PCIE_CONF_FUNC(fun)			(((fun) & 0x7)	<< 12)
#define PCIE_CONF_REG(reg)			((reg) & 0xffc)
#define PCIE_CONF_ADDR(bus, devfn, where)	\
	(PCIE_CONF_BUS(bus) | PCIE_CONF_DEV(PCI_SLOT(devfn))	| \
	 PCIE_CONF_FUNC(PCI_FUNC(devfn)) | PCIE_CONF_REG(where))

#define PIO_TIMEOUT_MS			1

#define LINK_WAIT_MAX_RETRIES		10
#define LINK_WAIT_USLEEP_MIN		90000
#define LINK_WAIT_USLEEP_MAX		100000

#define LEGACY_IRQ_NUM			4
#define MSI_IRQ_NUM			32

struct advk_pcie {
	struct platform_device *pdev;
	struct pci_bus *bus;
	void __iomem *base;
	struct phy *phy;
	struct list_head resources;
	struct irq_domain *irq_domain;
	struct irq_chip irq_chip;
	struct msi_controller msi;
	struct irq_domain *msi_domain;
	struct irq_chip msi_irq_chip;
	DECLARE_BITMAP(msi_irq_in_use, MSI_IRQ_NUM);
	struct mutex msi_used_lock;
	u16 msi_msg;
	int root_bus_nr;
	char *reset_name;
	struct gpio_desc *reset_gpio;
	enum of_gpio_flags flags;
	struct clk *clk;
};

static inline void advk_writel(struct advk_pcie *pcie, u32 val, u64 reg)
{
	writel(val, pcie->base + reg);
}

static inline u32 advk_readl(struct advk_pcie *pcie, u64 reg)
{
	return readl(pcie->base + reg);
}

static int advk_pcie_link_up(struct advk_pcie *pcie)
{
	u32 val, ltssm_state;

	val = advk_readl(pcie, CFG_REG);
	ltssm_state = (val >> LTSSM_SHIFT) & LTSSM_MASK;
	return ltssm_state >= LTSSM_L0;
}

static int advk_pcie_wait_for_link(struct advk_pcie *pcie)
{
	int retries;

	/* check if the link is up or not */
	for (retries = 0; retries < LINK_WAIT_MAX_RETRIES; retries++) {
		if (advk_pcie_link_up(pcie)) {
			dev_info(&pcie->pdev->dev, "link up\n");
			return 0;
		}

		usleep_range(LINK_WAIT_USLEEP_MIN, LINK_WAIT_USLEEP_MAX);
	}

	dev_err(&pcie->pdev->dev, "link never came up\n");

	return -ETIMEDOUT;
}

static void advk_pcie_setup_hw(struct advk_pcie *pcie)
{
	u32 reg;
	phys_addr_t msi_msg_phys;

	/* Set HW Reference Clock Buffer Control */
	advk_writel(pcie, PCIE_PHY_BUF_CTRL_INIT_VAL, PCIE_PHY_REF_CLOCK);

	/* Set to Direct mode */
	reg = advk_readl(pcie, CTRL_CONFIG_REG);
	reg &= ~(CTRL_MODE_MASK << CTRL_MODE_SHIFT);
	reg |= ((PCIE_CORE_MODE_DIRECT & CTRL_MODE_MASK) << CTRL_MODE_SHIFT);
	advk_writel(pcie, reg, CTRL_CONFIG_REG);

	/* Set PCI global control register to RC mode */
	reg = advk_readl(pcie, PCIE_CORE_CTRL0_REG);
	reg |= (IS_RC_MSK << IS_RC_SHIFT);
	advk_writel(pcie, reg, PCIE_CORE_CTRL0_REG);

	/* Set Advanced Error Capabilities and Control PF0 register */
	reg = PCIE_CORE_ERR_CAPCTL_ECRC_CHK_TX |
		PCIE_CORE_ERR_CAPCTL_ECRC_CHK_TX_EN |
		PCIE_CORE_ERR_CAPCTL_ECRC_CHCK |
		PCIE_CORE_ERR_CAPCTL_ECRC_CHCK_RCV;
	advk_writel(pcie, reg, PCIE_CORE_ERR_CAPCTL_REG);

	/* Set PCIe Device Control and Status 1 PF0 register */
	reg = PCIE_CORE_DEV_CTRL_STATS_RELAX_ORDER_DISABLE |
		(PCIE_CORE_DEV_CTRL_STATS_MAX_PAYLOAD_SZ << PCIE_CORE_DEV_CTRL_STATS_MAX_PAYLOAD_SZ_SHIFT) |
		PCIE_CORE_DEV_CTRL_STATS_SNOOP_DISABLE |
		(PCIE_CORE_DEV_CTRL_STATS_MAX_RD_REQ_SZ << PCIE_CORE_DEV_CTRL_STATS_MAX_RD_REQ_SIZE_SHIFT);
	advk_writel(pcie, reg, PCIE_CORE_DEV_CTRL_STATS_REG);

	/* Program PCIe Control 2 to disable strict ordering */
	reg = PCIE_CORE_CTRL2_RESERVED |
		PCIE_CORE_CTRL2_TD_ENABLE;
	advk_writel(pcie, reg, PCIE_CORE_CTRL2_REG);

	/* Set GEN2 */
	reg = advk_readl(pcie, PCIE_CORE_CTRL0_REG);
	reg &= ~PCIE_GEN_SEL_MSK;
	reg |= SPEED_GEN_2;
	advk_writel(pcie, reg, PCIE_CORE_CTRL0_REG);

	/* Set lane X1 */
	reg = advk_readl(pcie, PCIE_CORE_CTRL0_REG);
	reg &= ~LANE_CNT_MSK;
	reg |= LANE_COUNT_1;
	advk_writel(pcie, reg, PCIE_CORE_CTRL0_REG);

	/* Enable link training */
	reg = advk_readl(pcie, PCIE_CORE_CTRL0_REG);
	reg |= LINK_TRAINING_EN;
	advk_writel(pcie, reg, PCIE_CORE_CTRL0_REG);

	/* Set MSI Address in RC mode */
	msi_msg_phys = virt_to_phys(&pcie->msi_msg);
	advk_writel(pcie, lower_32_bits(msi_msg_phys),
		    PCIE_MSI_ADDR_LOW_REG);
	advk_writel(pcie, upper_32_bits(msi_msg_phys),
		    PCIE_MSI_ADDR_HIGH_REG);

	/* Enable MSI */
	reg = advk_readl(pcie, PCIE_CORE_CTRL2_REG);
	reg |= PCIE_CORE_CTRL2_MSI_ENABLE;
	advk_writel(pcie, reg, PCIE_CORE_CTRL2_REG);

	/* Clear all interrupts */
	advk_writel(pcie, PCIE_ISR0_ALL_MASK, PCIE_ISR0_REG);
	advk_writel(pcie, PCIE_ISR1_ALL_MASK, PCIE_ISR1_REG);
	advk_writel(pcie, PCIE_IRQ_ALL_MASK, HOST_CTRL_INT_STATUS_REG);

	/* Disable All ISR0/1 Sources */
	reg = PCIE_ISR0_ALL_MASK;
	reg &= ~PCIE_ISR0_MSI_INT_PENDING;
	advk_writel(pcie, reg, PCIE_ISR0_MASK_REG);

	advk_writel(pcie, PCIE_ISR1_ALL_MASK, PCIE_ISR1_MASK_REG);

	/* Unmask all MSI's */
	advk_writel(pcie, 0, PCIE_MSI_MASK_REG);

	/* Enable summary interrupt for GIC SPI source */
	reg = PCIE_IRQ_ALL_MASK & (~PCIE_IRQ_ENABLE_INTS_MASK);
	advk_writel(pcie, reg, HOST_CTRL_INT_MASK_REG);

	reg = advk_readl(pcie, PCIE_CORE_CTRL2_REG);
	reg |= PCIE_CORE_CTRL2_OB_WIN_ENABLE;
	advk_writel(pcie, reg, PCIE_CORE_CTRL2_REG);

	/* Bypass the address window mapping for PIO */
	reg = advk_readl(pcie, PIO_CTRL);
	reg |= PIO_CTRL_ADDR_WIN_DISABLE;
	advk_writel(pcie, reg, PIO_CTRL);

	/* Start link training */
	reg = advk_readl(pcie, PCIE_CORE_LINK_CTRL_STAT_REG);
	reg |= PCIE_CORE_LINK_TRAINING;
	advk_writel(pcie, reg, PCIE_CORE_LINK_CTRL_STAT_REG);

	advk_pcie_wait_for_link(pcie);

	reg = (1 << PCIE_CORE_LINK_WIDTH_SHIFT);
	advk_writel(pcie, reg, PCIE_CORE_LINK_CTRL_STAT_REG);

	reg = advk_readl(pcie, PCIE_CORE_CMD_STATUS_REG);
	reg |= PCIE_CORE_CMD_MEM_ACCESS_EN |
		PCIE_CORE_CMD_IO_ACCESS_EN |
		PCIE_CORE_CMD_MEM_IO_REQ_EN;
	advk_writel(pcie, reg, PCIE_CORE_CMD_STATUS_REG);
}

static void advk_pcie_check_pio_status(struct advk_pcie *pcie)
{
	u32 reg;
	unsigned int status;
	char *strcomp_status, *str_posted;

	reg = advk_readl(pcie, PIO_STAT);
	status = (reg & PIO_COMPLETION_STATUS_MASK) >>
		PIO_COMPLETION_STATUS_SHIFT;

	if (!status)
		return;

	switch (status) {
	case PIO_COMPLETION_STATUS_UR:
		strcomp_status = "UR";
		break;
	case PIO_COMPLETION_STATUS_CRS:
		strcomp_status = "CRS";
		break;
	case PIO_COMPLETION_STATUS_CA:
		strcomp_status = "CA";
		break;
	default:
		strcomp_status = "Unknown";
		break;
	}

	if (reg & PIO_NON_POSTED_REQ)
		str_posted = "Non-posted";
	else
		str_posted = "Posted";

	dev_err(&pcie->pdev->dev, "%s PIO Response Status: %s, %#x @ %#x\n",
		str_posted, strcomp_status, reg, advk_readl(pcie, PIO_ADDR_LS));
}

static int advk_pcie_wait_pio(struct advk_pcie *pcie)
{
	unsigned long timeout;

	timeout = jiffies + msecs_to_jiffies(PIO_TIMEOUT_MS);

	while (time_before(jiffies, timeout)) {
		u32 start, isr;

		start = advk_readl(pcie, PIO_START);
		isr = advk_readl(pcie, PIO_ISR);
		if (!start && isr)
			return 0;
	}

	dev_err(&pcie->pdev->dev, "config read/write timed out\n");
	return -ETIMEDOUT;
}

static int advk_pcie_rd_conf(struct pci_bus *bus, u32 devfn,
			     int where, int size, u32 *val)
{
	struct advk_pcie *pcie = bus->sysdata;
	u32 reg;
	int ret;

	if ((bus->number == pcie->root_bus_nr) && (PCI_SLOT(devfn) != 0)) {
		*val = 0xffffffff;
		return PCIBIOS_DEVICE_NOT_FOUND;
	}

	/* Start PIO */
	advk_writel(pcie, 0, PIO_START);
	advk_writel(pcie, 1, PIO_ISR);

	/* Program the control register */
	reg = advk_readl(pcie, PIO_CTRL);
	reg &= ~PIO_CTRL_TYPE_MASK;
	if (bus->number ==  pcie->root_bus_nr)
		reg |= PCIE_CONFIG_RD_TYPE0;
	else
		reg |= PCIE_CONFIG_RD_TYPE1;
	advk_writel(pcie, reg, PIO_CTRL);

	/* Program the address registers */
	reg = PCIE_CONF_ADDR(bus->number, devfn, where);
	advk_writel(pcie, reg, PIO_ADDR_LS);
	advk_writel(pcie, 0, PIO_ADDR_MS);

	/* Program the data strobe */
	advk_writel(pcie, 0xf, PIO_WR_DATA_STRB);

	/* Start the transfer */
	advk_writel(pcie, 1, PIO_START);

	ret = advk_pcie_wait_pio(pcie);
	if (ret < 0)
		return PCIBIOS_SET_FAILED;

	advk_pcie_check_pio_status(pcie);

	/* Get the read result */
	*val = advk_readl(pcie, PIO_RD_DATA);
	if (size == 1)
		*val = (*val >> (8 * (where & 3))) & 0xff;
	else if (size == 2)
		*val = (*val >> (8 * (where & 3))) & 0xffff;

	return PCIBIOS_SUCCESSFUL;
}

static int advk_pcie_wr_conf(struct pci_bus *bus, u32 devfn,
				int where, int size, u32 val)
{
	struct advk_pcie *pcie = bus->sysdata;
	u32 reg;
	u32 data_strobe = 0x0;
	int offset;
	int ret;

	if ((bus->number == pcie->root_bus_nr) && (PCI_SLOT(devfn) != 0))
		return PCIBIOS_DEVICE_NOT_FOUND;

	if (where % size)
		return PCIBIOS_SET_FAILED;

	/* Start PIO */
	advk_writel(pcie, 0, PIO_START);
	advk_writel(pcie, 1, PIO_ISR);

	/* Program the control register */
	reg = advk_readl(pcie, PIO_CTRL);
	reg &= ~PIO_CTRL_TYPE_MASK;
	if (bus->number == pcie->root_bus_nr)
		reg |= PCIE_CONFIG_WR_TYPE0;
	else
		reg |= PCIE_CONFIG_WR_TYPE1;
	advk_writel(pcie, reg, PIO_CTRL);

	/* Program the address registers */
	reg = PCIE_CONF_ADDR(bus->number, devfn, where);
	advk_writel(pcie, reg, PIO_ADDR_LS);
	advk_writel(pcie, 0, PIO_ADDR_MS);

	/* Calculate the write strobe */
	offset      = where & 0x3;
	reg         = val << (8 * offset);
	data_strobe = GENMASK(size - 1, 0) << offset;

	/* Program the data register */
	advk_writel(pcie, reg, PIO_WR_DATA);

	/* Program the data strobe */
	advk_writel(pcie, data_strobe, PIO_WR_DATA_STRB);

	/* Start the transfer */
	advk_writel(pcie, 1, PIO_START);

	ret = advk_pcie_wait_pio(pcie);
	if (ret < 0)
		return PCIBIOS_SET_FAILED;

	advk_pcie_check_pio_status(pcie);

	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops advk_pcie_ops = {
	.read = advk_pcie_rd_conf,
	.write = advk_pcie_wr_conf,
};

static int advk_pcie_alloc_msi(struct advk_pcie *pcie)
{
	int hwirq;

	mutex_lock(&pcie->msi_used_lock);
	hwirq = find_first_zero_bit(pcie->msi_irq_in_use, MSI_IRQ_NUM);
	if (hwirq >= MSI_IRQ_NUM)
		hwirq = -ENOSPC;
	else
		set_bit(hwirq, pcie->msi_irq_in_use);
	mutex_unlock(&pcie->msi_used_lock);

	return hwirq;
}

static void advk_pcie_free_msi(struct advk_pcie *pcie, int hwirq)
{
	mutex_lock(&pcie->msi_used_lock);
	if (!test_bit(hwirq, pcie->msi_irq_in_use))
		dev_err(&pcie->pdev->dev, "trying to free unused MSI#%d\n",
			hwirq);
	else
		clear_bit(hwirq, pcie->msi_irq_in_use);
	mutex_unlock(&pcie->msi_used_lock);
}

static int advk_pcie_setup_msi_irq(struct msi_controller *chip,
				   struct pci_dev *pdev,
				   struct msi_desc *desc)
{
	struct advk_pcie *pcie = pdev->bus->sysdata;
	struct msi_msg msg;
	int virq, hwirq;
	phys_addr_t msi_msg_phys;

	/* We support MSI, but not MSI-X */
	if (desc->msi_attrib.is_msix)
		return -EINVAL;

	hwirq = advk_pcie_alloc_msi(pcie);
	if (hwirq < 0)
		return hwirq;

	virq = irq_create_mapping(pcie->msi_domain, hwirq);
	if (!virq) {
		advk_pcie_free_msi(pcie, hwirq);
		return -EINVAL;
	}

	irq_set_msi_desc(virq, desc);

	msi_msg_phys = virt_to_phys(&pcie->msi_msg);

	msg.address_lo = lower_32_bits(msi_msg_phys);
	msg.address_hi = upper_32_bits(msi_msg_phys);
	msg.data = virq;

	pci_write_msi_msg(virq, &msg);

	return 0;
}

static void advk_pcie_teardown_msi_irq(struct msi_controller *chip,
				       unsigned int irq)
{
	struct irq_data *d = irq_get_irq_data(irq);
	struct msi_desc *msi = irq_data_get_msi_desc(d);
	struct advk_pcie *pcie = msi_desc_to_pci_sysdata(msi);
	unsigned long hwirq = d->hwirq;

	irq_dispose_mapping(irq);
	advk_pcie_free_msi(pcie, hwirq);
}

static int advk_pcie_msi_map(struct irq_domain *domain,
			     unsigned int virq, irq_hw_number_t hw)
{
	struct advk_pcie *pcie = domain->host_data;

	irq_set_chip_and_handler(virq, &pcie->msi_irq_chip,
				 handle_simple_irq);

	return 0;
}

static const struct irq_domain_ops advk_pcie_msi_irq_ops = {
	.map = advk_pcie_msi_map,
};

static void advk_pcie_irq_mask(struct irq_data *d)
{
	struct advk_pcie *pcie = d->domain->host_data;
	irq_hw_number_t hwirq = irqd_to_hwirq(d);
	u32 mask;

	mask = advk_readl(pcie, PCIE_ISR1_MASK_REG);
	mask |= PCIE_ISR1_INTX_ASSERT(hwirq);
	advk_writel(pcie, mask, PCIE_ISR1_MASK_REG);
}

static void advk_pcie_irq_unmask(struct irq_data *d)
{
	struct advk_pcie *pcie = d->domain->host_data;
	irq_hw_number_t hwirq = irqd_to_hwirq(d);
	u32 mask;

	mask = advk_readl(pcie, PCIE_ISR1_MASK_REG);
	mask &= ~PCIE_ISR1_INTX_ASSERT(hwirq);
	advk_writel(pcie, mask, PCIE_ISR1_MASK_REG);
}

static int advk_pcie_irq_map(struct irq_domain *h,
			     unsigned int virq, irq_hw_number_t hwirq)
{
	struct advk_pcie *pcie = h->host_data;

	advk_pcie_irq_mask(irq_get_irq_data(virq));
	irq_set_status_flags(virq, IRQ_LEVEL);
	irq_set_chip_and_handler(virq, &pcie->irq_chip,
				 handle_level_irq);
	irq_set_chip_data(virq, pcie);

	return 0;
}

static const struct irq_domain_ops advk_pcie_irq_domain_ops = {
	.map = advk_pcie_irq_map,
	.xlate = irq_domain_xlate_onecell,
};

static int advk_pcie_init_msi_irq_domain(struct advk_pcie *pcie)
{
	struct device *dev = &pcie->pdev->dev;
	struct device_node *node = dev->of_node;
	struct irq_chip *msi_irq_chip;
	struct msi_controller *msi;
	int ret;

	msi_irq_chip = &pcie->msi_irq_chip;

	msi_irq_chip->name = devm_kasprintf(dev, GFP_KERNEL, "%s-msi",
					    dev_name(dev));
	if (!msi_irq_chip->name)
		return -ENOMEM;

	msi_irq_chip->irq_enable = pci_msi_unmask_irq;
	msi_irq_chip->irq_disable = pci_msi_mask_irq;
	msi_irq_chip->irq_mask = pci_msi_mask_irq;
	msi_irq_chip->irq_unmask = pci_msi_unmask_irq;

	msi = &pcie->msi;

	msi->setup_irq = advk_pcie_setup_msi_irq;
	msi->teardown_irq = advk_pcie_teardown_msi_irq;
	msi->of_node = node;

	mutex_init(&pcie->msi_used_lock);

	pcie->msi_domain =
		irq_domain_add_linear(NULL, MSI_IRQ_NUM,
				      &advk_pcie_msi_irq_ops, pcie);
	if (!pcie->msi_domain)
		return -ENOMEM;

	ret = of_pci_msi_chip_add(msi);
	if (ret < 0) {
		irq_domain_remove(pcie->msi_domain);
		return ret;
	}

	return 0;
}

static void advk_pcie_remove_msi_irq_domain(struct advk_pcie *pcie)
{
	of_pci_msi_chip_remove(&pcie->msi);
	irq_domain_remove(pcie->msi_domain);
}

static int advk_pcie_init_irq_domain(struct advk_pcie *pcie)
{
	struct device *dev = &pcie->pdev->dev;
	struct device_node *node = dev->of_node;
	struct device_node *pcie_intc_node;
	struct irq_chip *irq_chip;

	pcie_intc_node =  of_get_next_child(node, NULL);
	if (!pcie_intc_node) {
		dev_err(dev, "No PCIe Intc node found\n");
		return -ENODEV;
	}

	irq_chip = &pcie->irq_chip;

	irq_chip->name = devm_kasprintf(dev, GFP_KERNEL, "%s-irq",
					dev_name(dev));
	if (!irq_chip->name) {
		of_node_put(pcie_intc_node);
		return -ENOMEM;
	}

	irq_chip->irq_mask = advk_pcie_irq_mask;
	irq_chip->irq_mask_ack = advk_pcie_irq_mask;
	irq_chip->irq_unmask = advk_pcie_irq_unmask;

	pcie->irq_domain =
		irq_domain_add_linear(pcie_intc_node, LEGACY_IRQ_NUM,
				      &advk_pcie_irq_domain_ops, pcie);
	if (!pcie->irq_domain) {
		dev_err(dev, "Failed to get a INTx IRQ domain\n");
		of_node_put(pcie_intc_node);
		return -ENOMEM;
	}

	return 0;
}

static void advk_pcie_remove_irq_domain(struct advk_pcie *pcie)
{
	irq_domain_remove(pcie->irq_domain);
}

static void advk_pcie_handle_msi(struct advk_pcie *pcie)
{
	u32 msi_val, msi_mask, msi_status, msi_idx;
	u16 msi_data;

	msi_mask = advk_readl(pcie, PCIE_MSI_MASK_REG);
	msi_val = advk_readl(pcie, PCIE_MSI_STATUS_REG);
	msi_status = msi_val & ~msi_mask;

	for (msi_idx = 0; msi_idx < MSI_IRQ_NUM; msi_idx++) {
		if (!(BIT(msi_idx) & msi_status))
			continue;

		advk_writel(pcie, BIT(msi_idx), PCIE_MSI_STATUS_REG);
		msi_data = advk_readl(pcie, PCIE_MSI_PAYLOAD_REG) & 0xFF;
		generic_handle_irq(msi_data);
	}

	advk_writel(pcie, PCIE_ISR0_MSI_INT_PENDING,
		    PCIE_ISR0_REG);
}

static void advk_pcie_handle_int(struct advk_pcie *pcie)
{
	u32 val, mask, status;
	u32 val2, mask2, status2;
	int i, virq;

	val = advk_readl(pcie, PCIE_ISR0_REG);
	mask = advk_readl(pcie, PCIE_ISR0_MASK_REG);
	status = val & ((~mask) & PCIE_ISR0_ALL_MASK);

	val2 = advk_readl(pcie, PCIE_ISR1_REG);
	mask2 = advk_readl(pcie, PCIE_ISR1_MASK_REG);
	status2 = val2 & ((~mask2) & PCIE_ISR1_ALL_MASK);

	if (!status && !status2) {
		advk_writel(pcie, val, PCIE_ISR0_REG);
		advk_writel(pcie, val2, PCIE_ISR1_REG);
		return;
	}

	/* Process MSI interrupts */
	if (status & PCIE_ISR0_MSI_INT_PENDING)
		advk_pcie_handle_msi(pcie);

	/* Process legacy interrupts */
	for (i = 0; i < LEGACY_IRQ_NUM; i++) {
		if (!(status2 & PCIE_ISR1_INTX_ASSERT(i)))
			continue;

		advk_writel(pcie, PCIE_ISR1_INTX_ASSERT(i),
			    PCIE_ISR1_REG);

		virq = irq_find_mapping(pcie->irq_domain, i);
		generic_handle_irq(virq);
	}
}

static irqreturn_t advk_pcie_irq_handler(int irq, void *arg)
{
	struct advk_pcie *pcie = arg;
	u32 status;

	status = advk_readl(pcie, HOST_CTRL_INT_STATUS_REG);
	if (!(status & PCIE_IRQ_CORE_INT))
		return IRQ_NONE;

	advk_pcie_handle_int(pcie);

	/* Clear interrupt */
	advk_writel(pcie, PCIE_IRQ_CORE_INT, HOST_CTRL_INT_STATUS_REG);

	return IRQ_HANDLED;
}

static int advk_pcie_parse_request_of_pci_ranges(struct advk_pcie *pcie)
{
	int err, res_valid = 0;
	struct device *dev = &pcie->pdev->dev;
	struct device_node *np = dev->of_node;
	struct resource_entry *win, *tmp;
	resource_size_t iobase;

	INIT_LIST_HEAD(&pcie->resources);

	err = of_pci_get_host_bridge_resources(np, 0, 0xff, &pcie->resources,
					       &iobase);
	if (err)
		return err;

	resource_list_for_each_entry_safe(win, tmp, &pcie->resources) {
		struct resource *parent = NULL;
		struct resource *res = win->res;

		switch (resource_type(res)) {
		case IORESOURCE_IO:
			parent = &ioport_resource;
			err = pci_remap_iospace(res, iobase);
			if (err) {
				dev_warn(dev, "error %d: failed to map resource %pR\n",
					 err, res);
				resource_list_destroy_entry(win);
				continue;
			}
			break;
		case IORESOURCE_MEM:
			parent = &iomem_resource;
			res_valid |= !(res->flags & IORESOURCE_PREFETCH);
			break;
		case IORESOURCE_BUS:
			pcie->root_bus_nr = res->start;
			break;
		default:
			continue;
		}

		if (parent) {
			err = devm_request_resource(dev, parent, res);
			if (err)
				goto out_release_res;
		}
	}

	if (!res_valid) {
		dev_err(dev, "non-prefetchable memory resource required\n");
		err = -EINVAL;
		goto out_release_res;
	}

	return 0;

out_release_res:
	pci_free_resource_list(&pcie->resources);
	return err;
}

static int advk_pcie_find_smpss(struct pci_dev *dev, void *data)
{
	u8 *smpss = data;

	if (!dev)
		return 0;

	if (!pci_is_pcie(dev))
		return 0;

	if (*smpss > dev->pcie_mpss)
		*smpss = dev->pcie_mpss;

	return 0;
}

static int advk_pcie_bus_configure_mps(struct pci_dev *dev, void *data)
{
	int mps;

	if (!dev)
		return 0;

	if (!pci_is_pcie(dev))
		return 0;

	mps = PCIE_CORE_MPS_UNIT_BYTE << *(u8 *)data;
	pcie_set_mps(dev, mps);

	return 0;
}

static void advk_pcie_configure_mps(struct advk_pcie *pcie)
{
	u8 smpss = PCIE_CORE_DEV_CTRL_STATS_MAX_PAYLOAD_SZ;
	struct pci_bus *bus = pcie->bus;
	u32 reg;

	/* Find the minimal supported MAX payload size */
	advk_pcie_find_smpss(bus->self, &smpss);
	pci_walk_bus(bus, advk_pcie_find_smpss, &smpss);

	/* Configure RC MAX payload size */
	reg = advk_readl(pcie, PCIE_CORE_DEV_CTRL_STATS_REG);
	reg &= ~PCI_EXP_DEVCTL_PAYLOAD;
	reg |= smpss << PCIE_CORE_DEV_CTRL_STATS_MAX_PAYLOAD_SZ_SHIFT;
	advk_writel(pcie, reg, PCIE_CORE_DEV_CTRL_STATS_REG);

	/* Configure device MAX payload size */
	advk_pcie_bus_configure_mps(bus->self, &smpss);
	pci_walk_bus(bus, advk_pcie_bus_configure_mps, &smpss);
}

static int advk_pcie_clk_enable_then_reset(struct advk_pcie *pcie)
{
	int ret;

	/* WA: to avoid reset fail, set the reset gpio to low first */
	gpiod_direction_output(pcie->reset_gpio, 0);

	/* Enable pcie clock */
	ret = clk_prepare_enable(pcie->clk);
	if (ret) {
		dev_err(&pcie->pdev->dev, "Failed to enable clock\n");
		return ret;
	}

	/* After 200ms to reset pcie */
	mdelay(200);
	gpiod_direction_output(pcie->reset_gpio,
			       (pcie->flags & OF_GPIO_ACTIVE_LOW) ? 0 : 1);

	return ret;
}

static int advk_pcie_probe(struct platform_device *pdev)
{
	struct advk_pcie *pcie;
	struct resource *res;
	struct pci_bus *bus, *child;
	struct msi_controller *msi;
	struct device_node *msi_node;
	struct phy *comphy;
	struct device_node *dn = pdev->dev.of_node;
	int ret, irq;
	int reset_gpio;

	pcie = devm_kzalloc(&pdev->dev, sizeof(struct advk_pcie),
			    GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	pcie->pdev = pdev;
	platform_set_drvdata(pdev, pcie);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pcie->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(pcie->base))
		return PTR_ERR(pcie->base);

	/* Get comphy and init if there is */
	comphy = devm_of_phy_get(&pdev->dev, dn, "comphy");
	if (!IS_ERR(comphy)) {
		pcie->phy = comphy;
		ret = phy_init(pcie->phy);
		if (ret)
			return ret;

		ret = phy_power_on(pcie->phy);
		if (ret) {
			phy_exit(pcie->phy);
			goto err_exit_phy;
		}
	}

	irq = platform_get_irq(pdev, 0);
	ret = devm_request_irq(&pdev->dev, irq, advk_pcie_irq_handler,
			       IRQF_SHARED | IRQF_NO_THREAD, "advk-pcie",
			       pcie);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register interrupt\n");
		return ret;
	}

	pcie->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(pcie->clk)) {
		dev_err(&pdev->dev, "Failed to obtain clock from DT\n");
		return PTR_ERR(pcie->clk);
	}

	/* Config reset gpio for pcie */
	reset_gpio = of_get_named_gpio_flags(dn, "reset-gpios", 0, &pcie->flags);
	if (reset_gpio != -EPROBE_DEFER) {
		if (gpio_is_valid(reset_gpio)) {
			pcie->reset_gpio = gpio_to_desc(reset_gpio);
			ret = advk_pcie_clk_enable_then_reset(pcie);
			if (ret)
				return ret;

			/* continue init flow after pcie reset */
			goto after_pcie_reset;
		}
	}

	ret = clk_prepare_enable(pcie->clk);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable clock\n");
		return ret;
	}
after_pcie_reset:
	ret = advk_pcie_parse_request_of_pci_ranges(pcie);
	if (ret) {
		dev_err(&pdev->dev, "Failed to parse resources\n");
		goto err_clk;
	}

	advk_pcie_setup_hw(pcie);

	ret = advk_pcie_init_irq_domain(pcie);
	if (ret) {
		dev_err(&pdev->dev, "Failed to initialize irq\n");
		goto err_clk;
	}

	ret = advk_pcie_init_msi_irq_domain(pcie);
	if (ret) {
		dev_err(&pdev->dev, "Failed to initialize irq\n");
		advk_pcie_remove_irq_domain(pcie);
		goto err_clk;
	}

	msi_node = of_parse_phandle(pdev->dev.of_node, "msi-parent", 0);
	if (msi_node)
		msi = of_pci_find_msi_chip_by_node(msi_node);
	else
		msi = NULL;

	bus = pci_scan_root_bus_msi(&pdev->dev, 0, &advk_pcie_ops,
				    pcie, &pcie->resources, &pcie->msi);
	if (!bus) {
		advk_pcie_remove_msi_irq_domain(pcie);
		advk_pcie_remove_irq_domain(pcie);
		ret = -ENOMEM;
		goto err_clk;
	}

	pci_bus_assign_resources(bus);
	pcie->bus = bus;

	list_for_each_entry(child, &bus->children, node)
		pcie_bus_configure_settings(child);

	/* Configure the MAX pay load size */
	advk_pcie_configure_mps(pcie);

	pci_bus_add_devices(bus);

	return 0;

err_clk:
	clk_disable_unprepare(pcie->clk);
err_exit_phy:
	if (pcie->phy)
		phy_exit(pcie->phy);

	return ret;
}

static int advk_pcie_suspend_noirq(struct device *dev)
{
	struct advk_pcie *pcie;

	pcie = dev_get_drvdata(dev);

	/* Gating clock */
	clk_disable_unprepare(pcie->clk);

	/* Power off PHY */
	if (!IS_ERR(pcie->phy)) {
		phy_power_off(pcie->phy);
		phy_exit(pcie->phy);
	}

	return 0;
}

static int advk_pcie_resume_noirq(struct device *dev)
{
	struct advk_pcie *pcie;
	int ret;

	pcie = dev_get_drvdata(dev);

	/* Power on PHY, it must be first, or pcie register access fail */
	if (!IS_ERR(pcie->phy)) {
		phy_init(pcie->phy);
		phy_power_on(pcie->phy);
	}

	if (pcie->reset_gpio)
		ret = advk_pcie_clk_enable_then_reset(pcie);
	else
		ret = clk_prepare_enable(pcie->clk);
	if (ret) {
		dev_err(dev, "Failed to enable clock\n");
		return ret;
	}

	advk_pcie_setup_hw(pcie);

	/* Reconfigure the MAX pay load size */
	advk_pcie_configure_mps(pcie);

	return 0;
}

static const struct of_device_id advk_pcie_of_match_table[] = {
	{ .compatible = "marvell,armada-3700-pcie", },
	{},
};

static const struct dev_pm_ops advk_pcie_pm_ops = {
	.suspend_noirq = advk_pcie_suspend_noirq,
	.resume_noirq = advk_pcie_resume_noirq,
};

static struct platform_driver advk_pcie_driver = {
	.driver = {
		.name = "advk-pcie",
		.of_match_table = advk_pcie_of_match_table,
		/* Driver unloading/unbinding currently not supported */
		.suppress_bind_attrs = true,
		.pm = &advk_pcie_pm_ops,
	},
	.probe = advk_pcie_probe,
};
module_platform_driver(advk_pcie_driver);

MODULE_AUTHOR("Hezi Shahmoon <hezi.shahmoon@marvell.com>");
MODULE_DESCRIPTION("Aardvark PCIe driver");
MODULE_LICENSE("GPL v2");
