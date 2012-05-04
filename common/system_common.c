/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* System module for Chrome EC : common functions */

#include "console.h"
#include "hooks.h"
#include "host_command.h"
#include "lpc.h"
#include "lpc_commands.h"
#include "system.h"
#include "task.h"
#include "uart.h"
#include "util.h"
#include "version.h"

/* Console output macros */
#define CPUTS(outstr) cputs(CC_SYSTEM, outstr)
#define CPRINTF(format, args...) cprintf(CC_SYSTEM, format, ## args)

struct jump_tag {
	uint16_t tag;
	uint8_t data_size;
	uint8_t data_version;
};


/* Data passed between the current image and the next one when jumping between
 * images. */
#define JUMP_DATA_MAGIC 0x706d754a  /* "Jump" */
#define JUMP_DATA_VERSION 3
struct jump_data {
	/* Add new fields to the _start_ of the struct, since we copy it to the
	 * _end_ of RAM between images.  This way, the magic number will always
	 * be the last word in RAM regardless of how many fields are added. */

	/* Fields from version 3 */
	uint8_t recovery_required; /* signal recovery mode to BIOS */
	int header_size;     /* Header size to correctly point to jump tags. */

	/* Fields from version 2 */
	int jump_tag_total;  /* Total size of all jump tags */

	/* Fields from version 1 */
	int reset_cause;     /* Reset cause for the previous boot */
	int version;         /* Version (JUMP_DATA_VERSION) */
	int magic;           /* Magic number (JUMP_DATA_MAGIC).  If this
			      * doesn't match at pre-init time, assume no valid
			      * data from the previous image. */
};

/* Jump data goes at the end of RAM */
static struct jump_data * const jdata =
	(struct jump_data *)(CONFIG_RAM_BASE + CONFIG_RAM_SIZE
			     - sizeof(struct jump_data));

static enum system_reset_cause_t reset_cause = SYSTEM_RESET_UNKNOWN;
static int jumped_to_image;


int system_usable_ram_end(void)
{
	/* Leave space at the end of RAM for jump data.
	 *
	 * Note that jump_tag_total is 0 on a reboot, so we have the maximum
	 * amount of RAM available on a reboot; we only lose space for stored
	 * tags after a sysjump.  When verified boot runs after a reboot, it'll
	 * have as much RAM as we can give it; after verified boot jumps to
	 * another image there'll be less RAM, but we'll care less too. */
	return (uint32_t)jdata - jdata->jump_tag_total;
}


enum system_reset_cause_t system_get_reset_cause(void)
{
	return reset_cause;
}


int system_get_recovery_required(void)
{
	return jdata->recovery_required;
}


int system_jumped_to_this_image(void)
{
	return jumped_to_image;
}


int system_add_jump_tag(uint16_t tag, int version, int size, const void *data)
{
	struct jump_tag *t;

	/* Only allowed during a sysjump */
	if (jdata->magic != JUMP_DATA_MAGIC)
		return EC_ERROR_UNKNOWN;

	/* Make room for the new tag */
	if (size > 255 || (size & 3))
		return EC_ERROR_INVAL;
	jdata->jump_tag_total += size + sizeof(struct jump_tag);

	t = (struct jump_tag *)system_usable_ram_end();
	t->tag = tag;
	t->data_size = size;
	t->data_version = version;
	if (size)
		memcpy(t + 1, data, size);

	return EC_SUCCESS;
}

const uint8_t *system_get_jump_tag(uint16_t tag, int *version, int *size)
{
	const struct jump_tag *t;
	int used = 0;

	/* Search through tag data for a match */
	while (used < jdata->jump_tag_total) {
		/* Check the next tag */
		t = (const struct jump_tag *)(system_usable_ram_end() + used);
		used += sizeof(struct jump_tag) + t->data_size;
		if (t->tag != tag)
			continue;

		/* Found a match */
		if (size)
			*size = t->data_size;
		if (version)
			*version = t->data_version;

		return (const uint8_t *)(t + 1);
	}

	/* If we're still here, no match */
	return NULL;
}


