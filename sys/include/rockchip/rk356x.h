/* Rockchip RK356x (RK3566 / RK3568) SoC family register definitions.
 *
 * The RK3566 and RK3568 share one SoC family ("rk356x" in the kernel
 * device trees).  This header is the single source of truth for the
 * peripheral register layouts that the user-space file servers (gpiofs,
 * i2cfs, pwmfs, ...) program through #P discovery + segattach.
 *
 * Base addresses here are documentation and fallback only: the drivers
 * discover the live addresses from the device tree through #P.  Register
 * *offsets* are constant for the family and are what the drivers depend
 * on.
 */
#ifndef HARVEY_ROCKCHIP_RK356X_H
#define HARVEY_ROCKCHIP_RK356X_H

/* ------------------------------------------------------------------ */
/* GPIO controller (version 2), 5 banks x 32 pins.                     */
/* ------------------------------------------------------------------ */

/* Register offsets in bytes from a bank base.  The low (pins 0-15) and
 * high (pins 16-31) halves are separate 32-bit words.  For the DR and
 * DDR words the upper 16 bits are a write-enable mask: write 0xffff to
 * update all 16 pins of that half. */
enum {
	RK_GPIO_DR_L		= 0x00,	/* data out, pins 0-15 */
	RK_GPIO_DR_H		= 0x04,	/* data out, pins 16-31 */
	RK_GPIO_DDR_L		= 0x08,	/* direction, 1 = output */
	RK_GPIO_DDR_H		= 0x0c,	/* direction, 1 = output */
	RK_GPIO_INTEN_L		= 0x10,
	RK_GPIO_INTEN_H		= 0x14,
	RK_GPIO_INTMASK_L	= 0x18,
	RK_GPIO_INTMASK_H	= 0x1c,
	RK_GPIO_INTTYPE_L	= 0x20,
	RK_GPIO_INTTYPE_H	= 0x24,
	RK_GPIO_INTPOL_L	= 0x28,
	RK_GPIO_INTPOL_H	= 0x2c,
	RK_GPIO_INTBOTH_L	= 0x30,
	RK_GPIO_INTBOTH_H	= 0x34,
	RK_GPIO_DEBOUNCE_L	= 0x38,
	RK_GPIO_DEBOUNCE_H	= 0x3c,
	RK_GPIO_DBCLK_EN	= 0x40,
	RK_GPIO_DBCLK_CON	= 0x48,
	RK_GPIO_INTSTATUS_L	= 0x50,
	RK_GPIO_INTSTATUS_H	= 0x54,
	RK_GPIO_INTRAW_L	= 0x58,
	RK_GPIO_INTRAW_H	= 0x5c,
	RK_GPIO_PORT_EOI_L	= 0x60,
	RK_GPIO_PORT_EOI_H	= 0x64,
	RK_GPIO_EXT_PORT	= 0x70,	/* actual pin state, all 32 pins */
	RK_GPIO_VERSION		= 0x78,
};

/* Bank base addresses (fallback; drivers normally use #P). */
enum {
	RK_GPIO0_BASE	= 0xfdd60000,
	RK_GPIO1_BASE	= 0xfe740000,
	RK_GPIO2_BASE	= 0xfe750000,
	RK_GPIO3_BASE	= 0xfe760000,
	RK_GPIO4_BASE	= 0xfe770000,
};

/* ------------------------------------------------------------------ */
/* System registers: GRF (pinmux), CRU (clock/reset).                  */
/* ------------------------------------------------------------------ */

enum {
	RK_GRF_BASE	= 0xfdc60000,	/* system general register file */
	RK_PMUGRF_BASE	= 0xfdc20000,	/* PMU GRF, owns GPIO0 pinmux */
	RK_CRU_BASE	= 0xfdd20000,	/* system clock/reset unit */
	RK_PMUCRU_BASE	= 0xfdd00000,	/* PMU clock/reset unit */
};

/* Pinmux (iomux) layout.  4 bits per pin, 4 pins per 32-bit register:
 * the L register covers pins 0-3 of a group, the H register pins 4-7.
 * A group is A/B/C/D = pins 0-7 / 8-15 / 16-23 / 24-31 of a bank.
 * GPIO0's iomux lives in PMUGRF; GPIO1..GPIO4 live in GRF with a 0x100
 * bank stride.  Function value 0 selects the plain GPIO function. */
enum {
	RK_IOMUX_FUNC_GPIO	= 0,
};

/* Byte offset (from the owning GRF/PMUGRF base) of the iomux register
 * holding the mux field for the given bank and pin. */
