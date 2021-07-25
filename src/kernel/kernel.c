#define SSFN_CONSOLEBITMAP_TRUECOLOR
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stivale/stivale2.h>
#include <kernel/kernel.h>
#include <ssfn.h>
#include <kernel/io.h>
#include <kernel/idt.h>
#include <kernel/isr.h>
#include <kernel/gdt.h>
#include <kernel/pic.h>
#include <kernel/pit.h>
#include <kernel/rtc.h>
#include <kernel/apic.h>
#include <kernel/panic.h>
#include <kernel/serial.h>
#include <kernel/keyboard.h>
 
// We need to tell the stivale bootloader where we want our stack to be.
// We are going to allocate our stack as an uninitialised array in .bss.
static uint8_t stack[4096];
extern void kmain(struct stivale2_struct *stivale2_struct);
extern unsigned char _binary_u_vga16_sfn_start;
 
// stivale2 uses a linked list of tags for both communicating TO the
// bootloader, or receiving info FROM it. More information about these tags
// is found in the stivale2 specification.

// stivale2 offers a runtime terminal service which can be ditched at any
// time, but it provides an easy way to print out to graphical terminal,
// especially during early boot.
// Read the notes about the requirements for using this feature below this
// code block.
static struct stivale2_header_tag_terminal terminal_hdr_tag = {
    // All tags need to begin with an identifier and a pointer to the next tag.
    .tag = {
        // Identification constant defined in stivale2.h and the specification.
        .identifier = STIVALE2_HEADER_TAG_TERMINAL_ID,
        // If next is 0, it marks the end of the linked list of header tags.
        .next = 0
    },
    // The terminal header tag possesses a flags field, leave it as 0 for now
    // as it is unused.
    .flags = 0
};

struct stivale2_header_tag_smp smp_hdr_tag = {
    .tag = {
        .identifier = STIVALE2_HEADER_TAG_SMP_ID,
        .next       = (uint64_t)&terminal_hdr_tag
    },
    .flags = 0
};
 
// We are now going to define a framebuffer header tag, which is mandatory when
// using the stivale2 terminal.
// This tag tells the bootloader that we want a graphical framebuffer instead
// of a CGA-compatible text mode. Omitting this tag will make the bootloader
// default to text mode, if available.
static struct stivale2_header_tag_framebuffer framebuffer_hdr_tag = {
    // Same as above.
    .tag = {
        .identifier = STIVALE2_HEADER_TAG_FRAMEBUFFER_ID,
        // Instead of 0, we now point to the previous header tag. The order in
        // which header tags are linked does not matter.0
        .next = (uint64_t)&smp_hdr_tag
    },
    // We set all the framebuffer specifics to 0 as we want the bootloader
    // to pick the best it can.
    .framebuffer_width  = 0,
    .framebuffer_height = 0,
    .framebuffer_bpp    = 0
};

// The stivale2 specification says we need to define a "header structure".
// This structure needs to reside in the .stivale2hdr ELF section in order
// for the bootloader to find it. We use this __attribute__ directive to
// tell the compiler to put the following structure in said section.
__attribute__((section(".stivale2hdr"), used))
static struct stivale2_header stivale_hdr = {
    // The entry_point member is used to specify an alternative entry
    // point that the bootloader should jump to instead of the executable's
    // ELF entry point. We do not care about that so we leave it zeroed.
    .entry_point = 0,
    // Let's tell the bootloader where our stack is.
    // We need to add the sizeof(stack) since in x86(_64) the stack grows
    // downwards.
    .stack = (uintptr_t)stack + sizeof(stack),
    // Bit 1, if set, causes the bootloader to return to us pointers in the
    // higher half, which we likely want.
    // Bit 2, if set, tells the bootloader to enable protected memory ranges,
    // that is, to respect the ELF PHDR mandated permissions for the executable's
    // segments.
    .flags = (1 << 1) | (1 << 2),
    // This header structure is the root of the linked list of header tags and
    // points to the first one in the linked list.
    .tags = (uintptr_t)&framebuffer_hdr_tag
};
 
