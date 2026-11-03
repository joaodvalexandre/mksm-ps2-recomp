#!/usr/bin/env python3
"""Disassembler for the game's IRX modules (IOP, MIPS I / R3000A), standard library only.

usage: python -I tools/iop/irx_disasm.py <module.irx> [--base 0x...] [-o out.txt]

The module (ELF32 little-endian, e_type 0xFF80 = relocatable IOP executable) is loaded as loadcore does: the single
PT_LOAD segment at the load base given with --base (0 by default), the SHT_REL sections applied against that base
(R_MIPS_32, R_MIPS_26 and R_MIPS_HI16/R_MIPS_LO16 pairs, the offsets are image-absolute and every symbol is 0) and
the .iopmod section (sh_type 0x70000080) read for the module name, version and entry.

The output has a header (sections, module info, import stubs, export tables, RPC servers and SIF command handlers
found by a heuristic), the .text disassembly, one line per word, and a hex dump of the data sections:

    0001af18: 0c006c6c  jal     0x1b1b0            ; libsd:17 sceSdVoiceTrans

Annotations: a label at every branch target (loc_<addr>) and jal target (func_<addr>, or the export name), the
library:ordinal name of a call to an import stub, and the 32-bit value of a lui/addiu, lui/ori or lui/load-store pair
("; = 0x1b2910"). Names come from a built-in table of the common IOP libraries (ps2sdk numbering, cross-checked with
the runtime's import handlers); an ordinal the table is not sure about ends in '?', an unknown one prints as
lib:ordinal. The heuristic pass tracks the constants the instructions put in the registers (lui, li, lui+addiu/ori,
move, addiu from $sp, lw of a .data word) and in the stack slots, and reads a0..a3 and the stack arguments
(0x10($sp)...) at each call of sceSifRegisterRpc, sceSifSetRpcQueue, sceSifRpcLoop, sceSifAddCmdHandler,
RegisterIntrHandler, the libsd interrupt handler setters and CreateThread (REPORTED_CALLS): the header lists the RPC
servers (SID, handler), the CMD handlers, the interrupt handlers and the thread entries, with '?' where a value is not
a constant the pass can see.
"""
from __future__ import annotations

import argparse
import struct
import sys

ET_SCE_IOPRELEXEC = (0xFF80, 0xFF81)
SHT_SYMTAB, SHT_REL, SHT_RELA, SHT_NOBITS = 2, 9, 4, 8
SHT_SCE_IOPMOD = 0x70000080
R_MIPS_NONE, R_MIPS_16, R_MIPS_32, R_MIPS_REL32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 0, 1, 2, 3, 4, 5, 6
IMPORT_MAGIC, EXPORT_MAGIC = 0x41E00000, 0x41C00000
JR_RA = 0x03E00008

REGS = ['$zero', '$at', '$v0', '$v1', '$a0', '$a1', '$a2', '$a3', '$t0', '$t1', '$t2', '$t3', '$t4', '$t5', '$t6',
        '$t7', '$s0', '$s1', '$s2', '$s3', '$s4', '$s5', '$s6', '$s7', '$t8', '$t9', '$k0', '$k1', '$gp', '$sp', '$fp',
        '$ra']
COP0 = {0: 'Index', 1: 'Random', 2: 'EntryLo', 3: 'BPC', 4: 'Context', 5: 'BDA', 6: 'PIDMask', 7: 'DCIC', 8: 'BadVaddr',
        9: 'BDAM', 10: 'EntryHi', 11: 'BPCM', 12: 'SR', 13: 'Cause', 14: 'EPC', 15: 'PRId'}

