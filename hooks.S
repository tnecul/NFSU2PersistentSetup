# NFSU2PersistentSetup — naked thunk stubs. SPDX-License-Identifier: CC0-1.0
# Called via E9 jmp patched into the game function prologs.
# Stack state at entry is identical to the original function's prolog entry,
# because the patch replaces a JMP in place (not a CALL).
#
# i386 MinGW COFF: C symbols carry a leading underscore.

.intel_syntax noprefix
.text

# ---------------------------------------------------------------------------
# HookSave: replaces FUN_0050FC20 prolog.
#   ECX        = setup manager `this` (untouched here)
#   [esp+4]    = slot argument (after pusha: [esp+36])
# Calls CaptureCommit(this, edxJunk, slot) then jumps to the trampoline,
# which re-runs the displaced prolog (cmp/jz) and falls back into the
# original body at 0x50FC25. The original function ends with RET 0x4.
# ---------------------------------------------------------------------------
.globl _HookSave
_HookSave:
    pusha
    mov     eax, dword ptr [esp+36]     # slot argument
    push    eax
    call    _CaptureCommit              # __fastcall: ECX=this, EDX=junk, slot on stack
    popa
    jmp     dword ptr [_g_trampSave]

# ---------------------------------------------------------------------------
# HookEntry: replaces FUN_00516AC0 prolog.
#   ECX        = source object `this` (untouched here)
#   [esp+4]    = param_1                    (after pusha: [esp+36])
#   [esp+8]    = dst car object             (after pusha: [esp+40])
#   [esp+0xC]  = setup mode (0..4)          (after pusha: [esp+44])
# Calls PreEntryPatch(this, edxJunk, dstCar, mode) then jumps to the
# trampoline, which re-runs the displaced 7-byte prolog (mov/sub esp) and
# falls back into the original body at 0x516AC7. Ends with RET 0xC.
# ---------------------------------------------------------------------------
.globl _HookEntry
_HookEntry:
    pusha
    mov     eax, dword ptr [esp+40]     # dst car object
    mov     edx, dword ptr [esp+44]     # setup mode (also lands in EDX as arg2, unused)
    push    edx                          # push order: mode first, dst second
    push    eax                          # -> fastcall stack args: dstCar, mode
    call    _PreEntryPatch               # __fastcall: ECX=this, EDX=junk, stack args
    popa
    jmp     dword ptr [_g_trampEntry]