// We will now write a helper function which will allow us to scan for tags
// that we want FROM the bootloader (structure tags).
void *stivale2_get_tag(struct stivale2_struct *stivale2_struct, uint64_t id) {
    struct stivale2_tag *current_tag = (void *)stivale2_struct->tags;
    for (;;) {
        // If the tag pointer is NULL (end of linked list), we did not find
        // the tag. Return NULL to signal this.
        if (current_tag == NULL) {
            return NULL;
        }
 
        // Check whether the identifier matches. If it does, return a pointer
        // to the matching tag.
        if (current_tag->identifier == id) {
            return current_tag;
        }
 
        // Get a pointer to the next tag in the linked list and repeat.
        current_tag = (void *)current_tag->next;
    }
}

void (*term_write)(const char *string, size_t length);
uint8_t *fb_addr;

// The following will be our kernel's entry point.
void _start(struct stivale2_struct *stivale2_struct) { 	
    // Let's get the terminal structure tag from the bootloader.
    struct stivale2_struct_tag_terminal *term_str_tag;
    struct stivale2_struct_tag_framebuffer *fb_str_tag;
    term_str_tag = stivale2_get_tag(stivale2_struct, STIVALE2_STRUCT_TAG_TERMINAL_ID);
    fb_str_tag = stivale2_get_tag(stivale2_struct, STIVALE2_STRUCT_TAG_FRAMEBUFFER_ID);
 
    // Check if the tag was actually found.
    if (term_str_tag == NULL) {
        // It wasn't found, just hang...
        for (;;) {
            asm ("hlt");
        }
    }
 
    // Let's get the address of the terminal write function.
    void *term_write_ptr = (void *)term_str_tag->term_write;
 
    // Now, let's assign this pointer to a function pointer which
    // matches the prototype described in the stivale2 specification for
    // the stivale2_term_write function.
    term_write = term_write_ptr;
    fb_addr = (uint8_t *)fb_str_tag->framebuffer_addr;
    
    // We should now be able to call the above function pointer to print out
    // a simple "Hello World" to screen.
    //printf("Hello World");
 
    // We're done, just hang...
    //for (;;) {
    //    asm ("hlt");
    //}

    /* set up context by global variables */
    ssfn_src = &_binary_u_vga16_sfn_start;      /* the bitmap font to use */
    ssfn_dst.ptr = fb_addr;                     /* framebuffer address and bytes per line */
    ssfn_dst.p = 4096;
    ssfn_dst.fg = 0xFFFFFFFF;                   /* colors, white on black */
    ssfn_dst.bg = 0;
    ssfn_dst.x = 64;                            /* coordinates to draw to */
    ssfn_dst.y = 64;
    
    kmain(stivale2_struct);
}

void kmain(struct stivale2_struct *stivale2_struct) {
	init_serial();
	printf("\033[44m   __                  \033[0m          \n\033[44m");
	printf("  / _| __ _ _ __ _   _ \033[0m ___  ___ \n\033[44m");
	printf(" | |_ / _` | '__| | | |\033[0m/ _ \\/ __|\n\033[44m");
	printf(" |  _| (_| | |  | |_| |\033[0m (_) \\__ \\\n\033[44m");
	printf(" |_|  \\__,_|_|   \\__,_|\033[0m\\___/|___/\n\033[44m");
	printf("                       \033[0m                    \n");
	printf("version %s\n", FARUOS_VERSION);
	printf("Copyright (C) 2021 Leap of Azzam\n\n");
	printf("info: Bootloader: %s %s\n", stivale2_struct->bootloader_brand, stivale2_struct->bootloader_version);
	printf("kernel: Initializing GDT...");
	init_gdt();
	printf(" [ \033[32mOK \033[0m]\n");
	printf("kernel: Initializing PIC...");
	init_pic();
	printf(" [ \033[32mOK \033[0m]\n");
	printf("kernel: Initializing APIC...");
	enable_apic();
	printf(" [ \033[32mOK \033[0m]\n");
	printf("kernel: Initializing PIT...");
	init_pit(1000);
	printf(" [ \033[32mOK \033[0m]\n");
	printf("kernel: Initializing interrupts...");
	isr_install();
	printf(" [ \033[32mOK \033[0m]\n");
	printf("\n");
	printf("Welcome to FaruOS!\n");
	
	while (true) {
		printf(get_key());
	}
	//for (;;) {
	//	asm ("hlt");
	//}
}