# Export ordinals of the IOP libraries the game's modules import (ps2sdk headers; the sifcmd, sifman, thbase, thevent,
# thsemap, stdio, sysclib, timrman, loadcore, intrman and cdvdman numbers also match the runtime's handlers in
# ps2xIOP/src/emulator). A name ending in '?' is a best guess. 0..3 are the same in every library.
COMMON_ORDINALS = {0: 'start', 1: 'reinit?', 2: 'shutdown', 3: '_retonly'}
LIBRARIES = {
    'loadcore': {3: 'GetLibraryEntryTable', 4: 'FlushIcache', 5: 'FlushDcache', 6: 'RegisterLibraryEntries',
                 7: 'ReleaseLibraryEntries', 8: 'findFixImports', 9: 'restoreImports', 10: 'RegisterNonAutoLinkEntries',
                 11: 'QueryLibraryEntryTable', 12: 'QueryBootMode', 13: 'RegisterBootMode', 14: 'setFlag',
                 15: 'resetFlag', 16: 'linkModule', 17: 'unlinkModule', 20: 'registerFunc?', 21: 'setModuleInfo?',
                 24: 'ReadModuleHeader', 25: 'LoadModuleImage', 26: 'linkImports?',
                 27: 'SetRebootTimeLibraryHandlingMode'},
    'intrman': {4: 'RegisterIntrHandler', 5: 'ReleaseIntrHandler', 6: 'EnableIntr', 7: 'DisableIntr',
                8: 'CpuDisableIntr', 9: 'CpuEnableIntr', 14: 'CpuInvokeInKmode', 15: 'DisableDispatch',
                16: 'EnableDispatch', 17: 'CpuSuspendIntr', 18: 'CpuResumeIntr', 23: 'QueryIntrContext',
                24: 'QueryIntrStack', 25: 'iCatchMultiIntr', 28: 'SetCtxSwitchHandler?', 29: 'ResetCtxSwitchHandler?',
                30: 'SetCtxSwitchReqHandler?', 31: 'ResetCtxSwitchReqHandler?'},
    'thbase': {4: 'CreateThread', 5: 'DeleteThread', 6: 'StartThread', 7: 'StartThreadArgs', 8: 'ExitThread',
               9: 'ExitDeleteThread', 10: 'TerminateThread', 11: 'iTerminateThread', 12: 'DisableDispatchThread',
               13: 'EnableDispatchThread', 14: 'ChangeThreadPriority', 15: 'iChangeThreadPriority',
               16: 'RotateThreadReadyQueue', 17: 'iRotateThreadReadyQueue', 18: 'ReleaseWaitThread',
               19: 'iReleaseWaitThread', 20: 'GetThreadId', 21: 'CheckThreadStack', 22: 'ReferThreadStatus',
               23: 'iReferThreadStatus', 24: 'SleepThread', 25: 'WakeupThread', 26: 'iWakeupThread',
               27: 'CancelWakeupThread', 28: 'iCancelWakeupThread', 29: 'SuspendThread', 30: 'iSuspendThread',
               31: 'ResumeThread', 32: 'iResumeThread', 33: 'DelayThread', 34: 'GetSystemTime', 35: 'SetAlarm',
               36: 'iSetAlarm', 37: 'CancelAlarm', 38: 'iCancelAlarm', 39: 'USec2SysClock', 40: 'SysClock2USec',
               41: 'GetSystemStatusFlag'},
    'thevent': {4: 'CreateEventFlag', 5: 'DeleteEventFlag', 6: 'SetEventFlag', 7: 'iSetEventFlag', 8: 'ClearEventFlag',
                9: 'iClearEventFlag', 10: 'WaitEventFlag', 11: 'PollEventFlag', 13: 'ReferEventFlagStatus',
                14: 'iReferEventFlagStatus'},
    'thsemap': {4: 'CreateSema', 5: 'DeleteSema', 6: 'SignalSema', 7: 'iSignalSema', 8: 'WaitSema', 9: 'PollSema',
                11: 'ReferSemaStatus', 12: 'iReferSemaStatus'},
    'thmsgbx': {4: 'CreateMbx', 5: 'DeleteMbx', 6: 'SendMbx', 7: 'iSendMbx', 8: 'ReceiveMbx', 9: 'PollMbx',
                11: 'ReferMbxStatus', 12: 'iReferMbxStatus'},
    'thfpool': {4: 'CreateFpl', 5: 'DeleteFpl', 6: 'AllocateFpl', 7: 'pAllocateFpl', 8: 'ipAllocateFpl', 9: 'FreeFpl',
                11: 'ReferFplStatus', 12: 'iReferFplStatus'},
    'thvpool': {4: 'CreateVpl', 5: 'DeleteVpl', 6: 'AllocateVpl', 7: 'pAllocateVpl', 8: 'ipAllocateVpl', 9: 'FreeVpl',
                11: 'ReferVplStatus', 12: 'iReferVplStatus'},
    'sifman': {4: 'sceSifDma2Init', 5: 'sceSifInit', 6: 'sceSifSetDChain', 7: 'sceSifSetDma', 8: 'sceSifDmaStat',
               9: 'sceSifSend', 10: 'sceSifSendSync', 11: 'sceSifIsSending', 20: 'sceSifGetMSFlag?',
               21: 'sceSifSetMSFlag?', 22: 'sceSifGetSMFlag?', 23: 'sceSifSetSMFlag?', 24: 'sceSifGetMainAddr?',
               25: 'sceSifGetSubAddr?', 26: 'sceSifSetSubAddr?', 27: 'sceSifIntrMain?', 28: 'sceSifIntrSub?',
               29: 'sceSifCheckInit', 32: 'sceSifSetDmaIntr', 33: 'sceSifDmaStatIntr?', 34: 'sceSifSetDmaIntrHandler?',
               35: 'sceSifResetDmaIntrHandler?', 36: 'sceSifSetOneDma?'},
    'sifcmd': {4: 'sceSifInitCmd', 5: 'sceSifExitCmd', 6: 'sceSifGetSreg', 7: 'sceSifSetSreg', 8: 'sceSifSetCmdBuffer',
               9: 'sceSifSetSysCmdBuffer', 10: 'sceSifAddCmdHandler', 11: 'sceSifRemoveCmdHandler', 12: 'sceSifSendCmd',
               13: 'isceSifSendCmd', 14: 'sceSifInitRpc', 15: 'sceSifBindRpc', 16: 'sceSifCallRpc',
               17: 'sceSifRegisterRpc', 18: 'sceSifCheckStatRpc', 19: 'sceSifSetRpcQueue', 20: 'sceSifGetNextRequest',
               21: 'sceSifExecRequest', 22: 'sceSifRpcLoop', 23: 'sceSifGetOtherData', 24: 'sceSifRemoveRpc',
               25: 'sceSifRemoveRpcQueue', 26: 'sceSifSetSif1CB', 27: 'sceSifClearSif1CB', 28: 'sceSifSendCmdIntr',
               29: 'isceSifSendCmdIntr'},
    'sysclib': {4: 'setjmp', 5: 'longjmp', 6: 'toupper', 7: 'tolower', 8: 'look_ctype_table', 9: 'get_ctype_table',
                10: 'memchr', 11: 'memcmp', 12: 'memcpy', 13: 'memmove', 14: 'memset', 15: 'bcmp', 16: 'bcopy',
                17: 'bzero', 18: 'prnt', 19: 'sprintf', 20: 'strcat', 21: 'strchr', 22: 'strcmp', 23: 'strcpy',
                24: 'strcspn', 25: 'index', 26: 'rindex', 27: 'strlen', 28: 'strncat', 29: 'strncmp', 30: 'strncpy',
                31: 'strpbrk', 32: 'strrchr', 33: 'strspn', 34: 'strstr', 35: 'strtok', 36: 'strtol', 37: 'atob',
                38: 'strtoul', 40: 'wmemcopy', 41: 'wmemset', 42: 'vsprintf', 43: 'strtok_r'},
    'sysmem': {4: 'AllocSysMemory', 5: 'FreeSysMemory', 6: 'QueryMemSize', 7: 'QueryMaxFreeMemSize',
               8: 'QueryTotalFreeMemSize', 9: 'QueryBlockTopAddress', 10: 'QueryBlockSize', 14: 'Kprintf',
               15: 'SetKprintf?'},
    'stdio': {4: 'printf', 5: 'getchar', 6: 'putchar', 7: 'puts', 8: 'gets', 9: 'fdprintf', 10: 'fdgetc', 11: 'fdputc',
              12: 'fdputs', 13: 'fdgets', 14: 'vfdprintf'},
    'dmacman': {4: 'dmac_ch_set_madr', 5: 'dmac_ch_get_madr', 6: 'dmac_ch_set_bcr', 7: 'dmac_ch_get_bcr',
                8: 'dmac_ch_set_chcr', 9: 'dmac_ch_get_chcr', 10: 'dmac_ch_set_tadr', 11: 'dmac_ch_get_tadr',
                12: 'dmac_set_dpcr', 13: 'dmac_get_dpcr', 14: 'dmac_set_dpcr2', 15: 'dmac_get_dpcr2',
                16: 'dmac_set_dpcr3', 17: 'dmac_get_dpcr3', 18: 'dmac_set_dicr', 19: 'dmac_get_dicr',
                20: 'dmac_set_dicr2', 21: 'dmac_get_dicr2', 28: 'dmac_request', 32: 'dmac_transfer?',
                33: 'dmac_ch_set_dpcr?', 34: 'dmac_enable?', 35: 'dmac_disable?'},
    'timrman': {3: 'GetTimersTable', 4: 'AllocHardTimer', 5: 'ReferHardTimer', 6: 'FreeHardTimer', 7: 'SetTimerMode',
                8: 'GetTimerStatus', 9: 'SetTimerCounter', 10: 'GetTimerCounter', 11: 'SetTimerCompare',
                12: 'GetTimerCompare', 13: 'SetHoldMode', 14: 'GetHoldMode', 15: 'GetHoldReg',
                16: 'GetHardTimerIntrCode', 17: 'GetTimerMode?', 18: 'GetTimerReadFunc?', 20: 'SetTimerHandler',
                21: 'SetOverflowHandler', 22: 'SetupHardTimer', 23: 'StartHardTimer', 24: 'StopHardTimer'},
    'modload': {4: 'ReBootStart', 5: 'LoadModuleAddress', 6: 'LoadModule', 7: 'LoadStartModule', 8: 'StartModule',
                9: 'LoadModuleBufferAddress', 10: 'LoadModuleBuffer', 11: 'LoadStartKelfModule?',
                12: 'SetSecrmanCallbacks?', 13: 'SetCheckKelfPathCallback', 14: 'GetLoadcoreInternalData?',
                16: 'GetModuleIdList?', 17: 'ReferModuleStatus?', 18: 'GetModuleIdListByName?',
                19: 'LoadModuleWithOption?', 20: 'StopModule?', 21: 'UnloadModule?', 22: 'SearchModuleByName?',
                23: 'SearchModuleByAddress?'},
    'cdvdman': {4: 'sceCdInit', 5: 'sceCdStandby', 6: 'sceCdRead', 7: 'sceCdSeek', 8: 'sceCdGetError', 9: 'sceCdGetToc',
                10: 'sceCdSearchFile', 11: 'sceCdSync', 12: 'sceCdGetDiskType', 13: 'sceCdDiskReady', 14: 'sceCdTrayReq',
                15: 'sceCdStop', 16: 'sceCdPosToInt', 17: 'sceCdIntToPos', 18: 'sceCdGetToc2?', 20: 'sceCdCheckCmd?',
                21: '_sceCdRI?', 24: 'sceCdReadClock', 25: 'sceCdWriteClock', 26: 'sceCdReadNVM', 27: 'sceCdWriteNVM',
                28: 'sceCdStatus', 29: 'sceCdApplySCmd', 30: 'sceCdSetHDMode', 31: 'sceCdOpenConfig',
                32: 'sceCdCloseConfig', 33: 'sceCdReadConfig', 34: 'sceCdWriteConfig', 35: 'sceCdReadKey',
                36: 'sceCdDecSet', 37: 'sceCdCallback', 38: 'sceCdPause', 39: 'sceCdBreak', 40: 'sceCdReadCdda?',
                41: 'sceCdReadConsoleID?', 42: 'sceCdWriteConsoleID?', 43: 'sceCdMV?', 44: 'sceCdCtrlADout?',
                45: 'sceCdNop?', 46: 'sceGetFsvRbuf?', 47: 'sceCdstm0Cb?', 48: 'sceCdstm1Cb?', 50: 'sceCdSC',
                51: 'sceCdRC?', 52: 'sceCdForbidDVDP?', 53: 'sceCdReadSUBQ?', 54: 'sceCdApplyNCmd?',
                55: 'sceCdAutoAdjustCtrl?', 56: 'sceCdStInit?', 57: 'sceCdStStart?', 58: 'sceCdStStat?',
                59: 'sceCdStStop?', 60: 'sceCdStRead?', 61: 'sceCdStSeek?', 62: 'sceCdStPause?', 63: 'sceCdStResume?',
                64: 'sceCdStSeekF?', 65: 'sceCdPOffCallback?', 66: 'sceCdReadChain?', 75: 'sceCdMmode'},
    # libsd: the names the runtime's native handlers use (patches/ps2recomp-iop-libsd-imports.patch); ps2sdk calls 31
    # sceSdCleanEffectWorkArea.
    'libsd': {2: 'sceSdQuit', 4: 'sceSdInit', 5: 'sceSdSetParam', 6: 'sceSdGetParam', 7: 'sceSdSetSwitch',
              8: 'sceSdGetSwitch', 9: 'sceSdSetAddr', 10: 'sceSdGetAddr', 11: 'sceSdSetCoreAttr', 12: 'sceSdGetCoreAttr',
              13: 'sceSdNote2Pitch', 14: 'sceSdPitch2Note', 15: 'sceSdProcBatch', 16: 'sceSdProcBatchEx',
              17: 'sceSdVoiceTrans', 18: 'sceSdBlockTrans', 19: 'sceSdVoiceTransStatus', 20: 'sceSdBlockTransStatus',
              21: 'sceSdSetTransCallback', 22: 'sceSdSetIRQCallback', 23: 'sceSdSetEffectAttr', 24: 'sceSdGetEffectAttr',
              25: 'sceSdClearEffectWorkArea', 26: 'sceSdSetTransIntrHandler', 27: 'sceSdSetSpu2IntrHandler',
              28: 'sceSdGetTransIntrHandlerArgument', 29: 'sceSdGetSpu2IntrHandlerArgument', 30: 'sceSdStopTrans',
              31: 'sceSdSetEffectWorkArea?', 32: 'sceSdSetEffectMode?', 33: 'sceSdSetEffectModeParams?'},
    'vblank': {4: 'WaitVblankStart', 5: 'WaitVblankEnd', 6: 'WaitVblank', 7: 'WaitNonVblank',
               8: 'RegisterVblankHandler', 9: 'ReleaseVblankHandler'},
    'heaplib': {4: 'CreateHeap', 5: 'DeleteHeap', 6: 'AllocHeapMemory', 7: 'FreeHeapMemory', 8: 'HeapTotalFreeSize',
                11: 'HeapSize?', 15: 'HeapAllocatedSize?'},
    'loadfile': {4: 'LoadModuleAddress?', 5: 'LoadModule?', 6: 'LoadStartModule?', 7: 'StartModule?',
                 8: 'LoadModuleBufferAddress?', 9: 'LoadModuleBuffer?', 10: 'GetModuleIdList?', 11: 'ReferModuleStatus?'},
    'ioman': {4: 'open', 5: 'close', 6: 'read', 7: 'write', 8: 'lseek', 9: 'ioctl', 10: 'remove', 11: 'mkdir',
              12: 'rmdir', 13: 'dopen', 14: 'dclose', 15: 'dread', 16: 'getstat', 17: 'chstat', 18: 'format',
              20: 'AddDrv', 21: 'DelDrv'},
    'iomanx': {4: 'open', 5: 'close', 6: 'read', 7: 'write', 8: 'lseek', 9: 'ioctl', 10: 'remove', 11: 'mkdir',
               12: 'rmdir', 13: 'dopen', 14: 'dclose', 15: 'dread', 16: 'getstat', 17: 'chstat', 18: 'format',
               20: 'AddDrv', 21: 'DelDrv', 25: 'rename?', 26: 'chdir?', 27: 'sync?', 28: 'mount?', 29: 'umount?',
               30: 'lseek64?', 31: 'devctl?', 32: 'symlink?', 33: 'readlink?', 34: 'ioctl2?'},
    'secrman': {4: 'SetMcCommandHandler', 5: 'SetMcDevIDHandler', 6: 'SecrAuthCard', 7: 'SecrResetAuthCard',
                8: 'SecrCardBootHeader?', 9: 'SecrCardBootBlock?', 10: 'SecrCardBootFile?', 11: 'SecrDiskBootHeader?',
                12: 'SecrDiskBootBlock?', 13: 'SecrDiskBootFile?'},
    'excepman': {4: 'RegisterExceptionHandler', 5: 'RegisterPriorityExceptionHandler',
                 6: 'RegisterDefaultExceptionHandler', 7: 'ReleaseExceptionHandler',
                 8: 'ReleaseDefaultExceptionHandler'},
    'sio2man': {4: 'sio2_ctrl_set', 5: 'sio2_ctrl_get', 6: 'sio2_stat6c_get', 7: 'sio2_portN_ctrl1_set',
                8: 'sio2_portN_ctrl1_get', 9: 'sio2_portN_ctrl2_set', 10: 'sio2_portN_ctrl2_get', 11: 'sio2_stat70_get',
                12: 'sio2_regdata_set?', 13: 'sio2_regdata_get?', 14: 'sio2_stat74_get', 15: 'sio2_unkn78_set?',
                16: 'sio2_unkn78_get?', 17: 'sio2_unkn7c_set?', 18: 'sio2_unkn7c_get?', 19: 'sio2_data_out',
                20: 'sio2_data_in', 21: 'sio2_stat_set', 22: 'sio2_stat_get', 23: 'sio2_pad_transfer_init',
                24: 'sio2_mc_transfer_init', 25: 'sio2_transfer', 26: 'sio2_transfer_reset?'},
    'mcman': {4: 'McDetectCard', 5: 'McOpen', 6: 'McClose', 7: 'McRead', 8: 'McWrite', 9: 'McSeek', 10: 'McFormat',
              11: 'McGetDir', 12: 'McDelete', 13: 'McFlush', 14: 'McChDir', 15: 'McSetFileInfo', 16: 'McEraseBlock',
              17: 'McReadPage', 18: 'McWritePage', 19: 'McDataChecksum', 20: 'McDetectCard2?', 21: 'McGetFormat?',
              22: 'McGetEntSpace?', 23: 'McReplaceBadBlock?', 24: 'McCheckBlock?', 25: 'McReadPS1PDACard?',
              26: 'McWritePS1PDACard?', 27: 'McUnformat?', 28: 'McRetOnly?', 29: 'McGetFreeClusters?',
              30: 'McGetMcType?', 31: 'McSetPS1CardFlag?'},
}


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def ordinal_name(library, ordinal):
    """'lib:ordinal name' (the name is left out when the table does not know it)"""
    name = LIBRARIES.get(library.lower(), {}).get(ordinal) or COMMON_ORDINALS.get(ordinal)
    return f'{library}:{ordinal}' + (f' {name}' if name else '')


