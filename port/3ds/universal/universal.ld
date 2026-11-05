/* universal/universal.ld: devkitARM 3dsx.ld (MPL 2.0) plus one shared area for the game copies data (tools/universal.py) */
/*--------------------------------------------------------------------------------
	This Source Code Form is subject to the terms of the Mozilla Public License,
	v. 2.0. If a copy of the MPL was not distributed with this file, You can
	obtain one at https://mozilla.org/MPL/2.0/.
--------------------------------------------------------------------------------*/
OUTPUT_FORMAT("elf32-littlearm", "elf32-bigarm", "elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)

PHDRS
{
	code   PT_LOAD FLAGS(5) /* Read | Execute */;
	rodata PT_LOAD FLAGS(4) /* Read */;
	data   PT_LOAD FLAGS(6) /* Read | Write */;
}

SECTIONS
{
	/* =========== CODE section =========== */

	PROVIDE(__start__ = 0x100000);
	. = __start__;

	.text ALIGN(0x1000) :
	{
		/* .init */
		KEEP( *(.crt0) )
		KEEP( *(.init) )
		. = ALIGN(4);

		/* .text */
		*(.text)
		*(.text.*)
		*(.glue_7)
		*(.glue_7t)
		*(.stub)
		*(.gnu.warning)
		*(.gnu.linkonce.t*)
		. = ALIGN(4);

		/* .fini */
		KEEP( *(.fini) )
		. = ALIGN(4);
	} : code

	/* =========== RODATA section =========== */

	. = ALIGN(0x1000);

	.rodata :
	{
		*(.rodata)
		*(.roda)
		*(.rodata.*)
		*all.rodata*(*)
		*(.gnu.linkonce.r*)
		SORT(CONSTRUCTORS)
		. = ALIGN(4);
		__tdata_align = .;
		LONG (ALIGNOF(.tdata));
		. = ALIGN(4);
	} : rodata

	.ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } : rodata
	__exidx_start = .;
	.ARM.exidx : { *(.ARM.exidx* .gnu.linkonce.armexidx.*) } : rodata
	__exidx_end = .;

	/* =========== DATA section =========== */

	. = ALIGN(0x1000);

	.data :
	{
		*(EXCLUDE_FILE(*build_universal/copy_*) .data)
		*(EXCLUDE_FILE(*build_universal/copy_*) .data.*)
		*(.gnu.linkonce.d*)
		CONSTRUCTORS
		. = ALIGN(4);
	} : data

	.tdata : ALIGN(4)
	{
		__tdata_lma = .;
		*(.tdata)
		*(.tdata.*)
		*(.gnu.linkonce.td.*)
		. = ALIGN(4);
		__tdata_lma_end = .;
	} : data

	.tbss : ALIGN(4)
	{
		*(.tbss)
		*(.tbss.*)
		*(.gnu.linkonce.tb.*)
		*(.tcommon)
		. = ALIGN(4);
	} : data

	/*
    __tdata_align_val = ABSOLUTE(ALIGNOF(.tdata));
    __tdata_size = ABSOLUTE(SIZEOF(.tdata));
    __tbss_align = ABSOLUTE(ALIGNOF(.tbss));
    __tbss_size = ABSOLUTE(SIZEOF(.tbss));
	*/

	.preinit_array ALIGN(4) :
	{
		PROVIDE (__preinit_array_start = .);
		KEEP (*(.preinit_array))
		PROVIDE (__preinit_array_end = .);
	} : data

	.init_array ALIGN(4) :
	{
		PROVIDE (__init_array_start = .);
		KEEP (*(SORT(.init_array.*)))
		KEEP (*(.init_array))
		PROVIDE (__init_array_end = .);
	} : data

	.fini_array ALIGN(4) :
	{
		PROVIDE (__fini_array_start = .);
		KEEP (*(.fini_array))
		KEEP (*(SORT(.fini_array.*)))
		PROVIDE (__fini_array_end = .);
	} : data

	.ctors ALIGN(4) :
	{
		KEEP (*crtbegin.o(.ctors)) /* MUST be first -- GCC requires it */
		KEEP (*(EXCLUDE_FILE (*crtend.o) .ctors))
		KEEP (*(SORT(.ctors.*)))
		KEEP (*(.ctors))
	} : data

	.dtors ALIGN(4) :
	{
		KEEP (*crtbegin.o(.dtors))
		KEEP (*(EXCLUDE_FILE (*crtend.o) .dtors))
		KEEP (*(SORT(.dtors.*)))
		KEEP (*(.dtors))
	} : data

	__bss_start__ = .;
	.bss ALIGN(4) :
	{
		*(.dynbss)
		*(EXCLUDE_FILE(*build_universal/copy_*) .bss)
		*(EXCLUDE_FILE(*build_universal/copy_*) .bss.*)
		*(.gnu.linkonce.b*)
		*(EXCLUDE_FILE(*build_universal/copy_*) COMMON)

		/* the game copies' data and zeroed globals, one area for all of
		   them: only the copy that runs is ever in it (universal/starter.c
		   loads its data from romfs before calling it). crt0 zeroes it. */
		. = ALIGN(0x1000);
		__pm3ds_copy_area = .;
		. += __pm3ds_copy_size;
		__pm3ds_copy_area_end = .;

		/* Reserve space for the TLS segment of the main thread.
		   We need (__tls_start - 8) to be aligned the same as .tdata, to account for
		   the 8-byte ARM TLS header. Since the header is not actually used for
		   ARM_TLS_LE32 relocation, we just fake it by subtracting 8 from the data
		   offset.
		 */
		. = 8 + ABSOLUTE(ALIGN(ABSOLUTE(. - 8), MAX(ALIGNOF(.tdata), ALIGNOF(.tbss))));
		__tls_start = .;
		. += SIZEOF(.tdata);

        /* Ensure the alignment of .tbss is accounted for in the total size,
           since SIZEOF(.tdata) doesn't include any padding before .tbss.
         */
        . = ALIGN(ALIGNOF(.tbss));
        . += SIZEOF(.tbss);
		__tls_end = .;
	} : data
	__bss_end__ = .;

	__end__ = ABSOLUTE(.) ;

	/* =========== the game copies (tools/universal.py) ===========
	   Each one's .data and .bss, linked at __pm3ds_copy_area but not
	   loaded (COPY: not allocated): tools/package.sh dumps .copy_<v>_data
	   into romfs, and the 3DS gets the ELF without these sections. */

	.copy_pal_data __pm3ds_copy_area (COPY) :
	{
		*build_universal/copy_pal/*(.data .data.*)
		. = ALIGN(8);
	}
	.copy_pal_bss ALIGN(__pm3ds_copy_area + SIZEOF(.copy_pal_data), 8) (COPY) :
	{
		*build_universal/copy_pal/*(.bss .bss.* COMMON)
		. = ALIGN(8);
	}
	__pm3ds_copy_pal_end = ADDR(.copy_pal_bss) + SIZEOF(.copy_pal_bss);
	ASSERT(__pm3ds_copy_pal_end <= __pm3ds_copy_area_end, "the copy area is too small for pal")

	.copy_us_data __pm3ds_copy_area (COPY) :
	{
		*build_universal/copy_us/*(.data .data.*)
		. = ALIGN(8);
	}
	.copy_us_bss ALIGN(__pm3ds_copy_area + SIZEOF(.copy_us_data), 8) (COPY) :
	{
		*build_universal/copy_us/*(.bss .bss.* COMMON)
		. = ALIGN(8);
	}
	__pm3ds_copy_us_end = ADDR(.copy_us_bss) + SIZEOF(.copy_us_bss);
	ASSERT(__pm3ds_copy_us_end <= __pm3ds_copy_area_end, "the copy area is too small for us")

	.copy_iq_data __pm3ds_copy_area (COPY) :
	{
		*build_universal/copy_iq/*(.data .data.*)
		. = ALIGN(8);
	}
	.copy_iq_bss ALIGN(__pm3ds_copy_area + SIZEOF(.copy_iq_data), 8) (COPY) :
	{
		*build_universal/copy_iq/*(.bss .bss.* COMMON)
		. = ALIGN(8);
	}
	__pm3ds_copy_iq_end = ADDR(.copy_iq_bss) + SIZEOF(.copy_iq_bss);
	ASSERT(__pm3ds_copy_iq_end <= __pm3ds_copy_area_end, "the copy area is too small for iq")

	/* ==================
	   ==== Metadata ====
	   ================== */

	/* Discard sections that difficult post-processing */
	/DISCARD/ : { *(.group .comment .note) }

	/* Stabs debugging sections. */
	.stab          0 : { *(.stab) }
	.stabstr       0 : { *(.stabstr) }
	.stab.excl     0 : { *(.stab.excl) }
	.stab.exclstr  0 : { *(.stab.exclstr) }
	.stab.index    0 : { *(.stab.index) }
	.stab.indexstr 0 : { *(.stab.indexstr) }

	/* DWARF debug sections.
	   Symbols in the DWARF debugging sections are relative to the beginning
	   of the section so we begin them at 0. */

	/* DWARF 1 */
	.debug          0 : { *(.debug) }
	.line           0 : { *(.line) }

	/* GNU DWARF 1 extensions */
	.debug_srcinfo  0 : { *(.debug_srcinfo) }
	.debug_sfnames  0 : { *(.debug_sfnames) }

	/* DWARF 1.1 and DWARF 2 */
	.debug_aranges  0 : { *(.debug_aranges) }
	.debug_pubnames 0 : { *(.debug_pubnames) }

	/* DWARF 2 */
	.debug_info     0 : { *(.debug_info) }
	.debug_abbrev   0 : { *(.debug_abbrev) }
	.debug_line     0 : { *(.debug_line) }
	.debug_frame    0 : { *(.debug_frame) }
	.debug_str      0 : { *(.debug_str) }
	.debug_loc      0 : { *(.debug_loc) }
	.debug_macinfo  0 : { *(.debug_macinfo) }
}
