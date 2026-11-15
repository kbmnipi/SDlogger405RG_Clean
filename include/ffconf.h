/*---------------------------------------------------------------------------/
/  FatFs - FAT file system module configuration file   (R0.12c / ST port)    /
/----------------------------------------------------------------------------/
/  Project : SDlogger405RG (STM32F405, HAL_SD backend)                       /
/---------------------------------------------------------------------------*/

#ifndef _FFCONF
#define _FFCONF	68300	/* Revision ID (must equal the one in ff.h) */

/*---------------------------------------------------------------------------/
/ Function Configurations                                                   /
/---------------------------------------------------------------------------*/
#define _FS_READONLY	0	/* 0: Read/Write */
#define _FS_MINIMIZE	0	/* Keep all basic API functions */
#define _USE_STRFUNC	0	/* Disable string I/O */
#define _USE_FIND		0	/* Disable filtered directory read */
#define _USE_MKFS		0	/* 0: No formatting support */
#define _USE_FASTSEEK	0	/* Disable fast-seek linkmap */
#define _USE_EXPAND		1	/* Enable f_expand() - used by vibl_open() (P3-1) */
#define _USE_CHMOD		0	/* Disable attribute manipulation */
#define _USE_LABEL		1	/* Enable volume label functions */
#define _USE_FORWARD	0	/* Disable f_forward() */

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations                                       /
/---------------------------------------------------------------------------*/
#define _CODE_PAGE	437	/* OEM code page (US) */
#define _USE_LFN		0	/* 0: Short file names only */
#define _MAX_LFN	255	/* Max LFN length */
#define _LFN_UNICODE	0	/* API strings are ANSI/OEM */
#define _STRF_ENCODE	3	/* Inert */
#define _FS_RPATH	0	/* Disable relative-path support */

/*---------------------------------------------------------------------------/
/ Drive / Volume Configurations                                             /
/---------------------------------------------------------------------------*/
#define _VOLUMES	1	/* One logical drive */
#define _STR_VOLUME_ID	0	/* No string volume IDs */
#define _MULTI_PARTITION	0	/* Single partition */
#define _MIN_SS		512	/* Supported sector size */
#define _MAX_SS		512	/* Max sector size */
#define _USE_TRIM	0	/* No ATA-TRIM support */
#define _FS_NOFSINFO	0	/* Trust FAT32 FSINFO */
#define _FS_TIMEOUT	1	/* Enable disk-access timeout */

/*---------------------------------------------------------------------------/
/ System Configurations                                                     /
/---------------------------------------------------------------------------*/
#define _FS_TINY	0	/* Normal buffer config */
#define _FS_EXFAT	0	/* exFAT disabled */
/* P2-9 (audit WARN-4): 1 -> 0 so GET_FATTIME() calls the application-provided
 * get_fattime() (ff.c: "GET_FATTIME() get_fattime()" branch; declared in ff.h
 * under `#if !_FS_READONLY && !_FS_NORTC`). The board has LSE 32.768 kHz on
 * PC14/PC15 (doc/WEACT_F405_PINOUT.md §3) driving the RTC, so file mtimes are
 * now real instead of the fixed 2026-01-01. */
#define _FS_NORTC	0	/* Use RTC via get_fattime() in diskio.c */
/* The _NORTC_* block below is inert while _FS_NORTC == 0 (ff.c only compiles it
 * for _FS_NORTC == 1). Kept as the documented fallback (P0 rollback) values. */
#define _NORTC_MON	1	/* Fixed month   (inert: _FS_NORTC == 0) */
#define _NORTC_MDAY	1	/* Fixed day     (inert: _FS_NORTC == 0) */
#define _NORTC_YEAR	2026	/* Fixed year    (inert: _FS_NORTC == 0) */
#define _FS_LOCK	0	/* Disable file-lock control */
#define _FS_REENTRANT	0	/* No RTOS */
#define _USE_MUTEX	0	/* Inert */

#endif /* _FFCONF */