class ImportTable:
    def __init__(self, address, library, version, stubs):
        self.address, self.library, self.version, self.stubs = address, library, version, stubs  # stubs: addr -> ordinal


class ExportTable:
    def __init__(self, address, library, version, functions):
        self.address, self.library, self.version, self.functions = address, library, version, functions  # [addr]


class IrxModule:
    """An IRX loaded and relocated at `base`: `image` holds text+data+bss, `word(addr)` reads it."""

    def __init__(self, data, base=0):
        if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
            raise ValueError('not an ELF32 little-endian file')
        e_type, e_machine = struct.unpack_from('<HH', data, 0x10)
        if e_type not in ET_SCE_IOPRELEXEC or e_machine != 8:
            raise ValueError(f'not a relocatable IOP executable (e_type 0x{e_type:x}, e_machine {e_machine})')
        self.base = base
        self.entry, phoff, shoff = struct.unpack_from('<III', data, 0x18)
        phnum, = struct.unpack_from('<H', data, 0x2C)
        shnum, shstrndx = struct.unpack_from('<HH', data, 0x30)
        self.sections = []   # (name, type, flags, addr, offset, size, link, info)
        names_off = struct.unpack_from('<10I', data, shoff + shstrndx * 40)[4] if shnum else 0
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, _, _ = struct.unpack_from('<10I', data, shoff + i * 40)
            end = data.index(b'\0', names_off + name)
            self.sections.append((data[names_off + name:end].decode('ascii', 'replace'), typ, flags, addr, off, size,
                                  link, info))
        # The image: the PT_LOAD segment (sections keep their image offsets as addresses)
        self.image = bytearray()
        for i in range(phnum):
            p_type, p_off, _, _, p_filesz, p_memsz, _, _ = struct.unpack_from('<8I', data, phoff + i * 32)
            if p_type == 1:
                self.image = bytearray(data[p_off:p_off + p_filesz]) + bytearray(max(0, p_memsz - p_filesz))
        for name, typ, flags, addr, off, size, _, _ in self.sections:
            if flags & 2 and addr + size > len(self.image):          # SHF_ALLOC beyond the segment (bss)
                self.image += bytearray(addr + size - len(self.image))
            if flags & 2 and typ != SHT_NOBITS and size and not self.image[addr:addr + size]:
                self.image[addr:addr + size] = data[off:off + size]
        # .iopmod: moduleinfo, entry, gp, text size, data size, bss size, version (u16), name
        self.name, self.version, self.gp = '', 0, 0
        self.text_size = self.data_size = self.bss_size = 0
        self.module_info = 0xFFFFFFFF
        for name, typ, _, _, off, size, _, _ in self.sections:
            if typ == SHT_SCE_IOPMOD and size >= 26:
                self.module_info, self.entry, self.gp, self.text_size, self.data_size, self.bss_size, self.version = \
                    struct.unpack_from('<6IH', data, off)
                self.name = data[off + 26:off + size].split(b'\0')[0].decode('ascii', 'replace')
        if not self.text_size:
            self.text_size = next((s[5] for s in self.sections if s[0] == '.text'), len(self.image))
        self.relocations = self._relocate(data)
        self.imports = self._find_imports()
        self.exports = self._find_exports()

    def word(self, address):
        offset = address - self.base
        if 0 <= offset <= len(self.image) - 4:
            return struct.unpack_from('<I', self.image, offset)[0]
        return None

    def _relocate(self, data):
        """Applies the SHT_REL sections as loadcore does for e_type 0xFF80: r_offset is an image offset, the symbol
        is the load base, and a LO16 completes every pending HI16 (the runtime's formula, iop_module_loader.cpp)."""
        base, applied, pending_hi = self.base, 0, []
        self.word_relocs = set()     # image offsets of the R_MIPS_32 words (pointers)
        for name, typ, _, _, off, size, link, info in self.sections:
            if typ not in (SHT_REL, SHT_RELA):
                continue
            entry = 12 if typ == SHT_RELA else 8
            for pos in range(off, off + size - entry + 1, entry):
                r_offset, r_info = struct.unpack_from('<II', data, pos)
                r_type = r_info & 0xFF
                if r_offset + 4 > len(self.image):
                    continue
                w = struct.unpack_from('<I', self.image, r_offset)[0]
                if r_type == R_MIPS_NONE:
                    continue
                if r_type in (R_MIPS_32, R_MIPS_REL32):
                    w = (w + base) & 0xFFFFFFFF
                    self.word_relocs.add(r_offset)
                elif r_type == R_MIPS_26:
                    target = (((w & 0x03FFFFFF) << 2) + base) & 0xFFFFFFFF
                    w = (w & 0xFC000000) | ((target >> 2) & 0x03FFFFFF)
                elif r_type == R_MIPS_HI16:
                    pending_hi.append(r_offset)
                    continue
                elif r_type == R_MIPS_LO16:
                    lo = sext16(w & 0xFFFF)
                    for hi_offset in pending_hi:
                        hi_word = struct.unpack_from('<I', self.image, hi_offset)[0]
                        full = (sext16(hi_word & 0xFFFF) << 16) + lo + base
                        hi = ((full + 0x8000) >> 16) & 0xFFFF
                        struct.pack_into('<I', self.image, hi_offset, (hi_word & 0xFFFF0000) | hi)
                    pending_hi = []
                    w = (w & 0xFFFF0000) | ((lo + base) & 0xFFFF)
                elif r_type == R_MIPS_16:
                    w = (w & 0xFFFF0000) | ((sext16(w & 0xFFFF) + base) & 0xFFFF)
                else:
                    raise ValueError(f'{name}: unsupported relocation type {r_type} at 0x{r_offset:x}')
                struct.pack_into('<I', self.image, r_offset, w)
                applied += 1
        return applied

    def _string(self, offset, size=8):
        raw = self.image[offset:offset + size].split(b'\0')[0]
        return raw.decode('ascii') if raw and all(c in b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_' for c in raw) else None

    def _find_imports(self):
        """Import tables: magic 0x41E00000, next, version (u16), flags (u16), name[8], stubs (jr $ra; addiu $zero,
        $zero, ordinal) until a 0,0 pair."""
        tables = []
        for off in range(0, len(self.image) - 20, 4):
            if struct.unpack_from('<I', self.image, off)[0] != IMPORT_MAGIC:
                continue
            library = self._string(off + 12)
            if not library:
                continue
            version, = struct.unpack_from('<H', self.image, off + 8)
            stubs, pos = {}, off + 20
            while pos + 8 <= len(self.image):
                w0, w1 = struct.unpack_from('<II', self.image, pos)
                if w0 == 0 and w1 == 0:
                    break
                if w0 != JR_RA or (w1 & 0xFFFF0000) != 0x24000000:
                    stubs = None
                    break
                stubs[self.base + pos] = w1 & 0xFFFF
                pos += 8
            if stubs:
                tables.append(ImportTable(self.base + off, library, version, stubs))
        return tables

    def _find_exports(self):
        """Export tables: magic 0x41C00000, next, version (u16), flags (u16), name[8], function pointers until 0."""
        tables, text_end = [], self.base + self.text_size
        for off in range(0, len(self.image) - 20, 4):
            if struct.unpack_from('<I', self.image, off)[0] != EXPORT_MAGIC:
                continue
            library = self._string(off + 12)
            if not library:
                continue
            version, = struct.unpack_from('<H', self.image, off + 8)
            functions, pos = [], off + 20
            while pos + 4 <= len(self.image):
                f, = struct.unpack_from('<I', self.image, pos)
                if f == 0:
                    break
                if not (self.base <= f < text_end) or f & 3:
                    functions = None
                    break
                functions.append(f)
                pos += 4
            if functions:
                tables.append(ExportTable(self.base + off, library, version, functions))
        return tables

    def stub_name(self, address):
        """'lib:ordinal name' of the import stub at address, or None"""
        for table in self.imports:
            if address in table.stubs:
                return ordinal_name(table.library, table.stubs[address])
        return None

    def export_name(self, address):
        for table in self.exports:
            for ordinal, f in enumerate(table.functions):
                if f == address:
                    return ordinal_name(table.library, ordinal)
        return None


# ---------------------------------------------------------------- MIPS I disassembler

SPECIAL = {0x00: 'sll', 0x02: 'srl', 0x03: 'sra', 0x04: 'sllv', 0x06: 'srlv', 0x07: 'srav', 0x08: 'jr', 0x09: 'jalr',
           0x0C: 'syscall', 0x0D: 'break', 0x10: 'mfhi', 0x11: 'mthi', 0x12: 'mflo', 0x13: 'mtlo', 0x18: 'mult',
           0x19: 'multu', 0x1A: 'div', 0x1B: 'divu', 0x20: 'add', 0x21: 'addu', 0x22: 'sub', 0x23: 'subu', 0x24: 'and',
           0x25: 'or', 0x26: 'xor', 0x27: 'nor', 0x2A: 'slt', 0x2B: 'sltu'}
REGIMM = {0x00: 'bltz', 0x01: 'bgez', 0x10: 'bltzal', 0x11: 'bgezal'}
IMMEDIATE = {0x08: 'addi', 0x09: 'addiu', 0x0A: 'slti', 0x0B: 'sltiu', 0x0C: 'andi', 0x0D: 'ori', 0x0E: 'xori'}
MEMORY = {0x20: 'lb', 0x21: 'lh', 0x22: 'lwl', 0x23: 'lw', 0x24: 'lbu', 0x25: 'lhu', 0x26: 'lwr', 0x28: 'sb',
          0x29: 'sh', 0x2A: 'swl', 0x2B: 'sw', 0x2E: 'swr', 0x30: 'lwc0', 0x31: 'lwc1', 0x32: 'lwc2', 0x33: 'lwc3',
          0x38: 'swc0', 0x39: 'swc1', 0x3A: 'swc2', 0x3B: 'swc3'}


def simm(v):
    """a signed 16-bit immediate as text: 1, -0x38, 0x4a8"""
    v = sext16(v)
    if -10 < v < 10:
        return str(v)
    return f'-0x{-v:x}' if v < 0 else f'0x{v:x}'


class Instruction:
    """One decoded word: mnemonic, operands text, branch/jump target, link flag, register written, lui value."""
    __slots__ = ('word', 'mnemonic', 'operands', 'target', 'is_jump', 'is_call', 'writes', 'reads_base')

    def __init__(self):
        self.word = 0
        self.mnemonic, self.operands = '', ''
        self.target = None       # static branch/jump target
        self.is_jump = False     # any control transfer (branch, j, jal, jr, jalr)
        self.is_call = False     # jal / jalr / bgezal / bltzal
        self.writes = None       # GPR written (for the lui tracking)
        self.reads_base = None   # (rs, imm, is_ori) for the lui pairing


def decode(word, address):
    i = Instruction()
    i.word = word
    op, rs, rt, rd, sa, fn, imm = word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31, \
        (word >> 6) & 31, word & 63, word & 0xFFFF
    R = REGS
    if op == 0:
        name = SPECIAL.get(fn)
        if word == 0:
            i.mnemonic = 'nop'
        elif name is None:
            i.mnemonic, i.operands = '.word', f'0x{word:08x}'
        elif fn in (0x00, 0x02, 0x03):
            i.mnemonic, i.operands, i.writes = name, f'{R[rd]}, {R[rt]}, {sa}', rd
        elif fn in (0x04, 0x06, 0x07):
            i.mnemonic, i.operands, i.writes = name, f'{R[rd]}, {R[rt]}, {R[rs]}', rd
        elif fn == 0x08:
            i.mnemonic, i.operands, i.is_jump = name, R[rs], True
        elif fn == 0x09:
            i.mnemonic, i.is_jump, i.is_call, i.writes = name, True, True, rd
            i.operands = R[rs] if rd == 31 else f'{R[rd]}, {R[rs]}'
        elif fn in (0x0C, 0x0D):
            code = (word >> 6) & 0xFFFFF
            i.mnemonic, i.operands = name, (f'0x{code:x}' if code else '')
        elif fn in (0x10, 0x12):
            i.mnemonic, i.operands, i.writes = name, R[rd], rd
        elif fn in (0x11, 0x13):
            i.mnemonic, i.operands = name, R[rs]
        elif fn in (0x18, 0x19, 0x1A, 0x1B):
            i.mnemonic, i.operands = name, f'{R[rs]}, {R[rt]}'
        elif fn in (0x21, 0x25) and rt == 0:            # addu/or rd, rs, $zero
            i.mnemonic, i.operands, i.writes = 'move', f'{R[rd]}, {R[rs]}', rd
        elif fn == 0x23 and rs == 0:                    # subu rd, $zero, rt
            i.mnemonic, i.operands, i.writes = 'negu', f'{R[rd]}, {R[rt]}', rd
        elif fn == 0x27 and rt == 0:                    # nor rd, rs, $zero
            i.mnemonic, i.operands, i.writes = 'not', f'{R[rd]}, {R[rs]}', rd
        else:
            i.mnemonic, i.operands, i.writes = name, f'{R[rd]}, {R[rs]}, {R[rt]}', rd
    elif op == 1:
        name = REGIMM.get(rt)
        if name is None:
            i.mnemonic, i.operands = '.word', f'0x{word:08x}'
        else:
            i.target = (address + 4 + (sext16(imm) << 2)) & 0xFFFFFFFF
            i.mnemonic, i.operands, i.is_jump = name, f'{R[rs]}, 0x{i.target:x}', True
            if rt in (0x10, 0x11):
                i.is_call, i.writes = True, 31
    elif op in (2, 3):
        i.target = ((address + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        i.mnemonic, i.operands, i.is_jump = ('j', 'jal')[op - 2], f'0x{i.target:x}', True
        if op == 3:
            i.is_call, i.writes = True, 31
    elif op in (4, 5, 6, 7):
        i.target = (address + 4 + (sext16(imm) << 2)) & 0xFFFFFFFF
        i.is_jump = True
        if op == 4 and rs == 0 and rt == 0:
            i.mnemonic, i.operands = 'b', f'0x{i.target:x}'
        elif op in (4, 5) and rt == 0:
            i.mnemonic, i.operands = ('beqz', 'bnez')[op - 4], f'{R[rs]}, 0x{i.target:x}'
        elif op in (4, 5):
            i.mnemonic, i.operands = ('beq', 'bne')[op - 4], f'{R[rs]}, {R[rt]}, 0x{i.target:x}'
        else:
            i.mnemonic, i.operands = ('blez', 'bgtz')[op - 6], f'{R[rs]}, 0x{i.target:x}'
    elif op in IMMEDIATE:
        name = IMMEDIATE[op]
        i.writes = rt
        if op in (0x08, 0x09) and rs == 0:
            i.mnemonic, i.operands = 'li', f'{R[rt]}, {simm(imm)}'
        elif op == 0x0D and rs == 0:
            i.mnemonic, i.operands = 'li', f'{R[rt]}, 0x{imm:x}'
        elif op in (0x08, 0x09, 0x0A, 0x0B):
            i.mnemonic, i.operands = name, f'{R[rt]}, {R[rs]}, {simm(imm)}'
            i.reads_base = (rs, sext16(imm), False) if op == 0x09 else None
        else:
            i.mnemonic, i.operands = name, f'{R[rt]}, {R[rs]}, 0x{imm:x}'
            i.reads_base = (rs, imm, True) if op == 0x0D else None
    elif op == 0x0F:
        i.mnemonic, i.operands, i.writes = 'lui', f'{R[rt]}, 0x{imm:x}', rt
    elif op == 0x10:
        if rs == 0:
            i.mnemonic, i.operands, i.writes = 'mfc0', f'{R[rt]}, ${COP0.get(rd, rd)}', rt
        elif rs == 4:
            i.mnemonic, i.operands = 'mtc0', f'{R[rt]}, ${COP0.get(rd, rd)}'
        elif rs == 2:
            i.mnemonic, i.operands, i.writes = 'cfc0', f'{R[rt]}, ${rd}', rt
        elif rs == 6:
            i.mnemonic, i.operands = 'ctc0', f'{R[rt]}, ${rd}'
        elif rs == 0x10 and fn == 0x10:
            i.mnemonic = 'rfe'
        else:
            i.mnemonic, i.operands = 'cop0', f'0x{word & 0x1FFFFFF:x}'
    elif op in (0x11, 0x12, 0x13):
        cop = op - 0x10
        if rs == 0:
            i.mnemonic, i.operands, i.writes = f'mfc{cop}', f'{R[rt]}, ${rd}', rt
        elif rs == 2:
            i.mnemonic, i.operands, i.writes = f'cfc{cop}', f'{R[rt]}, ${rd}', rt
        elif rs == 4:
            i.mnemonic, i.operands = f'mtc{cop}', f'{R[rt]}, ${rd}'
        elif rs == 6:
            i.mnemonic, i.operands = f'ctc{cop}', f'{R[rt]}, ${rd}'
        else:
            i.mnemonic, i.operands = f'cop{cop}', f'0x{word & 0x1FFFFFF:x}'
    elif op in MEMORY:
        name = MEMORY[op]
        if op >= 0x30:                                  # a coprocessor register, not a GPR
            i.mnemonic, i.operands = name, f'${rt}, {simm(imm)}({R[rs]})'
        else:
            i.mnemonic, i.operands = name, f'{R[rt]}, {simm(imm)}({R[rs]})'
            if op < 0x28:                               # loads write rt (lwl/lwr merge into it)
                i.writes = rt
        i.reads_base = (rs, sext16(imm), False)
    else:
        i.mnemonic, i.operands = '.word', f'0x{word:08x}'
    return i


def data_word(word):
    """a word that is data (an import/export table), shown as .word"""
    i = Instruction()
    i.word, i.mnemonic, i.operands = word, '.word', f'0x{word:08x}'
    return i


def format_line(address, word, mnemonic, operands, comment=''):
    text = f'{address:08x}: {word:08x}  {mnemonic:<8}{operands}'
    return f'{text:<48} ; {comment}' if comment else text


# ---------------------------------------------------------------- listing and heuristics

CALLER_SAVED = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 31}

# Calls whose arguments are worth reporting in the header: (library, ordinal) -> (section, argument names).
# Stack arguments (0x10($sp) onwards) follow a0..a3 in the o32 ABI.
REPORTED_CALLS = {
    ('sifcmd', 17): ('RPC servers', ['sd', 'sid', 'func', 'buff', 'cfunc', 'cbuff', 'qd']),
    ('sifcmd', 15): ('RPC clients', ['cd', 'sid', 'mode']),
    ('sifcmd', 16): ('RPC clients', ['cd', 'fno', 'mode', 'send', 'ssize', 'recv', 'rsize', 'end_func', 'end_arg']),
    ('sifcmd', 19): ('RPC queues', ['qd', 'thread']),
    ('sifcmd', 22): ('RPC queues', ['qd']),
    ('sifcmd', 24): ('RPC queues', ['sd', 'qd']),
    ('sifcmd', 25): ('RPC queues', ['qd']),
    ('sifcmd', 10): ('CMD handlers', ['cid', 'handler', 'harg']),
    ('sifcmd', 11): ('CMD handlers', ['cid']),
    ('sifcmd', 8): ('CMD handlers', ['buffer', 'size']),
    ('sifcmd', 9): ('CMD handlers', ['buffer', 'size']),
    ('intrman', 4): ('Interrupt handlers', ['irq', 'mode', 'handler', 'arg']),
    ('vblank', 8): ('Interrupt handlers', ['startend', 'priority', 'handler', 'arg']),
    ('timrman', 20): ('Interrupt handlers', ['timer', 'compare', 'handler', 'arg']),
    ('timrman', 21): ('Interrupt handlers', ['timer', 'handler', 'arg']),
    ('libsd', 21): ('Interrupt handlers', ['channel', 'func', 'arg']),
    ('libsd', 22): ('Interrupt handlers', ['func', 'arg']),
    ('libsd', 26): ('Interrupt handlers', ['channel', 'handler', 'arg']),
    ('libsd', 27): ('Interrupt handlers', ['handler', 'arg']),
    ('thbase', 4): ('Threads', ['param']),
    ('thbase', 6): ('Threads', ['thread', 'arg']),
}
THREAD_PARAM = ['attr', 'option', 'entry', 'stacksize', 'priority']


class Listing:
    """The annotated disassembly of a module and the call records of the heuristic pass."""

    def __init__(self, module):
        self.module = module
        self.text_start, self.text_end = module.base, module.base + module.text_size
        self.instructions = {a: decode(module.word(a), a) for a in range(self.text_start, self.text_end, 4)}
        self.table_words = self._table_words()
        for address in self.table_words:
            if address in self.instructions:
                self.instructions[address] = data_word(module.word(address))
        self.stubs = {a: ordinal_name(t.library, o) for t in module.imports for a, o in t.stubs.items()}
        self.stub_keys = {a: (t.library.lower(), o) for t in module.imports for a, o in t.stubs.items()}
        self.labels = {}
        self._find_labels()
        self.lines, self.calls = [], []   # calls: (address, (library, ordinal), name, section, {arg: value})
        self._analyze()                   # first pass: the handlers and thread entries the calls name get labels
        for address, key, name, section, values in self.calls:
            for field in ('func', 'cfunc', 'handler', 'entry', 'end_func'):
                v = values.get(field)
                if isinstance(v, int) and self.text_start <= v < self.text_end and v & 3 == 0 \
                        and self.labels.get(v, 'loc_').startswith('loc_'):
                    self.labels[v] = f'func_{v:x}'
        self.lines, self.calls = [], []
        self._analyze()

    def _table_words(self):
        """The words of the import and export tables, which are data: address -> comment."""
        words = {}
        for kind, tables in (('import', self.module.imports), ('export', self.module.exports)):
            for t in tables:
                words[t.address] = f'{kind} table of "{t.library}": magic'
                words[t.address + 4] = 'next'
                words[t.address + 8] = f'version 0x{t.version:x}, flags'
                words[t.address + 12] = f'"{t.library}"'
                words[t.address + 16] = ''
                if kind == 'import':
                    end = t.address + 20 + 8 * len(t.stubs)
                    words[end] = 'end of the import table'
                    words[end + 4] = ''
                else:
                    for ordinal, f in enumerate(t.functions):
                        words[t.address + 20 + 4 * ordinal] = ordinal_name(t.library, ordinal)
                    words[t.address + 20 + 4 * len(t.functions)] = 'end of the export table'
        return words

    def data_code_pointers(self):
        """Relocated words of the data sections that point into .text: [(address, section, target)]. A pointer in
        .rodata is usually a jump table entry (inside a function); elsewhere it is a function pointer."""
        m = self.module
        found = []
        for name, typ, flags, addr, off, size, link, info in m.sections:
            if not (flags & 2) or flags & 4 or typ == SHT_NOBITS:
                continue
            for a in range(m.base + addr, m.base + addr + size - 3, 4):
                v = m.word(a)
                if a - m.base in m.word_relocs and self.text_start <= v < self.text_end and v & 3 == 0:
                    found.append((a, name, v))
        return found

    def initialized(self, address):
        """True when address lies in a section the file initializes (.text, .data, .rodata: not .bss)"""
        m = self.module
        for name, typ, flags, addr, off, size, link, info in m.sections:
            if flags & 2 and typ != SHT_NOBITS and m.base + addr <= address < m.base + addr + size - 3:
                return True
        return False

    def _find_labels(self):
        m = self.module
        for address, ins in self.instructions.items():
            if ins.target is None or not self.text_start <= ins.target < self.text_end:
                continue
            if ins.is_call:
                self.labels[ins.target] = f'func_{ins.target:x}'
            else:
                self.labels.setdefault(ins.target, f'loc_{ins.target:x}')
        for address, section, target in self.data_code_pointers():
            if section == '.rodata':
                self.labels.setdefault(target, f'loc_{target:x}')
            elif self.labels.get(target, 'loc_').startswith('loc_'):
                self.labels[target] = f'func_{target:x}'
        for table in m.exports:
            for ordinal, f in enumerate(table.functions):
                name = ordinal_name(table.library, ordinal)
                self.labels[f] = name.split(' ', 1)[1] if ' ' in name else name.replace(':', '_')
        for address, name in self.stubs.items():
            self.labels[address] = 'stub_' + name.split(' ')[0].replace(':', '_')
        if self.text_start <= m.base + m.entry < self.text_end:   # the entry is a module offset
            self.labels[m.base + m.entry] = '_start'

    def _analyze(self):
        """One linear pass: comments (import names, lui pairs), and a tracker of the constants held by the registers
        (lui, li, lui+addiu/ori, move, addiu from $sp) and the stack slots, read at the calls REPORTED_CALLS lists."""
        known = {}        # register -> int value, or ('sp', offset) for an address on the stack
        stack = {}        # sp offset -> value stored there
        pending_call = None
        for address in range(self.text_start, self.text_end, 4):
            ins = self.instructions[address]
            done = self.instructions.get(address - 8)       # the instruction whose delay slot has just run
            if done is not None and done.is_call:            # the callee returned: caller-saved registers are gone
                for reg in CALLER_SAVED:
                    known.pop(reg, None)
                stack = {}
            if done is not None and done.word == JR_RA:      # a function ended (jr $ra and its delay slot)
                known, stack = {}, {}
            label = self.labels.get(address)
            if label:
                if label.startswith('stub_'):
                    self.lines.append(f'\n; import {self.stubs[address]}')
                elif not label.startswith('loc_'):
                    self.lines.append('')
                    known, stack = {}, {}      # a function entry: nothing is known about the registers
                self.lines.append(f'{label}:')
            comment = []
            if self.table_words.get(address):
                comment.append(self.table_words[address])
            if ins.is_call and ins.target is not None:
                if ins.target in self.stubs:
                    comment.append(self.stubs[ins.target])
                    pending_call = (address, ins.target)
                elif ins.target in self.labels and not self.labels[ins.target].startswith('func_'):
                    comment.append(self.labels[ins.target])
            elif ins.mnemonic == 'j' and ins.target in self.labels and not self.labels[ins.target].startswith('loc_'):
                comment.append(self.labels[ins.target])
            value = None
            op, rs, rt, rd, imm = ins.word >> 26, (ins.word >> 21) & 31, (ins.word >> 16) & 31, \
                (ins.word >> 11) & 31, ins.word & 0xFFFF
            if ins.reads_base is not None:
                base_value = known.get(rs)
                if isinstance(base_value, int) and rs != 29:
                    value = (base_value | imm) if ins.reads_base[2] else (base_value + sext16(imm)) & 0xFFFFFFFF
                    text = f'= 0x{value:x}'
                    if value in self.labels and ins.mnemonic in ('addiu', 'ori'):
                        text += f' {self.labels[value]}'
                    comment.append(text)
                elif isinstance(base_value, tuple) and ins.mnemonic == 'addiu':
                    value = ('sp', base_value[1] + sext16(imm))
            self.lines.append(format_line(address, ins.word, ins.mnemonic, ins.operands, '; '.join(comment)))
            # the tracker, after the instruction ($sp itself is never tracked: stack slots are relative to it)
            if ins.writes == 29:
                pass
            elif ins.mnemonic == 'lui':
                known[rt] = imm << 16
            elif ins.mnemonic == 'li':
                known[rt] = imm if op == 0x0D else sext16(imm) & 0xFFFFFFFF
            elif ins.mnemonic == 'move':
                if rs == 0:
                    known[rd] = 0
                elif rs in known:
                    known[rd] = known[rs]
                else:
                    known.pop(rd, None)
            elif ins.mnemonic in ('addiu', 'ori'):
                if value is not None:
                    known[rt] = value
                elif rs == 29 and ins.mnemonic == 'addiu':
                    known[rt] = ('sp', sext16(imm))
                else:
                    known.pop(rt, None)
            elif ins.mnemonic == 'sw' and (rs == 29 or isinstance(known.get(rs), tuple)):
                slot = sext16(imm) + (0 if rs == 29 else known[rs][1])   # through $sp or a register holding sp+n
                if rt in known:
                    stack[slot] = known[rt]
                elif rt == 0:
                    stack[slot] = 0
                else:
                    stack.pop(slot, None)
            elif ins.mnemonic == 'lw' and isinstance(value, int) and self.initialized(value) and rt != 0:
                known[rt] = self.module.word(value)      # a constant kept in .data/.rodata
            elif ins.writes is not None and ins.writes != 0:
                known.pop(ins.writes, None)
            if pending_call and address == pending_call[0] + 4:      # the delay slot ran: record the call
                self._record_call(pending_call[0], pending_call[1], known, stack)
                pending_call = None
        self.lines.append('')

    def _record_call(self, address, stub, known, stack):
        key = self.stub_keys[stub]
        if key not in REPORTED_CALLS:
            return
        section, names = REPORTED_CALLS[key]
        values = {}
        for n, name in enumerate(names):
            values[name] = known.get(4 + n) if n < 4 else stack.get(0x10 + 4 * (n - 4))
        if key == ('thbase', 4):                                     # the ThreadParam: on the stack or in data
            param = values.get('param')
            if isinstance(param, tuple):
                for n, field in enumerate(THREAD_PARAM):
                    values[field] = stack.get(param[1] + 4 * n)
            elif isinstance(param, int):
                for n, field in enumerate(THREAD_PARAM):
                    values[field] = self.module.word(param + 4 * n)
        self.calls.append((address, key, self.stubs[stub], section, values))

    def value_text(self, v):
        if v is None:
            return '?'
        if isinstance(v, tuple):
            return f'sp{v[1]:+#x}'
        text = f'0x{v:x}'
        if v in self.labels:
            text += f' ({self.labels[v]})'
        chars = v.to_bytes(4, 'big') if 0x20202020 <= v <= 0x7E7E7E7E else b''
        if chars and all(0x20 <= c < 0x7F for c in chars):     # an RPC id spelled in ASCII ('SNDF')
            text += f" '{chars.decode('ascii')}'"
        return text

    # -------- output

    def header(self):
        m = self.module
        L = [f'; module "{m.name}" version 0x{m.version:x} ({m.version >> 8}.{m.version & 0xFF})  base 0x{m.base:x}'
             f'  entry 0x{m.base + m.entry:x} (offset 0x{m.entry:x})  gp 0x{m.base + m.gp:x} (offset 0x{m.gp:x})'
             f'  moduleinfo offset 0x{m.module_info:x}',
             f'; text 0x{m.base:x}-0x{m.base + m.text_size:x} (0x{m.text_size:x})  data 0x{m.data_size:x}'
             f'  bss 0x{m.bss_size:x}  image 0x{len(m.image):x} bytes  relocations applied: {m.relocations}',
             '; address = base + module offset', '; sections:']
        for name, typ, flags, addr, off, size, link, info in m.sections:
            if flags & 2 or typ in (SHT_REL, SHT_RELA, SHT_SCE_IOPMOD):
                where = f'0x{m.base + addr:x}-0x{m.base + addr + size:x}' if flags & 2 else f'file 0x{off:x}'
                L.append(f';   {name:<12} type 0x{typ:x} flags 0x{flags:x} {where} size 0x{size:x}' +
                         (f' (applies to section {info})' if typ in (SHT_REL, SHT_RELA) else ''))
        L.append('; imports:')
        for table in m.imports:
            L.append(f';   {table.library} version 0x{table.version:x} (table at 0x{table.address:x})')
            for address, ordinal in table.stubs.items():
                L.append(f';     0x{address:x}  {ordinal_name(table.library, ordinal)}')
        if not m.imports:
            L.append(';   (none)')
        L.append('; exports:')
        for table in m.exports:
            L.append(f';   {table.library} version 0x{table.version:x} (table at 0x{table.address:x})')
            for ordinal, f in enumerate(table.functions):
                L.append(f';     0x{f:x}  {ordinal_name(table.library, ordinal)}')
        if not m.exports:
            L.append(';   (none)')
        for section in ['RPC servers', 'RPC queues', 'RPC clients', 'CMD handlers', 'Interrupt handlers', 'Threads']:
            calls = [c for c in self.calls if c[3] == section]
            L.append(f'; {section}:')
            if not calls:
                L.append(';   (none)')
            for address, key, name, _, values in calls:
                args = ', '.join(f'{k}={self.value_text(v)}' for k, v in values.items())
                L.append(f';   0x{address:x}: {name}({args})')
        L.append('; code pointers in data (relocated words):')
        pointers = self.data_code_pointers()
        for a, name, v in pointers[:400]:
            L.append(f';   0x{a:x} ({name}) -> 0x{v:x} {self.labels.get(v, "")}')
        if not pointers:
            L.append(';   (none)')
        return L

    def data_dump(self):
        m = self.module
        L = []
        for name, typ, flags, addr, off, size, link, info in m.sections:
            if not (flags & 2) or flags & 4 or not size:
                continue
            start = m.base + addr
            if typ == SHT_NOBITS:
                L.append(f'\n; {name}: 0x{start:x}-0x{start + size:x} (0x{size:x} bytes, zero-filled)')
                continue
            L.append(f'\n; {name}: 0x{start:x}-0x{start + size:x} (0x{size:x} bytes)')
            for a in range(start, start + size, 16):
                chunk = bytes(m.image[a - m.base:min(a - m.base + 16, addr + size)])
                hexes = ' '.join(f'{b:02x}' for b in chunk)
                ascii_ = ''.join(chr(b) if 32 <= b < 127 else '.' for b in chunk)
                L.append(f'{a:08x}: {hexes:<47}  |{ascii_}|')
        return L

    def render(self, source=''):
        L = [f'; {source}' if source else '; IRX disassembly'] + self.header()
        L.append(f'\n; .text: 0x{self.text_start:x}-0x{self.text_end:x}')
        L.extend(self.lines)
        L.extend(self.data_dump())
        return '\n'.join(L) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description='Disassemble an IOP module (IRX).')
    parser.add_argument('irx')
    parser.add_argument('--base', default='0', help='load address (hex), default 0')
    parser.add_argument('-o', '--output', help='write the listing here instead of stdout')
    args = parser.parse_args(argv)
    base = int(args.base, 16)
    with open(args.irx, 'rb') as f:
        module = IrxModule(f.read(), base)
    text = Listing(module).render(f'{args.irx} at base 0x{base:x}')
    if args.output:
        with open(args.output, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
