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

#endif
