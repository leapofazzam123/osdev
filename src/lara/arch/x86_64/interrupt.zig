const std = @import("std");
const root = @import("root");
const arch = @import("../x86_64.zig");
const log = std.log.scoped(.interrupt);

pub const InterruptFrame = extern struct {
    rax: u64,
    rbx: u64,
    rcx: u64,
    rdx: u64,
    rsi: u64,
    rdi: u64,
    rbp: u64,
    r8: u64,
    r9: u64,
    r10: u64,
    r11: u64,
    r12: u64,
    r13: u64,
    r14: u64,
    r15: u64,
    vec: u64,
    error_code: u64,
    rip: u64,
    cs: u64,
    rflags: u64,
    rsp: u64,
    ss: u64,

    pub fn dump(self: InterruptFrame, log_func: anytype) void {
        log_func("RAX: {X:0>16} - RBX: {X:0>16} - RCX: {X:0>16}", .{ self.rax, self.rbx, self.rcx });
        log_func("RDX: {X:0>16} - RDI: {X:0>16} - RSI: {X:0>16}", .{ self.rdx, self.rdi, self.rsi });
        log_func("RBP: {X:0>16} - R8:  {X:0>16} - R9:  {X:0>16}", .{ self.rbp, self.r8, self.r9 });
        log_func("R10: {X:0>16} - R11: {X:0>16} - R12: {X:0>16}", .{ self.r10, self.r11, self.r12 });
        log_func("R13: {X:0>16} - R14: {X:0>16} - R15: {X:0>16}", .{ self.r13, self.r14, self.r15 });
        log_func("RSP: {X:0>16} - RIP: {X:0>16} - CS:  {X:0>16}", .{ self.rsp, self.rip, self.cs });

        var cr2 = asm volatile ("mov %%cr2, %[out]"
            : [out] "=r" (-> u64),
            :
            : "memory"
        );
        log_func("Linear address: 0x{X:0>16}, EC bits: 0x{X:0>8}", .{ cr2, self.error_code });
    }
};

const Entry = packed struct {
    offset_low: u16,
    selector: u16,
    ist: u8,
    flags: u8,
    offset_mid: u16,
    offset_high: u32,
    reserved: u32 = 0,

    fn fromPtr(ptr: u64, ist: u8) Entry {
        return Entry{
            .offset_low = @truncate(u16, ptr),
            .selector = 0x28,
            .ist = ist,
            .flags = 0x8e,
            .offset_mid = @truncate(u16, ptr >> 16),
            .offset_high = @truncate(u32, ptr >> 32),
        };
    }
};

const InterruptStub = *const fn () callconv(.Naked) void;
const InterruptHandler = *const fn (*InterruptFrame) callconv(.C) void;
var entries: [256]Entry = undefined;

export var handlers: [256]InterruptHandler = [_]InterruptHandler{handleException} ** 32 ++ [_]InterruptHandler{handleIrq} ** 224;

pub fn setHandler(func: anytype, vec: u8) void {
    handlers[vec] = func;
}

pub fn load() void {
    const idtr = arch.Descriptor{
        .size = @as(u16, (@sizeOf(Entry) * 256) - 1),
        .ptr = @ptrToInt(&entries),
    };

    asm volatile ("lidt %[idtr]"
        :
        : [idtr] "*p" (&idtr),
    );
}

pub fn init() void {
    for (genStubTable()) |stub, idx| {
        entries[idx] = Entry.fromPtr(@as(u64, @ptrToInt(stub)), 0);
    }

    load();
}

fn handleIrq(frame: *InterruptFrame) callconv(.C) void {
    log.err("CPU triggered IRQ #{}, which has no handler!", .{frame.vec});
    @panic("Unhandled IRQ");
}

fn handleException(frame: *InterruptFrame) callconv(.C) void {
    log.err("CPU exception #{}: ", .{frame.vec});
    frame.dump(log.err);

    arch.halt();
}

fn genStubTable() [256]InterruptStub {
    var result = [1]InterruptStub{undefined} ** 256;

    comptime var i: usize = 0;

    inline while (i < 256) : (i += 1) {
        result[i] = comptime makeStub(i);
    }

    return result;
}

fn makeStub(comptime vec: u8) InterruptStub {
    return struct {
        fn stub() callconv(.Naked) void {
            const has_ec = switch (vec) {
                0x8 => true,
                0xA...0xE => true,
                0x11 => true,
                0x15 => true,
                0x1D...0x1E => true,
                else => false,
            };

            if (!comptime (has_ec)) {
                asm volatile ("push $0");
            }

            asm volatile ("push %[vec]"
                :
                : [vec] "i" (vec),
            );

            // zig fmt: off
            asm volatile (
                // perform a swapgs (if we came from usermode)
                \\cmpq $0x3b, 16(%rsp)
                \\jne 1f
                \\swapgs

                // push the trapframe
                \\1:
                \\push %r15
                \\push %r14
                \\push %r13
                \\push %r12
                \\push %r11
                \\push %r10
                \\push %r9
                \\push %r8
                \\push %rbp
                \\push %rdi
                \\push %rsi
                \\push %rdx
                \\push %rcx
                \\push %rbx
                \\push %rax
                \\cld

                // setup C enviroment and index into the handler
                \\lea handlers(%rip), %rbx
                \\add %[vec_off], %rbx
                \\mov %rsp, %rdi
                \\xor %rbp, %rbp
                \\call *(%rbx)

                // pop the trapframe back into place
                \\pop %rax
                \\pop %rbx
                \\pop %rcx
                \\pop %rdx
                \\pop %rsi
                \\pop %rdi
                \\pop %rbp
                \\pop %r8
                \\pop %r9
                \\pop %r10
                \\pop %r11
                \\pop %r12
                \\pop %r13
                \\pop %r14
                \\pop %r15
                \\add $16, %rsp

                // swap back to user gs (if needed)
                \\cmpq $0x3b, 8(%rsp)
                \\jne 1f
                \\swapgs

                // and away we go :-)
                \\1:
                \\iretq
                :
                : [vec_off] "i" (@as(u64, vec) * 8),
            );
        }
    }.stub;
    // zig fmt: on
}