void system_set_reset_cause(enum system_reset_cause_t cause)
{
	reset_cause = cause;
}


const char *system_get_reset_cause_string(void)
{
	static const char * const cause_descs[] = {
		"unknown", "other", "brownout", "power-on", "reset pin",
		"soft cold", "soft warm", "watchdog", "rtc alarm", "wake pin",
		"low battery"};

	return reset_cause < ARRAY_SIZE(cause_descs) ?
			cause_descs[reset_cause] : "?";
}


enum system_image_copy_t system_get_image_copy(void)
{
	int copy = ((uint32_t)system_get_image_copy - CONFIG_FLASH_BASE) /
		   CONFIG_FW_IMAGE_SIZE;
	switch (copy) {
	case 0:
		return SYSTEM_IMAGE_RO;
	case 1:
		return SYSTEM_IMAGE_RW_A;
	case 2:
		return SYSTEM_IMAGE_RW_B;
	default:
		return SYSTEM_IMAGE_UNKNOWN;
	}
}


/* Returns true if the given range is overlapped with the active image.
 *
 * We only care the runtime code since the EC is running over it.
 * We don't care about the vector table, FMAP, and init code. */
int system_unsafe_to_overwrite(uint32_t offset, uint32_t size) {
	int copy = ((uint32_t)system_unsafe_to_overwrite - CONFIG_FLASH_BASE) /
	           CONFIG_FW_IMAGE_SIZE;
	uint32_t r_offset = copy * CONFIG_FW_IMAGE_SIZE;
	uint32_t r_size = CONFIG_FW_IMAGE_SIZE;

	if ((offset >= r_offset && offset < (r_offset + r_size)) ||
	    (r_offset >= offset && r_offset < (offset + size)))
		return 1;
	else
		return 0;
}


const char *system_get_image_copy_string(void)
{
	static const char * const copy_descs[] = {"unknown", "RO", "A", "B"};
	int copy = system_get_image_copy();
	return copy < ARRAY_SIZE(copy_descs) ? copy_descs[copy] : "?";
}


/* Jump to what we hope is the init address of an image.  This function does
 * not return. */
static void jump_to_image(uint32_t init_addr,
			  int recovery_required)
{
	void (*resetvec)(void) = (void(*)(void))init_addr;

	/* Flush UART output unless the UART hasn't been initialized yet */
	if (uart_init_done())
		uart_flush_output();

	/* Disable interrupts before jump */
	interrupt_disable();

	/* Fill in preserved data between jumps */
	jdata->recovery_required = recovery_required != 0;
	jdata->magic = JUMP_DATA_MAGIC;
	jdata->version = JUMP_DATA_VERSION;
	jdata->reset_cause = reset_cause;
	jdata->jump_tag_total = 0;  /* Reset tags */
	jdata->header_size = sizeof(struct jump_data);

	/* Call other hooks; these may add tags */
	hook_notify(HOOK_SYSJUMP, 0);

	/* Jump to the reset vector */
	resetvec();
}


/* Return the base pointer for the image copy, or 0xffffffff if error. */
static uint32_t get_base(enum system_image_copy_t copy)
{
	switch (copy) {
	case SYSTEM_IMAGE_RO:
		return CONFIG_FW_RO_OFF;
	case SYSTEM_IMAGE_RW_A:
		return CONFIG_FW_A_OFF;
#ifndef CONFIG_NO_RW_B
	case SYSTEM_IMAGE_RW_B:
		return CONFIG_FW_B_OFF;
#endif
	default:
		return 0xffffffff;
	}
}


static const char * const image_names[] = {
	"Unknown",
	"RO",
	"A",
	"B"
};

