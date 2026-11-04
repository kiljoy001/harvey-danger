/*
 * Link script for Harvey/ARM64 APEX programs.
 *
 * The kernel's ELF loader maps each PT_LOAD at p_align and rejects
 * overlaps, so the text and data segments are 2 MiB apart and aligned to
 * the 4 KiB page.  The TLS geometry and the two phdrs libunwind needs are
 * recorded as symbols because Harvey's exec does not map the ELF header,
 * so __ehdr_start cannot be read at run time.
 */
ENTRY(_start)
PHDRS {
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
  tls PT_TLS FLAGS(4);
  eh_frame_hdr PT_GNU_EH_FRAME FLAGS(4);
}
SECTIONS {
  . = 0x200000 + SIZEOF_HEADERS;
  .text : { *(.text .text.*) *(.rodata .rodata.*) } :text
  .eh_frame_hdr : { *(.eh_frame_hdr) } :text :eh_frame_hdr
  . = ALIGN(0x200000);
  .data : { *(.data .data.*) *(.data.rel.ro*) } :data
  .tdata : { *(.tdata .tdata.*) } :data :tls
  .tbss : { *(.tbss .tbss.*) } :data :tls
  .bss : { *(.bss .bss.* COMMON) } :data
  PROVIDE(end = .);
  /* Non-zero whenever the script provided the TLS geometry: .tdata can be
   * empty (all TLS in .tbss), which would make __tls_vaddr zero. */
  __tls_present = 1;
  __tls_vaddr = ADDR(.tdata);
  __tls_filesz = SIZEOF(.tdata);
  __tls_memsz = SIZEOF(.tdata) + SIZEOF(.tbss);
  __tls_align = 16;
  __harvey_text_start = ADDR(.text);
  __harvey_text_size = SIZEOF(.text);
  __harvey_eh_frame_hdr = ADDR(.eh_frame_hdr);
  __harvey_eh_frame_hdr_size = SIZEOF(.eh_frame_hdr);
  /DISCARD/ : { *(.comment) *(.note*) }
}