#define RK_IOMUX_REG(bank, pin) \
	((((pin) / 8) * 0x10) + ((((pin) % 8) / 4) * 0x4) + \
	 ((bank) == 0 ? 0 : ((bank) - 1) * 0x100))

/* Bit position of a pin's 4-bit mux field inside its iomux register. */
#define RK_IOMUX_SHIFT(pin)	(((pin) % 4) * 4)

/* ------------------------------------------------------------------ */
/* I2C controller (Rockchip RK3x).                                     */
/* ------------------------------------------------------------------ */

/* Register offsets in bytes. */
enum {
	RK_I2C_CON	= 0x00,	/* control */
	RK_I2C_CLKDIV	= 0x04,	/* (divh << 16) | divl */
	RK_I2C_MRXADDR	= 0x08,	/* slave address, for register TX */
	RK_I2C_MRXRADDR	= 0x0c,	/* slave register address */
	RK_I2C_MTXCNT	= 0x10,	/* bytes to transmit */
	RK_I2C_MRXCNT	= 0x14,	/* bytes to receive */
	RK_I2C_IEN	= 0x18,	/* interrupt enable */
	RK_I2C_IPD	= 0x1c,	/* interrupt pending (status) */
	RK_I2C_FCNT	= 0x20,	/* finished count */
	RK_I2C_SCL_OE_DB = 0x24, /* SCL output-enable debounce */
	RK_I2C_TXBUF	= 0x100, /* transmit FIFO base */
	RK_I2C_RXBUF	= 0x200, /* receive FIFO base */
	RK_I2C_CON1	= 0x228, /* control 1 (RK356x extension) */
};

/* RK_I2C_CON bits. */
enum {
	RK_I2C_EN	= 1 << 0,
	RK_I2C_MOD_TX	= 0 << 1,	/* transmit */
	RK_I2C_MOD_REG_TX = 1 << 1,	/* register-then-restart TX (read) */
	RK_I2C_MOD_RX	= 2 << 1,	/* receive */
	RK_I2C_START	= 1 << 3,
	RK_I2C_STOP	= 1 << 4,
	RK_I2C_LASTACK	= 1 << 5,	/* NACK after last received byte */
	RK_I2C_ACTACK	= 1 << 6,	/* stop on NACK */
};

/* RK_I2C_IEN / RK_I2C_IPD bits. */
enum {
	RK_I2C_INT_BTF	= 1 << 0,	/* byte transmitted */
	RK_I2C_INT_BRF	= 1 << 1,	/* byte received */
	RK_I2C_INT_MBTF	= 1 << 2,	/* TX finished */
	RK_I2C_INT_MBRF	= 1 << 3,	/* RX finished */
	RK_I2C_INT_START = 1 << 4,	/* START generated */
	RK_I2C_INT_STOP	= 1 << 5,	/* STOP generated */
	RK_I2C_INT_NAK	= 1 << 6,	/* NACK received */
	RK_I2C_INT_ALL	= 0xff,
};

/* MRXADDR/MRXRADDR byte-valid bits: bit 24+x marks byte x present. */
#define RK_I2C_ADDR_VALID(x)	(1 << (24 + (x)))

/* ------------------------------------------------------------------ */
/* CRU (clock/reset unit) register offsets.                            */
/* ------------------------------------------------------------------ */

/* System CRU (RK_CRU_BASE) and PMU CRU (RK_PMUCRU_BASE) use different
 * register bases but the same offset scheme. */
#define RK_CRU_CLKGATE(n)	(0x300 + (n) * 4)
#define RK_CRU_SOFTRST(n)	(0x400 + (n) * 4)
#define RK_PMU_CLKGATE(n)	(0x180 + (n) * 4)
#define RK_PMU_SOFTRST(n)	(0x200 + (n) * 4)

/* Clock gates use the HIWORD-MASK + SET-TO-DISABLE scheme: bits [31:16]
 * are the write mask, bits [15:0] the value, and a clear bit ENABLES the
 * clock.  To enable clock gate bit b, write (1 << (16 + b)). */

/* I2C clock gates.  I2C0 lives in the PMUCRU, I2C1..I2C5 in the CRU:
 *   I2C0: PMU CLKGATE_CON(1), PCLK bit 0, CLK bit 1.
 *   I2Cn (n=1..5): CRU CLKGATE_CON(30), PCLK bit 2(n-1), CLK bit 2(n-1)+1.
 */

/* Soft reset IDs (flat) map to SOFTRST_CON(id/16), bit id%16.  The ID is
 * asserted/reset the same HIWORD-MASK way; a set bit asserts reset. */

#endif