int system_run_image_copy(enum system_image_copy_t copy,
			  int recovery_required)
{
	uint32_t base;
	uint32_t init_addr;

	/* TODO: sanity checks (crosbug.com/p/7468)
	 *
	 * For this to be allowed either WP must be disabled, or ALL of the
	 * following must be true:
	 *  - We must currently be running the RO image.
	 *  - We must still be in init (that is, before task_start().
	 *  - The target image must be A or B. */

	/* Load the appropriate reset vector */
	base = get_base(copy);
	if (base == 0xffffffff)
		return EC_ERROR_INVAL;

	/* Make sure the reset vector is inside the destination image */
	init_addr = *(uint32_t *)(base + 4);
	if (init_addr < base || init_addr >= base + CONFIG_FW_IMAGE_SIZE)
		return EC_ERROR_UNKNOWN;

	CPRINTF("Rebooting to image %s\n", image_names[copy]);

	jump_to_image(init_addr, recovery_required);

	/* Should never get here */
	return EC_ERROR_UNIMPLEMENTED;
}


const char *system_get_version(enum system_image_copy_t copy)
{
	uint32_t addr;
	const struct version_struct *v;

	/* Handle version of current image */
	if (copy == system_get_image_copy() || copy == SYSTEM_IMAGE_UNKNOWN)
		return version_data.version;

	addr = get_base(copy);
	if (addr == 0xffffffff)
		return "";

	/* The version string is always located after the reset vectors, so
	 * it's the same as in the current image. */
	addr += ((uint32_t)&version_data - get_base(system_get_image_copy()));

	/* Make sure the version struct cookies match before returning the
	 * version string. */
	v = (const struct version_struct *)addr;
	if (v->cookie1 == version_data.cookie1 &&
	    v->cookie2 == version_data.cookie2)
		return v->version;

	return "";
}


const char *system_get_build_info(void)
{
	return build_info;
}


int system_common_pre_init(void)
{
	/* Check jump data if this is a jump between images */
	if (jdata->magic == JUMP_DATA_MAGIC &&
	    jdata->version >= 1 &&
	    reset_cause == SYSTEM_RESET_SOFT_WARM) {
		int jtag_total;  /* #byte of jump tags */
		int delta;       /* delta of header size */
		uint8_t *jtag;   /* point to _real_ start of jump tags */

		/* Yes, we jumped to this image */
		jumped_to_image = 1;
		/* Overwrite the reset cause with the real one */
		reset_cause = jdata->reset_cause;

		/* Header version 3 introduces the header_size field.
		 * Thus we can estimate the real offset of jump tags
		 * between different header versions.
		 */
		if (jdata->version == 1) {
			jtag_total = 0;
			delta = sizeof(struct jump_data) - 12;
		} else if (jdata->version == 2) {
			jtag_total = jdata->jump_tag_total;
			delta = sizeof(struct jump_data) - 16;
		} else {
			jtag_total = jdata->jump_tag_total;
			delta = sizeof(struct jump_data) - jdata->header_size;
		}
		jtag = ((uint8_t*)jdata) + delta - jtag_total;
		/* TODO: re-write with memmove(). */
		if (delta > 0) {
			memcpy(jtag - delta, jtag, jtag_total);
		} else {
			int i;
			for (i = jtag_total - 1; i >= 0; i--)
				jtag[i - delta] = jtag[i];
		}

		/* Initialize fields added after version 1 */
		if (jdata->version < 2)
			jdata->jump_tag_total = 0;

		/* Initialize fields added after version 2 */
		if (jdata->version < 3) {
			jdata->recovery_required = 0;
			jdata->header_size = 16;
		}

		/* Clear the jump struct's magic number.  This prevents
		 * accidentally detecting a jump when there wasn't one, and
		 * disallows use of system_add_jump_tag(). */
		jdata->magic = 0;
	} else {
		/* Clear the whole jump_data struct */
		memset(jdata, 0, sizeof(struct jump_data));
	}

	return EC_SUCCESS;
}

/*****************************************************************************/
/* Console commands */

static int command_sysinfo(int argc, char **argv)
{
	ccprintf("Reset cause: %d (%s)\n",
		    system_get_reset_cause(),
		    system_get_reset_cause_string());
	ccprintf("Scratchpad: 0x%08x\n", system_get_scratchpad());
	ccprintf("Firmware copy: %s\n", system_get_image_copy_string());
	ccprintf("Jumped to this copy: %s\n",
		    system_jumped_to_this_image() ? "yes" : "no");
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(sysinfo, command_sysinfo);


static int command_chipinfo(int argc, char **argv)
{
	ccprintf("Chip vendor:   %s\n", system_get_chip_vendor());
	ccprintf("Chip name:     %s\n", system_get_chip_name());
	ccprintf("Chip revision: %s\n", system_get_chip_revision());
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(chipinfo, command_chipinfo);


static int command_set_scratchpad(int argc, char **argv)
{
	int s;
	char *e;

	if (argc < 2) {
		ccputs("Usage: scratchpad <value>\n");
		return EC_ERROR_UNKNOWN;
	}

	s = strtoi(argv[1], &e, 0);
	if (*e) {
		ccputs("Invalid scratchpad value\n");
		return EC_ERROR_UNKNOWN;
	}
	ccprintf("Setting scratchpad to 0x%08x\n", s);
	return  system_set_scratchpad(s);
}
DECLARE_CONSOLE_COMMAND(setscratchpad, command_set_scratchpad);


static int command_hibernate(int argc, char **argv)
{
	int seconds;
	int microseconds = 0;

	if (argc < 2) {
		ccputs("Usage: hibernate <seconds> [<microseconds>]\n");
		return EC_ERROR_UNKNOWN;
	}
	seconds = strtoi(argv[1], NULL, 0);
	if (argc >= 3)
		microseconds = strtoi(argv[2], NULL, 0);

	ccprintf("Hibernating for %d.%06d s ...\n", seconds, microseconds);
	cflush();

	system_hibernate(seconds, microseconds);

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(hibernate, command_hibernate);


static int command_version(int argc, char **argv)
{
	ccprintf("RO version:   %s\n",
		    system_get_version(SYSTEM_IMAGE_RO));
	ccprintf("RW-A version: %s\n",
		    system_get_version(SYSTEM_IMAGE_RW_A));
	ccprintf("RW-B version: %s\n",
		    system_get_version(SYSTEM_IMAGE_RW_B));
	ccprintf("Current build: %s\n", system_get_build_info());
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(version, command_version);


static int command_sysjump(int argc, char **argv)
{
	uint32_t addr;
	char *e;

	/* TODO: (crosbug.com/p/7468) For this command to be allowed, WP must
	 * be disabled. */

	if (argc < 2) {
		ccputs("Usage: sysjump <RO | A | B | addr>\n");
		return EC_ERROR_INVAL;
	}

	ccputs("Processing sysjump command\n");

	/* Handle named images */
	if (!strcasecmp(argv[1], "RO")) {
		return system_run_image_copy(SYSTEM_IMAGE_RO, 0);
	} else if (!strcasecmp(argv[1], "A")) {
		return system_run_image_copy(SYSTEM_IMAGE_RW_A, 0);
	} else if (!strcasecmp(argv[1], "B")) {
		return system_run_image_copy(SYSTEM_IMAGE_RW_B, 0);
	}

	/* Check for arbitrary address */
	addr = strtoi(argv[1], &e, 0);
	if (e && *e) {
		ccputs("Invalid image address\n");
		return EC_ERROR_INVAL;
	}
	ccprintf("Jumping directly to 0x%08x...\n", addr);
	cflush();
	jump_to_image(addr, 0);
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(sysjump, command_sysjump);


static int command_reboot(int argc, char **argv)
{
	ccputs("Rebooting!\n\n\n");
	cflush();
	system_reset(1);
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(reboot, command_reboot);

/*****************************************************************************/
/* Host commands */

static enum lpc_status host_command_get_version(uint8_t *data)
{
	struct lpc_response_get_version *r =
			(struct lpc_response_get_version *)data;

	strzcpy(r->version_string_ro, system_get_version(SYSTEM_IMAGE_RO),
		sizeof(r->version_string_ro));
	strzcpy(r->version_string_rw_a, system_get_version(SYSTEM_IMAGE_RW_A),
		sizeof(r->version_string_rw_a));
	strzcpy(r->version_string_rw_b, system_get_version(SYSTEM_IMAGE_RW_B),
		sizeof(r->version_string_rw_b));

	switch (system_get_image_copy()) {
	case SYSTEM_IMAGE_RO:
		r->current_image = EC_LPC_IMAGE_RO;
		break;
	case SYSTEM_IMAGE_RW_A:
		r->current_image = EC_LPC_IMAGE_RW_A;
		break;
	case SYSTEM_IMAGE_RW_B:
		r->current_image = EC_LPC_IMAGE_RW_B;
		break;
	default:
		r->current_image = EC_LPC_IMAGE_UNKNOWN;
		break;
	}

	return EC_LPC_RESULT_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_LPC_COMMAND_GET_VERSION, host_command_get_version);


static enum lpc_status host_command_build_info(uint8_t *data)
{
	struct lpc_response_get_build_info *r =
			(struct lpc_response_get_build_info *)data;

	strzcpy(r->build_string, system_get_build_info(),
		sizeof(r->build_string));

	return EC_LPC_RESULT_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_LPC_COMMAND_GET_BUILD_INFO, host_command_build_info);


static enum lpc_status host_command_get_chip_info(uint8_t *data)
{
	struct lpc_response_get_chip_info *r =
			(struct lpc_response_get_chip_info *)data;

	strzcpy(r->vendor, system_get_chip_vendor(), sizeof(r->vendor));
	strzcpy(r->name, system_get_chip_name(), sizeof(r->name));
	strzcpy(r->revision, system_get_chip_revision(), sizeof(r->revision));

	return EC_LPC_RESULT_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_LPC_COMMAND_GET_CHIP_INFO, host_command_get_chip_info);


#ifdef CONFIG_REBOOT_EC
static void clean_busy_bits(void) {
#ifdef CONFIG_LPC
	lpc_send_host_response(0, EC_LPC_RESULT_SUCCESS);
	lpc_send_host_response(1, EC_LPC_RESULT_SUCCESS);
#endif
}

enum lpc_status host_command_reboot(uint8_t *data)
{
	enum system_image_copy_t copy;

	struct lpc_params_reboot_ec *p =
		(struct lpc_params_reboot_ec *)data;

	int recovery_request = p->reboot_flags &
		EC_LPC_COMMAND_REBOOT_BIT_RECOVERY;

	/* TODO: (crosbug.com/p/7468) For this command to be allowed, WP must
	 * be disabled. */

	switch (p->target) {
	case EC_LPC_IMAGE_RO:
		copy = SYSTEM_IMAGE_RO;
		break;
	case EC_LPC_IMAGE_RW_A:
		copy = SYSTEM_IMAGE_RW_A;
		break;
	case EC_LPC_IMAGE_RW_B:
		copy = SYSTEM_IMAGE_RW_B;
		break;
	default:
		return EC_LPC_RESULT_ERROR;
	}

	clean_busy_bits();
	CPUTS("Executing host reboot command\n");
	system_run_image_copy(copy, recovery_request);

	/* We normally never get down here, because we'll have jumped to
	 * another image.  To confirm this command worked, the host will need
	 * to check what image is current using GET_VERSION.
	 *
	 * If we DO get down here, something went wrong in the reboot, so
	 * return error. */
	return EC_LPC_RESULT_ERROR;
}
DECLARE_HOST_COMMAND(EC_LPC_COMMAND_REBOOT_EC, host_command_reboot);
#endif /* CONFIG_REBOOT_EC */
