/*
 * Xbox 360 Hypervisor (HV) - kernel build 17559
 * Decompiled from ce_17559.bin / old_HV_17559.bin.i64 using IDA Pro 9.5 + Hex-Rays PPC64
 *
 * Base address: 0x0 (image loaded at 0)
 * Processor: PPC64 big-endian, HV mode
 *
 * Syscall dispatch:
 *   - _SyscallTable @ 0x15FD0 (128 entries, indexed by r0)
 *   - Syscalls 0-119 (0x00-0x77) are documented (see api/hv.md)
 *   - Syscalls 120-127 (0x78-0x7F) are internal HVP interfaces used by CB / shadow boot
 *   - HvSyscallDispatch @ 0xB04 is the primary dispatcher (r0 < 0x78)
 *   - HvSyscallDispatch_High @ 0xA10 handles r0 >= 0x78 (currently unused - falls through)
 *   - Most syscalls dispatch through a 2-instruction thunk (Hvx*_Thunk) which
 *     jumps via jt1_jumper @ 0x77C using an offset in r11 through the
 *     _SecondaryJumpTable @ 0xAE80 (pointer at HV_JT_POINTER_TABLE @ 0x80),
 *     then to the real implementation.
 *
 * Exception vectors: standard PPC64 layout (_v_RESET @ 0x100, _v_MACHINE_CHECK @ 0x200,
 *   _v_DATA_STORAGE @ 0x300, ..., _v_SYSTEM_CALL @ 0xC00, ...)
 *
 * Thread state:
 *   - HSPRG0 (SPR 304) -> HV_THREAD_STATE * (per-thread context save area)
 *   - HSPRG1 (SPR 305) -> HV per-thread stack base (top = HSPRG1 + 0x1F00)
 *   - HV_THREAD_STATE.SUSPENDED @ +0x83 (nonzero = thread is in HvpSleepThread)
 */

#include <stdint.h>

typedef uint8_t  X_BOOL;
typedef uint32_t X_STATUS;
typedef uint32_t X_RESULT;
typedef void    *X_HANDLE;

#define X_FALSE 0
#define X_TRUE  1

/* PPC64 Special Purpose Registers relevant to the HV */
enum HV_SPR {
    SPR_XER      = 1,
    SPR_LR       = 8,
    SPR_CTR      = 9,
    SPR_DSISR    = 18,
    SPR_DAR      = 19,
    SPR_DEC      = 22,
    SPR_SDR1     = 25,
    SPR_SRR0     = 26,
    SPR_SRR1     = 27,
    SPR_SPRG0    = 272,
    SPR_TBL      = 284,
    SPR_TBU      = 285,
    SPR_PVR      = 287,
    SPR_HSPRG0   = 304,   /* -> HV_THREAD_STATE *                        */
    SPR_HSPRG1   = 305,   /* HV per-thread stack base (top = +0x1F00)     */
    SPR_HDSISR   = 308,
    SPR_HDAR     = 309,
    SPR_HDEC     = 310,
    SPR_HSRR0    = 312,
    SPR_HSRR1    = 313,
    SPR_HID0     = 1008,
    SPR_HID1     = 1009,
    SPR_PIR      = 1023,  /* Processor Identification Register            */
};

/* Per-thread HV state, pointed to by HSPRG0 */
struct HV_THREAD_STATE {
    uint8_t   pad_00[0x04];
    uint32_t  flags_04;          /* +0x04: bit 0x2000 tested in _v_FPU_UNAVAILABLE */
    uint8_t   pad_08[0x18];
    uint64_t  saved_LR;          /* +0x20 */
    uint32_t  saved_CR;          /* +0x28 */
    uint32_t  saved_CR_hi;       /* +0x2C */
    uint32_t  saved_SRR1_lo;     /* +0x30 */
    uint32_t  field_34;          /* +0x34 */
    uint64_t  saved_R1;          /* +0x38 (SP) */
    uint64_t  saved_R2;          /* +0x40 */
    uint64_t  saved_R4;          /* +0x48 */
    uint64_t  saved_R5;          /* +0x50 */
    uint32_t  SLB_index;         /* +0x58 */
    uint8_t   pad_5C[0x27];
    uint8_t   SUSPENDED;         /* +0x83: nonzero = thread is suspended */
};

struct X_ANSI_STRING {
    uint16_t length;
    uint16_t maximum_length;
    char *pointer;
};

struct X_UNICODE_STRING {
    uint16_t length;
    uint16_t maximum_length;
    uint32_t pointer;
};

struct X_XEX_PAGE_DESCRIPTOR {
    uint32_t size_info;
    uint8_t  next_page_digest[20];
};

struct X_CRYPT_AES_STATE {
    uint8_t keytab_enc[11][4][4];
    uint8_t keytab_dec[11][4][4];
};

struct X_CRYPT_SHA_STATE {
    uint32_t count;
    uint32_t state[5];
    uint8_t  buffer[64];
};

struct X_CRYPT_HMAC_SHA_STATE {
    struct X_CRYPT_SHA_STATE sha_state[2];
};

struct X_CRYPT_DES_STATE {
    uint64_t keytab[16];
};

struct X_CRYPT_DES3_STATE {
    struct X_CRYPT_DES_STATE des_state[3];
};

struct X_CRYPT_RC4_STATE {
    uint8_t S[256];
    uint8_t i;
    uint8_t j;
};

/* ===== Syscall dispatch table (0x15FD0) =====
 *
 * Idx  Hex  Name                                     Impl addr  Kind
 * ---  ---  ---------------------------------------  ---------  ----------
 *   0  0x00  HvxGetVersion                            0x1cc8     direct
 *   1  0x01  HvxStartupProcessors                     0xc44      direct
 *   2  0x02  HvxQuiesceProcessor                      0x8b40     direct
 *   3  0x03  HvxFlushEntireTb                         0x1838     direct
 *   4  0x04  HvxFlushSingleTb                         0x2510     direct
 *   5  0x05  HvxRelocateAndFlush                      0x31b0     direct
 *   6  0x06  HvNoOp_ReturnZero                        0x2bb8     direct (no-op stub)
 *   7  0x07  HvNoOp_ReturnZero                        0x2bb8     direct (no-op stub)
 *   8  0x08  HvxGetSocRegister                        0x2588     direct
 *   9  0x09  HvxSetSocRegister                        0x26c8     direct
 *  10  0x0A  HvxSetTimebaseToZero                     0xc34      direct
 *  11  0x0B  HvxZeroPage                              0x2af0     direct
 *  12  0x0C  HvxFlushDcacheRange                      0x2a80     direct
 *  13  0x0D  HvxPostOutput                            0x2540     direct
 *  14  0x0E  HvxEnablePPUPerformanceMonitor           0x177c     direct (no-op stub)
 *  15  0x0F  HvxGetImagePageTableEntry_Thunk          0xb270     thunk
 *  16  0x10  HvxSetImagePageTableEntry_Thunk          0xb318     thunk (Debug-only: disabled in retail)
 *  17  0x11  HvxCreateImageMapping_Thunk              0xb180     thunk
 *  18  0x12  HvxMapImagePage_Thunk                    0xb2e8     thunk
 *  19  0x13  HvxCompleteImageMapping_Thunk            0xb178     thunk
 *  20  0x14  HvxLoadImageData_Thunk                   0xb2e0     thunk
 *  21  0x15  HvxFinishImageDataLoad_Thunk             0xb258     thunk
 *  22  0x16  HvxStartResolveImports_Thunk             0xb330     thunk
 *  23  0x17  HvxResolveImports_Thunk                  0xb2f0     thunk
 *  24  0x18  HvxXexUnk24_Thunk                        0xb338     thunk
 *  25  0x19  HvxXexUnk25_Thunk                        0xb310     thunk
 *  26  0x1A  HvxFinishImageLoad_Thunk                 0xb260     thunk
 *  27  0x1B  HvxAbandonImageLoad_Thunk                0xb170     thunk
 *  28  0x1C  HvxUnmapImagePages_Thunk                 0xb348     thunk
 *  29  0x1D  HvxUnmapImage_Thunk                      0xb340     thunk
 *  30  0x1E  HvxUnmapImageRange_Thunk                 0xb350     thunk
 *  31  0x1F  HvxCreateUserMode_Thunk                  0xb188     thunk
 *  32  0x20  HvxDeleteUserMode_Thunk                  0xb190     thunk
 *  33  0x21  HvxFlushUserModeTb_Thunk                 0xb268     thunk
 *  34  0x22  HvxSetPowerMode                          0x9370     direct
 *  35  0x23  HvxShadowBoot_Thunk                      0xb328     thunk (disabled: panic)
 *  36  0x24  HvxBlowFuses                             0xa560     direct
 *  37  0x25  HvxFsbInterrupt                          0x27f0     direct
 *  38  0x26  HvxLockL2                                0x2910     direct
 *  39  0x27  HvxDvdAuthBuildNVPage_Thunk              0xb198     thunk
 *  40  0x28  HvxDvdAuthVerifyNVPage_Thunk             0xb218     thunk
 *  41  0x29  HvxDvdAuthRecordAuthenticationPage_Thunk  0xb1e0     thunk
 *  42  0x2A  HvxDvdAuthRecordXControl_Thunk           0xb1e8     thunk
 *  43  0x2B  HvxDvdAuthGetAuthPage_Thunk              0xb1c0     thunk
 *  44  0x2C  HvxDvdAuthVerifyAuthPage_Thunk           0xb208     thunk
 *  45  0x2D  HvxDvdAuthGetNextLBAIndex_Thunk          0xb1d8     thunk
 *  46  0x2E  HvxDvdAuthVerifyLBA_Thunk                0xb210     thunk
 *  47  0x2F  HvxDvdAuthClearDiscAuthInfo_Thunk        0xb1a0     thunk
 *  48  0x30  HvxKeysInitialize                        0x7698     direct
 *  49  0x31  HvxKeysGetKeyProperties                  0x5088     direct
 *  50  0x32  HvxKeysGetStatus                         0x5b38     direct
 *  51  0x33  HvxKeysGenerateRandomKey                 0x7db0     direct
 *  52  0x34  HvxKeysGetFactoryChallenge               0x4250     direct
 *  53  0x35  HvxKeysSetFactoryResponse                0x7978     direct
 *  54  0x36  HvxKeysSaveBootLoader                    0x42d8     direct
 *  55  0x37  HvxKeysSaveKeyVault                      0x4f90     direct
 *  56  0x38  HvxKeysSetKey                            0x7a90     direct
 *  57  0x39  HvxKeysGetKey                            0x7ea0     direct
 *  58  0x3A  HvxKeysGetDigest                         0x50d8     direct
 *  59  0x3B  HvxKeysRsaPrvCrypt                       0x51b8     direct
 *  60  0x3C  HvxKeysHmacSha                           0x5518     direct
 *  61  0x3D  HvxKeysAesCbc                            0x55e0     direct
 *  62  0x3E  HvxKeysDes2Cbc                           0x5778     direct
 *  63  0x3F  HvxKeysDesCbc                            0x58e8     direct
 *  64  0x40  HvxKeysObscureKey                        0x5a20     direct
 *  65  0x41  HvxKeysSaveSystemUpdate                  0x5b40     direct
 *  66  0x42  HvxKeysExecute                           0x5f68     direct
 *  67  0x43  HvxDvdAuthTestMode_Thunk                 0xb200     thunk
 *  68  0x44  HvxEnableTimebase                        0x8d88     direct
 *  69  0x45  HvxHdcpCalculateMi_Thunk                 0xb288     thunk
 *  70  0x46  HvxHdcpCalculateAKsvSignature_Thunk      0xb278     thunk
 *  71  0x47  HvxHdcpCalculateBKsvSignature_Thunk      0xb280     thunk
 *  72  0x48  HvxSetRevocationList_Thunk               0xb320     thunk
 *  73  0x49  HvxEncryptedAllocationReserve_Thunk      0xb230     thunk
 *  74  0x4A  HvxEncryptedAllocationMap_Thunk          0xb220     thunk
 *  75  0x4B  HvxEncryptedAllocationUnmap_Thunk        0xb238     thunk
 *  76  0x4C  HvxEncryptedAllocationRelease_Thunk      0xb228     thunk
 *  77  0x4D  HvxEncryptedSweepAddressRange_Thunk      0xb240     thunk
 *  78  0x4E  HvxKeysExCreateKeyVault                  0x6240     direct
 *  79  0x4F  HvxKeysExLoadKeyVault                    0x62c0     direct
 *  80  0x50  HvxKeysExSaveKeyVault                    0x63c0     direct
 *  81  0x51  HvxKeysExSetKey                          0x6498     direct
 *  82  0x52  HvxKeysExGetKey                          0x65e8     direct
 *  83  0x53  HvxGetUpdateSequence                     0x6738     direct
 *  84  0x54  HvxSecurityInitialize                    0x68d8     direct
 *  85  0x55  HvxSecurityLoadSettings                  0x6a50     direct
 *  86  0x56  HvxSecuritySaveSettings                  0x6968     direct
 *  87  0x57  HvxSecuritySetDetected                   0x6bb0     direct
 *  88  0x58  HvxSecurityGetDetected                   0x6c48     direct
 *  89  0x59  HvxSecuritySetActivated                  0x6c98     direct
 *  90  0x5A  HvxSecurityGetActivated                  0x6d08     direct
 *  91  0x5B  HvxSecuritySetStat                       0x6d58     direct
 *  92  0x5C  HvxGetProtectedFlags                     0x6dc8     direct
 *  93  0x5D  HvxSetProtectedFlag                      0x6dd8     direct
 *  94  0x5E  HvxDvdAuthGetAuthResults_Thunk           0xb1c8     thunk
 *  95  0x5F  HvxDvdAuthSetDriveAuthResult_Thunk       0xb1f8     thunk
 *  96  0x60  HvxDvdAuthSetDiscAuthResult_Thunk        0xb1f0     thunk
 *  97  0x61  HvxImageTransformImageKey_Thunk          0xb2a0     thunk
 *  98  0x62  HvxImageXexHeader_Thunk                  0xb2b0     thunk
 *  99  0x63  HvxRevokeLoad_Thunk                      0xb2f8     thunk
 * 100  0x64  HvxRevokeSave_Thunk                      0xb300     thunk
 * 101  0x65  HvxRevokeUpdate_Thunk                    0xb308     thunk
 * 102  0x66  HvxDvdAuthGetMediaId_Thunk               0xb1d0     thunk
 * 103  0x67  HvxXexActivationGetNonce_Thunk           0xb358     thunk
 * 104  0x68  HvxXexActivationSetLicense_Thunk         0xb360     thunk
 * 105  0x69  HvxXexActivationVerifyOwnership_Thunk    0xb368     thunk
 * 106  0x6A  HvxIptvSetBoundaryKey_Thunk              0xb2c8     thunk
 * 107  0x6B  HvxIptvSetSessionKey_Thunk               0xb2d0     thunk
 * 108  0x6C  HvxIptvVerifyOmac1Signature_Thunk        0xb2d8     thunk
 * 109  0x6D  HvxIptvGetAesCtrTransform_Thunk          0xb2b8     thunk
 * 110  0x6E  HvxIptvGetSessionKeyHash_Thunk           0xb2c0     thunk
 * 111  0x6F  HvxImageDvdEmulationMode_Thunk           0xb290     thunk
 * 112  0x70  HvxImageUserMode_Thunk                   0xb2a8     thunk
 * 113  0x71  HvxImageShim_Thunk                       0xb298     thunk
 * 114  0x72  HvxExpansionInstall_Thunk                0xb250     thunk
 * 115  0x73  HvxExpansionCall_Thunk                   0xb248     thunk
 * 116  0x74  HvxDvdAuthFwcr_Thunk                     0xb1b8     thunk
 * 117  0x75  HvxDvdAuthFcrt_Thunk                     0xb1b0     thunk
 * 118  0x76  HvxDvdAuthEx_Thunk                       0xb1a8     thunk
 * 119  0x77  HvNoOp_ReturnZero                        0x2bb8     direct (no-op stub)
 * 120  0x78  HvpAquireSpinLock                        0x1b0      direct (internal HVP)
 * 121  0x79  HvpReleaseSpinLock                       0x1e0      direct (internal HVP)
 * 122  0x7A  HvpPhysicalToReal                        0x29d8     direct (internal HVP)
 * 123  0x7B  HvpRelocateCacheLines                    0xe14      direct (internal HVP)
 * 124  0x7C  HvpRelocatePhysicalToEncrypted           0x3280     direct (internal HVP)
 * 125  0x7D  HvpRelocateEncryptedToPhysical           0x3358     direct (internal HVP)
 * 126  0x7E  HvpRelocatePhysicalToProtected           0x33d8     direct (internal HVP)
 * 127  0x7F  HvpRelocatePhysicalToEncryptedStrict     0x34b8     direct (internal HVP)
 */

/* ===== Function definitions ===== */

/* ========== [0x100] _v_RESET (80 bytes) ========== */
__int64 __fastcall v_RESET(__int64 a1, __int64 a2, __int64 a3, __int64 a4, unsigned __int64 a5, unsigned __int64 a6)
{
  unsigned __int64 v6; // r3
  unsigned __int64 v7; // r12

  v6 = __mfspr(0x3F1u) & 0xF3FFFFBFFFFFFFFFLL | 0xC00004000000000LL;
  __sync();
  __mtspr(0x3F1u, v6);
  __sync();
  __isync();
  v7 = __mfspr(0x131u);
  if ( v7 == 0 )
    return HvPowerOnReset();
  if ( *(_BYTE *)(v7 + 131) == 0 )
    v_MACHINE_CHECK_0();
  *(_BYTE *)(v7 + 131) = 0;
  __mtsrr0(a5);
  __mtsrr1(a6);
  return 0;
}


/* ========== [0x150] HvpSleepThread (96 bytes) ========== */
void __noreturn HvpSleepThread()
{
  char v0; // r0
  __int64 v1; // r0
  unsigned __int64 v2; // r9

  v0 = __mfspr(0x3FFu);
  __mfsrr0();
  __mfsrr1();
  *(_BYTE *)(__mfspr(0x131u) + 131) = 1;
  __sync();
  __isync();
  v1 = v0 & 1;
  if ( (_DWORD)v1 != 1 )
  {
    v2 = (v1 << 58) & 0xC00000000000000LL | __mfspr(0x3F1u) & 0xF3FFFFFFFFFFFFFFLL;
    __sync();
    __mtspr(0x3F1u, v2);
    __sync();
    __isync();
  }
  while ( 1 )
    __mtspr(0x98u, 0);
}


/* ========== [0x1b0] HvpAquireSpinLock (48 bytes) ========== */
_DWORD *__fastcall HvpAquireSpinLock(_DWORD *result)
{
  while ( (unsigned int)__lwarx(0, (__int64)result) != 0 || !__stwcx((unsigned int)result, 0, (__int64)result) )
  {
    while ( *result != 0 )
      ;
  }
  __lwsync();
  return result;
}


/* ========== [0x1e0] HvpReleaseSpinLock (16 bytes) ========== */
_DWORD *__fastcall HvpReleaseSpinLock(_DWORD *result)
{
  __lwsync();
  *result = 0;
  return result;
}


/* ========== [0x200] _v_MACHINE_CHECK (4 bytes) ========== */
// attributes: thunk
void __fastcall __noreturn v_MACHINE_CHECK(__int64 a1)
{
  v_MACHINE_CHECK_0(a1);
}


/* ========== [0x204] _v_MACHINE_CHECK_filter (20 bytes) ========== */
int __fastcall v_MACHINE_CHECK_filter(int result)
{
  __int64 v1; // r2

  if ( result != *(_DWORD *)(v1 + 0x10000) )
    v_MACHINE_CHECK_0();
  return result;
}


/* ========== [0x218] _v_MACHINE_CHECK_0 (88 bytes) ========== */
void __noreturn v_MACHINE_CHECK_0()
{
  unsigned __int64 v0; // r3
  unsigned __int64 v1; // r3

  v0 = __mfspr(0x13Eu);
  __isync();
  __mtspr(0x13Eu, v0 & 0xFFFFFFFFFFFFFFFDLL | 2);
  __isync();
  MEMORY[0x8000020000061010] = 0xFF00000000000000LL;
  v1 = __mfspr(0x399u) & 0xFFFFFFFFFFEFFFFFLL;
  __isync();
  __mtspr(0x399u, v1);
  __isync();
  while ( 1 )
    __mtspr(0x98u, 0);
}


/* ========== [0x270] sub_270 (16 bytes) ========== */
__int64 sub_270()
{
  return 0x7601EE0000LL;
}


/* ========== [0x280] sub_280 (20 bytes) ========== */
unsigned __int64 sub_280()
{
  return 0x8000030001EF0000LL;
}


/* ========== [0x294] sub_294 (4 bytes) ========== */
// attributes: thunk
__int64 sub_294()
{
  return sub_298();
}


/* ========== [0x298] sub_298 (16 bytes) ========== */
__int64 sub_298()
{
  return 0x6401F10000LL;
}


/* ========== [0x2a8] sub_2A8 (16 bytes) ========== */
__int64 sub_2A8()
{
  return 0x6601F20000LL;
}


/* ========== [0x2b8] sub_2B8 (16 bytes) ========== */
__int64 sub_2B8()
{
  return 0x6E01F30000LL;
}


/* ========== [0x2c8] sub_2C8 (16 bytes) ========== */
__int64 sub_2C8()
{
  return 0x6C01F40000LL;
}


/* ========== [0x2d8] sub_2D8 (16 bytes) ========== */
__int64 sub_2D8()
{
  return 0x6801F50000LL;
}


/* ========== [0x2e8] sub_2E8 (16 bytes) ========== */
__int64 sub_2E8()
{
  return 0x6A01F60000LL;
}


/* ========== [0x300] _v_DATA_STORAGE (100 bytes) ========== */
__int64 __fastcall v_DATA_STORAGE(__int64 a1, __int64 a2, __int64 a3)
{
  unsigned __int64 v3; // r13
  unsigned __int64 v5; // r13
  unsigned __int8 v6; // cr35
  unsigned __int64 v7; // sp
  unsigned int v8; // r3
  int v9; // sp
  char v10; // cr32
  _DWORD *v12; // r4
  __int64 v13; // sp
  unsigned __int64 v14; // sp
  _DWORD *v16; // r4
  __int64 v17; // sp
  __int64 v20; // sp
  int v23; // sp
  unsigned int v24; // r5
  unsigned __int16 back_chain; // [sp+0h] [+0h] BYREF

  __mtsprg0(v3);
  v5 = __mfspr(0x130u);
  *(_QWORD *)(v5 + 56) = &back_chain;
  *(_QWORD *)(v5 + 64) = a1;
  *(_QWORD *)(v5 + 72) = a2;
  *(_QWORD *)(v5 + 80) = a3;
  v7 = (unsigned __int64)&back_chain | ((unsigned __int64)v6 << 28);
  LODWORD(v7) = (unsigned int)v7 >> 28;
  *(_BYTE *)(v5 + 15) = v7;
  v8 = __mfdar();
  __mfsrr1();
  __mfdsisr();
  if ( v10 == 0 )
  {
    if ( (v9 & 0x40000000) == 0 && &back_chain != nullptr )
    {
      __mfspr(0x3F5u);
      __mtspr(0x3F5u, (unsigned __int64)&back_chain);
    }
    goto LABEL_27;
  }
  if ( (v9 & 0x40000000) != 0 )
  {
    if ( &back_chain == nullptr )
      goto LABEL_27;
    if ( ((unsigned int)(*(_QWORD *)((char *)&unk_8
                                   + (((_QWORD)&back_chain << 32)
                                    | (unsigned int)((unsigned int)&back_chain + ((8LL * (_QWORD)&back_chain) & 0x10)))) >> (((v8 >> 27) & 0x1E) + 32))
        & 3) >= 2 )
    {
      if ( ((*(_QWORD *)((char *)&unk_8
                       + (((_QWORD)&back_chain << 32)
                        | (unsigned int)((unsigned int)&back_chain + ((8LL * (_QWORD)&back_chain) & 0x10)))) >> (((v8 >> 27) & 0x1E) + 32))
          & 3) == 3 )
      {
        v20 = HIDWORD(STACK[0x400]);
        if ( (_DWORD)v20 == 0 )
          goto LABEL_27;
        if ( ((unsigned __int64)&back_chain & 0x80) == 0 && (__mfdsisr() & 0x2000000) != 0 )
        {
          _SP = (unsigned __int64)&back_chain | 0x80;
          HIDWORD(STACK[0x400]) = _SP;
          __asm { tlbiel    r3, r1, 0,0,0 }
          __eieio();
          __ptesync();
        }
      }
      else
      {
        v16 = (_DWORD *)(((unsigned __int64)HIWORD(STACK[0x800]) << 13) | (4 * (_WORD)v8) & 0x1FFC
                                                                        | 0x8000000000000000LL);
        if ( HIWORD(STACK[0x800]) == 0 )
          goto LABEL_27;
        v17 = (unsigned int)*v16;
        if ( (_DWORD)v17 == 0 )
          goto LABEL_27;
        if ( ((unsigned __int64)&back_chain & 0x80) == 0 && (__mfdsisr() & 0x2000000) != 0 )
        {
          _SP = (unsigned __int64)&back_chain | 0x80;
          *v16 = _SP;
          __asm { tlbiel    r3, r1, 0,0,0 }
          __eieio();
          __ptesync();
        }
      }
      JUMPOUT(0x1188);
    }
    v12 = (_DWORD *)(((unsigned __int64)back_chain << 13)
                   | __builtin_rotateright32(v8, 10) & 0x1FFC
                   | 0x8000000000000000LL);
    if ( back_chain != 0 )
    {
      v13 = (unsigned int)*v12;
      if ( (_DWORD)v13 != 0 )
      {
        if ( ((unsigned __int64)&back_chain & 0x80) == 0 && (__mfdsisr() & 0x2000000) != 0 )
        {
          v14 = (unsigned __int64)&back_chain | 0x80;
          *v12 = v14;
          __asm { tlbiel    r3, r0, 0,0,0 }
          __eieio();
          __ptesync();
        }
        JUMPOUT(0x116C);
      }
    }
LABEL_27:
    v23 = __mfdsisr();
    *(_DWORD *)(v5 + 100) = v23;
    *(_DWORD *)(v5 + 96) = v8;
    *(_QWORD *)(v5 + 48) = __mfsprg0();
    v24 = __mfsrr1();
    *(_QWORD *)(v5 + 40) = ((_QWORD)&back_chain << 32) | v24;
    *(_QWORD *)(v5 + 32) = ((_QWORD)&back_chain << 32) | (unsigned int)__mfsrr0();
    __mtsrr0(0xFFFFFFFF80075F90LL);
    __mtsrr1((unsigned __int64)&back_chain);
    if ( (v24 & 0x4000) == 0 )
    {
      __asm { rfid }
      JUMPOUT(0x16BC);
    }
    JUMPOUT(0xB9C);
  }
  return ((__int64 (*)(void))HIWORD(STACK[0x8C0]))();
}


/* ========== [0x364] sub_364 (16 bytes) ========== */
__int64 sub_364()
{
  __int64 v0; // r3

  v0 = sub_B158();
  return sub_3100(v0);
}


/* ========== [0x380] _v_DATA_SEGMENT (4 bytes) ========== */
// attributes: thunk
void __noreturn v_DATA_SEGMENT()
{
  v_MACHINE_CHECK_0();
}


/* ========== [0x384] HvpSetRMCIEx (20 bytes) ========== */
__int64 __fastcall HvpSetRMCIEx(__int64 a1, __int64 a2, __int64 a3)
{
  __int64 result; // r3

  if ( ((a2 | a3) & 0x20000000000LL) != 0 )
    return HvpSetRMCI();
  return result;
}


/* ========== [0x398] HvpSetRMCI (44 bytes) ========== */
int __fastcall HvpSetRMCI(__int64 a1)
{
  unsigned __int64 v1; // r4
  int result; // r3

  v1 = (2 * a1) & 2 | __mfspr(0x13Eu) & 0xFFFFFFFFFFFFFFFDLL;
  __isync();
  __mtspr(0x13Eu, v1);
  __isync();
  __sync();
  __isync();
  __asm { tlbiel    r3, r1, 0,0,0 }
  return result;
}


/* ========== [0x3c4] HvpZeroCacheLines (28 bytes) ========== */
__int64 __fastcall HvpZeroCacheLines(__int64 result, __int64 a2)
{
  __int64 i; // ctr

  for ( i = a2; i != 0; --i )
  {
    __dcbz128(0, result);
    result += 128;
  }
  return result;
}


/* ========== [0x3e0] sub_3E0 (16 bytes) ========== */
unsigned __int64 __fastcall sub_3E0(unsigned __int64 result, unsigned __int64 a2, unsigned __int64 a3)
{
  __mtspr(0x3B8u, result);
  __mtspr(0x3B9u, a2);
  __mtspr(0x3BAu, a3);
  return result;
}


/* ========== [0x3f0] sub_3F0 (16 bytes) ========== */
unsigned __int64 __fastcall sub_3F0(unsigned __int64 result, unsigned __int64 a2, unsigned __int64 a3)
{
  __mtspr(0x3BBu, result);
  __mtspr(0x3BCu, a2);
  __mtspr(0x3BDu, a3);
  return result;
}


/* ========== [0x400] _v_INSTRUCTION_STORAGE (100 bytes) ========== */
__int64 __fastcall v_INSTRUCTION_STORAGE(__int64 a1, __int64 a2, __int64 a3)
{
  unsigned __int64 v3; // r13
  unsigned __int64 v5; // r13
  unsigned __int8 v6; // cr35
  unsigned __int64 v7; // sp
  unsigned int v8; // r3
  int v9; // sp
  char v10; // cr32
  unsigned int v11; // sp
  int v13; // sp
  _DWORD *v14; // r4
  __int64 v15; // sp
  unsigned __int64 v16; // sp
  _DWORD *v18; // r4
  __int64 v19; // sp
  __int64 v22; // sp
  unsigned __int16 v25; // [sp+0h] [+0h] BYREF

  __mtsprg0(v3);
  v5 = __mfspr(0x130u);
  *(_QWORD *)(v5 + 56) = &v25;
  *(_QWORD *)(v5 + 64) = a1;
  *(_QWORD *)(v5 + 72) = a2;
  *(_QWORD *)(v5 + 80) = a3;
  v7 = (unsigned __int64)&v25 | ((unsigned __int64)v6 << 28);
  LODWORD(v7) = (unsigned int)v7 >> 28;
  *(_BYTE *)(v5 + 15) = v7;
  __mtdsisr(v7);
  v8 = __mfsrr0();
  __mfsrr1();
  if ( v10 == 0 )
    goto LABEL_26;
  if ( (v9 & 0x40000000) != 0 )
  {
    if ( &v25 != nullptr )
    {
      if ( ((unsigned int)(*(_QWORD *)((char *)&unk_8
                                     + (((_QWORD)&v25 << 32)
                                      | (unsigned int)((unsigned int)&v25 + ((8LL * (_QWORD)&v25) & 0x10)))) >> (((v8 >> 27) & 0x1E) + 32))
          & 3) >= 2 )
      {
        if ( ((*(_QWORD *)((char *)&unk_8
                         + (((_QWORD)&v25 << 32) | (unsigned int)((unsigned int)&v25 + ((8LL * (_QWORD)&v25) & 0x10)))) >> (((v8 >> 27) & 0x1E) + 32))
            & 3) == 3 )
        {
          v22 = HIDWORD(STACK[0x400]);
          if ( (_DWORD)v22 != 0 )
          {
            if ( ((unsigned __int64)&v25 & 0x80) == 0 && (__mfdsisr() & 0x2000000) != 0 )
            {
              _SP = (unsigned __int64)&v25 | 0x80;
              HIDWORD(STACK[0x400]) = _SP;
              __asm { tlbiel    r3, r1, 0,0,0 }
              __eieio();
              __ptesync();
            }
            JUMPOUT(0x137C);
          }
        }
        else
        {
          v18 = (_DWORD *)(((unsigned __int64)HIWORD(STACK[0x800]) << 13)
                         | (4 * (_WORD)v8) & 0x1FFC
                         | 0x8000000000000000LL);
          if ( HIWORD(STACK[0x800]) != 0 )
          {
            v19 = (unsigned int)*v18;
            if ( (_DWORD)v19 != 0 )
            {
              if ( ((unsigned __int64)&v25 & 0x80) == 0 && (__mfdsisr() & 0x2000000) != 0 )
              {
                _SP = (unsigned __int64)&v25 | 0x80;
                *v18 = _SP;
                __asm { tlbiel    r3, r1, 0,0,0 }
                __eieio();
                __ptesync();
              }
              JUMPOUT(0x1284);
            }
          }
        }
      }
      else
      {
        v14 = (_DWORD *)(((unsigned __int64)v25 << 13) | __builtin_rotateright32(v8, 10) & 0x1FFC | 0x8000000000000000LL);
        if ( v25 != 0 )
        {
          v15 = (unsigned int)*v14;
          if ( (_DWORD)v15 != 0 )
          {
            if ( ((unsigned __int64)&v25 & 0x80) == 0 && (__mfdsisr() & 0x2000000) != 0 )
            {
              v16 = (unsigned __int64)&v25 | 0x80;
              *v14 = v16;
              __asm { tlbiel    r3, r0, 0,0,0 }
              __eieio();
              __ptesync();
            }
            JUMPOUT(0x116C);
          }
        }
      }
      JUMPOUT(0x1624);
    }
LABEL_26:
    JUMPOUT(0x1634);
  }
  v11 = v8 >> 28;
  if ( v11 == 8 )
    return sub_12BC();
  if ( v13 != 9 )
    goto LABEL_26;
  return sub_10C4();
}


/* ========== [0x464] sub_464 (24 bytes) ========== */
__int64 sub_464()
{
  return 0x300001A3680LL;
}


/* ========== [0x480] _v_INSTRUCTION_SEGMENT (4 bytes) ========== */
// attributes: thunk
void __noreturn v_INSTRUCTION_SEGMENT()
{
  v_MACHINE_CHECK_0();
}


/* ========== [0x484] CopyBy64 (36 bytes) ========== */
__int64 __fastcall CopyBy64(__int64 a1, unsigned int *a2, __int64 a3)
{
  __int64 result; // r3

  result = a1 - (_QWORD)a2;
  do
  {
    *(_QWORD *)((char *)a2 + result) = ((unsigned __int64)*a2 << 32) | a2[1];
    a2 += 2;
    --a3;
  }
  while ( a3 != 0 );
  return result;
}


/* ========== [0x4a8] Sleep (36 bytes) ========== */
unsigned __int64 __fastcall Sleep(unsigned int a1)
{
  unsigned __int64 result; // r3
  unsigned __int64 v2; // r4

  result = 50LL * a1;
  v2 = __mftb();
  while ( result > __mftb() - v2 )
    ;
  return result;
}


/* ========== [0x4cc] sub_4CC (12 bytes) ========== */
unsigned __int64 sub_4CC()
{
  __int64 v0; // lr

  return v0 & 0xFFFFFFFFFFFF0000LL;
}


/* ========== [0x4d8] HvpGetTimebase (16 bytes) ========== */
unsigned __int64 HvpGetTimebase()
{
  unsigned __int64 result; // r3

  do
    result = __mftb();
  while ( (_DWORD)result == 0 );
  return result;
}


/* ========== [0x4e8] sub_4E8 (20 bytes) ========== */
unsigned __int16 __fastcall sub_4E8(unsigned __int16 result, __int64 a2, unsigned __int64 *a3)
{
  unsigned __int64 v3; // r4

  v3 = ((((unsigned __int64)result << 16) | a2 & 0xFFFFFFFF0000FFFFLL) << 32)
     | (result << 16)
     | (unsigned int)(unsigned __int16)a2;
  *a3 = v3;
  a3[1] = v3;
  return result;
}


/* ========== [0x500] _v_EXTERNAL (124 bytes) ========== */
void __fastcall v_EXTERNAL(__int64 a1, __int64 a2)
{
  unsigned __int64 v2; // r13
  _QWORD *v3; // r13
  __int64 v4; // r4
  bool v5; // cr49
  __int64 v6; // r4
  bool v7; // zf
  __int64 v8; // [sp+0h] [+0h] BYREF

  __mtsprg0(v2);
  v3 = (_QWORD *)__mfspr(0x130u);
  v3[7] = &v8;
  __mfsprg0();
  v3[9] = a2;
  v3[6] = &v8;
  v3[4] = ((_QWORD)&v8 << 32) | (unsigned int)__mfsrr0();
  v4 = __mfsrr1();
  v5 = (v4 & 0x4000) != 0;
  v7 = v4 < 0;
  v6 = ((_QWORD)&v8 << 32) | (unsigned int)v4;
  if ( !v7 )
  {
    v3[5] = v6;
    __mtsrr0(0xFFFFFFFF80075F90LL);
    __mtsrr1((unsigned __int64)&v8);
    if ( !v5 )
      __asm { rfid }
    JUMPOUT(0xB9C);
  }
  v_MACHINE_CHECK_0();
}


/* ========== [0x57c] sub_57C (44 bytes) ========== */
__int64 __fastcall sub_57C(unsigned int a1)
{
  __int16 v1; // r12
  __int64 result; // r3
  unsigned int v3; // r4

  v1 = a1;
  v3 = (a1 >> 28) - 8;
  result = 0;
  if ( v3 <= 1 )
    return (4 * v1) & 0x7FFC | 0x6801F50000LL;
  return result;
}


/* ========== [0x5a8] sub_5A8 (24 bytes) ========== */
__int64 sub_5A8()
{
  return 0x6801F58000LL;
}


/* ========== [0x5c0] sub_5C0 (24 bytes) ========== */
__int64 sub_5C0()
{
  return 0x6A01F6C000LL;
}


/* ========== [0x5d8] sub_5D8 (20 bytes) ========== */
__int64 __fastcall sub_5D8(__int64 a1)
{
  __int64 v1; // r2

  return ((a1 << 8) & 0x100) + v1 + 65840;
}


/* ========== [0x5ec] sub_5EC (20 bytes) ========== */
__int64 __fastcall sub_5EC(__int64 a1)
{
  __int64 v1; // r2

  return ((16 * a1) & 0x10) + v1 + 66352;
}


/* ========== [0x600] _v_ALIGNMENT (140 bytes) ========== */
void __fastcall v_ALIGNMENT(__int64 a1, __int64 a2)
{
  unsigned __int64 v2; // r13
  unsigned __int64 v3; // r13
  __int64 v4; // r4
  bool v5; // cr49
  __int64 v6; // r4
  bool v7; // zf
  int v8; // sp
  int v9; // sp
  __int64 v10; // [sp+0h] [+0h] BYREF

  __mtsprg0(v2);
  v3 = __mfspr(0x130u);
  *(_QWORD *)(v3 + 56) = &v10;
  __mfsprg0();
  *(_QWORD *)(v3 + 72) = a2;
  *(_QWORD *)(v3 + 48) = &v10;
  *(_QWORD *)(v3 + 32) = ((_QWORD)&v10 << 32) | (unsigned int)__mfsrr0();
  v4 = __mfsrr1();
  v5 = (v4 & 0x4000) != 0;
  v7 = v4 < 0;
  v6 = ((_QWORD)&v10 << 32) | (unsigned int)v4;
  if ( !v7 )
  {
    *(_QWORD *)(v3 + 40) = v6;
    v8 = __mfdar();
    *(_DWORD *)(v3 + 96) = v8;
    v9 = __mfdsisr();
    *(_DWORD *)(v3 + 100) = v9;
    __mtsrr0(0xFFFFFFFF80075F90LL);
    __mtsrr1((unsigned __int64)&v10);
    if ( !v5 )
      __asm { rfid }
    JUMPOUT(0xB9C);
  }
  v_MACHINE_CHECK_0();
}


/* ========== [0x68c] HvpGetFlashBase (20 bytes) ========== */
unsigned __int64 HvpGetFlashBase()
{
  return 0x80000200C8000000LL;
}


/* ========== [0x6a0] HvpGetSocMMIORegs (20 bytes) ========== */
__int64 __fastcall HvpGetSocMMIORegs(__int64 a1)
{
  return a1 - 0x7FFFFE0000000000LL;
}


/* ========== [0x6b4] HvpBuildPciConfigRegs (24 bytes) ========== */
__int64 __fastcall HvpBuildPciConfigRegs(__int64 a1)
{
  return a1 - 0x7FFFFDFF30000000LL;
}


/* ========== [0x6cc] HvpGetHostBridgeRegs (24 bytes) ========== */
__int64 __fastcall HvpGetHostBridgeRegs(__int64 a1)
{
  return a1 - 0x7FFFFDFF20000000LL;
}


/* ========== [0x700] _v_PROGRAM (124 bytes) ========== */
void __fastcall v_PROGRAM(__int64 a1, __int64 a2)
{
  unsigned __int64 v2; // r13
  _QWORD *v3; // r13
  __int64 v4; // r4
  bool v5; // cr49
  __int64 v6; // r4
  bool v7; // zf
  __int64 v8; // [sp+0h] [+0h] BYREF

  __mtsprg0(v2);
  v3 = (_QWORD *)__mfspr(0x130u);
  v3[7] = &v8;
  __mfsprg0();
  v3[9] = a2;
  v3[6] = &v8;
  v3[4] = ((_QWORD)&v8 << 32) | (unsigned int)__mfsrr0();
  v4 = __mfsrr1();
  v5 = (v4 & 0x4000) != 0;
  v7 = v4 < 0;
  v6 = ((_QWORD)&v8 << 32) | (unsigned int)v4;
  if ( !v7 )
  {
    v3[5] = v6;
    __mtsrr0(0xFFFFFFFF80075F90LL);
    __mtsrr1((unsigned __int64)&v8);
    if ( !v5 )
      __asm { rfid }
    JUMPOUT(0xB9C);
  }
  v_MACHINE_CHECK_0();
}


/* ========== [0x77c] jt1_jumper (92 bytes) ========== */
__int64 jt1_jumper()
{
  __int64 v0; // r11

  return ((__int64 (*)(void))(*(unsigned int *)((char *)&SecondaryJumpTable + v0)
                            | ((unsigned __int64)*(unsigned int *)((char *)&SecondaryJumpTable + v0) << 17)
                            & 0x1FFFE00000000LL))();
}


/* ========== [0x800] _v_FPU_UNAVAILABLE (256 bytes) ========== */
void __fastcall v_FPU_UNAVAILABLE(__int64 a1, __int64 a2)
{
  unsigned __int64 v2; // r13
  _QWORD *v3; // r13
  unsigned int v4; // r4
  bool v5; // cr49
  __int64 v6; // [sp+0h] [+0h] BYREF

  __mtsprg0(v2);
  v3 = (_QWORD *)__mfspr(0x130u);
  v3[7] = &v6;
  v3[9] = a2;
  __mfsrr1();
  if ( (__int64)&v6 < 0 )
    v_MACHINE_CHECK_0();
  if ( &v6 != nullptr )
  {
    __mtsrr1((unsigned __int64)&v6 | __mfsrr1());
    __mfsprg0();
    __asm { rfid }
  }
  __mfsprg0();
  v3[6] = &v6;
  v3[4] = ((_QWORD)&v6 << 32) | (unsigned int)__mfsrr0();
  v4 = __mfsrr1();
  v5 = (v4 & 0x4000) != 0;
  v3[5] = ((_QWORD)&v6 << 32) | v4;
  __mtsrr0(0xFFFFFFFF80075F90LL);
  __mtsrr1((unsigned __int64)&v6);
  LODWORD(a2) = -2146999516;
  if ( !v5 )
  {
    __asm { rfid }
    JUMPOUT(0x8B4);
  }
  v_Reserved_B9C(a1, a2);
}


/* ========== [0x900] _v_DECREMENTER (128 bytes) ========== */
__int64 __fastcall v_DECREMENTER(__int64 a1, __int64 a2)
{
  unsigned __int64 v2; // r13
  _QWORD *v3; // r13
  __int64 v4; // r4
  bool v5; // cr49
  __int64 v6; // r4
  bool v7; // zf
  __int64 v9; // [sp+0h] [+0h] BYREF

  __mtsprg0(v2);
  v3 = (_QWORD *)__mfspr(0x130u);
  v3[7] = &v9;
  __mfsprg0();
  v3[9] = a2;
  v3[6] = &v9;
  v3[4] = ((_QWORD)&v9 << 32) | (unsigned int)__mfsrr0();
  v4 = __mfsrr1();
  v5 = (v4 & 0x4000) != 0;
  v7 = v4 < 0;
  v6 = ((_QWORD)&v9 << 32) | (unsigned int)v4;
  if ( v7 )
    v_MACHINE_CHECK_0();
  v3[5] = v6;
  __mtsrr0(0xFFFFFFFF80075F90LL);
  __mtsrr1((unsigned __int64)&v9);
  if ( !v5 )
  {
    __asm { rfid }
    JUMPOUT(0x97C);
  }
  return v_Reserved_B9C(a1, -2146999684);
}


/* ========== [0x980] _v_HYPERVISOR_DECREMENTER (24 bytes) ========== */
void __fastcall v_HYPERVISOR_DECREMENTER(__int64 a1, unsigned __int64 a2)
{
  __mtsprg0(a2);
  __mtspr(0x136u, 0x7FFFFFFFu);
  __mfsprg0();
  __asm { hrfid }
}


/* ========== [0x998] HvSyscallReturn (120 bytes) ========== */
/* Syscall return path: restores SLB, SRR0/SRR1, CR, r13 from thread state; rfid. Reached when caller r1 == 0 */
// Syscall return path: restores SLB, SRR0/SRR1, CR, r13 from thread state; rfid. Reached when caller r1 == 0
void __fastcall HvSyscallReturn(__int64 a1, __int64 a2, __int64 a3)
{
  __int64 v3; // r13
  __int64 v4; // sp
  __int64 *v5; // r5
  int v7; // r4
  bool v8; // cr34
  __int64 v11; // [sp+0h] [+0h] BYREF

  v4 = *(unsigned int *)(v3 + 44);
  __mtsrr1((unsigned __int64)&v11);
  if ( (v4 & 0x4000) != 0 )
  {
    *(_QWORD *)(v3 + 80) = a3;
    __asm { slbia     0 }
    v5 = (__int64 *)((((unsigned __int64)*(unsigned int *)(v3 + 88) << 7) & 0x180) + (((_QWORD)&v11 << 32) | 0x10130LL));
    do
    {
      _SP = *v5++;
      v7 = (unsigned __int16)_SP >> 12;
      v8 = v7 == 15;
      _R4 = ((v7 | 0x8000000) << 28) | v7 | 0x8000000;
      __asm { slbmte    r1, r4 }
    }
    while ( !v8 );
  }
  _SP = *(unsigned int *)(v3 + 40);
  __asm { mtcr      r1 }
  __mtsrr0(*(unsigned int *)(v3 + 36));
  __mfsprg0();
  __asm { rfid }
}


/* ========== [0xa10] HvSyscallDispatch_High (76 bytes) ========== */
/* Alt dispatch path for syscall numbers >= 120 (0x78) - undocumented/internal */
// Alt dispatch path for syscall numbers >= 120 (0x78) - undocumented/internal
void HvSyscallDispatch_High()
{
  __int64 v0; // r13
  __int64 v1; // [sp+0h] [+0h] BYREF

  *(_QWORD *)(v0 + 40) = &v1;
  *(_QWORD *)(v0 + 32) = ((_QWORD)&v1 << 32) | (unsigned int)__mfsrr0();
  __mtsrr0(0xFFFFFFFF80075F90LL);
  __mtsrr1((unsigned __int64)&v1);
  __asm { rfid }
}


/* ========== [0xa5c] _v_FPU_Assist (168 bytes) ========== */
void v_FPU_Assist()
{
  _QWORD *v0; // r13
  unsigned __int64 v1; // r0
  __int64 v2; // [sp+0h] [+0h] BYREF

  v0[5] = &v2;
  __mfsprg0();
  v0[4] = ((_QWORD)&v2 << 32) | (unsigned int)__mfsrr0();
  v0[6] = &v2;
  if ( v1 > 1 )
  {
    __mtsrr0(0xFFFFFFFF80075F90LL);
    JUMPOUT(0xA90);
  }
  JUMPOUT(0xAA0);
}


/* ========== [0xb04] HvSyscallDispatch (152 bytes) ========== */
/* Syscall dispatcher: r0=syscall num. If r0 >= 0x78 goes to HvSyscallDispatch_High; else indexes _SyscallTable (0x15FD0) and blrl */
// Syscall dispatcher: r0=syscall num. If r0 >= 0x78 goes to HvSyscallDispatch_High; else indexes _SyscallTable (0x15FD0) and blrl
// local variable allocation has failed, the output may be wrong!
void __fastcall HvSyscallDispatch(__int64 a1, __int64 a2)
{
  _QWORD *v2; // r13
  __int64 v3; // r2
  unsigned int v4; // r0
  void (__fastcall *v5)(__int64, __int64); // lr
  __int64 v6; // r4
  __int64 v7; // [sp+0h] [+0h] BYREF

  if ( v4 < 0x78 )
  {
    v2[5] = a2;
    v2[4] = &v7;
    if ( HIDWORD(STACK[0x200015FD0]) <= 0xFFFF )
    {
      v5 = (void (__fastcall *)(__int64, __int64))HIDWORD(STACK[0x200015FD0]);
      v6 = v2[9];
      __mfspr(0x131u);
      *(&v7 - 1) = v3;
      v5(a1, v6);
      JUMPOUT(0xB60);
    }
    v_MACHINE_CHECK_0();
  }
  HvSyscallDispatch_High();
}


/* ========== [0xb9c] _v_Reserved_B9C (92 bytes) ========== */
void __fastcall v_Reserved_B9C(__int64 a1, __int64 a2, __int64 a3)
{
  __int64 v3; // r13
  __int64 v4; // r3
  unsigned __int16 v5; // r5
  int v6; // r4
  bool v7; // cr34

  *(_QWORD *)(v3 + 80) = a3;
  *(_QWORD *)(v3 + 64) = a1;
  v4 = 0;
  __asm { slbia     0 }
  do
  {
    v5 = *(_WORD *)(v4 + 2272);
    v4 += 2;
    v6 = v5 >> 12;
    v7 = v6 == 15;
    _R4 = ((v6 | 0x8000000) << 28) | v6 | 0x8000000;
    __asm { slbmte    r5, r4 }
  }
  while ( !v7 );
  __asm { rfid }
  JUMPOUT(0xBF8);
}


/* ========== [0xc00] _v_SYSTEM_CALL (52 bytes) ========== */
void __fastcall v_SYSTEM_CALL(__int64 a1, __int64 a2)
{
  unsigned __int64 v2; // r13
  unsigned __int64 v3; // r13
  __int16 v4; // sp
  int v5; // sp
  __int64 v6; // [sp+0h] [+0h] BYREF

  __mtsprg0(v2);
  v3 = __mfspr(0x130u);
  *(_QWORD *)(v3 + 56) = &v6;
  *(_QWORD *)(v3 + 72) = a2;
  __mfsrr1();
  if ( (__int64)&v6 >= 0 )
  {
    if ( (v4 & 0x4000) == 0 )
    {
      if ( v5 == 0 )
        JUMPOUT(0x998);
      JUMPOUT(0xB04);
    }
    JUMPOUT(0xA5C);
  }
  v_MACHINE_CHECK_0();
}


/* ========== [0xc34] HvxSetTimebaseToZero (16 bytes) ========== */
void __fastcall HvxSetTimebaseToZero()
{
  __mtspr(0x11Cu, 0);
  __mtspr(0x11Du, 0);
}


/* ========== [0xc44] HvxStartupProcessors (164 bytes) ========== */
void __fastcall HvxStartupProcessors()
{
  if ( (unsigned int)__mfspr(0x3FFu) == 0 )
  {
    MEMORY[0x800002000001FFF8] = 0x3FFFC0003FFFC00LL;
    MEMORY[0x800002000001FFF0] = 0x3FFFC0003FFFC00LL;
    __sync();
    MEMORY[0x8000020000052070] = 124;
    __eieio();
    MEMORY[0x8000020000052008] = 0;
    __eieio();
    MEMORY[0x8000020000052000] = 4;
    __eieio();
    MEMORY[0x8000020000054070] = 124;
    __eieio();
    MEMORY[0x8000020000054008] = 0;
    __eieio();
    MEMORY[0x8000020000054000] = 16;
    __eieio();
    MEMORY[0x8000020000050010] = 1310840;
    __eieio();
    __mtspr(0x98u, 0xC00000u);
  }
}


/* ========== [0xd00] _v_TRACE (124 bytes) ========== */
void __fastcall v_TRACE(__int64 a1, __int64 a2)
{
  unsigned __int64 v2; // r13
  _QWORD *v3; // r13
  __int64 v4; // r4
  bool v5; // cr49
  __int64 v6; // r4
  bool v7; // zf
  __int64 v8; // [sp+0h] [+0h] BYREF

  __mtsprg0(v2);
  v3 = (_QWORD *)__mfspr(0x130u);
  v3[7] = &v8;
  __mfsprg0();
  v3[9] = a2;
  v3[6] = &v8;
  v3[4] = ((_QWORD)&v8 << 32) | (unsigned int)__mfsrr0();
  v4 = __mfsrr1();
  v5 = (v4 & 0x4000) != 0;
  v7 = v4 < 0;
  v6 = ((_QWORD)&v8 << 32) | (unsigned int)v4;
  if ( !v7 )
  {
    v3[5] = v6;
    __mtsrr0(0xFFFFFFFF80075F90LL);
    __mtsrr1((unsigned __int64)&v8);
    if ( !v5 )
      __asm { rfid }
    JUMPOUT(0xB9C);
  }
  v_MACHINE_CHECK_0();
}


/* ========== [0xd7c] sub_D7C (76 bytes) ========== */
unsigned __int64 __fastcall sub_D7C(unsigned __int64 result, __int64 a2)
{
  __int64 v2; // r4
  unsigned __int64 v3; // r3
  unsigned __int64 v4; // r4
  unsigned __int64 v5; // ctr

  if ( (_DWORD)a2 != 0 )
  {
    v2 = result + a2 - 1;
    v3 = result & 0xFFFFFFFFFFFFFF80LL;
    v4 = (v2 & 0xFFFFFFFFFFFFFF80LL) - v3;
    LODWORD(v4) = (unsigned int)v4 >> 7;
    v5 = v4 + 1;
    result = v3 & 0x7FFFFFFFFFFFFFFFLL | 0x8000000000000000LL;
    do
    {
      __dcbf(0, result);
      __icbi(0, result);
      result += 128LL;
      --v5;
    }
    while ( v5 != 0 );
    __sync();
    __isync();
  }
  return result;
}


/* ========== [0xdc8] sub_DC8 (76 bytes) ========== */
unsigned __int64 __fastcall sub_DC8(unsigned __int64 result, __int64 a2)
{
  __int64 v2; // r4
  unsigned __int64 v3; // r3
  unsigned __int64 v4; // r4
  unsigned __int64 v5; // ctr

  if ( (_DWORD)a2 != 0 )
  {
    v2 = result + a2 - 1;
    v3 = result & 0xFFFFFFFFFFFFFF80LL;
    v4 = (v2 & 0xFFFFFFFFFFFFFF80LL) - v3;
    LODWORD(v4) = (unsigned int)v4 >> 7;
    v5 = v4 + 1;
    result = v3 & 0x7FFFFFFFFFFFFFFFLL | 0x8000000000000000LL;
    do
    {
      __dcbst(0, result);
      __icbi(0, result);
      result += 128LL;
      --v5;
    }
    while ( v5 != 0 );
    __sync();
    __isync();
  }
  return result;
}


/* ========== [0xe14] HvpRelocateCacheLines (208 bytes) ========== */
__int64 *__fastcall HvpRelocateCacheLines(__int64 *result, _QWORD *a2, __int64 a3)
{
  __int64 v3; // r6
  __int64 v4; // ctr
  __int64 v5; // ctr
  __int64 v6; // r16
  __int64 v7; // r17
  __int64 v8; // r18
  __int64 v9; // r19
  __int64 v10; // r20
  __int64 v11; // r21
  __int64 v12; // r22
  __int64 v13; // r23
  __int64 v14; // r24
  __int64 v15; // r25
  __int64 v16; // r26
  __int64 v17; // r27
  __int64 v18; // r28
  __int64 v19; // r29
  __int64 v20; // r30
  __int64 v21; // r31

  v3 = 0;
  v4 = a3;
  do
  {
    __dcbst((__int64)result, v3);
    v3 += 128;
    --v4;
  }
  while ( v4 != 0 );
  __sync();
  __isync();
  v5 = a3;
  do
  {
    v6 = *result;
    v7 = result[1];
    v8 = result[2];
    v9 = result[3];
    v10 = result[4];
    v11 = result[5];
    v12 = result[6];
    v13 = result[7];
    v14 = result[8];
    v15 = result[9];
    v16 = result[10];
    v17 = result[11];
    v18 = result[12];
    v19 = result[13];
    v20 = result[14];
    v21 = result[15];
    __dcbf(0, (__int64)result);
    __icbi(0, (__int64)result);
    result += 16;
    __dcbz128(0, (__int64)a2);
    *a2 = v6;
    a2[1] = v7;
    a2[2] = v8;
    a2[3] = v9;
    a2[4] = v10;
    a2[5] = v11;
    a2[6] = v12;
    a2[7] = v13;
    a2[8] = v14;
    a2[9] = v15;
    a2[10] = v16;
    a2[11] = v17;
    a2[12] = v18;
    a2[13] = v19;
    a2[14] = v20;
    a2[15] = v21;
    __icbi(0, (__int64)a2);
    a2 += 16;
    --v5;
  }
  while ( v5 != 0 );
  __sync();
  __isync();
  return result;
}


/* ========== [0xee4] HvpSaveCachelines (36 bytes) ========== */
_BYTE *__fastcall HvpSaveCachelines(_BYTE *result, __int64 a2)
{
  do
  {
    *result = *result;
    __dcbst(0, (__int64)result);
    result += 128;
    --a2;
  }
  while ( a2 != 0 );
  __sync();
  __isync();
  return result;
}


/* ========== [0xf20] _v_VPU_UNAVAILABLE (180 bytes) ========== */
void __fastcall v_VPU_UNAVAILABLE(__int64 a1, __int64 a2)
{
  unsigned __int64 v2; // r13
  _QWORD *v3; // r13
  unsigned int v4; // r4
  __int64 v5; // [sp+0h] [+0h] BYREF

  __mtsprg0(v2);
  v3 = (_QWORD *)__mfspr(0x130u);
  v3[7] = &v5;
  v3[9] = a2;
  __mfsrr1();
  if ( (__int64)&v5 >= 0 )
  {
    if ( &v5 != nullptr )
    {
      __mtsrr1((unsigned __int64)&v5 | __mfsrr1());
      __mfsprg0();
      __asm { rfid }
    }
    __mfsprg0();
    v3[6] = &v5;
    v3[4] = ((_QWORD)&v5 << 32) | (unsigned int)__mfsrr0();
    v4 = __mfsrr1();
    v3[5] = ((_QWORD)&v5 << 32) | v4;
    __mtsrr0(0xFFFFFFFF80075F90LL);
    __mtsrr1((unsigned __int64)&v5);
    if ( (v4 & 0x4000) == 0 )
      __asm { rfid }
    JUMPOUT(0xB9C);
  }
  v_MACHINE_CHECK_0();
}


/* ========== [0xfd4] sub_FD4 (68 bytes) ========== */
__int64 sub_FD4()
{
  __int64 v0; // r3
  __int64 i; // ctr
  __int64 result; // r3
  __int64 j; // ctr

  v0 = 0x8000000001F70000LL;
  for ( i = 512; i != 0; --i )
  {
    __dcbz128(0, v0);
    v0 += 128;
  }
  result = 0x8000000001F80000LL;
  for ( j = 4096; j != 0; --j )
  {
    __dcbz128(0, result);
    result += 128;
  }
  return result;
}


/* ========== [0x1018] sub_1018 (88 bytes) ========== */
void sub_1018()
{
  int v0; // sp

  v0 = __mfsrr0();
  if ( v0 == -2147000432 )
  {
    __mtsrr1(0x1030u);
    __asm { rfid }
  }
  JUMPOUT(0x1634);
}


/* ========== [0x1070] sub_1070 (84 bytes) ========== */
void sub_1070()
{
  int back_chain; // [sp+0h] [+0h] BYREF

  if ( ((unsigned __int64)&back_chain & 1) != 0 )
    JUMPOUT(0x1160);
  sub_1018();
}


/* ========== [0x10c4] sub_10C4 (388 bytes) ========== */
void __fastcall sub_10C4(unsigned __int64 a1)
{
  unsigned __int64 v1; // r13
  unsigned __int64 v2; // r4
  unsigned __int64 v3; // r13
  unsigned __int64 v4; // r3
  unsigned __int64 v5; // r13
  __int64 v6; // [sp+0h] [+0h] BYREF

  if ( (*(_DWORD *)(((a1 >> 14) & 0x7FFC) + 0x6801F50000LL) & 0xFu) >= 8
    && ((*(unsigned __int8 *)((unsigned __int16)__builtin_rotateright32(a1, 13) | 0x6801F50000LL) >> (((unsigned int)a1 >> 10) & 4))
      & 0xF) != 0 )
  {
    v1 = __mfspr(0x131u);
    v2 = __mfspr(0x3B2u) & 0xF | (((a1 >> 20) & 7) << 45) | ((a1 >> 8) & 0xFF0 ^ (a1 >> 16) & 0xF00) & 0xFF0;
    v3 = (2 * (_BYTE)v1) & 0x40 | v1 & 0xFFFFFFFFFFFFFFBFLL;
    v4 = (a1 >> 16) & 0xFFFFFFFFFF80LL | 1;
    v5 = (2 * (_WORD)v3) & 0x2000 | v3 & 0xFFFFFFFFFFFFDFFFLL;
    *(_QWORD *)v5 = v5;
    __lwsync();
    if ( *(_QWORD *)(v5 ^ 0x40) != 0 )
    {
      if ( (v5 & 0x40) != 0 )
      {
        do
        {
          *(_QWORD *)v5 = 0;
          __lwsync();
          while ( *(_QWORD *)(v5 - 64) != 0 )
            ;
          *(_QWORD *)v5 = v5;
          __lwsync();
        }
        while ( *(_QWORD *)(v5 - 64) != 0 );
      }
      else
      {
        while ( *(_QWORD *)(v5 + 64) != 0 )
          ;
      }
    }
    __mtspr(0x3B3u, v2);
    __mtspr(0x3B5u, (unsigned __int64)&v6);
    __mtspr(0x3B4u, v4);
    *(_QWORD *)v5 = 0;
    __mfspr(0x130u);
    __mfsprg0();
    __asm { rfid }
  }
  JUMPOUT(0x1634);
}


/* ========== [0x1248] sub_1248 (60 bytes) ========== */
void __fastcall sub_1248(unsigned __int64 a1)
{
  __int64 v1; // sp
  __int64 v2; // [sp+0h] [+0h] BYREF

  v1 = (a1 >> 14) & 0xFFFC;
  if ( (_DWORD)v1 == 65532 || ((unsigned __int64)&v2 & 1) != 0 )
    JUMPOUT(0x1188);
  sub_1018();
}


/* ========== [0x12bc] sub_12BC (52 bytes) ========== */
void __fastcall sub_12BC(unsigned __int64 a1)
{
  if ( (*(_DWORD *)(((a1 >> 14) & 0x7FFC) + 0x6801F50000LL) & 0xFu) >= 8 )
    JUMPOUT(0x1188);
  JUMPOUT(0x1634);
}


/* ========== [0x12f0] sub_12F0 (72 bytes) ========== */
void sub_12F0()
{
  unsigned __int64 v0; // sp
  __int64 v1; // [sp+0h] [+0h] BYREF

  v0 = (unsigned __int64)&v1 & 0xF;
  if ( (_DWORD)v0 != 0 )
    JUMPOUT(0x1188);
  JUMPOUT(0x1634);
}


/* ========== [0x1338] sub_1338 (68 bytes) ========== */
void sub_1338()
{
  unsigned __int64 v0; // sp
  __int64 v1; // [sp+0h] [+0h] BYREF

  v0 = (unsigned __int64)&v1 & 0xF;
  if ( (_DWORD)v0 != 0 )
    JUMPOUT(0x137C);
  JUMPOUT(0x1634);
}


/* ========== [0x1600] _v_MAINTENANCE (4 bytes) ========== */
// attributes: thunk
void __noreturn v_MAINTENANCE()
{
  v_MACHINE_CHECK_0();
}


/* ========== [0x1700] _v_VMX_ASSIST (124 bytes) ========== */
void __fastcall v_VMX_ASSIST(__int64 a1, __int64 a2)
{
  unsigned __int64 v2; // r13
  _QWORD *v3; // r13
  __int64 v4; // r4
  bool v5; // cr49
  __int64 v6; // r4
  bool v7; // zf
  __int64 v8; // [sp+0h] [+0h] BYREF

  __mtsprg0(v2);
  v3 = (_QWORD *)__mfspr(0x130u);
  v3[7] = &v8;
  __mfsprg0();
  v3[9] = a2;
  v3[6] = &v8;
  v3[4] = ((_QWORD)&v8 << 32) | (unsigned int)__mfsrr0();
  v4 = __mfsrr1();
  v5 = (v4 & 0x4000) != 0;
  v7 = v4 < 0;
  v6 = ((_QWORD)&v8 << 32) | (unsigned int)v4;
  if ( !v7 )
  {
    v3[5] = v6;
    __mtsrr0(0xFFFFFFFF80075F90LL);
    __mtsrr1((unsigned __int64)&v8);
    if ( !v5 )
      __asm { rfid }
    JUMPOUT(0xB9C);
  }
  v_MACHINE_CHECK_0();
}


/* ========== [0x177c] HvxEnablePPUPerformanceMonitor (8 bytes) ========== */
__int64 HvxEnablePPUPerformanceMonitor()
{
  return 0;
}


/* ========== [0x1800] _v_THERMAL_MANAGEMENT (4 bytes) ========== */
// attributes: thunk
void __noreturn v_THERMAL_MANAGEMENT()
{
  v_MACHINE_CHECK_0();
}


/* ========== [0x1804] HvpFlushSingleTb (52 bytes) ========== */
int __fastcall HvpFlushSingleTb(unsigned int a1, unsigned int a2)
{
  int result; // r3

  if ( ((a2 >> ((a1 >> 27) & 0x1E)) & 3) != 0 )
    __asm { tlbiel    r3, r1, 0,0,0 }
  else
    __asm { tlbiel    r3, r0, 0,0,0 }
  __eieio();
  __ptesync();
  return result;
}


/* ========== [0x1838] HvxFlushEntireTb (56 bytes) ========== */
void __fastcall HvxFlushEntireTb()
{
  __int64 i; // ctr

  for ( i = 256; i != 0; --i )
    __asm { tlbiel    r8, r0, 0,0,0 }
  __eieio();
  __ptesync();
  __asm { tlbiel    r8, r1, 0,0,0 }
  __eieio();
  __ptesync();
}


/* ========== [0x1870] HvPowerOnReset (672 bytes) ========== */
void HvPowerOnReset()
{
  unsigned __int64 v0; // r21
  __int64 v1; // r3
  __int64 v2; // r3
  unsigned __int64 v3; // r3
  unsigned __int64 v4; // r3
  unsigned __int64 v5; // r3
  __int64 v6; // r5
  unsigned __int16 v7; // r3
  int v8; // r4
  bool v9; // cr34
  unsigned __int64 v12; // r3
  __int64 v13; // r5
  __int64 v14; // r4
  __int64 v15; // r3
  __int64 v16; // r5
  __int64 v17; // r4
  __int64 v18; // r3
  __int64 v19; // r5
  __int64 v20; // r4
  __int64 v21; // r3
  __int64 v22; // r5
  unsigned __int64 v23; // r4
  __int64 v24; // r2
  _QWORD *v25; // r3

  v0 = __mfspr(0x3FFu) & 7;
  if ( v0 == 0 )
  {
    v1 = sub_2D8();
    HvpZeroCacheLines(v1, 512);
    v2 = sub_2E8();
    HvpZeroCacheLines(v2, 512);
  }
  if ( __mfspr(0x130u) == 0 )
  {
    if ( (v0 & 1) != 0 )
    {
      v3 = __mfspr(0x13Eu) & 0xFFFFFFFFFFFFFFFDLL | 2;
      __isync();
      __mtspr(0x13Eu, v3);
    }
    else
    {
      __mtspr(0x39Au, 0x1000u);
      __isync();
      v4 = __mfspr(0x399u) & 0xFFFFFFFF07F2FFFFLL | 0xD0000;
      __isync();
      __mtspr(0x399u, v4);
    }
    __isync();
    if ( (_DWORD)v0 != 0 && (v0 & 1) == 0 )
      __mtspr(0x98u, 0xC00000u);
  }
  __mtspr(0x130u, (unsigned int)(__builtin_rotateleft32(v0, 12) + 1769472) | 0x6200000000LL);
  v5 = (unsigned __int16)__builtin_rotateleft32(v0, 13) | sub_2E8() & 0xFFFFFFFFFFFF0000LL;
  *(_BYTE *)(v5 + 128) = v0;
  __mtspr(0x131u, v5);
  if ( (_DWORD)v0 == 0 )
    sub_364();
  v6 = 0;
  __asm { slbia     0 }
  do
  {
    v7 = *(_WORD *)(v6 + 2272);
    v6 += 2;
    v8 = v7 >> 12;
    v9 = v8 == 15;
    _R4 = ((v8 | 0x8000000) << 28) | v8 | 0x8000000;
    __asm { slbmte    r3, r4 }
  }
  while ( !v9 );
  if ( (_DWORD)v0 == 0 )
  {
    __mfspr(0x139u);
    v12 = sub_280();
    LODWORD(v12) = __builtin_rotateright64(v13 | v12, 10) & 0xFFFFFFE0 | 0xF;
    *(_DWORD *)(v14 + 14336) = v12;
    *(_DWORD *)(v14 + 14340) = v12 + 64;
    v15 = sub_298();
    *(_DWORD *)(v17 + 14348) = __builtin_rotateright64(v16 | v15, 10) & 0xFFFFFFE0 | 0xF;
    v18 = sub_2A8();
    *(_DWORD *)(v20 + 14356) = __builtin_rotateright64(v19 | v18, 10) & 0xFFFFFFE0 | 0xF;
    v21 = sub_2B8();
    *(_DWORD *)(v23 + 0x4000) = __builtin_rotateright64(v22 | v21, 10) & 0xFFFFFFE0 | 0xF;
    *(_DWORD *)(v24 + 65540) = ((v22 | v23) >> 16) & 0x3FFFFFF;
  }
  HvxFlushEntireTb();
  v25 = (_QWORD *)((v0 << 12) & 0x7000 | 0x8000020000050000LL);
  v25[14] = 124;
  __eieio();
  v25[1] = 124;
  __eieio();
  *v25 = (unsigned int)(1 << v0);
  __eieio();
  __mtsrr0(0xFFFFFFFF80078DB0LL);
  __mtsrr1(0x2003030u);
  __mfspr(0x130u);
  __sync();
  __isync();
  __asm { rfid }
}


/* ========== [0x1b10] sub_1B10 (320 bytes) ========== */
__int64 sub_1B10()
{
  __int64 v0; // r2
  __int64 v1; // lr
  int v2; // r11
  int v3; // r31
  unsigned __int64 FlashBase; // r3
  __int64 v5; // r2
  int v6; // r3
  _WORD v8[176]; // [sp+60h] [-390h] BYREF
  unsigned __int16 v9; // [sp+1C0h] [-230h] BYREF
  int v10; // [sp+1C8h] [-228h]
  int v11; // [sp+1CCh] [-224h]
  _WORD v12[8]; // [sp+1D0h] [-220h] BYREF
  _WORD v13[128]; // [sp+1E0h] [-210h] BYREF
  _DWORD v14[56]; // [sp+2E0h] [-110h] BYREF
  _WORD v15[10]; // [sp+3C0h] [-30h] BYREF
  int v16; // [sp+3D4h] [-1Ch]
  __int64 v17; // [sp+3E8h] [-8h]

  v17 = v1;
  v2 = *(unsigned __int8 *)(v0 + 91669);
  v16 = *(_DWORD *)(v0 + 0x10000);
  if ( v2 == 0 || v2 == 2 )
  {
    v3 = -1;
  }
  else
  {
    v3 = 0;
    FlashBase = HvpGetFlashBase();
    CopyBy64((__int64)&v9, (unsigned int *)(FlashBase + 512), 64);
    if ( v9 == 56139 && v10 == 288 && v11 == 512 )
    {
      sub_B388(v8, 32);
      sub_B370(v8, v14, 224, v14, v12, 0);
      j_XeCryptHmacSha(32, 16, &v9, 16, v14, 224, 0, 0);
      if ( (unsigned int)j_XeCryptBnQwBeSigVerify(v13, v15, v5 + 67332, v5 + 91136) != 0 && v14[0] == 1 && v14[1] != 0 )
        v3 = v14[4];
    }
  }
  v6 = v16;
  *(_DWORD *)(v0 + 92844) = v3;
  return v_MACHINE_CHECK_filter(v6);
}


/* ========== [0x1c50] sub_1C50 (108 bytes) ========== */
void sub_1C50()
{
  __int64 v0; // r2
  unsigned int v1; // r11

  v1 = *(unsigned __int8 *)(v0 + 91669);
  if ( v1 != 255
    && (v1 >= 5
     || (*(_DWORD *)(v0 + 92844) & 1) == 0
     && *(unsigned __int8 *)(v0 + 91040) < (unsigned int)*(unsigned __int8 *)(*(unsigned __int8 *)(v0 + 91669)
                                                                            + v0
                                                                            + 67048)) )
  {
    v_MACHINE_CHECK_0();
  }
}


/* ========== [0x1cbc] nullsub_1 (4 bytes) ========== */
void nullsub_1()
{
  ;
}


/* ========== [0x1cc0] sub_1CC0 (4 bytes) ========== */
// attributes: thunk
__int64 __fastcall sub_1CC0(__int64 a1, __int64 a2, __int64 a3)
{
  return sub_B480(a1, a2, a3);
}


/* ========== [0x1cc8] HvxGetVersion (20 bytes) ========== */
uint64_t __fastcall HvxGetVersion()
{
  return 0x760000044970000LL;
}


/* ========== [0x1ce0] sub_1CE0 (296 bytes) ========== */
__int64 __fastcall sub_1CE0(__int64 a1)
{
  __int64 v1; // r2
  __int64 *SocMMIORegs; // r27
  __int64 v4; // r3
  __int64 v5; // r11
  _QWORD *v6; // r10
  __int64 v7; // r9
  unsigned __int64 v8; // r31
  _QWORD *v9; // r29
  __int64 v10; // r30
  unsigned int i; // r28
  unsigned int v12; // r3
  _WORD v14[48]; // [sp+50h] [-150h] BYREF
  _WORD v15[80]; // [sp+B0h] [-F0h] BYREF
  _WORD v16[10]; // [sp+150h] [-50h] BYREF
  int v17; // [sp+164h] [-3Ch]

  v17 = *(_DWORD *)(v1 + 0x10000);
  SocMMIORegs = (__int64 *)HvpGetSocMMIORegs(155648);
  v4 = HvpGetSocMMIORegs(0x20000);
  v5 = 0;
  v6 = v14;
  do
  {
    v7 = 8LL * (unsigned int)v5;
    v5 += 64;
    *v6++ = *(_QWORD *)(v7 + v4);
  }
  while ( (unsigned int)v5 < 0x300 );
  v8 = 0;
  v9 = v15;
  v10 = 20;
  do
  {
    for ( i = 0; i < 0x64; ++i )
    {
      while ( *SocMMIORegs < 0 )
        ;
      v8 ^= SocMMIORegs[1];
      if ( i >= 3 )
      {
        v12 = sub_B450(v8);
        if ( v12 >= 0x1A && v12 <= 0x26 )
          break;
      }
    }
    --v10;
    v8 ^= __builtin_rotateleft64(__mftb(), v8 % 0x1D);
    *v9++ = v8;
  }
  while ( (_DWORD)v10 != 0 );
  sub_B4D8(v15, 160, v14, 96, 24, 8, v16, 20);
  j_XeCryptRc4Key(a1, v16, 20);
  return v_MACHINE_CHECK_filter(v17);
}


/* ========== [0x1e08] sub_1E08 (340 bytes) ========== */
__int64 sub_1E08()
{
  __int64 v0; // r2
  __int64 v1; // r2
  unsigned __int16 *v2; // r29
  __int64 v3; // r28
  __int64 v4; // r27
  int v5; // r30
  unsigned int v6; // r31
  unsigned __int64 v7; // r11
  unsigned __int64 v9; // [sp+50h] [-A0h] BYREF
  _WORD v10[44]; // [sp+60h] [-90h] BYREF
  int v11; // [sp+B8h] [-38h]

  v11 = *(_DWORD *)(v0 + 0x10000);
  j_XeCryptShaInit(v10);
  v2 = (unsigned __int16 *)(v1 + 67344);
  v3 = v1 + 65728;
  v4 = v1 + 92856;
  while ( 1 )
  {
    v5 = *v2;
    if ( v5 == 3 && *((_DWORD *)v2 + 1) >= 4u )
      return v_MACHINE_CHECK_filter(v11);
    v6 = *((_DWORD *)v2 + 1);
    if ( v2[1] != 0 )
      v6 |= *(_DWORD *)(4LL * ((unsigned int)v2[1] - 1) + v4);
    if ( *v2 != 0 )
    {
      switch ( v5 )
      {
        case 1:
          v7 = *(unsigned int *)HvpGetSocMMIORegs(v6);
          break;
        case 2:
          switch ( v6 )
          {
            case 0x139u:
              v7 = __mfspr(0x139u);
              break;
            case 0x13Eu:
              v7 = __mfspr(0x13Eu);
              break;
            case 0xFFFFFFFF:
              v7 = __mfmsr();
              break;
            case 0x3F1u:
              v7 = __mfspr(0x3F1u);
              break;
            default:
              v7 = 0;
              break;
          }
          break;
        case 3:
          v7 = 17559;
          break;
        default:
          goto LABEL_23;
      }
    }
    else
    {
      v7 = *(_QWORD *)HvpGetSocMMIORegs(v6);
    }
    v9 = v7;
LABEL_23:
    j_XeCryptShaUpdate(v10, &v9, 8);
    if ( v5 == 3 )
    {
      j_XeCryptShaFinal(v10, 16LL * v6 + v3, 16);
      j_XeCryptShaInit(v10);
    }
    v2 += 4;
  }
}


/* ========== [0x1f60] sub_1F60 (332 bytes) ========== */
__int64 sub_1F60()
{
  __int64 v0; // r2
  __int64 v1; // lr
  unsigned int *FlashBase; // r3
  __int64 v3; // r2
  unsigned __int16 *v4; // r3
  __int64 v5; // r2
  __int64 v6; // r31
  int v7; // r30
  unsigned __int8 *v8; // r3
  int v9; // r10
  __int64 v10; // r2
  __int64 SocMMIORegs; // r3
  __int64 v12; // r2
  unsigned int v13; // r10
  __int64 v14; // r9
  unsigned __int64 v15; // r11
  _WORD v17[64]; // [sp+50h] [-A0h] BYREF
  int v18; // [sp+D0h] [-20h]
  __int64 v19; // [sp+E8h] [-8h]

  v19 = v1;
  v18 = *(_DWORD *)(v0 + 0x10000);
  FlashBase = (unsigned int *)HvpGetFlashBase();
  CopyBy64((__int64)v17, FlashBase, 16);
  *(_QWORD *)(v3 + 92832) = (unsigned int)__mfpvr();
  v4 = (unsigned __int16 *)HvpBuildPciConfigRegs(0x8000);
  v6 = v5 + 0x10000;
  if ( __builtin_bswap16(*v4) == 60395 )
    *(_WORD *)(v5 + 92828) |= 0x8000u;
  v7 = __builtin_bswap16(*(_WORD *)HvpBuildPciConfigRegs(65538));
  v8 = (unsigned __int8 *)HvpBuildPciConfigRegs(65544);
  v9 = v17[0] & 0xF0;
  *(_DWORD *)(v10 + 92848) = (v7 << 16) | *v8;
  if ( v9 == 96 )
    *(_WORD *)(v6 + 27292) |= 2u;
  SocMMIORegs = HvpGetSocMMIORegs(130816);
  if ( *(_DWORD *)(SocMMIORegs + 32) == 1111753474 )
  {
    v15 = *(unsigned int *)(SocMMIORegs + 36);
    v14 = (v15 >> 24) & 0xF;
    *(_WORD *)(v6 + 27292) |= 4u;
    v13 = (unsigned int)v15 >> 28;
  }
  else
  {
    v13 = 0;
    LODWORD(v14) = 1;
  }
  *(_DWORD *)(v12 + 92856) = v13 << 20;
  *(_DWORD *)(v12 + 92860) = (_DWORD)v14 << 20;
  sub_1E08();
  return v_MACHINE_CHECK_filter(v18);
}


/* ========== [0x20b0] sub_20B0 (348 bytes) ========== */
__int64 __fastcall sub_20B0(int a1)
{
  __int64 v1; // lr
  _QWORD *SocMMIORegs; // r30
  __int64 v4; // r4
  __int64 v5; // r2
  _QWORD *v6; // r31
  bool v7; // zf
  _DWORD *v8; // r11
  __int64 i; // ctr
  _BYTE *v10; // r9
  _BYTE *v11; // r10
  __int64 j; // ctr
  __int64 result; // r3
  _QWORD v14[3]; // [sp+50h] [-30h] BYREF
  __int64 v15; // [sp+78h] [-8h]

  v15 = v1;
  SocMMIORegs = (_QWORD *)HvpGetSocMMIORegs(147456);
  v7 = a1 == 0;
  v6 = (_QWORD *)(v5 + 65792);
  if ( v7 )
  {
    v8 = (_DWORD *)(v5 + 65792);
    for ( i = 12; i != 0; --i )
      *v8++ = 1431655765;
  }
  else
  {
    XeCryptRandom((uint8_t *)(v5 + 65792), 48);
  }
  *SocMMIORegs = *v6;
  __eieio();
  SocMMIORegs[1] = v6[1];
  __eieio();
  SocMMIORegs[512] = *v6;
  __eieio();
  SocMMIORegs[513] = v6[1];
  __eieio();
  SocMMIORegs[520] = v6[2];
  __eieio();
  SocMMIORegs[521] = v6[3];
  __eieio();
  v10 = v14;
  v11 = v6 + 2;
  for ( j = 16; j != 0; --j )
    *v10++ = *v11++;
  LODWORD(v4) = v5 + 67056;
  result = sub_B390(v14, v4);
  SocMMIORegs[8] = v14[0];
  __eieio();
  SocMMIORegs[9] = v14[1];
  __eieio();
  SocMMIORegs[16] = v6[4];
  __eieio();
  SocMMIORegs[17] = v6[5];
  __eieio();
  SocMMIORegs[528] = v6[4];
  __eieio();
  SocMMIORegs[529] = v6[5];
  __eieio();
  __isync();
  __isync();
  return result;
}


/* ========== [0x2210] generate_hypervisor_hash_F0 (232 bytes) ========== */
__int64 generate_hypervisor_hash_F0()
{
  __int64 v0; // r2
  __int64 v1; // r2
  __int64 v2; // r28
  _DWORD *v3; // r29
  int v4; // r11
  __int64 v5; // r10
  unsigned int v6; // r31
  unsigned int v7; // r30
  __int64 SocMMIORegs; // r3
  __int64 v9; // r5
  _WORD v11[44]; // [sp+50h] [-90h] BYREF
  int v12; // [sp+A8h] [-38h]

  v12 = *(_DWORD *)(v0 + 0x10000);
  j_XeCryptShaInit(v11);
  v2 = 0;
  v3 = (_DWORD *)(v1 + 67704);
  do
  {
    v4 = *v3 + 127;
    v5 = v3[1] & 0xFFFFFF80;
    v6 = v4 & 0xFFFFFF80;
    if ( (v4 & 0xFFFFFF80) < (unsigned int)v5 )
    {
      v7 = v5 - v6;
      HvpSaveCachelines(
        (_BYTE *)(0x200010000LL * ((unsigned __int64)(v4 & 0xFFFFFF80) >> 16) + ((unsigned __int16)v4 & 0xFF80)),
        ((v5 - (unsigned __int64)(v4 & 0xFFFFFF80)) >> 7) & 0x1FFFFFF);
      if ( (v6 & 0x7F) != 0 || v6 >= 0x40000 )
        v_MACHINE_CHECK_0();
      SocMMIORegs = HvpGetSocMMIORegs((v6 >> 6) + 0x10000);
      LODWORD(v9) = v7 >> 6;
      j_XeCryptShaUpdate(v11, SocMMIORegs, v9);
    }
    ++v2;
    v3 += 2;
  }
  while ( (unsigned int)v2 < 6 );
  j_XeCryptShaFinal(v11, v1 + 65776, 16);
  return v_MACHINE_CHECK_filter(v12);
}


/* ========== [0x22f8] sub_22F8 (324 bytes) ========== */
_BYTE *__fastcall sub_22F8(_BYTE *a1, int a2)
{
  __int64 v4; // r3
  _BYTE *v6; // r3

  sub_84A8(0, 512);
  sub_84A8(0x200010000LL, 512);
  sub_84A8(0x400020000LL, 512);
  sub_84A8(0x600030000LL, 512);
  sub_84A8(__mfspr(0x131u), 512);
  v4 = sub_2D8();
  sub_84A8(v4, 512);
  sub_84A8(a1, 512);
  sub_20B0(a2);
  if ( a2 == 0 )
    return (_BYTE *)generate_hypervisor_hash_F0();
  HvpSaveCachelines(nullptr, 512);
  HvpSaveCachelines((_BYTE *)0x200010000LL, 512);
  HvpSaveCachelines((_BYTE *)0x400020000LL, 512);
  HvpSaveCachelines((_BYTE *)0x600030000LL, 512);
  HvpSaveCachelines((_BYTE *)__mfspr(0x131u), 512);
  v6 = (_BYTE *)sub_2D8();
  HvpSaveCachelines(v6, 512);
  return HvpSaveCachelines(a1, 512);
}


/* ========== [0x2440] ComputeRSAOutput (204 bytes) ========== */
__int64 ComputeRSAOutput()
{
  __int64 v0; // r2
  __int64 v1; // lr
  __int64 v2; // r2
  __int64 v3; // r2
  __int64 v4; // r31
  uint8_t v6[32]; // [sp+50h] [-C0h] BYREF
  _WORD v7[64]; // [sp+70h] [-A0h] BYREF
  int v8; // [sp+F0h] [-20h]
  __int64 v9; // [sp+108h] [-8h]

  v9 = v1;
  v8 = *(_DWORD *)(v0 + 0x10000);
  XeCryptRandom(v6, 20);
  if ( (unsigned int)j_HvSetupMemEncryptionKey(v2 + 65792, 48, v7, 128, 0, v6) != 0 )
  {
    sub_B498(v7, 128);
    j_XeCryptBnQw_SwapDwQwLeBe(v7, v7, 16);
    v4 = v3 + 65600;
    if ( (unsigned int)j_XeCryptBnQwNeRsaPubCrypt(v7, v3 + 65600, v3 + 66744) != 0 )
      j_XeCryptBnQw_SwapDwQwLeBe(v4, v4, 16);
    else
      memset(v4, 0, 128);
  }
  return v_MACHINE_CHECK_filter(v8);
}


/* ========== [0x2510] HvxFlushSingleTb (48 bytes) ========== */
void __fastcall HvxFlushSingleTb(void *address)
{
  HvpFlushSingleTb((unsigned int)address, 0xFA2AA00u);
}


/* ========== [0x2540] HvxPostOutput (68 bytes) ========== */
void __fastcall HvxPostOutput(__int64 code)
{
  *(_QWORD *)(HvpGetSocMMIORegs(397312) + 16) = code << 56;
  __eieio();
}


/* ========== [0x2588] HvxGetSocRegister (320 bytes) ========== */
uint64_t __fastcall HvxGetSocRegister(uint32_t reg)
{
  __int64 v1; // r2

  if ( (reg & 7) == 0
    && (*(_QWORD *)(v1 + 92832) < 0x710700u || reg - 0x40000 > 0xFFFF)
    && (reg == 397720 || reg == 397712) )
  {
    return *(_QWORD *)HvpGetSocMMIORegs(reg);
  }
  else
  {
    return 0;
  }
}


/* ========== [0x26c8] HvxSetSocRegister (296 bytes) ========== */
void __fastcall HvxSetSocRegister(uint32_t reg, uint64_t val)
{
  __int64 v2; // r2

  if ( (reg & 7) == 0 && (*(_QWORD *)(v2 + 92832) < 0x710700u || reg - 0x40000 > 0xFFFF) && reg == 397720 )
  {
    *(_QWORD *)HvpGetSocMMIORegs(397720) = val;
    __eieio();
  }
}


/* ========== [0x27f0] HvxFsbInterrupt (288 bytes) ========== */
__int64 HvxFsbInterrupt()
{
  __int64 v0; // r29
  __int64 SocMMIORegs; // r30
  __int64 v2; // r3
  __int64 v3; // r2
  _QWORD *v4; // r31
  __int64 v5; // r3
  __int64 v6; // r4
  __int64 v7; // r3
  __int64 v8; // r4

  v0 = 0;
  SocMMIORegs = HvpGetSocMMIORegs(196608);
  v2 = HvpGetSocMMIORegs(0x40000);
  v4 = (_QWORD *)v2;
  if ( *(_QWORD *)(v3 + 92832) < 0x710700u )
  {
    if ( (*(_QWORD *)(v2 + 12288) & 0x400000000000LL) != 0 )
    {
      if ( *(_DWORD *)(v3 + 92848) <= 0x58110011u )
      {
        v5 = sub_4CC();
        LODWORD(v6) = 512;
        HIDWORD(v7) = (unsigned __int64)sub_84A8(v5, v6) >> 32;
        LODWORD(v7) = __mfspr(0x131u);
        LODWORD(v8) = 64;
        sub_84A8(v7, v8);
        v4[3620] = 0x40000000000LL;
        __eieio();
        while ( (HIDWORD(v4[3616]) & 0xFLL) != 0xE )
          ;
        v0 = 1;
      }
      v4[1536] = v4[1536] & 0xFFFF83FFFFFFFFFFLL | 0x180000000000LL;
      __eieio();
      *(_QWORD *)(SocMMIORegs + 12304) = 0xFFFFFDFFFFFFFFFFLL;
      __eieio();
    }
    v4[1538] = ~v4[1600];
    __eieio();
    v4[1600] = 0;
    __eieio();
  }
  *(_QWORD *)(SocMMIORegs + 12304) = ~*(_QWORD *)(SocMMIORegs + 12800);
  __eieio();
  *(_QWORD *)(SocMMIORegs + 12800) = 0;
  __eieio();
  return v0;
}


/* ========== [0x2910] HvxLockL2 (196 bytes) ========== */
__int64 __fastcall HvxLockL2(__int64 a1, __int64 a2, __int64 a3, __int64 a4, unsigned __int64 a5)
{
  __int64 v5; // r2
  _DWORD *v6; // r27
  int v8; // r9
  char v9; // r11
  __int64 v10; // r10

  v6 = (_DWORD *)(v5 + 92840);
  HvpAquireSpinLock((_DWORD *)(v5 + 92840));
  v8 = 0;
  v9 = 0;
  v10 = 8;
  do
  {
    if ( ((1LL << v9) & HIBYTE(a5)) != 0 )
      ++v8;
    --v10;
    ++v9;
  }
  while ( (_DWORD)v10 != 0 );
  if ( v8 <= 0 || v8 >= 4 )
  {
    __sync();
    *(_QWORD *)(HvpGetSocMMIORegs(393216) + 768) = a5;
    __eieio();
    __sync();
  }
  HvpReleaseSpinLock(v6);
  return 0;
}


/* ========== [0x29d8] HvpPhysicalToReal (60 bytes) ========== */
/* Converts physical address (r3) to real-mode address. Validates r3 in [0x40000, 0x20000000) and r4 (size) <= 0x20000000-r3. Returns r3 | 0x8000000000000000. Panics via _v_MACHINE_CHECK_0 on invalid input. */
// Converts physical address (r3) to real-mode address. Validates r3 in [0x40000, 0x20000000) and r4 (size) <= 0x20000000-r3. Returns r3 | 0x8000000000000000. Panics via _v_MACHINE_CHECK_0 on invalid input.
unsigned __int64 __fastcall HvpPhysicalToReal(unsigned int a1, unsigned int a2)
{
  if ( a1 - 0x40000 > 0x1FFBFFFF || a2 > 0x20000000 - a1 )
    v_MACHINE_CHECK_0();
  return a1 | 0x8000000000000000LL;
}


/* ========== [0x2a18] HvpPhysicalToRealExt (100 bytes) ========== */
unsigned __int64 __fastcall HvpPhysicalToRealExt(unsigned int a1, unsigned int a2)
{
  if ( a1 + 0x80000000 <= 0x1FFFFFFF && a2 <= -1610612736 - a1 )
    return jt1_jumper();
  if ( a1 - 0x40000 > 0x1FFBFFFF || a2 > 0x20000000 - a1 )
    v_MACHINE_CHECK_0();
  return a1 | 0x8000000000000000LL;
}


/* ========== [0x2a80] HvxFlushDcacheRange (108 bytes) ========== */
void __fastcall HvxFlushDcacheRange(__int64 phys, uint32_t size)
{
  int v2; // r11

  if ( (unsigned int)phys < 0x40000 )
  {
    v2 = 0x40000 - phys;
    if ( size <= 0x40000 - (int)phys )
      return;
    phys = 0x40000;
    size -= v2;
  }
  if ( (unsigned int)phys < 0x20000000 )
  {
    if ( size > 0x20000000 - (int)phys )
      size = 0x20000000 - phys;
    if ( size != 0 )
      sub_D7C(phys);
  }
}


/* ========== [0x2af0] HvxZeroPage (196 bytes) ========== */
void __fastcall HvxZeroPage(uint32_t phys)
{
  if ( phys >= 0x40000 )
    v_MACHINE_CHECK_0();
  if ( (phys << 12) - 0x40000 > 0x1FFBFFFF || 0x20000000 - (phys << 12) < 0x1000 )
    v_MACHINE_CHECK_0();
  HvpZeroCacheLines((phys << 12) | 0x8000000000000000LL, 32);
}


/* ========== [0x2bb8] HvNoOp_ReturnZero (8 bytes) ========== */
__int64 HvNoOp_ReturnZero()
{
  return 0;
}


/* ========== [0x2bc0] sub_2BC0 (532 bytes) ========== */
// write access to const memory has been detected, the output may be wrong!
_QWORD *sub_2BC0()
{
  _QWORD *SocMMIORegs; // r29
  _QWORD *v1; // r30
  __int64 v2; // r28
  __int64 v3; // r31
  _QWORD *result; // r3
  __int64 v5; // r2

  HvxPostOutput(89);
  SocMMIORegs = (_QWORD *)HvpGetSocMMIORegs(196608);
  v1 = (_QWORD *)HvpGetSocMMIORegs(0x40000);
  v2 = HvpGetSocMMIORegs(147456);
  v3 = HvpGetSocMMIORegs(393216);
  result = (_QWORD *)HvpGetSocMMIORegs(294912);
  if ( *(_QWORD *)(v5 + 92832) < 0x710700u )
  {
    v1[1536] = v1[1536] & 0xFFFF83FFFFFFFFFFLL | 0x180000000000LL;
    __eieio();
    if ( *(_QWORD *)(v5 + 92832) < 0x710700u )
    {
      v1[1570] = 0xFFFFBFFFFFFFFFFFLL;
      __eieio();
    }
  }
  SocMMIORegs[1570] = 0xFFFFFDFFFFFFFFFFLL;
  __eieio();
  *(_QWORD *)(v3 + 1792) |= 0xE0000000000000uLL;
  __eieio();
  *(_QWORD *)(v3 + 2112) = *(_QWORD *)(v3 + 2112) & 0xFFFF83FFFFFFFFFFLL | 0x3C0000000000LL;
  __eieio();
  if ( *(_QWORD *)(v5 + 92832) < 0x710700u )
  {
    *v1 = *v1 & 0x1FF03FFFFFFFFFFFLL | 0x4009000000000000LL;
    __eieio();
    if ( *(_QWORD *)(v5 + 92832) < 0x710700u )
    {
      *result = *result & 0xA0FFBFFFFFFFFFFFLL | 0x100400000000000LL;
      __eieio();
    }
  }
  *SocMMIORegs |= 0x4000000000000000uLL;
  __eieio();
  if ( (unsigned __int64)(*(_QWORD *)(v5 + 92832) - 7406848LL) <= 0x1FF )
  {
    v1[3072] = 0x1E00000000LL;
    __eieio();
    __isync();
    v1[3104] = v1[3136] | 0x8000000000000000LL;
    __eieio();
    __isync();
    v1[3072] = 0xE0000000000LL;
    __eieio();
    __isync();
  }
  unk_18 = *(_QWORD *)(v2 + 4280);
  return result;
}


/* ========== [0x2dd8] sub_2DD8 (184 bytes) ========== */
_WORD *sub_2DD8()
{
  _DWORD *v0; // r31
  _WORD *result; // r3
  __int64 v2; // r2
  _BYTE *v3; // r9
  unsigned int v4; // r11
  int v5; // r8
  unsigned int v6; // r10

  HvxPostOutput(90);
  sub_FD4();
  v0 = (_DWORD *)sub_2D8();
  result = (_WORD *)sub_5C0();
  v3 = (_BYTE *)(v2 + 66984);
  v4 = 0;
  do
  {
    if ( *v3 != 0 )
    {
      v5 = (((unsigned __int8)*v3 << 18) & 0x3000000 | v4 & 0x3FFF) << 6;
      v6 = v5 | *v3 & 0x3F;
      if ( (v5 & 0xC0000000) == 0x40000000 && v4 >= 4 )
        v6 = v5 & 0xC00FFFFF | *v3 & 0x3F | 0x18800000;
      *v0 = v6;
      *result |= 0x800u;
    }
    ++v4;
    ++v0;
    ++result;
    ++v3;
  }
  while ( v4 < 0x40 );
  return result;
}


/* ========== [0x2e90] sub_2E90 (348 bytes) ========== */
__int64 sub_2E90()
{
  __int64 v0; // r2
  _BYTE *v1; // r31
  unsigned int *v2; // r30
  __int64 i; // r28
  __int64 v4; // r11
  __int64 v5; // r4
  __int64 v6; // r3
  __int64 v7; // r3
  __int64 v8; // r3
  unsigned __int64 v9; // r3
  __int64 v10; // r3
  __int64 v11; // r3
  __int64 v12; // r3
  __int64 v13; // r3
  _WORD v15[130]; // [sp+50h] [-130h] BYREF
  int v16; // [sp+154h] [-2Ch]

  v1 = nullptr;
  v16 = *(_DWORD *)(v0 + 0x10000);
  HvxPostOutput(91);
  sub_B0F0();
  sub_1CE0((__int64)v15);
  HvpSetRMCI(0);
  v2 = (unsigned int *)sub_57C(0x80040000);
  for ( i = 0x800040000LL; ; i += 0x200010000LL )
  {
    HvpRelocateCacheLines(
      (__int64 *)i,
      (_QWORD *)(((unsigned __int64)*v2 << 10) & 0x3FFFFFF0000LL | 0x8000000000000000LL),
      512);
    v4 = *v2;
    if ( (v4 & 0xC0000000) == 0x40000000 )
      v1 = (_BYTE *)((v4 << 10) & 0x3FFFFFF0000LL | 0x8000000000000000LL);
    if ( (v4 & 0x20) != 0 )
      break;
    ++v2;
  }
  HvpSetRMCI(1);
  sub_22F8(v1, 0);
  sub_22F8(v1, 1);
  HvpSetRMCI(0);
  LODWORD(v5) = 0;
  LODWORD(v6) = -2147090432;
  sub_B0C8(v6, v5);
  v7 = sub_464();
  memcpy(v7, v15);
  v8 = sub_270();
  HvpZeroCacheLines(v8, 512);
  v9 = sub_280();
  HvpZeroCacheLines(v9, 1024);
  v10 = sub_298();
  HvpZeroCacheLines(v10, 512);
  v11 = sub_2A8();
  HvpZeroCacheLines(v11, 512);
  v12 = sub_2B8();
  HvpZeroCacheLines(v12, 512);
  v13 = sub_2C8();
  HvpZeroCacheLines(v13, 512);
  ComputeRSAOutput();
  HvpSetRMCI(1);
  return v_MACHINE_CHECK_filter(v16);
}


/* ========== [0x2ff0] sub_2FF0 (76 bytes) ========== */
__int64 sub_2FF0()
{
  __int64 v0; // r3
  __int64 v1; // r3
  __int64 v2; // r3
  __int64 v3; // r3
  __int64 v4; // r3
  __int64 v5; // r3
  __int64 v6; // r3
  __int64 v7; // r3
  __int64 v8; // r3

  HvxPostOutput(92);
  v0 = sub_B0C0();
  v1 = sub_B0B0(v0);
  v2 = sub_B0D0(v1);
  v3 = sub_B0E8(v2);
  v4 = sub_B0F8(v3);
  v5 = sub_B110(v4);
  v6 = sub_8478(v5);
  v7 = sub_B118(v6);
  v8 = sub_B128(v7);
  return sub_B160(v8);
}


/* ========== [0x3040] sub_3040 (188 bytes) ========== */
void sub_3040()
{
  __int64 v0; // r2
  __int64 SocMMIORegs; // r3

  HvxPostOutput(94);
  sub_B150();
  if ( (unsigned __int64)(*(_QWORD *)(v0 + 92832) - 7406848LL) <= 0x1FF && *(_DWORD *)(v0 + 92848) >= 0x58310010u )
  {
    SocMMIORegs = HvpGetSocMMIORegs(0x40000);
    *(_QWORD *)(SocMMIORegs + 24832) = 0x80021000FFFFFFFFLL;
    __eieio();
    __isync();
    *(_QWORD *)(SocMMIORegs + 24576) = 0xE00FFFFFFFFLL;
    __eieio();
    __isync();
  }
  HvxPostOutput(95);
}


/* ========== [0x3100] sub_3100 (172 bytes) ========== */
void sub_3100()
{
  _QWORD *v0; // r3
  __int64 v1; // r3
  __int64 v2; // r2

  sub_1F60();
  v0 = sub_2BC0();
  LODWORD(v1) = sub_A120(v0);
  sub_3F88(v1);
  sub_1B10();
  sub_1C50();
  sub_2DD8();
  sub_2E90();
  sub_2FF0();
  if ( (*(_WORD *)(v2 + 92828) & 0x8000) == 0 )
    sub_3040();
}


/* ========== [0x31b0] HvxRelocateAndFlush (204 bytes) ========== */
void __fastcall HvxRelocateAndFlush(void *virt, __int64 dest_phys, uint32_t src_phys)
{
  unsigned int v3; // r31
  unsigned int v5; // r5

  v3 = (unsigned int)virt;
  if ( (unsigned int)virt >= 0x40000000 )
  {
    if ( (unsigned int)virt >= 0x80000000 )
      return;
    v5 = 0x10000;
  }
  else
  {
    v5 = 4096;
  }
  if ( (unsigned int)(dest_phys - 0x40000) > 0x1FFBFFFF || v5 > 0x20000000 - (int)dest_phys )
    v_MACHINE_CHECK_0();
  if ( src_phys - 0x40000 > 0x1FFBFFFF || v5 > 0x20000000 - src_phys )
    v_MACHINE_CHECK_0();
  LODWORD(virt) = src_phys;
  memcpy(virt, dest_phys);
  HvpFlushSingleTb(v3, 0xFA2AA00u);
}


/* ========== [0x3280] HvpRelocatePhysicalToEncrypted (212 bytes) ========== */
__int64 __fastcall HvpRelocatePhysicalToEncrypted(unsigned int a1, unsigned int a2)
{
  __int64 v2; // lr
  unsigned __int64 v4; // r31
  __int64 v5; // r5
  __int64 v7; // [sp+50h] [-20h] BYREF
  __int64 v8; // [sp+68h] [-8h]

  v8 = v2;
  if ( (a1 & 0x7F) != 0 || a2 == 0 || (a2 & 0x7F) != 0 )
    v_MACHINE_CHECK_0();
  if ( a1 - 0x40000 > 0x1FFBFFFF || a2 > 0x20000000 - a1 )
    v_MACHINE_CHECK_0();
  v4 = a1 | 0x8000000000000000LL;
  XeCryptRandom((uint8_t *)&v7, 8);
  LODWORD(v5) = a2 >> 7;
  HvpRelocateCacheLines((__int64 *)v4, (_QWORD *)(v7 & 0xFFC0000000LL | v4 & 0x3FFFFF80 | 0x8000030000000000LL), v5);
  return v7;
}


/* ========== [0x3358] HvpRelocateEncryptedToPhysical (124 bytes) ========== */
__int64 *__fastcall HvpRelocateEncryptedToPhysical(__int64 *a1, unsigned int a2, __int64 a3)
{
  if ( ((unsigned __int8)a1 & 0x7F) != 0
    || a2 == 0
    || (a2 & 0x7F) != 0
    || (unsigned __int64)a1 >> 40 != 8388611
    || ((unsigned int)a1 & 0x3FFFFFFF) - 0x40000 > 0x1FFBFFFF
    || a2 > 0x20000000 - ((unsigned int)a1 & 0x3FFFFFFF) )
  {
    v_MACHINE_CHECK_0();
  }
  LODWORD(a3) = a2 >> 7;
  return HvpRelocateCacheLines(a1, (_QWORD *)((unsigned int)a1 & 0x3FFFFFFF | 0x8000000000000000LL), a3);
}


/* ========== [0x33d8] HvpRelocatePhysicalToProtected (220 bytes) ========== */
unsigned __int64 __fastcall HvpRelocatePhysicalToProtected(unsigned int a1, unsigned int a2, __int64 a3)
{
  unsigned __int64 v3; // r3
  unsigned __int64 v4; // r11
  unsigned __int64 v5; // r31

  if ( (a1 & 0x7F) != 0
    || a2 == 0
    || (a2 & 0x7F) != 0
    || a2 > 0x10000
    || (((a1 + a2 - 1) ^ a1) & 0xFFFF0000) != 0
    || (unsigned int)(a3 - 5) > 0x3A )
  {
    v_MACHINE_CHECK_0();
  }
  if ( a1 - 0x40000 > 0x1FFBFFFF || a2 > 0x20000000 - a1 )
    v_MACHINE_CHECK_0();
  v3 = a1 | 0x8000000000000000LL;
  v4 = (a3 << 33) | v3 & 0x3FFFFF80;
  LODWORD(a3) = a2 >> 7;
  v5 = v4 | 0x8000010000000000LL;
  HvpRelocateCacheLines((__int64 *)v3, (_QWORD *)(v4 | 0x8000010000000000LL), a3);
  return v5;
}


/* ========== [0x34b8] HvpRelocatePhysicalToEncryptedStrict (184 bytes) ========== */
__int64 *__fastcall HvpRelocatePhysicalToEncryptedStrict(__int64 *a1, unsigned int a2, __int64 a3)
{
  if ( ((unsigned __int8)a1 & 0x7F) != 0
    || a2 == 0
    || (a2 & 0x7F) != 0
    || a2 > 0x10000
    || ((((unsigned __int64)a1 + a2 - 1) ^ (unsigned __int64)a1) & 0xFFFFFFFFFFFF0000LL) != 0
    || (unsigned int)(a3 - 4) > 0x3B
    || (unsigned __int64)a1 >> 32 != ((unsigned int)(2 * a3) | 0x80000100LL)
    || ((unsigned int)a1 & 0x3FFFFFFF) - 0x40000 > 0x1FFBFFFF
    || a2 > 0x20000000 - ((unsigned int)a1 & 0x3FFFFFFF) )
  {
    v_MACHINE_CHECK_0();
  }
  LODWORD(a3) = a2 >> 7;
  return HvpRelocateCacheLines(a1, (_QWORD *)((unsigned int)a1 & 0x3FFFFFFF | 0x8000000000000000LL), a3);
}


/* ========== [0x3570] sub_3570 (48 bytes) ========== */
bool __fastcall sub_3570(_BYTE *a1, int a2)
{
  unsigned __int8 v2; // r11

  v2 = 0;
  while ( a2 != 0 )
  {
    --a2;
    v2 |= *a1++;
  }
  return (_cntlzw(v2) & 0x20) != 0;
}


/* ========== [0x35a0] sub_35A0 (12 bytes) ========== */
__int64 sub_35A0()
{
  return 0;
}


/* ========== [0x35b0] sub_35B0 (112 bytes) ========== */
__int64 sub_35B0()
{
  return 0;
}


/* ========== [0x3620] HvpComputeKeyVaultHmacSha (136 bytes) ========== */
__int64 __fastcall HvpComputeKeyVaultHmacSha(int a1, __int64 a2, __int64 a3)
{
  __int64 v3; // r2
  int v4; // r30
  int v5; // r31
  int v6; // r29
  __int64 v7; // r5
  __int64 v8; // r4
  __int64 v9; // r5
  __int64 v10; // r4
  __int64 v11; // r5
  __int64 v12; // r4
  __int64 v13; // r5
  __int64 v14; // r4
  _WORD v16[88]; // [sp+50h] [-E0h] BYREF
  int v17; // [sp+100h] [-30h]

  v4 = a2;
  v5 = a1 + 24;
  v6 = a3;
  LODWORD(a3) = 16;
  LODWORD(a2) = 32;
  v17 = *(_DWORD *)(v3 + 0x10000);
  j_XeCryptHmacShaInit(v16, a2, a3);
  LODWORD(v7) = 212;
  LODWORD(v8) = v5 + 4;
  j_XeCryptHmacShaUpdate(v16, v8, v7);
  LODWORD(v9) = 7416;
  LODWORD(v10) = v5 + 232;
  j_XeCryptHmacShaUpdate(v16, v10, v9);
  LODWORD(v11) = 8456;
  LODWORD(v12) = v5 + 7904;
  j_XeCryptHmacShaUpdate(v16, v12, v11);
  LODWORD(v13) = v6;
  LODWORD(v14) = v4;
  j_XeCryptHmacShaFinal(v16, v14, v13);
  return v_MACHINE_CHECK_filter(v17);
}


/* ========== [0x36a8] HvpKeysExKeyVaultSeal (184 bytes) ========== */
__int64 __fastcall HvpKeysExKeyVaultSeal(__int64 a1)
{
  __int64 v1; // r2
  __int64 v3; // r30
  __int64 v4; // r6
  __int64 v5; // r5
  __int64 v6; // r4
  _WORD v8[8]; // [sp+60h] [-50h] BYREF
  _WORD v9[8]; // [sp+70h] [-40h] BYREF
  int v10; // [sp+80h] [-30h]

  v3 = a1 + 16;
  v10 = *(_DWORD *)(v1 + 0x10000);
  XeCryptRandom((uint8_t *)(a1 + 16), 8);
  j_XeCryptHmacSha(32, 16, v3, 16368, v8, 2, 0, 0);
  j_XeCryptHmacSha(32, 16, a1, 16, 0, 0, 0, 0);
  LODWORD(v4) = 16368;
  LODWORD(v5) = v3;
  LODWORD(v6) = 16;
  j_XeCryptRc4(v9, v6, v5, v4);
  return v_MACHINE_CHECK_filter(v10);
}


/* ========== [0x3760] sub_3760 (204 bytes) ========== */
bool __fastcall sub_3760(__int64 a1)
{
  __int64 v1; // r2
  __int64 v2; // r31
  _WORD v4[8]; // [sp+60h] [-60h] BYREF
  _WORD v5[8]; // [sp+70h] [-50h] BYREF
  _WORD v6[8]; // [sp+80h] [-40h] BYREF
  int v7; // [sp+90h] [-30h]

  v2 = a1;
  v7 = *(_DWORD *)(v1 + 0x10000);
  j_XeCryptHmacSha(32, 16, a1, 16, 0, 0, 0, 0);
  j_XeCryptRc4(v5, 16, v2 + 16, 16368);
  j_XeCryptHmacSha(32, 16, v2 + 16, 16368, v4, 2, 0, 0);
  LOBYTE(v2) = _cntlzw(sub_B480(v2, v6, 16));
  v_MACHINE_CHECK_filter(v7);
  return (v2 & 0x20) != 0;
}


/* ========== [0x3830] sub_3830 (92 bytes) ========== */
// write access to const memory has been detected, the output may be wrong!
_DWORD *__fastcall sub_3830(int a1, int a2)
{
  __int64 v2; // r2
  _DWORD *v4; // r29
  __int64 v6; // r2
  __int64 v7; // r11

  v4 = (_DWORD *)(v2 + 91740);
  HvpAquireSpinLock((_DWORD *)(v2 + 91740));
  dword_30 = (a1 | 0x21281D3) & ~a2;
  v7 = *(_QWORD *)(v6 + 90832);
  if ( v7 != 0 )
    *(_DWORD *)(v7 + 1552) = 34767315;
  return HvpReleaseSpinLock(v4);
}


/* ========== [0x3890] sub_3890 (68 bytes) ========== */
__int64 __fastcall sub_3890(__int64 a1, unsigned int a2)
{
  __int64 result; // r3
  bool v3; // zf

  if ( a1 != ((a1 + 15) & 0xFFFFFFFFFFFFFFF0LL) )
    return 0;
  if ( (unsigned __int64)(a1 - 128) > 0x7FFFF7F )
    return 0;
  v3 = a2 >= (unsigned __int64)(0x8000000 - a1);
  result = 1;
  if ( v3 )
    return 0;
  return result;
}


/* ========== [0x38d8] sub_38D8 (164 bytes) ========== */
__int64 __fastcall sub_38D8(__int64 a1, __int64 a2, int *a3)
{
  __int64 v3; // r29
  __int64 v4; // r2
  int v7; // r10
  int v8; // r31
  __int64 v10; // r5
  __int64 v11; // r31
  _WORD v12[256]; // [sp+50h] [-230h] BYREF
  int v13; // [sp+250h] [-30h]

  v7 = *a3;
  v8 = 8 * v7;
  v13 = *(_DWORD *)(v4 + 0x10000);
  if ( (unsigned int)(8 * v7) <= 0x200
    && (LODWORD(v3) = v7 & 0x1FFFFFFF,
        j_XeCryptBnQw_SwapDwQwLeBe(a2, v12, v3),
        (unsigned int)j_XeCryptBnQwNeRsaPubCrypt(v12, v12, a3) != 0) )
  {
    j_XeCryptBnQw_SwapDwQwLeBe(v12, v12, v3);
    LODWORD(v10) = v8;
    v11 = sub_B3B0(a1, v12, v10);
    v_MACHINE_CHECK_filter(v13);
    return v11;
  }
  else
  {
    v_MACHINE_CHECK_filter(v13);
    return 0;
  }
}


/* ========== [0x3980] sub_3980 (128 bytes) ========== */
__int64 __fastcall sub_3980(
        __int64 a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  __int64 v8; // lr
  __int64 v11; // r31
  __int64 v12; // r2
  _WORD v13[180]; // [sp+50h] [-180h] BYREF
  __int64 v14; // [sp+1C8h] [-8h]

  v14 = v8;
  if ( (_DWORD)a3 != 16 )
    return -939524092;
  v11 = HvpPhysicalToReal(a2, 16, a3, a4, a5, a6, a7, a8);
  sub_B388(v13, v12 + 91408);
  sub_B380(v13, v11, a1, 0);
  return 0;
}


/* ========== [0x3a00] sub_3A00 (272 bytes) ========== */
__int64 __fastcall sub_3A00(__int64 a1, int a2, __int64 a3, __int64 a4, __int64 a5, __int64 a6, __int64 a7)
{
  __int64 v7; // r2
  __int64 v8; // lr
  __int64 v10; // r3
  __int64 v11; // r2
  __int64 v12; // r31
  __int64 v13; // r2
  _WORD v14[128]; // [sp+50h] [-120h] BYREF
  int v15; // [sp+150h] [-20h]
  __int64 v16; // [sp+168h] [-8h]

  v16 = v8;
  v15 = *(_DWORD *)(v7 + 0x10000);
  if ( a2 == 256 )
  {
    v10 = HvpPhysicalToReal(a1, 256, a3, a4, a5, a6, a7, 2);
    memcpy(v14, v10);
    v12 = v11 + 91712;
    if ( (unsigned int)sub_38D8(v11 + 91712, (__int64)v14, (int *)(v11 + 72312)) != 0
      || *(_DWORD *)(v13 + 68040) == 32 && (unsigned int)sub_38D8(v12, (__int64)v14, (int *)(v13 + 68040)) != 0 )
    {
      sub_3830(0x80000, 0);
      v_MACHINE_CHECK_filter(v15);
      return 0;
    }
    else
    {
      v_MACHINE_CHECK_filter(v15);
      return -939524091;
    }
  }
  else
  {
    v_MACHINE_CHECK_filter(v15);
    return -939524092;
  }
}


/* ========== [0x3b10] sub_3B10 (320 bytes) ========== */
__int64 __fastcall sub_3B10(
        __int64 a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  __int64 v8; // r2
  __int64 v9; // lr
  __int64 v12; // r3
  __int64 v13; // r2
  __int64 v14; // r31
  __int64 v15; // r2
  __int64 v16; // r2
  __int64 v17; // r2
  __int64 v18; // r2
  __int64 v19; // r2
  _WORD v20[88]; // [sp+50h] [-D0h] BYREF
  int v21; // [sp+100h] [-20h]
  __int64 v22; // [sp+118h] [-8h]

  v22 = v9;
  v21 = *(_DWORD *)(v8 + 0x10000);
  if ( (_DWORD)a3 == 16 )
  {
    v12 = HvpPhysicalToReal(a2, 16, a3, a4, a5, a6, a7, a8);
    j_XeCryptHmacShaInit(v20, v12, 16);
    v14 = v13 + 91104;
    j_XeCryptHmacShaUpdate(v20, v13 + 91136, 272);
    j_XeCryptHmacShaUpdate(v20, v14 + 304, 16);
    j_XeCryptHmacShaUpdate(v20, v15 + 72600, 10);
    j_XeCryptHmacShaUpdate(v20, 24, 8);
    j_XeCryptHmacShaUpdate(v20, v16 + 67768, 1424);
    j_XeCryptHmacShaUpdate(v20, 84, 16);
    j_XeCryptHmacShaUpdate(v20, v17 + 69640, 16);
    j_XeCryptHmacShaUpdate(v20, *(_QWORD *)(v18 + 91072) + 240LL, 16);
    j_XeCryptHmacShaUpdate(v20, v19 + 69624, 16);
    j_XeCryptHmacShaFinal(v20, a1, 16);
    v_MACHINE_CHECK_filter(v21);
    return 0;
  }
  else
  {
    v_MACHINE_CHECK_filter(v21);
    return -939524092;
  }
}


/* ========== [0x3c50] sub_3C50 (556 bytes) ========== */
__int64 __fastcall sub_3C50(unsigned int a1, int a2)
{
  __int64 v2; // r2
  _DWORD *v4; // r29
  unsigned __int64 v5; // r31
  _BYTE *v6; // r9
  _BYTE *v7; // r10
  __int64 i; // ctr
  __int64 v9; // r2
  int *v10; // r5
  __int64 v11; // r4
  __int64 v12; // r2
  __int64 v13; // r30
  unsigned int v14; // r11
  int v15; // r10
  unsigned int v16; // r11
  int v17; // r11
  bool v18; // zf
  _WORD v20[176]; // [sp+60h] [-1B0h] BYREF
  _WORD v21[8]; // [sp+1C0h] [-50h] BYREF
  _WORD v22[10]; // [sp+1D0h] [-40h] BYREF
  int v23; // [sp+1E4h] [-2Ch]

  v23 = *(_DWORD *)(v2 + 0x10000);
  if ( (a1 & 0xFFF) == 0 && a2 == 4096 )
  {
    v4 = (_DWORD *)(v2 + 92824);
    HvpAquireSpinLock((_DWORD *)(v2 + 92824));
    v5 = HvpRelocatePhysicalToProtected(a1, 0x1000u, 58);
    v6 = v21;
    v7 = (_BYTE *)(v5 + 256);
    for ( i = 16; i != 0; --i )
      *v6++ = *v7++;
    sub_B388(v20, 32);
    sub_B370(v20, v5 + 272, 3824, v5 + 272, v21, 0);
    j_XeCryptHmacSha(32, 16, v5 + 272, 3824, 0, 0, 0, 0);
    if ( (*(_DWORD *)(v5 + 272) & 1) != 0 )
      v10 = (int *)(v9 + 68312);
    else
      v10 = (int *)(v9 + 70424);
    if ( (unsigned int)sub_38D8((__int64)v22, v5, v10) != 0 )
    {
      if ( *(_DWORD *)(v5 + 276) != 1 )
        goto LABEL_11;
      v14 = *(_DWORD *)(v5 + 284);
      if ( v14 == 0 || (v15 = 0, v14 <= 0x4497) )
        v15 = 1;
      v16 = *(_DWORD *)(v5 + 288);
      if ( v16 == 0 || (v18 = v16 <= 0x4497, v17 = 0, !v18) )
        v17 = 1;
      if ( v15 == 0 || v17 == 0 )
      {
        v13 = -939524081;
        goto LABEL_23;
      }
      if ( *(_QWORD *)(v5 + 296) == *(_QWORD *)(*(_QWORD *)(v12 + 90832) + 32LL) )
      {
        LODWORD(v11) = *(_DWORD *)(v5 + 292);
        v13 = sub_B140(v5 + 304, v11);
      }
      else
      {
LABEL_11:
        v13 = -939524082;
      }
    }
    else
    {
      v13 = -939524083;
    }
LABEL_23:
    HvpZeroCacheLines(v5, 32);
    HvpRelocatePhysicalToEncryptedStrict((__int64 *)v5, 0x1000u, 58);
    HvpReleaseSpinLock(v4);
    v_MACHINE_CHECK_filter(v23);
    return v13;
  }
  v_MACHINE_CHECK_filter(v23);
  return -939524092;
}


/* ========== [0x3e80] sub_3E80 (260 bytes) ========== */
__int64 __fastcall sub_3E80(int a1)
{
  __int64 v1; // r2
  __int64 v2; // lr
  int v3; // r31
  _BYTE *v4; // r10
  _BYTE *v5; // r11
  __int64 i; // ctr
  int v7; // r3
  __int64 v8; // r11
  __int64 v10; // [sp+50h] [-1B0h] BYREF
  __int64 v11; // [sp+58h] [-1A8h]
  _BYTE v12[352]; // [sp+60h] [-1A0h] BYREF
  _QWORD v13[2]; // [sp+1C0h] [-40h] BYREF
  _QWORD v14[2]; // [sp+1D0h] [-30h] BYREF
  int v15; // [sp+1E0h] [-20h]
  __int64 v16; // [sp+1F8h] [-8h]

  v16 = v2;
  v15 = *(_DWORD *)(v1 + 0x10000);
  if ( (a1 & 0x8000000) != 0 )
  {
    v13[0] = -1;
    v10 = -1;
    v13[1] = -1;
    v11 = -1;
  }
  else
  {
    v14[0] = 0;
    v14[1] = 0;
    do
    {
      XeCryptRandom((uint8_t *)v13, 16);
      sub_B388(v12, v13);
      sub_B380(v12, v14, &v10, 0);
      v3 = sub_B450(v10);
    }
    while ( (unsigned int)sub_B450(v11) + v3 != 64 );
  }
  v4 = (_BYTE *)(v1 + 91056);
  v5 = v13;
  for ( i = 16; i != 0; --i )
    *v4++ = *v5++;
  v7 = v15;
  v8 = v11;
  *(_QWORD *)(v1 + 91632) = v10;
  *(_QWORD *)(v1 + 91640) = v8;
  return v_MACHINE_CHECK_filter(v7);
}


/* ========== [0x3f88] sub_3F88 (712 bytes) ========== */
__int64 sub_3F88()
{
  __int64 v0; // lr
  __int64 result; // r3
  __int64 v2; // r2
  __int64 v3; // r31
  _BYTE *v4; // r8
  __int64 v5; // r9
  char v6; // r7
  __int64 v7; // r11
  char v8; // r9
  _BYTE *v9; // r10
  __int64 v10; // r11
  char v11; // r9
  _BYTE *v12; // r10
  __int64 v13; // r31
  int v14; // r11
  __int64 SocMMIORegs; // r3
  unsigned int v16; // r11
  _WORD *v17; // r9
  __int64 v18; // r4
  _WORD *v19; // r10
  __int64 i; // ctr
  __int64 v21; // r2
  int v22; // r10
  __int64 v23; // r2
  _WORD v24[180]; // [sp+60h] [-180h] BYREF
  __int64 v25; // [sp+1D8h] [-8h]

  v25 = v0;
  result = sub_270();
  v3 = v2 + 67768;
  v4 = (_BYTE *)(v2 + 68856);
  v5 = 16;
  v6 = 0;
  *(_QWORD *)(v2 + 91072) = result;
  *(_QWORD *)(v2 + 91032) = result + 0x4000;
  *(_QWORD *)(v2 + 91688) = result + 63488;
  *(_QWORD *)(v2 + 91696) = result + 64896;
  do
  {
    --v5;
    v6 |= *v4++;
  }
  while ( (_DWORD)v5 != 0 );
  if ( v6 != 0 )
    result = j_XeCryptHmacSha(24, 8, v2 + 68856, 16, 0, 0, v5, 0);
  v7 = 16;
  v8 = 0;
  v9 = (_BYTE *)(v3 + 1392);
  do
  {
    --v7;
    v8 |= *v9++;
  }
  while ( (_DWORD)v7 != 0 );
  if ( v8 != 0 )
    result = j_XeCryptHmacSha(24, 8, v3 + 1392, 16, 0, 0, 0, 0);
  v10 = 16;
  v11 = 0;
  v12 = (_BYTE *)(v3 + 1408);
  do
  {
    --v10;
    v11 |= *v12++;
  }
  while ( (_DWORD)v10 != 0 );
  if ( v11 != 0 )
    result = j_XeCryptHmacSha(24, 8, v3 + 1408, 16, 0, 0, 0, 0);
  v13 = v2 + 91104;
  v14 = *(_WORD *)(v2 + 91104) & 0xFFF;
  if ( v14 != 834 && v14 != 866 && v14 != 836 )
  {
    memset(v2 + 91104, 0, 512);
    SocMMIORegs = HvpGetSocMMIORegs(0);
    v16 = *(_DWORD *)(SocMMIORegs + 252);
    if ( v16 > 0x5E98 || (v16 & 7) != 0 )
      v_MACHINE_CHECK_0();
    v17 = (_WORD *)(v13 + 6);
    v18 = *(unsigned int *)(SocMMIORegs + 252) + SocMMIORegs;
    v19 = (_WORD *)(v18 + 344);
    *(_WORD *)v13 = 20046;
    *(_WORD *)(v13 + 2) = 17559;
    for ( i = 5; i != 0; --i )
      *v17++ = *v19++;
    *(_QWORD *)(v13 + 16) = *(_QWORD *)(v18 + 328);
    *(_QWORD *)(v13 + 24) = *(_QWORD *)(v18 + 336);
    result = memcpy(v13 + 32, v18);
    v22 = *(_WORD *)(v21 + 92828) & 2;
    *(_QWORD *)(v13 + 40) = 0xFD336B67936AA211LL;
    if ( v22 != 0 )
    {
      sub_B388(v24, v13 + 16);
      return sub_B380(v24, v23 + 72584, v13 + 304, 0);
    }
    else
    {
      *(_QWORD *)(v13 + 304) = *(_QWORD *)(v13 + 16);
      *(_QWORD *)(v13 + 312) = *(_QWORD *)(v13 + 24);
    }
  }
  return result;
}


/* ========== [0x4250] HvxKeysGetFactoryChallenge (132 bytes) ========== */
__int64 __fastcall HvxKeysGetFactoryChallenge(
        __int64 a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  _BYTE *v9; // r3
  __int64 v10; // r2
  _BYTE *v11; // r11
  __int64 i; // ctr

  if ( (unsigned int)a2 < 0x14 )
    return -939524093;
  v9 = (_BYTE *)HvpPhysicalToReal(a1, a2, a3, a4, a5, a6, a7, a8);
  v11 = (_BYTE *)(v10 + 91712);
  for ( i = 20; i != 0; --i )
    *v9++ = *v11++;
  return 0;
}


/* ========== [0x42d8] HvxKeysSaveBootLoader (3252 bytes) ========== */
// write access to const memory has been detected, the output may be wrong!
__int64 HvxKeysSaveBootLoader()
{
  __int64 v0; // r2

  v_MACHINE_CHECK_filter(*(_DWORD *)(v0 + 0x10000));
  return -939524080;
}


/* ========== [0x4f90] HvxKeysSaveKeyVault (244 bytes) ========== */
__int64 HvxKeysSaveKeyVault()
{
  return -939524089;
}


/* ========== [0x5088] HvxKeysGetKeyProperties (76 bytes) ========== */
__int64 __fastcall HvxKeysGetKeyProperties(unsigned int a1)
{
  __int64 v1; // r2
  __int64 v2; // r11
  __int64 v3; // r10

  if ( a1 < 0x39 )
  {
    v2 = v1 + 69312;
    v3 = 4LL * a1;
    return *(unsigned int *)(v3 + v2);
  }
  if ( a1 - 256 <= 0xD )
  {
    v2 = v1 + 69568;
    v3 = 4LL * (a1 - 256);
    return *(unsigned int *)(v3 + v2);
  }
  return 0;
}


/* ========== [0x50d8] HvxKeysGetDigest (220 bytes) ========== */
__int64 __fastcall HvxKeysGetDigest(__int64 a1, __int64 a2)
{
  __int64 v2; // r2
  __int64 v3; // lr
  __int64 v5; // r31
  __int64 v7; // r10
  __int64 v8; // r9
  __int64 v9; // r8
  __int64 v10; // r7
  __int64 v11; // r6
  __int64 v12; // r5
  __int64 v13; // r3
  _WORD v14[10]; // [sp+50h] [-30h] BYREF
  int v15; // [sp+64h] [-1Ch]
  __int64 v16; // [sp+78h] [-8h]

  v16 = v3;
  v5 = a2;
  v15 = *(_DWORD *)(v2 + 0x10000);
  if ( (unsigned int)a2 <= 0x14 )
  {
    LODWORD(a2) = 16360;
    sub_B4D8(*(_QWORD *)(v2 + 91072) + 24LL, a2, 0, 0, 0, 0, v14, 20);
    v13 = HvpPhysicalToReal(a1, v5, v12, v11, v10, v9, v8, v7);
    memcpy(v13, v14);
    v_MACHINE_CHECK_filter(v15);
    return 0;
  }
  else
  {
    v_MACHINE_CHECK_filter(v15);
    return -939524092;
  }
}


/* ========== [0x51b8] HvxKeysRsaPrvCrypt (396 bytes) ========== */
__int64 __fastcall HvxKeysRsaPrvCrypt(__int64 a1, __int64 a2, __int64 a3, __int64 a4, __int64 a5)
{
  __int64 v5; // r2
  __int64 v10; // r8
  __int64 v11; // r9
  __int64 v12; // r10
  int v13; // r30
  __int64 v14; // r11
  __int64 v15; // r11
  unsigned int *v16; // r31
  int v18; // r30
  __int64 v19; // r10
  __int64 v20; // r9
  __int64 v21; // r8
  __int64 v22; // r7
  __int64 v23; // r6
  __int64 v24; // r5
  __int64 v25; // r3
  __int64 v26; // r4
  __int64 v27; // r5

  if ( (unsigned int)a1 >= 0x39 )
  {
    if ( (unsigned int)(a1 - 256) > 0xD )
      return -939524095;
    v12 = v5 + 69540;
    v15 = (unsigned int)(a1 - 256);
    v10 = 2 * v15;
    v11 = v5 + 69568;
    v13 = *(_DWORD *)(4 * v15 + v5 + 69568);
    v14 = *(_QWORD *)(v5 + 91032);
  }
  else
  {
    v10 = 2LL * (unsigned int)a1;
    v11 = v5 + 69312;
    v12 = v5 + 69192;
    v13 = *(_DWORD *)(4LL * (unsigned int)a1 + v5 + 69312);
    v14 = *(_QWORD *)(v5 + 91072);
  }
  v16 = (unsigned int *)(*(unsigned __int16 *)(v10 + v12) + v14 + 24);
  if ( (_DWORD)a1 == 53 )
  {
    LODWORD(a1) = 0;
    goto LABEL_7;
  }
  if ( (_DWORD)a1 == 52 )
  {
    LODWORD(a1) = 1;
LABEL_7:
    if ( (unsigned int)sub_B100(a1) == 0 )
      return -939524094;
  }
  if ( (v13 & 0x200000) == 0 )
    return -939524094;
  if ( (_DWORD)a3 != (_DWORD)a5 || (unsigned int)a3 != 8LL * *v16 )
    return -939524092;
  v18 = HvpPhysicalToReal(a2, a3, a3, a4, a5, v10, v11, (unsigned int)a3);
  v25 = HvpPhysicalToReal(a4, a5, v24, v23, v22, v21, v20, v19);
  LODWORD(v26) = v25;
  LODWORD(v27) = (_DWORD)v16;
  LODWORD(v25) = v18;
  if ( (unsigned int)j_XeCryptBnQwNeRsaPrvCrypt(v25, v26, v27) != 0 )
    return 0;
  else
    return -939524090;
}


/* ========== [0x5348] sub_5348 (460 bytes) ========== */
__int64 __fastcall sub_5348(unsigned int a1, int a2, __int64 a3, __int64 a4, __int64 a5, __int64 a6, __int64 a7)
{
  __int64 v7; // r2
  __int64 v8; // lr
  _BYTE *v11; // r11
  __int64 v12; // r2
  _BYTE *v13; // r10
  __int64 i; // ctr
  int v15; // r11
  unsigned int v16; // r9
  _BYTE *v17; // r10
  _BYTE *v18; // r11
  __int64 j; // ctr
  _WORD v20[176]; // [sp+50h] [-170h] BYREF
  __int64 v21; // [sp+1B8h] [-8h]

  v21 = v8;
  if ( a1 == 0 )
  {
    if ( (_DWORD)a4 != 16 )
      return -939524092;
    v11 = (_BYTE *)HvpPhysicalToReal(a3, 16, a3, a4, a5, a6, a7, 1);
    v13 = (_BYTE *)a5;
    for ( i = 16; i != 0; --i )
      *v13++ = *v11++;
    sub_B388(v20, *(_QWORD *)(v12 + 91072) + 224LL);
    sub_B380(v20, a5, a5, 0);
    return 0;
  }
  if ( a1 < 0x39 )
  {
    v15 = *(_DWORD *)(4LL * a1 + v7 + 69312);
    v16 = (unsigned __int16)v15;
    v17 = (_BYTE *)(*(unsigned __int16 *)(2LL * a1 + v7 + 69192) + *(_QWORD *)(v7 + 91072) + 24LL);
    goto LABEL_17;
  }
  if ( a1 == 67 )
  {
    v17 = (_BYTE *)(v7 + 69624);
LABEL_11:
    v15 = 5308416;
LABEL_16:
    v16 = 16;
LABEL_17:
    if ( (v15 & (a2 | 0x10000)) != (a2 | 0x10000) || v16 < 0x10 )
      return -939524094;
    v18 = (_BYTE *)a5;
    for ( j = 16; j != 0; --j )
      *v18++ = *v17++;
    return 0;
  }
  if ( a1 == 71 )
  {
    v17 = (_BYTE *)(v7 + 91648);
    goto LABEL_11;
  }
  if ( a1 - 224 <= 7 )
  {
    v15 = 13697024;
    v17 = (_BYTE *)(16LL * (a1 - 224) + *(_QWORD *)(v7 + 91696));
    goto LABEL_16;
  }
  return -939524095;
}


/* ========== [0x5518] HvxKeysHmacSha (196 bytes) ========== */
__int64 __fastcall HvxKeysHmacSha(__int64 a1, __int64 a2, __int64 a3, __int64 a4, __int64 a5, __int64 a6, __int64 a7)
{
  __int64 v7; // r2
  __int64 v11; // r5
  __int64 v14; // r27
  __int64 v15; // r10
  __int64 v16; // r9
  __int64 v17; // r8
  __int64 v18; // r7
  __int64 v19; // r6
  __int64 v20; // r5
  __int64 v22; // r29
  __int64 v23; // r10
  __int64 v24; // r9
  __int64 v25; // r8
  __int64 v26; // r7
  __int64 v27; // r6
  __int64 v28; // r5
  _WORD v29[8]; // [sp+60h] [-50h] BYREF
  int v30; // [sp+70h] [-40h]

  v11 = a2;
  LODWORD(a2) = 0x800000;
  v30 = *(_DWORD *)(v7 + 0x10000);
  v14 = sub_5348(a1, a2, v11, a3, v29);
  if ( (_DWORD)v14 == 0 )
  {
    if ( (unsigned int)a7 > 0x14 )
    {
      v_MACHINE_CHECK_filter(v30);
      return -939524092;
    }
    v22 = HvpPhysicalToReal(a4, a5, v20, v19, v18, v17, v16, v15);
    HvpPhysicalToReal(a6, a7, v28, v27, v26, v25, v24, v23);
    j_XeCryptHmacSha(v29, 16, v22, a5, 0, 0, 0, 0);
  }
  v_MACHINE_CHECK_filter(v30);
  return v14;
}


/* ========== [0x55e0] HvxKeysAesCbc (408 bytes) ========== */
__int64 __fastcall HvxKeysAesCbc(
        __int64 a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        unsigned int a5,
        __int64 a6,
        __int64 a7,
        int a8)
{
  __int64 v8; // r2
  int v11; // r31
  int v13; // r26
  __int64 v16; // r25
  __int64 v17; // r2
  _DWORD *v19; // r24
  __int64 v20; // r4
  __int64 v21; // r31
  __int64 v22; // r4
  __int64 v23; // r30
  __int64 v24; // r10
  __int64 v25; // r9
  __int64 v26; // r8
  __int64 v27; // r7
  __int64 v28; // r6
  __int64 v29; // r5
  __int64 v30; // r4
  unsigned int i; // r29
  __int64 v32; // r7
  __int64 v33; // r8
  __int64 v34; // r6
  __int64 v35; // r5
  __int64 v36; // r4
  _WORD v37[176]; // [sp+50h] [-1D0h] BYREF
  _WORD v38[8]; // [sp+1B0h] [-70h] BYREF
  int v39; // [sp+1C0h] [-60h]

  v11 = HIDWORD(a1);
  v13 = a1;
  v39 = *(_DWORD *)(v8 + 0x10000);
  v16 = sub_5348(HIDWORD(a1), 0x400000, a2, a3, v38);
  if ( (_DWORD)v16 == 0 )
  {
    if ( v11 == 27 && (unsigned int)sub_B100(8) == 0 )
    {
      v_MACHINE_CHECK_filter(v39);
      return -939524094;
    }
    if ( (a5 & 0xF) != 0 )
    {
      v_MACHINE_CHECK_filter(v39);
      return -939524092;
    }
    v19 = (_DWORD *)(v17 + 92448);
    HvpAquireSpinLock((_DWORD *)(v17 + 92448));
    LODWORD(v20) = a5;
    v21 = HvpPhysicalToRealExt(a4, v20);
    LODWORD(v22) = a5;
    v23 = HvpPhysicalToRealExt(a6, v22);
    HvpSetRMCIEx(0, v21, v23);
    sub_B388(v37, v38);
    if ( a8 != 0 )
    {
      if ( a8 == 16 )
      {
        LODWORD(v32) = HvpPhysicalToReal(a7, 16, v29, v28, v27, v26, v25, v24);
        LODWORD(v33) = v13;
        LODWORD(v34) = v23;
        LODWORD(v35) = a5;
        LODWORD(v36) = v21;
        sub_B370(v37, v36, v35, v34, v32, v33);
      }
      else
      {
        v16 = -939524092;
      }
    }
    else
    {
      for ( i = a5 >> 4; i != 0; --i )
      {
        LODWORD(v28) = v13;
        LODWORD(v29) = v23;
        LODWORD(v30) = v21;
        sub_B380(v37, v30, v29, v28);
        v21 += 16;
        v23 += 16;
      }
    }
    HvpSetRMCIEx(1, v21, v23);
    HvpReleaseSpinLock(v19);
  }
  v_MACHINE_CHECK_filter(v39);
  return v16;
}


/* ========== [0x5778] HvxKeysDes2Cbc (364 bytes) ========== */
__int64 __fastcall HvxKeysDes2Cbc(
        __int64 a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        int a8)
{
  __int64 v8; // r2
  int v11; // r30
  int v13; // r26
  __int64 v16; // r24
  __int64 v17; // r10
  __int64 v18; // r9
  __int64 v19; // r8
  __int64 v20; // r7
  __int64 v21; // r6
  __int64 v22; // r5
  __int64 v24; // r30
  __int64 v25; // r10
  __int64 v26; // r9
  __int64 v27; // r8
  __int64 v28; // r7
  __int64 v29; // r6
  __int64 v30; // r5
  __int64 v31; // r29
  __int64 v32; // r4
  __int64 v33; // r10
  __int64 v34; // r9
  __int64 v35; // r8
  __int64 v36; // r7
  __int64 v37; // r6
  __int64 v38; // r5
  __int64 v39; // r4
  unsigned int i; // r31
  __int64 v41; // r7
  __int64 v42; // r8
  __int64 v43; // r6
  __int64 v44; // r5
  __int64 v45; // r4
  _WORD v46[192]; // [sp+50h] [-1F0h] BYREF
  _QWORD v47[3]; // [sp+1D0h] [-70h] BYREF
  int v48; // [sp+1E8h] [-58h]

  v11 = HIDWORD(a1);
  v13 = a1;
  v48 = *(_DWORD *)(v8 + 0x10000);
  v16 = sub_5348(HIDWORD(a1), 0x400000, a2, a3, v47);
  if ( (_DWORD)v16 == 0 )
  {
    if ( v11 == 27 && (unsigned int)sub_B100(8) == 0 )
    {
      v_MACHINE_CHECK_filter(v48);
      return -939524094;
    }
    if ( (a5 & 7) != 0 )
      goto LABEL_6;
    v24 = HvpPhysicalToReal(a4, a5, v22, v21, v20, v19, v18, v17);
    v31 = HvpPhysicalToReal(a6, a5, v30, v29, v28, v27, v26, v25);
    LODWORD(v32) = 24;
    v47[2] = v47[0];
    j_XeCryptDesParity(v47, v32, v47);
    j_XeCryptDes3Key(v46, v47);
    if ( a8 != 0 )
    {
      if ( a8 != 8 )
      {
LABEL_6:
        v_MACHINE_CHECK_filter(v48);
        return -939524092;
      }
      LODWORD(v41) = HvpPhysicalToReal(a7, 8, v38, v37, v36, v35, v34, v33);
      LODWORD(v42) = v13;
      LODWORD(v43) = v31;
      LODWORD(v44) = a5;
      LODWORD(v45) = v24;
      j_XeCryptDes3Cbc(v46, v45, v44, v43, v41, v42);
    }
    else
    {
      for ( i = (unsigned int)a5 >> 3; i != 0; --i )
      {
        LODWORD(v37) = v13;
        LODWORD(v38) = v31;
        LODWORD(v39) = v24;
        j_XeCryptDes3Ecb(v46, v39, v38, v37);
        v24 += 8;
        v31 += 8;
      }
    }
  }
  v_MACHINE_CHECK_filter(v48);
  return v16;
}


/* ========== [0x58e8] HvxKeysDesCbc (312 bytes) ========== */
__int64 __fastcall HvxKeysDesCbc(
        __int64 a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        int a8)
{
  __int64 v8; // r2
  int v14; // r27
  __int64 v15; // r25
  __int64 v16; // r10
  __int64 v17; // r9
  __int64 v18; // r8
  __int64 v19; // r7
  __int64 v20; // r6
  __int64 v21; // r5
  __int64 v23; // r30
  __int64 v24; // r10
  __int64 v25; // r9
  __int64 v26; // r8
  __int64 v27; // r7
  __int64 v28; // r6
  __int64 v29; // r5
  __int64 v30; // r29
  __int64 v31; // r10
  __int64 v32; // r9
  __int64 v33; // r8
  __int64 v34; // r7
  __int64 v35; // r6
  __int64 v36; // r5
  __int64 v37; // r4
  unsigned int i; // r31
  __int64 v39; // r7
  __int64 v40; // r8
  __int64 v41; // r6
  __int64 v42; // r5
  __int64 v43; // r4
  _WORD v44[64]; // [sp+50h] [-E0h] BYREF
  _WORD v45[8]; // [sp+D0h] [-60h] BYREF
  int v46; // [sp+E0h] [-50h]

  v46 = *(_DWORD *)(v8 + 0x10000);
  v14 = a1;
  v15 = sub_5348(HIDWORD(a1), 0x4000000, a2, a3, v45);
  if ( (_DWORD)v15 == 0 )
  {
    if ( (a5 & 7) != 0 )
    {
LABEL_3:
      v_MACHINE_CHECK_filter(v46);
      return -939524092;
    }
    v23 = HvpPhysicalToReal(a4, a5, v21, v20, v19, v18, v17, v16);
    v30 = HvpPhysicalToReal(a6, a5, v29, v28, v27, v26, v25, v24);
    j_XeCryptDesParity(v45, 8, v45);
    j_XeCryptDesKey(v44, v45);
    if ( a8 != 0 )
    {
      if ( a8 != 8 )
        goto LABEL_3;
      LODWORD(v39) = HvpPhysicalToReal(a7, 8, v36, v35, v34, v33, v32, v31);
      LODWORD(v40) = v14;
      LODWORD(v41) = v30;
      LODWORD(v42) = a5;
      LODWORD(v43) = v23;
      j_XeCryptDesCbc(v44, v43, v42, v41, v39, v40);
    }
    else
    {
      for ( i = (unsigned int)a5 >> 3; i != 0; --i )
      {
        LODWORD(v35) = v14;
        LODWORD(v36) = v30;
        LODWORD(v37) = v23;
        j_XeCryptDesEcb(v44, v37, v36, v35);
        v23 += 8;
        v30 += 8;
      }
    }
  }
  v_MACHINE_CHECK_filter(v46);
  return v15;
}


/* ========== [0x5a20] HvxKeysObscureKey (280 bytes) ========== */
__int64 __fastcall HvxKeysObscureKey(
        __int64 a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7)
{
  __int64 v7; // r2
  _BYTE *v10; // r31
  __int64 v11; // r10
  __int64 v12; // r9
  __int64 v13; // r8
  __int64 v14; // r7
  __int64 v15; // r6
  __int64 v16; // r5
  _BYTE *v17; // r30
  __int64 v18; // r2
  _BYTE *v19; // r10
  __int64 i; // ctr
  _BYTE *v21; // r10
  __int64 j; // ctr
  _WORD v24[176]; // [sp+50h] [-1A0h] BYREF
  _WORD v25[8]; // [sp+1B0h] [-40h] BYREF
  int v26; // [sp+1C0h] [-30h]

  v26 = *(_DWORD *)(v7 + 0x10000);
  if ( (unsigned int)a2 < 0x10 || (unsigned int)a4 < 0x10 )
  {
    v_MACHINE_CHECK_filter(v26);
    return -939524093;
  }
  else
  {
    v10 = (_BYTE *)HvpPhysicalToReal(a1, a2, a3, a4, a5, a6, a7, 2);
    v17 = (_BYTE *)HvpPhysicalToReal(a3, a4, v16, v15, v14, v13, v12, v11);
    v19 = v25;
    for ( i = 16; i != 0; --i )
      *v19++ = *v10++;
    sub_B388(v24, *(_QWORD *)(v18 + 91072) + 224LL);
    sub_B380(v24, v25, v25, 1);
    v21 = v25;
    for ( j = 16; j != 0; --j )
      *v17++ = *v21++;
    v_MACHINE_CHECK_filter(v26);
    return 0;
  }
}


/* ========== [0x5b38] HvxKeysGetStatus (8 bytes) ========== */
__int64 HvxKeysGetStatus()
{
  return 34767315;
}


/* ========== [0x5b40] HvxKeysSaveSystemUpdate (1060 bytes) ========== */
__int64 __fastcall HvxKeysSaveSystemUpdate(unsigned int a1, int a2)
{
  __int64 v2; // r2
  _BYTE *v4; // r10
  _BYTE *v5; // r11
  __int64 i; // ctr
  _DWORD *v7; // r23
  __int64 v8; // r4
  unsigned __int64 v9; // r3
  __int64 v10; // r31
  __int64 v11; // r30
  __int64 v12; // r11
  __int64 v13; // r27
  unsigned int v14; // r26
  __int64 v15; // r27
  __int64 v16; // r2
  __int16 v17; // r8
  _WORD v19[136]; // [sp+60h] [-390h] BYREF
  _WORD v20[8]; // [sp+170h] [-280h] BYREF
  _WORD v21[8]; // [sp+180h] [-270h] BYREF
  _WORD v22[256]; // [sp+190h] [-260h] BYREF
  int v23; // [sp+390h] [-60h]

  v23 = *(_DWORD *)(v2 + 0x10000);
  if ( (_WORD)a1 == 0 && a2 == 0x10000 )
  {
    v4 = v21;
    v5 = (_BYTE *)(v2 + 91408);
    for ( i = 16; i != 0; --i )
      *v4++ = *v5++;
    v7 = (_DWORD *)(v2 + 91664);
    HvpAquireSpinLock((_DWORD *)(v2 + 91664));
    v9 = HvpRelocatePhysicalToProtected(a1, 0x10000u, 60);
    v10 = v9;
    if ( (*(_WORD *)v9 & 0xFFF) == 0x346 )
    {
      v12 = *(unsigned int *)(v9 + 12);
      v13 = (v12 + 15) & 0xFFFFFFF0LL;
      if ( (unsigned int)v12 < 0x360 || (unsigned int)v13 < (unsigned int)v12 || (unsigned int)v13 > 0x10000 )
      {
        v11 = -939524060;
      }
      else if ( ((*(_WORD *)v9 ^ 0x4E4E) & 0xF000) != 0 )
      {
        v11 = -939524059;
      }
      else if ( *(_DWORD *)(v9 + 16) == 123731968 )
      {
        v14 = *(_DWORD *)(v9 + 20);
        if ( (v14 & 0x8000) == 0 )
        {
          LODWORD(v8) = 0;
          LODWORD(v9) = 1;
          if ( (unsigned int)sub_35B0(v9, v8) != 0 || v14 > 0x44970000 )
          {
            memcpy(v22, v10 + 48);
            v11 = 0;
            j_XeCryptHmacSha(v21, 16, v10 + 32, 16, 0, 0, 0, 0);
            j_XeCryptRc4Key(v19, v20, 16);
            v15 = v13 - 48;
            j_XeCryptRc4Ecb(v19, v10 + 48, v15);
            memset(v10 + 48, 0, 512);
            memcpy(v10 + 48, v22);
            *(_BYTE *)(v10 + 539) = 1;
            v17 = *(_WORD *)(v10 + 6);
            *(_DWORD *)(v10 + 540) = *(unsigned __int8 *)(v16 + 91680) | 0xC81CB400;
            if ( (v17 & 0x4000) != 0
              && *(unsigned __int8 *)(v16 + 91680) < (unsigned int)*(unsigned __int8 *)(v16 + 91736)
              && v14 >= 0x44970000 )
            {
              ++*(_DWORD *)(v10 + 540);
            }
            XeCryptRandom((uint8_t *)(v10 + 32), 0x10u);
            j_XeCryptHmacSha(v21, 16, v10 + 32, 16, 0, 0, 0, 0);
            j_XeCryptHmacSha(32, 16, v10, 32, v20, 16, v10 + 48, 496);
            j_XeCryptRc4Key(v19, v20, 16);
            j_XeCryptRc4Ecb(v19, v10 + 48, v15);
            goto LABEL_25;
          }
        }
        v11 = -939524057;
      }
      else
      {
        v11 = -939524058;
      }
    }
    else
    {
      v11 = -939524061;
    }
    HvpZeroCacheLines(v10, 512);
LABEL_25:
    HvpRelocatePhysicalToEncryptedStrict((__int64 *)v10, 0x10000u, 60);
    HvpReleaseSpinLock(v7);
    v_MACHINE_CHECK_filter(v23);
    return v11;
  }
  v_MACHINE_CHECK_filter(v23);
  return -939524063;
}


/* ========== [0x5f68] HvxKeysExecute (724 bytes) ========== */
__int64 __fastcall HvxKeysExecute(
        __int64 phys_address,
        __int64 size,
        uint64_t arg1,
        uint64_t arg2,
        uint64_t arg3,
        uint64_t arg4)
{
  __int64 v6; // r2
  int v9; // r22
  int v10; // r21
  int v11; // r20
  int v12; // r19
  _DWORD *v13; // r17
  unsigned __int64 v14; // r3
  __int64 v15; // r2
  unsigned __int64 v16; // r31
  __int64 v17; // r30
  _QWORD *v18; // r11
  __int64 i; // ctr
  __int64 v20; // r10
  __int64 v21; // r9
  __int64 v22; // r8
  __int64 v23; // r7
  __int64 v24; // r6
  __int64 v25; // r5
  __int64 v26; // r3
  __int64 v27; // r4
  __int64 v28; // r31
  __int64 v30; // r26
  __int64 v31; // r27
  unsigned int v32; // r24
  int v33; // r28
  _QWORD *v34; // r29
  __int64 v35; // r8
  __int64 v36; // r6
  __int64 v37; // r5
  __int64 v38; // r4
  __int64 v39; // r3
  __int64 v40; // r3
  __int64 v41; // r6
  int v42; // r2
  __int64 v43; // r5
  __int64 v44; // r7
  __int64 v45; // r6
  __int64 v46; // r5
  __int64 v47; // r4
  _WORD v48[136]; // [sp+60h] [-2D0h] BYREF
  _WORD v49[8]; // [sp+170h] [-1C0h] BYREF
  _WORD v50[16]; // [sp+180h] [-1B0h] BYREF
  _WORD v51[128]; // [sp+1A0h] [-190h] BYREF
  int v52; // [sp+2A0h] [-90h]

  v9 = arg1;
  v10 = arg2;
  v11 = arg3;
  v12 = arg4;
  v52 = *(_DWORD *)(v6 + 0x10000);
  if ( (phys_address & 0x7F) == 0
    && (size & 0x7F) == 0
    && (unsigned int)(size - 288) <= 0xFEE0
    && ((((_DWORD)phys_address + (_DWORD)size - 1) ^ (unsigned int)phys_address) & 0xFFFF0000) == 0 )
  {
    v13 = (_DWORD *)(v6 + 91616);
    HvpAquireSpinLock((_DWORD *)(v6 + 91616));
    v14 = HvpRelocatePhysicalToProtected(phys_address, size, 62);
    v16 = v14;
    if ( (*(_WORD *)v14 & 0xF0F) != 0xD0D )
    {
      v17 = -939524046;
      goto LABEL_7;
    }
    if ( ((*(_WORD *)v14 ^ 0x4E4E) & 0xF000) != 0 )
    {
      v17 = -939524045;
      goto LABEL_7;
    }
    v30 = *(unsigned int *)(v14 + 12);
    v31 = (v30 + 15) & 0xFFFFFFF0LL;
    if ( (unsigned int)v30 < 0x120 || (unsigned int)v31 < (unsigned int)v30 || (unsigned int)v31 > (unsigned int)size )
    {
      v17 = -939524044;
    }
    else
    {
      v32 = *(_DWORD *)(v14 + 8);
      if ( (v32 & 3) != 0 || v32 < 0x120 || v32 >= ((unsigned int)v30 & 0xFFFFFFFC) )
      {
        v17 = -939524043;
        goto LABEL_7;
      }
      v33 = v15 + 91104;
      v34 = (_QWORD *)(v14 + 16);
      j_XeCryptHmacSha(v15 + 91408, 16, v14 + 16, 16, 0, 0, 0, 0);
      j_XeCryptRc4Key(v48, v49, 16);
      j_XeCryptRc4Ecb(v48, v16 + 32, v31 - 32);
      LODWORD(v35) = 20;
      LODWORD(v36) = v31 - 288;
      LODWORD(v37) = v16 + 288;
      LODWORD(v38) = 16;
      LODWORD(v39) = v16;
      HIDWORD(v40) = (unsigned __int64)j_XeCryptSha(v39, v38, v37, v36, v50, v35) >> 32;
      LODWORD(v41) = v33 + 32;
      LODWORD(v43) = v42 + 72612;
      LODWORD(v40) = v16 + 32;
      if ( (unsigned int)j_XeCryptBnQwBeSigVerify(v40, v50, v43, v41) == 0 )
      {
        v17 = -939524042;
        goto LABEL_7;
      }
      *v34 = 0;
      *(_QWORD *)(v16 + 24) = 0;
      memset(v16 + 32, 0, 256);
      if ( (unsigned int)v30 < (unsigned int)size )
        memset((unsigned int)v30 + v16, 0, size - v30);
      LODWORD(v44) = v12;
      LODWORD(v45) = v11;
      LODWORD(v46) = v10;
      LODWORD(v47) = v9;
      v17 = ((__int64 (__fastcall *)(unsigned __int64, __int64, __int64, __int64, __int64))(v32 + v16))(
              v16,
              v47,
              v46,
              v45,
              v44);
    }
    if ( (_DWORD)v17 == 0 )
    {
      memcpy(v51, v16 + 32);
LABEL_9:
      sub_D7C(v16);
      v26 = HvpPhysicalToReal(phys_address, size, v25, v24, v23, v22, v21, v20);
      LODWORD(v27) = (unsigned int)size >> 7;
      v28 = v26;
      HvpZeroCacheLines(v26, v27);
      memcpy(v28 + 32, v51);
      HvpReleaseSpinLock(v13);
      v_MACHINE_CHECK_filter(v52);
      return v17;
    }
LABEL_7:
    v18 = v51;
    for ( i = 32; i != 0; --i )
      *v18++ = 0;
    goto LABEL_9;
  }
  v_MACHINE_CHECK_filter(v52);
  return -939524048;
}


/* ========== [0x6240] HvxKeysExCreateKeyVault (124 bytes) ========== */
__int64 HvxKeysExCreateKeyVault()
{
  return -939524089;
}


/* ========== [0x62c0] HvxKeysExLoadKeyVault (256 bytes) ========== */
__int64 HvxKeysExLoadKeyVault()
{
  return -939524094;
}


/* ========== [0x63c0] HvxKeysExSaveKeyVault (216 bytes) ========== */
__int64 __fastcall HvxKeysExSaveKeyVault(unsigned int a1, int a2)
{
  __int64 *v4; // r31
  __int64 v5; // r2
  __int64 v6; // r5

  if ( (a1 & 0xFFF) != 0 )
    return -939524087;
  if ( a2 != 0x4000 )
    return -939524088;
  HvpSetRMCI(0);
  v4 = (__int64 *)HvpRelocatePhysicalToEncrypted(a1, 0x4000u);
  memcpy(v4, *(_QWORD *)(v5 + 91032));
  HvpKeysExKeyVaultSeal(v4);
  HvpRelocateEncryptedToPhysical(v4, 0x4000u, v6);
  HvpSetRMCI(1);
  return 0;
}


/* ========== [0x6498] HvxKeysExSetKey (332 bytes) ========== */
__int64 __fastcall HvxKeysExSetKey(int a1, __int64 a2, __int64 a3, __int64 a4, __int64 a5)
{
  __int64 v5; // r2
  __int64 v10; // r11
  int v11; // r31
  __int64 v12; // r10
  __int64 v13; // r29
  _WORD *v14; // r28
  __int64 v15; // r3
  __int64 v16; // r31
  bool v17; // zf
  _WORD *v18; // r3

  if ( (unsigned int)(a1 - 256) > 0xD )
    return -939524095;
  if ( (unsigned int)(a1 - 259) <= 5 && (unsigned int)sub_B100(8) == 0 )
    return -939524094;
  v10 = (unsigned int)(a1 - 256);
  v11 = *(_DWORD *)(4 * v10 + v5 + 69568);
  v12 = *(unsigned __int16 *)(2 * v10 + v5 + 69540);
  v14 = (_WORD *)(v12 + *(_QWORD *)(v5 + 91032) + 24);
  v15 = HvpPhysicalToReal(a2, a3, a3, a4, a5, 4 * v10, v5 + 69540, v12);
  v13 = (unsigned __int16)v11;
  v17 = (v11 & 0x10000) == 0;
  v16 = v15;
  if ( v17 )
  {
    if ( (unsigned int)a3 > (unsigned int)v13 || (unsigned int)(v13 - a3) < 2 )
      return -939524092;
    memset(v14, 0, v13);
    v18 = v14 + 1;
    *v14 = a3;
  }
  else
  {
    if ( (_DWORD)a3 != (_DWORD)v13 )
      return -939524092;
    v18 = v14;
  }
  memcpy(v18, v16);
  return 0;
}


/* ========== [0x65e8] HvxKeysExGetKey (336 bytes) ========== */
__int64 __fastcall HvxKeysExGetKey(int a1, __int64 a2, __int64 a3, __int64 a4, __int64 a5)
{
  __int64 v5; // r2
  __int64 v7; // r11
  int v8; // r31
  __int64 v9; // r10
  unsigned int v10; // r30
  unsigned __int16 *v11; // r27
  unsigned int *v12; // r28
  __int64 v13; // r3
  unsigned int v14; // r29
  __int64 v15; // r10
  __int64 v16; // r9
  __int64 v17; // r8
  __int64 v18; // r7
  __int64 v19; // r6
  __int64 v20; // r5
  __int64 v21; // r3
  unsigned int v23; // r11

  if ( (unsigned int)(a1 - 256) > 0xD )
    return -939524095;
  if ( a1 == 259 || a1 == 260 )
    return -939524094;
  v7 = (unsigned int)(a1 - 256);
  v8 = *(_DWORD *)(4 * v7 + v5 + 69568);
  v9 = *(unsigned __int16 *)(2 * v7 + v5 + 69540);
  v10 = (unsigned __int16)v8;
  v11 = (unsigned __int16 *)(v9 + *(_QWORD *)(v5 + 91032) + 24);
  v12 = (unsigned int *)HvpPhysicalToReal(a3, 4, a3, a4, a5, 4 * v7, v5 + 69540, v9);
  v13 = a2;
  v14 = *v12;
  v21 = HvpPhysicalToReal(v13, *v12, v20, v19, v18, v17, v16, v15);
  if ( (v8 & 0x10000) != 0 )
  {
    if ( v14 != (unsigned __int16)v8 )
      return -939524092;
    goto LABEL_9;
  }
  if ( (unsigned __int16)v8 < 2u )
  {
    v10 = 0;
LABEL_9:
    memcpy(v21, v11);
    *v12 = v10;
    return 0;
  }
  v23 = *v11;
  v10 = (unsigned __int16)v8 - 2;
  ++v11;
  if ( v10 > v23 )
    v10 = v23;
  if ( v14 >= v10 )
    goto LABEL_9;
  return -939524093;
}


/* ========== [0x6738] HvxGetUpdateSequence (36 bytes) ========== */
__int64 HvxGetUpdateSequence()
{
  return 0;
}


/* ========== [0x6760] sub_6760 (196 bytes) ========== */
bool __fastcall sub_6760(__int64 a1)
{
  __int64 v1; // r2
  __int64 v2; // r31
  _WORD v4[8]; // [sp+60h] [-50h] BYREF
  _WORD v5[8]; // [sp+70h] [-40h] BYREF
  int v6; // [sp+80h] [-30h]

  v2 = a1;
  v6 = *(_DWORD *)(v1 + 0x10000);
  j_XeCryptHmacSha(32, 16, a1, 16, 0, 0, 0, 0);
  j_XeCryptRc4(v4, 16, v2 + 16, 1008);
  j_XeCryptHmacSha(32, 16, v2 + 16, 1008, 0, 0, 0, 0);
  LOBYTE(v2) = _cntlzw(sub_B480(v2, v5, 16));
  v_MACHINE_CHECK_filter(v6);
  return (v2 & 0x20) != 0;
}


/* ========== [0x6828] HvpSecuritySettingsSeal (176 bytes) ========== */
__int64 __fastcall HvpSecuritySettingsSeal(__int64 a1)
{
  __int64 v1; // r2
  __int64 v3; // r30
  _WORD v5[8]; // [sp+60h] [-40h] BYREF
  int v6; // [sp+70h] [-30h]

  v3 = a1 + 16;
  v6 = *(_DWORD *)(v1 + 0x10000);
  XeCryptRandom((uint8_t *)(a1 + 16), 8);
  j_XeCryptHmacSha(32, 16, v3, 1008, 0, 0, 0, 0);
  j_XeCryptHmacSha(32, 16, a1, 16, 0, 0, 0, 0);
  j_XeCryptRc4(v5, 16, v3, 1008);
  return v_MACHINE_CHECK_filter(v6);
}


/* ========== [0x68d8] HvxSecurityInitialize (140 bytes) ========== */
__int64 HvxSecurityInitialize()
{
  return -939524032;
}


/* ========== [0x6968] HvxSecuritySaveSettings (228 bytes) ========== */
__int64 __fastcall HvxSecuritySaveSettings(unsigned int a1, int a2, __int64 a3)
{
  __int64 v3; // r2
  __int64 v4; // r31
  __int64 *v8; // r30
  __int64 v9; // r2
  __int64 v10; // r3
  __int64 v11; // r5

  v4 = v3 + 0x10000;
  if ( *(_BYTE *)(*(_QWORD *)(v3 + 91688) + 24LL) == 0 )
    return -939524032;
  if ( a2 != 1024 )
    return -939524031;
  if ( (a1 & 0xFFF) != 0 )
    return -939524030;
  HvpSetRMCI(0);
  v8 = (__int64 *)HvpRelocatePhysicalToEncrypted(a1, 0x400u);
  *(_BYTE *)(*(_QWORD *)(v4 + 26152) + 25LL) = *(_BYTE *)(v9 + 91680);
  if ( (unsigned int)((__int64 (*)(void))loc_2B70)() != 0 )
    ++*(_BYTE *)(*(_QWORD *)(v4 + 26152) + 25LL);
  *(_QWORD *)(*(_QWORD *)(v4 + 26152) + 32LL) = a3;
  memcpy(v8, *(_QWORD *)(v4 + 26152));
  LODWORD(v10) = (_DWORD)v8;
  HvpSecuritySettingsSeal(v10);
  HvpRelocateEncryptedToPhysical(v8, 0x400u, v11);
  HvpSetRMCI(1);
  return 0;
}


/* ========== [0x6a50] HvxSecurityLoadSettings (352 bytes) ========== */
__int64 HvxSecurityLoadSettings()
{
  return -939524032;
}


/* ========== [0x6bb0] HvxSecuritySetDetected (148 bytes) ========== */
__int64 __fastcall HvxSecuritySetDetected(__int64 a1)
{
  __int64 v1; // r2
  __int64 v2; // r11

  v2 = *(_QWORD *)(v1 + 91688);
  if ( *(_BYTE *)(v2 + 24) == 0 )
    return -939524032;
  if ( (unsigned int)a1 >= 0x40 )
    return -939524028;
  if ( (*(_WORD *)(v1 + 92820) & 1) == 0 )
  {
    *(_QWORD *)(v2 + 40) |= 1LL << a1;
    sub_B138(a1);
  }
  return 0;
}


/* ========== [0x6c48] HvxSecurityGetDetected (76 bytes) ========== */
__int64 HvxSecurityGetDetected()
{
  __int64 v0; // r2
  __int64 result; // r3
  __int64 v2; // r11

  if ( (*(_WORD *)(v0 + 92820) & 1) != 0 )
    return 0;
  result = -1;
  v2 = *(_QWORD *)(v0 + 91688);
  if ( *(_BYTE *)(v2 + 24) != 0 )
    return *(_QWORD *)(v2 + 40);
  return result;
}


/* ========== [0x6c98] HvxSecuritySetActivated (108 bytes) ========== */
__int64 __fastcall HvxSecuritySetActivated(__int64 a1)
{
  __int64 v1; // r2
  __int64 v2; // r11

  v2 = *(_QWORD *)(v1 + 91688);
  if ( *(_BYTE *)(v2 + 24) == 0 )
    return -939524032;
  if ( (*(_WORD *)(v1 + 92820) & 1) == 0 )
  {
    *(_QWORD *)(v2 + 48) = a1;
    sub_B138(a1);
  }
  return 0;
}


/* ========== [0x6d08] HvxSecurityGetActivated (76 bytes) ========== */
__int64 HvxSecurityGetActivated()
{
  __int64 v0; // r2
  __int64 result; // r3
  __int64 v2; // r11

  if ( (*(_WORD *)(v0 + 92820) & 1) != 0 )
    return 0;
  result = -1;
  v2 = *(_QWORD *)(v0 + 91688);
  if ( *(_BYTE *)(v2 + 24) != 0 )
    return *(_QWORD *)(v2 + 48);
  return result;
}


/* ========== [0x6d58] HvxSecuritySetStat (112 bytes) ========== */
__int64 __fastcall HvxSecuritySetStat(__int64 a1)
{
  __int64 v1; // r2
  __int64 v2; // r11

  v2 = *(_QWORD *)(v1 + 91688);
  if ( *(_BYTE *)(v2 + 24) == 0 )
    return -939524032;
  if ( (unsigned int)a1 >= 5 )
    return -939524027;
  ++*(_DWORD *)(4 * ((unsigned int)a1 + 14LL) + v2);
  sub_B138(a1);
  return 0;
}


/* ========== [0x6dc8] HvxGetProtectedFlags (12 bytes) ========== */
__int64 HvxGetProtectedFlags()
{
  __int64 v0; // r2

  return *(_QWORD *)(v0 + 91672);
}


/* ========== [0x6dd8] HvxSetProtectedFlag (140 bytes) ========== */
__int64 __fastcall HvxSetProtectedFlag(unsigned int a1, int a2)
{
  __int64 v2; // r2
  __int64 v3; // r31
  __int64 v4; // r10
  __int64 v5; // r11
  __int64 v6; // r11
  _DWORD *v7; // r30
  __int64 v8; // r2

  v3 = v2 + 0x10000;
  if ( a1 < 3 )
  {
    v4 = 1LL << a1;
    v5 = *(_QWORD *)(v2 + 91672);
    if ( a2 != 0 )
      v6 = v4 | v5;
    else
      v6 = v5 & ~v4;
    *(_QWORD *)(v2 + 91672) = v6;
  }
  v7 = (_DWORD *)(v2 + 90820);
  HvpAquireSpinLock((_DWORD *)(v2 + 90820));
  *(_QWORD *)(*(_QWORD *)(v8 + 90832) + 1656LL) = *(_QWORD *)(v3 + 26136);
  HvpReleaseSpinLock(v7);
  return 0;
}


/* ========== [0x6e68] sub_6E68 (1192 bytes) ========== */
// write access to const memory has been detected, the output may be wrong!
__int64 __fastcall sub_6E68(__int64 a1)
{
  __int64 v1; // r2
  __int64 v3; // r31
  _BYTE *v4; // r9
  _BYTE *v5; // r10
  __int64 i; // ctr
  _BYTE *v7; // r10
  _BYTE *v8; // r11
  __int64 j; // ctr
  _WORD *v10; // r24
  unsigned int *FlashBase; // r28
  __int64 v12; // r31
  unsigned int v13; // r22
  __int64 v14; // r5
  __int64 v15; // r30
  int v16; // r11
  _QWORD *v17; // r7
  __int64 v18; // r8
  __int64 v19; // r2
  __int64 v20; // r23
  _BYTE *v21; // r11
  _BYTE *v22; // r10
  __int64 k; // ctr
  __int64 v24; // r31
  unsigned int *v25; // r28
  __int64 v26; // r5
  __int64 v27; // r30
  _QWORD *v28; // r7
  unsigned int v29; // r11
  _DWORD v31[3]; // [sp+60h] [-3B0h] BYREF
  unsigned int v32; // [sp+6Ch] [-3A4h]
  _WORD v33[3]; // [sp+70h] [-3A0h] BYREF
  __int16 v34; // [sp+76h] [-39Ah]
  int v35; // [sp+78h] [-398h]
  unsigned int v36; // [sp+7Ch] [-394h]
  _WORD v37[136]; // [sp+80h] [-390h] BYREF
  _WORD v38[136]; // [sp+190h] [-280h] BYREF
  __int16 v39; // [sp+2A0h] [-170h] BYREF
  unsigned int v40; // [sp+2A8h] [-168h]
  _WORD v41[8]; // [sp+320h] [-F0h] BYREF
  _WORD v42[8]; // [sp+330h] [-E0h] BYREF
  _BYTE v43[32]; // [sp+340h] [-D0h] BYREF
  _QWORD v44[2]; // [sp+360h] [-B0h] BYREF
  _WORD v45[8]; // [sp+370h] [-A0h] BYREF
  _WORD v46[10]; // [sp+380h] [-90h] BYREF
  int v47; // [sp+394h] [-7Ch]

  v3 = v1 + 91104;
  v47 = *(_DWORD *)(v1 + 0x10000);
  memcpy(v37, v1 + 91136);
  v4 = v45;
  v5 = (_BYTE *)(v3 + 16);
  for ( i = 16; i != 0; --i )
    *v4++ = *v5++;
  v7 = v42;
  v8 = (_BYTE *)(v3 + 6);
  for ( j = 10; j != 0; --j )
    *v7++ = *v8++;
  v10 = nullptr;
  v44[0] = 0;
  v44[1] = 0;
  FlashBase = (unsigned int *)HvpGetFlashBase();
  CopyBy64((__int64)&v39, FlashBase, 16);
  v12 = v40;
  v13 = v40;
  if ( v40 != ((v40 + 15LL) & 0x1FFFFFFF0LL)
    || (unsigned __int64)v40 - 128 > 0x7FFFF7F
    || 0x8000000 - (unsigned __int64)v40 <= 0x10 )
  {
    v_MACHINE_CHECK_0();
  }
  CopyBy64((__int64)v31, (unsigned int *)((char *)FlashBase + v40), 2);
  *(_DWORD *)v33 = v31[0];
  v33[2] = HIWORD(v31[1]);
  v35 = v31[2];
  v36 = v32;
  v34 = 0;
  if ( (HIWORD(v31[0]) & 0xF0F) != 0x302 )
    v_MACHINE_CHECK_0();
  v15 = (v32 + 15) & 0xFFFFFFF0;
  if ( v12 != ((v12 + 15) & 0xFFFFFFFFFFFFFFF0LL)
    || (unsigned __int64)(v12 - 128) > 0x7FFFF7F
    || ((v32 + 15) & 0xFFFFFFF0) >= (unsigned __int64)(0x8000000 - v12)
    || v32 > 0x10000 )
  {
    v_MACHINE_CHECK_0();
  }
  LODWORD(v14) = (unsigned int)v15 >> 3;
  CopyBy64(a1, (unsigned int *)((char *)FlashBase + v12), v14);
  v16 = *(_WORD *)a1 & 0xF0;
  if ( v16 == 64 )
  {
    v17 = nullptr;
    v18 = 0;
  }
  else
  {
    if ( v16 != 96 )
      v_MACHINE_CHECK_0();
    v18 = 16;
    v17 = (_QWORD *)&byte_20;
    if ( (*(_WORD *)(a1 + 6) & 0x8000) == 0 )
      v17 = v44;
  }
  j_XeCryptHmacSha(v45, 16, a1 + 16, 16, v17, v18, 0, 0);
  j_XeCryptRc4Key(v38, v41, 16);
  j_XeCryptRc4Ecb(v38, a1 + 32, v15 - 32);
  j_XeCryptSha(a1, 16, a1 + 320, v15 - 320, v43, 20);
  if ( (unsigned int)j_XeCryptBnQwBeSigVerify(a1 + 64, v43, v42, v37) == 0 )
    v_MACHINE_CHECK_0();
  if ( (*(_WORD *)(a1 + 6) & 0x800) != 0 )
  {
    if ( (*(_WORD *)(a1 + 6) & 0x1000) != 0 )
    {
      v10 = v33;
      v20 = 16;
    }
    else
    {
      v20 = 0;
    }
    v21 = (_BYTE *)(a1 + 924);
    v22 = v46;
    for ( k = 20; k != 0; --k )
      *v22++ = *v21++;
    v24 = (unsigned int)v15 + v13;
    if ( v24 != ((v24 + 15) & 0x1FFFFFFF0LL)
      || (unsigned __int64)(v24 - 128) > 0x7FFFF7F
      || (unsigned __int64)(0x8000000 - v24) <= 0x10 )
    {
      v_MACHINE_CHECK_0();
    }
    v25 = (unsigned int *)((char *)FlashBase + v24);
    CopyBy64((__int64)v31, v25, 2);
    if ( (HIWORD(v31[0]) & 0xF0F) != 0x302 )
      v_MACHINE_CHECK_0();
    v27 = (v32 + 15) & 0xFFFFFFF0;
    if ( v24 != ((v24 + 15) & 0xFFFFFFFFFFFFFFF0LL)
      || ((v32 + 15) & 0xFFFFFFF0) >= (unsigned __int64)(0x8000000 - v24)
      || v32 > 0x10000 )
    {
      v_MACHINE_CHECK_0();
    }
    LODWORD(v26) = (unsigned int)v27 >> 3;
    CopyBy64(a1, v25, v26);
    if ( (HIWORD(v31[0]) & 0xF000) == 0x4000 )
      v28 = (_QWORD *)&byte_20;
    else
      v28 = v44;
    j_XeCryptHmacSha(v41, 16, a1 + 16, 16, v28, 16, v10, v20);
    j_XeCryptRc4Key(v38, v41, 16);
    j_XeCryptRc4Ecb(v38, a1 + 32, v27 - 32);
    j_XeCryptSha(a1, 16, a1 + 320, v27 - 320, v43, 20);
    if ( (unsigned int)sub_B480(v46, v43, 20) != 0 )
      v_MACHINE_CHECK_0();
  }
  if ( *(_BYTE *)(v19 + 91669) != 0 || *(unsigned __int16 *)(a1 + 2) >= 0x74Cu )
  {
    if ( *(unsigned __int8 *)(a1 + 944) != *(unsigned __int8 *)(v19 + 91669) )
      v_MACHINE_CHECK_0();
    v29 = *(unsigned __int8 *)(a1 + 945);
    if ( v29 > 0x10 )
      v_MACHINE_CHECK_0();
    if ( *(_BYTE *)(a1 + 945) != 0 && ((1 << (v29 - 1)) & *(unsigned __int16 *)(a1 + 946)) == 0 )
      v_MACHINE_CHECK_0();
    dword_74 = *(_DWORD *)(a1 + 944);
  }
  return v_MACHINE_CHECK_filter(v47);
}


/* ========== [0x7310] sub_7310 (168 bytes) ========== */
_DWORD *sub_7310()
{
  __int64 v0; // r2
  _DWORD *result; // r3

  if ( *(_BYTE *)(*(_QWORD *)(v0 + 91072) + 24LL) != 0 )
    return sub_3830(520, 0);
  return result;
}


/* ========== [0x73b8] nullsub_2 (4 bytes) ========== */
void nullsub_2()
{
  ;
}


/* ========== [0x73c0] HvpKeysFindZeroedSlot (212 bytes) ========== */
__int64 __fastcall HvpKeysFindZeroedSlot(int a1)
{
  __int64 v1; // r2
  __int64 v2; // r31
  char v3; // r9
  int v4; // r11
  _BYTE *v5; // r10
  int v6; // r4
  int v7; // r3

  v2 = 0;
  if ( a1 == 0 )
    goto LABEL_9;
  do
  {
    v3 = 0;
    if ( *(int *)(4LL * (unsigned int)v2 + v1 + 69312) >= 0 )
    {
      v4 = (unsigned __int16)*(_DWORD *)(4LL * (unsigned int)v2 + v1 + 69312);
      v5 = (_BYTE *)(*(unsigned __int16 *)(2LL * (unsigned int)v2 + v1 + 69192) + *(_QWORD *)(v1 + 91072) + 24LL);
      while ( v4 != 0 )
      {
        --v4;
        v3 |= *v5++;
      }
      if ( v3 == 0 )
        break;
    }
    ++v2;
  }
  while ( (unsigned int)v2 < 0x39 );
  if ( (unsigned int)v2 >= 0x39 )
  {
    v6 = 0;
    v7 = 128;
  }
  else
  {
LABEL_9:
    v6 = 128;
    v7 = 0;
  }
  sub_3830(v7, v6);
  sub_7310();
  return v2;
}


/* ========== [0x7498] sub_7498 (100 bytes) ========== */
__int64 __fastcall sub_7498(__int64 a1)
{
  __int64 v1; // lr
  __int64 v3; // r2
  __int64 v4; // r31
  _WORD v6[10]; // [sp+50h] [-30h] BYREF
  int v7; // [sp+64h] [-1Ch]
  __int64 v8; // [sp+78h] [-8h]

  v8 = v1;
  HvpComputeKeyVaultHmacSha(a1, (__int64)v6, 20);
  v4 = sub_38D8((__int64)v6, a1 + 7672, (int *)(v3 + 70424));
  v_MACHINE_CHECK_filter(v7);
  return v4;
}


/* ========== [0x7500] sub_7500 (180 bytes) ========== */
_DWORD *__fastcall sub_7500(__int64 a1, __int64 a2)
{
  __int64 SocMMIORegs; // r3
  __int64 v5; // r2
  __int64 v6; // r4
  _DWORD *result; // r3

  sub_3830(0, 0x100000);
  SocMMIORegs = HvpGetSocMMIORegs(0);
  sub_B4D8(SocMMIORegs, 23552, 0, 0, 0, 0, a1, a2);
  if ( *(_QWORD *)(v5 + 92832) >= 0x710700u )
    v6 = v5 + 72624;
  else
    v6 = v5 + 72644;
  result = (_DWORD *)sub_B480(a1, v6, a2);
  if ( (_DWORD)result == 0 )
    return sub_3830(0x100000, 0);
  return result;
}


/* ========== [0x75b8] sub_75B8 (220 bytes) ========== */
// write access to const memory has been detected, the output may be wrong!
__int64 __fastcall sub_75B8(__int64 a1, unsigned int a2, __int64 a3)
{
  __int64 v5; // r2

  if ( a2 != ((a2 + 15LL) & 0x1FFFFFFF0LL) )
    return 0;
  if ( (unsigned __int64)a2 - 128 > 0x7FFFF7F )
    return 0;
  if ( 0x8000000 - (unsigned __int64)a2 <= 0x4000 )
    return 0;
  CopyBy64(a3, (unsigned int *)(a2 + a1), 2048);
  if ( !sub_3760(a3) )
    return 0;
  if ( *(_BYTE *)(v5 + 91669) == 0 && (*(_BYTE *)(a3 + 26) & 4) != 0 )
    word_6 = -10178;
  return 1;
}


/* ========== [0x7698] HvxKeysInitialize (732 bytes) ========== */
__int64 __fastcall HvxKeysInitialize(unsigned int a1)
{
  __int64 v1; // r2
  _DWORD *v3; // r29
  __int64 *v4; // r31
  __int64 v5; // r3

  v3 = (_DWORD *)(v1 + 92824);
  HvpAquireSpinLock((_DWORD *)(v1 + 92824));
  v4 = (__int64 *)HvpRelocatePhysicalToProtected(a1, 0x10000u, 58);
  sub_6E68();
  HvpZeroCacheLines((__int64)v4, 512);
  HvpRelocatePhysicalToEncryptedStrict(v4, 0x10000u, 58);
  LODWORD(v5) = 0;
  sub_B130(v5);
  HvpReleaseSpinLock(v3);
  return 0;
}


/* ========== [0x7978] HvxKeysSetFactoryResponse (280 bytes) ========== */
__int64 __fastcall HvxKeysSetFactoryResponse(
        __int64 a1,
        int a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7)
{
  __int64 v7; // r2
  __int64 v8; // lr
  __int64 v10; // r3
  int v11; // r2
  __int64 v12; // r5
  int v13; // r31
  __int64 v14; // r3
  __int64 v15; // r5
  __int64 v16; // r3
  __int64 v17; // r2
  __int64 v18; // r3
  __int64 v19; // r3
  _WORD v20[128]; // [sp+50h] [-120h] BYREF
  int v21; // [sp+150h] [-20h]
  __int64 v22; // [sp+168h] [-8h]

  v22 = v8;
  v21 = *(_DWORD *)(v7 + 0x10000);
  if ( a2 == 256 )
  {
    v10 = HvpPhysicalToReal(a1, 256, a3, a4, a5, a6, a7, 2);
    memcpy(v20, v10);
    LODWORD(v12) = v11 + 70152;
    v13 = v11 + 91712;
    LODWORD(v14) = v11 + 91712;
    v16 = sub_38D8(v14, v20, v12);
    if ( (_DWORD)v16 != 0
      || *(_DWORD *)(v17 + 68040) == 32
      && (LODWORD(v15) = v17 + 68040, LODWORD(v16) = v13, (unsigned int)sub_38D8(v16, v20, v15) != 0) )
    {
      LODWORD(v18) = sub_3830(4, 0);
      v19 = sub_7310(v18);
      sub_B138(v19);
      v_MACHINE_CHECK_filter(v21);
      return 0;
    }
    else
    {
      v_MACHINE_CHECK_filter(v21);
      return -939524091;
    }
  }
  else
  {
    v_MACHINE_CHECK_filter(v21);
    return -939524092;
  }
}


/* ========== [0x7a90] HvxKeysSetKey (800 bytes) ========== */
__int64 __fastcall HvxKeysSetKey(__int64 a1, __int64 a2, __int64 a3)
{
  __int64 v3; // r2
  __int64 v4; // r28

  v4 = a1;
  LODWORD(a1) = a2;
  if ( (unsigned int)v4 < 0x39 || (_DWORD)v4 == 68 )
    return -939524094;
  switch ( (_DWORD)v4 )
  {
    case 'F':
      LODWORD(a2) = a3;
      return sub_3C50(a1, a2);
    case 'G':
      return sub_3980(v3 + 91648, a2, a3);
    case 'M':
      LODWORD(a2) = a3;
      return sub_3A00(a1, a2);
    default:
      if ( (unsigned int)(v4 - 224) > 7 )
      {
        if ( (_DWORD)v4 == 240 )
        {
          memset(*(_QWORD *)(v3 + 91696), 0, 128);
          return 0;
        }
        else if ( (unsigned int)(v4 - 4096) > 0xFFF )
        {
          return -939524095;
        }
        else
        {
          return sub_B148(v4 - 4096, a2, a3);
        }
      }
      else
      {
        return sub_3B10(16LL * (unsigned int)(v4 - 224) + *(_QWORD *)(v3 + 91696), a2, a3);
      }
  }
}


/* ========== [0x7db0] HvxKeysGenerateRandomKey (236 bytes) ========== */
__int64 __fastcall HvxKeysGenerateRandomKey(unsigned int a1)
{
  __int64 v1; // r2
  int v3; // r11
  uint32_t v4; // r4
  int v5; // r11

  if ( a1 >= 0x39 )
    return -939524095;
  v3 = *(_DWORD *)(4LL * a1 + v1 + 69312);
  v4 = (unsigned __int16)v3;
  if ( (v3 & 0x10000) == 0 || (v3 & 0x100000) == 0 )
    return -939524094;
  v5 = (v3 & 0x20000000) != 0 ? 0 : v3 & 0x10000000;
  if ( v5 == 0 )
    return -939524094;
  XeCryptRandom((uint8_t *)(*(unsigned __int16 *)(2LL * a1 + v1 + 69192) + *(_QWORD *)(v1 + 91072) + 24LL), v4);
  HvpKeysFindZeroedSlot(1);
  return 0;
}


/* ========== [0x7ea0] HvxKeysGetKey (1256 bytes) ========== */
__int64 __fastcall HvxKeysGetKey(
        unsigned int a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7)
{
  __int64 v7; // r2
  __int64 v9; // r10
  __int64 v10; // r27
  int v12; // r31
  unsigned __int16 *v13; // r30
  int v14; // r31
  __int64 v15; // r31
  int v16; // r30
  int v17; // r11
  __int64 v18; // r2
  unsigned int v20; // r28
  int v21; // r11
  unsigned int *v22; // r25
  unsigned int v23; // r26
  __int64 v24; // r10
  __int64 v25; // r9
  __int64 v26; // r8
  __int64 v27; // r7
  __int64 v28; // r6
  __int64 v29; // r5
  __int64 v30; // r24
  _DWORD *v31; // r2
  _DWORD *v32; // r27
  int v33; // r11
  unsigned int v34; // r29
  __int64 v35; // r6
  __int64 v36; // r4
  __int64 v37; // r5
  __int64 v38; // r7
  __int64 v39; // r6
  __int64 v40; // r4
  __int64 v41; // r3
  __int64 v42; // r4
  __int64 v43; // r5
  __int64 v44; // r5
  int v45; // r3
  unsigned int v46; // r11
  _WORD v47[16]; // [sp+50h] [-2A0h] BYREF
  _WORD v48[8]; // [sp+70h] [-280h] BYREF
  uint8_t v49[32]; // [sp+80h] [-270h] BYREF
  _WORD v50[256]; // [sp+A0h] [-250h] BYREF
  int v51; // [sp+2A0h] [-50h]

  v9 = 2;
  v10 = a2;
  v51 = *(_DWORD *)(v7 + 0x10000);
  if ( a1 < 0x39 )
  {
    a6 = 2LL * a1;
    a7 = v7 + 69312;
    v12 = *(_DWORD *)(4LL * a1 + v7 + 69312);
    v9 = *(unsigned __int16 *)(a6 + v7 + 69192);
    v13 = (unsigned __int16 *)(v9 + *(_QWORD *)(v7 + 91072) + 24);
    goto LABEL_39;
  }
  switch ( a1 )
  {
    case '9':
      v13 = (unsigned __int16 *)(v7 + 67768);
LABEL_17:
      v12 = 16843024;
      goto LABEL_39;
    case '<':
      v13 = (unsigned __int16 *)(v7 + 70424);
      goto LABEL_17;
    case '=':
      v13 = (unsigned __int16 *)(v7 + 69880);
      goto LABEL_17;
    case ':':
      v12 = 16843024;
      v13 = (unsigned __int16 *)(v7 + 68312);
      goto LABEL_39;
    case ';':
      v12 = 16843024;
      v13 = (unsigned __int16 *)(v7 + 68584);
      goto LABEL_39;
    case '>':
      v13 = (unsigned __int16 *)(v7 + 71512);
      goto LABEL_17;
    case '?':
      v13 = (unsigned __int16 *)(v7 + 70696);
      goto LABEL_17;
    case '@':
      v13 = (unsigned __int16 *)(v7 + 69672);
      v12 = 16842960;
      goto LABEL_39;
    case 'A':
      v12 = 16843024;
      v13 = (unsigned __int16 *)(v7 + 68888);
      goto LABEL_39;
    case 'B':
      sub_B0E0(v48);
      v13 = v48;
      v12 = 16842768;
      goto LABEL_39;
    case 'D':
      HvpComputeKeyVaultHmacSha(*(_QWORD *)(v7 + 91072), (__int64)v47, 20);
      v14 = 33619968;
      v13 = v47;
LABEL_38:
      v12 = v14 | 0x14;
      goto LABEL_39;
    case 'E':
      LODWORD(a2) = 20;
      sub_7500(v47, a2);
      v13 = v47;
LABEL_37:
      v14 = 16842752;
      goto LABEL_38;
    case 'H':
      v15 = v7 + 91632;
      v16 = sub_B450(*(_QWORD *)(v7 + 91632));
      v17 = sub_B450(*(_QWORD *)(v15 + 8)) + v16;
      if ( v17 != 64 && v17 != 128 )
      {
        v_MACHINE_CHECK_filter(v51);
        return -939524082;
      }
      v13 = (unsigned __int16 *)(v18 + 91056);
      v12 = 18087952;
LABEL_39:
      v20 = (unsigned __int16)v12;
      if ( (v12 & 0x2000000) != 0 )
        v21 = 0;
      else
        v21 = v12 & 0x1000000;
      if ( v21 == 0 )
        goto LABEL_42;
      v22 = (unsigned int *)HvpPhysicalToReal(a3, 4, a3, a4, a5, a6, a7, v9);
      v23 = *v22;
      v30 = HvpPhysicalToReal(v10, *v22, v29, v28, v27, v26, v25, v24);
      if ( (v12 & 0x40000) == 0 )
      {
        if ( (v12 & 0x10000) != 0 )
        {
          if ( v23 != (unsigned __int16)v12 )
          {
            v_MACHINE_CHECK_filter(v51);
            return -939524092;
          }
          goto LABEL_67;
        }
        if ( (unsigned __int16)v12 < 2u )
        {
          v20 = 0;
          goto LABEL_67;
        }
        v46 = *v13;
        v20 = (unsigned __int16)v12 - 2;
        ++v13;
        if ( v20 > v46 )
          v20 = v46;
LABEL_70:
        if ( v23 < v20 )
        {
          v_MACHINE_CHECK_filter(v51);
          return -939524093;
        }
LABEL_67:
        memcpy(v30, v13);
        v45 = v51;
        *v22 = v20;
        v_MACHINE_CHECK_filter(v45);
        return 0;
      }
      if ( a1 == 28 )
      {
        v32 = v31 + 17810;
      }
      else
      {
        if ( a1 == 72 )
        {
          v32 = v31 + 17946;
          v33 = 1;
          goto LABEL_51;
        }
        v32 = v31 + 17538;
      }
      v33 = 0;
LABEL_51:
      v34 = 8 * *v32;
      if ( v34 - 64 <= 0x1C0 )
      {
        if ( v33 != 0 )
        {
          XeCryptRandom(v49, 0x14u);
          LODWORD(v38) = 0;
          LODWORD(v39) = 512;
          LODWORD(v40) = (unsigned __int16)v12;
          LODWORD(v41) = (_DWORD)v13;
          if ( (unsigned int)j_HvSetupMemEncryptionKey(v41, v40, v50, v39, v38, v49) != 0 )
          {
            LODWORD(v42) = 512;
            sub_B498(v50, v42);
            LODWORD(v37) = 64;
LABEL_60:
            j_XeCryptBnQw_SwapDwQwLeBe(v50, v50, v37);
            LODWORD(v43) = (_DWORD)v32;
            if ( (unsigned int)j_XeCryptBnQwNeRsaPubCrypt(v50, v50, v43) != 0 )
            {
              LODWORD(v44) = v34 >> 3;
              j_XeCryptBnQw_SwapDwQwLeBe(v50, v50, v44);
              v13 = v50;
              v20 = v34;
              goto LABEL_70;
            }
          }
          v_MACHINE_CHECK_filter(v51);
          return -939524090;
        }
        if ( (v12 & 0x100000) != 0 && (v12 & 0x10000) != 0 && (unsigned __int16)v12 <= 0x14u )
        {
          memcpy(v47, v13);
          LODWORD(v35) = v34;
          LODWORD(v36) = 0;
          sub_B3A8(v47, v36, v50, v35);
          LODWORD(v37) = v34 >> 3;
          goto LABEL_60;
        }
      }
LABEL_42:
      v_MACHINE_CHECK_filter(v51);
      return -939524094;
    case 'I':
      v13 = (unsigned __int16 *)(v7 + 91732);
      v12 = 16842756;
      goto LABEL_39;
    case 'M':
      v13 = (unsigned __int16 *)(v7 + 91712);
      goto LABEL_37;
    default:
      break;
  }
  v_MACHINE_CHECK_filter(v51);
  return -939524095;
}


/* ========== [0x8388] sub_8388 (240 bytes) ========== */
__int64 __fastcall sub_8388(_QWORD *a1)
{
  __int64 v1; // lr
  __int64 SocMMIORegs; // r3
  __int64 v4; // r10
  __int64 v5; // r11
  __int64 v6; // r9
  __int64 v7; // r8
  __int64 v8; // r9
  __int64 v9; // r8
  __int64 v10; // r11
  char v11; // r31
  __int64 result; // r3
  __int64 v13; // r2
  unsigned int v14; // r11
  int v15; // r11
  __int64 v16; // [sp+50h] [-30h] BYREF
  __int64 v17; // [sp+58h] [-28h]
  __int64 v18; // [sp+60h] [-20h]
  __int64 v19; // [sp+68h] [-18h]
  __int64 v20; // [sp+78h] [-8h]

  v20 = v1;
  SocMMIORegs = HvpGetSocMMIORegs(0x20000);
  v4 = 0;
  v5 = 512;
  do
  {
    v6 = 8LL * (unsigned int)v5;
    v7 = 8LL * (int)v4;
    v5 += 64;
    ++v4;
    *(__int64 *)((char *)&v16 + v7) = *(_QWORD *)(v6 + SocMMIORegs);
  }
  while ( (unsigned int)v5 < 0x300 );
  if ( a1 != nullptr )
  {
    v8 = v17;
    v9 = v18;
    v10 = v19;
    *a1 = v16;
    a1[1] = v8;
    a1[2] = v9;
    a1[3] = v10;
  }
  v11 = sub_B450(v18 | v16);
  result = sub_B450(v19 | v17);
  v14 = (unsigned __int8)(result + v11);
  *(_BYTE *)(v13 + 91668) = v14;
  if ( v14 < 0x40 )
    v15 = ((_cntlzw(v14) & 0x20) == 0) + 1;
  else
    v15 = 0;
  *(_DWORD *)(v13 + 91732) = v15;
  return result;
}


/* ========== [0x8478] sub_8478 (44 bytes) ========== */
void sub_8478()
{
  _BYTE *v0; // r2

  v0[91026] = 0x80;
  v0[91025] = -1;
  v0[91027] = -1;
  v0[91024] = -1;
}


/* ========== [0x84a8] sub_84A8 (56 bytes) ========== */
unsigned __int8 *__fastcall sub_84A8(unsigned __int8 *result, __int64 a2)
{
  __int64 i; // r11
  __int64 v3; // r10

  for ( i = 0; (_DWORD)a2 != 0; i += v3 )
  {
    v3 = *result;
    --a2;
    result += 128;
  }
  __isync();
  return result;
}


/* ========== [0x84e0] sub_84E0 (712 bytes) ========== */
_QWORD *__fastcall sub_84E0(unsigned int a1)
{
  unsigned __int64 v1; // r30
  __int64 SocMMIORegs; // r29
  _QWORD *result; // r3
  _QWORD *v4; // r2
  _QWORD *v5; // r28
  __int64 v6; // r10
  _BYTE *v7; // r11
  __int64 v8; // r31
  unsigned int i; // r10
  unsigned __int64 v10; // r11
  __int64 v11; // r11
  __int64 v12; // r10
  __int64 v13; // r9
  __int64 v14; // r11
  unsigned __int8 *v15; // r10
  __int64 v16; // r9
  __int64 v17; // r9
  unsigned __int64 v18; // r11

  v1 = __mfspr(0x131u);
  if ( *(_BYTE *)(v1 + 129) != 0 )
    v_MACHINE_CHECK_0();
  if ( a1 > 2 )
    v_MACHINE_CHECK_0();
  *(_BYTE *)(v1 + 130) = a1;
  __sync();
  *(_BYTE *)(v1 + 129) = 1;
  __sync();
  SocMMIORegs = HvpGetSocMMIORegs(397312);
  result = (_QWORD *)HvpGetSocMMIORegs(327680);
  v5 = &result[512 * (unsigned __int64)*(unsigned __int8 *)(v1 + 128)];
  if ( *(_BYTE *)(v1 + 128) != 0 )
  {
    while ( *(_BYTE *)(v1 + 129) != 2 )
      ;
  }
  else
  {
    v6 = 5;
    v7 = (_BYTE *)(v1 + 41089);
    do
    {
      while ( *v7 != 1 )
        ;
      --v6;
      v7 -= 0x2000;
    }
    while ( (_DWORD)v6 != 0 );
    *(_QWORD *)(SocMMIORegs + 416) &= ~0x100uLL;
    __eieio();
    __isync();
    v8 = 0;
    v4[11372] = result[3072];
    v4[11373] = result[3074];
    v4[11374] = result[3076];
    v4[11375] = result[3078];
    v4[11376] = result[3080];
    result[3072] = 0;
    __eieio();
    result[3074] = 0;
    __eieio();
    result[3076] = 0;
    __eieio();
    result[3078] = 0;
    __eieio();
    result[3080] = 0;
    __eieio();
    __isync();
    v4[11377] = *(_QWORD *)(SocMMIORegs + 456);
    *(_QWORD *)(SocMMIORegs + 456) = 0;
    __eieio();
    __isync();
    for ( i = 5; i != 0; --i )
    {
      v10 = ((unsigned __int64)i << 13) + v1;
      if ( *(unsigned __int8 *)(v10 + 130) != *(unsigned __int8 *)(v1 + 130) )
        v_MACHINE_CHECK_0();
      *(_BYTE *)(v10 + 129) = 2;
      __sync();
      while ( *(_BYTE *)(v10 + 129) != 3 )
        ;
    }
    result = (_QWORD *)sub_4CC();
    v11 = 512;
    v12 = 0;
    do
    {
      v13 = *(unsigned __int8 *)result;
      --v11;
      result += 16;
      v12 += v13;
    }
    while ( (_DWORD)v11 != 0 );
    __isync();
    v14 = 384;
    v15 = (unsigned __int8 *)v1;
    do
    {
      v16 = *v15;
      --v14;
      v15 += 128;
      v8 += v16;
    }
    while ( (_DWORD)v14 != 0 );
    __isync();
  }
  v5[1] = 124;
  __eieio();
  __isync();
  *(_DWORD *)(v1 + 136) = __mfdec();
  __mtdec(0x7FFFFFFFu);
  v17 = *(unsigned __int8 *)(v1 + 128);
  if ( (v17 & 1) == 0 )
  {
    while ( (__mfspr(0x88u) & 0x400000) != 0 )
      ;
    *(_DWORD *)(v1 + 140) = __mfspr(0x136u);
    __mtspr(0x136u, 0x7FFFFFFFu);
  }
  v18 = __mfspr(0x399u);
  __sync();
  __mtspr(0x399u, (~v17 << 20) & 0x100000 | v18 & 0xFFFFFFFFFF8FFFFFLL);
  __sync();
  __isync();
  *(_BYTE *)(v1 + 129) = 3;
  __sync();
  return result;
}


/* ========== [0x87a8] sub_87A8 (568 bytes) ========== */
_QWORD *sub_87A8()
{
  unsigned __int64 v0; // r31
  _BYTE *v1; // r29
  __int64 SocMMIORegs; // r30
  _QWORD *result; // r3
  _QWORD *v4; // r2
  __int64 v5; // r10
  _QWORD *v6; // r9
  _BYTE *v7; // r10
  __int64 v8; // r11

  v0 = __mfspr(0x131u);
  v1 = (_BYTE *)(v0 + 129);
  if ( *(_BYTE *)(v0 + 129) != 3 )
    v_MACHINE_CHECK_0();
  *v1 = 4;
  __sync();
  SocMMIORegs = HvpGetSocMMIORegs(397312);
  result = (_QWORD *)HvpGetSocMMIORegs(327680);
  v5 = *(unsigned __int8 *)(v0 + 128);
  v6 = &result[512 * v5];
  if ( (v5 & 1) == 0 )
  {
    if ( *(_BYTE *)(v0 + 128) == 0 )
    {
      result[3072] = v4[11372];
      __eieio();
      result[3074] = v4[11373];
      __eieio();
      result[3076] = v4[11374];
      __eieio();
      result[3078] = v4[11375];
      __eieio();
      result[3080] = v4[11376];
      __eieio();
      __isync();
      *(_QWORD *)(SocMMIORegs + 456) = v4[11377];
      __eieio();
      __isync();
    }
    __mtspr(0x136u, *(unsigned int *)(v0 + 140));
    __mtspr(0x98u, __mfspr(0x88u) | 0xC00000);
    while ( *(_BYTE *)(v0 + 8321) != 5 )
      ;
  }
  __mtdec(*(_DWORD *)(v0 + 136));
  v6[1] = 0;
  __eieio();
  __isync();
  if ( *(_BYTE *)(v0 + 128) != 0 )
  {
    *v1 = 5;
    __sync();
    while ( *v1 != 0 )
      ;
  }
  else
  {
    result[1025] = 0;
    __eieio();
    __isync();
    v6[2] = 262264;
    __eieio();
    while ( *(_BYTE *)(v0 + 16513) != 5 )
      ;
    result[2049] = 0;
    __eieio();
    __isync();
    v6[2] = 1048696;
    __eieio();
    while ( *(_BYTE *)(v0 + 32897) != 5 )
      ;
    *(_QWORD *)(SocMMIORegs + 416) |= 0x100uLL;
    __eieio();
    __isync();
    v7 = (_BYTE *)(v0 + 129);
    v8 = 6;
    do
    {
      --v8;
      *v7 = 0;
      v7 += 0x2000;
    }
    while ( (_DWORD)v8 != 0 );
    __sync();
  }
  return result;
}


/* ========== [0x89e0] sub_89E0 (124 bytes) ========== */
__int64 __fastcall sub_89E0(unsigned __int64 a1, __int64 a2)
{
  __int64 v2; // r2
  __int64 v3; // r11

  if ( *(_QWORD *)(v2 + 92832) >= 0x710700u )
  {
    v3 = a1 + a2;
    if ( (__int64)(a1 + a2) <= 63 )
    {
      if ( v3 < 1 )
        return 1;
    }
    else
    {
      return 63;
    }
  }
  else
  {
    if ( a1 < 0x15 )
      a1 += 62LL;
    v3 = a1 + a2;
    if ( (__int64)(a1 + a2) > 82 )
    {
      v3 = 82;
LABEL_9:
      v3 -= 62;
      return v3;
    }
    if ( v3 < 21 )
      v3 = 21;
    if ( v3 >= 62 )
      goto LABEL_9;
  }
  return v3;
}


/* ========== [0x8a60] sub_8A60 (224 bytes) ========== */
unsigned __int64 __fastcall sub_8A60(unsigned __int64 result)
{
  __int64 v1; // r2
  unsigned __int64 v2; // r10
  unsigned __int64 v3; // r11

  v2 = *(_QWORD *)(v1 + 92832);
  if ( v2 >= 0x710500 )
  {
    if ( v2 >= 0x710700 )
    {
      if ( v2 >= 0x710900 )
      {
        if ( result < 0x20 )
          return 32;
      }
      else if ( result < 0x19 )
      {
        return 25;
      }
    }
    else
    {
      v3 = result;
      if ( result < 0x15 )
        v3 = result + 62;
      if ( v3 < 0x25 )
        return 37;
    }
  }
  return result;
}


/* ========== [0x8b40] HvxQuiesceProcessor (40 bytes) ========== */
void __fastcall __noreturn HvxQuiesceProcessor(__int64 a1)
{
  __int64 v1; // r3

  if ( *(_BYTE *)(__mfspr(0x131u) + 128) == 0 )
    v_MACHINE_CHECK_0(a1);
  v1 = sub_84E0(a1);
  HvpSleepThread(v1);
}


/* ========== [0x8b80] sub_8B80 (192 bytes) ========== */
__int64 __fastcall sub_8B80(__int64 result, __int64 a2, __int64 a3)
{
  __int64 v3; // r11

  v3 = *(_QWORD *)(result + 392);
  *(_QWORD *)(a2 + 24608) = 1024;
  __eieio();
  __isync();
  while ( (*(_QWORD *)(a2 + 24608) & 0x2000LL) != 0 )
    ;
  *(_QWORD *)(result + 392) = (a3 << 8) & 0x3F00 | 0xFF004000LL | v3 & 0xFFFFFFFFFFFF40F7LL;
  __eieio();
  while ( (*(_QWORD *)(a2 + 24608) & 0x2000LL) == 0 )
    ;
  *(_QWORD *)(result + 392) |= 0x8000uLL;
  __eieio();
  __isync();
  *(_QWORD *)(a2 + 24608) = 0;
  __eieio();
  __isync();
  return result;
}


/* ========== [0x8c40] sub_8C40 (80 bytes) ========== */
__int64 __fastcall sub_8C40(__int64 a1)
{
  __int64 v1; // r2
  unsigned __int64 v2; // r10
  int v3; // r11

  v2 = *(_QWORD *)(v1 + 92832);
  if ( v2 < 0x710500 )
    return 0;
  v3 = _cntlzd(*(_QWORD *)(a1 + 48) & 0x7FFFFFFFFFFFFFFFLL);
  if ( v2 >= 0x710900 )
    return (unsigned int)(13 - v3);
  else
    return (unsigned int)(9 - v3);
}


/* ========== [0x8c90] sub_8C90 (248 bytes) ========== */
__int64 __fastcall sub_8C90(__int64 a1, int a2)
{
  __int64 v2; // r2
  unsigned __int64 v3; // r10
  __int64 v4; // r11
  unsigned __int64 v5; // r11
  unsigned __int64 v6; // r3

  v3 = *(_QWORD *)(v2 + 92832);
  if ( v3 < 0x710500 )
    return 0;
  if ( v3 >= 0x710900 )
    return sub_8C40(a1);
  if ( a2 != 0 )
  {
    if ( a2 == 1 )
      v4 = *(unsigned __int8 *)(v2 + 91027);
    else
      v4 = *(unsigned __int8 *)(v2 + 91025);
  }
  else
  {
    v4 = *(unsigned __int8 *)(v2 + 91024);
  }
  if ( v4 == 255 )
  {
    v5 = *(_QWORD *)(a1 + 392);
    if ( v3 >= 0x710700 )
    {
      return *(unsigned __int8 *)((HIWORD(v5) & 0x3F) + v2 + 72784);
    }
    else
    {
      if ( a2 == 2 )
      {
        v6 = 56;
      }
      else
      {
        v6 = HIWORD(v5) & 0x3F;
        if ( a2 == 1 )
          v6 = sub_89E0(v6, (v5 >> 39) & 0xE);
      }
      return *(unsigned __int8 *)(v6 + v2 + 72720);
    }
  }
  return v4;
}


/* ========== [0x8d88] HvxEnableTimebase (116 bytes) ========== */
__int64 __fastcall HvxEnableTimebase(unsigned int a1)
{
  __int64 SocMMIORegs; // r3

  SocMMIORegs = HvpGetSocMMIORegs(397312);
  *(_QWORD *)(SocMMIORegs + 416) = ((unsigned __int64)((_cntlzw(a1) & 0x20) != 0) << 8) ^ 0x100
                                 | *(_QWORD *)(SocMMIORegs + 416) & 0xFFFFFFFFFFFFFEFFLL;
  __eieio();
  __isync();
  return 0;
}


/* ========== [0x8e00] sub_8E00 (1392 bytes) ========== */
__int64 __fastcall sub_8E00(unsigned __int64 a1, __int64 a2, unsigned __int64 a3)
{
  __int64 SocMMIORegs; // r16
  _QWORD *v7; // r30
  __int64 v8; // r24
  _QWORD *v9; // r18
  _QWORD *v10; // r19
  __int64 HostBridgeRegs; // r29
  __int64 v12; // r17
  unsigned __int64 v13; // r3
  __int64 v14; // r2
  __int64 v15; // r25
  unsigned __int64 v16; // r31
  unsigned __int64 v17; // r28
  __int64 result; // r3
  unsigned __int64 v19; // r20
  unsigned __int64 v20; // r11
  unsigned __int64 v21; // r10
  unsigned __int64 v22; // r10
  unsigned __int64 v23; // r11

  SocMMIORegs = HvpGetSocMMIORegs(393216);
  v7 = (_QWORD *)HvpGetSocMMIORegs(397312);
  v8 = HvpGetSocMMIORegs(327680);
  v9 = (_QWORD *)HvpGetSocMMIORegs(196608);
  v10 = (_QWORD *)HvpGetSocMMIORegs(294912);
  HostBridgeRegs = HvpGetHostBridgeRegs(16908288);
  v12 = HvpGetHostBridgeRegs(0x1000000);
  v13 = sub_8A60(a1);
  v15 = v14 + 0x10000;
  v16 = v7[49];
  v17 = v13;
  if ( *(_QWORD *)(v14 + 92832) >= 0x710700u && (v16 & 0x80) == 0 )
  {
    v16 |= 0x80u;
    v7[49] = v16;
    __eieio();
    __isync();
  }
  result = sub_8C40((__int64)v7);
  v19 = result;
  if ( a2 != (v16 & 7) || v17 != ((v16 >> 8) & 0x3F) || a3 != result )
  {
    *(_QWORD *)(SocMMIORegs + 2904) |= 0x8000000000000000LL;
    __eieio();
    __isync();
    v7[10] &= ~0x200000000uLL;
    __eieio();
    __isync();
    v7[12] |= 0x200000000uLL;
    __eieio();
    __isync();
    *(_DWORD *)(HostBridgeRegs + 4) = __builtin_bswap32(__builtin_bswap32(*(_DWORD *)(HostBridgeRegs + 4)) | 1);
    __eieio();
    __isync();
    HvxEnableTimebase(1u);
    Sleep(0x2710u);
    result = HvxEnableTimebase(0);
    if ( a2 != 1 )
    {
      v9[4] = 0x2000000000000000LL;
      __eieio();
      __isync();
      if ( *(_QWORD *)(v15 + 27296) >= 0x710700u )
      {
        v9[3584] = 0xC0F04C000000000LL;
        __eieio();
        __isync();
        *(_DWORD *)(v12 + 28672) = 201997;
      }
      else
      {
        *v10 = *v10 & 0xF0FFFFFFFFFFFFFFLL | 0x100000000000000LL;
      }
      __eieio();
      __isync();
    }
    if ( a3 > v19 && (unsigned __int64)(*(_QWORD *)(v15 + 27296) - 7406848LL) <= 0x3FF )
    {
      v7[6] = a3 << 60;
      __eieio();
    }
    v20 = (v16 >> 8) & 0x3F;
    v21 = v17;
    if ( *(_QWORD *)(v15 + 27296) >= 0x710700u )
    {
      if ( v17 >= v20 )
        goto LABEL_24;
    }
    else
    {
      if ( v17 < 0x15 )
        v21 = v17 + 62;
      if ( v20 < 0x15 )
        v20 += 62LL;
      if ( v21 >= v20 )
      {
LABEL_24:
        if ( (v16 & 7) != a2 )
        {
          *(_QWORD *)(v8 + 24608) = sub_1047C;
          __eieio();
          __isync();
          while ( (*(_QWORD *)(v8 + 24608) & 0x2000LL) != 0 )
            ;
          *(_QWORD *)(v8 + 8) = 120;
          __eieio();
          __isync();
          v7[49] = v7[49] & 0xFFFFFFFFFFFF3FF8LL | a2 & 7 | 0xFF0008;
          __eieio();
          HvpSleepThread();
        }
        v22 = v17;
        v23 = (v16 >> 8) & 0x3F;
        if ( *(_QWORD *)(v15 + 27296) >= 0x710700u )
        {
          if ( v23 >= v17 )
            goto LABEL_37;
        }
        else
        {
          if ( v23 < 0x15 )
            v23 += 62LL;
          if ( v17 < 0x15 )
            v22 = v17 + 62;
          if ( v23 >= v22 )
            goto LABEL_37;
        }
        result = sub_8B80((__int64)v7, v8, v17);
        v16 = v7[49];
LABEL_37:
        if ( a3 < v19 && (unsigned __int64)(*(_QWORD *)(v15 + 27296) - 7406848LL) <= 0x3FF )
        {
          v7[6] = a3 << 60;
          __eieio();
        }
        if ( (v16 & 7) == 1 )
        {
          v9[2] = 0xDFFFFFFFFFFFFFFFLL;
          __eieio();
          __isync();
          if ( *(_QWORD *)(v15 + 27296) >= 0x710700u )
          {
            v9[3584] = 0x2523040000000000LL;
            __eieio();
            __isync();
            *(_DWORD *)(v12 + 28672) = 201997;
          }
          else
          {
            *v10 = *v10 & 0xF0FFFFFFFFFFFFFFLL | 0x200000000000000LL;
          }
          __eieio();
          __isync();
        }
        *(_QWORD *)(SocMMIORegs + 2904) &= ~0x8000000000000000LL;
        __eieio();
        __isync();
        *(_DWORD *)(HostBridgeRegs + 4) = __builtin_bswap32(__builtin_bswap32(*(_DWORD *)(HostBridgeRegs + 4)) & 0xFFFFFFFE);
        __eieio();
        __isync();
        return result;
      }
    }
    result = sub_8B80((__int64)v7, v8, v17);
    v16 = v7[49];
    goto LABEL_24;
  }
  return result;
}


/* ========== [0x9370] HvxSetPowerMode (484 bytes) ========== */
__int64 __fastcall HvxSetPowerMode(uint64_t a1)
{
  _BYTE *v1; // r2
  _BYTE *v2; // r28
  __int64 SocMMIORegs; // r30
  __int64 v4; // r4
  __int64 v5; // r2
  unsigned __int64 v6; // r8
  __int64 v7; // r11
  __int64 v8; // r29
  int v9; // r9
  __int64 v10; // r7
  int v11; // r11
  int v12; // r9
  __int64 v13; // r3
  __int64 v14; // r5
  int v15; // r8
  __int64 v16; // r4
  int v17; // r7
  __int64 v18; // r3
  __int64 v19; // r3
  __int64 v21; // [sp+50h] [-50h]

  if ( *(_BYTE *)(__mfspr(0x131u) + 128) != 0 )
    v_MACHINE_CHECK_0(a1);
  sub_84E0(0);
  if ( (a1 & 0x200000000LL) != 0 )
  {
    v1[91024] = BYTE3(a1) >> 4;
    v1[91027] = BYTE2(a1) & 0xF;
    v1[91025] = WORD1(a1) >> 12;
  }
  v2 = v1 + 0x10000;
  if ( (a1 & 0x400000000LL) != 0 && (unsigned __int64)BYTE4(a1) - 121 <= 0xE )
    v1[91026] = BYTE4(a1);
  SocMMIORegs = HvpGetSocMMIORegs(397312);
  v6 = *(_QWORD *)(SocMMIORegs + 392);
  v7 = HIBYTE(v6) & 0x3F;
  v8 = HIWORD(v6) & 0x3F;
  if ( v7 == 0 || v7 == 59 )
    v8 = 48;
  v9 = BYTE3(a1) & 1;
  v10 = v8;
  if ( v9 != 0 || *(_DWORD *)(v5 + 92848) > 0x58110011u )
  {
    LODWORD(v4) = (unsigned __int8)v2[25490] - 128;
    v10 = sub_89E0(v8, v4);
  }
  if ( v9 != 0 )
    goto LABEL_18;
  v11 = 2;
  if ( *(_QWORD *)(v5 + 92832) >= 0x710700u )
    v11 = 1;
  LODWORD(v4) = ((v6 >> 40) & 7) * v11;
  sub_89E0(v10, v4);
  if ( v12 != 0 )
LABEL_18:
    LODWORD(v4) = 0;
  else
    LODWORD(v4) = 1;
  v13 = sub_8C90(SocMMIORegs, v4);
  LODWORD(v14) = v13;
  LODWORD(v16) = v15;
  LODWORD(v13) = v17;
  v18 = sub_8E00(v13, v16, v14);
  HIDWORD(v19) = (unsigned __int64)sub_87A8(v18) >> 32;
  LODWORD(v19) = SocMMIORegs;
  LOBYTE(v21) = sub_8C40(v19);
  return v21;
}


/* ========== [0x9558] sub_9558 (476 bytes) ========== */
__int64 __fastcall sub_9558(int a1, unsigned int a2)
{
  __int64 SocMMIORegs; // r27
  __int64 *v5; // r31
  __int64 v6; // r24
  __int64 v7; // r2
  __int64 v8; // r26

  SocMMIORegs = HvpGetSocMMIORegs(397312);
  v5 = (__int64 *)HvpGetSocMMIORegs(0x20000);
  v6 = 3;
  v8 = v7 + 0x10000;
  while ( 1 )
  {
    if ( *(_QWORD *)(v8 + 27296) >= 0x710700u )
    {
      *(_QWORD *)(SocMMIORegs + 392) &= ~0x80uLL;
      __eieio();
      __isync();
    }
    while ( v5[1024] < 0 )
      ;
    v5[1027] |= 0x8000000000000000LL;
    __eieio();
    if ( a1 == -1 )
    {
      if ( a2 == 0 )
        a2 = 1;
      if ( a2 > 0xFFF )
        a2 = 4095;
      HvxEnableTimebase(1u);
      Sleep(1000 * a2);
      HvxEnableTimebase(0);
    }
    else
    {
      HvxEnableTimebase(1u);
      Sleep(0x3E8u);
      HvxEnableTimebase(0);
      v5[a1] = 1;
      __eieio();
    }
    while ( v5[1024] < 0 )
      ;
    v5[1027] &= ~0x8000000000000000LL;
    __eieio();
    HvxEnableTimebase(1u);
    Sleep(0x7D0u);
    HvxEnableTimebase(0);
    while ( v5[1024] < 0 )
      ;
    if ( *(_QWORD *)(v8 + 27296) >= 0x710700u )
    {
      *(_QWORD *)(SocMMIORegs + 392) |= 0x80uLL;
      __eieio();
      __isync();
    }
    if ( a1 == -1 )
      break;
    v5[1026] = 0x8000000000000000LL;
    __eieio();
    while ( v5[1024] < 0 )
      ;
    if ( ((0x8000000000000000LL >> (a1 & 0x3F)) & v5[a1]) != 0 )
      break;
    if ( (int)--v6 <= 0 )
      return 0;
  }
  return 1;
}


/* ========== [0x9738] sub_9738 (416 bytes) ========== */
__int64 sub_9738()
{
  __int64 v0; // r2
  __int64 SocMMIORegs; // r3
  __int16 *v3; // r11
  __int64 i; // ctr
  __int64 v5; // r11
  __int16 *v6; // r10
  __int64 v7; // r9
  unsigned __int64 v8; // r28
  __int64 v9; // r9
  unsigned __int8 v10; // r10
  unsigned __int64 v11; // r11
  unsigned __int8 v12; // r26
  __int64 v13; // r31
  __int64 v14; // r25
  unsigned __int64 v15; // r29
  __int64 v16; // r27
  __int64 v17; // r30
  __int16 v18; // [sp+50h] [-B0h] BYREF
  unsigned __int64 v19; // [sp+58h] [-A8h]

  if ( *(_QWORD *)(v0 + 92832) >= 0x710900u && *(_DWORD *)(v0 + 91732) != 0 )
    return 1;
  SocMMIORegs = HvpGetSocMMIORegs(0x20000);
  v3 = &v18;
  for ( i = 12; i != 0; --i )
  {
    *(_QWORD *)v3 = 0;
    v3 += 4;
  }
  v5 = 0;
  v6 = &v18;
  do
  {
    v7 = 8LL * (unsigned int)v5;
    v5 += 64;
    *(_QWORD *)v6 = *(_QWORD *)(v7 + SocMMIORegs);
    v6 += 4;
  }
  while ( (unsigned int)v5 < 0x300 );
  v8 = v19;
  v9 = 0;
  v10 = 0;
  v11 = v19;
  do
  {
    if ( (_BYTE)v11 != 0 && ((v11 & 0xF0) != 0 && (v11 & 0xF) != 0 || ((v10 & 1) == 0) != ((v11 & 0xF) != 0)) )
      return 2;
    ++v9;
    v10 >>= 1;
    v11 >>= 8;
  }
  while ( (unsigned int)v9 < 8 );
  v12 = 0;
  v13 = 127;
  v14 = 0;
LABEL_15:
  if ( (v12 & 1) != 0 )
  {
    v15 = (unsigned __int8)v8 >> 4;
    v13 -= 4;
  }
  else
  {
    v15 = v8 & 0xF;
  }
  v16 = 0;
  v17 = 0;
  while ( (v15 & 1) != 0 || (unsigned int)sub_9558(v13, 0) != 0 || (unsigned int)++v16 <= 1 )
  {
    ++v17;
    --v13;
    v15 >>= 1;
    if ( (unsigned int)v17 >= 4 )
    {
      if ( (v12 & 1) == 0 )
        v13 -= 4;
      ++v14;
      v12 >>= 1;
      v8 >>= 8;
      if ( (unsigned int)v14 >= 8 )
        return 0;
      goto LABEL_15;
    }
  }
  return 3;
}


/* ========== [0x98d8] sub_98D8 (332 bytes) ========== */
__int64 sub_98D8()
{
  __int64 v0; // r2
  __int64 SocMMIORegs; // r3
  _QWORD *v3; // r11
  __int64 i; // ctr
  __int64 v5; // r11
  _QWORD *v6; // r10
  __int64 v7; // r9
  int v8; // r10
  unsigned __int64 v9; // r11
  _WORD v10[8]; // [sp+50h] [-90h] BYREF
  unsigned __int64 v11; // [sp+60h] [-80h]

  if ( *(_QWORD *)(v0 + 92832) >= 0x710900u && *(_DWORD *)(v0 + 91732) != 0 )
    return 4;
  SocMMIORegs = HvpGetSocMMIORegs(0x20000);
  v3 = v10;
  for ( i = 12; i != 0; --i )
    *v3++ = 0;
  v5 = 0;
  v6 = v10;
  do
  {
    v7 = 8LL * (unsigned int)v5;
    v5 += 64;
    *v6++ = *(_QWORD *)(v7 + SocMMIORegs);
  }
  while ( (unsigned int)v5 < 0x300 );
  v8 = 16;
  v9 = v11;
  do
  {
    if ( (v9 & 0xF) != 0 )
      break;
    --v8;
    v9 >>= 4;
  }
  while ( v8 != 0 );
  if ( v8 != 0 )
    return 5;
  else
    return 0;
}


/* ========== [0x9a28] sub_9A28 (668 bytes) ========== */
__int64 sub_9A28()
{
  __int64 v0; // r2
  __int64 SocMMIORegs; // r3
  _QWORD *v3; // r11
  __int64 i; // ctr
  __int64 v5; // r11
  _QWORD *v6; // r10
  __int64 v7; // r9
  __int64 v8; // r30
  __int64 v9; // r29
  int v10; // r31
  __int64 v11; // r31
  __int64 v12; // r31
  int v13; // r10
  unsigned int v14; // r11
  unsigned int v15; // r8
  int v16; // r10
  __int64 v17; // r28
  unsigned int v18; // r29
  unsigned int v19; // r30
  unsigned int v20; // r31
  uint8_t v21[8]; // [sp+50h] [-B0h] BYREF
  __int64 v22; // [sp+58h] [-A8h] BYREF
  __int64 v23; // [sp+60h] [-A0h] BYREF
  __int64 v24; // [sp+68h] [-98h]
  _WORD v25[12]; // [sp+70h] [-90h] BYREF
  _QWORD v26[15]; // [sp+88h] [-78h] BYREF

  if ( *(_QWORD *)(v0 + 92832) >= 0x710900u && *(_DWORD *)(v0 + 91732) != 0 )
    return 7;
  SocMMIORegs = HvpGetSocMMIORegs(0x20000);
  v3 = v25;
  for ( i = 12; i != 0; --i )
    *v3++ = 0;
  v5 = 0;
  v6 = v25;
  do
  {
    v7 = 8LL * (unsigned int)v5;
    v5 += 64;
    *v6++ = *(_QWORD *)(v7 + SocMMIORegs);
  }
  while ( (unsigned int)v5 < 0x300 );
  v8 = v26[1] | v26[0];
  v9 = v26[3] | v26[2];
  if ( (int)sub_B500(&v23) >= 0 )
  {
    v10 = sub_B450(v23);
    if ( (unsigned int)sub_B450(v24 & 0xFFFFFFFFFF030000LL) + v10 == 53 )
      return 0;
  }
  v11 = sub_B450(v8);
  v12 = sub_B450(v24 & 0xFFFFFFFFFF030000LL) + v11;
  if ( (unsigned int)v12 <= 0x35 )
  {
    if ( (unsigned int)v12 >= 0x35 )
    {
LABEL_21:
      sub_B508(&v23);
      if ( (v8 & ~v23) == 0 && (v9 & ~v24) == 0 )
      {
        v17 = 0;
        v18 = 0;
        v19 = 0;
        while ( 2 )
        {
          v20 = 0;
          v22 = *(__int64 *)((char *)&v23 + ((4LL * v18) & 0x3FFFFFFF8LL)) & ~v26[v18];
          do
          {
            if ( ((*((unsigned __int8 *)&v22 + (v20 >> 3)) >> (v20 & 7)) & 1) != 0
              && (unsigned int)sub_9558((v20 & 0x38) - (v20 & 7) + v19 + 199, 0) == 0
              && (unsigned int)++v17 > 3 )
            {
              return 9;
            }
            ++v20;
          }
          while ( v20 < 0x40 );
          v19 += 64;
          ++v18;
          if ( v19 < 0x100 )
            continue;
          break;
        }
        return 0;
      }
    }
    else
    {
      while ( 1 )
      {
        XeCryptRandom(v21, 4);
        v13 = *(_DWORD *)v21;
        v14 = 0;
        v15 = *(_DWORD *)v21 / (unsigned int)(106 - v12);
        __twllei(106 - v12, 0);
        v16 = v13 - v15 * (106 - v12);
        *(_DWORD *)v21 = v16;
        do
        {
          if ( ((*((unsigned __int8 *)&v23 + (v14 >> 3)) >> (v14 & 7)) & 1) == 0 )
          {
            if ( v16 == 0 )
              goto LABEL_19;
            *(_DWORD *)v21 = --v16;
          }
          ++v14;
        }
        while ( v14 < 0x6A );
        if ( v16 != 0 )
          return 8;
LABEL_19:
        if ( v14 == 106 )
          break;
        LODWORD(v12) = v12 + 1;
        *((_BYTE *)&v23 + (v14 >> 3)) ^= 1 << (v14 & 7);
        if ( (unsigned int)v12 >= 0x35 )
          goto LABEL_21;
      }
    }
  }
  return 8;
}


/* ========== [0x9cc8] sub_9CC8 (228 bytes) ========== */
unsigned __int64 sub_9CC8()
{
  __int64 v0; // r2
  __int64 SocMMIORegs; // r3
  _QWORD *v3; // r11
  __int64 i; // ctr
  __int64 v5; // r11
  _QWORD *v6; // r10
  __int64 v7; // r9
  unsigned __int64 v8; // r29
  unsigned __int64 v9; // r28
  int v10; // r31
  __int64 v11; // r30
  _QWORD v12[18]; // [sp+50h] [-90h] BYREF

  if ( *(_QWORD *)(v0 + 92832) >= 0x710900u && *(_DWORD *)(v0 + 91732) != 0 )
    return 10;
  SocMMIORegs = HvpGetSocMMIORegs(0x20000);
  v3 = v12;
  for ( i = 12; i != 0; --i )
    *v3++ = 0;
  v5 = 0;
  v6 = v12;
  do
  {
    v7 = 8LL * (unsigned int)v5;
    v5 += 64;
    *v6++ = *(_QWORD *)(v7 + SocMMIORegs);
  }
  while ( (unsigned int)v5 < 0x300 );
  v8 = v12[0];
  v9 = 0;
  v10 = 63;
  v11 = 56;
  do
  {
    if ( (v8 & 1) == 0 && (unsigned int)sub_9558(v10, 0) == 0 )
      ++v9;
    --v11;
    --v10;
    v8 >>= 1;
  }
  while ( (_DWORD)v11 != 0 );
  return v9 <= 0xC ? 0 : 0xCuLL;
}


/* ========== [0x9db0] sub_9DB0 (196 bytes) ========== */
// write access to const memory has been detected, the output may be wrong!
__int64 sub_9DB0()
{
  __int64 v0; // r2
  unsigned int v2; // r11
  int v3; // r31
  __int128 v4; // r30
  __int64 v5; // r2

  if ( *(_QWORD *)(v0 + 92832) >= 0x710900u && *(_DWORD *)(v0 + 91732) != 0 )
    return 19;
  v2 = *(unsigned __int8 *)(v0 + 91620);
  if ( v2 > *(unsigned __int8 *)(v0 + 91680) )
  {
    v3 = 4 * v2 + 447;
    v4 = 4u;
    do
    {
      if ( (unsigned int)sub_9558(v3, 0) == 0 )
        ++DWORD1(v4);
      *((_QWORD *)&v4 + 1) = v4 - 1;
      --v3;
    }
    while ( (_DWORD)v4 != 0 );
    if ( DWORD1(v4) == 4 )
      return 21;
    word_6 = -10210;
    *(_WORD *)(*(_QWORD *)(v5 + 90832) + 1536LL) = -10210;
  }
  return 0;
}


/* ========== [0x9e78] sub_9E78 (492 bytes) ========== */
__int64 __fastcall sub_9E78(int a1)
{
  __int64 v1; // r2
  __int64 result; // r3
  __int64 SocMMIORegs; // r24
  __int64 v5; // r2
  __int64 v6; // r25
  __int64 *v7; // r10
  __int64 v8; // r2
  _QWORD *v9; // r27
  _BYTE *v10; // r11
  __int64 i; // ctr
  unsigned int v12; // r29
  __int64 v13; // r28
  unsigned int v14; // r31
  unsigned __int64 v15; // r30
  unsigned int j; // r30
  __int64 v17; // r31
  _QWORD v18[2]; // [sp+50h] [-80h] BYREF
  __int64 v19; // [sp+60h] [-70h] BYREF
  __int64 v20; // [sp+68h] [-68h]
  __int64 v21; // [sp+70h] [-60h]
  __int64 v22; // [sp+78h] [-58h]

  if ( *(_QWORD *)(v1 + 92832) < 0x710900u )
    return 0;
  SocMMIORegs = HvpGetSocMMIORegs(0x20000);
  v19 = 0;
  v20 = 0;
  v21 = 0;
  v22 = 0;
  *(_QWORD *)(SocMMIORegs + 8232) = *(_QWORD *)(SocMMIORegs + 8232) & 0x3FFFFFFFFFFFFFFFLL | 0x4000000000000000LL;
  sub_8388(&v19);
  v6 = v5 + 0x10000;
  if ( *(unsigned __int8 *)(v5 + 91668) <= 0x40u )
  {
    if ( *(_BYTE *)(v5 + 91668) != 0 && (a1 & 0x8000000) == 0 )
    {
      result = 23;
      goto LABEL_23;
    }
    sub_3E80(a1);
    v7 = &v19;
    v9 = (_QWORD *)(v8 + 91632);
    v10 = (_BYTE *)(v8 + 91632);
    for ( i = 16; i != 0; --i )
    {
      *(_BYTE *)v7 = *v10++;
      v7 = (__int64 *)((char *)v7 + 1);
    }
    v12 = 0;
    v13 = 0;
    do
    {
      v14 = 0;
      v15 = 0;
      v18[0] = *(__int64 *)((char *)&v19 + ((8 * v13) & 8));
      do
      {
        if ( ((*((unsigned __int8 *)v18 + (v15 >> 3)) >> (v14 & 7)) & 1) != 0 )
          sub_9558((v14 & 0x38) - (v14 & 7) + v12 + 519, 0);
        ++v14;
        ++v15;
      }
      while ( v14 < 0x40 );
      v12 += 64;
      ++v13;
    }
    while ( v12 < 0x100 );
    sub_8388(&v19);
    if ( (v21 | v19) != *v9 || (v22 | v20) != v9[1] )
    {
      for ( j = 512; j < 0x300; j += 64 )
      {
        v17 = 0;
        do
          sub_9558(j + v17++, 0);
        while ( (unsigned int)v17 < 0x40 );
      }
      sub_8388(&v19);
      result = *(unsigned __int8 *)(v6 + 26132) <= 0x40uLL ? 24LL : 0x100000000LL;
      goto LABEL_23;
    }
  }
  result = 0;
LABEL_23:
  *(_QWORD *)(SocMMIORegs + 8232) &= 0x3FFFFFFFFFFFFFFFuLL;
  return result;
}


/* ========== [0xa068] sub_A068 (180 bytes) ========== */
unsigned __int64 sub_A068()
{
  __int64 SocMMIORegs; // r3
  __int16 *v1; // r11
  __int64 i; // ctr
  __int64 v3; // r11
  __int16 *v4; // r10
  __int64 v5; // r9
  unsigned __int64 v6; // r29
  __int64 v7; // r28
  int v8; // r31
  __int64 v9; // r30
  __int16 v11; // [sp+50h] [-90h] BYREF
  unsigned __int64 v12; // [sp+58h] [-88h]

  SocMMIORegs = HvpGetSocMMIORegs(0x20000);
  v1 = &v11;
  for ( i = 12; i != 0; --i )
  {
    *(_QWORD *)v1 = 0;
    v1 += 4;
  }
  v3 = 0;
  v4 = &v11;
  do
  {
    v5 = 8LL * (unsigned int)v3;
    v3 += 64;
    *(_QWORD *)v4 = *(_QWORD *)(v5 + SocMMIORegs);
    v4 += 4;
  }
  while ( (unsigned int)v3 < 0x300 );
  v6 = v12;
  v7 = 0;
  v8 = 127;
  v9 = 64;
  do
  {
    if ( (v6 & 1) == 0 && (unsigned int)sub_9558(v8, 0) == 0 )
      ++v7;
    --v9;
    --v8;
    v6 >>= 1;
  }
  while ( (_DWORD)v9 != 0 );
  return v7 == 0 ? 0 : 3uLL;
}


/* ========== [0xa120] sub_A120 (820 bytes) ========== */
// write access to const memory has been detected, the output may be wrong!
_DWORD *sub_A120()
{
  __int64 v0; // lr
  __int64 SocMMIORegs; // r3
  __int64 v2; // r2
  __int64 v3; // r31
  _QWORD *v4; // r11
  __int64 i; // ctr
  __int64 v6; // r11
  _QWORD *v7; // r10
  __int64 v8; // r9
  _DWORD *result; // r3
  __int64 v10; // r2
  int v11; // r31
  unsigned __int64 v12; // r10
  int v13; // r8
  __int64 v14; // r11
  int v15; // r9
  unsigned __int64 v16; // r10
  int j; // r11
  __int64 v18; // r7
  __int64 v19; // r30
  __int64 v20; // r9
  int v21; // r11
  int v22; // r10
  __int64 v23; // r31
  unsigned __int8 v24; // r11
  bool v25; // zf
  int v26; // r9
  unsigned __int64 v27; // r8
  int k; // r11
  int v29; // r11
  __int64 v30; // [sp+50h] [-90h] BYREF
  __int64 v31; // [sp+58h] [-88h]
  _QWORD v32[3]; // [sp+60h] [-80h] BYREF
  __int64 v33; // [sp+D8h] [-8h]

  v33 = v0;
  SocMMIORegs = HvpGetSocMMIORegs(0x20000);
  v3 = SocMMIORegs;
  v4 = v32;
  for ( i = 12; i != 0; --i )
    *v4++ = 0;
  v6 = 0;
  v7 = v32;
  do
  {
    v8 = 8LL * (int)v6;
    v6 += 64;
    *v7++ = *(_QWORD *)(v8 + SocMMIORegs);
  }
  while ( (int)v6 < 768 );
  if ( *(_QWORD *)(v2 + 92832) < 0x710900u )
  {
    *(_BYTE *)(v2 + 91668) = 0x80;
    *(_DWORD *)(v2 + 91732) = 0;
  }
  else
  {
    *(_QWORD *)(SocMMIORegs + 8232) = *(_QWORD *)(SocMMIORegs + 8232) & 0x3FFFFFFFFFFFFFFFLL | 0x4000000000000000LL;
    sub_8388(nullptr);
    *(_QWORD *)(v3 + 8232) &= 0x3FFFFFFFFFFFFFFFuLL;
  }
  result = (_DWORD *)sub_B500(&v30);
  if ( (int)result >= 0 )
  {
    v11 = sub_B450(v30);
    result = (_DWORD *)sub_B450(v31 & 0xFFFFFFFFFF030000LL);
    if ( (_DWORD)result + v11 == 53 )
    {
      byte_20 = v30;
      unk_28 = v31;
    }
  }
  v12 = v32[1];
  v13 = 0;
  v14 = 0;
  while ( (_BYTE)v12 != 0 )
  {
    v15 = ((v12 & 0xF0) != 0) << v14++;
    v13 |= v15;
    v12 >>= 8;
    if ( (int)v14 >= 8 )
      goto LABEL_16;
  }
  v13 = 255;
LABEL_16:
  if ( v13 == 2 )
    *(_WORD *)(v10 + 92820) |= 1u;
  v16 = v32[2];
  for ( j = 16; j > 0; --j )
  {
    if ( (v16 & 0xF) != 0 )
      break;
    v16 >>= 4;
  }
  v18 = v32[0];
  v19 = v10 + 0x10000;
  v20 = v32[0] & 0x300000000000000LL;
  *(_BYTE *)(v10 + 91040) = j;
  v21 = 5;
  *(_BYTE *)(v10 + 91669) = v13;
  if ( v20 != 0 )
    v21 = 1;
  v22 = v21 - 1;
  v23 = v10 + 0x10000;
  *(_BYTE *)(v10 + 91680) = 0;
  *(_BYTE *)(v10 + 91736) = 16 * v21;
  v25 = v21 - 1 < 0;
  v24 = 0;
  if ( !v25 )
  {
    v26 = v22 + 7;
    while ( 1 )
    {
      v27 = v32[v26];
      if ( v27 != 0 )
        break;
      --v22;
      --v26;
      if ( v22 < 0 )
        goto LABEL_32;
    }
    for ( k = 16; k > 0; --k )
    {
      if ( (v27 & 0xF) != 0 )
        break;
      v27 >>= 4;
    }
    v24 = 16 * v22 + k;
    *(_BYTE *)(v10 + 91680) = v24;
  }
LABEL_32:
  if ( (v18 & 0xFFFFFFFFFFFFFFLL) != 0 )
  {
    result = sub_3830(1, 0);
    v24 = *(_BYTE *)(v23 + 26144);
  }
  dword_14 = v24 | 0xC81CB400;
  v29 = *(unsigned __int8 *)(v19 + 26133);
  if ( v29 != 1 && v29 != 3 && v29 != 4 && (*(_BYTE *)(v19 + 26133) == 0 || v29 == 2) )
    v_MACHINE_CHECK_0();
  if ( (*(_WORD *)(v10 + 92828) & 4) != 0 && (v29 == 1 || v29 == 3) )
    v_MACHINE_CHECK_0();
  return result;
}


/* ========== [0xa458] sub_A458 (260 bytes) ========== */
__int64 __fastcall sub_A458(unsigned __int64 *a1)
{
  __int64 SocMMIORegs; // r3
  __int16 *v3; // r11
  __int64 i; // ctr
  __int64 v5; // r11
  __int16 *v6; // r10
  __int64 v7; // r9
  __int64 v8; // r11
  unsigned __int16 v9; // r3
  __int64 v10; // r10
  __int64 v11; // r11
  __int64 v12; // r4
  __int64 v13; // r10
  __int16 v15; // [sp+50h] [-70h] BYREF
  __int64 v16; // [sp+58h] [-68h]
  __int64 v17; // [sp+60h] [-60h]

  SocMMIORegs = HvpGetSocMMIORegs(0x20000);
  v3 = &v15;
  for ( i = 12; i != 0; --i )
  {
    *(_QWORD *)v3 = 0;
    v3 += 4;
  }
  v5 = 0;
  v6 = &v15;
  do
  {
    v7 = 8LL * (int)v5;
    v5 += 64;
    *(_QWORD *)v6 = *(_QWORD *)(v7 + SocMMIORegs);
    v6 += 4;
  }
  while ( (int)v5 < 768 );
  v8 = v16;
  v9 = 0;
  v10 = 16;
  do
  {
    --v10;
    v9 = (2 * v9) | ((v8 & 0xF000000000000000LL) != 0);
    v8 *= 16;
  }
  while ( (_DWORD)v10 != 0 );
  v11 = v17;
  LOWORD(v12) = 0;
  v13 = 16;
  do
  {
    --v13;
    v12 = (unsigned __int16)((2 * v12) | ((v11 & 0xF000000000000000LL) != 0));
    v11 *= 16;
  }
  while ( (_DWORD)v13 != 0 );
  return sub_4E8(v9, v12, a1);
}


/* ========== [0xa560] HvxBlowFuses (620 bytes) ========== */
__int64 __fastcall HvxBlowFuses(__int64 a1)
{
  unsigned __int64 v1; // r28
  __int64 SocMMIORegs; // r27
  __int64 v3; // r29
  __int64 v4; // r2
  __int64 v5; // r30
  unsigned __int64 v6; // r11
  __int64 v7; // r26
  __int64 v8; // r25
  __int64 v9; // r3
  __int64 v10; // r8
  __int64 v11; // r3
  unsigned int v12; // r11
  __int64 v13; // r10
  __int64 v14; // r31
  __int64 v15; // r3
  __int64 v16; // r3

  if ( *(_BYTE *)(__mfspr(0x131u) + 128) != 0 )
    v_MACHINE_CHECK_0(a1);
  v1 = (unsigned int)a1 & 0xDFFFFFFF;
  if ( (a1 & 0x40000000) != 0 )
    v1 = 0;
  if ( (v1 & 0x10000000) != 0 )
    v1 = (unsigned int)v1 & 0xEFFFFFFF;
  SocMMIORegs = HvpGetSocMMIORegs(397312);
  v3 = HvpGetSocMMIORegs(0x20000);
  sub_84E0(1);
  v5 = v4 + 0x10000;
  v6 = *(_QWORD *)(SocMMIORegs + 392);
  v7 = v6 & 7;
  v8 = (v6 >> 8) & 0x3F;
  if ( *(_QWORD *)(v4 + 92832) >= 0x710700u )
    sub_89E0(HIWORD(v6) & 0x3F, ((v6 >> 40) & 7) + *(unsigned __int8 *)(v4 + 91026) - 128);
  v9 = sub_8C90(SocMMIORegs, 2);
  LODWORD(v11) = sub_8E00(v10, 4, v9);
  if ( *(_QWORD *)(v5 + 27296) >= 0x710700u )
  {
    v12 = 949026816;
    v13 = 135141645;
  }
  else
  {
    v12 = 111147008;
    v13 = 204088857;
  }
  *(_QWORD *)(v3 + 8200) = (v13 << 32) | v12;
  __eieio();
  __isync();
  v14 = 0;
  if ( (v1 & 0x80) != 0 )
  {
    LODWORD(v11) = -1;
    sub_9558(v11, (v1 >> 8) & 0xFFF);
  }
  else if ( (v1 & 1) == 0 || (v11 = sub_9738(v11), v14 = v11, (_DWORD)v11 == 0) )
  {
    if ( (v1 & 2) == 0 || (v11 = sub_98D8(v11), v14 = v11, (_DWORD)v11 == 0) )
    {
      if ( (v1 & 4) == 0 || (v11 = sub_9A28(v1), v14 = v11, (_DWORD)v11 == 0) )
      {
        if ( (v1 & 8) == 0 || (v11 = sub_9CC8(v11), v14 = v11, (_DWORD)v11 == 0) )
        {
          if ( (v1 & 0x10) != 0 )
          {
            v14 = 13;
          }
          else if ( (v1 & 0x20) != 0 )
          {
            v14 = 16;
          }
          else if ( (v1 & 0x40) == 0 || (v11 = sub_9DB0(v11), v14 = v11, (_DWORD)v11 == 0) )
          {
            if ( (v1 & 0x100000) == 0 || (v11 = sub_9E78(v1), v14 = v11, (_DWORD)v11 == 0) )
            {
              if ( (v1 & 0x20000000) != 0 )
                v14 = sub_A068(v11);
            }
          }
        }
      }
    }
  }
  v15 = sub_8C90(SocMMIORegs, v7 != 1);
  v16 = sub_8E00(v8, v7, v15);
  if ( (v1 & 0x80) == 0 )
    v16 = sub_A120(v16);
  sub_87A8(v16);
  return v14;
}


/* ========== [0xa7d0] __Save_R12_14_thru_31 (4 bytes) ========== */
__int64 _Save_R12_14_thru_31()
{
  return _Save_R12_15_thru_31();
}


/* ========== [0xa7d4] __Save_R12_15_thru_31 (4 bytes) ========== */
__int64 _Save_R12_15_thru_31()
{
  return _Save_R12_16_thru_31();
}


/* ========== [0xa7d8] __Save_R12_16_thru_31 (4 bytes) ========== */
__int64 _Save_R12_16_thru_31()
{
  return _Save_R12_17_thru_31();
}


/* ========== [0xa7dc] __Save_R12_17_thru_31 (4 bytes) ========== */
__int64 _Save_R12_17_thru_31()
{
  return _Save_R12_18_thru_31();
}


/* ========== [0xa7e0] __Save_R12_18_thru_31 (4 bytes) ========== */
__int64 _Save_R12_18_thru_31()
{
  return _Save_R12_19_thru_31();
}


/* ========== [0xa7e4] __Save_R12_19_thru_31 (4 bytes) ========== */
__int64 _Save_R12_19_thru_31()
{
  return _Save_R12_20_thru_31();
}


/* ========== [0xa7e8] __Save_R12_20_thru_31 (4 bytes) ========== */
__int64 _Save_R12_20_thru_31()
{
  return _Save_R12_21_thru_31();
}


/* ========== [0xa7ec] __Save_R12_21_thru_31 (4 bytes) ========== */
__int64 _Save_R12_21_thru_31()
{
  return _Save_R12_22_thru_31();
}


/* ========== [0xa7f0] __Save_R12_22_thru_31 (4 bytes) ========== */
__int64 _Save_R12_22_thru_31()
{
  return _Save_R12_23_thru_31();
}


/* ========== [0xa7f4] __Save_R12_23_thru_31 (4 bytes) ========== */
__int64 _Save_R12_23_thru_31()
{
  return _Save_R12_24_thru_31();
}


/* ========== [0xa7f8] __Save_R12_24_thru_31 (4 bytes) ========== */
__int64 _Save_R12_24_thru_31()
{
  return _Save_R12_25_thru_31();
}


/* ========== [0xa7fc] __Save_R12_25_thru_31 (4 bytes) ========== */
__int64 _Save_R12_25_thru_31()
{
  return _Save_R12_26_thru_31();
}


/* ========== [0xa800] __Save_R12_26_thru_31 (4 bytes) ========== */
__int64 _Save_R12_26_thru_31()
{
  return _Save_R12_27_thru_31();
}


/* ========== [0xa804] __Save_R12_27_thru_31 (4 bytes) ========== */
__int64 _Save_R12_27_thru_31()
{
  return _Save_R12_28_thru_31();
}


/* ========== [0xa808] __Save_R12_28_thru_31 (4 bytes) ========== */
__int64 _Save_R12_28_thru_31()
{
  return _Save_R12_29_thru_31();
}


/* ========== [0xa80c] __Save_R12_29_thru_31 (4 bytes) ========== */
__int64 _Save_R12_29_thru_31()
{
  return _Save_R12_30_thru_31();
}


/* ========== [0xa810] __Save_R12_30_thru_31 (4 bytes) ========== */
__int64 _Save_R12_30_thru_31()
{
  return _Save_R12_31_thru_31();
}


/* ========== [0xa814] __Save_R12_31_thru_31 (12 bytes) ========== */
void _Save_R12_31_thru_31()
{
  ;
}


/* ========== [0xa820] __Rest_R12_lr_14_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_14_thru_31()
{
  return _Rest_R12_lr_15_thru_31();
}


/* ========== [0xa824] __Rest_R12_lr_15_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_15_thru_31()
{
  return _Rest_R12_lr_16_thru_31();
}


/* ========== [0xa828] __Rest_R12_lr_16_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_16_thru_31()
{
  return _Rest_R12_lr_17_thru_31();
}


/* ========== [0xa82c] __Rest_R12_lr_17_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_17_thru_31()
{
  return _Rest_R12_lr_18_thru_31();
}


/* ========== [0xa830] __Rest_R12_lr_18_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_18_thru_31()
{
  return _Rest_R12_lr_19_thru_31();
}


/* ========== [0xa834] __Rest_R12_lr_19_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_19_thru_31()
{
  return _Rest_R12_lr_20_thru_31();
}


/* ========== [0xa838] __Rest_R12_lr_20_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_20_thru_31()
{
  return _Rest_R12_lr_21_thru_31();
}


/* ========== [0xa83c] __Rest_R12_lr_21_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_21_thru_31()
{
  return _Rest_R12_lr_22_thru_31();
}


/* ========== [0xa840] __Rest_R12_lr_22_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_22_thru_31()
{
  return _Rest_R12_lr_23_thru_31();
}


/* ========== [0xa844] __Rest_R12_lr_23_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_23_thru_31()
{
  return _Rest_R12_lr_24_thru_31();
}


/* ========== [0xa848] __Rest_R12_lr_24_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_24_thru_31()
{
  return _Rest_R12_lr_25_thru_31();
}


/* ========== [0xa84c] __Rest_R12_lr_25_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_25_thru_31()
{
  return _Rest_R12_lr_26_thru_31();
}


/* ========== [0xa850] __Rest_R12_lr_26_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_26_thru_31()
{
  return _Rest_R12_lr_27_thru_31();
}


/* ========== [0xa854] __Rest_R12_lr_27_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_27_thru_31()
{
  return _Rest_R12_lr_28_thru_31();
}


/* ========== [0xa858] __Rest_R12_lr_28_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_28_thru_31()
{
  return _Rest_R12_lr_29_thru_31();
}


/* ========== [0xa85c] __Rest_R12_lr_29_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_29_thru_31()
{
  return _Rest_R12_lr_30_thru_31();
}


/* ========== [0xa860] __Rest_R12_lr_30_thru_31 (4 bytes) ========== */
__int64 _Rest_R12_lr_30_thru_31()
{
  return _Rest_R12_lr_31_thru_31();
}


/* ========== [0xa864] __Rest_R12_lr_31_thru_31 (16 bytes) ========== */
__int64 _Rest_R12_lr_31_thru_31()
{
  __int64 (*v1)(void); // [sp+8h] [-8h]

  return v1();
}


/* ========== [0xa880] memcpy (1004 bytes) ========== */
__int64 __fastcall memcpy(__int64 a1, __int64 a2, unsigned __int64 a3, __int64 a4, __int64 a5)
{
  __int64 v5; // r6
  __int64 v6; // r3
  __int64 v7; // r4
  __int64 v8; // ctr
  _QWORD *v9; // r4
  _QWORD *v10; // r3
  __int64 v11; // ctr
  char *v12; // r3
  char *v13; // r4
  __int64 v14; // ctr
  __int64 v16; // r6
  bool v17; // zf
  __int64 v18; // ctr
  bool v19; // cr38
  int v20; // r9
  __int64 v21; // ctr
  unsigned int v22; // r11
  __int64 v23; // ctr
  __int64 v24; // r7
  __int64 v25; // r8
  __int64 v26; // r6
  __int64 v27; // r7
  __int64 v28; // r8
  __int64 v29; // r6
  __int64 v30; // r7
  __int64 v31; // r8
  __int64 v32; // r6
  __int64 v33; // r7
  __int64 v34; // r8
  __int64 v35; // r6
  __int64 v36; // r7
  __int64 v37; // r8
  __int64 v38; // r6
  _DWORD *v39; // r4
  _DWORD *v40; // r3
  __int64 v41; // ctr
  char *v42; // r3
  char *v43; // r4
  __int64 v44; // ctr
  __int64 v45; // r6
  __int64 v46; // ctr
  bool v47; // cr38
  int v48; // r9
  __int64 v49; // ctr
  unsigned int v50; // r11
  __int64 v51; // ctr
  __int64 v52; // r6
  int v53; // r7
  int v54; // r8
  int v55; // r0
  __int64 v56; // r4
  __int64 v57; // r3
  __int64 i; // ctr
  __int64 v59; // r6
  __int64 v60; // ctr
  bool v61; // cr38
  int v62; // r9
  __int64 v63; // ctr
  unsigned int v64; // r11
  __int64 v65; // ctr
  __int64 v66; // r6
  unsigned __int16 v67; // r7
  unsigned __int8 v68; // r9
  unsigned __int8 v69; // r10
  __int64 back_chain; // [sp+0h] [-8h]

  back_chain = a1;
  v5 = 8 - (a1 & 7);
  if ( (a1 & 7) != 0 )
  {
    if ( (unsigned int)a3 <= (unsigned int)v5 )
      goto LABEL_9;
    v6 = a1 - 1;
    v7 = a2 - 1;
    a3 -= v5;
    v8 = v5;
    do
    {
      *(_BYTE *)++v6 = *(_BYTE *)++v7;
      --v8;
    }
    while ( v8 != 0 );
    a1 = v6 + 1;
    a2 = v7 + 1;
  }
  if ( (a2 & 7) == 4 )
  {
    v39 = (_DWORD *)(a2 - 4);
    if ( (unsigned int)a3 >= 0x80 )
    {
      v45 = 128 - (a1 & 0x7F);
      v17 = (a1 & 0x7F) == 0;
      v40 = (_DWORD *)(a1 - 4);
      if ( !v17 )
      {
        LODWORD(a5) = (unsigned int)v45 >> 2;
        a3 -= v45;
        v46 = a5;
        do
        {
          *++v40 = *++v39;
          --v46;
        }
        while ( v46 != 0 );
      }
      if ( (unsigned int)a3 >> 7 != 0 )
      {
        v47 = (a3 & 0x7F) == 0;
        v48 = 4;
        v49 = (((unsigned __int8)((unsigned int)(a3 + 127) >> 7) - 1) & 7) + 1LL;
        do
        {
          v48 += 128;
          --v49;
        }
        while ( v49 != 0 );
        v50 = (_DWORD)v39 + a3 - v48;
        LODWORD(v45) = (unsigned int)a3 >> 7;
        v51 = v45;
        do
        {
          while ( 1 )
          {
            v52 = 8;
            do
            {
              --v52;
              v53 = v39[2];
              v54 = v39[3];
              v40[1] = v39[1];
              v39 += 4;
              v55 = *v39;
              v40[2] = v53;
              v40[3] = v54;
              v40 += 4;
              *v40 = v55;
            }
            while ( (_DWORD)v52 != 0 );
            if ( (unsigned int)v39 >= v50 )
              break;
            if ( --v51 == 0 )
              goto LABEL_33;
          }
          if ( !v47 )
            v47 = false;
          --v51;
        }
        while ( v51 != 0 );
      }
    }
    else
    {
      v40 = (_DWORD *)(a1 - 4);
    }
LABEL_33:
    if ( ((a3 >> 2) & 0x1F) != 0 )
    {
      v41 = (a3 >> 2) & 0x1F;
      do
      {
        *++v40 = *++v39;
        --v41;
      }
      while ( v41 != 0 );
    }
    if ( (a3 & 3) != 0 )
    {
      v42 = (char *)v40 + 3;
      v43 = (char *)v39 + 3;
      v44 = a3 & 3;
      do
      {
        *++v42 = *++v43;
        --v44;
      }
      while ( v44 != 0 );
    }
    return back_chain;
  }
  else
  {
    if ( (a2 & 7) == 0 )
    {
      if ( (unsigned int)a3 >= 0x80 )
      {
        v9 = (_QWORD *)(a2 - 8);
        v16 = 128 - (a1 & 0x7F);
        v17 = (a1 & 0x7F) == 0;
        v10 = (_QWORD *)(a1 - 8);
        if ( !v17 )
        {
          LODWORD(a5) = (unsigned int)v16 >> 3;
          a3 -= v16;
          v18 = a5;
          do
          {
            *++v10 = *++v9;
            --v18;
          }
          while ( v18 != 0 );
        }
        if ( (unsigned int)a3 >> 7 != 0 )
        {
          v19 = (a3 & 0x7F) == 0;
          v20 = 8;
          v21 = (((unsigned __int8)((unsigned int)(a3 + 127) >> 7) - 1) & 7) + 1LL;
          do
          {
            v20 += 128;
            --v21;
          }
          while ( v21 != 0 );
          v22 = (_DWORD)v9 + a3 - v20;
          LODWORD(v16) = (unsigned int)a3 >> 7;
          v23 = v16;
          do
          {
            while ( 1 )
            {
              v24 = v9[2];
              v25 = v9[3];
              v10[1] = v9[1];
              v26 = v9[4];
              v10[2] = v24;
              v27 = v9[5];
              v10[3] = v25;
              v28 = v9[6];
              v10[4] = v26;
              v29 = v9[7];
              v10[5] = v27;
              v30 = v9[8];
              v10[6] = v28;
              v31 = v9[9];
              v10[7] = v29;
              v32 = v9[10];
              v10[8] = v30;
              v33 = v9[11];
              v10[9] = v31;
              v34 = v9[12];
              v10[10] = v32;
              v35 = v9[13];
              v10[11] = v33;
              v36 = v9[14];
              v10[12] = v34;
              v37 = v9[15];
              v10[13] = v35;
              v9 += 16;
              v38 = *v9;
              v10[14] = v36;
              v10[15] = v37;
              v10 += 16;
              *v10 = v38;
              if ( (unsigned int)v9 >= v22 )
                break;
              if ( --v23 == 0 )
                goto LABEL_10;
            }
            if ( !v19 )
              v19 = false;
            --v23;
          }
          while ( v23 != 0 );
        }
        goto LABEL_10;
      }
LABEL_9:
      v9 = (_QWORD *)(a2 - 8);
      v10 = (_QWORD *)(a1 - 8);
LABEL_10:
      if ( ((a3 >> 3) & 0xF) != 0 )
      {
        v11 = (a3 >> 3) & 0xF;
        do
        {
          *++v10 = *++v9;
          --v11;
        }
        while ( v11 != 0 );
      }
      if ( (a3 & 7) != 0 )
      {
        v12 = (char *)v10 + 7;
        v13 = (char *)v9 + 7;
        v14 = a3 & 7;
        do
        {
          *++v12 = *++v13;
          --v14;
        }
        while ( v14 != 0 );
      }
      return back_chain;
    }
    v56 = a2 - 1;
    if ( (unsigned int)a3 >= 0x80 )
    {
      v59 = 128 - (a1 & 0x7F);
      v17 = (a1 & 0x7F) == 0;
      v57 = a1 - 1;
      if ( !v17 )
      {
        LODWORD(a3) = a3 - v59;
        v60 = v59;
        do
        {
          v59 = *(unsigned __int8 *)++v56;
          *(_BYTE *)++v57 = v59;
          --v60;
        }
        while ( v60 != 0 );
      }
      if ( (unsigned int)a3 >> 7 != 0 )
      {
        v61 = (a3 & 0x7F) == 0;
        v62 = 1;
        v63 = (((unsigned __int8)((unsigned int)(a3 + 127) >> 7) - 1) & 7) + 1LL;
        do
        {
          v62 += 128;
          --v63;
        }
        while ( v63 != 0 );
        v64 = v56 + a3 - v62;
        LODWORD(v59) = (unsigned int)a3 >> 7;
        v65 = v59;
        do
        {
          while ( 1 )
          {
            v66 = 32;
            do
            {
              --v66;
              v67 = __builtin_bswap16(*(_WORD *)(v56 + 3));
              v68 = *(_BYTE *)(v56 + 2);
              v69 = *(_BYTE *)(v56 + 1);
              v56 += 4;
              *(_DWORD *)(v57 + 1) = (v69 << 24) | (v68 << 16) | v67;
              v57 += 4;
            }
            while ( (_DWORD)v66 != 0 );
            if ( (unsigned int)v56 >= v64 )
              break;
            if ( --v65 == 0 )
              goto LABEL_58;
          }
          if ( !v61 )
            v61 = false;
          --v65;
        }
        while ( v65 != 0 );
      }
    }
    else
    {
      v57 = a1 - 1;
    }
LABEL_58:
    for ( i = a3 & 0x7F; i != 0; --i )
      *(_BYTE *)++v57 = *(_BYTE *)++v56;
    return back_chain;
  }
}


/* ========== [0xac6c] _blkmov (168 bytes) ========== */
_BYTE *__fastcall blkmov(_BYTE *result, unsigned __int8 *a2, __int64 a3)
{
  _BYTE *v3; // r6
  __int64 v4; // ctr
  __int64 v5; // ctr
  __int64 i; // ctr
  int v7; // r7

  v3 = result;
  v4 = a3 + 1;
  while ( --v4 != 0 && ((unsigned __int8)v3 & 3) != 0 )
  {
    LODWORD(a3) = a3 - 1;
    *v3++ = *a2++;
  }
  if ( (unsigned int)a3 >> 2 != 0 )
  {
    v5 = (unsigned int)a3 >> 2;
    if ( ((unsigned __int8)a2 & 3) != 0 )
    {
      do
      {
        v7 = (*a2 << 24) | (a2[1] << 16) | __builtin_bswap16(*((_WORD *)a2 + 1));
        a2 += 4;
        *(_DWORD *)v3 = v7;
        v3 += 4;
        --v5;
      }
      while ( v5 != 0 );
    }
    else
    {
      do
      {
        *(_DWORD *)v3 = *(_DWORD *)a2;
        a2 += 4;
        v3 += 4;
        --v5;
      }
      while ( v5 != 0 );
    }
  }
  for ( i = a3 & 3; i != 0; --i )
    *v3++ = *a2++;
  return result;
}


/* ========== [0xad20] memset (160 bytes) ========== */
__int64 __fastcall memset(__int64 result, unsigned __int8 a2, unsigned __int64 a3)
{
  unsigned __int64 v3; // ctr
  __int64 i; // r6
  int v5; // r4
  __int64 v6; // ctr
  __int64 v7; // r0

  v3 = a3 + 1;
  for ( i = result; --v3 != 0 && (i & 3) != 0; ++i )
  {
    --a3;
    *(_BYTE *)i = a2;
  }
  v5 = ((unsigned __int16)((a2 << 8) | a2) << 16) | (a2 << 8) | a2;
  if ( (unsigned int)a3 >> 4 != 0 )
  {
    v6 = (unsigned int)a3 >> 4;
    do
    {
      *(_DWORD *)i = v5;
      *(_DWORD *)(i + 4) = v5;
      *(_DWORD *)(i + 8) = v5;
      *(_DWORD *)(i + 12) = v5;
      i += 16;
      --v6;
    }
    while ( v6 != 0 );
  }
  v7 = (a3 >> 2) & 3;
  if ( v7 != 0 )
  {
    *(_DWORD *)i = v5;
    i += 4;
    if ( v7 != 1 )
    {
      *(_DWORD *)i = v5;
      i += 4;
      if ( v7 != 2 )
      {
        *(_DWORD *)i = v5;
        i += 4;
      }
    }
  }
  if ( (a3 & 3) != 0 )
  {
    *(_BYTE *)i = v5;
    if ( (a3 & 3) != 1 )
    {
      *(_BYTE *)(i + 1) = v5;
      if ( (a3 & 3) != 2 )
        *(_BYTE *)(i + 2) = v5;
    }
  }
  return result;
}


/* ========== [0xadc0] sub_ADC0 (188 bytes) ========== */
__int64 __fastcall sub_ADC0(__int64 result, __int64 a2, unsigned __int64 a3, __int64 a4, __int64 a5)
{
  unsigned __int64 v5; // r4
  unsigned __int64 v6; // ctr
  char v7; // r0
  __int64 v8; // ctr
  int v9; // r7
  unsigned __int64 i; // ctr
  char v11; // r0
  int v12; // r7

  if ( (_DWORD)result != (_DWORD)a2 )
  {
    if ( (int)result >= (int)a2 )
    {
      result += a3;
      v5 = a2 + a3;
      v6 = a3 + 1;
      while ( --v6 != 0 && (result & 3) != 0 )
      {
        LODWORD(a3) = a3 - 1;
        v7 = *(_BYTE *)--v5;
        *(_BYTE *)--result = v7;
      }
      if ( (unsigned int)a3 >> 2 != 0 )
      {
        v8 = (unsigned int)a3 >> 2;
        if ( (v5 & 3) != 0 )
        {
          do
          {
            result -= 4;
            v12 = (*(unsigned __int8 *)(v5 - 4) << 24)
                | (*(unsigned __int8 *)(v5 - 3) << 16)
                | __builtin_bswap16(*(_WORD *)(v5 - 2));
            v5 -= 4LL;
            *(_DWORD *)result = v12;
            --v8;
          }
          while ( v8 != 0 );
        }
        else
        {
          do
          {
            v9 = *(_DWORD *)(v5 - 4);
            v5 -= 4LL;
            *(_DWORD *)(result - 4) = v9;
            result -= 4;
            --v8;
          }
          while ( v8 != 0 );
        }
      }
      for ( i = a3 & 3; i != 0; --i )
      {
        v11 = *(_BYTE *)--v5;
        *(_BYTE *)--result = v11;
      }
    }
    else
    {
      return memcpy(result, a2, a3, a4, a5);
    }
  }
  return result;
}


/* ========== [0xb0b0] sub_B0B0 (8 bytes) ========== */
__int64 sub_B0B0()
{
  return jt1_jumper();
}


/* ========== [0xb0b8] sub_B0B8 (8 bytes) ========== */
__int64 sub_B0B8()
{
  return jt1_jumper();
}


/* ========== [0xb0c0] sub_B0C0 (8 bytes) ========== */
__int64 sub_B0C0()
{
  return jt1_jumper();
}


/* ========== [0xb0c8] sub_B0C8 (8 bytes) ========== */
__int64 sub_B0C8()
{
  return jt1_jumper();
}


/* ========== [0xb0d0] sub_B0D0 (8 bytes) ========== */
__int64 sub_B0D0()
{
  return jt1_jumper();
}


/* ========== [0xb0d8] sub_B0D8 (8 bytes) ========== */
__int64 sub_B0D8()
{
  return jt1_jumper();
}


/* ========== [0xb0e0] sub_B0E0 (8 bytes) ========== */
__int64 sub_B0E0()
{
  return jt1_jumper();
}


/* ========== [0xb0e8] sub_B0E8 (8 bytes) ========== */
__int64 sub_B0E8()
{
  return jt1_jumper();
}


/* ========== [0xb0f0] sub_B0F0 (8 bytes) ========== */
__int64 sub_B0F0()
{
  return jt1_jumper();
}


/* ========== [0xb0f8] sub_B0F8 (8 bytes) ========== */
__int64 sub_B0F8()
{
  return jt1_jumper();
}


/* ========== [0xb100] sub_B100 (8 bytes) ========== */
__int64 sub_B100()
{
  return jt1_jumper();
}


/* ========== [0xb110] sub_B110 (8 bytes) ========== */
__int64 sub_B110()
{
  return jt1_jumper();
}


/* ========== [0xb118] sub_B118 (8 bytes) ========== */
__int64 sub_B118()
{
  return jt1_jumper();
}


/* ========== [0xb120] sub_B120 (8 bytes) ========== */
__int64 sub_B120()
{
  return jt1_jumper();
}


/* ========== [0xb128] sub_B128 (8 bytes) ========== */
__int64 sub_B128()
{
  return jt1_jumper();
}


/* ========== [0xb130] sub_B130 (8 bytes) ========== */
__int64 sub_B130()
{
  return jt1_jumper();
}


/* ========== [0xb138] sub_B138 (8 bytes) ========== */
__int64 sub_B138()
{
  return jt1_jumper();
}


/* ========== [0xb140] sub_B140 (8 bytes) ========== */
__int64 sub_B140()
{
  return jt1_jumper();
}


/* ========== [0xb148] sub_B148 (8 bytes) ========== */
__int64 sub_B148()
{
  return jt1_jumper();
}


/* ========== [0xb150] sub_B150 (8 bytes) ========== */
__int64 sub_B150()
{
  return jt1_jumper();
}


/* ========== [0xb158] sub_B158 (8 bytes) ========== */
__int64 sub_B158()
{
  return jt1_jumper();
}


/* ========== [0xb160] sub_B160 (8 bytes) ========== */
__int64 sub_B160()
{
  return jt1_jumper();
}


/* ========== [0xb168] sub_B168 (8 bytes) ========== */
__int64 sub_B168()
{
  return jt1_jumper();
}


/* ========== [0xb170] HvxAbandonImageLoad_Thunk (8 bytes) ========== */
/* HvxUnmapImage */
// HvxUnmapImage
__int64 HvxAbandonImageLoad_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb178] HvxCompleteImageMapping_Thunk (8 bytes) ========== */
/* HvxCompleteImageMapping */
// HvxCompleteImageMapping
__int64 HvxCompleteImageMapping_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb180] HvxCreateImageMapping_Thunk (8 bytes) ========== */
/* HvxCreateImageMapping */
// HvxCreateImageMapping
__int64 HvxCreateImageMapping_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb188] HvxCreateUserMode_Thunk (8 bytes) ========== */
/* HvxFlushUserModeTb */
// HvxFlushUserModeTb
__int64 HvxCreateUserMode_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb190] HvxDeleteUserMode_Thunk (8 bytes) ========== */
/* HvxSetPowerMode */
// HvxSetPowerMode
__int64 HvxDeleteUserMode_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb198] HvxDvdAuthBuildNVPage_Thunk (8 bytes) ========== */
/* HvxDvdAuthRecordAuthenticationPage */
// HvxDvdAuthRecordAuthenticationPage
__int64 HvxDvdAuthBuildNVPage_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb1a0] HvxDvdAuthClearDiscAuthInfo_Thunk (8 bytes) ========== */
/* HvxKeysGetKeyProperties */
// HvxKeysGetKeyProperties
__int64 HvxDvdAuthClearDiscAuthInfo_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb1a8] HvxDvdAuthEx_Thunk (8 bytes) ========== */
/* HvxSyscall_76 */
// HvxSyscall_76
__int64 HvxDvdAuthEx_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb1b0] HvxDvdAuthFcrt_Thunk (8 bytes) ========== */
/* HvxDvdAuthFwcr */
// HvxDvdAuthFwcr
__int64 HvxDvdAuthFcrt_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb1b8] HvxDvdAuthFwcr_Thunk (8 bytes) ========== */
/* HvxSyscall_74 */
// HvxSyscall_74
__int64 HvxDvdAuthFwcr_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb1c0] HvxDvdAuthGetAuthPage_Thunk (8 bytes) ========== */
/* HvxDvdAuthGetNextLBAIndex */
// HvxDvdAuthGetNextLBAIndex
__int64 HvxDvdAuthGetAuthPage_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb1c8] HvxDvdAuthGetAuthResults_Thunk (8 bytes) ========== */
/* HvxSyscall_5E */
// HvxSyscall_5E
__int64 HvxDvdAuthGetAuthResults_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb1d0] HvxDvdAuthGetMediaId_Thunk (8 bytes) ========== */
/* HvxSyscall_66 */
// HvxSyscall_66
__int64 HvxDvdAuthGetMediaId_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb1d8] HvxDvdAuthGetNextLBAIndex_Thunk (8 bytes) ========== */
/* HvxDvdAuthClearDiscAuthInfo */
// HvxDvdAuthClearDiscAuthInfo
__int64 HvxDvdAuthGetNextLBAIndex_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb1e0] HvxDvdAuthRecordAuthenticationPage_Thunk (8 bytes) ========== */
/* HvxDvdAuthGetAuthPage */
// HvxDvdAuthGetAuthPage
__int64 HvxDvdAuthRecordAuthenticationPage_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb1e8] HvxDvdAuthRecordXControl_Thunk (8 bytes) ========== */
/* HvxDvdAuthVerifyAuthPage */
// HvxDvdAuthVerifyAuthPage
__int64 HvxDvdAuthRecordXControl_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb1f0] HvxDvdAuthSetDiscAuthResult_Thunk (8 bytes) ========== */
/* HvxSyscall_60 */
// HvxSyscall_60
__int64 HvxDvdAuthSetDiscAuthResult_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb1f8] HvxDvdAuthSetDriveAuthResult_Thunk (8 bytes) ========== */
/* HvxSyscall_5F */
// HvxSyscall_5F
__int64 HvxDvdAuthSetDriveAuthResult_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb200] HvxDvdAuthTestMode_Thunk (8 bytes) ========== */
/* HvxSyscall_43 */
// HvxSyscall_43
__int64 HvxDvdAuthTestMode_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb208] HvxDvdAuthVerifyAuthPage_Thunk (8 bytes) ========== */
/* HvxDvdAuthVerifyLBA */
// HvxDvdAuthVerifyLBA
__int64 HvxDvdAuthVerifyAuthPage_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb210] HvxDvdAuthVerifyLBA_Thunk (8 bytes) ========== */
/* HvxKeysInitialize */
// HvxKeysInitialize
__int64 HvxDvdAuthVerifyLBA_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb218] HvxDvdAuthVerifyNVPage_Thunk (8 bytes) ========== */
/* HvxDvdAuthRecordXControl */
// HvxDvdAuthRecordXControl
__int64 HvxDvdAuthVerifyNVPage_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb220] HvxEncryptedAllocationMap_Thunk (8 bytes) ========== */
/* HvxSyscall_4A */
// HvxSyscall_4A
__int64 HvxEncryptedAllocationMap_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb228] HvxEncryptedAllocationRelease_Thunk (8 bytes) ========== */
/* HvxSyscall_4C */
// HvxSyscall_4C
__int64 HvxEncryptedAllocationRelease_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb230] HvxEncryptedAllocationReserve_Thunk (8 bytes) ========== */
/* HvxSyscall_49 */
// HvxSyscall_49
__int64 HvxEncryptedAllocationReserve_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb238] HvxEncryptedAllocationUnmap_Thunk (8 bytes) ========== */
/* HvxSyscall_4B */
// HvxSyscall_4B
__int64 HvxEncryptedAllocationUnmap_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb240] HvxEncryptedSweepAddressRange_Thunk (8 bytes) ========== */
/* HvxSyscall_4D */
// HvxSyscall_4D
__int64 HvxEncryptedSweepAddressRange_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb248] HvxExpansionCall_Thunk (8 bytes) ========== */
/* HvxExpansionCall */
// HvxExpansionCall
__int64 HvxExpansionCall_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb250] HvxExpansionInstall_Thunk (8 bytes) ========== */
/* HvxExpansionInstall */
// HvxExpansionInstall
__int64 HvxExpansionInstall_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb258] HvxFinishImageDataLoad_Thunk (8 bytes) ========== */
/* HvxFinishImageDataLoad */
// HvxFinishImageDataLoad
__int64 HvxFinishImageDataLoad_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb260] HvxFinishImageLoad_Thunk (8 bytes) ========== */
/* HvxUnmapImagePages */
// HvxUnmapImagePages
__int64 HvxFinishImageLoad_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb268] HvxFlushUserModeTb_Thunk (8 bytes) ========== */
/* HvxShadowBoot */
// HvxShadowBoot
__int64 HvxFlushUserModeTb_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb270] HvxGetImagePageTableEntry_Thunk (8 bytes) ========== */
/* HvxGetImagePageTableEntry */
// HvxGetImagePageTableEntry
__int64 HvxGetImagePageTableEntry_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb278] HvxHdcpCalculateAKsvSignature_Thunk (8 bytes) ========== */
/* HvxSyscall_46 */
// HvxSyscall_46
__int64 HvxHdcpCalculateAKsvSignature_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb280] HvxHdcpCalculateBKsvSignature_Thunk (8 bytes) ========== */
/* HvxSyscall_47 */
// HvxSyscall_47
__int64 HvxHdcpCalculateBKsvSignature_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb288] HvxHdcpCalculateMi_Thunk (8 bytes) ========== */
/* HvxSyscall_45 */
// HvxSyscall_45
__int64 HvxHdcpCalculateMi_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb290] HvxImageDvdEmulationMode_Thunk (8 bytes) ========== */
/* HvxSyscall_6F */
// HvxSyscall_6F
__int64 HvxImageDvdEmulationMode_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb298] HvxImageShim_Thunk (8 bytes) ========== */
/* HvxSyscall_71 */
// HvxSyscall_71
__int64 HvxImageShim_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb2a0] HvxImageTransformImageKey_Thunk (8 bytes) ========== */
/* HvxSyscall_61 */
// HvxSyscall_61
__int64 HvxImageTransformImageKey_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb2a8] HvxImageUserMode_Thunk (8 bytes) ========== */
/* HvxSyscall_70 */
// HvxSyscall_70
__int64 HvxImageUserMode_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb2b0] HvxImageXexHeader_Thunk (8 bytes) ========== */
/* HvxSyscall_62 */
// HvxSyscall_62
__int64 HvxImageXexHeader_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb2b8] HvxIptvGetAesCtrTransform_Thunk (8 bytes) ========== */
/* HvxSyscall_6D */
// HvxSyscall_6D
__int64 HvxIptvGetAesCtrTransform_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb2c0] HvxIptvGetSessionKeyHash_Thunk (8 bytes) ========== */
/* HvxSyscall_6E */
// HvxSyscall_6E
__int64 HvxIptvGetSessionKeyHash_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb2c8] HvxIptvSetBoundaryKey_Thunk (8 bytes) ========== */
/* HvxSyscall_6A */
// HvxSyscall_6A
__int64 HvxIptvSetBoundaryKey_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb2d0] HvxIptvSetSessionKey_Thunk (8 bytes) ========== */
/* HvxSyscall_6B */
// HvxSyscall_6B
__int64 HvxIptvSetSessionKey_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb2d8] HvxIptvVerifyOmac1Signature_Thunk (8 bytes) ========== */
/* HvxSyscall_6C */
// HvxSyscall_6C
__int64 HvxIptvVerifyOmac1Signature_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb2e0] HvxLoadImageData_Thunk (8 bytes) ========== */
/* HvxLoadImageData */
// HvxLoadImageData
__int64 HvxLoadImageData_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb2e8] HvxMapImagePage_Thunk (8 bytes) ========== */
/* HvxMapImagePage */
// HvxMapImagePage
__int64 HvxMapImagePage_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb2f0] HvxResolveImports_Thunk (8 bytes) ========== */
/* HvxResolveImports */
// HvxResolveImports
__int64 HvxResolveImports_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb2f8] HvxRevokeLoad_Thunk (8 bytes) ========== */
/* HvxSyscall_63 */
// HvxSyscall_63
__int64 HvxRevokeLoad_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb300] HvxRevokeSave_Thunk (8 bytes) ========== */
/* HvxSyscall_64 */
// HvxSyscall_64
__int64 HvxRevokeSave_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb308] HvxRevokeUpdate_Thunk (8 bytes) ========== */
/* HvxSyscall_65 */
// HvxSyscall_65
__int64 HvxRevokeUpdate_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb310] HvxXexUnk25_Thunk (8 bytes) ========== */
/* HvxAbandonImageLoad */
// HvxAbandonImageLoad
__int64 HvxXexUnk25_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb318] HvxSetImagePageTableEntry_Thunk (8 bytes) ========== */
/* HvxSetImagePageTableEntry */
// HvxSetImagePageTableEntry
__int64 HvxSetImagePageTableEntry_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb320] HvxSetRevocationList_Thunk (8 bytes) ========== */
/* HvxSyscall_48 */
// HvxSyscall_48
__int64 HvxSetRevocationList_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb328] HvxShadowBoot_Thunk (8 bytes) ========== */
/* HvxFsbInterrupt */
// HvxFsbInterrupt
__int64 HvxShadowBoot_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb330] HvxStartResolveImports_Thunk (8 bytes) ========== */
/* HvxStartResolveImports */
// HvxStartResolveImports
__int64 HvxStartResolveImports_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb338] HvxXexUnk24_Thunk (8 bytes) ========== */
/* HvxFinishImageLoad */
// HvxFinishImageLoad
__int64 HvxXexUnk24_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb340] HvxUnmapImage_Thunk (8 bytes) ========== */
/* HvxCreateUserMode */
// HvxCreateUserMode
__int64 HvxUnmapImage_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb348] HvxUnmapImagePages_Thunk (8 bytes) ========== */
/* HvxUnmapImageRange */
// HvxUnmapImageRange
__int64 HvxUnmapImagePages_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb350] HvxUnmapImageRange_Thunk (8 bytes) ========== */
/* HvxDeleteUserMode */
// HvxDeleteUserMode
__int64 HvxUnmapImageRange_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb358] HvxXexActivationGetNonce_Thunk (8 bytes) ========== */
/* HvxSyscall_67 */
// HvxSyscall_67
__int64 HvxXexActivationGetNonce_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb360] HvxXexActivationSetLicense_Thunk (8 bytes) ========== */
/* HvxSyscall_68 */
// HvxSyscall_68
__int64 HvxXexActivationSetLicense_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb368] HvxXexActivationVerifyOwnership_Thunk (8 bytes) ========== */
/* HvxSyscall_69 */
// HvxSyscall_69
__int64 HvxXexActivationVerifyOwnership_Thunk()
{
  return jt1_jumper();
}


/* ========== [0xb370] sub_B370 (8 bytes) ========== */
__int64 sub_B370()
{
  return jt1_jumper();
}


/* ========== [0xb378] sub_B378 (8 bytes) ========== */
__int64 sub_B378()
{
  return jt1_jumper();
}


/* ========== [0xb380] sub_B380 (8 bytes) ========== */
__int64 sub_B380()
{
  return jt1_jumper();
}


/* ========== [0xb388] sub_B388 (8 bytes) ========== */
__int64 sub_B388()
{
  return jt1_jumper();
}


/* ========== [0xb390] sub_B390 (8 bytes) ========== */
__int64 sub_B390()
{
  return jt1_jumper();
}


/* ========== [0xb398] j_XeCryptBnDwLePkcs1Format (8 bytes) ========== */
__int64 j_XeCryptBnDwLePkcs1Format()
{
  return jt1_jumper();
}


/* ========== [0xb3a0] sub_B3A0 (8 bytes) ========== */
__int64 sub_B3A0()
{
  return jt1_jumper();
}


/* ========== [0xb3a8] sub_B3A8 (8 bytes) ========== */
__int64 sub_B3A8()
{
  return jt1_jumper();
}


/* ========== [0xb3b0] sub_B3B0 (8 bytes) ========== */
__int64 sub_B3B0()
{
  return jt1_jumper();
}


/* ========== [0xb3b8] sub_B3B8 (8 bytes) ========== */
__int64 sub_B3B8()
{
  return jt1_jumper();
}


/* ========== [0xb3c0] j_XeCryptBnQw_SwapDwQwLeBe (8 bytes) ========== */
__int64 j_XeCryptBnQw_SwapDwQwLeBe()
{
  return jt1_jumper();
}


/* ========== [0xb3c8] sub_B3C8 (8 bytes) ========== */
__int64 sub_B3C8()
{
  return jt1_jumper();
}


/* ========== [0xb3d0] sub_B3D0 (8 bytes) ========== */
__int64 sub_B3D0()
{
  return jt1_jumper();
}


/* ========== [0xb3d8] j_XeCryptBnQwBeSigVerify (8 bytes) ========== */
__int64 j_XeCryptBnQwBeSigVerify()
{
  return jt1_jumper();
}


/* ========== [0xb3e0] sub_B3E0 (8 bytes) ========== */
__int64 sub_B3E0()
{
  return jt1_jumper();
}


/* ========== [0xb3e8] sub_B3E8 (8 bytes) ========== */
__int64 sub_B3E8()
{
  return jt1_jumper();
}


/* ========== [0xb3f0] sub_B3F0 (8 bytes) ========== */
__int64 sub_B3F0()
{
  return jt1_jumper();
}


/* ========== [0xb3f8] j_XeCryptBnQwNeRsaPrvCrypt (8 bytes) ========== */
__int64 j_XeCryptBnQwNeRsaPrvCrypt()
{
  return jt1_jumper();
}


/* ========== [0xb400] j_XeCryptBnQwNeRsaPubCrypt (8 bytes) ========== */
__int64 j_XeCryptBnQwNeRsaPubCrypt()
{
  return jt1_jumper();
}


/* ========== [0xb408] j_XeCryptDes3Cbc (8 bytes) ========== */
__int64 j_XeCryptDes3Cbc()
{
  return jt1_jumper();
}


/* ========== [0xb410] j_XeCryptDes3Ecb (8 bytes) ========== */
__int64 j_XeCryptDes3Ecb()
{
  return jt1_jumper();
}


/* ========== [0xb418] j_XeCryptDes3Key (8 bytes) ========== */
__int64 j_XeCryptDes3Key()
{
  return jt1_jumper();
}


/* ========== [0xb420] j_XeCryptDesCbc (8 bytes) ========== */
__int64 j_XeCryptDesCbc()
{
  return jt1_jumper();
}


/* ========== [0xb428] j_XeCryptDesEcb (8 bytes) ========== */
__int64 j_XeCryptDesEcb()
{
  return jt1_jumper();
}


/* ========== [0xb430] j_XeCryptDesKey (8 bytes) ========== */
__int64 j_XeCryptDesKey()
{
  return jt1_jumper();
}


/* ========== [0xb438] j_XeCryptDesParity (8 bytes) ========== */
__int64 j_XeCryptDesParity()
{
  return jt1_jumper();
}


/* ========== [0xb440] j_HvSetupMemEncryptionKey (8 bytes) ========== */
__int64 j_HvSetupMemEncryptionKey()
{
  return jt1_jumper();
}


/* ========== [0xb448] sub_B448 (8 bytes) ========== */
__int64 sub_B448()
{
  return jt1_jumper();
}


/* ========== [0xb450] sub_B450 (8 bytes) ========== */
__int64 sub_B450()
{
  return jt1_jumper();
}


/* ========== [0xb458] j_XeCryptHmacSha (8 bytes) ========== */
__int64 j_XeCryptHmacSha()
{
  return jt1_jumper();
}


/* ========== [0xb460] j_XeCryptHmacShaFinal (8 bytes) ========== */
__int64 j_XeCryptHmacShaFinal()
{
  return jt1_jumper();
}


/* ========== [0xb468] j_XeCryptHmacShaInit (8 bytes) ========== */
__int64 j_XeCryptHmacShaInit()
{
  return jt1_jumper();
}


/* ========== [0xb470] j_XeCryptHmacShaUpdate (8 bytes) ========== */
__int64 j_XeCryptHmacShaUpdate()
{
  return jt1_jumper();
}


/* ========== [0xb478] sub_B478 (8 bytes) ========== */
__int64 sub_B478()
{
  return jt1_jumper();
}


/* ========== [0xb480] sub_B480 (8 bytes) ========== */
__int64 sub_B480()
{
  return jt1_jumper();
}


/* ========== [0xb488] sub_B488 (8 bytes) ========== */
__int64 sub_B488()
{
  return jt1_jumper();
}


/* ========== [0xb490] J_XeCryptMemDiff (8 bytes) ========== */
__int64 J_XeCryptMemDiff()
{
  return jt1_jumper();
}


/* ========== [0xb498] sub_B498 (8 bytes) ========== */
__int64 sub_B498()
{
  return jt1_jumper();
}


/* ========== [0xb4a0] XeCryptRandom (8 bytes) ========== */
void __fastcall XeCryptRandom(uint8_t *random, __int64 random_size)
{
  jt1_jumper(random, random_size);
}


/* ========== [0xb4a8] j_XeCryptRc4 (8 bytes) ========== */
__int64 j_XeCryptRc4()
{
  return jt1_jumper();
}


/* ========== [0xb4b0] j_XeCryptRc4Ecb (8 bytes) ========== */
__int64 j_XeCryptRc4Ecb()
{
  return jt1_jumper();
}


/* ========== [0xb4b8] j_XeCryptRc4Key (8 bytes) ========== */
__int64 j_XeCryptRc4Key()
{
  return jt1_jumper();
}


/* ========== [0xb4c0] sub_B4C0 (8 bytes) ========== */
__int64 sub_B4C0()
{
  return jt1_jumper();
}


/* ========== [0xb4c8] j_XeCryptRotSumSha (8 bytes) ========== */
__int64 j_XeCryptRotSumSha()
{
  return jt1_jumper();
}


/* ========== [0xb4d0] j_XeCryptSha (8 bytes) ========== */
__int64 j_XeCryptSha()
{
  return jt1_jumper();
}


/* ========== [0xb4d8] sub_B4D8 (8 bytes) ========== */
__int64 sub_B4D8()
{
  return jt1_jumper();
}


/* ========== [0xb4e0] j_XeCryptShaFinal (8 bytes) ========== */
__int64 j_XeCryptShaFinal()
{
  return jt1_jumper();
}


/* ========== [0xb4e8] j_XeCryptShaInit (8 bytes) ========== */
__int64 j_XeCryptShaInit()
{
  return jt1_jumper();
}


/* ========== [0xb4f0] j_XeCryptShaUpdate (8 bytes) ========== */
__int64 j_XeCryptShaUpdate()
{
  return jt1_jumper();
}


/* ========== [0xb4f8] sub_B4F8 (8 bytes) ========== */
__int64 sub_B4F8()
{
  return jt1_jumper();
}


/* ========== [0xb500] sub_B500 (8 bytes) ========== */
__int64 sub_B500()
{
  return jt1_jumper();
}


/* ========== [0xb508] sub_B508 (8 bytes) ========== */
__int64 sub_B508()
{
  return jt1_jumper();
}


/* ========== [0x10350] __Save_R12_14_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_14_thru_31_()
{
  return _Save_R12_15_thru_31_();
}


/* ========== [0x10354] __Save_R12_15_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_15_thru_31_()
{
  return _Save_R12_16_thru_31_();
}


/* ========== [0x10358] __Save_R12_16_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_16_thru_31_()
{
  return _Save_R12_17_thru_31_();
}


/* ========== [0x1035c] __Save_R12_17_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_17_thru_31_()
{
  return _Save_R12_18_thru_31_();
}


/* ========== [0x10360] __Save_R12_18_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_18_thru_31_()
{
  return _Save_R12_19_thru_31_();
}


/* ========== [0x10364] __Save_R12_19_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_19_thru_31_()
{
  return _Save_R12_20_thru_31_();
}


/* ========== [0x10368] __Save_R12_20_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_20_thru_31_()
{
  return _Save_R12_21_thru_31_();
}


/* ========== [0x1036c] __Save_R12_21_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_21_thru_31_()
{
  return _Save_R12_22_thru_31_();
}


/* ========== [0x10370] __Save_R12_22_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_22_thru_31_()
{
  return _Save_R12_23_thru_31_();
}


/* ========== [0x10374] __Save_R12_23_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_23_thru_31_()
{
  return _Save_R12_24_thru_31_();
}


/* ========== [0x10378] __Save_R12_24_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_24_thru_31_()
{
  return _Save_R12_25_thru_31_();
}


/* ========== [0x1037c] __Save_R12_25_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_25_thru_31_()
{
  return _Save_R12_26_thru_31_();
}


/* ========== [0x10380] __Save_R12_26_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_26_thru_31_()
{
  return _Save_R12_27_thru_31_();
}


/* ========== [0x10384] __Save_R12_27_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_27_thru_31_()
{
  return _Save_R12_28_thru_31_();
}


/* ========== [0x10388] __Save_R12_28_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_28_thru_31_()
{
  return _Save_R12_29_thru_31_();
}


/* ========== [0x1038c] __Save_R12_29_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_29_thru_31_()
{
  return _Save_R12_30_thru_31_();
}


/* ========== [0x10390] __Save_R12_30_thru_31_ (4 bytes) ========== */
__int64 _Save_R12_30_thru_31_()
{
  return _Save_R12_31_thru_31_();
}


/* ========== [0x10394] __Save_R12_31_thru_31_ (12 bytes) ========== */
void _Save_R12_31_thru_31_()
{
  ;
}


/* ========== [0x103a0] __Rest_R12_lr_14_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_14_thru_31_()
{
  return _Rest_R12_lr_15_thru_31_();
}


/* ========== [0x103a4] __Rest_R12_lr_15_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_15_thru_31_()
{
  return _Rest_R12_lr_16_thru_31_();
}


/* ========== [0x103a8] __Rest_R12_lr_16_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_16_thru_31_()
{
  return _Rest_R12_lr_17_thru_31_();
}


/* ========== [0x103ac] __Rest_R12_lr_17_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_17_thru_31_()
{
  return _Rest_R12_lr_18_thru_31_();
}


/* ========== [0x103b0] __Rest_R12_lr_18_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_18_thru_31_()
{
  return _Rest_R12_lr_19_thru_31_();
}


/* ========== [0x103b4] __Rest_R12_lr_19_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_19_thru_31_()
{
  return _Rest_R12_lr_20_thru_31_();
}


/* ========== [0x103b8] __Rest_R12_lr_20_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_20_thru_31_()
{
  return _Rest_R12_lr_21_thru_31_();
}


/* ========== [0x103bc] __Rest_R12_lr_21_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_21_thru_31_()
{
  return _Rest_R12_lr_22_thru_31_();
}


/* ========== [0x103c0] __Rest_R12_lr_22_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_22_thru_31_()
{
  return _Rest_R12_lr_23_thru_31_();
}


/* ========== [0x103c4] __Rest_R12_lr_23_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_23_thru_31_()
{
  return _Rest_R12_lr_24_thru_31_();
}


/* ========== [0x103c8] __Rest_R12_lr_24_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_24_thru_31_()
{
  return _Rest_R12_lr_25_thru_31_();
}


/* ========== [0x103cc] __Rest_R12_lr_25_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_25_thru_31_()
{
  return _Rest_R12_lr_26_thru_31_();
}


/* ========== [0x103d0] __Rest_R12_lr_26_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_26_thru_31_()
{
  return _Rest_R12_lr_27_thru_31_();
}


/* ========== [0x103d4] __Rest_R12_lr_27_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_27_thru_31_()
{
  return _Rest_R12_lr_28_thru_31_();
}


/* ========== [0x103d8] __Rest_R12_lr_28_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_28_thru_31_()
{
  return _Rest_R12_lr_29_thru_31_();
}


/* ========== [0x103dc] __Rest_R12_lr_29_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_29_thru_31_()
{
  return _Rest_R12_lr_30_thru_31_();
}


/* ========== [0x103e0] __Rest_R12_lr_30_thru_31_ (4 bytes) ========== */
__int64 _Rest_R12_lr_30_thru_31_()
{
  return _Rest_R12_lr_31_thru_31_();
}


/* ========== [0x103e4] __Rest_R12_lr_31_thru_31_ (16 bytes) ========== */
__int64 _Rest_R12_lr_31_thru_31_()
{
  unsigned int v1; // [sp+8h] [-8h]

  return ((__int64 (*)(void))v1)();
}


/* ========== [0x103f4] sub_103F4 (4 bytes) ========== */
__int64 sub_103F4()
{
  return sub_103F8();
}


/* ========== [0x103f8] sub_103F8 (4 bytes) ========== */
__int64 sub_103F8()
{
  return sub_103FC();
}


/* ========== [0x103fc] sub_103FC (4 bytes) ========== */
__int64 sub_103FC()
{
  return sub_10400();
}


/* ========== [0x10400] sub_10400 (4 bytes) ========== */
__int64 sub_10400()
{
  return sub_10404();
}


/* ========== [0x10404] sub_10404 (4 bytes) ========== */
__int64 sub_10404()
{
  return sub_10408();
}


/* ========== [0x10408] sub_10408 (4 bytes) ========== */
__int64 sub_10408()
{
  return sub_1040C();
}


/* ========== [0x1040c] sub_1040C (4 bytes) ========== */
__int64 sub_1040C()
{
  return sub_10410();
}


/* ========== [0x10410] sub_10410 (4 bytes) ========== */
__int64 sub_10410()
{
  return sub_10414();
}


/* ========== [0x10414] sub_10414 (4 bytes) ========== */
__int64 sub_10414()
{
  return sub_10418();
}


/* ========== [0x10418] sub_10418 (4 bytes) ========== */
__int64 sub_10418()
{
  return sub_1041C();
}


/* ========== [0x1041c] sub_1041C (4 bytes) ========== */
__int64 sub_1041C()
{
  return sub_10420();
}


/* ========== [0x10420] sub_10420 (4 bytes) ========== */
__int64 sub_10420()
{
  return sub_10424();
}


/* ========== [0x10424] sub_10424 (4 bytes) ========== */
__int64 sub_10424()
{
  return sub_10428();
}


/* ========== [0x10428] sub_10428 (4 bytes) ========== */
__int64 sub_10428()
{
  return sub_1042C();
}


/* ========== [0x1042c] sub_1042C (4 bytes) ========== */
__int64 sub_1042C()
{
  return sub_10430();
}


/* ========== [0x10430] sub_10430 (4 bytes) ========== */
__int64 sub_10430()
{
  return sub_10434();
}


/* ========== [0x10434] sub_10434 (4 bytes) ========== */
__int64 sub_10434()
{
  return sub_10438();
}


/* ========== [0x10438] sub_10438 (12 bytes) ========== */
void sub_10438()
{
  JUMPOUT(0x10444);
}


/* ========== [0x10450] sub_10450 (4 bytes) ========== */
__int64 sub_10450()
{
  return sub_10454();
}


/* ========== [0x10454] sub_10454 (4 bytes) ========== */
__int64 sub_10454()
{
  return sub_10458();
}


/* ========== [0x10458] sub_10458 (4 bytes) ========== */
__int64 sub_10458()
{
  return sub_1045C();
}


/* ========== [0x1045c] sub_1045C (4 bytes) ========== */
__int64 sub_1045C()
{
  return sub_10460();
}


/* ========== [0x10460] sub_10460 (4 bytes) ========== */
__int64 sub_10460()
{
  return sub_10464();
}


/* ========== [0x10464] sub_10464 (4 bytes) ========== */
__int64 sub_10464()
{
  return sub_10468();
}


/* ========== [0x10468] sub_10468 (4 bytes) ========== */
__int64 sub_10468()
{
  return sub_1046C();
}


/* ========== [0x1046c] sub_1046C (4 bytes) ========== */
__int64 sub_1046C()
{
  return sub_10470();
}


/* ========== [0x10470] sub_10470 (4 bytes) ========== */
__int64 sub_10470()
{
  return sub_10474();
}


/* ========== [0x10474] sub_10474 (4 bytes) ========== */
__int64 sub_10474()
{
  return sub_10478();
}


/* ========== [0x10478] sub_10478 (4 bytes) ========== */
__int64 sub_10478()
{
  return sub_1047C();
}


/* ========== [0x1047c] sub_1047C (4 bytes) ========== */
__int64 sub_1047C()
{
  return sub_10480();
}


/* ========== [0x10480] sub_10480 (4 bytes) ========== */
__int64 sub_10480()
{
  return sub_10484();
}


/* ========== [0x10484] sub_10484 (4 bytes) ========== */
__int64 sub_10484()
{
  return sub_10488();
}


/* ========== [0x10488] sub_10488 (4 bytes) ========== */
__int64 sub_10488()
{
  return sub_1048C();
}


/* ========== [0x1048c] sub_1048C (4 bytes) ========== */
__int64 sub_1048C()
{
  return sub_10490();
}


/* ========== [0x10490] sub_10490 (4 bytes) ========== */
__int64 sub_10490()
{
  return sub_10494();
}


/* ========== [0x10494] sub_10494 (16 bytes) ========== */
void sub_10494()
{
  JUMPOUT(0x104A4);
}


/* ========== [0x20000] sub_20000 (64 bytes) ========== */
__int64 __fastcall sub_20000(unsigned int a1)
{
  __int64 v1; // r2
  __int64 v2; // r2

  if ( a1 > 0x2800 )
    return 0;
  HvpAquireSpinLock((_DWORD *)(v1 + 92536));
  return *(_QWORD *)(v2 + 92808);
}


/* ========== [0x20040] sub_20040 (28 bytes) ========== */
_DWORD *__fastcall sub_20040(_DWORD *result)
{
  __int64 v1; // r2

  if ( result == *(_DWORD **)(v1 + 92808) )
    return HvpReleaseSpinLock((_DWORD *)(v1 + 92536));
  return result;
}


/* ========== [0x20060] sub_20060 (72 bytes) ========== */
_DWORD *__fastcall sub_20060(uint8_t *a1, __int64 a2)
{
  __int64 v2; // r2
  _DWORD *v4; // r31
  __int64 v6; // r2

  v4 = (_DWORD *)(v2 + 92816);
  HvpAquireSpinLock((_DWORD *)(v2 + 92816));
  XeCryptRc4Ecb((X_CRYPT_RC4_STATE *)(v6 + 92544), a1, a2);
  return HvpReleaseSpinLock(v4);
}


/* ========== [0x200f8] XeCryptAesKey (12 bytes) ========== */
void __fastcall XeCryptAesKey(X_CRYPT_AES_STATE *state, const uint8_t *key, __int64 a3)
{
  int v3; // r2

  LODWORD(a3) = v3 + 81504;
  sub_23E70(state, key, a3);
}


/* ========== [0x20108] XeCryptAesEcb (32 bytes) ========== */
void __fastcall XeCryptAesEcb(X_CRYPT_AES_STATE *state, const uint8_t *input, uint8_t *output, __int64 encrypt)
{
  int v4; // r2

  if ( (_DWORD)encrypt != 0 )
  {
    LODWORD(encrypt) = v4 + 73056;
    sub_2402C(state, input, output, encrypt);
  }
  else
  {
    LODWORD(encrypt) = v4 + 77152;
    XeCryptBnQwBeSigDifference(state, input, output, encrypt);
  }
}


/* ========== [0x20128] XeCryptAesCbc (292 bytes) ========== */
void __fastcall XeCryptAesCbc(
        X_CRYPT_AES_STATE *state,
        const uint8_t *input,
        uint32_t size,
        uint8_t *output,
        uint8_t *feed,
        int encrypt)
{
  __int64 v6; // r29
  __int64 v7; // r2
  __int64 v12; // r11
  __int64 v13; // r10
  __int64 v14; // r26
  __int64 v15; // r26
  __int64 v16; // r8
  __int64 v17; // [sp+50h] [-70h] BYREF
  __int64 v18; // [sp+58h] [-68h]
  _QWORD v19[2]; // [sp+60h] [-60h] BYREF
  _QWORD v20[10]; // [sp+70h] [-50h] BYREF

  LODWORD(v6) = size >> 4;
  v12 = *(_QWORD *)feed;
  v13 = *((_QWORD *)feed + 1);
  v17 = *(_QWORD *)feed;
  v18 = v13;
  if ( encrypt != 0 )
  {
    if ( (_DWORD)v6 != 0 )
    {
      v14 = v7 + 73056;
      do
      {
        sub_2402C(state, &v17, &v17, v14);
        v12 = v17;
        --v6;
        v13 = v18;
        input += 16;
        *(_QWORD *)output = v17;
        *((_QWORD *)output + 1) = v13;
        output += 16;
      }
      while ( (_DWORD)v6 != 0 );
    }
  }
  else if ( (_DWORD)v6 != 0 )
  {
    v15 = v7 + 77152;
    do
    {
      XeCryptBnQwBeSigDifference(state, v19, v20, v15);
      --v6;
      input += 16;
      v16 = v20[1] ^ v18;
      v12 = v19[0];
      v13 = v19[1];
      *(_QWORD *)output = v20[0] ^ v17;
      *((_QWORD *)output + 1) = v16;
      output += 16;
      v17 = v12;
      v18 = v13;
    }
    while ( (_DWORD)v6 != 0 );
  }
  *(_QWORD *)feed = v12;
  *((_QWORD *)feed + 1) = v13;
}


/* ========== [0x20250] XeCryptAesCtr (176 bytes) ========== */
void __fastcall XeCryptAesCtr(
        const X_CRYPT_AES_STATE *state,
        const uint8_t *input,
        uint32_t size,
        uint8_t *output,
        uint8_t *counter)
{
  __int64 v5; // r28
  __int64 v6; // r2
  __int64 v11; // r26
  int v12; // r9
  __int64 v13; // r10
  int v14; // r11
  _QWORD v15[10]; // [sp+50h] [-50h] BYREF

  LODWORD(v5) = size >> 4;
  if ( size >> 4 != 0 )
  {
    v11 = v6 + 73056;
    do
    {
      sub_2402C(state, counter, v15, v11);
      v12 = 15;
      v13 = v15[1];
      *(_QWORD *)output = *(_QWORD *)input ^ v15[0];
      *((_QWORD *)output + 1) = *((_QWORD *)input + 1) ^ v13;
      do
      {
        v14 = (unsigned __int8)(counter[v12] + 1);
        counter[v12] = v14;
        if ( v14 != 0 )
          break;
        --v12;
      }
      while ( v12 != -1 );
      --v5;
      input += 16;
      output += 16;
    }
    while ( (_DWORD)v5 != 0 );
  }
}


/* ========== [0x20300] XeCryptAesCbcMac (164 bytes) ========== */
void __fastcall XeCryptAesCbcMac(const uint8_t *key, const uint8_t *input, uint32_t size, uint8_t *output)
{
  __int64 v4; // r31
  __int64 v5; // r2
  __int64 v6; // r11
  __int64 v7; // r10
  __int64 v10; // r29
  __int64 v11; // r28
  __int64 v12; // [sp+50h] [-1B0h] BYREF
  __int64 v13; // [sp+58h] [-1A8h]
  __int64 v14; // [sp+60h] [-1A0h] BYREF
  __int64 v15; // [sp+68h] [-198h]
  _WORD v16[200]; // [sp+70h] [-190h] BYREF

  v6 = *(_QWORD *)key;
  LODWORD(v4) = size >> 4;
  v7 = *((_QWORD *)key + 1);
  v12 = *(_QWORD *)key;
  v13 = v7;
  if ( size >> 4 != 0 )
  {
    v10 = v5 + 73056;
    v11 = v5 + 81504;
    do
    {
      sub_23E70(v16, input, v11);
      sub_2402C(v16, &v12, &v14, v10);
      --v4;
      input += 16;
      v6 = v14 ^ v12;
      v7 = v15 ^ v13;
      v12 ^= v14;
      v13 ^= v15;
    }
    while ( (_DWORD)v4 != 0 );
  }
  *(_QWORD *)output = v6;
  *((_QWORD *)output + 1) = v7;
}


/* ========== [0x203a8] XeCryptBnDwLePkcs1Format (192 bytes) ========== */
_BYTE *__fastcall XeCryptBnDwLePkcs1Format(__int64 a1, int a2, _BYTE *a3, unsigned int a4)
{
  _BYTE *result; // r3
  __int64 v9; // r2
  _BYTE *v10; // r10
  __int64 v11; // r11
  __int64 v12; // r4

  result = (_BYTE *)memset(a3, 255, a4);
  if ( a4 - 39 <= 0x1D9 )
  {
    v10 = a3;
    v11 = 19;
    a3[a4 - 1] = 0;
    a3[a4 - 2] = 1;
    do
      *v10++ = *(_BYTE *)((int)v11-- + a1);
    while ( (int)v11 > -1 );
    result = a3 + 20;
    if ( a2 == 0 )
    {
      v12 = v9 + 85872;
      return (_BYTE *)memcpy(result, v12);
    }
    if ( a2 == 1 )
    {
      v12 = v9 + 85888;
      return (_BYTE *)memcpy(result, v12);
    }
    *result = 0;
  }
  return result;
}


/* ========== [0x20468] XeCryptBnDwLePkcs1Verify (180 bytes) ========== */
__int64 __fastcall XeCryptBnDwLePkcs1Verify(__int64 a1, _BYTE *a2, unsigned int a3)
{
  __int64 v3; // r2
  __int64 v4; // lr
  int v7; // r4
  _BYTE v9[512]; // [sp+50h] [-220h] BYREF
  int v10; // [sp+250h] [-20h]
  __int64 v11; // [sp+268h] [-8h]

  v11 = v4;
  v10 = *(_DWORD *)(v3 + 0x10000);
  if ( a3 - 39 > 0x1D9
    || (a2[22] != 0 ? (v7 = ((_cntlzw((unsigned int)(unsigned __int8)a2[22] - 26) & 0x20) == 0) + 1) : (v7 = 0),
        XeCryptBnDwLePkcs1Format(a1, v7, v9, a3),
        XeCryptMemDiff(a2, v9, a3) != 0) )
  {
    v_MACHINE_CHECK_filter(v10);
    return 0;
  }
  else
  {
    v_MACHINE_CHECK_filter(v10);
    return 1;
  }
}


/* ========== [0x20520] XeCryptBnDw_Copy (44 bytes) ========== */
char *__fastcall XeCryptBnDw_Copy(char *result, char *a2, __int64 a3)
{
  bool v3; // cr58
  signed __int64 v4; // r4
  __int64 v5; // ctr

  v3 = result == a2;
  v4 = a2 - result;
  v5 = a3;
  if ( (_DWORD)a3 != 0 && !v3 )
  {
    do
    {
      *(_DWORD *)&result[v4] = *(_DWORD *)result;
      result += 4;
      --v5;
    }
    while ( v5 != 0 );
  }
  return result;
}


/* ========== [0x20550] XeCryptBnDw_SwapLeBe (36 bytes) ========== */
unsigned int *__fastcall XeCryptBnDw_SwapLeBe(unsigned int *result, __int64 a2, __int64 a3)
{
  __int64 v3; // r4
  __int64 v4; // ctr

  v3 = a2 - (_QWORD)result;
  v4 = a3;
  if ( (_DWORD)a3 != 0 )
  {
    do
    {
      *(unsigned int *)((char *)result + v3) = __builtin_bswap32(*result);
      ++result;
      --v4;
    }
    while ( v4 != 0 );
  }
  return result;
}


/* ========== [0x20578] XeCryptBnQwBeSigFormat (264 bytes) ========== */
__int64 __fastcall XeCryptBnQwBeSigFormat(unsigned __int64 a1, const uint8_t *a2, const uint8_t *a3)
{
  __int64 v3; // r2
  __int64 *v7; // r11
  __int64 *v8; // r10
  __int64 v9; // r9
  X_CRYPT_RC4_STATE v11; // [sp+50h] [-130h] BYREF
  int v12; // [sp+154h] [-2Ch]

  v12 = *(_DWORD *)(v3 + 0x10000);
  memset(a1, 0, 224);
  *(_BYTE *)(a1 + 224) = 1;
  memcpy(a1 + 225, a3);
  *(_BYTE *)(a1 + 255) = -68;
  XeCryptShaInit((X_CRYPT_SHA_STATE *)&v11);
  XeCryptShaUpdate((X_CRYPT_SHA_STATE *)&v11, (const uint8_t *)a1, 8u);
  XeCryptShaUpdate((X_CRYPT_SHA_STATE *)&v11, a2, 0x14u);
  XeCryptShaUpdate((X_CRYPT_SHA_STATE *)&v11, a3, 0xAu);
  XeCryptShaFinal((X_CRYPT_SHA_STATE *)&v11, (uint8_t *)(a1 + 235), 0x14u);
  XeCryptRc4Key(&v11, (const uint8_t *)(a1 + 235), 0x14u);
  XeCryptRc4Ecb(&v11, (uint8_t *)a1, 0xEBu);
  v7 = (__int64 *)(a1 + 248);
  v8 = (__int64 *)a1;
  *(_BYTE *)a1 &= ~0x80u;
  if ( a1 < a1 + 248 )
  {
    do
    {
      v9 = *v8;
      *v8++ = *v7;
      *v7-- = v9;
    }
    while ( v8 < v7 );
  }
  return v_MACHINE_CHECK_filter(v12);
}


/* ========== [0x20680] XeCryptBnQwBeSigVerify (112 bytes) ========== */
bool __fastcall XeCryptBnQwBeSigVerify(__int64 a1, __int64 a2, __int64 a3, _DWORD *a4)
{
  __int64 v4; // lr
  int v5; // r11
  _WORD v7[128]; // [sp+50h] [-210h] BYREF
  _WORD v8[132]; // [sp+150h] [-110h] BYREF
  __int64 v9; // [sp+258h] [-8h]

  v9 = v4;
  v5 = a4[1];
  if ( *a4 != 32 || v5 != 3 && v5 != 65537 )
    return false;
  XeCryptBnQwNeDigLen();
  return (_cntlzw(XeCryptMemDiff(v8, v7, 0x100u)) & 0x20) != 0;
}


/* ========== [0x206f0] sub_206F0 (368 bytes) ========== */
char *__fastcall sub_206F0(_QWORD *a1, unsigned __int64 *a2, char *a3, __int64 a4)
{
  unsigned __int64 *v4; // r31
  char *v5; // r23
  unsigned __int64 v6; // r3
  __int64 v7; // r30
  __int64 v8; // r26
  __int64 v9; // r25
  __int64 v10; // r27
  __int64 v11; // r11
  unsigned __int64 v12; // r29
  unsigned __int64 v13; // r11
  __int64 v14; // r29
  unsigned __int64 v15; // r11
  __int64 v16; // r4
  __int64 v17; // r6
  __int64 v18; // r5
  unsigned __int64 v19; // r11

  v4 = a2;
  v5 = a3;
  v7 = *(unsigned int *)a1;
  v8 = a1[2];
  v9 = a1[3];
  v10 = a1[1];
  v6 = 0;
  v11 = v7 - 2;
  v12 = (unsigned __int64)&a2[v11];
  if ( &a2[v11] >= a2 )
  {
    do
    {
      v13 = *(_QWORD *)(8LL * (unsigned int)(v7 + 1) + v12);
      LODWORD(a4) = v7;
      if ( v13 >= v6 )
      {
        LODWORD(a3) = v9;
        LODWORD(a2) = v13 - v6;
      }
      else
      {
        LODWORD(a3) = v8;
        LODWORD(a2) = v6 - v13;
      }
      v6 = sub_24C88(v12, a2, a3, a4);
      v12 -= 8LL;
    }
    while ( v12 >= (unsigned __int64)v4 );
  }
  v14 = v7;
  v15 = v4[v7];
  v4[v7] = v15 - v6;
  if ( v15 >= v6 )
  {
    if ( (int)sub_24D08(v4 + 1, v10, v7) >= 0 )
      sub_24B38(v4 + 1, v4 + 1, v10, v7);
  }
  else
  {
    while ( XeCryptBnQwNeAcc(v4 + 1, (__int64)(v4 + 1), v10, v7) == 0 )
      ;
  }
  LODWORD(v16) = sub_24788(v4[v14], v4[(unsigned int)(v7 - 1)], *(_QWORD *)(8LL * (unsigned int)(v7 - 1) + v10), 0);
  LODWORD(v17) = v7;
  LODWORD(v18) = v10;
  v19 = v4[v14] - sub_24C88(v4, v16, v18, v17);
  v4[v14] = v19;
  while ( v19 != 0 )
  {
    v19 = XeCryptBnQwNeAcc(v4, (__int64)v4, v10, v7) + v4[v14];
    v4[v14] = v19;
  }
  return XeCryptBnQw_Copy((char *)v4, v5, v7);
}


/* ========== [0x20860] sub_20860 (84 bytes) ========== */
char *__fastcall sub_20860(__int64 a1, char *a2, __int64 a3)
{
  char *v4; // r30
  __int64 v5; // r6

  v4 = a2;
  LODWORD(a2) = a3;
  LODWORD(a3) = *(_DWORD *)a1;
  sub_246B0(*(_QWORD *)(a1 + 32), a2, a3);
  return sub_206F0((_QWORD *)a1, *(unsigned __int64 **)(a1 + 32), v4, v5);
}


/* ========== [0x208b8] sub_208B8 (88 bytes) ========== */
char *__fastcall sub_208B8(unsigned int *a1, char *a2, __int64 a3, __int64 a4)
{
  __int64 v6; // r6

  sub_24600(*((_QWORD *)a1 + 4), a3, a4, *a1);
  return sub_206F0(a1, *((unsigned __int64 **)a1 + 4), a2, v6);
}


/* ========== [0x20910] XeCryptBnQwNeModExp (1044 bytes) ========== */
__int64 __fastcall XeCryptBnQwNeModExp(_QWORD *a1, __int64 a2, __int64 a3, __int64 a4, int a5)
{
  int v8; // r22
  __int64 v10; // r14
  __int64 v11; // r4
  __int64 v12; // r3
  __int64 v13; // r31
  __int64 v14; // r24
  __int64 v15; // r11
  int v16; // r30
  __int64 v17; // r28
  __int64 v18; // r4
  __int64 v19; // r3
  int v20; // r23
  __int64 v22; // r21
  int v23; // r29
  __int64 v24; // r5
  __int64 v25; // r4
  __int64 v26; // r3
  int v27; // r27
  __int64 v28; // r11
  __int64 v29; // r9
  __int64 v30; // r30
  __int64 v31; // r7
  int v32; // r28
  int v33; // r26
  __int64 v34; // r29
  __int64 v35; // r25
  __int64 v36; // r6
  __int64 v37; // r5
  __int64 v38; // r4
  __int64 v39; // r3
  __int64 v40; // r3
  __int64 v41; // r7
  __int64 v42; // r6
  __int64 v43; // r5
  __int64 v44; // r4
  __int64 v45; // r6
  __int64 v46; // r5
  __int64 v47; // r4
  __int64 v48; // r3
  __int64 v49; // r5
  __int64 v50; // r4
  __int64 v51; // r7
  __int64 v52; // r6
  __int64 v53; // r5
  __int64 v54; // r4
  __int64 v55; // r3
  __int64 v56; // r29
  int v57; // r28
  __int64 v58; // r30
  int v59; // r30
  int v60; // r28
  int v61; // r24
  int v62; // r23
  unsigned __int64 v63; // r22
  bool v64; // zf
  unsigned __int64 v65; // r20
  int v66; // r29
  int v67; // r26
  __int64 v68; // r4
  int v69; // [sp+50h] [-C0h] BYREF
  __int64 v70; // [sp+58h] [-B8h]
  __int64 v71; // [sp+60h] [-B0h]
  __int64 v72; // [sp+68h] [-A8h]
  __int64 v73; // [sp+70h] [-A0h]

  v8 = a2;
  LODWORD(a2) = a5;
  v10 = 0;
  v12 = XeCryptBnQwNeMul(a4, a2);
  v13 = v12;
  if ( (_DWORD)v12 == 0 )
    return 0;
  v14 = 8LL * ((int)v12 - 1);
  v15 = *(_QWORD *)(v14 + a4);
  if ( v15 == 0 )
    return 0;
  LODWORD(v15) = 16 * v12;
  LODWORD(v11) = a5;
  v16 = 8 * v12;
  v17 = v15 + 8;
  v19 = sub_245B0(a3, v11);
  v20 = v19;
  if ( (_DWORD)v19 == 0 )
  {
    LODWORD(v18) = a5 - 1;
    LODWORD(v19) = (_DWORD)a1 + 8;
    *a1 = 1;
    XeCryptBnQw_Zero(v19, v18);
    return 1;
  }
  if ( (int)v19 > 17 )
  {
    if ( (int)v19 > 32 )
    {
      if ( (int)v19 > 64 )
      {
        if ( (int)v19 > 128 )
        {
          v22 = 16;
          if ( (int)v19 > 256 )
            v22 = 32;
        }
        else
        {
          v22 = 8;
        }
      }
      else
      {
        v22 = 4;
      }
    }
    else
    {
      v22 = 2;
    }
  }
  else
  {
    v22 = 1;
  }
  v23 = (v22 + 4) * v16 + v17;
  v26 = sub_20000(((int)v22 + 4) * (__int64)v16 + v17);
  v27 = v26;
  if ( v26 == 0 )
    return 0;
  v28 = (unsigned int)(8 * v13);
  v29 = (unsigned int)(v22 * v16) - v26;
  v30 = v28 + v26;
  v31 = v23;
  v70 = v28 + v26;
  v33 = v28 + v28 + v28 + v26;
  v34 = v28 + v28 + v28 + v28 + v26;
  v71 = v28 + v28 + v26;
  v35 = (unsigned int)v17 + v34;
  v32 = v28 + v28 + v26;
  v72 = v28 + v71;
  v73 = v34;
  if ( v29 + v35 == v31 )
  {
    LODWORD(v24) = v13;
    LODWORD(v25) = v28 + v26;
    LODWORD(v26) = a4;
    HIDWORD(v39) = (unsigned __int64)XeCryptBnQw_Copy(v26, v25, v24) >> 32;
    while ( *(__int64 *)(v14 + v30) >= 0 )
    {
      LODWORD(v36) = v13;
      LODWORD(v37) = v30;
      LODWORD(v38) = v30;
      LODWORD(v39) = v30;
      HIDWORD(v39) = (unsigned __int64)XeCryptBnQwNeAcc(v39, v38, v37, v36) >> 32;
    }
    LODWORD(v39) = v34;
    LODWORD(v38) = v13 + 1;
    HIDWORD(v40) = (unsigned __int64)XeCryptBnQw_Zero(v39, v38) >> 32;
    LODWORD(v41) = v13;
    LODWORD(v42) = v13 + 2;
    LODWORD(v43) = v32;
    LODWORD(v44) = a4;
    LODWORD(v40) = v34;
    *(_QWORD *)(8LL * ((int)v13 + 1) + v34) = 1;
    v26 = XeCryptBnQwNeAdd(v40, v44, v43, v42, v41);
    if ( (_DWORD)v26 != 0 )
    {
      LODWORD(v45) = v13;
      LODWORD(v46) = v32;
      LODWORD(v47) = a4;
      LODWORD(v26) = v33;
      HIDWORD(v48) = (unsigned __int64)sub_24B38(v26, v47, v46, v45) >> 32;
      LODWORD(v49) = v13;
      LODWORD(v50) = v35;
      LODWORD(v48) = v8;
      HIDWORD(v55) = (unsigned __int64)XeCryptBnQw_Copy(v48, v50, v49) >> 32;
      if ( (int)v22 > 1 )
      {
        LODWORD(v53) = v8;
        LODWORD(v54) = v27;
        sub_20860(&v69, v54, v53);
        v56 = v13;
        v57 = 0;
        v58 = v22 - 1;
        do
        {
          LODWORD(v52) = v27;
          LODWORD(v53) = 8 * v57 + v35;
          LODWORD(v54) = 8 * v56 + v35;
          HIDWORD(v55) = (unsigned __int64)sub_208B8(&v69, v54, v53, v52) >> 32;
          --v58;
          v56 += v13;
          v57 += v13;
        }
        while ( (_DWORD)v58 != 0 );
      }
      v59 = 0;
      v60 = v20 - 1;
      v61 = v20 - 1;
      v63 = 1LL << ((v20 - 1) & 0x3F);
      v64 = v20 - 1 < 0;
      v62 = 1;
      if ( !v64 )
      {
        v65 = v60;
        do
        {
          v59 *= 2;
          if ( (*(_QWORD *)(((v65 >> 3) & 0x1FFFFFFFFFFFFFF8LL) + a3) & v63) != 0 )
            ++v59;
          v63 = (v63 << 63) | (v63 >> 1);
          if ( v60 == 0 || v59 >= (int)v22 )
          {
            v66 = v61 - v60;
            v67 = 0;
            v61 = v60;
            while ( v59 != 0 && v59 % 2 == 0 )
            {
              --v66;
              v59 /= 2;
              ++v67;
            }
            if ( v62 != 0 )
            {
              LODWORD(v53) = v13;
              LODWORD(v54) = v27;
              LODWORD(v55) = 8 * ((v59 - 1) * (int)v13 / 2) + v35;
              HIDWORD(v55) = (unsigned __int64)XeCryptBnQw_Copy(v55, v54, v53) >> 32;
              v62 = 0;
            }
            else
            {
              while ( v66 != 0 )
              {
                LODWORD(v53) = v27;
                LODWORD(v54) = v27;
                --v66;
                HIDWORD(v55) = (unsigned __int64)sub_20860(&v69, v54, v53) >> 32;
              }
              if ( v59 != 0 )
              {
                LODWORD(v53) = v27;
                LODWORD(v54) = v27;
                LODWORD(v52) = 8 * ((v59 - 1) * (int)v13 / 2) + v35;
                HIDWORD(v55) = (unsigned __int64)sub_208B8(&v69, v54, v53, v52) >> 32;
              }
            }
            while ( v67 != 0 )
            {
              LODWORD(v53) = v27;
              LODWORD(v54) = v27;
              --v67;
              HIDWORD(v55) = (unsigned __int64)sub_20860(&v69, v54, v53) >> 32;
            }
            v59 = 0;
          }
          --v60;
          --v65;
        }
        while ( v60 >= 0 );
      }
      LODWORD(v51) = v13;
      LODWORD(v52) = v13;
      LODWORD(v53) = (_DWORD)a1;
      LODWORD(v54) = a4;
      LODWORD(v55) = v27;
      v26 = XeCryptBnQwNeAdd(v55, v54, v53, v52, v51);
      if ( (_DWORD)v26 != 0 )
      {
        LODWORD(v68) = a5 - v13;
        LODWORD(v26) = 8 * v13 + (_DWORD)a1;
        HIDWORD(v26) = (unsigned __int64)XeCryptBnQw_Zero(v26, v68) >> 32;
        v10 = 1;
      }
    }
  }
  LODWORD(v26) = v27;
  sub_20040(v26);
  return v10;
}


/* ========== [0x20d28] XeCryptBnQwNeModExpRoot (412 bytes) ========== */
__int64 __fastcall XeCryptBnQwNeModExpRoot(
        __int64 a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        int a7,
        __int64 a8)
{
  __int64 v8; // r28
  __int64 v17; // r22
  __int64 v18; // r6
  __int64 v19; // r5
  __int64 v20; // r3
  __int64 v21; // r6
  __int64 v22; // r5
  __int64 v23; // r3
  _WORD v25[128]; // [sp+50h] [-460h] BYREF
  _QWORD v26[32]; // [sp+150h] [-360h] BYREF
  _QWORD v27[76]; // [sp+250h] [-260h] BYREF

  v17 = 0;
  if ( (unsigned int)a8 <= 0x20 )
  {
    LODWORD(v8) = 2 * a8;
    if ( (unsigned int)XeCryptBnQwNeAdd(a2, a3, v25, v8, a8) != 0
      && (unsigned int)XeCryptBnQwNeModExp(v26, (__int64)v25, a5, a3, a8) != 0
      && (unsigned int)XeCryptBnQwNeAdd(a2, a4, v25, v8, a8) != 0
      && (unsigned int)XeCryptBnQwNeModExp(v27, (__int64)v25, a6, a4, a8) != 0 )
    {
      v20 = sub_24B38(v25, v26, v27, a8);
      if ( v20 != 0 )
      {
        do
          v20 = XeCryptBnQwNeAcc(v25, v25, a3, a8);
        while ( v20 == 0 );
      }
      LODWORD(v18) = a8;
      LODWORD(v19) = a7;
      LODWORD(v20) = a1;
      sub_24600(v20, v25, v19, v18);
      if ( (unsigned int)XeCryptBnQwNeAdd(a1, a3, v25, v8, a8) != 0 )
      {
        LODWORD(v21) = a8;
        LODWORD(v22) = a4;
        LODWORD(v23) = a1;
        sub_24600(v23, v25, v22, v21);
        XeCryptBnQw_Zero(&v27[(unsigned int)a8], a8);
        XeCryptBnQwNeAcc(a1, a1, v27, v8);
        v17 = 1;
      }
    }
  }
  XeCryptBnQw_Zero(v26, 32);
  XeCryptBnQw_Zero(v27, 64);
  XeCryptBnQw_Zero(v25, 32);
  return v17;
}


/* ========== [0x20ec8] XeCryptBnQwNeModInv (56 bytes) ========== */
__int64 __fastcall XeCryptBnQwNeModInv(__int64 a1)
{
  __int64 v1; // r10
  unsigned int v2; // r9
  __int64 v3; // r11

  v1 = (3 * a1) ^ 2;
  v2 = 5;
  v3 = 1 - v1 * a1;
  do
  {
    v2 *= 2;
    v1 *= v3 + 1;
    v3 *= v3;
  }
  while ( v2 < 0x20 );
  return (v3 + 1) * v1;
}


/* ========== [0x20f00] XeCryptBnQwNeModMul (552 bytes) ========== */
_QWORD *__fastcall XeCryptBnQwNeModMul(
        _QWORD *result,
        unsigned __int64 *a2,
        unsigned __int64 *a3,
        __int64 a4,
        __int64 *a5,
        __int64 a6)
{
  __int64 v6; // ctr
  __int64 *v7; // r14
  __int64 v8; // r10
  __int64 v9; // ctr
  _QWORD *v10; // r9
  unsigned __int64 v11; // r11
  __int64 v12; // r12
  unsigned __int64 v13; // r12
  unsigned __int64 v14; // r14
  unsigned __int64 v15; // r15
  __int64 v16; // r21
  __int64 v17; // r22
  __int64 v18; // ctr
  unsigned __int128 v19; // r18
  unsigned __int64 v20; // r16
  unsigned __int128 v21; // r18
  unsigned __int64 v22; // r14
  unsigned __int64 v23; // r15
  unsigned __int64 *v24; // r9
  __int64 v25; // ctr
  unsigned __int64 v26; // r17
  unsigned __int64 *v27; // r9
  __int64 v28; // ctr
  unsigned __int64 v29; // r16
  unsigned __int64 v30; // r17
  unsigned __int64 v31; // r18
  unsigned __int64 v32; // r18
  __int64 *v33; // r9
  __int64 v34; // ctr
  __int64 v35; // r16
  __int64 v36; // r17
  __int64 v37; // r18
  __int64 v38; // r19
  unsigned __int64 v39; // r19
  unsigned __int64 v40; // r20
  _QWORD back_chain[10]; // [sp+0h] [-2B0h] BYREF
  __int64 v42; // [sp+50h] [-260h] BYREF
  _QWORD v43[65]; // [sp+58h] [-258h] BYREF

  v6 = 11;
  v7 = &v42;
  do
  {
    *v7 = 0;
    v7[1] = 0;
    v7[2] = 0;
    v7[3] = 0;
    v7[4] = 0;
    v7[5] = 0;
    v7 += 6;
    --v6;
  }
  while ( v6 != 0 );
  v8 = a4 * *result;
  v9 = a6;
  do
  {
    v10 = v43;
    v11 = *a2;
    v12 = v8 * *a2++;
    v13 = v12 + a4 * (v43[0] - v43[33]);
    v14 = 0;
    v15 = 0;
    v16 = 0;
    v17 = v9;
    v18 = a6;
    do
    {
      v19 = v11 * (unsigned __int128)(unsigned __int64)result[v16];
      *((_QWORD *)&v19 + 1) = v19 + *v10;
      if ( (unsigned __int64)v19 < *v10 )
        ++*(_QWORD *)&v19;
      *((_QWORD *)&v19 + 1) = v19 + v14;
      if ( (unsigned __int64)v19 < v14 )
        ++*(_QWORD *)&v19;
      v14 = *(_QWORD *)&v19;
      *(v10 - 1) = v19;
      v21 = v13 * (unsigned __int128)(unsigned __int64)a5[v16];
      v20 = v10[33];
      *((_QWORD *)&v21 + 1) = v21 + v20;
      if ( (unsigned __int64)v21 < v20 )
        ++*(_QWORD *)&v21;
      *((_QWORD *)&v21 + 1) = v21 + v15;
      if ( (unsigned __int64)v21 < v15 )
        ++*(_QWORD *)&v21;
      v15 = *(_QWORD *)&v21;
      v10[32] = v21;
      ++v10;
      ++v16;
      --v18;
    }
    while ( v18 != 0 );
    *(v10 - 1) = v14;
    v10[32] = *(_QWORD *)&v21;
    v9 = v17 - 1;
  }
  while ( v17 != 1 );
  v22 = 0;
  v23 = 0;
  v24 = &back_chain[a6 + 10];
  v25 = a6;
  while ( 1 )
  {
    v26 = v24[33];
    if ( *v24 < v26 )
      break;
    if ( *v24 <= v26 )
    {
      --v24;
      if ( --v25 != 0 )
        continue;
    }
    v27 = v43;
    v28 = a6;
    do
    {
      v29 = *v27;
      v30 = v27[33];
      v31 = *v27++ - v30;
      v32 = v31 - v22;
      *a3++ = v32;
      v22 = (v29 ^ (v32 ^ v29 | v30 ^ v29)) >> 63;
      --v28;
    }
    while ( v28 != 0 );
    return result;
  }
  v33 = v43;
  v34 = a6;
  do
  {
    v35 = *v33;
    v36 = v33[33];
    v37 = *a5;
    v38 = *v33++ + *a5++;
    v39 = v38 + v22;
    v40 = v39 - v36 - v23;
    *a3++ = v40;
    v22 = ((v35 ^ v39 | v37 ^ v39) ^ v39) >> 63;
    v23 = ((v36 ^ v39 | v40 ^ v39) ^ v39) >> 63;
    --v34;
  }
  while ( v34 != 0 );
  return result;
}


/* ========== [0x21128] XeCryptBnQwNeRsaPubCrypt (124 bytes) ========== */
__int64 __fastcall XeCryptBnQwNeRsaPubCrypt(__int64 a1, _QWORD *a2, _DWORD *a3)
{
  _QWORD *v4; // r27
  __int64 v5; // r28
  __int64 v6; // r31
  _QWORD v8[70]; // [sp+50h] [-230h] BYREF

  v4 = a2;
  v5 = (__int64)(a3 + 4);
  v6 = (unsigned int)*a3;
  if ( (unsigned int)(v6 - 1) > 0x3F )
    return 0;
  LODWORD(a3) = *a3;
  LODWORD(a2) = v5;
  if ( (int)sub_24D08(a1, a2, a3) >= 0 )
    return 0;
  XeCryptBnQw_Zero(v8, v6);
  return XeCryptBnQwNeModExp(v4, a1, (__int64)v8, v5, v6);
}


/* ========== [0x211a8] XeCryptBnQwNeRsaPrvCrypt (68 bytes) ========== */
__int64 __fastcall XeCryptBnQwNeRsaPrvCrypt(__int64 a1, __int64 a2, unsigned int *a3)
{
  __int64 v3; // r10
  __int64 v6; // r4
  __int64 v7; // r11
  __int64 v8; // r5

  v3 = *a3;
  LODWORD(v3) = (unsigned int)v3 >> 1;
  v6 = a1;
  v7 = 8LL * (unsigned int)v3;
  v8 = (__int64)&a3[2 * (unsigned int)(2 * v3) + 4];
  return XeCryptBnQwNeModExpRoot(
           a2,
           v6,
           v8,
           v7 + v8,
           v7 + v7 + v8,
           v7 + v7 + v7 + v8,
           (int)v7 + (int)v7 + (int)v7 + (int)v7 + (int)v8,
           v3);
}


/* ========== [0x211f0] XeCryptBnQw_Zero (32 bytes) ========== */
_QWORD *__fastcall XeCryptBnQw_Zero(_QWORD *result, __int64 a2)
{
  __int64 v2; // ctr

  v2 = a2;
  if ( (_DWORD)a2 != 0 )
  {
    do
    {
      *result++ = 0;
      --v2;
    }
    while ( v2 != 0 );
  }
  return result;
}


/* ========== [0x21210] XeCryptBnQw_Copy (44 bytes) ========== */
char *__fastcall XeCryptBnQw_Copy(char *result, char *a2, __int64 a3)
{
  bool v3; // cr58
  __int64 v4; // ctr
  signed __int64 v5; // r4

  v3 = result == a2;
  v4 = a3;
  v5 = a2 - result;
  if ( (_DWORD)a3 != 0 && !v3 )
  {
    do
    {
      *(_QWORD *)&result[v5] = *(_QWORD *)result;
      result += 8;
      --v4;
    }
    while ( v4 != 0 );
  }
  return result;
}


/* ========== [0x21240] XeCryptBnQw_SwapDwQwLeBe (48 bytes) ========== */
__int64 *__fastcall XeCryptBnQw_SwapDwQwLeBe(__int64 *result, __int64 a2, __int64 a3)
{
  __int64 v3; // r4
  __int64 v4; // ctr
  __int64 v5; // r7

  v3 = a2 - (_QWORD)result;
  v4 = a3;
  if ( (_DWORD)a3 != 0 )
  {
    do
    {
      v5 = *result;
      *(_DWORD *)((char *)result + v3) = __builtin_bswap32(*result);
      *(_DWORD *)((char *)result++ + v3 + 4) = __builtin_bswap32(__builtin_rotateleft64(v5, 32));
      --v4;
    }
    while ( v4 != 0 );
  }
  return result;
}


/* ========== [0x21270] XeCryptDesEcb (1180 bytes) ========== */
void __fastcall XeCryptDesEcb(const X_CRYPT_DES_STATE *state, const uint8_t *input, uint8_t *output, int encrypt)
{
  __int64 v4; // r2
  unsigned int v5; // r10
  __int64 v6; // r4
  unsigned int v7; // r11
  int v8; // r10
  unsigned int v9; // r9
  int v10; // r8
  int v11; // r10
  int v12; // r9
  unsigned int v13; // r11
  int v14; // r10
  int v15; // r9
  int v16; // r11
  int v17; // r9
  int v18; // r11
  int v19; // r10
  int v20; // r9
  int v21; // r11
  int v22; // r10
  unsigned int v23; // r9
  int v24; // r11
  int v25; // r9
  unsigned int v26; // r10
  __int64 v27; // r11
  __int64 v28; // r8
  const X_CRYPT_DES_STATE *v29; // r31
  unsigned __int64 v30; // r7
  unsigned __int64 v31; // r6
  unsigned __int64 v32; // r7
  unsigned __int64 v33; // r6
  __int64 v34; // r8
  uint64_t *v35; // r31
  unsigned __int64 v36; // r7
  unsigned __int64 v37; // r6
  unsigned __int64 v38; // r7
  unsigned __int64 v39; // r6
  int v40; // r11
  unsigned int v41; // r9
  int v42; // r10
  int v43; // r9
  int v44; // r11
  int v45; // r10
  int v46; // r9
  int v47; // r11
  int v48; // r9
  int v49; // r11
  int v50; // r10
  unsigned int v51; // r9
  int v52; // r11
  int v53; // r10
  unsigned int v54; // r9

  v5 = __builtin_bswap32(*(_DWORD *)input);
  v7 = __builtin_bswap32(*((_DWORD *)input + 1));
  v6 = 8;
  v8 = __builtin_rotateleft32(v5, 4);
  v9 = (v7 ^ v8) & 0xF0F0F0F0;
  v10 = v7 ^ v9;
  v12 = v8 ^ v9;
  v11 = __builtin_rotateright32(v10, 12);
  v13 = (v12 ^ v11) & 0xFFF0000F;
  v14 = __builtin_rotateleft32(v13 ^ v11, 14);
  v15 = v13 ^ v12;
  v16 = (v14 ^ v15) & 0x33333333;
  v17 = v15 ^ v16;
  v18 = v14 ^ v16;
  v19 = __builtin_rotateright32(v17, 10);
  v20 = (v19 ^ v18) & 0x3FC03FC;
  v21 = v20 ^ v18;
  v22 = __builtin_rotateleft32(v20 ^ v19, 9);
  v23 = (v21 ^ v22) & 0xAAAAAAAA;
  v24 = v21 ^ v23;
  v25 = v22 ^ v23;
  v26 = __builtin_rotateleft32(v24, 1);
  v27 = v4 + 85904;
  if ( encrypt != 0 )
  {
    v28 = 2;
    v29 = state;
    do
    {
      v30 = HIDWORD(v29->keytab[0]) ^ (unsigned int)v25;
      v31 = (unsigned int)__builtin_rotateright32(*((_DWORD *)state->keytab + (unsigned int)(v28 - 1)) ^ v25, 4);
      v26 ^= *(_DWORD *)(((v30 >> 24) & 0xFC) + v27 + 1536)
           ^ *(_DWORD *)(((v30 >> 16) & 0xFC) + v27 + 1024)
           ^ *(_DWORD *)(((v30 >> 8) & 0xFC) + v27 + 512)
           ^ *(_DWORD *)(((v31 >> 24) & 0xFC) + v27 + 1792)
           ^ *(_DWORD *)(((v31 >> 16) & 0xFC) + v27 + 1280)
           ^ *(_DWORD *)(((v31 >> 8) & 0xFC) + v27 + 768)
           ^ *(_DWORD *)(((unsigned __int8)v30 & 0xFC) + v27)
           ^ *(_DWORD *)(((unsigned __int8)v31 & 0xFC) + v27 + 256);
      v32 = *((_DWORD *)state->keytab + (unsigned int)v28) ^ v26;
      v33 = (unsigned int)__builtin_rotateright32(*((_DWORD *)state->keytab + (unsigned int)(v28 + 1)) ^ v26, 4);
      --v6;
      v29 = (const X_CRYPT_DES_STATE *)((char *)v29 + 16);
      v28 += 4;
      v25 ^= *(_DWORD *)(((v32 >> 24) & 0xFC) + v27 + 1536)
           ^ *(_DWORD *)(((v32 >> 16) & 0xFC) + v27 + 1024)
           ^ *(_DWORD *)(((v32 >> 8) & 0xFC) + v27 + 512)
           ^ *(_DWORD *)(((v33 >> 24) & 0xFC) + v27 + 1792)
           ^ *(_DWORD *)(((v33 >> 16) & 0xFC) + v4 + 87184)
           ^ *(_DWORD *)(((v33 >> 8) & 0xFC) + v4 + 86672)
           ^ *(_DWORD *)(((unsigned __int8)v32 & 0xFC) + v27)
           ^ *(_DWORD *)(((unsigned __int8)v33 & 0xFC) + v4 + 86160);
    }
    while ( (_DWORD)v6 != 0 );
  }
  else
  {
    v34 = 28;
    v35 = &state->keytab[15];
    do
    {
      v36 = *(_DWORD *)v35 ^ (unsigned int)v25;
      v37 = (unsigned int)__builtin_rotateright32(*((_DWORD *)&state->keytab[1] + (int)v34 + 1) ^ v25, 4);
      v26 ^= *(_DWORD *)(((v36 >> 24) & 0xFC) + v27 + 1536)
           ^ *(_DWORD *)(((v36 >> 16) & 0xFC) + v27 + 1024)
           ^ *(_DWORD *)(((v36 >> 8) & 0xFC) + v27 + 512)
           ^ *(_DWORD *)(((v37 >> 24) & 0xFC) + v27 + 1792)
           ^ *(_DWORD *)(((v37 >> 16) & 0xFC) + v27 + 1280)
           ^ *(_DWORD *)(((v37 >> 8) & 0xFC) + v27 + 768)
           ^ *(_DWORD *)(((unsigned __int8)v36 & 0xFC) + v27)
           ^ *(_DWORD *)(((unsigned __int8)v37 & 0xFC) + v27 + 256);
      v38 = *((_DWORD *)state->keytab + (int)v34) ^ v26;
      v39 = (unsigned int)__builtin_rotateright32(*((_DWORD *)state->keytab + (int)v34 + 1) ^ v26, 4);
      --v6;
      v35 -= 2;
      v34 -= 4;
      v25 ^= *(_DWORD *)(((v38 >> 24) & 0xFC) + v27 + 1536)
           ^ *(_DWORD *)(((v38 >> 16) & 0xFC) + v27 + 1024)
           ^ *(_DWORD *)(((v38 >> 8) & 0xFC) + v27 + 512)
           ^ *(_DWORD *)(((v39 >> 24) & 0xFC) + v4 + 87696)
           ^ *(_DWORD *)(((v39 >> 16) & 0xFC) + v27 + 1280)
           ^ *(_DWORD *)(((v39 >> 8) & 0xFC) + v4 + 86672)
           ^ *(_DWORD *)(((unsigned __int8)v38 & 0xFC) + v27)
           ^ *(_DWORD *)(((unsigned __int8)v39 & 0xFC) + v4 + 86160);
    }
    while ( (_DWORD)v6 != 0 );
  }
  v40 = __builtin_rotateright32(v25, 1);
  v41 = (v26 ^ v40) & 0xAAAAAAAA;
  v42 = __builtin_rotateright32(v41 ^ v26, 9);
  v43 = v41 ^ v40;
  v44 = (v42 ^ v43) & 0x3FC03FC;
  v45 = __builtin_rotateleft32(v42 ^ v44, 10);
  v46 = v43 ^ v44;
  v47 = (v46 ^ v45) & 0x33333333;
  v48 = v47 ^ v46;
  v49 = v47 ^ v45;
  v50 = __builtin_rotateright32(v48, 14);
  v51 = (v49 ^ v50) & 0xFFF0000F;
  v52 = v49 ^ v51;
  v53 = __builtin_rotateleft32(v50 ^ v51, 12);
  v54 = (v53 ^ v52) & 0xF0F0F0F0;
  *(_DWORD *)output = __builtin_bswap32(__builtin_rotateright32(v54 ^ v52, 4));
  *((_DWORD *)output + 1) = ((unsigned __int8)(v54 ^ v53) << 24)
                          | ((v54 ^ v53) << 8) & 0xFF0000
                          | (unsigned __int16)(((unsigned __int16)v54 ^ (unsigned __int16)v53) << 8)
                          | ((v54 ^ v53) >> 24);
}


/* ========== [0x21710] XeCryptDesCbc (176 bytes) ========== */
void __fastcall XeCryptDesCbc(
        const X_CRYPT_DES_STATE *state,
        const uint8_t *input,
        uint32_t size,
        uint8_t *output,
        uint8_t *feed,
        int encrypt)
{
  uint32_t v9; // r31
  __int64 v10; // r28
  __int64 v11; // r27
  __int64 v12; // [sp+50h] [-50h] BYREF
  __int64 v13; // [sp+58h] [-48h] BYREF

  v9 = size >> 3;
  v10 = *(_QWORD *)feed;
  if ( encrypt != 0 )
  {
    while ( v9 != 0 )
    {
      XeCryptDesEcb(state, (const uint8_t *)&v12, (uint8_t *)&v12, 1);
      v10 = v12;
      --v9;
      input += 8;
      *(_QWORD *)output = v12;
      output += 8;
    }
  }
  else
  {
    while ( v9 != 0 )
    {
      v11 = *(_QWORD *)input;
      XeCryptDesEcb(state, (const uint8_t *)&v12, (uint8_t *)&v13, 0);
      --v9;
      input += 8;
      *(_QWORD *)output = v13 ^ v10;
      v10 = v11;
      output += 8;
    }
  }
  *(_QWORD *)feed = v10;
}


/* ========== [0x217c0] XeCryptDes3Ecb (128 bytes) ========== */
void __fastcall XeCryptDes3Ecb(const X_CRYPT_DES3_STATE *state, const uint8_t *input, uint8_t *output, int encrypt)
{
  uint8_t *v5; // r5
  uint8_t *v6; // r5
  int v7; // r6
  const X_CRYPT_DES3_STATE *v8; // r3
  uint8_t *v9; // r5

  if ( encrypt != 0 )
  {
    XeCryptDesEcb(state->des_state, input, output, 1);
    XeCryptDesEcb(&state->des_state[1], v5, v5, 0);
    v7 = 1;
    v8 = (const X_CRYPT_DES3_STATE *)&state->des_state[2];
  }
  else
  {
    XeCryptDesEcb(&state->des_state[2], input, output, 0);
    XeCryptDesEcb(&state->des_state[1], v9, v9, 1);
    v7 = 0;
    v8 = state;
  }
  XeCryptDesEcb(v8->des_state, v6, v6, v7);
}


/* ========== [0x21840] XeCryptDes3Cbc (296 bytes) ========== */
void __fastcall XeCryptDes3Cbc(
        const X_CRYPT_DES3_STATE *state,
        const uint8_t *input,
        uint32_t size,
        uint8_t *output,
        uint8_t *feed,
        int encrypt)
{
  __int64 v6; // r31
  __int64 v11; // r27
  __int64 v12; // r26
  __int64 v13; // [sp+50h] [-60h] BYREF
  __int64 v14; // [sp+58h] [-58h] BYREF

  LODWORD(v6) = size >> 3;
  v11 = *(_QWORD *)feed;
  if ( encrypt != 0 )
  {
    for ( ; (_DWORD)v6 != 0; output += 8 )
    {
      XeCryptDesEcb(state->des_state, (const uint8_t *)&v13, (uint8_t *)&v13, 1);
      XeCryptDesEcb(&state->des_state[1], (const uint8_t *)&v13, (uint8_t *)&v13, 0);
      XeCryptDesEcb(&state->des_state[2], (const uint8_t *)&v13, (uint8_t *)&v13, 1);
      v11 = v13;
      --v6;
      input += 8;
      *(_QWORD *)output = v13;
    }
  }
  else
  {
    for ( ; (_DWORD)v6 != 0; output += 8 )
    {
      v12 = *(_QWORD *)input;
      XeCryptDesEcb(&state->des_state[2], (const uint8_t *)&v14, (uint8_t *)&v13, 0);
      XeCryptDesEcb(&state->des_state[1], (const uint8_t *)&v13, (uint8_t *)&v13, 1);
      XeCryptDesEcb(state->des_state, (const uint8_t *)&v13, (uint8_t *)&v13, 0);
      --v6;
      input += 8;
      *(_QWORD *)output = v13 ^ v11;
      v11 = v12;
    }
  }
  *(_QWORD *)feed = v11;
}


/* ========== [0x21968] XeCryptDesKey (608 bytes) ========== */
void __fastcall XeCryptDesKey(X_CRYPT_DES_STATE *state, const uint64_t *key)
{
  __int64 v2; // r2
  _BYTE *v4; // r3
  unsigned int v5; // r11
  unsigned int v6; // r10
  __int64 v7; // r5
  int v8; // r9
  unsigned int v9; // r11
  unsigned int v10; // r10
  int v11; // r9
  unsigned int v12; // r11
  int v13; // r10
  int v14; // r9
  unsigned int v15; // r10
  int v16; // r11
  int v17; // r9
  int v18; // r8
  int v19; // r9
  int v20; // r7
  __int16 v21; // t0
  int v22; // r11
  unsigned int v23; // r8
  unsigned __int64 v24; // r7
  unsigned int v25; // r10
  unsigned int v26; // r9
  int v27; // r8
  unsigned int v28; // r7
  int v29; // r9
  int v30; // r10

  v4 = (_BYTE *)(v2 + 90000);
  v5 = __builtin_bswap32(*(_DWORD *)key);
  v6 = __builtin_bswap32(*((_DWORD *)key + 1));
  v7 = 16;
  v8 = ((v6 >> 4) ^ v5) & 0xF0F0F0F;
  v9 = (((((v8 ^ v5) << 18) ^ v8 ^ v5) & 0xCCCC0000) >> 18) ^ (((v8 ^ v5) << 18) ^ v8 ^ v5) & 0xCCCC0000 ^ v8 ^ v5;
  v10 = ((((((16 * v8) ^ v6) << 18) ^ (16 * v8) ^ v6) & 0xCCCC0000) >> 18)
      ^ ((((16 * v8) ^ v6) << 18)
       ^ (16 * v8)
       ^ v6)
      & 0xCCCC0000
      ^ (16 * v8)
      ^ v6;
  v11 = ((v10 >> 1) ^ v9) & 0x55555555;
  v12 = v11 ^ v9;
  v13 = (2 * v11) ^ v10;
  v14 = ((v12 >> 8) ^ v13) & 0xFF00FF;
  v15 = v14 ^ v13;
  v16 = (v14 << 8) ^ v12;
  v17 = ((v15 >> 1) ^ v16) & 0x55555555;
  v18 = 2 * v17;
  v20 = (unsigned __int8)((2 * v17) ^ v15) << 16;
  v21 = __builtin_rotateright32((2 * v17) ^ v15, 12);
  v19 = v17 ^ v16;
  v22 = (v18 ^ v15) & 0xFF00;
  v23 = v19 & 0xFFFFFFF;
  LODWORD(v24) = ((v21 & 0xFF0 | v19 & 0xFFFFF00F) >> 4) & 0xF0000FF | v20 | v22;
  do
  {
    if ( *v4 != 0 )
    {
      v25 = v23 << 26;
      v26 = v23 >> 2;
      v27 = (_DWORD)v24 << 26;
      v28 = (unsigned int)v24 >> 2;
    }
    else
    {
      v25 = v23 << 27;
      v26 = v23 >> 1;
      v27 = (_DWORD)v24 << 27;
      v28 = (unsigned int)v24 >> 1;
    }
    v24 = (v27 | v28) & 0xFFFFFFF;
    v23 = (v25 | v26) & 0xFFFFFFF;
    --v7;
    ++v4;
    v29 = *(_DWORD *)((((((((unsigned __int64)v23 >> 1) & 0x7000000 | v23 & 0xC00000LL) >> 1) | v23 & 0x100000LL) >> 18)
                     & 0xFC)
                    + v2
                    + 88720)
        | *(_DWORD *)((((((unsigned __int64)v23 >> 1) & 0xF00 | (unsigned __int8)v23 & 0xC0) >> 4) & 0xFC) + v2 + 88208)
        | *(_DWORD *)((((((unsigned __int64)v23 >> 1) & 0x60000 | v23 & 0x1E000LL) >> 11) & 0xFC) + v2 + 88464)
        | *(_DWORD *)(((4LL * v23) & 0xFC) + v2 + 87952);
    v30 = *(_DWORD *)(((((v24 >> 1) & 0x6000000 | v24 & 0x1E00000) >> 19) & 0xFC) + v2 + 89744)
        | *(_DWORD *)(((((v24 >> 1) & 0x1E00 | v24 & 0x180) >> 5) & 0xFC) + v2 + 89232)
        | *(_DWORD *)(((v24 >> 13) & 0xFC) + v2 + 89488)
        | *(_DWORD *)(((4 * v24) & 0xFC) + v2 + 88976);
    HIDWORD(state->keytab[0]) = __builtin_rotateleft32(((unsigned __int16)v30 << 16) | (unsigned __int16)v29, 2);
    LODWORD(state->keytab[0]) = __builtin_rotateleft32(HIWORD(v29) | v30 & 0xFFFF0000, 6);
    state = (X_CRYPT_DES_STATE *)((char *)state + 8);
  }
  while ( (_DWORD)v7 != 0 );
}


/* ========== [0x21bc8] XeCryptDes3Key (64 bytes) ========== */
void __fastcall XeCryptDes3Key(X_CRYPT_DES3_STATE *state, const uint64_t *keys)
{
  __int64 v4; // r31

  v4 = 3;
  do
  {
    XeCryptDesKey(state->des_state, keys);
    --v4;
    state = (X_CRYPT_DES3_STATE *)((char *)state + 128);
    ++keys;
  }
  while ( (_DWORD)v4 != 0 );
}


/* ========== [0x21c08] XeCryptDesParity (104 bytes) ========== */
void __fastcall XeCryptDesParity(uint8_t *input, __int64 size, uint8_t *output)
{
  __int64 v3; // r2
  uint8_t v4; // r10
  unsigned __int64 v5; // r11

  for ( ; (_DWORD)size != 0; ++output )
  {
    v4 = *input;
    v5 = *input++;
    LODWORD(v5) = *(unsigned __int8 *)((v5 >> 4) + v3 + 90016) + *(unsigned __int8 *)((v5 & 0xF) + v3 + 90016);
    if ( (_DWORD)v5 == 2 * ((int)v5 >> 1) )
      v4 ^= 1u;
    --size;
    *output = v4;
  }
}


/* ========== [0x21c70] sub_21C70 (728 bytes) ========== */
__int64 __fastcall sub_21C70(char a1, char a2)
{
  bool v2; // r8
  bool v3; // r6
  bool v5; // r31
  bool v6; // r30
  bool v7; // r29
  bool v8; // r28
  bool v9; // r27
  bool v10; // r26
  bool v11; // r25
  bool v12; // r11
  bool v13; // r10
  bool v14; // r8
  bool v15; // r9
  bool v16; // r7
  unsigned __int8 v17; // r6
  unsigned __int8 v18; // r5
  bool v19; // r3
  bool v20; // r30
  bool v21; // r28
  bool v22; // r22
  char v23; // r26
  bool v24; // xer_ca
  char v25; // r21
  __int64 v26; // r24

  v2 = (a1 & 0x20) != 0;
  v3 = (a2 & 2) != 0;
  v5 = (a1 & 2) != 0;
  v6 = (a2 & 0x40) != 0;
  v7 = (a1 & 4) != 0;
  v8 = (a2 & 0x20) != 0;
  v9 = (a1 & 8) != 0;
  v10 = (a2 & 0x10) != 0;
  v11 = (a2 & 8) != 0;
  v12 = (a1 & 0x10) != 0;
  v15 = (a1 & 0x40) != 0;
  v16 = (a2 & 4) != 0;
  v18 = a2 & 1;
  v13 = v2;
  v19 = v6;
  v22 = v10 && v12;
  v24 = ((v11 && v12) ^ (v16 && v2) ^ (v3 && v15) ^ (v6 && v5) ^ (v8 && v7) ^ (v10 && v9)) == 0;
  v20 = v8;
  v25 = (v7 && v16) ^ (v9 && v3) ^ v12 & v18;
  v26 = (v9 && v16) ^ (v12 && v3) ^ v2 & v18 ^ a1 & 1u & v8 ^ (v5 && v10) ^ (v7 && v11);
  v21 = v10;
  v23 = (v12 && v16) ^ (v2 && v3) ^ v15 & v18 ^ a1 & 1 & v19 ^ (v5 && v20) ^ (v7 && v10) ^ (v9 && v11);
  v14 = v3;
  v17 = a1 & 1;
  return (unsigned __int8)(((v26 == 0 ? 0 : 0x20) | (v24 ? 0 : 3) | (v23 == 0 ? 0 : 0x40))
                         ^ ((v22 ^ (v11 && v13) ^ (v16 && v15) ^ (v19 && v7) ^ (v20 && v9)) == 0 ? 0 : 6)
                         ^ ((unsigned __int8)(v25 ^ v17 & v21 ^ (v5 && v11)) == 0 ? 0 : 0x10)
                         ^ (((v21 && v13) ^ (v11 && v15) ^ (v19 && v9) ^ (v20 && v12)) == 0 ? 0 : 0xC)
                         ^ (((v7 && v14) ^ v9 & v18 ^ v17 & v11 ^ (v5 && v16)) == 0 ? 0 : 8)
                         ^ (((v20 && v13) ^ (v21 && v15) ^ (v19 && v12)) == 0 ? 0 : 0x18)
                         ^ (((v5 && v14) ^ v7 & v18 ^ v17 & v16) == 0 ? 0 : 4)
                         ^ ((v19 && v13) == (v20 && v15) ? 0 : 0x30)
                         ^ ((v17 & v14) == (v5 & v18) ? 0 : 2)
                         ^ (!v19 || !v15 ? 0 : 0x60))
       ^ ((_cntlzw((unsigned __int8)(v17 & v18)) & 0x20) == 0);
}


/* ========== [0x21f48] sub_21F48 (208 bytes) ========== */
__int64 __fastcall sub_21F48(__int64 result)
{
  unsigned __int64 v1; // r10
  __int64 v2; // r6
  unsigned __int64 i; // r11
  unsigned __int64 v4; // r9
  unsigned __int64 v5; // r10
  unsigned __int64 v6; // r7
  unsigned __int64 v7; // r31
  char v8; // r10
  unsigned __int64 v9; // r11
  unsigned __int64 v10; // r8

  v1 = 0;
  v2 = 0;
  for ( i = 0; i < 0x6A; ++i )
  {
    v4 = (*(unsigned __int8 *)((i >> 3) + result) >> (i & 7)) & 1LL;
    v5 = v4 ^ v1;
    if ( (v5 & 1) != 0 )
      v5 ^= 0x360325u;
    v1 = v5 >> 1;
    v2 ^= v4;
  }
  while ( i < 0x7F )
  {
    v6 = v1 & 1;
    v7 = *(unsigned __int8 *)((i >> 3) + result);
    if ( ((v7 >> (i & 7)) & 1) != v6 )
      *(_BYTE *)((i >> 3) + result) = (1 << (i & 7)) ^ v7;
    ++i;
    v1 >>= 1;
    v2 ^= v6;
  }
  v8 = i & 7;
  v9 = i >> 3;
  v10 = *(unsigned __int8 *)(v9 + result);
  if ( ((v10 >> v8) & 1) != v2 )
    *(_BYTE *)(v9 + result) = (1 << v8) ^ v10;
  return result;
}


/* ========== [0x22018] sub_22018 (1712 bytes) ========== */
__int64 __fastcall sub_22018(__int64 a1)
{
  char v2; // r24
  char v3; // r9
  char v4; // r23
  char v5; // r22
  unsigned __int64 i; // r7
  unsigned __int8 v7; // r11
  __int64 v8; // r29
  __int64 v9; // r31
  __int64 v10; // r30
  __int64 v11; // r5
  __int64 v12; // r28
  bool v13; // r11
  char v14; // r28
  char v15; // r27
  char v16; // r29
  unsigned int v17; // r31
  __int64 result; // r3
  char v19; // r3
  unsigned __int8 v20; // r11
  int v21; // r28
  unsigned __int8 v22; // r27
  unsigned __int8 v23; // r31
  unsigned __int8 v24; // r11
  __int64 v25; // r9
  char v26; // r11
  unsigned int v27; // r6
  unsigned __int64 v28; // r8
  unsigned __int8 v29; // r11

  v2 = 0;
  v3 = 0;
  v4 = 0;
  v5 = 0;
  for ( i = 0; i < 0x80; i += 8LL )
  {
    v7 = *(_BYTE *)((i >> 3) + a1);
    v8 = v7 & 2;
    v9 = v7 & 4;
    v10 = v7 & 0x20;
    v11 = v7 & 0x10;
    v12 = v7 & 8;
    v2 = ((unsigned __int8)(((unsigned __int8)(((unsigned __int8)((v7 >> 2) ^ v7 & 0x40) >> 2) ^ v7 & 0x20) >> 2)
                          ^ v7
                          & 0x10) >> 1)
       ^ (2 * ((4 * ((4 * (v7 & 2)) ^ v7 & 4)) ^ v7 & 8))
       ^ ((v2 & 0x10) == 0 ? 0 : 0x60)
       ^ ((v2 & 0x20) == 0 ? 0 : 0x43)
       ^ ((v2 & 0x40) == 0 ? 0 : 5)
       ^ ((v2 & 8) == 0 ? 0 : 0x30)
       ^ ((v2 & 4) == 0 ? 0 : 0x18)
       ^ ((v2 & 2) == 0 ? 0 : 0xC)
       ^ ((v2 & 1) == 0 ? 0 : 6)
       ^ ((v7 & 1) == 0 ? 0 : 3);
    v3 = ((v3 & 0x40) == 0 ? 0 : 0x44)
       ^ ((v3 & 1) == 0 ? 0 : 0x78)
       ^ ((v3 & 2) == 0 ? 0 : 0x73)
       ^ ((v3 & 4) == 0 ? 0 : 0x65)
       ^ ((v3 & 8) == 0 ? 0 : 0x49)
       ^ ((v3 & 0x20) == 0 ? 0 : 0x22)
       ^ ((v3 & 0x10) == 0 ? 0 : 0x11)
       ^ ((unsigned __int8)((v7 >> 4) ^ v7 & 0x40) >> 3)
       ^ (v9 == 0 ? 0 : 0xA)
       ^ (v11 == 0 ? 0 : 0xC)
       ^ (v8 == 0 ? 0 : 0x50)
       ^ (v12 == 0 ? 0 : 0x60)
       ^ ((v7 & 1) == 0 ? 0 : 0xF)
       ^ (2 * v10);
    v4 = ((v4 & 0x40) == 0 ? 0 : 0x4E)
       ^ ((v4 & 1) == 0 ? 0 : 0x74)
       ^ ((v4 & 2) == 0 ? 0 : 0x6B)
       ^ ((v4 & 0x10) == 0 ? 0 : 0x52)
       ^ ((v4 & 4) == 0 ? 0 : 0x55)
       ^ ((v4 & 0x20) == 0 ? 0 : 0x27)
       ^ ((v4 & 8) == 0 ? 0 : 0x29)
       ^ ((unsigned __int8)((v7 >> 6) ^ v7 & 0x40) >> 1)
       ^ (v10 == 0 ? 0 : 0x18)
       ^ (v9 == 0 ? 0 : 0x73)
       ^ (v11 == 0 ? 0 : 0xA)
       ^ (v8 == 0 ? 0 : 0x44)
       ^ (v12 == 0 ? 0 : 0x46)
       ^ ((v7 & 1) == 0 ? 0 : 0x33);
    v5 ^= ((unsigned __int8)(((unsigned __int8)(((unsigned __int8)(((unsigned __int8)v11 >> 1) ^ v12) >> 1) ^ v9) >> 1)
                           ^ v8) >> 1)
        ^ (((unsigned __int8)(((unsigned __int8)((v7 >> 1) ^ v7) >> 1) ^ v7) >> 5)
         ^ v7)
        & 1;
  }
  v13 = (v7 & 0x80) != 0;
  v14 = v13 ^ v3;
  v15 = v13 ^ v4;
  v16 = v13 ^ v2;
  v17 = (unsigned __int8)(v13 ^ v2);
  if ( ((unsigned __int8)(v5 | v13 ^ v4 | v13 ^ v3) | (unsigned __int8)v17) == 0 )
    return 0;
  v20 = sub_21C70(
          v16,
          (2 * ((2 * ((2 * v17) & 0x10 | v17 & 0xEF)) & 0x28 ^ v17 & 2))
        ^ ((v17 & 0x40) == 0 ? 0 : 0x60)
        ^ ((v17 & 0x20) == 0 ? 0 : 0x18)
        ^ ((v17 & 0x10) == 0 ? 0 : 6)
        ^ v17
        & 1)
      ^ v13
      ^ v3;
  v19 = v14;
  v21 = v20;
  v22 = sub_21C70(
          v19,
          (2 * ((2 * ((2 * v17) & 0x10 | v17 & 0xEF)) & 0x28 ^ v17 & 2))
        ^ ((v17 & 0x40) == 0 ? 0 : 0x60)
        ^ ((v17 & 0x20) == 0 ? 0 : 0x18)
        ^ ((v17 & 0x10) == 0 ? 0 : 6)
        ^ v17
        & 1)
      ^ v15;
  if ( v21 != 0 )
  {
    v23 = sub_21C70(v16, v21);
    v24 = (2 * ((2 * ((2 * v21) & 0x10 | v21 & 0xEF)) & 0x28 ^ v21 & 2))
        ^ ((v21 & 0x40) == 0 ? 0 : 0x60)
        ^ ((v21 & 0x20) == 0 ? 0 : 0x18)
        ^ ((v21 & 0x10) == 0 ? 0 : 6)
        ^ v21
        & 1
        ^ sub_21C70(v16, v22);
    v25 = ((_cntlzw(v24) & 0x20) == 0) + 2LL;
  }
  else
  {
    if ( v22 != 0 )
      return -1;
    v26 = _cntlzw(v17);
    LOBYTE(v21) = 1;
    v23 = v16;
    v25 = (v26 & 0x20) == 0;
    v24 = 0;
  }
  v27 = v25;
  if ( (v25 & 1) != v5 )
  {
    if ( v25 == 3 )
      return -1;
    v27 = v25 + 1;
    *(_BYTE *)(a1 + 15) ^= 0x80u;
  }
  v28 = 127;
  do
  {
    LOBYTE(v21) = 2 * v21;
    if ( (v21 & 0x80) != 0 )
      LOBYTE(v21) = v21 ^ 0x83;
    if ( (v22 & 1) != 0 )
      v22 ^= 0x83u;
    v22 >>= 1;
    if ( (v24 & 1) != 0 )
      v24 ^= 0x83u;
    v29 = v24 >> 1;
    if ( (v29 & 1) != 0 )
      v29 ^= 0x83u;
    v24 = v29 >> 1;
    --v28;
    if ( ((unsigned __int8)(v24 ^ v22 ^ v21) ^ v23) == 0 )
    {
      --v25;
      *(_BYTE *)((v28 >> 3) + a1) ^= 1 << (v28 & 7);
    }
  }
  while ( v28 != 0 );
  result = -1;
  if ( v25 == 0 )
    return v27;
  return result;
}


/* ========== [0x226c8] sub_226C8 (36 bytes) ========== */
__int64 __fastcall sub_226C8(unsigned __int64 a1)
{
  unsigned int v1; // r11

  v1 = 0;
  while ( a1 != 0 )
  {
    v1 += a1 & 1;
    a1 >>= 1;
  }
  return v1;
}


/* ========== [0x226f0] XeCryptShaInit (72 bytes) ========== */
void __fastcall XeCryptShaInit(X_CRYPT_SHA_STATE *state)
{
  *(_QWORD *)&state->count = 1732584193;
  state->state[1] = 0xEFCDAB89;
  state->state[2] = 0x98BADCFE;
  state->state[3] = 271733878;
  state->state[4] = 0xC3D2E1F0;
}


/* ========== [0x22738] XeCryptShaTransform (1176 bytes) ========== */
void __fastcall XeCryptShaTransform(X_CRYPT_SHA_STATE *state, char *a2)
{
  unsigned int *v3; // r26
  __int64 count; // r27
  __int64 v5; // r28
  __int64 v6; // r29
  __int64 v7; // r30
  __int64 v8; // r31
  __int64 v9; // r11
  __int16 *v10; // r9
  __int64 v11; // r10
  __int64 v12; // r4
  int v13; // r8
  int v14; // r7
  unsigned int *v15; // r10
  __int64 v16; // r6
  __int64 v17; // r4
  __int64 v18; // r9
  __int64 v19; // r25
  __int64 v20; // r3
  __int64 v21; // r7
  __int64 v22; // r8
  __int64 v23; // r28
  __int64 v24; // r7
  __int64 v25; // r4
  __int64 v26; // r7
  unsigned int *v27; // r10
  __int64 v28; // r6
  __int64 v29; // r7
  __int64 v30; // r3
  __int64 v31; // r9
  __int64 v32; // r4
  __int64 v33; // r8
  __int64 v34; // r28
  __int64 v35; // r8
  __int64 v36; // r7
  __int64 v37; // r27
  __int64 v38; // r3
  __int64 v39; // r8
  __int64 v40; // r4
  __int64 v41; // r8
  __int64 v42; // r5
  __int64 v43; // r8
  unsigned int *v44; // r10
  __int64 v45; // r6
  __int64 v46; // r4
  __int64 v47; // r25
  __int64 v48; // r3
  __int64 v49; // r9
  __int64 v50; // r8
  __int64 v51; // r7
  __int64 v52; // r28
  __int64 v53; // r8
  __int64 v54; // r4
  __int64 v55; // r8
  unsigned int *v56; // r10
  __int64 v57; // r6
  __int64 v58; // r7
  __int64 v59; // r3
  __int64 v60; // r9
  __int64 v61; // r4
  __int64 v62; // r8
  __int64 v63; // r28
  __int64 v64; // r8
  __int64 v65; // r7
  __int64 v66; // r27
  __int64 v67; // r3
  __int64 v68; // r8
  __int64 v69; // r4
  __int64 v70; // r8
  __int64 v71; // r5
  __int64 v72; // r8
  uint32_t v73; // r7
  uint32_t v74; // r9
  uint32_t v75; // r10
  uint32_t v76; // r11
  _WORD v77[4]; // [sp+50h] [-190h] BYREF
  __int16 v78; // [sp+58h] [-188h] BYREF
  __int16 v79; // [sp+90h] [-150h] BYREF

  v3 = (unsigned int *)v77;
  count = state->count;
  v5 = state->state[0];
  v6 = state->state[1];
  v7 = state->state[2];
  v8 = state->state[3];
  XeCryptBnDw_Copy(a2, (char *)v77, 16);
  v9 = 2;
  v10 = &v79;
  v11 = 64;
  do
  {
    v12 = 2LL * (int)v9;
    v13 = *(_DWORD *)&v77[2 * (int)v9 + 22] ^ *(_DWORD *)&v77[2 * (int)v9 + 12];
    --v11;
    v14 = *(_DWORD *)&v77[2 * (int)v9++ - 4];
    *(_DWORD *)v10 = __builtin_rotateleft32(v13 ^ v14 ^ *(_DWORD *)&v77[v12], 1);
    v10 += 2;
  }
  while ( (_DWORD)v11 != 0 );
  v15 = (unsigned int *)&v78;
  v16 = 4;
  do
  {
    v17 = v15[1];
    v18 = (unsigned int)__builtin_rotateright32(v5, 2);
    v19 = *v15;
    v20 = v15[2];
    --v16;
    v21 = ((v7 ^ v6) & v5 ^ v7) + (unsigned int)__builtin_rotateleft32(count, 5) + v8 + *v3 + 1518500249;
    v22 = (unsigned int)__builtin_rotateright32(count, 2);
    v23 = (v18 ^ v22) & v21;
    v8 = (unsigned int)__builtin_rotateright32(v21, 2);
    v24 = ((v6 ^ v18) & count ^ v6) + *(v15 - 1) + (unsigned int)__builtin_rotateleft32(v21, 5) + v7 + 1518500249;
    v3 += 5;
    v7 = (unsigned int)__builtin_rotateright32(v24, 2);
    v15 += 5;
    v25 = ((v8 ^ v22) & v24 ^ v22) + v17;
    v26 = (v23 ^ v18) + (unsigned int)__builtin_rotateleft32(v24, 5) + v6 + v19 + 1518500249;
    v6 = (unsigned int)__builtin_rotateright32(v26, 2);
    v5 = v25 + (unsigned int)__builtin_rotateleft32(v26, 5) + v18 + 1518500249;
    count = ((v8 ^ v7) & v26 ^ v8) + v20 + (unsigned int)__builtin_rotateleft32(v5, 5) + v22 + 1518500249;
  }
  while ( (_DWORD)v16 != 0 );
  v27 = v3 + 2;
  v28 = 4;
  do
  {
    v29 = *v3;
    v30 = *v27;
    v31 = (unsigned int)__builtin_rotateright32(v5, 2);
    v32 = v27[1];
    v33 = (v7 ^ v6 ^ v5) + (unsigned int)__builtin_rotateleft32(count, 5);
    v34 = v27[2];
    --v28;
    v3 += 5;
    v35 = v33 + v8 + v29 + 1859775393;
    v36 = (unsigned int)__builtin_rotateright32(count, 2);
    v37 = (v6 ^ v31 ^ count) + *(v27 - 1);
    v8 = (unsigned int)__builtin_rotateright32(v35, 2);
    v27 += 5;
    v38 = (v35 ^ v31 ^ v36) + v30;
    v39 = v37 + (unsigned int)__builtin_rotateleft32(v35, 5) + v7 + 1859775393;
    v7 = (unsigned int)__builtin_rotateright32(v39, 2);
    v40 = (v8 ^ v39 ^ v36) + v32;
    v41 = v38 + (unsigned int)__builtin_rotateleft32(v39, 5) + v6 + 1859775393;
    v42 = v40 + (unsigned int)__builtin_rotateleft32(v41, 5);
    v6 = (unsigned int)__builtin_rotateright32(v41, 2);
    v43 = (v8 ^ v7 ^ v41) + v34;
    v5 = v42 + v31 + 1859775393;
    count = v43 + (unsigned int)__builtin_rotateleft32(v5, 5) + v36 + 1859775393;
  }
  while ( (_DWORD)v28 != 0 );
  v44 = v3 + 2;
  v45 = 4;
  do
  {
    v46 = v44[1];
    v47 = *v44;
    v48 = v44[2];
    v49 = (unsigned int)__builtin_rotateright32(v5, 2);
    --v45;
    v50 = ((v6 | v5) & v7 | v6 & v5) + (unsigned int)__builtin_rotateleft32(count, 5) + v8 + *v3 - 1894007588;
    v51 = (unsigned int)__builtin_rotateright32(count, 2);
    v8 = (unsigned int)__builtin_rotateright32(v50, 2);
    v52 = (v50 | v51) & v49 | v50 & v51;
    v3 += 5;
    v53 = ((v49 | count) & v6 | v49 & count)
        + *(v44 - 1)
        + (unsigned int)__builtin_rotateleft32(v50, 5)
        + v7
        - 1894007588;
    v44 += 5;
    v7 = (unsigned int)__builtin_rotateright32(v53, 2);
    v54 = ((v8 | v53) & v51 | v8 & v53) + v46;
    v55 = v52 + (unsigned int)__builtin_rotateleft32(v53, 5) + v6 + v47 - 1894007588;
    v6 = (unsigned int)__builtin_rotateright32(v55, 2);
    v5 = v54 + (unsigned int)__builtin_rotateleft32(v55, 5) + v49 - 1894007588;
    count = ((v7 | v55) & v8 | v7 & v55) + v48 + (unsigned int)__builtin_rotateleft32(v5, 5) + v51 - 1894007588;
  }
  while ( (_DWORD)v45 != 0 );
  v56 = v3 + 2;
  v57 = 4;
  do
  {
    v58 = *v3;
    v59 = *v56;
    v60 = (unsigned int)__builtin_rotateright32(v5, 2);
    v61 = v56[1];
    v62 = (v7 ^ v6 ^ v5) + (unsigned int)__builtin_rotateleft32(count, 5);
    v63 = v56[2];
    --v57;
    v3 += 5;
    v64 = v62 + v8 + v58 - 899497514;
    v65 = (unsigned int)__builtin_rotateright32(count, 2);
    v66 = (v6 ^ v60 ^ count) + *(v56 - 1);
    v8 = (unsigned int)__builtin_rotateright32(v64, 2);
    v56 += 5;
    v67 = (v64 ^ v60 ^ v65) + v59;
    v68 = v66 + (unsigned int)__builtin_rotateleft32(v64, 5) + v7 - 899497514;
    v7 = (unsigned int)__builtin_rotateright32(v68, 2);
    v69 = (v8 ^ v68 ^ v65) + v61;
    v70 = v67 + (unsigned int)__builtin_rotateleft32(v68, 5) + v6 - 899497514;
    v71 = v69 + (unsigned int)__builtin_rotateleft32(v70, 5);
    v6 = (unsigned int)__builtin_rotateright32(v70, 2);
    v72 = (v8 ^ v7 ^ v70) + v63;
    v5 = v71 + v60 - 899497514;
    count = v72 + (unsigned int)__builtin_rotateleft32(v5, 5) + v65 - 899497514;
  }
  while ( (_DWORD)v57 != 0 );
  v73 = state->state[0] + v5;
  v74 = state->state[1] + v6;
  v75 = v7 + state->state[2];
  v76 = state->state[3] + v8;
  state->count += count;
  state->state[0] = v73;
  state->state[1] = v74;
  state->state[2] = v75;
  state->state[3] = v76;
}


/* ========== [0x22bd0] XeCryptShaUpdate (284 bytes) ========== */
void __fastcall XeCryptShaUpdate(X_CRYPT_SHA_STATE *state, const uint8_t *input, __int64 input_size)
{
  __int64 v3; // r27
  unsigned int v5; // r31
  char *v6; // r30
  uint32_t count; // r11
  __int64 v8; // r28

  v5 = input_size;
  v6 = (char *)input;
  count = state->count;
  v8 = state->count & 0x3FLL;
  state->count += input_size;
  if ( (count & 0x3F) != 0 )
  {
    v3 = v8 + input_size;
    if ( (unsigned int)(v8 + input_size) >= 0x40 )
    {
      memcpy(&state->buffer[(unsigned int)v8], input);
      v6 += (unsigned int)(64 - v8);
      v5 = v3 - 64;
      XeCryptShaTransform((X_CRYPT_SHA_STATE *)state->state, (char *)state->buffer);
      LODWORD(v8) = 0;
    }
  }
  if ( ((unsigned __int8)v6 & 3) != 0 )
  {
    if ( v5 >= 0x40 )
    {
      LODWORD(v3) = v5 >> 6;
      do
      {
        memcpy(state->buffer, v6);
        XeCryptShaTransform((X_CRYPT_SHA_STATE *)state->state, (char *)state->buffer);
        --v3;
        v6 += 64;
        v5 -= 64;
      }
      while ( (_DWORD)v3 != 0 );
    }
  }
  else if ( v5 >= 0x40 )
  {
    LODWORD(v3) = v5 >> 6;
    do
    {
      XeCryptShaTransform((X_CRYPT_SHA_STATE *)state->state, v6);
      --v3;
      v6 += 64;
      v5 -= 64;
    }
    while ( (_DWORD)v3 != 0 );
  }
  if ( v5 != 0 )
    memcpy(&state->buffer[(unsigned int)v8], v6);
}


/* ========== [0x22cf0] XeCryptShaFinal (200 bytes) ========== */
void __fastcall XeCryptShaFinal(X_CRYPT_SHA_STATE *state, uint8_t *digest, uint32_t digest_size)
{
  char *buffer; // r30
  uint32_t v7; // r29
  uint8_t *v8; // r27
  X_CRYPT_SHA_STATE *v9; // r31
  int v10; // [sp+50h] [-50h] BYREF

  buffer = (char *)state->buffer;
  v7 = state->count & 0x3F;
  v8 = &state->buffer[state->count & 0x3FLL];
  memset(v8, 0, 64 - v7);
  *v8 = 0x80;
  if ( 64 - v7 < 9 )
  {
    XeCryptShaTransform((X_CRYPT_SHA_STATE *)state->state, buffer);
    memset(buffer, 0, v7 + 1);
  }
  memcpy(buffer + 60, &v10);
  v9 = (X_CRYPT_SHA_STATE *)state->state;
  XeCryptShaTransform(v9, buffer);
  if ( digest_size != 0 )
    memcpy(digest, v9);
}


/* ========== [0x22db8] XeCryptSha (220 bytes) ========== */
void __fastcall XeCryptSha(
        const uint8_t *input1,
        __int64 input1_size,
        const uint8_t *input2,
        __int64 input2_size,
        const uint8_t *input3,
        __int64 input3_size,
        uint8_t *digest,
        uint32_t digest_size)
{
  __int64 v8; // r2
  X_CRYPT_SHA_STATE v15; // [sp+50h] [-A0h] BYREF
  int v16; // [sp+A8h] [-48h]

  v16 = *(_DWORD *)(v8 + 0x10000);
  v15.count = 0;
  v15.state[0] = 1732584193;
  v15.state[4] = 0xC3D2E1F0;
  v15.state[1] = 0xEFCDAB89;
  v15.state[2] = 0x98BADCFE;
  v15.state[3] = 271733878;
  if ( (_DWORD)input1_size != 0 )
    XeCryptShaUpdate(&v15, input1, input1_size);
  if ( (_DWORD)input2_size != 0 )
    XeCryptShaUpdate(&v15, input2, input2_size);
  if ( (_DWORD)input3_size != 0 )
    XeCryptShaUpdate(&v15, input3, input3_size);
  XeCryptShaFinal(&v15, digest, digest_size);
  v_MACHINE_CHECK_filter(v16);
}


/* ========== [0x22e98] XeCryptHmacShaInit (364 bytes) ========== */
void __fastcall XeCryptHmacShaInit(X_CRYPT_HMAC_SHA_STATE *state, const uint8_t *key, uint32_t key_size)
{
  __int64 v3; // r2
  int v5; // r6
  X_CRYPT_SHA_STATE *v6; // r30
  __int64 v8; // r11
  __int64 v9; // r10
  __int64 v10; // r6
  uint8_t v11[64]; // [sp+50h] [-C0h] BYREF
  uint8_t v12[64]; // [sp+90h] [-80h] BYREF
  int v13; // [sp+D0h] [-40h]

  v5 = *(_DWORD *)(v3 + 0x10000);
  state->sha_state[0].state[0] = 1732584193;
  v6 = &state->sha_state[1];
  state->sha_state[0].state[1] = 0xEFCDAB89;
  v13 = v5;
  state->sha_state[0].count = 0;
  state->sha_state[0].state[2] = 0x98BADCFE;
  state->sha_state[0].state[3] = 271733878;
  state->sha_state[0].state[4] = 0xC3D2E1F0;
  *(_QWORD *)&state->sha_state[1].count = 1732584193;
  state->sha_state[1].state[1] = 0xEFCDAB89;
  state->sha_state[1].state[2] = 0x98BADCFE;
  state->sha_state[1].state[3] = 271733878;
  state->sha_state[1].state[4] = 0xC3D2E1F0;
  memset(v11, 0, sizeof(v11));
  memset(v12, 0, sizeof(v12));
  memcpy(v11, key);
  memcpy(v12, key);
  v8 = 0;
  v9 = 8;
  do
  {
    --v9;
    v10 = *(_QWORD *)&v12[v8];
    *(_QWORD *)&v11[v8] ^= 0x3636363636363636uLL;
    *(_QWORD *)&v12[v8] = v10 ^ 0x5C5C5C5C5C5C5C5CLL;
    v8 += 8;
  }
  while ( (_DWORD)v9 != 0 );
  XeCryptShaUpdate(state->sha_state, v11, 64);
  XeCryptShaUpdate(v6, v12, 64);
  v_MACHINE_CHECK_filter(v13);
}


/* ========== [0x23008] XeCryptHmacShaUpdate (4 bytes) ========== */
// attributes: thunk
void __fastcall XeCryptHmacShaUpdate(X_CRYPT_SHA_STATE *state, const uint8_t *input, __int64 input_size)
{
  XeCryptShaUpdate(state, input, input_size);
}


/* ========== [0x23010] XeCryptHmacShaFinal (80 bytes) ========== */
void __fastcall XeCryptHmacShaFinal(X_CRYPT_SHA_STATE *state, uint8_t *digest, uint32_t digest_size)
{
  XeCryptShaFinal(state, nullptr, 0);
  XeCryptShaUpdate(state + 1, (const uint8_t *)state->state, 20);
  XeCryptShaFinal(state + 1, digest, digest_size);
}


/* ========== [0x23060] XeCryptHmacSha (168 bytes) ========== */
void __fastcall XeCryptHmacSha(
        const uint8_t *input1,
        uint32_t input1_size,
        const uint8_t *input2,
        __int64 input2_size,
        const uint8_t *input3,
        __int64 input3_size,
        uint8_t *digest,
        __int64 digest_size)
{
  __int64 v8; // r2
  X_CRYPT_HMAC_SHA_STATE v15; // [sp+50h] [-F0h] BYREF
  int v16; // [sp+100h] [-40h]
  uint8_t *v17; // [sp+190h] [+50h]
  uint32_t v18; // [sp+19Ch] [+5Ch]

  v16 = *(_DWORD *)(v8 + 0x10000);
  XeCryptHmacShaInit(&v15, input1, input1_size);
  if ( (_DWORD)input2_size != 0 )
    XeCryptShaUpdate(v15.sha_state, input2, input2_size);
  if ( (_DWORD)input3_size != 0 )
    XeCryptShaUpdate(v15.sha_state, input3, input3_size);
  if ( (_DWORD)digest_size != 0 )
    XeCryptShaUpdate(v15.sha_state, digest, digest_size);
  XeCryptHmacShaFinal(v15.sha_state, v17, v18);
  v_MACHINE_CHECK_filter(v16);
}


/* ========== [0x23108] XeCryptRc4 (104 bytes) ========== */
void __fastcall XeCryptRc4(const uint8_t *key, uint32_t key_size, uint8_t *input_output, uint32_t input_output_size)
{
  __int64 v4; // r2
  __int64 v5; // lr
  X_CRYPT_RC4_STATE v8; // [sp+50h] [-120h] BYREF
  int v9; // [sp+154h] [-1Ch]
  __int64 v10; // [sp+168h] [-8h]

  v10 = v5;
  v9 = *(_DWORD *)(v4 + 0x10000);
  XeCryptRc4Key(&v8, key, key_size);
  XeCryptRc4Ecb(&v8, input_output, input_output_size);
  v_MACHINE_CHECK_filter(v9);
}


/* ========== [0x23170] XeCryptRc4Key (168 bytes) ========== */
void __fastcall XeCryptRc4Key(X_CRYPT_RC4_STATE *state, const uint8_t *key, uint32_t key_size)
{
  __int64 v3; // r6
  __int64 i; // ctr
  __int64 v5; // r6
  __int64 v6; // r8
  uint8_t *v7; // r3
  __int64 v8; // r9
  __int64 v9; // r10
  __int64 j; // ctr
  char v11; // r11

  v3 = 0x1020304050607LL;
  for ( i = 8; i != 0; --i )
  {
    *(_QWORD *)state->S = v3;
    v5 = v3 + 0x808080808080808LL;
    *(_QWORD *)&state->S[8] = v5;
    v5 += 0x808080808080808LL;
    *(_QWORD *)&state->S[16] = v5;
    v5 += 0x808080808080808LL;
    *(_QWORD *)&state->S[24] = v5;
    v3 = v5 + 0x808080808080808LL;
    state = (X_CRYPT_RC4_STATE *)((char *)state + 32);
  }
  v6 = 0;
  *(_WORD *)state->S = 0;
  v7 = &state[-1].S[2];
  LOBYTE(v8) = 0;
  v9 = 0;
  for ( j = 256; j != 0; --j )
  {
    v11 = v7[v6];
    v8 = (unsigned __int8)(v8 + v11 + key[v9++]);
    v7[v6++] = v7[v8];
    v7[v8] = v11;
    if ( (_DWORD)v9 == key_size )
      v9 = 0;
  }
}


/* ========== [0x23218] XeCryptRc4Ecb (96 bytes) ========== */
void __fastcall XeCryptRc4Ecb(X_CRYPT_RC4_STATE *state, uint8_t *input_output, __int64 input_output_size)
{
  __int64 v3; // ctr
  __int64 v4; // r6
  __int64 v5; // r7
  uint8_t v6; // r8
  uint8_t v7; // r9

  if ( (_DWORD)input_output_size != 0 )
  {
    v3 = input_output_size;
    LOBYTE(v4) = state->i;
    LOBYTE(v5) = state->j;
    do
    {
      v4 = (unsigned __int8)(v4 + 1);
      v6 = state->S[v4];
      v5 = (unsigned __int8)(v5 + v6);
      v7 = state->S[v5];
      state->S[v4] = v7;
      state->S[v5] = v6;
      *input_output++ ^= state->S[(unsigned __int8)(v6 + v7)];
      --v3;
    }
    while ( v3 != 0 );
    state->i = v4;
    state->j = v5;
  }
}


/* ========== [0x23278] XeCryptRotSum (116 bytes) ========== */
__int64 *__fastcall XeCryptRotSum(__int64 *result, unsigned __int64 *a2, int a3)
{
  __int64 v3; // r7
  __int64 v4; // r9
  unsigned __int64 v5; // r6
  __int64 v6; // r10
  unsigned __int64 v7; // r8
  unsigned __int64 v8; // r10

  v3 = *result;
  v4 = result[1];
  v5 = result[2];
  v6 = result[3];
  while ( a3 != 0 )
  {
    v7 = *a2 + v4;
    v8 = v6 - *a2;
    v3 += v7 < *a2;
    v4 = __builtin_rotateleft64(v7, 29);
    --a3;
    v5 = (__PAIR128__(v5, *a2) - v8) >> 64;
    v6 = __builtin_rotateleft64(v8, 31);
    ++a2;
  }
  *result = v3;
  result[1] = v4;
  result[2] = v5;
  result[3] = v6;
  return result;
}


/* ========== [0x23330] XeCryptRotSumSha (284 bytes) ========== */
void __fastcall XeCryptRotSumSha(
        uint8_t *input1,
        __int64 input1_size,
        uint8_t *input2,
        __int64 input2_size,
        uint8_t *input3,
        uint32_t input3_size,
        uint8_t *digest,
        uint32_t output_size)
{
  __int64 v8; // r2
  __int64 v15; // [sp+50h] [-C0h] BYREF
  X_CRYPT_SHA_STATE v16; // [sp+70h] [-A0h] BYREF
  int v17; // [sp+C8h] [-48h]

  v17 = *(_DWORD *)(v8 + 0x10000);
  XeCryptRotSum(&v15, (unsigned __int64 *)input1, (unsigned int)input1_size >> 3);
  XeCryptRotSum(&v15, (unsigned __int64 *)input2, (unsigned int)input2_size >> 3);
  XeCryptShaInit(&v16);
  XeCryptShaUpdate(&v16, (const uint8_t *)&v15, 32);
  XeCryptShaUpdate(&v16, (const uint8_t *)&v15, 32);
  XeCryptShaUpdate(&v16, input1, input1_size);
  XeCryptShaUpdate(&v16, input2, input2_size);
  XeCryptShaUpdate(&v16, (const uint8_t *)&v15, 32);
  XeCryptShaUpdate(&v16, (const uint8_t *)&v15, 32);
  XeCryptShaFinal(&v16, input3, input3_size);
  v_MACHINE_CHECK_filter(v17);
}


/* ========== [0x23500] XeCryptMemDiff (56 bytes) ========== */
__int64 __fastcall XeCryptMemDiff(unsigned __int8 *a, unsigned __int8 *b, __int64 size)
{
  __int64 v3; // ctr
  __int64 v4; // r7
  __int64 v5; // r6
  __int64 i; // r5

  v3 = size;
  v4 = 0;
  if ( (_DWORD)size != 0 )
  {
    v5 = *b;
    for ( i = *a; ; i = *a )
    {
      v4 |= v5 ^ i;
      if ( --v3 == 0 )
        break;
      v5 = *++b;
      ++a;
    }
  }
  return v4;
}


/* ========== [0x23538] sub_23538 (232 bytes) ========== */
__int64 __fastcall sub_23538(__int64 a1, __int64 a2, _BYTE *a3, __int64 a4, __int64 a5)
{
  __int64 result; // r3
  __int64 v10; // r2
  __int64 v11; // r27
  __int64 v12; // r26
  int v13; // r10
  __int64 v14; // r9
  char v15; // r11
  char v16; // r11
  __int64 v17; // r10
  __int64 v18; // r9
  char v19; // r11
  _WORD v20[208]; // [sp+50h] [-1A0h] BYREF

  result = memcpy(a5, a1, 0x10u, a4, a5);
  if ( (_DWORD)a2 != 0 )
  {
    v11 = v10 + 73056;
    v12 = v10 + 81504;
    do
    {
      v13 = *(unsigned __int8 *)(a4 + 8) >> 7;
      v14 = 2LL * *(_QWORD *)(a4 + 8);
      *(_QWORD *)a4 *= 2LL;
      v15 = *(_BYTE *)(a4 + 7);
      *(_QWORD *)(a4 + 8) = v14;
      v16 = v13 | v15;
      LOBYTE(v13) = *(_BYTE *)(a4 + 15);
      *(_BYTE *)(a4 + 7) = v16;
      *(_BYTE *)(a4 + 15) = (*a3 >> 7) | v13;
      v17 = (unsigned __int8)a3[8] >> 7;
      v18 = 2LL * *((_QWORD *)a3 + 1);
      *(_QWORD *)a3 *= 2LL;
      v19 = a3[7];
      *((_QWORD *)a3 + 1) = v18;
      a3[7] = v17 | v19;
      sub_23E70(v20, a5, v12);
      result = sub_2402C(v20, a4, a5, v11);
      --a2;
    }
    while ( (_DWORD)a2 != 0 );
  }
  return result;
}


/* ========== [0x23620] sub_23620 (104 bytes) ========== */
__int64 __fastcall sub_23620(__int64 a1, __int64 a2, __int64 a3, __int64 a4, __int64 a5)
{
  __int64 v5; // r2
  __int64 v9; // r7
  __int64 v10; // r6
  _WORD v12[8]; // [sp+50h] [-50h] BYREF
  _BYTE v13[16]; // [sp+60h] [-40h] BYREF
  int v14; // [sp+70h] [-30h]

  v14 = *(_DWORD *)(v5 + 0x10000);
  memcpy((__int64)v13, a2, 0x10u, a4, a5);
  memcpy((__int64)v12, a3, 0x10u, v10, v9);
  sub_23538(a1, 128, v13, (__int64)v12, a4);
  return v_MACHINE_CHECK_filter(v14);
}


/* ========== [0x23698] HvpHdcpCalculateAKsvSignature (128 bytes) ========== */
unsigned __int64 __fastcall HvpHdcpCalculateAKsvSignature(unsigned __int64 a1, unsigned __int8 a2, unsigned __int8 a3)
{
  return (unsigned __int8)(((a1 >> 17) & 0x80 | (a1 >> 25) & 0x7F)
                         + ((a1 >> 37) & 7 | (a1 >> 29) & 0xF8)
                         + ((a1 >> 22) & 3 | (a1 >> 14) & 0xFC)
                         + ((a1 >> 11) & 0x1F | (a1 >> 3) & 0xE0)
                         + (((a1 & 0x80) != 0) | (2 * a1)))
       ^ (unsigned __int64)a2
       ^ a3
       ^ 0x3A;
}


/* ========== [0x23718] HvpHdcpCalculateBKsvSignature (124 bytes) ========== */
unsigned __int64 __fastcall HvpHdcpCalculateBKsvSignature(unsigned __int64 a1, __int16 a2, __int16 a3)
{
  return (unsigned __int8)((((a1 & 0x8000000000LL) != 0) | (a1 >> 31) & 0xFE)
                         + ((a1 >> 27) & 0x1F | (a1 >> 19) & 0xE0)
                         + (((unsigned __int16)a1 >> 10) | (a1 >> 2) & 0xC0)
                         + ((a1 >> 20) & 0xF | (a1 >> 12) & 0xF0)
                         + a1)
       ^ (unsigned __int64)HIBYTE(a2)
       ^ HIBYTE(a3)
       ^ 0x72;
}


/* ========== [0x23798] XeCryptAesKeyExpand128 (152 bytes) ========== */
int *__fastcall XeCryptAesKeyExpand128(int *result, __int64 a2)
{
  int v2; // r10
  unsigned __int8 *v3; // r5
  int v4; // r9
  __int64 v5; // r6
  int v6; // r8
  unsigned int v7; // r7
  int v8; // r31

  v2 = *result;
  v3 = (unsigned __int8 *)(a2 + 271);
  v4 = result[1];
  v5 = 10;
  v6 = result[2];
  v7 = result[3];
  do
  {
    v8 = *v3;
    --v5;
    --v3;
    v2 ^= ((((((*(unsigned __int8 *)(BYTE1(v7) + a2) ^ v8) << 8) ^ *(unsigned __int8 *)(BYTE2(v7) + a2)) << 8)
          ^ *(unsigned __int8 *)((unsigned __int8)v7 + a2)) << 8)
        ^ *(unsigned __int8 *)(((unsigned __int64)v7 >> 24) + a2);
    v4 ^= v2;
    v6 ^= v4;
    v7 ^= v6;
  }
  while ( (_DWORD)v5 != 0 );
  *result = v2;
  result[1] = v4;
  result[2] = v6;
  result[3] = v7;
  return result;
}


/* ========== [0x23830] sub_23830 (228 bytes) ========== */
__int64 __fastcall sub_23830(const uint8_t *a1, __int64 a2, _BYTE *a3, unsigned int a4)
{
  __int64 v4; // r2
  __int64 v8; // r29
  unsigned int v9; // r31
  unsigned int v10; // r11
  __int64 v11; // r10
  _BYTE *v12; // r11
  __int64 v13; // r9
  uint8_t v15[16]; // [sp+50h] [-C0h] BYREF
  X_CRYPT_SHA_STATE v16; // [sp+60h] [-B0h] BYREF
  uint8_t v17[20]; // [sp+C0h] [-50h] BYREF
  int i; // [sp+D4h] [-3Ch]

  v8 = 0;
  v9 = a4;
  for ( i = *(_DWORD *)(v4 + 0x10000); v9 != 0; a3 += (unsigned int)v11 )
  {
    XeCryptShaInit(&v16);
    XeCryptShaUpdate(&v16, a1, a2);
    XeCryptShaUpdate(&v16, v15, 4);
    XeCryptShaFinal(&v16, v17, 0x14u);
    v10 = 20;
    if ( v9 <= 0x14 )
      v10 = v9;
    v11 = v10;
    if ( v10 != 0 )
    {
      v12 = a3;
      v13 = v11;
      do
      {
        --v13;
        *v12 ^= v12[v17 - a3];
        ++v12;
      }
      while ( (_DWORD)v13 != 0 );
    }
    v9 -= v11;
    ++v8;
  }
  return v_MACHINE_CHECK_filter(i);
}


/* ========== [0x23918] sub_23918 (248 bytes) ========== */
__int64 __fastcall sub_23918(__int64 a1, unsigned int a2, _BYTE *a3, __int64 a4, __int64 a5, const uint8_t *a6)
{
  __int64 v6; // r2
  __int64 v11; // r28
  _BYTE *v14; // r30
  uint8_t *v15; // r31
  __int64 v16; // r29
  unsigned int v17; // r28
  __int64 v18; // r7
  __int64 v19; // r6
  __int64 v20; // r7
  __int64 v21; // r6

  v11 = a5;
  if ( (unsigned int)a4 < 0x2A || a2 > (int)a4 - 42 )
    return 0;
  if ( a5 == 0 )
    v11 = v6 + 90032;
  v14 = a3 + 1;
  *a3 = 0;
  memcpy((__int64)(a3 + 1), (__int64)a6, 0x14u, a4, a5);
  v15 = a3 + 21;
  memcpy((__int64)v15, v11, 0x14u, v19, v18);
  v17 = a4 - a2 - 42;
  memset((__int64)(v15 + 20), 0, v17);
  v16 = a4 - 21;
  v15[v17 + 20] = 1;
  memcpy((__int64)&v15[v17 + 21], a1, a2, v21, v20);
  sub_23830(a6, 20, v15, v16);
  sub_23830(v15, v16, v14, 0x14u);
  return 1;
}


/* ========== [0x23a10] sub_23A10 (252 bytes) ========== */
__int64 __fastcall sub_23A10(__int64 a1, __int64 a2, _QWORD *a3, _DWORD *a4, unsigned __int8 *a5)
{
  __int64 v5; // r2
  unsigned int v6; // r28
  unsigned __int8 *v10; // r26
  _BYTE *v11; // r11
  _BYTE *v12; // r10
  _BYTE *v13; // r11

  v6 = a2;
  v10 = a5;
  if ( (unsigned int)a2 < 0x2A || *(_BYTE *)a1 != 0 )
    return 0;
  if ( a5 == nullptr )
    v10 = (unsigned __int8 *)(v5 + 90032);
  sub_23830((const uint8_t *)(a1 + 21), a2 - 21, (_BYTE *)(a1 + 1), 0x14u);
  sub_23830((const uint8_t *)(a1 + 1), 20, (_BYTE *)(a1 + 21), v6 - 21);
  if ( (unsigned int)XeCryptMemDiff((unsigned __int8 *)(a1 + 21), v10, 20) != 0 )
    return 0;
  v11 = (_BYTE *)(a1 + 41);
  v12 = (_BYTE *)(v6 + a1);
  while ( v11 < v12 && *v11 == 0 )
    ++v11;
  if ( v11 == v12 || *v11 != 1 )
    return 0;
  v13 = v11 + 1;
  *a3 = v13;
  *a4 = a1 - (_DWORD)v13 + v6;
  return 1;
}


/* ========== [0x23b10] sub_23B10 (68 bytes) ========== */
char *__fastcall sub_23B10(char *result, unsigned int a2)
{
  __int64 v2; // r11
  char *v3; // r10
  char v4; // r8
  __int64 v5; // r9

  v2 = 0;
  if ( a2 >> 1 != 0 )
  {
    v3 = result;
    do
    {
      v4 = *v3;
      v5 = a2 - (unsigned int)v2++ - 1;
      *v3++ = result[v5];
      result[v5] = v4;
    }
    while ( (unsigned int)v2 < a2 >> 1 );
  }
  return result;
}


/* ========== [0x23b58] sub_23B58 (632 bytes) ========== */
__int64 __fastcall sub_23B58(__int64 a1, __int64 a2, const uint8_t *a3, int a4, __int64 a5, __int64 a6)
{
  __int64 v6; // r2
  int v9; // r31
  __int64 v10; // r6
  unsigned int v11; // r10
  int v12; // r9
  int *v13; // r11
  int v14; // r8
  int v15; // r10
  bool v16; // zf
  __int64 v17; // r7
  unsigned int v18; // r10
  int v19; // r9
  uint8_t *v20; // r11
  int v21; // r8
  int v22; // r10
  uint8_t v23; // r7
  __int64 v24; // r7
  __int64 v25; // r6
  __int64 v26; // r5
  __int64 v27; // r4
  __int64 v28; // r3
  bool v29; // cr58
  __int64 v30; // r11
  __int64 v31; // r31
  __int64 v32; // r30
  __int64 v33; // r11
  __int64 v34; // r10
  __int64 v35; // r11
  __int64 v36; // r10
  X_CRYPT_AES_STATE v38; // [sp+50h] [-1E0h] BYREF
  uint8_t v39[16]; // [sp+1B0h] [-80h] BYREF
  uint8_t v40[16]; // [sp+1C0h] [-70h] BYREF
  _WORD v41[8]; // [sp+1D0h] [-60h] BYREF
  uint8_t v42[15]; // [sp+1E0h] [-50h] BYREF
  char v43; // [sp+1EFh] [-41h]
  int v44; // [sp+1F0h] [-40h] BYREF

  v9 = a6 == 0 ? 0 : 0xF;
  v44 = *(_DWORD *)(v6 + 0x10000);
  if ( a4 != 16 )
  {
    v_MACHINE_CHECK_filter(v44);
    JUMPOUT(0x23E60);
  }
  XeCryptAesKey(&v38, a3, (__int64)a3);
  memset((__int64)v41, 0, sizeof(v41));
  XeCryptAesEcb(&v38, (const uint8_t *)v41, v42, 1);
  v11 = 0;
  v12 = 16;
  v13 = &v44;
  v14 = v42[v9] & 0x80;
  do
  {
    v13 = (int *)((char *)v13 - 1);
    --v12;
    v16 = v11 == 0;
    v15 = *(unsigned __int8 *)v13;
    if ( v16 )
      v17 = 2 * (unsigned int)*(unsigned __int8 *)v13;
    else
      v17 = (unsigned __int8)(2 * v15) | 1LL;
    *(_BYTE *)v13 = v17;
    v11 = v15 & 0xFFFFFF80;
  }
  while ( v12 != 0 );
  if ( v14 != 0 )
    v43 ^= 0x87u;
  memcpy((__int64)v41, (__int64)v42, 0x10u, v10, v17);
  v18 = 0;
  v19 = 16;
  v20 = v42;
  v21 = *((_BYTE *)v41 + v9) & 0x80;
  do
  {
    --v20;
    --v19;
    v16 = v18 == 0;
    v22 = *v20;
    if ( v16 )
      v23 = 2 * *v20;
    else
      v23 = (2 * v22) | 1;
    *v20 = v23;
    v18 = v22 & 0xFFFFFF80;
  }
  while ( v19 != 0 );
  if ( v21 != 0 )
    LOBYTE(v41[7]) ^= 0x87u;
  v28 = memset((__int64)v39, 0, sizeof(v39));
  v29 = (_DWORD)a2 == 16;
  if ( (unsigned int)a2 > 0x10 )
  {
    v30 = a2 - 17;
    LODWORD(v30) = (unsigned int)(a2 - 17) >> 4;
    v31 = a1 - (_QWORD)v39;
    v32 = v30 + 1;
    a1 += 16LL * (unsigned int)(v30 + 1);
    do
    {
      v33 = 0;
      v34 = 4;
      do
      {
        --v34;
        *(_DWORD *)&v40[v33] = *(_DWORD *)&v39[v33 + v31] ^ *(_DWORD *)&v39[v33];
        v33 += 4;
      }
      while ( (_DWORD)v34 != 0 );
      XeCryptAesEcb(&v38, v40, v39, 1);
      --v32;
      LODWORD(a2) = a2 - 16;
      v31 += 16;
    }
    while ( (_DWORD)v32 != 0 );
    v29 = (_DWORD)a2 == 16;
  }
  if ( v29 )
  {
    v35 = 0;
    v36 = 4;
    do
    {
      --v36;
      *(_DWORD *)&v40[v35] = *(_DWORD *)&v42[v35] ^ *(_DWORD *)&v39[v35 + a1 - (_QWORD)v39] ^ *(_DWORD *)&v39[v35];
      v35 += 4;
    }
    while ( (_DWORD)v36 != 0 );
    JUMPOUT(0x23E30);
  }
  if ( (_DWORD)a2 != 0 )
    v28 = memcpy((__int64)v40, a1, (unsigned int)a2, v25, v24);
  v40[(unsigned int)a2] = 0x80;
  if ( (unsigned int)(a2 + 1) >= 0x10 )
    JUMPOUT(0x23DF0);
  return XeCryptAesKeyTable(v28, v27, v26, v25, v24, 128, (__int64)v40, (unsigned int)(a2 + 1));
}


/* ========== [0x23dd0] XeCryptAesKeyTable (152 bytes) ========== */
// positive sp value has been detected, the output may be wrong!
__int64 __fastcall XeCryptAesKeyTable(
        __int64 a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  __int64 v8; // r27
  __int64 v9; // r11
  _BYTE *v10; // r10
  __int64 v11; // ctr
  __int64 v12; // r11
  __int64 v13; // r10
  X_CRYPT_AES_STATE v15; // [sp-1E0h] [-1E0h] BYREF
  uint8_t v16[16]; // [sp-80h] [-80h] BYREF
  uint8_t v17[48]; // [sp-70h] [-70h] BYREF
  int v18; // [sp-40h] [-40h]

  v10 = (_BYTE *)(a8 + a7);
  if ( (_DWORD)v9 != 0 )
  {
    v11 = v9;
    do
    {
      *v10++ = 0;
      --v11;
    }
    while ( v11 != 0 );
  }
  v12 = 0;
  v13 = 4;
  do
  {
    --v13;
    *(_DWORD *)&v17[v12] ^= *(_DWORD *)&v16[v12] ^ *(_DWORD *)&v17[v12 + 16];
    v12 += 4;
  }
  while ( (_DWORD)v13 != 0 );
  XeCryptAesEcb(&v15, v17, v16, 1);
  memcpy(v8, v16);
  v_MACHINE_CHECK_filter(v18);
  return 1;
}


/* ========== [0x23e70] sub_23E70 (284 bytes) ========== */
void __fastcall sub_23E70(_DWORD *a1, unsigned int *a2, __int64 a3)
{
  __int64 v3; // r16
  __int64 v4; // r6
  __int64 v5; // r7
  __int64 v6; // r8
  __int64 v7; // r9
  _DWORD *v8; // r3
  __int64 v9; // r0
  unsigned __int64 v10; // r6
  __int64 v11; // r7
  unsigned __int64 v12; // r8
  unsigned __int64 v13; // r9

  v4 = *a2;
  v5 = a2[1];
  v6 = a2[2];
  v7 = a2[3];
  *a1 = v4;
  a1[1] = v5;
  a1[2] = v6;
  a1[3] = v7;
  a1[44] = v4;
  a1[45] = v5;
  a1[46] = v6;
  a1[47] = v7;
  v8 = a1 + 4;
  LODWORD(v3) = BYTE4(v7);
  v9 = *(unsigned __int8 *)(a3 + 4367);
  LODWORD(v9) = (_DWORD)v9 << 24;
  v10 = (((unsigned __int64)*(unsigned __int8 *)(a3 + BYTE5(v7)) << 24)
       | ((unsigned __int64)*(unsigned __int8 *)(a3 + BYTE6(v7)) << 16)
       | (unsigned __int16)((*(unsigned __int8 *)(a3 + (unsigned __int8)v7) << 8) | *(unsigned __int8 *)(a3 + v3)))
      ^ v9
      ^ v4;
  v11 = v10 ^ v5;
  v12 = v11 ^ v6;
  v13 = v12 ^ v7;
  *v8 = v10;
  v8[1] = v11;
  v8[2] = v12;
  v8[3] = v13;
  v8[44] = *(_DWORD *)(a3 + 256 + (__builtin_rotateleft32(v10, 10) & 0x3FCLL))
         ^ *(_DWORD *)(a3 + 1280 + ((v10 >> 14) & 0x3FC))
         ^ *(_DWORD *)(a3 + 2304 + ((v10 >> 6) & 0x3FC))
         ^ *(_DWORD *)(a3 + 3328 + 4LL * (unsigned __int8)v10);
  XeCryptAesEncrypt(v8, a3 + 4367, a3, v10, v11, v12, v13);
}


/* ========== [0x23f8c] XeCryptAesEncrypt (112 bytes) ========== */
void __fastcall XeCryptAesEncrypt(
        _DWORD *a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        unsigned __int64 a6,
        unsigned __int64 a7)
{
  int v7; // r19
  int v8; // r18
  int v9; // r16
  __int64 v10; // r15
  __int64 v11; // r14
  __int64 v12; // r12
  __int64 v13; // r11

  a1[45] = v9 ^ v8 ^ v7;
  a1[46] = *(_DWORD *)(v13 + (__builtin_rotateleft32(a6, 10) & 0x3FCLL))
         ^ *(_DWORD *)(v12 + ((a6 >> 14) & 0x3FC))
         ^ *(_DWORD *)(v11 + ((a6 >> 6) & 0x3FC))
         ^ *(_DWORD *)(v10 + 4LL * (unsigned __int8)a6);
  a1[47] = *(_DWORD *)(v13 + (__builtin_rotateleft32(a7, 10) & 0x3FCLL))
         ^ *(_DWORD *)(v12 + ((a7 >> 14) & 0x3FC))
         ^ *(_DWORD *)(v11 + ((a7 >> 6) & 0x3FC))
         ^ *(_DWORD *)(v10 + 4LL * (unsigned __int8)a7);
  JUMPOUT(0x23ED8);
}


/* ========== [0x2402c] sub_2402C (424 bytes) ========== */
_DWORD *__fastcall sub_2402C(unsigned int *a1, unsigned int *a2, int *a3, __int64 a4)
{
  __int64 v4; // r12
  __int64 v5; // r14
  __int64 v6; // r15
  __int64 v7; // r16
  __int64 v8; // r17
  __int64 v9; // r18
  __int64 v10; // r19
  __int64 v11; // r20
  __int64 v12; // r4
  __int64 v13; // r7
  __int64 v14; // r8
  unsigned int *v15; // r3
  unsigned __int64 v16; // r12
  unsigned __int64 v17; // r14
  unsigned __int64 v18; // r15
  unsigned __int64 v19; // r16
  __int64 i; // ctr
  unsigned __int64 v21; // r9
  unsigned __int64 v22; // r10
  unsigned __int64 v23; // r11
  unsigned __int64 v24; // r16
  __int64 v25; // r17
  __int64 v26; // r18
  __int64 v27; // r19
  __int64 v28; // r20

  v4 = *a1;
  v5 = a1[1];
  v6 = a1[2];
  v7 = a1[3];
  v8 = *a2;
  v9 = a2[1];
  v10 = a2[2];
  v11 = a2[3];
  v12 = a4 + 1024;
  v13 = a4 + 2048;
  v14 = a4 + 3072;
  v15 = a1 + 4;
  v16 = v4 ^ v8;
  v17 = v5 ^ v9;
  v18 = v6 ^ v10;
  v19 = v7 ^ v11;
  for ( i = 9; i != 0; --i )
  {
    v21 = *(unsigned int *)(a4 + (__builtin_rotateleft32(v16, 10) & 0x3FCLL))
        ^ (unsigned __int64)*(unsigned int *)(v12 + ((v17 >> 14) & 0x3FC))
        ^ *(unsigned int *)(v13 + ((v18 >> 6) & 0x3FC))
        ^ (unsigned __int64)*(unsigned int *)(v14 + 4LL * (unsigned __int8)v19);
    v22 = *(unsigned int *)(a4 + (__builtin_rotateleft32(v17, 10) & 0x3FCLL))
        ^ (unsigned __int64)*(unsigned int *)(v12 + ((v18 >> 14) & 0x3FC))
        ^ *(unsigned int *)(v13 + ((v19 >> 6) & 0x3FC))
        ^ (unsigned __int64)*(unsigned int *)(v14 + 4LL * (unsigned __int8)v16);
    v23 = *(unsigned int *)(a4 + (__builtin_rotateleft32(v18, 10) & 0x3FCLL))
        ^ (unsigned __int64)*(unsigned int *)(v12 + ((v19 >> 14) & 0x3FC))
        ^ *(unsigned int *)(v13 + ((v16 >> 6) & 0x3FC))
        ^ (unsigned __int64)*(unsigned int *)(v14 + 4LL * (unsigned __int8)v17);
    v24 = *(unsigned int *)(a4 + (__builtin_rotateleft32(v19, 10) & 0x3FCLL))
        ^ (unsigned __int64)*(unsigned int *)(v12 + ((v16 >> 14) & 0x3FC))
        ^ *(unsigned int *)(v13 + ((v17 >> 6) & 0x3FC))
        ^ (unsigned __int64)*(unsigned int *)(v14 + 4LL * (unsigned __int8)v18);
    v25 = *v15;
    v26 = v15[1];
    v27 = v15[2];
    v28 = v15[3];
    v15 += 4;
    v16 = v21 ^ v25;
    v17 = v22 ^ v26;
    v18 = v23 ^ v27;
    v19 = v24 ^ v28;
  }
  a3[3] = v15[3]
        ^ ((*(unsigned __int8 *)(a4 + 1 + (__builtin_rotateleft32(v19, 10) & 0x3FCLL)) << 24)
         | (*(unsigned __int8 *)(a4 + 1 + ((v16 >> 14) & 0x3FC)) << 16)
         | (unsigned __int16)((*(unsigned __int8 *)(a4 + 1 + ((v17 >> 6) & 0x3FC)) << 8)
                            | *(unsigned __int8 *)(a4 + 1 + 4LL * (unsigned __int8)v18)));
  return XeCryptAesDecrypt(v15, v12, a3, a4, v13, v14, v15[2], a4 + 1);
}


/* ========== [0x241d4] XeCryptAesDecrypt (160 bytes) ========== */
// positive sp value has been detected, the output may be wrong!
_DWORD *__fastcall XeCryptAesDecrypt(
        _DWORD *result,
        __int64 a2,
        int *a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        int a7,
        __int64 a8)
{
  unsigned __int8 v8; // r20
  int v9; // r17
  unsigned __int64 v10; // r16
  unsigned __int64 v11; // r15
  unsigned __int64 v12; // r14
  int v13; // r12

  a3[2] = a7 ^ ((v8 << 24) | v9 & 0xFFFFFF);
  a3[1] = result[1]
        ^ ((*(unsigned __int8 *)(a8 + (__builtin_rotateleft32(v12, 10) & 0x3FCLL)) << 24)
         | (*(unsigned __int8 *)(a8 + ((v11 >> 14) & 0x3FC)) << 16)
         | (unsigned __int16)((*(unsigned __int8 *)(a8 + ((v10 >> 6) & 0x3FC)) << 8)
                            | *(unsigned __int8 *)(a8 + 4LL * (unsigned __int8)v13)));
  *a3 = *result
      ^ ((*(unsigned __int8 *)(a8 + (__builtin_rotateleft32(v13, 10) & 0x3FCLL)) << 24)
       | (*(unsigned __int8 *)(a8 + ((v12 >> 14) & 0x3FC)) << 16)
       | (unsigned __int16)((*(unsigned __int8 *)(a8 + ((v11 >> 6) & 0x3FC)) << 8)
                          | *(unsigned __int8 *)(a8 + 4LL * (unsigned __int8)v10)));
  return result;
}


/* ========== [0x24274] XeCryptBnQwBeSigDifference (588 bytes) ========== */
unsigned int *__fastcall XeCryptBnQwBeSigDifference(__int64 a1, unsigned int *a2, unsigned int *a3, __int64 a4)
{
  unsigned int *v4; // r3
  __int64 v5; // r12
  __int64 v6; // r14
  __int64 v7; // r15
  __int64 v8; // r16
  __int64 v9; // r17
  __int64 v10; // r18
  __int64 v11; // r19
  __int64 v12; // r20
  __int64 v13; // r4
  __int64 v14; // r7
  __int64 v15; // r8
  unsigned int *result; // r3
  unsigned __int64 v17; // r12
  unsigned __int64 v18; // r14
  unsigned __int64 v19; // r15
  unsigned __int64 v20; // r16
  __int64 i; // ctr
  unsigned __int64 v22; // r9
  unsigned __int64 v23; // r10
  unsigned __int64 v24; // r11
  unsigned __int64 v25; // r16
  __int64 v26; // r17
  __int64 v27; // r18
  __int64 v28; // r19
  __int64 v29; // r20

  v4 = (unsigned int *)(a1 + 336);
  v5 = *v4;
  v6 = v4[1];
  v7 = v4[2];
  v8 = v4[3];
  v9 = *a2;
  v10 = a2[1];
  v11 = a2[2];
  v12 = a2[3];
  v13 = a4 + 1024;
  v14 = a4 + 2048;
  v15 = a4 + 3072;
  result = v4 - 4;
  v17 = v5 ^ v9;
  v18 = v6 ^ v10;
  v19 = v7 ^ v11;
  v20 = v8 ^ v12;
  for ( i = 9; i != 0; --i )
  {
    v22 = *(unsigned int *)(a4 + (__builtin_rotateleft32(v17, 10) & 0x3FCLL))
        ^ (unsigned __int64)*(unsigned int *)(v13 + ((v20 >> 14) & 0x3FC))
        ^ *(unsigned int *)(v14 + ((v19 >> 6) & 0x3FC))
        ^ (unsigned __int64)*(unsigned int *)(v15 + 4LL * (unsigned __int8)v18);
    v23 = *(unsigned int *)(a4 + (__builtin_rotateleft32(v18, 10) & 0x3FCLL))
        ^ (unsigned __int64)*(unsigned int *)(v13 + ((v17 >> 14) & 0x3FC))
        ^ *(unsigned int *)(v14 + ((v20 >> 6) & 0x3FC))
        ^ (unsigned __int64)*(unsigned int *)(v15 + 4LL * (unsigned __int8)v19);
    v24 = *(unsigned int *)(a4 + (__builtin_rotateleft32(v19, 10) & 0x3FCLL))
        ^ (unsigned __int64)*(unsigned int *)(v13 + ((v18 >> 14) & 0x3FC))
        ^ *(unsigned int *)(v14 + ((v17 >> 6) & 0x3FC))
        ^ (unsigned __int64)*(unsigned int *)(v15 + 4LL * (unsigned __int8)v20);
    v25 = *(unsigned int *)(a4 + (__builtin_rotateleft32(v20, 10) & 0x3FCLL))
        ^ (unsigned __int64)*(unsigned int *)(v13 + ((v19 >> 14) & 0x3FC))
        ^ *(unsigned int *)(v14 + ((v18 >> 6) & 0x3FC))
        ^ (unsigned __int64)*(unsigned int *)(v15 + 4LL * (unsigned __int8)v17);
    v26 = *result;
    v27 = result[1];
    v28 = result[2];
    v29 = result[3];
    result -= 4;
    v17 = v22 ^ v26;
    v18 = v23 ^ v27;
    v19 = v24 ^ v28;
    v20 = v25 ^ v29;
  }
  LODWORD(v29) = BYTE4(v20);
  a3[3] = result[3]
        ^ ((*(unsigned __int8 *)(a4 + 4096 + v29) << 24)
         | (*(unsigned __int8 *)(a4 + 4096 + BYTE5(v19)) << 16)
         | (unsigned __int16)((*(unsigned __int8 *)(a4 + 4096 + BYTE6(v18)) << 8)
                            | *(unsigned __int8 *)(a4 + 4096 + (unsigned __int8)(v22 ^ v26))));
  a3[2] = result[2]
        ^ ((*(unsigned __int8 *)(a4 + 4096 + BYTE4(v19)) << 24)
         | (*(unsigned __int8 *)(a4 + 4096 + BYTE5(v18)) << 16)
         | (unsigned __int16)((*(unsigned __int8 *)(a4 + 4096 + BYTE6(v17)) << 8)
                            | *(unsigned __int8 *)(a4 + 4096 + (unsigned __int8)v20)));
  a3[1] = result[1]
        ^ ((*(unsigned __int8 *)(a4 + 4096 + BYTE4(v18)) << 24)
         | (*(unsigned __int8 *)(a4 + 4096 + BYTE5(v17)) << 16)
         | (unsigned __int16)((*(unsigned __int8 *)(a4 + 4096 + BYTE6(v20)) << 8)
                            | *(unsigned __int8 *)(a4 + 4096 + (unsigned __int8)(v24 ^ v28))));
  *a3 = *result
      ^ ((*(unsigned __int8 *)(a4 + 4096 + BYTE4(v17)) << 24)
       | (*(unsigned __int8 *)(a4 + 4096 + BYTE5(v20)) << 16)
       | (unsigned __int16)((*(unsigned __int8 *)(a4 + 4096 + BYTE6(v19)) << 8)
                          | *(unsigned __int8 *)(a4 + 4096 + (unsigned __int8)(v23 ^ v27))));
  return result;
}


/* ========== [0x244c0] XeCryptBnQwNeDigLen (184 bytes) ========== */
__int64 __fastcall XeCryptBnQwNeDigLen(char *a1, const uint8_t *a2, const uint8_t *a3, __int64 a4, char *a5, char *a6)
{
  __int64 *v6; // r28
  unsigned int v7; // r27
  __int64 v13; // r26
  unsigned int i; // r29

  v6 = (__int64 *)(a4 + 16);
  v7 = *(_DWORD *)(a4 + 4);
  v13 = XeCryptBnQwNeModInv(*(_QWORD *)(a4 + 16));
  XeCryptBnQw_Copy(a1, a5, 32);
  XeCryptBnQw_Copy(a5, a6, 32);
  for ( i = v7 >> 1; i != 0; i >>= 1 )
    XeCryptBnQwNeModMul(a6, (unsigned __int64 *)a6, (unsigned __int64 *)a6, v13, v6, 32);
  XeCryptBnQwNeModMul(a6, (unsigned __int64 *)a5, (unsigned __int64 *)a5, v13, v6, 32);
  XeCryptBnQw_Copy(a5, a5, 32);
  return XeCryptBnQwBeSigFormat((unsigned __int64)a6, a2, a3);
}


/* ========== [0x24578] XeCryptBnQwNeMul (52 bytes) ========== */
__int64 __fastcall XeCryptBnQwNeMul(__int64 a1, __int64 a2)
{
  _QWORD *v2; // r11

  v2 = (_QWORD *)(8LL * (unsigned int)a2 + a1);
  while ( (_DWORD)a2 != 0 )
  {
    if ( *--v2 != 0 )
      break;
    --a2;
  }
  return a2;
}


/* ========== [0x245b0] sub_245B0 (80 bytes) ========== */
unsigned __int64 __fastcall sub_245B0(__int64 a1, __int64 a2)
{
  unsigned __int64 *v2; // r11
  unsigned __int64 v3; // r10

  v2 = (unsigned __int64 *)(8LL * (unsigned int)a2 + a1);
  if ( (_DWORD)a2 != 0 )
  {
    do
    {
      v3 = *--v2;
      if ( *v2 != 0 )
        break;
      --a2;
    }
    while ( (_DWORD)a2 != 0 );
    if ( (_DWORD)a2 != 0 )
    {
      LODWORD(v3) = (_DWORD)a2 << 6;
      return v3 - _cntlzd(*v2);
    }
  }
  return a2;
}


/* ========== [0x24600] sub_24600 (176 bytes) ========== */
_QWORD *__fastcall sub_24600(_QWORD *a1, __int64 a2, __int64 a3, __int64 a4)
{
  unsigned int v4; // r31
  __int64 v5; // r25
  _QWORD *result; // r3
  __int64 v9; // r27
  _QWORD *v10; // r11
  __int64 v11; // r29
  _QWORD *v12; // r30
  __int64 v13; // r25
  __int64 v14; // r11

  v4 = a4;
  v5 = a2;
  LODWORD(a2) = 2 * a4;
  v9 = a4;
  result = XeCryptBnQw_Zero(a1, a2);
  v10 = (_QWORD *)(8LL * v4 + a3);
  if ( v4 != 0 )
  {
    do
    {
      if ( *--v10 != 0 )
        break;
      --v9;
    }
    while ( (_DWORD)v9 != 0 );
    if ( (_DWORD)v9 != 0 )
    {
      v11 = 0;
      v12 = a1;
      v13 = v5 - (_QWORD)a1;
      do
      {
        result = (_QWORD *)sub_24B90(v12, *(_QWORD *)((char *)v12 + v13), a3, v9);
        v14 = __builtin_rotateleft64(v9 + v11++, 3) & 0x7FFFFFFFFLL;
        ++v12;
        *(_QWORD *)((char *)a1 + (v14 & 0x7FFFFFFF8LL)) = result;
      }
      while ( (unsigned int)v11 < v4 );
    }
  }
  return result;
}


/* ========== [0x246b0] sub_246B0 (216 bytes) ========== */
unsigned __int64 *__fastcall sub_246B0(unsigned __int64 *a1, __int64 a2, __int64 a3)
{
  int v3; // r31
  unsigned __int64 *v4; // r25
  __int64 v6; // r6
  unsigned __int64 *result; // r3
  __int64 v8; // r26
  unsigned __int64 *v9; // r11
  __int64 v10; // r31
  _QWORD *v11; // r30
  unsigned __int64 *v12; // r11
  __int64 v13; // r29
  unsigned __int64 *v14; // r27
  __int64 v15; // r3

  v3 = a3;
  v4 = (unsigned __int64 *)a2;
  LODWORD(a2) = 2 * a3;
  v8 = a3;
  result = XeCryptBnQw_Zero(a1, a2);
  v9 = &v4[v3];
  if ( v3 != 0 )
  {
    do
    {
      if ( *--v9 != 0 )
        break;
      --v8;
    }
    while ( (_DWORD)v8 != 0 );
    if ( (_DWORD)v8 != 0 )
    {
      v10 = v8 - 1;
      v11 = a1 + 1;
      v12 = v4;
      if ( (_DWORD)v8 != 1 )
      {
        v13 = (unsigned int)v10;
        do
        {
          v14 = v12 + 1;
          v15 = sub_24B90(v11, *v12, v12 + 1, v10--);
          v11[v13] = v15;
          v12 = v14;
          --v13;
          v11 += 2;
        }
        while ( (_DWORD)v10 != 0 );
      }
      LODWORD(v6) = 2 * v8;
      XeCryptBnQwNeAcc(a1, (__int64)a1, (__int64)a1, v6);
      return XeCryptBnQwNeCompare(a1, v4, v8);
    }
  }
  return result;
}


/* ========== [0x24788] sub_24788 (72 bytes) ========== */
__int64 __fastcall sub_24788(unsigned __int128 a1, unsigned __int128 a3, __int64 r7_0, __int64 a4)
{
  unsigned __int128 v4; // r11
  __int64 result; // r3
  unsigned __int64 i; // r9

  *((_QWORD *)&v4 + 1) = *(_QWORD *)&a1;
  if ( (_QWORD)a3 != 0 && (a3 & 0x8000000000000000uLL) == 0LL )
  {
    if ( a1 < a3 )
      return XeCryptBnQwNeMod(0, 2 * a1, a3, r7_0, a4, 0x8000000000000000LL);
  }
  else if ( *(_QWORD *)&a1 < *(_QWORD *)&a3 )
  {
    result = 0;
    for ( i = 0x8000000000000000LL; i != 0; i >>= 1 )
    {
      *(_QWORD *)&v4 = (unsigned __int64)v4 >> 63;
      *((_QWORD *)&v4 + 1) = ((unsigned __int64)a1 >> 63) | (2 * v4);
      *((_QWORD *)&a1 + 1) = 2 * a1;
      if ( v4 >= *(unsigned __int64 *)&a3 )
      {
        *((_QWORD *)&v4 + 1) = v4 - *(_QWORD *)&a3;
        result |= i;
      }
    }
    return result;
  }
  return -1;
}


/* ========== [0x247d0] XeCryptBnQwNeMod (68 bytes) ========== */
__int64 __fastcall XeCryptBnQwNeMod(
        __int64 result,
        unsigned __int64 a2,
        unsigned __int128 a3,
        __int64 a4,
        __int64 a5,
        unsigned __int64 a6)
{
  unsigned __int64 v6; // r11

  if ( __PAIR128__(v6, a2) >= a3 )
    result |= a6;
  if ( a6 >> 1 != 0 )
    JUMPOUT(0x247C0);
  return result;
}


/* ========== [0x24870] XeCryptBnQwNeAdd (612 bytes) ========== */
__int64 __fastcall XeCryptBnQwNeAdd(char *a1, char *a2, char *a3, __int64 a4, __int64 a5)
{
  __int64 v9; // r31
  char *i; // r11
  __int64 v11; // r30
  char *j; // r11
  __int64 v13; // r4
  char *v14; // r3
  unsigned __int64 v16; // r26
  __int64 v17; // r6
  __int64 v18; // r29
  __int64 v19; // r25
  __int64 v20; // r30
  __int64 v21; // r6
  __int64 v22; // r5
  __int64 v23; // r4
  int v24; // r3
  __int64 v25; // r6
  _QWORD v26[66]; // [sp+50h] [-8A0h] BYREF
  char v27[528]; // [sp+260h] [-690h] BYREF
  _QWORD v28[144]; // [sp+470h] [-480h] BYREF

  if ( (unsigned int)a4 >= 0x82 || (unsigned int)a5 >= 0x42 )
    return 0;
  v9 = a5;
  for ( i = &a2[8 * (unsigned int)a5]; (_DWORD)v9 != 0; --v9 )
  {
    i -= 8;
    if ( *(_QWORD *)i != 0 )
      break;
  }
  if ( (_DWORD)v9 == 0 )
    return 0;
  v11 = a4;
  for ( j = &a1[8 * (unsigned int)a4]; (_DWORD)v11 != 0; --v11 )
  {
    j -= 8;
    if ( *(_QWORD *)j != 0 )
      break;
  }
  if ( (unsigned int)v11 >= (unsigned int)v9 )
  {
    XeCryptBnQw_Copy(a2, v27, v9);
    v16 = 8LL * (unsigned int)v9;
    *(_QWORD *)&v27[v16] = 0;
    XeCryptBnQw_Copy(a1, (char *)v28, v11);
    v18 = v11 - v9;
    v28[(unsigned int)v11] = 0;
    if ( (int)v11 - (int)v9 >= 0 )
    {
      v19 = v11;
      do
      {
        if ( (unsigned int)v9 <= 1 )
          LODWORD(v17) = 0;
        else
          v17 = *(_QWORD *)&a2[8 * (unsigned int)(v9 - 2)];
        v20 = sub_24788(
                v28[(unsigned int)v19],
                v28[(unsigned int)(v19 - 1)],
                *(_QWORD *)&a2[8 * (unsigned int)(v9 - 1)],
                v17);
        if ( v20 == 0 )
          LODWORD(v20) = 1;
        XeCryptBnQw_Zero(v26, v9);
        LODWORD(v21) = v9;
        LODWORD(v22) = (_DWORD)a2;
        LODWORD(v23) = v20;
        v26[v16 / 8] = sub_24B90(v26, v23, v22, v21);
        while ( 1 )
        {
          v24 = sub_24D08(v26, &v28[(int)v18], v9 + 1);
          v25 = v9 + 1;
          if ( v24 != 1 )
            break;
          sub_24B38(v26, v26, v27, v25);
        }
        sub_24B38(&v28[(int)v18], &v28[(int)v18], v26, v25);
        if ( (int)sub_24D08(&v28[(int)v18], v27, v9 + 1) >= 0 )
        {
          ++v18;
          ++v19;
        }
        --v18;
        --v19;
      }
      while ( (int)v18 >= 0 );
    }
    XeCryptBnQw_Copy((char *)v28, a3, v9);
    v13 = a5 - v9;
    v14 = &a3[v16];
  }
  else
  {
    XeCryptBnQw_Copy(a1, a3, v11);
    v13 = a5 - v11;
    v14 = &a3[8 * (unsigned int)v11];
  }
  XeCryptBnQw_Zero(v14, v13);
  return 1;
}


/* ========== [0x24ae0] XeCryptBnQwNeAcc (88 bytes) ========== */
unsigned __int64 __fastcall XeCryptBnQwNeAcc(unsigned __int64 *a1, __int64 a2, __int64 a3, __int64 a4)
{
  unsigned __int64 v4; // r7
  __int64 v5; // r4
  __int64 v6; // r5
  __int64 v7; // ctr
  unsigned __int64 v8; // r9
  unsigned __int64 v9; // r8
  bool v10; // cr36
  unsigned __int64 v11; // r8

  v4 = 0;
  v5 = a2 - (_QWORD)a1;
  v6 = a3 - (_QWORD)a1;
  v7 = a4;
  if ( (_DWORD)a4 != 0 )
  {
    do
    {
      v8 = *(unsigned __int64 *)((char *)a1 + v6);
      v9 = *(unsigned __int64 *)((char *)a1 + v5) + v4;
      v10 = v9 < v4;
      v11 = v9 + v8;
      *a1++ = v11;
      v4 = (unsigned __int8)((2 * (v10 && v11 < v8)) | v10 ^ (v11 < v8));
      --v7;
    }
    while ( v7 != 0 );
  }
  return v4;
}


/* ========== [0x24b38] sub_24B38 (88 bytes) ========== */
unsigned __int64 __fastcall sub_24B38(_QWORD *a1, __int64 a2, __int64 a3, __int64 a4)
{
  unsigned __int64 v4; // r7
  __int64 v5; // r4
  __int64 v6; // r5
  __int64 v7; // ctr
  unsigned __int64 v8; // r8
  unsigned __int64 v9; // r9
  bool v10; // cr36
  unsigned __int64 v11; // r8

  v4 = 0;
  v5 = a2 - (_QWORD)a1;
  v6 = a3 - (_QWORD)a1;
  v7 = a4;
  if ( (_DWORD)a4 != 0 )
  {
    do
    {
      v8 = *(_QWORD *)((char *)a1 + v5);
      v9 = *(_QWORD *)((char *)a1 + v6);
      v10 = v8 < v4;
      v11 = v8 - v4;
      *a1++ = v11 - v9;
      v4 = (unsigned __int8)((2 * (v10 && v11 < v9)) | v10 ^ (v11 < v9));
      --v7;
    }
    while ( v7 != 0 );
  }
  return v4;
}


/* ========== [0x24b90] sub_24B90 (124 bytes) ========== */
unsigned __int64 __fastcall sub_24B90(unsigned __int64 *a1, unsigned __int64 a2, __int64 a3, __int64 a4)
{
  unsigned __int64 v4; // r12
  __int64 v5; // r5
  __int64 v6; // ctr
  unsigned __int64 v7; // r9
  unsigned __int64 v8; // r8
  bool v9; // cr36
  unsigned __int64 v10; // r8

  v4 = 0;
  if ( a2 <= 1 )
  {
    if ( a2 == 1 )
      return XeCryptBnQwNeAcc(a1, (__int64)a1, a3, a4);
    else
      return 0;
  }
  else
  {
    v5 = a3 - (_QWORD)a1;
    v6 = a4;
    if ( (_DWORD)a4 != 0 )
    {
      do
      {
        v7 = *(unsigned __int64 *)((char *)a1 + v5);
        v8 = *a1 + v4;
        v9 = v8 < v4;
        v10 = v8 + a2 * v7;
        *a1++ = v10;
        v4 = ((a2 * (unsigned __int128)v7) >> 64)
           + (unsigned __int8)((2 * (v9 && v10 < a2 * v7)) | v9 ^ (v10 < a2 * v7));
        --v6;
      }
      while ( v6 != 0 );
    }
    return v4;
  }
}


/* ========== [0x24c0c] XeCryptBnQwNeCompare (124 bytes) ========== */
unsigned __int64 *__fastcall XeCryptBnQwNeCompare(unsigned __int64 *result, unsigned __int64 *a2, __int64 a3)
{
  _BOOL8 v3; // r12
  __int64 v4; // ctr
  unsigned __int64 v5; // r9
  unsigned __int64 v6; // r5
  unsigned __int64 v7; // r8
  __int64 v8; // r11
  bool v9; // cr36
  unsigned __int64 v10; // r8
  __int64 v11; // r7
  unsigned __int64 v12; // r5

  v3 = false;
  v4 = a3;
  if ( (_DWORD)a3 != 0 )
  {
    do
    {
      v5 = *a2;
      v6 = result[1];
      v7 = *result + v3;
      v8 = (v5 * (unsigned __int128)v5) >> 64;
      v9 = v7 < v3;
      v10 = v7 + v5 * v5;
      *result = v10;
      ++a2;
      v11 = (unsigned __int8)((2 * (v9 && v10 < v5 * v5)) | v9 ^ (v10 < v5 * v5));
      v12 = v6 + v8 + v11;
      result[1] = v12;
      result += 2;
      v3 = v12 < v8 + v11;
      --v4;
    }
    while ( v4 != 0 );
  }
  return result;
}


/* ========== [0x24c88] sub_24C88 (124 bytes) ========== */
unsigned __int64 __fastcall sub_24C88(unsigned __int64 *a1, unsigned __int64 a2, __int64 a3, __int64 a4)
{
  unsigned __int64 v4; // r12
  __int64 v5; // r5
  __int64 v6; // ctr
  unsigned __int64 v7; // r9
  bool v8; // cr36
  unsigned __int64 v9; // r8

  v4 = 0;
  if ( a2 <= 1 )
  {
    if ( a2 == 1 )
      return sub_24B38(a1, (__int64)a1, a3, a4);
    else
      return 0;
  }
  else
  {
    v5 = a3 - (_QWORD)a1;
    v6 = a4;
    if ( (_DWORD)a4 != 0 )
    {
      do
      {
        v7 = *(unsigned __int64 *)((char *)a1 + v5);
        v8 = *a1 < v4;
        v9 = *a1 - v4;
        *a1++ = v9 - a2 * v7;
        v4 = ((a2 * (unsigned __int128)v7) >> 64) + (unsigned __int8)((2 * (v8 && v9 < a2 * v7)) | v8 ^ (v9 < a2 * v7));
        --v6;
      }
      while ( v6 != 0 );
    }
    return v4;
  }
}


/* ========== [0x24d08] sub_24D08 (80 bytes) ========== */
__int64 __fastcall sub_24D08(__int64 a1, __int64 a2, unsigned int a3)
{
  __int64 v3; // r11
  _QWORD *v4; // r10
  _QWORD *v5; // r11
  __int64 result; // r3

  v3 = 8LL * a3;
  v4 = (_QWORD *)(v3 + a1);
  v5 = (_QWORD *)(v3 + a2);
  while ( 1 )
  {
    if ( a3 == 0 )
      return 0;
    if ( *--v4 != *--v5 )
      break;
    --a3;
  }
  result = 1;
  if ( *v4 <= *v5 )
    return -1;
  return result;
}


/* ========== [0x24d58] sub_24D58 (128 bytes) ========== */
__int64 __fastcall sub_24D58(unsigned __int8 *a1)
{
  __int64 v1; // r2
  _DWORD *v3; // r30
  __int64 v4; // r2
  int v5; // r3
  __int64 v6; // r31

  v3 = (_DWORD *)(v1 + 93776);
  HvpAquireSpinLock((_DWORD *)(v1 + 93776));
  if ( *(_BYTE *)(v4 + 65548) == 0 || (v5 = XeCryptMemDiff(a1, (unsigned __int8 *)(v4 + 65552), 16), v6 = 1, v5 != 0) )
    v6 = 0;
  HvpReleaseSpinLock(v3);
  return v6;
}


/* ========== [0x24dd8] sub_24DD8 (120 bytes) ========== */
__int64 __fastcall sub_24DD8(_BYTE *a1, int a2)
{
  __int64 v2; // r2
  __int64 v4; // r31
  _DWORD *v5; // r29
  __int64 v6; // r2
  _BYTE *v7; // r10
  _BYTE *v8; // r11
  __int64 i; // ctr

  v4 = 0;
  if ( a2 == 16 )
  {
    v5 = (_DWORD *)(v2 + 93776);
    HvpAquireSpinLock((_DWORD *)(v2 + 93776));
    v4 = *(unsigned __int8 *)(v6 + 65548);
    if ( *(_BYTE *)(v6 + 65548) != 0 )
    {
      v7 = a1;
      v8 = (_BYTE *)(v6 + 65552);
      for ( i = 16; i != 0; --i )
        *v7++ = *v8++;
    }
    HvpReleaseSpinLock(v5);
  }
  return v4;
}


/* ========== [0x24e50] sub_24E50 (268 bytes) ========== */
__int64 __fastcall sub_24E50(uint8_t *a1)
{
  __int64 v1; // r2
  uint8_t v3; // r11
  __int64 v4; // r2
  __int64 v5; // r5
  __int64 v6; // r2
  uint8_t *v7; // r9
  _BYTE *v8; // r30
  uint8_t *v9; // r10
  __int64 i; // ctr
  __int64 v11; // r2
  _BYTE *v12; // r11
  _BYTE *v13; // r9
  __int64 j; // ctr
  X_CRYPT_AES_STATE v16; // [sp+50h] [-190h] BYREF
  uint8_t v17[16]; // [sp+1B0h] [-30h] BYREF
  int v18; // [sp+1C0h] [-20h]

  v18 = *(_DWORD *)(v1 + 0x10000);
  memset((__int64)a1, 0, 0x32u);
  v3 = *a1 & 0xC0 | 0x3B;
  a1[1] = 48;
  *a1 = v3;
  XeCryptAesKey(&v16, (const uint8_t *)(*(_QWORD *)(v4 + 91072) + 256LL), v5);
  v7 = v17;
  v8 = (_BYTE *)(v6 + 65584);
  v9 = (uint8_t *)(v6 + 65584);
  for ( i = 16; i != 0; --i )
    *v7++ = *v9++;
  XeCryptAesCbc(&v16, (const uint8_t *)(v6 + 65568), 0x10u, a1 + 2, v17, 1);
  XeCryptAesCbc(&v16, (const uint8_t *)(v11 + 93072), 0x10u, a1 + 18, v17, 1);
  v12 = a1 + 34;
  v13 = v8;
  for ( j = 16; j != 0; --j )
    *v12++ = *v13++;
  return v_MACHINE_CHECK_filter(v18);
}


/* ========== [0x24f60] sub_24F60 (340 bytes) ========== */
__int64 __fastcall sub_24F60(uint8_t *a1)
{
  __int64 v1; // r2
  __int64 v3; // r7
  __int64 v4; // r2
  uint8_t *v5; // r30
  __int64 v6; // r10
  __int64 v7; // r5
  __int64 v8; // r2
  uint8_t *v9; // r9
  uint8_t *v10; // r10
  __int64 i; // ctr
  __int64 v12; // r2
  __int64 v13; // r2
  _BYTE *v14; // r11
  uint8_t *v15; // r9
  __int64 j; // ctr
  __int64 v17; // r7
  X_CRYPT_AES_STATE v19; // [sp+50h] [-1D0h] BYREF
  _QWORD v20[2]; // [sp+1B0h] [-70h] BYREF
  uint8_t v21[16]; // [sp+1C0h] [-60h] BYREF
  uint8_t v22[16]; // [sp+1D0h] [-50h] BYREF
  uint8_t v23[16]; // [sp+1E0h] [-40h] BYREF
  int v24; // [sp+1F0h] [-30h]

  v24 = *(_DWORD *)(v1 + 0x10000);
  memset((__int64)a1, 0, 0x42u);
  v3 = *a1 & 0xC0 | 0x28LL;
  v5 = (uint8_t *)(v4 + 65584);
  v20[0] = 0;
  v20[1] = 0;
  *a1 = v3;
  a1[1] = 64;
  v6 = *(_QWORD *)(v4 + 91072);
  LOBYTE(v20[1]) = 1;
  sub_23620(v6 + 256, v4 + 65584, (__int64)v20, (__int64)v21, v3);
  XeCryptAesKey(&v19, v21, v7);
  v9 = v22;
  v10 = v5;
  for ( i = 16; i != 0; --i )
    *v9++ = *v10++;
  XeCryptAesCbc(&v19, (const uint8_t *)(v8 + 65568), 0x10u, a1 + 2, v22, 1);
  XeCryptAesCbc(&v19, (const uint8_t *)(v12 + 93072), 0x10u, a1 + 18, v22, 1);
  v14 = a1 + 34;
  v15 = v5;
  for ( j = 16; j != 0; --j )
    *v14++ = *v15++;
  XeCryptAesCbcMac((const uint8_t *)(v13 + 66936), a1 + 2, 0x20u, v23);
  sub_23620((__int64)v21, (__int64)v23, (__int64)v20, (__int64)(a1 + 50), v17);
  return v_MACHINE_CHECK_filter(v24);
}


/* ========== [0x250b8] HvxDvdAuthBuildNVPage (128 bytes) ========== */
__int64 __fastcall HvxDvdAuthBuildNVPage(__int64 a1)
{
  __int64 v1; // r2
  _DWORD *v3; // r30
  __int64 v4; // r10
  __int64 v5; // r9
  __int64 v6; // r8
  __int64 v7; // r7
  __int64 v8; // r6
  __int64 v9; // r5
  __int64 v10; // r3
  __int64 v11; // r2

  v3 = (_DWORD *)(v1 + 93776);
  HvpAquireSpinLock((_DWORD *)(v1 + 93776));
  v10 = HvpPhysicalToReal(a1, 74, v9, v8, v7, v6, v5, v4);
  LODWORD(v10) = v10 + 8;
  if ( (*(_WORD *)(*(_QWORD *)(v11 + 90832) + 1540LL) & 0x200) == 0x200 )
    sub_24F60(v10);
  else
    sub_24E50(v10);
  HvpReleaseSpinLock(v3);
  return 0;
}


/* ========== [0x25138] sub_25138 (124 bytes) ========== */
__int64 __fastcall sub_25138(int a1, int a2)
{
  __int64 v2; // r2
  __int64 result; // r3
  __int64 v4; // r2

  if ( a1 != 0 )
  {
    *(_BYTE *)(v2 + 65548) = 0;
    *(_BYTE *)(v2 + 93712) = 0;
  }
  if ( a2 != 0 )
  {
    *(_DWORD *)(v2 + 65552) = 0;
    *(_DWORD *)(v2 + 65556) = 0;
    *(_DWORD *)(v2 + 65560) = 0;
    *(_DWORD *)(v2 + 65564) = 0;
  }
  result = memset(v2 + 93088, 0, 0x270u);
  *(_DWORD *)(v4 + 93716) = 1;
  return result;
}


/* ========== [0x251b8] HvxDvdAuthClearDiscAuthInfo (88 bytes) ========== */
__int64 __fastcall HvxDvdAuthClearDiscAuthInfo(int a1)
{
  __int64 v1; // r2
  _DWORD *v3; // r31
  __int64 v4; // r4
  __int64 v5; // r3

  v3 = (_DWORD *)(v1 + 93776);
  HvpAquireSpinLock((_DWORD *)(v1 + 93776));
  LODWORD(v4) = 0;
  LODWORD(v5) = a1;
  sub_25138(v5, v4);
  HvpReleaseSpinLock(v3);
  return 0;
}


/* ========== [0x25210] sub_25210 (136 bytes) ========== */
__int64 sub_25210()
{
  __int64 v0; // r2
  __int64 v1; // r31
  unsigned int v2; // r30
  unsigned int v3; // r11
  __int64 v4; // r9
  __int64 v5; // r11
  __int64 result; // r3
  uint8_t v7[8]; // [sp+50h] [-20h] BYREF

  v1 = v0 + 93088;
  v2 = *(unsigned __int8 *)(v0 + 93123) - (*(unsigned __int8 *)(v0 + 93425) >> 3);
  sub_20060(v7, 4);
  v3 = *(_DWORD *)v7;
  __twllei(v2, 0);
  v4 = v3 % v2;
  v5 = v2 - 1;
  result = *(unsigned __int8 *)(v4 + v1 + 344);
  *(_BYTE *)(v4 + v1 + 344) = *(_BYTE *)(v5 + v1 + 344);
  *(_BYTE *)(v5 + v1 + 344) = result;
  return result;
}


/* ========== [0x25298] sub_25298 (528 bytes) ========== */
__int64 __fastcall sub_25298(__int64 a1, __int64 a2, __int64 a3, __int64 a4, __int64 a5)
{
  __int64 v5; // r2
  __int64 v7; // r31
  __int64 v8; // r28
  unsigned int v9; // r11
  unsigned int v10; // r30
  __int64 v11; // r5
  unsigned int v12; // r11
  unsigned int v13; // r11
  unsigned int v14; // r11
  _BYTE *v15; // r9
  _BYTE *v16; // r10
  __int64 i; // ctr
  __int64 v18; // r10
  int v19; // r11
  int v20; // r10
  uint8_t v22[16]; // [sp+50h] [-B0h] BYREF
  X_CRYPT_SHA_STATE v23; // [sp+60h] [-A0h] BYREF
  uint8_t v24[20]; // [sp+C0h] [-40h] BYREF
  int v25; // [sp+D4h] [-2Ch]

  v7 = v5 + 93088;
  v8 = 0;
  v25 = *(_DWORD *)(v5 + 0x10000);
  memcpy(v5 + 93424, a1, 0x34u, a4, a5);
  XeCryptShaInit(&v23);
  XeCryptShaUpdate(&v23, (const uint8_t *)(v7 + 336), 36);
  *(_DWORD *)(v7 + 340) = __builtin_bswap32(*(_DWORD *)(v7 + 340));
  *(_DWORD *)(v7 + 352) = __builtin_bswap32(*(_DWORD *)(v7 + 352));
  v9 = __builtin_bswap32(*(_DWORD *)(v7 + 356));
  *(_DWORD *)(v7 + 356) = v9;
  v10 = v9 - (*(_DWORD *)(v7 + 348) >> 15) - 12;
  *(_DWORD *)(v7 + 360) = __builtin_bswap32(*(_DWORD *)(v7 + 360));
  *(_DWORD *)(v7 + 364) = __builtin_bswap32(*(_DWORD *)(v7 + 364));
  *(_DWORD *)(v7 + 368) = __builtin_bswap32(*(_DWORD *)(v7 + 368));
  sub_20060(v22, 4);
  v12 = *(_DWORD *)v22;
  __twllei(v10, 0);
  v13 = v12 % v10;
  *(_DWORD *)(v7 + 604) = v13;
  if ( (((v13 + 11) ^ v13) & 0xFFFFF800) != 0 )
  {
    v13 -= ((_WORD)v13 + 12) & 0x7FF;
    *(_DWORD *)(v7 + 604) = v13;
  }
  if ( v13 >= *(_DWORD *)(v7 + 356) )
    goto LABEL_4;
  v14 = v13 >> 11;
  *(_DWORD *)(v7 + 596) = v14;
  if ( v14 >= *(_DWORD *)(v7 + 364) )
    goto LABEL_4;
  v15 = (_BYTE *)(v7 + 388);
  v16 = (_BYTE *)(16LL * v14 + a1 + 36);
  for ( i = 16; i != 0; --i )
    *v15++ = *v16++;
  LODWORD(v11) = 16 * *(_DWORD *)(v7 + 596);
  XeCryptShaUpdate(&v23, (const uint8_t *)(a1 + 36), v11);
  XeCryptShaUpdate(&v23, (const uint8_t *)(v7 + 388), 16);
  v19 = *(_DWORD *)(v7 + 596);
  if ( 16 * v19 + 52 < 0x8000 )
  {
    LODWORD(v18) = 16 * v19;
    XeCryptShaUpdate(&v23, (const uint8_t *)(16LL * (unsigned int)(v19 + 1) + a1 + 36), 32716 - v18);
  }
  XeCryptShaFinal(&v23, v24, 0x14u);
  if ( (unsigned int)XeCryptMemDiff((unsigned __int8 *)(v7 + 290), v24, 20) != 0 )
  {
LABEL_4:
    v8 = -939523963;
  }
  else
  {
    v20 = *(_DWORD *)(v7 + 604) >> 11;
    *(_DWORD *)(v7 + 328) = 1;
    *(_DWORD *)(v7 + 608) = 16 * (v20 + *(_DWORD *)(v7 + 352)) + *(_DWORD *)(v7 + 312);
  }
  v_MACHINE_CHECK_filter(v25);
  return v8;
}


/* ========== [0x254a8] sub_254A8 (136 bytes) ========== */
void sub_254A8()
{
  _DWORD *v0; // r2
  __int64 v1; // r31
  __int64 v2; // r3
  unsigned __int64 v3; // r6
  int v4; // r5
  int v5; // r4
  unsigned int v6; // r8
  unsigned int v7; // r9
  unsigned int v8; // r10

  v1 = (unsigned int)v0[23426];
  if ( (_DWORD)v1 != 12 )
  {
    v2 = (unsigned int)v0[23357];
    v3 = (unsigned int)v0[23361];
    v4 = v0[23364];
    v5 = v0[23362];
    v6 = v0[23350];
    v7 = v0[23423];
    do
    {
      __tdllei(v3, 0);
      v8 = 16 * ((unsigned __int64)v7 * v2 % v3);
      v0[23425] = v8;
      if ( v8 < v6 )
        break;
      if ( v8 >= v4 + v5 + v6 )
        break;
      ++v1;
      ++v7;
      v0[23426] = v1;
      v0[23423] = v7;
    }
    while ( (_DWORD)v1 != 12 );
  }
}


/* ========== [0x25530] sub_25530 (208 bytes) ========== */
__int64 __fastcall sub_25530(const uint8_t *a1, __int64 a2, __int64 a3, __int64 a4, __int64 a5)
{
  __int64 v5; // r2
  __int64 v7; // r31
  __int64 v8; // r30
  __int64 v9; // r8
  __int64 v10; // r4
  uint8_t v12[16]; // [sp+50h] [-40h] BYREF
  int v13; // [sp+60h] [-30h]

  v7 = v5 + 93088;
  v8 = *(_DWORD *)(v5 + 93692) & 0x7FFLL;
  memcpy(v5 + 93492, (__int64)&a1[16 * v8], 0xC0u, a4, a5);
  v9 = 2036 - v8;
  LODWORD(v9) = 16 * (2036 - v8);
  LODWORD(v10) = 16 * v8;
  XeCryptSha(a1, v10, (const uint8_t *)(v7 + 404), 192, &a1[(unsigned int)(16 * (v8 + 12))], v9, v12, 0x10u);
  if ( (unsigned int)XeCryptMemDiff(v12, (unsigned __int8 *)(v7 + 388), 16) != 0 )
  {
    v_MACHINE_CHECK_filter(v13);
    return -939523963;
  }
  else
  {
    *(_DWORD *)(v7 + 328) = 2;
    *(_DWORD *)(v7 + 616) = 0;
    *(_DWORD *)(v7 + 620) = 0;
    sub_254A8();
    v_MACHINE_CHECK_filter(v13);
    return 0;
  }
}


/* ========== [0x25600] sub_25600 (264 bytes) ========== */
__int64 __fastcall sub_25600(const uint8_t *a1, _DWORD *a2)
{
  __int64 v3; // r2
  _DWORD *v4; // r31
  int v5; // r11
  __int64 v6; // r31
  int v7; // r3
  uint8_t v9[16]; // [sp+50h] [-30h] BYREF
  int v10; // [sp+60h] [-20h]

  XeCryptSha(a1, 0x8000, nullptr, 0, nullptr, 0, v9, 0x10u);
  v4 = (_DWORD *)(v3 + 93088);
  if ( (unsigned int)XeCryptMemDiff(v9, (unsigned __int8 *)(16LL * *(unsigned int *)(v3 + 93704) + v3 + 93492), 16) != 0 )
    v5 = 0;
  else
    v5 = v4[155] + 1;
  v4[155] = v5;
  ++v4[154];
  ++v4[151];
  sub_254A8();
  if ( v4[155] == 4 )
  {
    v6 = 0;
LABEL_6:
    v7 = v10;
    *a2 = 1;
    v_MACHINE_CHECK_filter(v7);
    return v6;
  }
  if ( v4[154] == 12 )
  {
    v6 = -939523963;
    goto LABEL_6;
  }
  v_MACHINE_CHECK_filter(v10);
  return 0;
}


/* ========== [0x25708] HvxDvdAuthTestMode (84 bytes) ========== */
__int64 __fastcall HvxDvdAuthTestMode(char a1)
{
  __int64 v1; // r2
  _DWORD *v3; // r31
  __int64 v4; // r2

  v3 = (_DWORD *)(v1 + 93776);
  HvpAquireSpinLock((_DWORD *)(v1 + 93776));
  *(_BYTE *)(v4 + 93713) = a1;
  HvpReleaseSpinLock(v3);
  return 0;
}


/* ========== [0x25760] HvxDvdAuthGetAuthResults (328 bytes) ========== */
__int64 __fastcall HvxDvdAuthGetAuthResults(
        __int64 a1,
        int a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  __int64 v8; // r2
  __int64 v9; // r3
  __int64 v10; // r2
  _BYTE *v11; // r8
  _BYTE *v12; // r7
  __int64 i; // ctr
  _BYTE *v14; // r10
  _BYTE *v15; // r11
  __int64 j; // ctr

  if ( a2 == 1 )
  {
    if ( *(_BYTE *)(v8 + 93712) == 1 )
    {
      v9 = HvpPhysicalToReal(a1, 2052, a3, a4, a5, a6, a7, a8);
      v11 = (_BYTE *)(v9 + 268);
      v12 = (_BYTE *)(v10 + 93378);
      *(_DWORD *)(v9 + 260) = *(_DWORD *)(v10 + 93400);
      *(_BYTE *)(v9 + 20) = *(_BYTE *)(v10 + 93420);
      for ( i = 20; i != 0; --i )
        *v11++ = *v12++;
      v14 = (_BYTE *)(v9 + 8);
      v15 = (_BYTE *)(v10 + 93404);
      for ( j = 12; j != 0; --j )
        *v14++ = *v15++;
      return 0;
    }
    else
    {
      return -939523968;
    }
  }
  else if ( a2 != 0 )
  {
    return -939523958;
  }
  else
  {
    return -939523946;
  }
}


/* ========== [0x258a8] HvxDvdAuthSetDriveAuthResult (72 bytes) ========== */
__int64 __fastcall HvxDvdAuthSetDriveAuthResult(int a1, int a2)
{
  __int64 v2; // r2
  _DWORD *v4; // r31
  __int64 v6; // r2

  v4 = (_DWORD *)(v2 + 93776);
  HvpAquireSpinLock((_DWORD *)(v2 + 93776));
  *(_DWORD *)(v6 + 93728) = a1;
  *(_DWORD *)(v6 + 93732) = a2;
  HvpReleaseSpinLock(v4);
  return 0;
}


/* ========== [0x258f0] HvxDvdAuthSetDiscAuthResult (124 bytes) ========== */
__int64 __fastcall HvxDvdAuthSetDiscAuthResult(
        __int64 a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  _DWORD *v8; // r31
  __int64 v9; // r2
  _DWORD *v10; // r30
  __int64 v11; // r2
  _DWORD *v12; // r11
  __int64 i; // ctr

  v8 = (_DWORD *)HvpPhysicalToReal(a1, 40, a3, a4, a5, a6, a7, a8);
  v10 = (_DWORD *)(v9 + 93776);
  HvpAquireSpinLock((_DWORD *)(v9 + 93776));
  v12 = (_DWORD *)(v11 + 93736);
  for ( i = 10; i != 0; --i )
    *v12++ = *v8++;
  HvpReleaseSpinLock(v10);
  return 0;
}


/* ========== [0x25970] sub_25970 (168 bytes) ========== */
__int64 __fastcall sub_25970(int a1)
{
  __int64 v1; // r2
  _DWORD *v3; // r30
  __int64 v4; // r2
  _DWORD *v5; // r10
  _DWORD *v6; // r9
  __int64 i; // ctr
  _DWORD v9[20]; // [sp+50h] [-50h] BYREF

  v3 = (_DWORD *)(v1 + 93776);
  HvpAquireSpinLock((_DWORD *)(v1 + 93776));
  v9[2] = *(_DWORD *)(v4 + 93716);
  v9[1] = a1;
  v5 = v9;
  v9[0] = 2;
  v6 = (_DWORD *)(v4 + 93736);
  for ( i = 10; i != 0; --i )
    *v6++ = *v5++;
  sub_25138(1, 1);
  HvpReleaseSpinLock(v3);
  return 0;
}


/* ========== [0x25a18] sub_25A18 (116 bytes) ========== */
__int64 __fastcall sub_25A18(int a1, int a2)
{
  __int64 v2; // r2

  if ( a1 != 0 )
  {
    *(_BYTE *)(v2 + 65548) = 0;
    *(_BYTE *)(v2 + 93712) = 0;
    memset(v2 + 93088, 0, 0x270u);
    *(_DWORD *)(v2 + 93716) = 1;
  }
  else
  {
    *(_DWORD *)(v2 + 93716) = a2;
  }
  HvpReleaseSpinLock((_DWORD *)(v2 + 93776));
  return 0;
}


/* ========== [0x25a90] HvxDvdAuthGetMediaId (192 bytes) ========== */
__int64 __fastcall HvxDvdAuthGetMediaId(__int64 a1, int a2, int a3)
{
  __int64 v3; // r2
  _DWORD *v6; // r29
  __int64 v7; // r10
  __int64 v8; // r9
  __int64 v9; // r8
  __int64 v10; // r7
  __int64 v11; // r6
  __int64 v12; // r5
  __int64 v13; // r2
  __int64 v14; // r31
  _BYTE *v15; // r3
  __int64 v16; // r2
  _BYTE *v17; // r11
  __int64 i; // ctr

  if ( (a1 & 0xFFF) != 0 || a2 != 16 )
    return -939523961;
  v6 = (_DWORD *)(v3 + 93776);
  HvpAquireSpinLock((_DWORD *)(v3 + 93776));
  if ( a3 == 0 || *(_BYTE *)(v13 + 65548) != 0 && *(_BYTE *)(v13 + 93712) != 0 )
  {
    v15 = (_BYTE *)HvpPhysicalToReal(a1, 16, v12, v11, v10, v9, v8, v7);
    v17 = (_BYTE *)(v16 + 65552);
    for ( i = 16; i != 0; --i )
      *v15++ = *v17++;
    v14 = 0;
  }
  else
  {
    v14 = -939523960;
  }
  HvpReleaseSpinLock(v6);
  return v14;
}


/* ========== [0x25b50] sub_25B50 (204 bytes) ========== */
__int64 __fastcall sub_25B50(__int64 a1, __int64 a2, __int64 a3)
{
  __int64 v3; // r2
  int v6; // r30
  uint8_t *v7; // r9
  __int64 v8; // r2
  uint8_t *v9; // r31
  uint8_t *v10; // r10
  __int64 i; // ctr
  uint8_t *v12; // r9
  uint8_t *v13; // r10
  __int64 j; // ctr
  X_CRYPT_AES_STATE v16; // [sp+50h] [-1A0h] BYREF
  uint8_t v17[16]; // [sp+1B0h] [-40h] BYREF
  int v18; // [sp+1C0h] [-30h]

  v6 = a3;
  v18 = *(_DWORD *)(v3 + 0x10000);
  XeCryptAesKey(&v16, (const uint8_t *)(v3 + 65568), a3);
  v7 = v17;
  v9 = (uint8_t *)(v8 + 65584);
  v10 = (uint8_t *)(v8 + 65584);
  for ( i = 16; i != 0; --i )
    *v7++ = *v10++;
  XeCryptAesCbc(&v16, (const uint8_t *)(a2 + 16), 0x20u, (uint8_t *)(a2 + 16), v17, v6);
  v12 = v17;
  v13 = v9;
  for ( j = 16; j != 0; --j )
    *v12++ = *v13++;
  XeCryptAesCbc(&v16, (const uint8_t *)(a1 + 2), 0x30u, (uint8_t *)(a1 + 2), v17, v6);
  return v_MACHINE_CHECK_filter(v18);
}


/* ========== [0x25c20] sub_25C20 (232 bytes) ========== */
__int64 __fastcall sub_25C20(__int64 a1, unsigned __int8 *a2)
{
  unsigned __int8 *v4; // r30
  __int64 v5; // r6
  __int64 v6; // r2
  __int64 v7; // r11
  __int64 v8; // r10
  __int64 result; // r3

  v4 = (unsigned __int8 *)(a1 + 2);
  if ( sub_3570((_BYTE *)(a1 + 2), 16) )
    return -939523961;
  if ( sub_3570(a2 + 16, 16) )
    return -939523961;
  sub_25B50(a1, (__int64)a2, 0);
  if ( *a2 >= 6u )
    return -939523961;
  if ( a2[1] >= 0x10u )
    return -939523961;
  v7 = *a2;
  v8 = *(_QWORD *)(v6 + 90832) + 2208LL;
  *(_BYTE *)(v7 + v8) = a2[1];
  *(_DWORD *)(4 * (v7 + 2) + v8) = *((_DWORD *)a2 + 3);
  memcpy(32 * (v7 + 1) + v8, (__int64)v4, 0x20u, v5, v7 + 2);
  if ( (unsigned int)XeCryptMemDiff(v4, a2 + 16, 16) != 0 )
    return -939523961;
  result = sub_3570((_BYTE *)(a1 + 18), 16);
  if ( (_DWORD)result != 0 )
    return -939523961;
  return result;
}


/* ========== [0x25d08] sub_25D08 (236 bytes) ========== */
__int64 __fastcall sub_25D08(__int64 a1, unsigned __int8 *a2)
{
  __int64 v2; // r2
  __int64 v5; // r11
  __int64 v6; // r10
  _BYTE *v7; // r11
  _BYTE *v8; // r9
  __int64 i; // ctr
  _BYTE *v10; // r11
  __int64 j; // ctr

  if ( *a2 >= 6u || a2[1] >= 0x10u )
    return -939523961;
  v5 = *a2;
  v6 = *(_QWORD *)(v2 + 90832) + 2208LL;
  *(_BYTE *)(v5 + v6) = -1;
  *(_DWORD *)(4 * (v5 + 2) + v6) = 259;
  memset(32 * (v5 + 1) + v6, 0, 0x20u);
  v7 = a2 + 16;
  v8 = (_BYTE *)(a1 + 2);
  for ( i = 16; i != 0; --i )
    *v7++ = *v8++;
  v10 = (_BYTE *)(a1 + 18);
  for ( j = 16; j != 0; --j )
    *v10++ = 0;
  sub_25B50(a1, (__int64)a2, 1);
  return 0;
}


/* ========== [0x25df8] sub_25DF8 (100 bytes) ========== */
__int64 __fastcall sub_25DF8(_BYTE *a1, __int64 a2)
{
  unsigned __int8 v3; // r8
  unsigned __int64 v4; // r11

  if ( *(unsigned __int8 *)(a2 + 1) >= 0x10u )
    return -939523961;
  v3 = a1[*(unsigned __int8 *)(a2 + 1) + 2];
  a1[46] = v3 & 1;
  v4 = ((unsigned __int64)v3 >> 1) & 7;
  if ( (_DWORD)v4 != 0 )
    LOBYTE(v4) = 1 << v4;
  a1[47] = v4;
  a1[48] = v3 >> 4;
  return 0;
}


/* ========== [0x25e60] sub_25E60 (220 bytes) ========== */
__int64 __fastcall sub_25E60(_BYTE *a1, __int64 a2)
{
  int v2; // r11
  char v4; // r9
  unsigned int v5; // r11
  unsigned int v6; // r11

  v2 = *a1 & 0x3F;
  if ( v2 == 36 || v2 == 35 )
  {
    if ( a1[46] != 0 && a1[46] != 1 )
      return -939523961;
    v4 = 0;
    v5 = (unsigned __int8)a1[47];
    if ( v5 > 0x10 )
      return -939523961;
    while ( v5 != 0 && (v5 & 1) == 0 )
    {
      v5 >>= 1;
      ++v4;
    }
    if ( (v5 & 0xFFFFFFFE) != 0 )
      return -939523961;
    v6 = (unsigned __int8)a1[47];
    if ( a1[47] == 0 )
      v6 = 1;
    if ( (unsigned __int8)a1[48] >= v6 || *(unsigned __int8 *)(a2 + 1) >= 0x10u )
      return -939523961;
    a1[*(unsigned __int8 *)(a2 + 1) + 2] = (2 * ((8 * a1[48]) | v4 & 7)) | a1[46] & 1;
  }
  else
  {
    *(_BYTE *)(a2 + 1) = 0;
  }
  return 0;
}


/* ========== [0x25f40] sub_25F40 (112 bytes) ========== */
__int64 __fastcall sub_25F40(__int64 a1, __int64 a2)
{
  uint8_t *v4; // r29
  int v5; // r11
  unsigned __int8 *v6; // r4
  __int64 result; // r3
  uint8_t v8[48]; // [sp+50h] [-30h] BYREF

  v4 = (uint8_t *)(a1 + 2);
  v5 = *(_DWORD *)(a1 + 2);
  if ( v5 == -1 )
  {
    sub_20060(v8, 4);
    LOBYTE(v5) = v8[3];
  }
  *(_BYTE *)(a2 + 1) = v5 & 0xF;
  sub_20060(v4, 16);
  result = sub_25E60((_BYTE *)a1, a2);
  if ( (_DWORD)result == 0 )
    return sub_25D08(a1, v6);
  return result;
}


/* ========== [0x25fb0] sub_25FB0 (228 bytes) ========== */
__int64 __fastcall sub_25FB0(_BYTE *a1, __int64 a2)
{
  __int64 v2; // r2
  __int64 v3; // r11
  __int64 result; // r3
  _BYTE *v5; // r10
  __int64 v6; // r7
  unsigned int v7; // r8
  unsigned int v8; // r11
  _BYTE *v9; // r8
  __int64 i; // ctr
  __int64 v11; // r6
  unsigned __int8 *v12; // r4

  v3 = *(_QWORD *)(v2 + 90848);
  if ( *(_BYTE *)v3 == 0 || (*a1 & 0x3F) != ((*(_DWORD *)(v3 + 272) & 1LL) == 0 ? 36 : 41) )
    return -939523959;
  v5 = a1 + 2;
  v6 = *(unsigned int *)(v3 + 284) + v3;
  v7 = *(_DWORD *)(v3 + 280) >> 5;
  v8 = *(_DWORD *)(a1 + 2);
  if ( v8 >= v7 )
    return -939523961;
  v9 = (_BYTE *)(32LL * v8 + v6);
  for ( i = 16; i != 0; --i )
    *v5++ = *v9++;
  *(_BYTE *)(a2 + 1) = v8 & 0xF;
  result = sub_25DF8(a1, a2);
  if ( (_DWORD)result == 0 )
    return sub_25D08(v11, v12);
  return result;
}


/* ========== [0x26098] sub_26098 (352 bytes) ========== */
__int64 __fastcall sub_26098(_BYTE *a1, unsigned __int8 *a2)
{
  __int64 v2; // r2
  __int64 v5; // r30
  __int64 result; // r3
  __int64 v7; // r28
  unsigned int v8; // r29
  unsigned int v9; // r11
  unsigned int v10; // r29
  unsigned __int8 *v11; // r10
  __int64 i; // ctr
  char v13; // r11
  char v14; // r9
  char v15; // r6
  _BYTE *v16; // r10
  _BYTE *v17; // r8
  __int64 j; // ctr
  _BYTE *v19; // r9
  _BYTE *v20; // r10
  __int64 k; // ctr
  unsigned int v22; // [sp+50h] [-40h] BYREF

  v5 = *(_QWORD *)(v2 + 90848);
  if ( *(_BYTE *)v5 == 0 )
    return -939523959;
  v7 = *(unsigned int *)(v5 + 284) + v5;
  v8 = *(_DWORD *)(v5 + 280) >> 5;
  sub_20060((uint8_t *)&v22, 4);
  v9 = v22;
  __twllei(v8, 0);
  v10 = v9 % v8;
  memset((__int64)a1, 0, 0x32u);
  v11 = a2;
  for ( i = 12; i != 0; --i )
  {
    *(_DWORD *)v11 = 0;
    v11 += 4;
  }
  if ( (*(_DWORD *)(v5 + 272) & 1) != 0 )
  {
    v13 = 41;
    v14 = 34;
  }
  else
  {
    v13 = 36;
    v14 = 50;
  }
  v15 = v13 & 0x3F | *a1 & 0xC0;
  v16 = a1 + 2;
  a1[1] = v14 - 2;
  v17 = (_BYTE *)(32LL * v10 + v7);
  *a1 = v15;
  for ( j = 16; j != 0; --j )
    *v16++ = *v17++;
  v19 = (_BYTE *)(32LL * v10 + v7 + 16);
  v20 = a2 + 32;
  for ( k = 16; k != 0; --k )
    *v20++ = *v19++;
  if ( v13 != 36 )
    return sub_25D08((__int64)a1, a2);
  a2[1] = v10 & 0xF;
  result = sub_25DF8(a1, (__int64)a2);
  if ( (_DWORD)result == 0 )
    return sub_25D08((__int64)a1, a2);
  return result;
}


/* ========== [0x261f8] sub_261F8 (124 bytes) ========== */
__int64 __fastcall sub_261F8(__int64 a1, unsigned __int8 *a2)
{
  __int64 v4; // r29

  v4 = sub_25C20(a1, a2);
  if ( (_DWORD)v4 != 0 )
    goto LABEL_5;
  if ( (unsigned int)XeCryptMemDiff((unsigned __int8 *)(a1 + 2), a2 + 16, 16) != 0
    || (unsigned int)XeCryptMemDiff((unsigned __int8 *)(a1 + 18), a2 + 32, 16) != 0 )
  {
    v4 = -939523958;
LABEL_5:
    sub_3830(0, 2048);
    return v4;
  }
  return 0;
}


/* ========== [0x26278] HvxDvdAuthFwcr (424 bytes) ========== */
__int64 HvxDvdAuthFwcr()
{
  __int64 v0; // r2

  v_MACHINE_CHECK_filter(*(_DWORD *)(v0 + 0x10000));
  return -939523959;
}


/* ========== [0x26420] sub_26420 (512 bytes) ========== */
__int64 __fastcall sub_26420(__int64 a1, __int64 a2, __int64 a3, __int64 a4, __int64 a5)
{
  __int64 v5; // r2
  __int64 v6; // r27
  __int64 v7; // r2
  __int64 v8; // r31
  int *v9; // r30
  __int64 v10; // r5
  __int64 v11; // r28
  __int64 v13; // r11
  __int64 v14; // r28
  uint8_t *v15; // r30
  uint8_t *v16; // r9
  uint8_t *v17; // r10
  __int64 i; // ctr
  X_CRYPT_AES_STATE v19; // [sp+60h] [-1D0h] BYREF
  uint8_t v20[32]; // [sp+1C0h] [-70h] BYREF
  uint8_t v21[16]; // [sp+1E0h] [-50h] BYREF
  int v22; // [sp+1F0h] [-40h]

  v22 = *(_DWORD *)(v5 + 0x10000);
  v6 = v5 + 0x10000;
  memcpy(*(_QWORD *)(v5 + 90848), a1, 0x4000u, a4, a5);
  v8 = *(_QWORD *)(v6 + 25312);
  if ( *(int *)(v8 + 272) < 0 )
    v9 = (int *)(v7 + 68312);
  else
    v9 = (int *)(v7 + 70424);
  XeCryptHmacSha(byte_20, 0x10u, (const uint8_t *)(v8 + 256), 64, nullptr, 0, nullptr, 0);
  if ( (unsigned int)sub_38D8((__int64)v20, v8, v9) == 0 )
    goto LABEL_5;
  v13 = *(unsigned int *)(v8 + 284);
  v14 = 0x4000 - v13;
  v15 = (uint8_t *)(v13 + v8);
  XeCryptAesKey(&v19, byte_20, v10);
  v16 = v21;
  v17 = (uint8_t *)(v8 + 256);
  for ( i = 16; i != 0; --i )
    *v16++ = *v17++;
  XeCryptAesCbc(&v19, v15, v14, v15, v21, 0);
  XeCryptSha(v15, v14, nullptr, 0, nullptr, 0, v20, 0x14u);
  if ( (unsigned int)XeCryptMemDiff(v20, (unsigned __int8 *)(v8 + 300), 20) != 0 )
  {
LABEL_5:
    v11 = -939523958;
    memset(*(_QWORD *)(v6 + 25312), 0, 0x4000u);
    sub_3830(8, 0);
  }
  else
  {
    memset(v8, 0, 0x100u);
    v11 = 0;
    *(_DWORD *)(v8 + 256) = 0;
    *(_DWORD *)(v8 + 260) = 0;
    *(_DWORD *)(v8 + 264) = 0;
    *(_DWORD *)(v8 + 268) = 0;
    *(_DWORD *)(v8 + 300) = 0;
    *(_DWORD *)(v8 + 304) = 0;
    *(_DWORD *)(v8 + 308) = 0;
    *(_DWORD *)(v8 + 312) = 0;
    *(_DWORD *)(v8 + 316) = 0;
    *(_BYTE *)v8 = 1;
  }
  sub_3830(0x1000000, 0);
  v_MACHINE_CHECK_filter(v22);
  return v11;
}


/* ========== [0x26620] sub_26620 (404 bytes) ========== */
__int64 sub_26620()
{
  __int64 v0; // r2

  v_MACHINE_CHECK_filter(*(_DWORD *)(v0 + 0x10000));
  return -939523959;
}


/* ========== [0x267b8] HvxDvdAuthFcrt (112 bytes) ========== */
__int64 __fastcall HvxDvdAuthFcrt(
        int a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  __int64 v9; // r3

  if ( (a2 & 0xFFF) != 0 || (_DWORD)a3 != 0x4000 )
    return -939523961;
  LODWORD(v9) = HvpPhysicalToReal(a2, 0x4000, a3, a4, a5, a6, a7, a8);
  if ( a1 == 0 )
    return sub_26420(v9);
  if ( a1 == 1 )
    return sub_26620(v9);
  else
    return -939523961;
}


/* ========== [0x26828] sub_26828 (120 bytes) ========== */
__int64 sub_26828()
{
  __int64 v0; // r2
  __int64 v1; // r2
  __int64 v2; // r2
  __int64 v3; // r2

  sub_20060((uint8_t *)(v0 + 65568), 16);
  sub_20060((uint8_t *)(v1 + 65584), 16);
  sub_20060((uint8_t *)(v2 + 93072), 16);
  sub_3830(0, 2048);
  *(_DWORD *)(v3 + 93728) = 0;
  *(_DWORD *)(v3 + 93736) = 0;
  return sub_25138(1, 1);
}


/* ========== [0x268a0] HvxDvdAuthVerifyNVPage (368 bytes) ========== */
__int64 __fastcall HvxDvdAuthVerifyNVPage(
        __int64 a1,
        int a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  __int64 v8; // r2
  char v10; // r31
  __int64 v11; // r30
  __int64 v12; // r2
  _DWORD *v13; // r29
  __int64 v14; // r5
  __int64 v15; // r2
  uint8_t *v16; // r9
  __int64 v17; // r2
  uint8_t *v18; // r10
  __int64 i; // ctr
  __int64 v20; // r4
  int v21; // r10
  __int64 v22; // r31
  X_CRYPT_AES_STATE v23; // [sp+50h] [-1C0h] BYREF
  uint8_t v24[16]; // [sp+1B0h] [-60h] BYREF
  uint8_t v25[16]; // [sp+1C0h] [-50h] BYREF
  unsigned __int8 v26[16]; // [sp+1D0h] [-40h] BYREF
  int v27; // [sp+1E0h] [-30h]

  v27 = *(_DWORD *)(v8 + 0x10000);
  if ( a2 < 0 )
  {
    v_MACHINE_CHECK_filter(v27);
    return -939523968;
  }
  v10 = 40;
  if ( (*(_WORD *)(*(_QWORD *)(v8 + 90832) + 1540LL) & 0x200) != 0x200 )
    v10 = 59;
  v11 = HvpPhysicalToReal(a1, 74, a3, a4, a5, a6, a7, a8);
  v13 = (_DWORD *)(v12 + 93776);
  HvpAquireSpinLock((_DWORD *)(v12 + 93776));
  if ( (*(_BYTE *)(v11 + 8) & 0x3F) != v10 || *(_BYTE *)(v11 + 9) != 48 )
  {
    v21 = 5;
    goto LABEL_14;
  }
  XeCryptAesKey(&v23, (const uint8_t *)(v15 + 65568), v14);
  v16 = v24;
  v18 = (uint8_t *)(v17 + 65584);
  for ( i = 16; i != 0; --i )
    *v16++ = *v18++;
  XeCryptAesCbc(&v23, (const uint8_t *)(v11 + 10), 0x20u, v25, v24, 0);
  LODWORD(v20) = 16;
  if ( (unsigned int)sub_3570(v25, v20) == 0
    || (unsigned int)XeCryptMemDiff(v26, (unsigned __int8 *)(v15 + 93072), 16) != 0 )
  {
    v21 = 6;
LABEL_14:
    v22 = -939523968;
    goto LABEL_15;
  }
  sub_3830(2048, 0);
  v21 = 7;
  v22 = 0;
LABEL_15:
  *(_DWORD *)(v15 + 93728) = v21;
  *(_DWORD *)(v15 + 93732) = 0;
  HvpReleaseSpinLock(v13);
  v_MACHINE_CHECK_filter(v27);
  return v22;
}


/* ========== [0x26a10] HvxDvdAuthRecordAuthenticationPage (252 bytes) ========== */
__int64 __fastcall HvxDvdAuthRecordAuthenticationPage(__int64 a1)
{
  __int64 v1; // r2
  __int64 v2; // r31
  __int64 v3; // r10
  __int64 v4; // r9
  __int64 v5; // r8
  __int64 v6; // r7
  __int64 v7; // r6
  __int64 v8; // r5
  __int64 v10; // r31
  __int64 v11; // r2
  __int64 v12; // r5
  __int64 v13; // r2
  uint8_t *v14; // r10
  uint8_t *v15; // r9
  __int64 i; // ctr
  __int64 v17; // r30
  __int64 v18; // r2
  X_CRYPT_AES_STATE v19; // [sp+50h] [-190h] BYREF
  uint8_t v20[16]; // [sp+1B0h] [-30h] BYREF
  int v21; // [sp+1C0h] [-20h]

  v2 = a1;
  LODWORD(a1) = 1;
  v21 = *(_DWORD *)(v1 + 0x10000);
  if ( (unsigned int)sub_25970(a1) != 0 )
  {
    v10 = HvpPhysicalToReal(v2, 42, v8, v7, v6, v5, v4, v3);
    XeCryptAesKey(&v19, (const uint8_t *)(v11 + 65568), v12);
    v14 = (uint8_t *)(v10 + 26);
    v15 = v20;
    for ( i = 16; i != 0; --i )
      *v15++ = *v14++;
    v17 = v13 + 93088;
    memcpy(v13 + 93088, v10 + 8);
    XeCryptAesCbc(&v19, (const uint8_t *)(v10 + 10), 0x10u, (uint8_t *)(v17 + 2), v20, 0);
    *(_DWORD *)(v18 + 93716) = 2;
    HvpReleaseSpinLock((_DWORD *)(v18 + 93776));
    v_MACHINE_CHECK_filter(v21);
    return 0;
  }
  else
  {
    v_MACHINE_CHECK_filter(v21);
    return -939523962;
  }
}


/* ========== [0x26b10] HvxDvdAuthRecordXControl (1924 bytes) ========== */
__int64 __fastcall HvxDvdAuthRecordXControl(__int64 a1, __int64 a2)
{
  __int64 v2; // r2
  __int64 v4; // r30
  int v6; // r29
  __int64 v7; // r10
  __int64 v8; // r9
  __int64 v9; // r8
  __int64 v10; // r7
  __int64 v11; // r6
  __int64 v12; // r5
  __int64 v14; // r3
  __int64 v15; // r2
  __int64 v16; // r5
  _BYTE *v17; // r9
  __int64 v18; // r2
  __int64 v19; // r31
  _BYTE *v20; // r10
  __int64 i; // ctr
  uint32_t v22; // r10
  uint8_t *v23; // r9
  __int64 v24; // r2
  __int64 v25; // r4
  _DWORD *v26; // r3
  __int64 v27; // r2
  _DWORD *v28; // r10
  int *v29; // r11
  __int64 i3; // ctr
  __int16 *v31; // r11
  _BYTE *v32; // r10
  __int64 j; // ctr
  __int16 *v34; // r11
  _BYTE *v35; // r10
  __int64 k; // ctr
  __int64 v37; // r2
  __int64 v38; // r5
  __int64 v39; // r2
  unsigned int v40; // r8
  __int64 v41; // r11
  unsigned int v42; // r7
  char v43; // r6
  unsigned __int8 v44; // r10
  unsigned __int8 v45; // r9
  __int64 v46; // r8
  int v47; // r9
  unsigned __int8 v48; // r9
  _DWORD *v49; // r10
  int *v50; // r11
  __int64 kk; // ctr
  _DWORD *v52; // r10
  int *v53; // r11
  __int64 jj; // ctr
  _DWORD *v55; // r10
  int *v56; // r11
  __int64 ii; // ctr
  _DWORD *v58; // r10
  int *v59; // r11
  __int64 n; // ctr
  int v61; // r7
  unsigned __int8 v62; // r8
  __int64 v63; // r5
  __int64 v64; // r6
  _DWORD *v65; // r10
  int *v66; // r11
  __int64 mm; // ctr
  char v68; // r9
  __int64 v69; // r7
  unsigned __int64 v70; // r8
  unsigned __int64 v71; // r8
  __int64 v72; // r10
  unsigned __int8 v73; // r11
  unsigned __int64 v74; // r11
  unsigned __int64 v75; // r10
  _DWORD *v76; // r10
  int *v77; // r11
  __int64 nn; // ctr
  unsigned int v79; // r11
  _DWORD *v80; // r10
  int *v81; // r11
  __int64 i1; // ctr
  __int64 v83; // r11
  char v84; // r11
  unsigned __int64 v85; // r11
  unsigned int v86; // r10
  _BYTE *v87; // r10
  unsigned __int8 *v88; // r11
  __int64 i2; // ctr
  __int64 v90; // r9
  _DWORD *v91; // r9
  int *v92; // r10
  __int64 m; // ctr
  int v94; // [sp+50h] [-9F0h] BYREF
  int v95; // [sp+54h] [-9ECh]
  X_CRYPT_AES_STATE v96; // [sp+80h] [-9C0h] BYREF
  _WORD v97[2]; // [sp+1E0h] [-860h] BYREF
  uint8_t v98[4]; // [sp+1E4h] [-85Ch] BYREF
  __int16 v99; // [sp+1E8h] [-858h] BYREF
  char v100; // [sp+1F4h] [-84Ch]
  uint8_t v101[10]; // [sp+1F5h] [-84Bh] BYREF
  char v102; // [sp+1FFh] [-841h]
  int v103; // [sp+2E4h] [-75Ch]
  __int16 v104; // [sp+2ECh] [-754h] BYREF
  __int16 v105; // [sp+4E4h] [-55Ch] BYREF
  uint8_t v106[348]; // [sp+4E8h] [-558h] BYREF
  __int16 v107; // [sp+644h] [-3FCh] BYREF
  _BYTE v108[5]; // [sp+6C3h] [-37Dh] BYREF
  _QWORD v109[2]; // [sp+9F0h] [-50h] BYREF
  uint8_t v110[20]; // [sp+A00h] [-40h] BYREF
  int v111; // [sp+A14h] [-2Ch]

  v4 = 0;
  v6 = 0;
  v111 = *(_DWORD *)(v2 + 0x10000);
  if ( (unsigned int)sub_25970(2) == 0 )
  {
    v_MACHINE_CHECK_filter(v111);
    return -939523962;
  }
  v14 = HvpPhysicalToReal(a1, 2052, v12, v11, v10, v9, v8, v7);
  memcpy(v97, v14);
  XeCryptAesKey(&v96, (const uint8_t *)(v15 + 65568), v16);
  v17 = v109;
  v19 = v18 + 93088;
  v20 = (_BYTE *)(v18 + 93106);
  for ( i = 16; i != 0; --i )
    *v17++ = *v20++;
  XeCryptAesCbc(&v96, v101, 0x650u, v101, (uint8_t *)v109, 0);
  XeCryptRotSumSha(v98, 1227, nullptr, 0, v110, 0x14u, v23, v22);
  if ( XeCryptBnQwBeSigVerify((__int64)v108, (__int64)v110, v24 + 66912, (_DWORD *)(v24 + 67768)) )
  {
    v31 = &v104;
    v32 = (_BYTE *)(v19 + 290);
    for ( j = 20; j != 0; --j )
    {
      *v32 = *(_BYTE *)v31;
      v31 = (__int16 *)((char *)v31 + 1);
      ++v32;
    }
    v34 = &v99;
    v35 = (_BYTE *)(v19 + 316);
    for ( k = 12; k != 0; --k )
    {
      *v35 = *(_BYTE *)v34;
      v34 = (__int16 *)((char *)v34 + 1);
      ++v35;
    }
    *(_DWORD *)(v19 + 312) = v103;
    *(_DWORD *)(v19 + 328) = 0;
    *(_BYTE *)(v19 + 332) = v100;
    memcpy(v19 + 34, &v105);
    v109[0] = 0;
    v109[1] = 0;
    XeCryptAesKey(&v96, (const uint8_t *)(v37 + 69640), v38);
    XeCryptAesCbc(&v96, v106, 0xFCu, (uint8_t *)(v19 + 38), (uint8_t *)v109, 0);
    LOBYTE(v25) = *(_BYTE *)(v19 + 35);
    if ( *(_BYTE *)(v19 + 34) != 2 || (v40 = *(unsigned __int8 *)(v19 + 35), *(_BYTE *)(v19 + 35) == 0) || v40 > 0x15 )
    {
      HIBYTE(v95) = *(_BYTE *)(v19 + 34);
      BYTE1(v95) = v25;
      v94 = 6;
      v91 = (_DWORD *)(v39 + 93736);
      v92 = &v94;
      for ( m = 10; m != 0; --m )
        *v91++ = *v92++;
LABEL_86:
      v4 = -939523965;
      goto LABEL_87;
    }
    v41 = 0;
    LOBYTE(v42) = 0;
    v43 = *(_BYTE *)(v19 + 338) | 0xF8;
    v44 = 0;
    *(_BYTE *)(v19 + 338) = v43;
    *(_BYTE *)(v19 + 339) = 0;
    v45 = *(_BYTE *)(v19 + 341) & 3;
    *(_WORD *)(v19 + 340) = v45;
    if ( v40 != 0 )
    {
      while ( 1 )
      {
        v46 = 12LL * (unsigned __int8)v42;
        v47 = *(unsigned __int8 *)(v46 + v19 + 38);
        if ( v47 == 1 )
          break;
        switch ( v47 )
        {
          case 20:
          case 21:
            if ( (v41 & 7) == 7 )
            {
              v94 = 7;
              v95 = 3;
              v58 = (_DWORD *)(v39 + 93736);
              v59 = &v94;
              for ( n = 10; n != 0; --n )
                *v58++ = *v59++;
              goto LABEL_86;
            }
            v48 = v41 & 0xF8 | (v41 + 1) & 7;
            goto LABEL_30;
          case 36:
          case 37:
            if ( (v44 & 0xE0u) >= 0xE0 )
            {
              v94 = 7;
              v95 = 4;
              v55 = (_DWORD *)(v39 + 93736);
              v56 = &v94;
              for ( ii = 10; ii != 0; --ii )
                *v55++ = *v56++;
              goto LABEL_86;
            }
            v44 = v44 & 0x1F | ((v44 & 0xE0) + 32) & 0xE0;
            *(_BYTE *)(v19 + 340) = v44;
            break;
          case 224:
            *(_BYTE *)(v19 + 338) = (8 * v42) | v43 & 7;
            *(_DWORD *)(v46 + v19 + 42) = 0;
            v44 = *(_BYTE *)(v19 + 340);
            v41 = *(unsigned __int8 *)(v19 + 339);
            v43 = *(_BYTE *)(v19 + 338);
            LOBYTE(v25) = *(_BYTE *)(v19 + 35);
            break;
          default:
            break;
        }
LABEL_31:
        v42 = (unsigned __int8)(v42 + 1);
        if ( v42 >= (unsigned __int8)v25 )
        {
          v45 = *(_BYTE *)(v19 + 341);
          goto LABEL_33;
        }
      }
      if ( ((unsigned __int8)v41 & 0xF8u) >= 0xF8 )
      {
        v94 = 7;
        v95 = 1;
        v52 = (_DWORD *)(v39 + 93736);
        v53 = &v94;
        for ( jj = 10; jj != 0; --jj )
          *v52++ = *v53++;
        goto LABEL_86;
      }
      v48 = v41 & 7 | ((v41 & 0xF8) + 8) & 0xF8;
LABEL_30:
      v41 = v48;
      *(_BYTE *)(v19 + 339) = v48;
      goto LABEL_31;
    }
LABEL_33:
    if ( (v43 & 0xF8) == 0xF8 )
    {
      v94 = 7;
      v95 = 5;
      v49 = (_DWORD *)(v39 + 93736);
      v50 = &v94;
      for ( kk = 10; kk != 0; --kk )
        *v49++ = *v50++;
      goto LABEL_86;
    }
    v61 = *(unsigned __int8 *)(v39 + 93713);
    *(_BYTE *)(v19 + 336) &= ~0x80u;
    *(_BYTE *)(v19 + 337) &= 7u;
    v62 = *(_BYTE *)(v19 + 342);
    v63 = v62 & 0xC0;
    *(_BYTE *)(v19 + 342) = v62 & 0xC0;
    if ( v61 != 0 )
    {
      v64 = v41 & 7;
      if ( (v44 >> 5) + (unsigned int)(v41 & 7) < 4 )
      {
        v94 = 7;
        v95 = 2;
        v65 = (_DWORD *)(v39 + 93736);
        v66 = &v94;
        for ( mm = 10; mm != 0; --mm )
          *v65++ = *v66++;
        goto LABEL_86;
      }
      v69 = (32LL * (unsigned __int8)v41) | (v44 >> 3) & 0x1C | v45 & 3u;
      v68 = *(_BYTE *)(v19 + 343);
      v70 = ((unsigned int)v41 >> 3) & 0x1F | (unsigned __int64)(v44 & 0xE0);
      *(_BYTE *)(v19 + 341) = v69 & 0xFC;
      *(_BYTE *)(v19 + 342) = v63 & 0x3F;
      *(_BYTE *)(v19 + 340) = v70;
      *(_BYTE *)(v19 + 343) = v68 & 0xF
                            | ((((unsigned __int8)(v69 & 0xFC) >> 1) & 0x70) + ((4 * (v69 & 0xFC)) & 0x70)) & 0xF0;
      goto LABEL_76;
    }
    v71 = v44;
    v69 = (unsigned __int8)v41;
    LODWORD(v41) = v44 >> 5;
    LODWORD(v72) = v69 & 7;
    v64 = v41 + (v69 & 7);
    if ( (_DWORD)v64 == 4 )
    {
      v73 = (32 * v69) | (v71 >> 3) & 0x1C | v45 & 3;
    }
    else
    {
      if ( (unsigned int)v41 >= 2 )
        LOBYTE(v41) = 2;
      v74 = (4 * (_BYTE)v41) & 0x1C | v45 & 0xE3u;
      if ( (int)v72 >= (int)(4 - ((v74 >> 2) & 7)) )
        v72 = 4 - ((v74 >> 2) & 7);
      v73 = (32 * v72) | v74 & 0x1F;
    }
    v75 = v73;
    *(_BYTE *)(v19 + 341) = v73;
    if ( ((v73 >> 2) & 7) + (v73 >> 5) != 4 )
    {
      v94 = 7;
      v95 = 2;
      v76 = (_DWORD *)(v39 + 93736);
      v77 = &v94;
      for ( nn = 10; nn != 0; --nn )
        *v76++ = *v77++;
      goto LABEL_86;
    }
    v79 = (unsigned int)v69 >> 3;
    if ( (unsigned int)v69 >> 3 >= 3 )
      LOBYTE(v79) = 3;
    v70 = v79 & 0x1F | v71 & 0xFFFFFFFFFFFFFFE0LL;
    *(_BYTE *)(v19 + 340) = v70;
    if ( (v70 & 0x1F) == 0 )
    {
      v95 = 0;
      v94 = 7;
      v80 = (_DWORD *)(v39 + 93736);
      v81 = &v94;
      for ( i1 = 10; i1 != 0; --i1 )
        *v80++ = *v81++;
      goto LABEL_86;
    }
    v83 = (v75 >> 2) & 7;
    if ( (_DWORD)v83 == 2 )
    {
      v84 = v75 & 0xFC | 1;
    }
    else
    {
      if ( (_DWORD)v83 != 3 && (_DWORD)v83 != 4 )
        goto LABEL_75;
      v84 = v75 & 0xFC | 2;
    }
    *(_BYTE *)(v19 + 341) = v84;
LABEL_75:
    v63 = (((unsigned __int8)v70 + 255LL) << 6) | v63 & 0xFFFFFFFF0000003FLL;
    *(_BYTE *)(v19 + 343) = *(_BYTE *)(v19 + 343) & 0xF | 0x10;
    *(_BYTE *)(v19 + 342) = v63;
LABEL_76:
    v85 = 0;
    if ( (_BYTE)v25 != 0 )
    {
      LOBYTE(v86) = 0;
      do
      {
        v70 = v85;
        *(_BYTE *)((unsigned __int8)v85 + v19 + 344) = v85;
        v85 = (unsigned __int8)(v86 + 1);
        v86 = (unsigned __int8)(v86 + 1);
      }
      while ( v86 < *(unsigned __int8 *)(v19 + 35) );
    }
    v87 = (_BYTE *)(v39 + 65552);
    v88 = (unsigned __int8 *)&v107;
    for ( i2 = 16; i2 != 0; --i2 )
    {
      v90 = *v88++;
      *v87++ = v90;
    }
    if ( v102 == 6 )
      v6 = 6;
    v26 = (_DWORD *)HvpPhysicalToReal(a2, 4, v63, v64, v69, v70, v90, (__int64)v87);
    *v26 = v6;
    goto LABEL_87;
  }
  v28 = (_DWORD *)(v27 + 93736);
  v94 = 5;
  v29 = &v94;
  for ( i3 = 10; i3 != 0; --i3 )
    *v28++ = *v29++;
  v4 = -939523966;
LABEL_87:
  LODWORD(v25) = 3;
  LODWORD(v26) = v4;
  sub_25A18(v26, v25);
  v_MACHINE_CHECK_filter(v111);
  return v4;
}


/* ========== [0x27298] HvxDvdAuthVerifyAuthPage (1244 bytes) ========== */
__int64 __fastcall HvxDvdAuthVerifyAuthPage(__int64 a1, int a2, __int64 a3, unsigned __int64 a4)
{
  __int64 v4; // r2
  __int64 v9; // r27
  __int64 v10; // r24
  __int64 v11; // r10
  __int64 v12; // r9
  __int64 v13; // r8
  __int64 v14; // r7
  __int64 v15; // r6
  __int64 v16; // r5
  __int64 v17; // r2
  __int64 v19; // r3
  _BYTE *v20; // r31
  unsigned __int64 v21; // r28
  __int64 v22; // r9
  __int64 v23; // r8
  __int64 v24; // r7
  __int64 v25; // r6
  unsigned __int8 *v26; // r5
  _BYTE *v27; // r3
  __int64 v28; // r2
  _BYTE *v29; // r29
  __int64 v30; // r25
  int v31; // r11
  __int64 v32; // r10
  char v33; // r11
  unsigned __int8 v34; // r11
  char v35; // r11
  __int64 v36; // r11
  __int64 v37; // r30
  unsigned __int64 v38; // r11
  uint8_t *v39; // r10
  uint8_t *v40; // r9
  __int64 i; // ctr
  int v42; // r4
  __int16 v43; // r11
  int v44; // r10
  __int64 v45; // r11
  int *v46; // r9
  _BYTE *v47; // r10
  __int64 j; // ctr
  int *v49; // r11
  __int64 k; // ctr
  int *v51; // r9
  _BYTE *v52; // r10
  __int64 m; // ctr
  int *v54; // r11
  __int64 n; // ctr
  __int64 ii; // ctr
  _BYTE *v57; // r10
  __int16 *v58; // r9
  __int64 jj; // ctr
  int *v60; // r11
  __int64 kk; // ctr
  int v62; // [sp+50h] [-230h] BYREF
  int v63; // [sp+54h] [-22Ch]
  unsigned int v64; // [sp+58h] [-228h]
  int v65; // [sp+5Ch] [-224h] BYREF
  __int16 v66; // [sp+60h] [-220h] BYREF
  uint8_t v67[10]; // [sp+82h] [-1FEh] BYREF
  unsigned int v68; // [sp+8Ch] [-1F4h]
  X_CRYPT_AES_STATE v69; // [sp+B0h] [-1D0h] BYREF
  uint8_t v70[16]; // [sp+210h] [-70h] BYREF
  int v71; // [sp+220h] [-60h]

  v9 = 0;
  v10 = 0;
  v71 = *(_DWORD *)(v4 + 0x10000);
  if ( (unsigned int)sub_25970(4) == 0 )
  {
    v_MACHINE_CHECK_filter(v71);
    return -939523962;
  }
  v19 = a1;
  v20 = (_BYTE *)(v17 + 93088);
  v21 = ((unsigned __int64)*(unsigned __int8 *)(v17 + 93424) >> 2) & 0x1F;
  v27 = (_BYTE *)HvpPhysicalToReal(v19, 34, v16, v15, v14, v13, v12, v11);
  v29 = v27;
  v30 = v28 + 93728;
  v31 = *v27 & 0x3F;
  if ( v31 == 62 )
  {
    v32 = (unsigned __int8)v27[1];
    if ( (_DWORD)v32 == 32 )
    {
      if ( (v20[336] & 0x80) != 0 )
      {
        v10 = 1;
        if ( a2 < 0 || (v20[342] & 0xFu) < (unsigned __int8)v20[343] >> 4 )
        {
          v33 = 0;
          v9 = -939523964;
        }
        else
        {
          v33 = 1;
        }
        v32 = v28 + 0x10000;
        *(_BYTE *)(v28 + 93712) = v33;
        goto LABEL_49;
      }
      if ( a2 < 0 )
      {
        if ( a2 == -1073741668 )
        {
          v23 = (__int64)(v20 + 38);
          v22 = 12LL * (unsigned int)v21;
          v9 = -939523964;
          v34 = v20[342];
          if ( v20[v22 + 38] == 1 )
          {
            v32 = (unsigned __int8)v20[342];
            v22 = (unsigned __int8)v32 & 0xC0;
            if ( (v20[342] & 0xC0) != 0 )
            {
              v9 = 0;
              v32 = (((v34 >> 6) - 1LL) << 6) & 0xC0 | v20[342] & 0x3F;
              v20[342] = (((v34 >> 6) - 1) << 6) | v20[342] & 0x3F;
            }
          }
          else
          {
            v32 = (v20[342] & 0xF0) + 16LL;
            v35 = v32 & 0x30 | v34 & 0xCF;
            v20[342] = v35;
            if ( (v35 & 0x30u) <= 0x20 )
            {
              if ( (*(_BYTE *)(v22 + v23) & 0x20) != 0 )
              {
                v36 = (unsigned __int8)v20[341];
                v32 = v36 & 3;
                if ( (v36 & 3) != 0 )
                {
                  v9 = 0;
                  v32 = (unsigned __int8)v36 & 0xFC | (v36 - 1) & 0xFFFFFFFFFFFFFF03LL;
                  v20[341] = v32;
                }
              }
              else
              {
                v9 = 0;
              }
            }
          }
        }
        else
        {
          v9 = -939523964;
        }
        goto LABEL_49;
      }
      v37 = 12LL * (unsigned int)v21;
      v38 = 12 * ((unsigned __int64)(unsigned __int8)v20[338] >> 3);
      v10 = 0;
      *(_DWORD *)&v20[v38 + 42] = __builtin_rotateright32((unsigned __int8)v20[v37 + 39], 8)
                                | (*(_DWORD *)&v20[v38 + 42] >> 8);
      XeCryptAesKey(&v69, (const uint8_t *)(v28 + 65568), (__int64)v26);
      v39 = v29 + 18;
      v40 = v70;
      for ( i = 16; i != 0; --i )
        *v40++ = *v39++;
      XeCryptAesCbc(&v69, v29 + 2, 0x10u, v67, v70, 0);
      v26 = &v20[v37 + 38];
      v42 = *v26;
      if ( (v42 & 0x20) != 0 )
      {
        v43 = (unsigned __int8)v20[v37 + 40];
        v44 = *(_DWORD *)&v20[v37 + 46];
        v23 = __builtin_bswap32(v68);
        LOWORD(v24) = v44 + v43;
        v45 = (__int16)(v44 - v43);
        v25 = (unsigned __int16)v23;
        v22 = v45;
        v24 = (__int16)v24;
        if ( (int)v45 < 0 )
        {
          if ( (unsigned __int16)v23 > 0xB4u )
          {
            v24 = 360;
            LOWORD(v45) = v45 + 360;
          }
          else
          {
            LOWORD(v45) = 0;
          }
        }
        if ( ((v44 ^ (unsigned int)v23) & 0xFFFF0000) == 0 )
        {
          v32 = (unsigned __int16)v23;
          if ( (unsigned __int16)v23 >= (__int16)v45 && (unsigned __int16)v23 <= (__int16)v24 )
            goto LABEL_43;
        }
        v63 = v21;
        v64 = v23;
        v46 = &v65;
        v47 = &v20[v37 + 38];
        v62 = 12;
        for ( j = 12; j != 0; --j )
        {
          *(_BYTE *)v46 = *v47++;
          v46 = (int *)((char *)v46 + 1);
        }
        v49 = &v62;
        v32 = v30 + 8;
        for ( k = 10; k != 0; --k )
        {
          v22 = (unsigned int)*v49++;
          *(_DWORD *)v32 = v22;
          v32 += 4;
        }
      }
      else
      {
        v32 = *(unsigned int *)&v20[v37 + 46];
        if ( (_DWORD)v32 == v68 )
        {
LABEL_43:
          if ( v42 != 1 && a4 >= 0x2625A0 )
          {
            v32 = v20[342] & 0xF0 | (unsigned __int64)(((unsigned __int8)v20[342] + 1) & 0x10F);
            v20[342] = v32;
          }
          goto LABEL_49;
        }
        v64 = v68;
        v63 = v21;
        v51 = &v65;
        v62 = 13;
        v52 = &v20[v37 + 38];
        for ( m = 12; m != 0; --m )
        {
          *(_BYTE *)v51 = *v52++;
          v51 = (int *)((char *)v51 + 1);
        }
        v54 = &v62;
        v32 = v30 + 8;
        for ( n = 10; n != 0; --n )
        {
          v22 = (unsigned int)*v54++;
          *(_DWORD *)v32 = v22;
          v32 += 4;
        }
      }
      v9 = -939523964;
      goto LABEL_43;
    }
  }
  v22 = v28 + 93736;
  BYTE1(v63) = v27[1];
  HIBYTE(v63) = v31;
  v62 = 10;
  v32 = (__int64)&v62;
  for ( ii = 10; ii != 0; --ii )
  {
    *(_DWORD *)v22 = *(_DWORD *)v32;
    v32 += 4;
    v22 += 4;
  }
  v9 = -939523968;
LABEL_49:
  if ( a2 < 0 && (_DWORD)v9 != 0 )
  {
    v63 = a2;
    v64 = v21;
    v57 = &v20[12 * (unsigned int)v21 + 38];
    v58 = &v66;
    v65 = (unsigned __int8)v20[336] >> 7;
    v62 = 11;
    for ( jj = 12; jj != 0; --jj )
    {
      *(_BYTE *)v58 = *v57++;
      v58 = (__int16 *)((char *)v58 + 1);
    }
    v60 = &v62;
    v32 = v30 + 8;
    for ( kk = 10; kk != 0; --kk )
    {
      v22 = (unsigned int)*v60++;
      *(_DWORD *)v32 = v22;
      v32 += 4;
    }
  }
  *(_DWORD *)HvpPhysicalToReal(a3, 4, (__int64)v26, v25, v24, v23, v22, v32) = v10;
  sub_25A18(v9, v10 == 0 ? 3LL : 5LL);
  v_MACHINE_CHECK_filter(v71);
  return v9;
}


/* ========== [0x27778] HvpDvdAuthGetAuthPage (428 bytes) ========== */
__int64 HvpDvdAuthGetAuthPage()
{
  unsigned __int8 *v0; // r2
  unsigned __int8 *v1; // r31
  unsigned __int8 v3; // r9
  unsigned int v4; // r10
  unsigned int v5; // r11
  unsigned __int64 v6; // r10
  unsigned __int8 v7; // r9
  int v8; // r10
  unsigned __int8 v9; // r11
  unsigned __int8 v10; // r11

  v1 = v0 + 93088;
  if ( (v0[93428] & 0x1F) != 0 || (v0[93429] & 0xE0) != 0 || (v0[93429] & 0x1C) != 0 )
  {
    v4 = v0[93123];
    v5 = v0[93425] >> 3;
    if ( v5 == v4 || v5 >= v4 )
    {
      return -939523964;
    }
    else
    {
      while ( 1 )
      {
        v6 = (4 * (unsigned __int8)sub_25210()) & 0x7C | v1[336] & 0x83u;
        v7 = v1[337] & 7 | ((v1[337] & 0xF8) + 8) & 0xF8;
        v1[336] = v6;
        v1[337] = v7;
        v8 = v1[12 * ((v6 >> 2) & 0x1F) + 38];
        if ( v8 == 1 && (v1[340] & 0x1F) != 0 )
        {
          v1[340] = v1[340] & 0xE0 | (v1[340] - 1) & 0x1F;
          return 0;
        }
        v9 = v1[341];
        if ( v8 == 20 && (v1[341] & 0xE0) != 0 || v8 == 21 && (v1[341] & 0xE0) != 0 )
        {
          v10 = (32 * ((v9 >> 5) - 1)) | v1[341] & 0x1F;
          goto LABEL_21;
        }
        if ( v8 == 36 && (v1[341] & 0x1C) != 0 || v8 == 37 && (v1[341] & 0x1C) != 0 )
          break;
        if ( v7 >> 3 >= (unsigned int)v1[35] )
          return -939523964;
      }
      v10 = (4 * ((v9 >> 2) - 1)) & 0x1C | v1[341] & 0xE3;
LABEL_21:
      v1[341] = v10;
      return 0;
    }
  }
  else
  {
    v3 = (8 * v0[93123]) | v0[93425] & 7;
    v0[93424] = (v0[93426] >> 1) & 0x7C | v0[93424] & 3 | 0x80;
    v0[93425] = v3;
    return 0;
  }
}


/* ========== [0x27928] HvxDvdAuthGetNextLBAIndex (324 bytes) ========== */
__int64 __fastcall HvxDvdAuthGetNextLBAIndex(__int64 a1)
{
  __int64 v2; // r30
  __int64 v3; // r10
  __int64 v4; // r9
  __int64 v5; // r8
  __int64 v6; // r7
  __int64 v7; // r6
  __int64 v8; // r5
  _DWORD *v10; // r3
  __int64 v11; // r2
  _DWORD *v12; // r10
  _DWORD *v13; // r11
  __int64 j; // ctr
  int v15; // r11
  int v16; // r11
  _DWORD *v17; // r11
  _DWORD *v18; // r10
  __int64 i; // ctr
  _DWORD v20[10]; // [sp+50h] [-40h] BYREF

  v2 = 0;
  if ( (unsigned int)sub_25970(5) == 0 )
    return -939523962;
  v10 = (_DWORD *)HvpPhysicalToReal(a1, 4, v8, v7, v6, v5, v4, v3);
  if ( *(_BYTE *)(v11 + 93712) != 0 )
  {
    v15 = *(_DWORD *)(v11 + 93416);
    if ( v15 != 0 )
    {
      if ( v15 == 1 )
      {
        v16 = *(_DWORD *)(v11 + 93696);
      }
      else
      {
        if ( v15 != 2 )
        {
          v20[1] = *(_DWORD *)(v11 + 93416);
          v17 = v20;
          v20[0] = 16;
          v18 = (_DWORD *)(v11 + 93736);
          for ( i = 10; i != 0; --i )
            *v18++ = *v17++;
          v2 = -939523963;
          *(_DWORD *)(v11 + 93416) = 0;
          goto LABEL_17;
        }
        v16 = *(_DWORD *)(v11 + 93700);
      }
    }
    else
    {
      v16 = *(_DWORD *)(v11 + 93400);
    }
    *v10 = v16;
  }
  else
  {
    v12 = (_DWORD *)(v11 + 93736);
    v20[0] = 15;
    v13 = v20;
    for ( j = 10; j != 0; --j )
      *v12++ = *v13++;
    v2 = -939523964;
  }
LABEL_17:
  sub_25A18(v2, 6);
  return v2;
}


/* ========== [0x27a70] HvxDvdAuthVerifyLBA (640 bytes) ========== */
__int64 __fastcall HvxDvdAuthVerifyLBA(__int64 a1, int a2, __int64 a3)
{
  __int64 v3; // r27
  __int64 v7; // r10
  __int64 v8; // r9
  __int64 v9; // r8
  __int64 v10; // r7
  __int64 v11; // r6
  __int64 v12; // r5
  int *v14; // r10
  __int64 v15; // r9
  __int64 v16; // r8
  __int64 v17; // r7
  __int64 v18; // r6
  __int64 v19; // r5
  __int64 v20; // r3
  __int64 v21; // r2
  __int64 v22; // r28
  _DWORD *v23; // r31
  int v24; // r11
  int v25; // r30
  __int64 v26; // r3
  __int64 v27; // r29
  __int64 v28; // r3
  int v29; // r11
  int *v30; // r11
  _DWORD *v31; // r10
  __int64 k; // ctr
  int v33; // r11
  int *v34; // r11
  _DWORD *v35; // r10
  __int64 i; // ctr
  __int64 j; // ctr
  _DWORD v38[4]; // [sp+50h] [-70h] BYREF
  int v39; // [sp+60h] [-60h] BYREF
  int v40; // [sp+64h] [-5Ch]
  int v41; // [sp+68h] [-58h]
  int v42; // [sp+6Ch] [-54h]
  int v43; // [sp+70h] [-50h]
  int v44; // [sp+74h] [-4Ch]
  int v45; // [sp+78h] [-48h]
  int v46; // [sp+7Ch] [-44h]
  int v47; // [sp+80h] [-40h]
  int v48; // [sp+84h] [-3Ch]

  v3 = 0;
  if ( (unsigned int)sub_25970(6) == 0 )
    return -939523962;
  LODWORD(v20) = HvpPhysicalToReal(a1, 0x8000, v12, v11, v10, v9, v8, v7);
  v22 = v21 + 93728;
  if ( a2 < 0 )
  {
    v40 = a2;
    v39 = 17;
    v23 = (_DWORD *)(v21 + 93088);
    v41 = *(_DWORD *)(v21 + 93416);
    if ( v41 != 0 )
    {
      if ( v41 == 1 )
      {
        v33 = *(_DWORD *)(v21 + 93696);
      }
      else if ( v41 == 2 )
      {
        v33 = *(_DWORD *)(v21 + 93700);
      }
      else
      {
        v33 = -1;
      }
    }
    else
    {
      v33 = *(_DWORD *)(v21 + 93400);
    }
    v42 = v33;
    v34 = &v39;
    v35 = (_DWORD *)(v21 + 93736);
    for ( i = 10; i != 0; --i )
    {
      v15 = (unsigned int)*v34++;
      *v35++ = v15;
    }
    v27 = -939523963;
    goto LABEL_33;
  }
  v23 = (_DWORD *)(v21 + 93088);
  v24 = *(_DWORD *)(v21 + 93416);
  v25 = v24;
  if ( v24 != 0 )
  {
    if ( v24 != 1 )
    {
      if ( v24 == 2 )
      {
        v28 = sub_25600(v20, v38);
        v3 = v38[0];
        v27 = v28;
      }
      else
      {
        v27 = -939523963;
        *(_DWORD *)(v21 + 93416) = 0;
      }
      goto LABEL_12;
    }
    v26 = sub_25530(v20);
  }
  else
  {
    v26 = sub_25298(v20);
  }
  v27 = v26;
LABEL_12:
  if ( (_DWORD)v27 == 0 )
  {
    if ( (_DWORD)v3 != 0 )
    {
      v15 = v22 + 8;
      v23[82] = 0;
      *(_BYTE *)(v21 + 65548) = 1;
      v14 = &v39;
      v39 = 20;
      for ( j = 10; j != 0; --j )
      {
        *(_DWORD *)v15 = *v14++;
        v15 += 4;
      }
    }
    goto LABEL_37;
  }
  v40 = v27;
  v41 = v25;
  v39 = 18;
  v43 = v23[151];
  v44 = v23[89];
  v45 = v23[149];
  v46 = v23[91];
  v47 = v23[155];
  v48 = v23[154];
  if ( v25 != 0 )
  {
    if ( v25 == 1 )
    {
      v29 = v23[152];
    }
    else if ( v25 == 2 )
    {
      v29 = v23[153];
    }
    else
    {
      v29 = -1;
    }
  }
  else
  {
    v29 = v23[78];
  }
  v42 = v29;
  v30 = &v39;
  v31 = (_DWORD *)(v22 + 8);
  for ( k = 10; k != 0; --k )
  {
    v15 = (unsigned int)*v30++;
    *v31++ = v15;
  }
LABEL_33:
  v14 = (int *)(v21 + 0x10000);
  v23[82] = 0;
  *(_BYTE *)(v21 + 65548) = 0;
LABEL_37:
  *(_DWORD *)HvpPhysicalToReal(a3, 4, v19, v18, v17, v16, v15, (__int64)v14) = v3;
  sub_25A18(v27, v3 == 0 ? 5LL : 7LL);
  return v27;
}


/* ========== [0x27cf0] HvxDvdAuthGetAuthPage (492 bytes) ========== */
__int64 __fastcall HvxDvdAuthGetAuthPage(__int64 a1)
{
  __int64 v1; // r2
  __int64 v4; // r3
  __int64 v5; // r2
  __int64 v6; // r30
  _DWORD *v7; // r10
  _DWORD *v8; // r11
  __int64 i; // ctr
  _DWORD *v10; // r9
  __int64 j; // ctr
  char v12; // r8
  unsigned __int64 v13; // r10
  __int64 v14; // r10
  int v15; // r9
  uint8_t v16; // r7
  __int16 *v17; // r9
  _BYTE *v18; // r10
  __int64 k; // ctr
  uint8_t *v20; // r10
  uint8_t *v21; // r11
  __int64 m; // ctr
  __int64 v23; // r10
  __int64 v24; // r9
  __int64 v25; // r8
  __int64 v26; // r7
  __int64 v27; // r6
  __int64 v28; // r5
  __int64 v29; // r3
  _BYTE v30[2]; // [sp+50h] [-1F0h] BYREF
  uint8_t v31[6]; // [sp+52h] [-1EEh] BYREF
  int v32; // [sp+58h] [-1E8h]
  __int16 v33; // [sp+62h] [-1DEh] BYREF
  _DWORD v34[12]; // [sp+80h] [-1C0h] BYREF
  X_CRYPT_AES_STATE v35; // [sp+B0h] [-190h] BYREF
  uint8_t v36[16]; // [sp+210h] [-30h] BYREF
  int v37; // [sp+220h] [-20h]

  v37 = *(_DWORD *)(v1 + 0x10000);
  if ( (unsigned int)sub_25970(3) != 0 )
  {
    v4 = HvpDvdAuthGetAuthPage();
    v6 = v4;
    if ( (_DWORD)v4 != 0 )
    {
      v34[1] = v4;
      v34[0] = 8;
      v7 = (_DWORD *)(v5 + 93736);
      v8 = v34;
      for ( i = 10; i != 0; --i )
        *v7++ = *v8++;
    }
    else
    {
      v10 = v30;
      for ( j = 8; j != 0; --j )
        *v10++ = 0;
      *(_WORD *)v10 = 0;
      v30[1] = 32;
      v12 = v30[0] & 0xC0 | 0x3E;
      v13 = *(unsigned __int8 *)(v5 + 93424);
      v30[0] = v12;
      v31[1] = 1;
      v14 = 12 * ((v13 >> 2) & 0x1F);
      v31[3] = 0xE1u;
      v31[4] = 3;
      v15 = *(unsigned __int8 *)(v14 + v5 + 93126);
      v16 = *(_BYTE *)(v14 + v5 + 93127);
      LODWORD(v14) = *(_DWORD *)(v14 + v5 + 93130);
      v31[5] = v16;
      v32 = v14;
      if ( v15 == 20 || v15 == 36 )
        v30[0] = v12 | 0x40;
      v17 = &v33;
      v18 = (_BYTE *)(v5 + 93106);
      for ( k = 16; k != 0; --k )
      {
        *(_BYTE *)v17 = *v18++;
        v17 = (__int16 *)((char *)v17 + 1);
      }
      v20 = v36;
      v21 = (uint8_t *)(v5 + 93106);
      for ( m = 16; m != 0; --m )
        *v20++ = *v21++;
      XeCryptAesKey(&v35, (const uint8_t *)(v5 + 65568), v5 + 93130);
      XeCryptAesCbc(&v35, v31, 0x10u, v31, v36, 1);
      v29 = HvpPhysicalToReal(a1, 34, v28, v27, v26, v25, v24, v23);
      memcpy(v29, v30);
    }
    sub_25A18(v6, 4);
    v_MACHINE_CHECK_filter(v37);
    return v6;
  }
  else
  {
    v_MACHINE_CHECK_filter(v37);
    return -939523962;
  }
}


/* ========== [0x27ee0] sub_27EE0 (40 bytes) ========== */
__int64 sub_27EE0()
{
  __int64 result; // r3
  __int64 v1; // r2

  result = sub_2C8();
  *(_QWORD *)(v1 + 93056) = result;
  return result;
}


/* ========== [0x27f08] sub_27F08 (188 bytes) ========== */
_DWORD *sub_27F08()
{
  __int64 v0; // r2
  _DWORD *v1; // r31
  __int64 v2; // r2
  __int64 v4; // [sp+50h] [-20h]

  v1 = (_DWORD *)(v0 + 90820);
  HvpAquireSpinLock((_DWORD *)(v0 + 90820));
  *(_QWORD *)(*(_QWORD *)(v2 + 90832) + 1608LL) = v4;
  return HvpReleaseSpinLock(v1);
}


/* ========== [0x27fc8] sub_27FC8 (128 bytes) ========== */
__int64 __fastcall sub_27FC8(__int64 result, __int64 a2, __int64 a3, __int64 a4, __int64 a5)
{
  __int64 v5; // r2
  __int64 v6; // r30
  __int64 v7; // r4
  unsigned __int64 v8; // r5
  __int64 v9; // r31
  __int64 v10; // r3
  bool v11; // zf

  if ( (_DWORD)result != 0 )
  {
    v6 = v5 + 0x10000;
    v7 = *(_QWORD *)(v5 + 92904);
    if ( v7 != 0 )
    {
      v8 = *(unsigned int *)(v5 + 92912);
      if ( (_DWORD)v8 != 0 )
      {
        v9 = (int)result;
        v11 = (int)result <= 0;
        v10 = (int)result + v7;
        if ( v11 )
          result = memcpy(v10, v7, v8, a4, a5);
        else
          result = sub_ADC0(v10, v7, *(unsigned int *)(v5 + 92912), a4, a5);
        *(_QWORD *)(v6 + 27368) += v9;
      }
    }
  }
  return result;
}


/* ========== [0x28048] sub_28048 (560 bytes) ========== */
__int64 __fastcall sub_28048(int *a1, _BYTE *a2)
{
  __int64 v2; // r2
  __int64 v3; // r30
  int v4; // r11
  __int64 v5; // r10
  __int64 v6; // r8
  _BYTE *v7; // r10
  int *v8; // r31
  _BYTE *v9; // r9
  __int64 k; // ctr
  _BYTE *v11; // r11
  __int64 i; // ctr
  __int64 j; // ctr
  _BYTE *v14; // r11
  _BYTE *v15; // r10
  __int64 m; // ctr
  char *v17; // r11
  char *v18; // r10
  __int64 n; // ctr
  _QWORD v21[2]; // [sp+60h] [-40h] BYREF
  _WORD v22[10]; // [sp+70h] [-30h] BYREF
  int v23; // [sp+84h] [-1Ch]

  v3 = (__int64)(a1 + 8);
  v4 = *a1;
  v5 = *(_QWORD *)(v2 + 91072);
  v23 = *(_DWORD *)(v2 + 0x10000);
  v6 = v5 + 2504;
  switch ( v4 )
  {
    case 1145128268:
      if ( (~*((_WORD *)a1 + 3) & 0x8000) != 0 )
        v8 = (int *)(v2 + 69880);
      else
        v8 = (int *)(v2 + 68584);
      v11 = (_BYTE *)(v5 + 240);
      for ( i = 16; i != 0; --i )
        *a2++ = *v11++;
      break;
    case 1145128269:
      if ( (~*((_WORD *)a1 + 3) & 0x8000) != 0 )
        v8 = (int *)(v2 + 70424);
      else
        v8 = (int *)(v2 + 68312);
      for ( j = 16; j != 0; --j )
        *a2++ = 0;
      break;
    case 1145128272:
      v7 = (_BYTE *)&byte_54;
      v8 = (int *)(v2 + 67768);
      v9 = a2;
      for ( k = 16; k != 0; --k )
        *v9++ = *v7++;
      break;
    default:
LABEL_30:
      v_MACHINE_CHECK_filter(v23);
      return -939523950;
  }
  if ( *v8 != 32 && *v8 != 64 )
    goto LABEL_30;
  if ( (*((_WORD *)a1 + 3) & 2) != 0 )
  {
    v14 = (_BYTE *)(v6 + 2);
    v15 = v21;
    for ( m = 5; m != 0; --m )
      *v15++ = *v14++;
    v17 = (char *)(v6 + 7);
    v18 = (char *)v21 + 5;
    for ( n = 11; n != 0; --n )
      *v18++ = *v17++;
  }
  else
  {
    v21[0] = 0;
    v21[1] = 0;
  }
  XeCryptHmacSha((const uint8_t *)v21, 0x10u, (const uint8_t *)a1, 32, nullptr, 0, nullptr, 0);
  if ( (unsigned int)sub_38D8((__int64)v22, v3, v8) != 0 )
  {
    v_MACHINE_CHECK_filter(v23);
    return 0;
  }
  else
  {
    v_MACHINE_CHECK_filter(v23);
    return -939523949;
  }
}


/* ========== [0x28278] sub_28278 (156 bytes) ========== */
__int64 __fastcall sub_28278(__int64 a1, const uint8_t *a2)
{
  __int64 v2; // r2
  unsigned __int8 v5[16]; // [sp+60h] [-30h] BYREF
  int v6; // [sp+70h] [-20h]

  v6 = *(_DWORD *)(v2 + 0x10000);
  XeCryptHmacSha(a2, 0x10u, (const uint8_t *)(a1 + 288), 16, (const uint8_t *)(a1 + 304), 16, nullptr, 0);
  if ( (unsigned int)XeCryptMemDiff(v5, (unsigned __int8 *)(a1 + 320), 16) != 0 )
  {
    v_MACHINE_CHECK_filter(v6);
    return -939523951;
  }
  else
  {
    v_MACHINE_CHECK_filter(v6);
    return 0;
  }
}


/* ========== [0x28318] sub_28318 (152 bytes) ========== */
__int64 __fastcall sub_28318(__int64 a1)
{
  uint8_t v3[20]; // [sp+50h] [-30h] BYREF
  int v4; // [sp+64h] [-1Ch]

  XeCryptSha((const uint8_t *)(a1 + 336), *(unsigned __int16 *)(a1 + 4) - 336LL, nullptr, 0, nullptr, 0, v3, 0x14u);
  if ( (unsigned int)XeCryptMemDiff(v3, (unsigned __int8 *)(a1 + 12), 20) != 0 )
  {
    v_MACHINE_CHECK_filter(v4);
    return -939523951;
  }
  else
  {
    v_MACHINE_CHECK_filter(v4);
    return 0;
  }
}


/* ========== [0x283b0] sub_283B0 (356 bytes) ========== */
__int64 __fastcall sub_283B0(__int64 a1, const uint8_t *a2, __int64 a3)
{
  __int64 v3; // r2
  int v6; // r30
  int v7; // r10
  uint32_t v8; // r26
  __int64 v9; // r25
  __int64 v11; // r30
  __int64 v12; // r31
  X_CRYPT_AES_STATE v13; // [sp+50h] [-1C0h] BYREF
  _QWORD v14[2]; // [sp+1B0h] [-60h] BYREF
  int v15; // [sp+1C0h] [-50h]

  v6 = a3;
  v7 = *(_WORD *)(a1 + 288) & 1;
  v15 = *(_DWORD *)(v3 + 0x10000);
  if ( v7 != 0 )
    a2 = byte_20;
  v8 = *(unsigned __int16 *)(a1 + 4) - 336;
  if ( (_DWORD)a3 != 0 )
  {
    v9 = sub_28278(a1, a2);
    if ( (_DWORD)v9 != 0 || (_DWORD)(v9 = sub_28318(a1)) != 0 )
    {
      v_MACHINE_CHECK_filter(v15);
      return v9;
    }
  }
  v14[0] = 0;
  v14[1] = 0;
  XeCryptAesKey(&v13, a2, a3);
  XeCryptAesCbc(&v13, (const uint8_t *)(a1 + 304), 0x10u, (uint8_t *)(a1 + 304), (uint8_t *)v14, v6);
  XeCryptAesCbc(&v13, (const uint8_t *)(a1 + 320), 0x10u, (uint8_t *)(a1 + 320), (uint8_t *)v14, v6);
  XeCryptAesCbc(&v13, (const uint8_t *)(a1 + 336), v8, (uint8_t *)(a1 + 336), (uint8_t *)v14, v6);
  if ( v6 != 0 )
    goto LABEL_12;
  v11 = sub_28278(a1, a2);
  if ( (_DWORD)v11 != 0 )
  {
    v_MACHINE_CHECK_filter(v15);
    return v11;
  }
  v12 = sub_28318(a1);
  if ( (_DWORD)v12 == 0 )
  {
LABEL_12:
    v_MACHINE_CHECK_filter(v15);
    return 0;
  }
  else
  {
    v_MACHINE_CHECK_filter(v15);
    return v12;
  }
}


/* ========== [0x28518] sub_28518 (184 bytes) ========== */
__int64 __fastcall sub_28518(__int64 a1, unsigned int a2)
{
  __int64 v2; // r2
  __int64 v6; // r30
  __int64 v7; // r31
  uint8_t v8[16]; // [sp+50h] [-40h] BYREF
  int v9; // [sp+60h] [-30h]

  v9 = *(_DWORD *)(v2 + 0x10000);
  if ( a2 < 0x150 )
    goto LABEL_2;
  v6 = sub_28048((int *)a1, v8);
  if ( (_DWORD)v6 != 0 )
  {
    v_MACHINE_CHECK_filter(v9);
    return v6;
  }
  else if ( *(_WORD *)(a1 + 8) == 1 )
  {
    if ( a2 < *(unsigned __int16 *)(a1 + 4) )
    {
LABEL_2:
      v_MACHINE_CHECK_filter(v9);
      return -939523951;
    }
    v7 = sub_283B0(a1, v8, 0);
    v_MACHINE_CHECK_filter(v9);
    return v7;
  }
  else
  {
    v_MACHINE_CHECK_filter(v9);
    return -939523948;
  }
}


/* ========== [0x285d0] sub_285D0 (120 bytes) ========== */
__int64 __fastcall sub_285D0(__int64 a1, __int64 a2)
{
  __int64 v2; // r2

  if ( *(_QWORD *)(a1 + 304) != a2 || *(unsigned __int8 *)(a1 + 319) != *(unsigned __int8 *)(v2 + 91680) )
    return -939523951;
  sub_3830(0x200000, 0);
  return 0;
}


/* ========== [0x28648] sub_28648 (164 bytes) ========== */
void __fastcall sub_28648(__int64 a1, __int64 a2)
{
  const uint8_t *v3; // r30
  uint8_t *v4; // r31
  __int64 v5; // r3
  __int64 v6; // r2

  v3 = (const uint8_t *)(a1 + 288);
  v4 = (uint8_t *)(a1 + 304);
  sub_20060((uint8_t *)(a1 + 288), 16);
  *(_WORD *)v3 |= 1u;
  LODWORD(v5) = (unsigned int)sub_20060(v4, 16);
  *(_QWORD *)v4 = a2;
  v4[15] = *(_BYTE *)(v6 + 91680);
  if ( (unsigned int)((__int64 (__fastcall *)(__int64))loc_2B70)(v5) != 0 )
    ++v4[15];
  XeCryptHmacSha(byte_20, 0x10u, v3, 16, v4, 16, nullptr, 0);
}


/* ========== [0x286f0] sub_286F0 (72 bytes) ========== */
__int64 __fastcall sub_286F0(_DWORD *a1, __int64 a2)
{
  __int64 v2; // r2
  __int64 v3; // r11
  int v4; // r11

  v3 = *(_QWORD *)(v2 + 91904);
  if ( v3 == 0 || (_DWORD)a2 == 0 )
    return 0;
  v4 = *(_DWORD *)(v3 + 12);
  while ( *a1 != v4 )
  {
    --a2;
    ++a1;
    if ( (_DWORD)a2 == 0 )
      return 0;
  }
  return 1;
}


/* ========== [0x28738] sub_28738 (204 bytes) ========== */
unsigned __int16 *__fastcall sub_28738(__int64 a1)
{
  __int64 v1; // r2
  __int64 v2; // r30
  unsigned __int16 *v3; // r31
  unsigned __int8 *v4; // r29
  __int64 v5; // r26
  unsigned __int8 *v6; // r28
  unsigned __int8 *v7; // r27
  unsigned __int16 v8; // r11
  unsigned __int8 *v9; // r3

  v2 = *(unsigned __int16 *)(a1 + 2);
  v3 = (unsigned __int16 *)(a1 + 16);
  v4 = nullptr;
  if ( *(_WORD *)(a1 + 2) == 0 )
    return nullptr;
  v5 = v1 + 0x10000;
  v6 = (unsigned __int8 *)(v1 + 65552);
  v7 = (unsigned __int8 *)(v1 + 66952);
  while ( 1 )
  {
    v8 = v3[2];
    if ( (v8 & 2) == 0 )
      break;
    if ( (unsigned int)sub_286F0((_DWORD *)v3 + 8, v3[1]) != 0 )
      v4 = v7;
LABEL_13:
    --v2;
    v3 = (unsigned __int16 *)((char *)v3 + *v3);
    if ( (_DWORD)v2 == 0 )
      return nullptr;
  }
  if ( v4 != nullptr )
  {
    v9 = v4;
  }
  else if ( (v8 & 1) != 0 )
  {
    v9 = *(unsigned __int8 **)(v5 + 26352);
  }
  else
  {
    v9 = v6;
  }
  if ( v9 == nullptr || (unsigned int)XeCryptMemDiff(v9, (unsigned __int8 *)v3 + 16, 16) != 0 )
    goto LABEL_13;
  return v3;
}


/* ========== [0x28808] sub_28808 (408 bytes) ========== */
__int64 sub_28808()
{
  return -939523946;
}


/* ========== [0x289a0] sub_289A0 (212 bytes) ========== */
__int64 __fastcall sub_289A0(__int64 a1, unsigned int a2, __int64 a3, __int64 a4, __int64 a5)
{
  __int64 v5; // r2
  __int64 v8; // r30
  __int64 v9; // r11
  __int64 v10; // r10
  __int64 result; // r3
  __int64 v12; // r30

  v8 = *(unsigned int *)(v5 + 92900);
  v9 = *(_QWORD *)(v5 + 92920);
  v10 = *(unsigned int *)(v5 + 92912);
  if ( v9 == 0 || *(_QWORD *)(v5 + 92904) == 0 || (_DWORD)v8 == 0 || (_DWORD)v10 == 0 )
    return -939523946;
  if ( (int)v10 + (int)v8 > a2 )
    return -939524093;
  memcpy(a1, v9, v10 + v8, a4, a5);
  v12 = (unsigned int)v8 + a1;
  sub_28648(a1, a3);
  sub_28648(v12, a3);
  result = sub_283B0(a1, nullptr, 1);
  if ( (_DWORD)result == 0 )
    return sub_283B0(v12, nullptr, 1);
  return result;
}


/* ========== [0x28a78] sub_28A78 (336 bytes) ========== */
__int64 __fastcall sub_28A78(unsigned __int16 *a1, unsigned int a2)
{
  __int64 result; // r3
  __int64 v4; // r2
  __int64 v5; // r28
  unsigned __int64 v6; // r30
  __int64 v7; // r6
  __int64 v8; // r5
  __int64 v9; // r7
  int v10; // r11
  __int64 v11; // r29
  __int64 v12; // r11
  __int64 v13; // r10
  unsigned int v14; // r11
  __int64 v15; // r2
  __int64 v16; // r31

  result = sub_28518((__int64)a1, a2);
  if ( (_DWORD)result == 0 )
  {
    if ( (a1[144] & 1) != 0 )
    {
      return -939523951;
    }
    else
    {
      v5 = v4 + 0x10000;
      v6 = a1[2];
      v7 = v4 + 0x10000;
      v8 = a1[171];
      v9 = *(unsigned int *)(v4 + 92900);
      if ( (a1[3] & 1LL) != 0 )
        v10 = *(_DWORD *)(v4 + 92900);
      else
        v10 = *(_DWORD *)(v4 + 92912);
      if ( (unsigned int)v6 <= 0x10000 - v10 )
      {
        v11 = v4 + 0x10000;
        if ( (a1[3] & 1LL) != 0 )
          v12 = *(_QWORD *)(v4 + 92904);
        else
          v12 = *(_QWORD *)(v4 + 92920);
        if ( v12 != 0 )
        {
          v13 = *(unsigned __int16 *)(v12 + 4);
          v14 = *(unsigned __int16 *)(v12 + 342);
        }
        else
        {
          v13 = 0;
          v14 = 0xFFFFFFFF;
        }
        if ( (unsigned int)v8 > v14 )
        {
          if ( (a1[3] & 1LL) != 0 )
          {
            *(_DWORD *)(v4 + 92912) = v6;
            v16 = (unsigned int)v9 + *(_QWORD *)(v4 + 93056);
            *(_QWORD *)(v4 + 92904) = v16;
          }
          else
          {
            sub_27FC8(v6 - v13, 0, v8, v7, v9);
            *(_DWORD *)(v5 + 27364) = v6;
            v16 = *(_QWORD *)(v15 + 93056);
            *(_QWORD *)(v11 + 27384) = v16;
          }
          memcpy(v16, (__int64)a1, v6, v7, v9);
          memset(v16 + 288, 0, 0x30u);
          return 0;
        }
        else
        {
          return -939523947;
        }
      }
      else
      {
        return -939524093;
      }
    }
  }
  return result;
}


/* ========== [0x28bc8] sub_28BC8 (116 bytes) ========== */
__int64 __fastcall sub_28BC8(int a1)
{
  __int64 v1; // r2
  __int64 v2; // r11
  __int64 result; // r3
  unsigned __int16 sender_lr_6; // [sp+16h] [+16h]

  sender_lr_6 = a1;
  v2 = *(_QWORD *)(v1 + 91688) + 24LL;
  if ( *(_BYTE *)v2 == 0 )
    return -939523946;
  if ( HIWORD(a1) <= (unsigned int)*(unsigned __int16 *)(*(_QWORD *)(v1 + 91688) + 28LL) )
  {
    result = 0;
  }
  else
  {
    *(_WORD *)(*(_QWORD *)(v1 + 91688) + 28LL) = HIWORD(a1);
    result = -939523940;
  }
  if ( sender_lr_6 > (unsigned int)*(unsigned __int16 *)(v2 + 6) )
  {
    *(_WORD *)(v2 + 6) = sender_lr_6;
    return -939523940;
  }
  return result;
}


/* ========== [0x28c40] sub_28C40 (852 bytes) ========== */
__int64 sub_28C40()
{
  _QWORD *v0; // r2
  _QWORD *v1; // r29
  __int64 v2; // r31
  __int64 v3; // r10
  __int64 v4; // r11
  unsigned int v5; // r30
  __int64 v6; // r3
  unsigned int v7; // r11
  __int64 v8; // r7
  __int64 v9; // r6
  unsigned __int64 v10; // r5
  unsigned __int16 *v11; // r3
  _BYTE *v12; // r2
  unsigned int v13; // r11
  unsigned __int16 *v15; // r28
  _BYTE *v16; // r26
  _QWORD *v17; // r10
  __int64 i; // ctr
  unsigned int v19; // r27
  unsigned int v20; // r30
  unsigned int v21; // r10
  unsigned int v22; // r9
  int v23; // r11
  unsigned int v24; // r31
  __int64 v25; // r11
  unsigned __int64 Timebase; // r3
  unsigned __int64 v27; // r11
  unsigned int v28; // r31
  unsigned int v29; // r11
  unsigned int v30; // r10
  unsigned int v31; // r31
  unsigned int v32; // r11
  unsigned int v33; // r30
  int v34; // r10
  unsigned __int64 v35; // r11
  unsigned __int64 v36; // r9
  __int64 v37; // r31
  __int64 *v38; // r29
  int v39; // r11
  __int64 v40; // r8
  unsigned int v41; // r7
  unsigned int v42; // r11
  __int64 v43; // r11
  __int64 v44; // r9
  unsigned int v45; // r9
  int v46; // r11
  unsigned __int64 v47; // r5
  int v48; // r8
  __int64 v49; // r7
  __int64 v50; // r6
  unsigned int v51; // r11
  uint8_t v52[16]; // [sp+50h] [-250h] BYREF
  _WORD v53[288]; // [sp+60h] [-240h] BYREF

  v1 = v0 + 0x2000;
  v2 = v0[11615];
  v3 = v0[11461];
  v4 = v0[11613];
  v5 = *(unsigned __int16 *)(v3 + 28);
  if ( v2 == 0 || v4 == 0 )
    return -939523945;
  v6 = v4 + 336;
  v7 = *(unsigned __int16 *)(v4 + 342);
  if ( v7 != 0 && v7 > *(unsigned __int16 *)(v3 + 30) || (v11 = sub_28738(v6)) == nullptr )
  {
    v13 = *(unsigned __int16 *)(v2 + 342);
    if ( v13 > 0x400 && v13 > v5 )
      return -939523944;
    v11 = sub_28738(v2 + 336);
    if ( v11 == nullptr )
      return -939523944;
  }
  v15 = v11 + 16;
  v16 = v12 + 92928;
  v17 = v12 + 92928;
  for ( i = 16; i != 0; --i )
    *v17++ = 0;
  v19 = *((unsigned __int8 *)v11 + 11);
  v20 = v11[1];
  v21 = *((unsigned __int8 *)v11 + 12);
  v22 = *((unsigned __int8 *)v11 + 13);
  if ( v19 - 3 > 9 )
    return -939523943;
  if ( v21 > v19 )
    return -939523943;
  if ( v22 > v19 )
    return -939523943;
  if ( v20 < v19 )
    return -939523943;
  v12[92929] = v19;
  v12[92932] = v21;
  v12[92933] = v22;
  v23 = *((unsigned __int8 *)v11 + 14);
  if ( (unsigned int)(v23 - 1) > 0xB3 )
    return -939523943;
  v12[92934] = v23;
  v24 = *((unsigned __int8 *)v11 + 15);
  if ( v24 - 1 > 0x17 )
    return -939523943;
  if ( v19 > v20 )
    v19 = v20;
  v25 = v1[3269];
  if ( *(_BYTE *)(v25 + 24) == 0 || *(_QWORD *)(v25 + 40) != 0 )
    v20 = v19;
  if ( v20 < 4 * v19 )
  {
    v31 = 0;
  }
  else
  {
    Timebase = HvpGetTimebase();
    v27 = 180000000000LL * v24;
    __tdllei(v27, 0);
    v28 = Timebase / v27;
    if ( v28 > 3 )
      v28 = 3;
    sub_20060(v52, 4);
    if ( v52[3] >= 0x99u )
    {
      if ( v52[3] >= 0xD9u )
      {
        if ( v52[3] >= 0xF3u )
          v29 = 3;
        else
          v29 = 2;
      }
      else
      {
        v29 = 1;
      }
      if ( v29 > v28 )
        v29 = v28;
    }
    else
    {
      v29 = 0;
    }
    v30 = (v20 + 3) >> 2;
    v31 = v30 * v29;
    v32 = v31 + v30;
    if ( v31 + v30 <= v20 )
      goto LABEL_39;
  }
  v32 = v20;
LABEL_39:
  v33 = v32 - v31;
  if ( v32 - v31 > 0x40 )
  {
    sub_20060(v52, 4);
    v34 = *(_DWORD *)v52;
    v35 = v33 - 63LL;
    v36 = *(unsigned int *)v52 / v35;
    __tdllei(v35, 0);
    LODWORD(v35) = v36 * (v33 - 63);
    v33 = 64;
    v31 += v34 - v35;
  }
  LODWORD(v10) = 8 * v33;
  memcpy((__int64)v53, (__int64)&v15[4 * v31], v10, v9, v8);
  v37 = 0;
  if ( v33 != 1 )
  {
    v38 = (__int64 *)v53;
    do
    {
      sub_20060(v52, 4);
      v39 = *(_DWORD *)v52;
      v40 = *v38;
      v41 = *(_DWORD *)v52 / (v33 - (unsigned int)v37);
      __twllei(v33 - v37, 0);
      v42 = v39 - v41 * (v33 - v37) + v37;
      ++v37;
      v43 = 4LL * v42;
      v44 = *(_QWORD *)&v53[v43];
      *(_QWORD *)&v53[v43] = v40;
      *v38++ = v44;
    }
    while ( (unsigned int)v37 < v33 - 1 );
  }
  sub_20060(v52, 4);
  v45 = v33 - v19 + 1;
  v46 = *(_DWORD *)v52;
  LODWORD(v47) = 8 * v19;
  v48 = *(_DWORD *)v52 / v45;
  __twllei(v45, 0);
  memcpy((__int64)(v16 + 32), (__int64)&v53[4 * (v46 - v48 * v45)], v47, v50, v49);
  sub_20060(v52, 4);
  v51 = *(_DWORD *)v52;
  __twllei(v19, 0);
  v16[7] = v51 % v19;
  return 0;
}


/* ========== [0x28f98] sub_28F98 (516 bytes) ========== */
__int64 __fastcall sub_28F98(__int64 a1, int a2)
{
  __int64 v2; // r2
  __int64 v5; // r5
  unsigned int v6; // r4
  _DWORD *v7; // r9
  __int64 v8; // r11
  __int64 v9; // r10
  __int64 i; // ctr
  __int16 *v11; // r6
  uint8_t *v12; // r30
  _BYTE *v13; // r9
  unsigned __int64 v14; // r11
  __int64 j; // ctr
  unsigned int v16; // r31
  int v17; // r11
  int v18; // r10
  unsigned int v19; // r11
  uint8_t *v20; // r10
  uint8_t *v21; // r11
  __int64 k; // ctr
  __int64 v23; // r7
  __int64 v24; // r6
  uint8_t v25[16]; // [sp+50h] [-1E0h] BYREF
  X_CRYPT_AES_STATE v26; // [sp+60h] [-1D0h] BYREF
  _BYTE v27[2]; // [sp+1C0h] [-70h] BYREF
  __int16 v28; // [sp+1C2h] [-6Eh] BYREF
  int v29; // [sp+1C4h] [-6Ch]
  int v30; // [sp+1C8h] [-68h]
  __int16 v31; // [sp+1CEh] [-62h]
  __int16 v32; // [sp+1D2h] [-5Eh] BYREF
  uint8_t v33[16]; // [sp+1F0h] [-40h] BYREF
  int v34; // [sp+200h] [-30h]

  v34 = *(_DWORD *)(v2 + 0x10000);
  if ( a2 == 34 )
  {
    v5 = v2 + 92928;
    v6 = *(unsigned __int8 *)(v2 + 92928);
    if ( v6 < *(unsigned __int8 *)(v2 + 92929) )
    {
      v7 = v27;
      v8 = 8LL * *(unsigned __int8 *)(v2 + 92928);
      v9 = v2 + 92960;
      for ( i = 8; i != 0; --i )
        *v7++ = 0;
      *(_WORD *)v7 = 0;
      v11 = &v32;
      v12 = (uint8_t *)(v2 + 65584);
      v27[1] = 32;
      v27[0] = v27[0] & 0xC0 | 0x39;
      HIBYTE(v28) = 17;
      v13 = (_BYTE *)(v2 + 65584);
      v29 = *(_QWORD *)(v8 + v9) >> 40;
      v14 = *(_QWORD *)(v8 + v9);
      v31 = 360;
      v30 = (v14 >> 16) & 0xFFFFFF;
      for ( j = 16; j != 0; --j )
      {
        *(_BYTE *)v11 = *v13++;
        v11 = (__int16 *)((char *)v11 + 1);
      }
      if ( v6 == *(unsigned __int8 *)(v2 + 92935) )
      {
        v16 = (v30 - v29) / 3u;
        sub_20060(v25, 4);
        v17 = *(_DWORD *)v25;
        v18 = *(_DWORD *)v25 / v16;
        __twllei(v16, 0);
        v29 += (v17 - v18 * v16) & 0xFFFFFFF0;
        sub_20060(v25, 4);
        v19 = *(_DWORD *)v25;
        __twllei(v16, 0);
        *(_DWORD *)v25 = (v19 % v16) & 0xFFFFFFF0;
        v30 -= *(_DWORD *)v25;
      }
      v20 = v33;
      v21 = v12;
      for ( k = 16; k != 0; --k )
        *v20++ = *v21++;
      XeCryptAesKey(&v26, (const uint8_t *)(v2 + 65568), v5);
      XeCryptAesCbc(&v26, (const uint8_t *)&v28, 0x10u, (uint8_t *)&v28, v33, 1);
      memcpy(a1, (__int64)v27, 0x22u, v24, v23);
      v_MACHINE_CHECK_filter(v34);
      return 0;
    }
    else
    {
      v_MACHINE_CHECK_filter(v34);
      return -939523942;
    }
  }
  else
  {
    v_MACHINE_CHECK_filter(v34);
    return -939523952;
  }
}


/* ========== [0x291a0] sub_291A0 (372 bytes) ========== */
__int64 __fastcall sub_291A0(__int64 a1, int a2, int a3, __int64 a4)
{
  __int64 v4; // r2
  unsigned __int8 *v5; // r31
  unsigned int v6; // r30
  int v7; // r29
  int v8; // r28
  __int64 v9; // r5
  __int64 v10; // r2
  uint8_t *v11; // r10
  uint8_t *v12; // r11
  __int64 i; // ctr
  X_CRYPT_AES_STATE v15; // [sp+50h] [-1D0h] BYREF
  _BYTE v16[2]; // [sp+1B0h] [-70h] BYREF
  uint8_t v17[12]; // [sp+1B2h] [-6Eh] BYREF
  unsigned __int16 v18; // [sp+1BEh] [-62h]
  uint8_t v19[16]; // [sp+1E0h] [-40h] BYREF
  int v20; // [sp+1F0h] [-30h]

  v20 = *(_DWORD *)(v4 + 0x10000);
  v5 = (unsigned __int8 *)(v4 + 92928);
  if ( a2 != 34 )
    goto LABEL_15;
  v6 = *v5;
  if ( v6 >= *(unsigned __int8 *)(v4 + 92929) )
    goto LABEL_15;
  v7 = *(unsigned __int8 *)(v4 + 92934);
  v8 = (unsigned __int16)*(_QWORD *)(8LL * *v5 + v4 + 92960);
  if ( a3 < 0 )
    goto LABEL_15;
  memcpy((__int64)v16, a1, 0x22u, a4, v4 + 92960);
  if ( (v16[0] & 0x3F) != 0x39 )
    goto LABEL_15;
  if ( v16[1] != 32 )
    goto LABEL_15;
  v11 = v19;
  v12 = (uint8_t *)(v10 + 65584);
  for ( i = 16; i != 0; --i )
    *v11++ = *v12++;
  XeCryptAesKey(&v15, (const uint8_t *)(v10 + 65568), v9);
  XeCryptAesCbc(&v15, v17, 0x10u, v17, v19, 0);
  if ( v17[0] != 17 || v18 >= 0x168u )
    goto LABEL_15;
  if ( v6 != v5[7] )
  {
    if ( ((unsigned int)v18 - v8 + v7 + 360) % 0x168 > 2 * v7 )
      ++v5[3];
    else
      ++v5[2];
LABEL_15:
    ++*v5;
    goto LABEL_16;
  }
  v5[7] = 0xFFu;
LABEL_16:
  v_MACHINE_CHECK_filter(v20);
  return 0;
}


/* ========== [0x29318] sub_29318 (116 bytes) ========== */
__int64 sub_29318()
{
  unsigned __int8 *v0; // r2
  _QWORD *v1; // r11
  __int64 result; // r3
  __int64 i; // ctr

  v1 = v0 + 92928;
  if ( v0[92928] != v0[92929] )
    goto LABEL_6;
  if ( v0[92931] < (unsigned int)v0[92933] )
  {
    if ( v0[92930] >= (unsigned int)v0[92932] )
    {
      result = -939523937;
      goto LABEL_7;
    }
LABEL_6:
    result = -939523939;
    goto LABEL_7;
  }
  result = -939523938;
LABEL_7:
  for ( i = 16; i != 0; --i )
    *v1++ = 0;
  return result;
}


/* ========== [0x29390] sub_29390 (76 bytes) ========== */
__int64 __fastcall sub_29390(int a1, __int64 a2)
{
  __int64 v2; // r2
  _DWORD *v4; // r31
  __int64 v6; // r4
  unsigned int v7; // r30

  v4 = (_DWORD *)(v2 + 92896);
  HvpAquireSpinLock((_DWORD *)(v2 + 92896));
  LODWORD(v6) = a1;
  v7 = ((__int64 (__fastcall *)(__int64, __int64))loc_790)(a2, v6);
  sub_27F08();
  HvpReleaseSpinLock(v4);
  return v7;
}


/* ========== [0x293e0] sub_293E0 (324 bytes) ========== */
__int64 __fastcall sub_293E0(
        __int64 a1,
        unsigned __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  __int64 v8; // r2
  int v9; // r25
  unsigned int v10; // r30
  int v12; // r28
  int v13; // r27
  int v14; // r26
  _DWORD *v15; // r24
  unsigned __int64 v17; // r3
  __int64 v18; // r2
  __int64 *v19; // r29
  _DWORD *v20; // r30
  __int64 v21; // r6
  __int64 v22; // r5
  __int64 v23; // r4
  __int64 v24; // r3
  __int64 v25; // r28
  __int64 v26; // r4

  v9 = a5;
  v10 = a1;
  v12 = a3;
  v13 = a4;
  v14 = a6;
  v15 = (_DWORD *)(v8 + 92824);
  if ( (_DWORD)a5 != 0 )
  {
    if ( (a1 & 0x7F) != 0
      || (a2 & 0x7F) != 0
      || (_DWORD)a2 == 0
      || (unsigned int)a2 > 0x10000
      || ((((_DWORD)a1 + (_DWORD)a2 - 1) ^ (unsigned int)a1) & 0xFFFF0000) != 0 )
    {
      return -939523952;
    }
    HvpAquireSpinLock((_DWORD *)(v8 + 92824));
    v17 = HvpRelocatePhysicalToProtected(v10, a2, 58);
  }
  else
  {
    v17 = HvpPhysicalToReal(a1, a2, a3, a4, a5, a6, a7, a8);
  }
  v19 = (__int64 *)v17;
  v20 = (_DWORD *)(v18 + 92896);
  HvpAquireSpinLock((_DWORD *)(v18 + 92896));
  LODWORD(v21) = v13;
  LODWORD(v22) = v12;
  LODWORD(v23) = a2;
  LODWORD(v24) = (_DWORD)v19;
  v25 = (unsigned int)((__int64 (__fastcall *)(__int64, __int64, __int64, __int64))loc_7A0)(v24, v23, v22, v21);
  sub_27F08();
  HvpReleaseSpinLock(v20);
  if ( v14 != 0 || (_DWORD)v25 != 0 )
  {
    if ( v9 != 0 )
    {
      LODWORD(v26) = (unsigned int)a2 >> 7;
      HvpZeroCacheLines((__int64)v19, v26);
    }
    else
    {
      memset((__int64)v19, 0, a2);
    }
  }
  if ( v9 != 0 )
  {
    HvpRelocatePhysicalToEncryptedStrict(v19, a2, 58);
    HvpReleaseSpinLock(v15);
  }
  return v25;
}


/* ========== [0x29528] HvxDvdAuthEx (284 bytes) ========== */
__int64 __fastcall HvxDvdAuthEx(int a1, __int64 a2, __int64 a3, __int64 a4, __int64 a5, __int64 a6)
{
  __int64 v6; // r2
  int v7; // r10
  unsigned int v8; // r11

  v7 = a3;
  v8 = a2;
  LODWORD(a3) = a4;
  switch ( a1 )
  {
    case 0:
      LODWORD(a6) = 1;
      LODWORD(a5) = 1;
      LODWORD(a4) = v6 + 165896;
      LODWORD(a2) = v7;
      return sub_293E0(v8, a2, a3, a4, a5, a6);
    case 1:
      LODWORD(a6) = 0;
      LODWORD(a5) = 1;
      LODWORD(a4) = v6 + 166304;
      LODWORD(a2) = v7;
      return sub_293E0(v8, a2, a3, a4, a5, a6);
    case 2:
      LODWORD(a6) = 1;
      LODWORD(a5) = 1;
      LODWORD(a4) = v6 + 166520;
      LODWORD(a3) = 0;
      LODWORD(a2) = v7;
      return sub_293E0(v8, a2, a3, a4, a5, a6);
    case 3:
      return sub_29390(v6 + 166856, a2, a3);
    case 4:
      LODWORD(a2) = 0;
      return sub_29390(v6 + 166976, a2, a3);
    case 5:
      LODWORD(a6) = 0;
      LODWORD(a5) = 0;
      LODWORD(a4) = v6 + 167832;
      LODWORD(a3) = 0;
      LODWORD(a2) = v7;
      return sub_293E0(v8, a2, a3, a4, a5, a6);
    case 6:
      LODWORD(a6) = 0;
      LODWORD(a5) = 0;
      LODWORD(a4) = v6 + 168352;
      LODWORD(a2) = v7;
      return sub_293E0(v8, a2, a3, a4, a5, a6);
    case 7:
      LODWORD(a2) = 0;
      return sub_29390(v6 + 168728, a2, a3);
    default:
      return -939523952;
  }
}


/* ========== [0x29648] HvxHdcpCalculateMi (24 bytes) ========== */
__int64 __fastcall HvxHdcpCalculateMi(__int64 a1, __int64 a2, __int64 a3)
{
  __int64 v4; // r2

  return *(_QWORD *)(v4 + 92864) ^ a1 ^ a2 ^ a3;
}


/* ========== [0x29660] HvxHdcpCalculateAKsvSignature (4 bytes) ========== */
// attributes: thunk
__int64 HvxHdcpCalculateAKsvSignature()
{
  return HvpHdcpCalculateAKsvSignature();
}


/* ========== [0x29668] HvxHdcpCalculateBKsvSignature (4 bytes) ========== */
// attributes: thunk
__int64 HvxHdcpCalculateBKsvSignature()
{
  return HvpHdcpCalculateBKsvSignature();
}


/* ========== [0x29670] sub_29670 (160 bytes) ========== */
__int64 sub_29670()
{
  __int64 v0; // r2
  __int64 v1; // r5
  __int64 v2; // r2
  uint8_t v4[16]; // [sp+60h] [-1A0h] BYREF
  X_CRYPT_AES_STATE v5; // [sp+70h] [-190h] BYREF
  uint8_t v6[16]; // [sp+1D0h] [-30h] BYREF
  int v7; // [sp+1E0h] [-20h]

  v7 = *(_DWORD *)(v0 + 0x10000);
  XeCryptHmacSha(v4, 8u, nullptr, 0, nullptr, 0, nullptr, 0);
  XeCryptAesKey(&v5, v6, v1);
  XeCryptAesEcb(&v5, (const uint8_t *)(v2 + 66968), (uint8_t *)(v2 + 92864), 0);
  return v_MACHINE_CHECK_filter(v7);
}


/* ========== [0x29710] sub_29710 (16 bytes) ========== */
_DWORD *sub_29710()
{
  __int64 v0; // r2

  return sub_20060((uint8_t *)(v0 + 91920), 16);
}


/* ========== [0x29720] sub_29720 (60 bytes) ========== */
__int64 sub_29720()
{
  __int64 v0; // r3
  __int64 v1; // r2
  __int64 result; // r3
  __int64 v3; // r2

  v0 = sub_270();
  *(_QWORD *)(v1 + 92528) = v0 + 59392;
  result = sub_2A8();
  *(_QWORD *)(v3 + 91896) = result;
  return result;
}


/* ========== [0x29760] sub_29760 (144 bytes) ========== */
__int64 __fastcall sub_29760(__int64 result, char a2, unsigned int a3)
{
  unsigned int v3; // r29
  unsigned int v4; // r31
  unsigned __int8 v5; // r30
  unsigned __int64 v6; // r5
  _BYTE *v7; // r31
  bool v8; // zf

  v3 = a3;
  v4 = result;
  if ( a3 != 0 )
  {
    v5 = (16 * (a2 & 0xF)) | a2 & 0xF;
    result = sub_5A8();
    v8 = (v4 & 1) == 0;
    v7 = (_BYTE *)((v4 >> 1) + result);
    if ( !v8 )
    {
      --v3;
      *v7 = v5 & 0xF0 | *v7 & 0xF;
      ++v7;
    }
    if ( v3 > 1 )
    {
      LODWORD(v6) = v3 >> 1;
      result = memset((__int64)v7, v5, v6);
    }
    if ( (v3 & 1) != 0 )
      v7[v3 >> 1] = v5 & 0xF | v7[v3 >> 1] & 0xF0;
  }
  return result;
}


/* ========== [0x297f0] sub_297F0 (72 bytes) ========== */
int __fastcall sub_297F0(unsigned int a1)
{
  char v1; // r31
  unsigned int v2; // r11
  int result; // r3

  v1 = a1;
  v2 = *(unsigned __int8 *)((a1 >> 1) + sub_5A8());
  result = v2 >> 4;
  if ( (v1 & 1) == 0 )
    return v2 & 0xF;
  return result;
}


/* ========== [0x29838] sub_29838 (172 bytes) ========== */
__int64 __fastcall sub_29838(unsigned __int64 a1, unsigned int *a2)
{
  _DWORD *v4; // r3
  _DWORD *v5; // r29
  int v6; // r3
  unsigned int v8; // r10
  unsigned int v9; // r11

  *a2 = 0;
  v4 = (_DWORD *)sub_57C(a1);
  v5 = v4;
  if ( v4 == nullptr || *v4 == 0 )
    return 0;
  *a2 = *v4;
  if ( (a1 & 0xF0000000) == 0x90000000 )
  {
    v6 = sub_297F0((unsigned __int16)(a1 >> 12));
    if ( v6 == 0 )
    {
      *a2 = 0;
      return 0;
    }
    v8 = v6 & 0xF | *a2 & 0xFFFFFFF0;
    *a2 = v8;
    v9 = v8 & 0xFFFFFFF7 | *v5 & 8;
    *a2 = v9;
    if ( (v8 & 0x40000000) != 0 && (v6 & 8) == 0 )
      *a2 = v9 & 0xFFFFF;
  }
  return 1;
}


/* ========== [0x298e8] sub_298E8 (128 bytes) ========== */
_DWORD *__fastcall sub_298E8(int a1)
{
  __int64 v1; // r2
  unsigned int v2; // r8
  __int64 v3; // r9
  _DWORD *v4; // r11
  _DWORD *v5; // r10

  v2 = 0;
  v3 = *(_QWORD *)(v1 + 91896);
  v4 = nullptr;
  v5 = (_DWORD *)(v3 + 24);
  do
  {
    if ( v2 >= *(_DWORD *)(v3 + 20) )
      break;
    if ( *v5 == a1 )
      v4 = v5;
    ++v2;
    v5 += 2;
  }
  while ( v4 == nullptr );
  if ( v4 == nullptr )
    return nullptr;
  if ( *((_BYTE *)v4 + 3) == 0 )
    return (_DWORD *)(unsigned int)v4[1];
  if ( *((_BYTE *)v4 + 3) == 1 )
    return v4 + 1;
  return (_DWORD *)((unsigned int)v4[1] + v3);
}


/* ========== [0x29968] sub_29968 (252 bytes) ========== */
__int64 __fastcall sub_29968(unsigned __int64 a1, unsigned __int64 a2, __int64 a3, unsigned __int64 a4, int a5)
{
  unsigned __int64 v6; // r30
  unsigned __int64 v7; // r29
  unsigned int v9; // r11
  __int64 v11; // r7
  __int64 v12; // r6
  unsigned __int64 v13; // r31
  unsigned __int64 v14; // r3
  unsigned __int64 v15; // r4
  unsigned int v16; // [sp+50h] [-40h] BYREF

  v6 = a2;
  v7 = a4;
  if ( a3 != 0 )
  {
    v9 = *(_DWORD *)(a3 + 16);
    if ( (unsigned int)a2 < v9 || (int)a2 + (int)a4 > *(_DWORD *)(a3 + 20) + v9 )
      return 0;
  }
  while ( (_DWORD)v7 != 0 )
  {
    if ( (unsigned int)sub_29838(v6, &v16) == 0 || a5 != 0 && ((v16 & 1) == 0 || (v16 & 0x40000000) == 0) )
      return 0;
    v13 = (((_DWORD)v6 + 4096) & 0xFFFFF000) - v6;
    v14 = ((unsigned __int64)v16 << 10) & 0x3FFFFFF0000LL | 0x8000000000000000LL | (unsigned __int16)v6;
    if ( (unsigned int)v13 > (unsigned int)v7 )
      v13 = v7;
    if ( a5 != 0 )
    {
      v15 = ((unsigned __int64)v16 << 10) & 0x3FFFFFF0000LL | 0x8000000000000000LL | (unsigned __int16)v6;
      v14 = a1;
    }
    else
    {
      v15 = a1;
    }
    memcpy(v14, v15, v13, v12, v11);
    v7 -= v13;
    v6 += v13;
    a1 += (unsigned int)v13;
  }
  return 1;
}


/* ========== [0x29a68] sub_29A68 (236 bytes) ========== */
__int64 __fastcall sub_29A68(
        __int64 a1,
        X_CRYPT_AES_STATE *a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        uint8_t *a7)
{
  __int64 v7; // r2
  int v10; // r11
  uint32_t v11; // r10
  BOOL v12; // r29
  __int64 v13; // r30
  __int64 v14; // r7
  __int64 v15; // r6
  __int64 v16; // r2
  __int64 v17; // r31
  __int64 v18; // r5
  __int64 v19; // r31
  const uint8_t *v20; // r4
  _WORD v22[128]; // [sp+50h] [-140h] BYREF
  uint8_t v23[20]; // [sp+150h] [-40h] BYREF
  int v24; // [sp+164h] [-2Ch]

  v10 = *(_DWORD *)(a1 + 260);
  v11 = _cntlzw(v10 & 6);
  v24 = *(_DWORD *)(v7 + 0x10000);
  v12 = (v11 & 0x20) == 0;
  if ( v10 >= 0 )
    v13 = v7 + 66888;
  else
    v13 = v7 + 66900;
  XeCryptRotSumSha((uint8_t *)(a1 + 256), *(unsigned int *)(a1 + 256) - 256LL, nullptr, 0, v23, 0x14u, a7, v11);
  memcpy((__int64)v22, a1, 0x100u, v15, v14);
  v17 = v16 + 67768;
  if ( XeCryptBnQwBeSigVerify((__int64)v22, (__int64)v23, v13, (_DWORD *)(v16 + 67768)) )
  {
    if ( a2 != nullptr )
    {
      v20 = (const uint8_t *)(v17 + 1088);
      if ( !v12 )
        v20 = (_BYTE *)&byte_54;
      XeCryptAesKey(a2, v20, v18);
    }
    v19 = 0;
  }
  else
  {
    v19 = -1073741279;
  }
  v_MACHINE_CHECK_filter(v24);
  return v19;
}


/* ========== [0x29b58] HvpImageDataLoadAesEcb (144 bytes) ========== */
__int64 __fastcall HvpImageDataLoadAesEcb(unsigned __int64 a1, __int64 a2)
{
  __int64 v4; // r5
  __int64 result; // r3
  __int64 v6; // r2
  uint8_t v7[48]; // [sp+50h] [-1B0h] BYREF
  X_CRYPT_AES_STATE v8; // [sp+80h] [-180h] BYREF

  result = sub_29968((unsigned __int64)v7, a1, a2, 0x2Cu, 1);
  if ( (_DWORD)result != 0 )
  {
    XeCryptAesKey(&v8, (const uint8_t *)(v6 + 91920), v4);
    XeCryptAesEcb(&v8, v7, v7, 1);
    return sub_29968((unsigned __int64)v7, a1, a2, 0x2Cu, 0);
  }
  return result;
}


/* ========== [0x29be8] HvxGetImagePageTableEntry (116 bytes) ========== */
__int64 __fastcall HvxGetImagePageTableEntry(void *virt)
{
  __int64 v1; // r2
  int v2; // r30
  _DWORD *v3; // r31
  __int64 v4; // r3
  int v5; // r30
  int v7; // [sp+50h] [-20h] BYREF

  v2 = (int)virt;
  v3 = (_DWORD *)(v1 + 92448);
  HvpAquireSpinLock((_DWORD *)(v1 + 92448));
  LODWORD(v4) = v2;
  v5 = 0;
  if ( (unsigned int)sub_29838(v4, &v7) != 0 )
    v5 = v7;
  HvpReleaseSpinLock(v3);
  return v5 & 0x400FFFFF;
}


/* ========== [0x29c60] HvxSetImagePageTableEntry (4 bytes) ========== */
// attributes: thunk
__int64 __fastcall HvxSetImagePageTableEntry(void *virt, uint32_t entry)
{
  return HvNoOp_ReturnZero();
}


/* ========== [0x29c68] sub_29C68 (88 bytes) ========== */
__int64 __fastcall sub_29C68(__int64 a1, unsigned int a2)
{
  __int64 v2; // r9
  __int16 v3; // r11
  uint8_t v5[8]; // [sp+50h] [-10h] BYREF

  v2 = 2LL * a2;
  v3 = *(_WORD *)(v2 + a1);
  if ( (v3 & 0x400) != 0 )
  {
    sub_20060(v5, 2);
    v3 = *(_WORD *)v5;
  }
  else
  {
    ++*(_WORD *)(v2 + a1);
  }
  return v3 & 0x3FF;
}


/* ========== [0x29cc0] HvxMapImagePage (292 bytes) ========== */
__int64 __fastcall HvxMapImagePage(uint32_t idx, void *address, uint32_t phys_address)
{
  __int64 v3; // r2
  _DWORD *v8; // r28
  __int64 v9; // r2
  __int64 v10; // r29
  __int64 v11; // r30
  _DWORD *v12; // r3
  _DWORD *v13; // r31
  __int64 v14; // r3
  unsigned __int64 v15; // r11

  if ( idx >= 2 )
    return 0;
  v8 = (_DWORD *)(v3 + 92448);
  HvpAquireSpinLock((_DWORD *)(v3 + 92448));
  v10 = 0;
  v11 = 56LL * idx + v9 + 91776;
  if ( *(_BYTE *)(v11 + 52) == 1 && (_WORD)address == 0 && (_WORD)phys_address == 0 && phys_address < 0x20000000 )
  {
    v12 = (_DWORD *)sub_57C(address);
    v13 = v12;
    if ( v12 != nullptr
      && (*v12 & 8) == 0
      && (unsigned __int64)v12 >= *(_QWORD *)v11
      && (unsigned __int64)v12 <= *(_QWORD *)(v11 + 8)
      && (*v12 & 0xFFFC0) == 0 )
    {
      v14 = sub_5C0();
      if ( (*(_WORD *)(((phys_address >> 15) & 0xFFFFFFFFFFFFFFFELL) + v14) & 0x800) == 0 )
      {
        v10 = 1;
        v15 = (phys_address >> 10) & 0xFFFC0 | (unsigned __int64)(*v13 & 0xFFF0003F);
        *v13 = v15;
        *(_WORD *)(((v15 >> 5) & 0x7FFE) + v14) |= 0x800u;
      }
    }
  }
  HvpReleaseSpinLock(v8);
  return v10;
}


/* ========== [0x29de8] HvxCompleteImageMapping (200 bytes) ========== */
__int64 __fastcall HvxCompleteImageMapping(uint32_t idx)
{
  __int64 v1; // r2
  _DWORD *v4; // r30
  __int64 v5; // r10
  __int64 v6; // r2
  __int64 v7; // r31
  __int64 v8; // r11
  unsigned int *i; // r10
  char v10; // r10

  if ( idx >= 2 )
    return 0;
  v4 = (_DWORD *)(v1 + 92448);
  HvpAquireSpinLock((_DWORD *)(v1 + 92448));
  v5 = 56LL * idx;
  v7 = 0;
  v8 = v5 + v6 + 91776;
  if ( *(_BYTE *)(v8 + 52) == 1 )
  {
    for ( i = *(unsigned int **)v8; ; ++i )
    {
      if ( (unsigned __int64)i > *(_QWORD *)(v8 + 8) )
      {
        v10 = 2;
        v7 = 1;
        goto LABEL_9;
      }
      if ( (*i & 0xFFFC0) == 0 )
        break;
      *i = *i & 0xFFFFFFF0 | 0xE;
    }
    v10 = 15;
LABEL_9:
    *(_BYTE *)(v8 + 52) = v10;
  }
  HvpReleaseSpinLock(v4);
  return v7;
}


/* ========== [0x29eb0] HvxUnmapImagePages (164 bytes) ========== */
__int64 __fastcall HvxUnmapImagePages(void *address)
{
  __int64 v1; // r2
  _DWORD *v3; // r29
  __int64 v4; // r30
  unsigned int *v5; // r3
  bool v6; // cr58

  v3 = (_DWORD *)(v1 + 92448);
  v4 = 0;
  HvpAquireSpinLock((_DWORD *)(v1 + 92448));
  if ( (_WORD)address == 0 && (_DWORD)address != 0x80000000 )
  {
    v5 = (unsigned int *)sub_57C(address);
    if ( v5 != nullptr && (*v5 & 0x10) != 0 )
    {
      do
      {
        v6 = (*v5 & 0x20LL) == 0;
        *v5 = *v5 & 0xFFFFFFF0 | 7;
        ++v5;
      }
      while ( v6 );
      v4 = 1;
    }
  }
  HvpReleaseSpinLock(v3);
  return v4;
}


/* ========== [0x29f58] HvxUnmapImage (344 bytes) ========== */
__int64 __fastcall HvxUnmapImage(unsigned __int64 address)
{
  __int64 v2; // r29
  __int64 v3; // r26
  __int64 v4; // r2
  _DWORD *v5; // r25
  __int64 v6; // r5
  __int64 v7; // r4
  unsigned int *v8; // r3
  __int64 v9; // r2
  unsigned int *v10; // r31
  __int64 v11; // r28
  BOOL v12; // r27
  __int64 v13; // r11
  unsigned __int64 v14; // r11
  int v15; // r11
  __int64 v16; // r10
  _DWORD *v17; // r11

  v2 = 0;
  v3 = sub_5C0();
  v5 = (_DWORD *)(v4 + 92448);
  HvpAquireSpinLock((_DWORD *)(v4 + 92448));
  HvpSetRMCI(0);
  if ( (_WORD)address == 0 && (_DWORD)address != 0x80000000 )
  {
    v8 = (unsigned int *)sub_57C(address);
    v10 = v8;
    if ( v8 != nullptr )
    {
      v11 = (unsigned __int16)(address >> 12);
      v12 = (_cntlzw(-1879048192 - ((unsigned int)address & 0xF0000000)) & 0x20) != 0;
      if ( *v8 != 0 && (*v8 & 0x10) != 0 )
      {
        do
        {
          v13 = *v10;
          if ( (v13 & 0xC0000000) != 0 )
            sub_D7C((v13 << 10) & 0x3FFFFFF0000LL);
          if ( v12 )
          {
            LODWORD(v6) = 16;
            LODWORD(v7) = 0;
            LODWORD(v8) = v11;
            HIDWORD(v8) = (unsigned __int64)sub_29760(v8, v7, v6) >> 32;
            v11 += 16;
          }
          v14 = *v10;
          if ( (v14 & 0xFFFC0) != 0 )
            *(_WORD *)(((v14 >> 5) & 0x7FFE) + v3) &= ~0x800u;
          v15 = *v10;
          *v10++ = 0;
        }
        while ( (v15 & 0x20) == 0 );
        v16 = 12;
        v17 = (_DWORD *)(v9 + 92480);
        do
        {
          if ( *v17 == (_DWORD)address )
            *v17 = 0;
          --v16;
          ++v17;
        }
        while ( (_DWORD)v16 != 0 );
        v2 = 1;
      }
    }
  }
  HvpSetRMCI(1);
  HvpReleaseSpinLock(v5);
  return v2;
}


/* ========== [0x2a0b0] HvxAbandonImageLoad (112 bytes) ========== */
void __fastcall HvxAbandonImageLoad(uint32_t idx)
{
  __int64 v1; // r2
  _DWORD *v3; // r31
  __int64 v4; // r2

  if ( idx < 2 )
  {
    v3 = (_DWORD *)(v1 + 92448);
    HvpAquireSpinLock((_DWORD *)(v1 + 92448));
    *(_BYTE *)(56LL * idx + v4 + 91828) = 0;
    HvpReleaseSpinLock(v3);
  }
}


/* ========== [0x2a120] HvxLoadImageData (752 bytes) ========== */
__int64 __fastcall HvxLoadImageData(uint32_t idx, X_XEX_PAGE_DESCRIPTOR page_descriptor)
{
  __int64 v2; // r2
  _DWORD *v5; // r19
  __int64 v6; // r20
  __int64 v7; // r8
  __int64 v8; // r2
  __int64 v9; // r27
  int v10; // r31
  char v11; // r10
  __int64 v12; // r21
  unsigned int v13; // r11
  unsigned int v14; // r24
  __int64 v15; // r28
  unsigned int *v16; // r30
  unsigned int v17; // r10
  uint8_t *v18; // r29
  __int64 v19; // r10
  __int64 v20; // r5
  unsigned int v21; // r31
  __int64 v22; // r3
  uint8_t *next_page_digest; // r11
  _DWORD *v24; // r10
  __int64 i; // ctr
  uint8_t v26[20]; // [sp+50h] [-90h] BYREF
  int v27; // [sp+64h] [-7Ch]
  X_XEX_PAGE_DESCRIPTOR v28; // [sp+100h] [+20h] BYREF

  v28 = page_descriptor;
  v27 = *(_DWORD *)(v2 + 0x10000);
  if ( idx >= 2 )
  {
    v_MACHINE_CHECK_filter(v27);
    return 0;
  }
  v5 = (_DWORD *)(v2 + 92448);
  HvpAquireSpinLock((_DWORD *)(v2 + 92448));
  v6 = 0;
  HvpSetRMCI(0);
  v9 = 56LL * idx + v8 + 91776;
  if ( *(_BYTE *)(v9 + 52) == 2 && ((v28.size_info & 2) != 0 || (v28.size_info & 1) != 0) && (v28.size_info & 4) == 0 )
  {
    if ( (v28.size_info & 2) == 0 || (v10 = 0, (v28.size_info & 8) != 0) )
      v10 = 1;
    if ( (*(_BYTE *)(v9 + 53) & 1) != 0 )
    {
      LODWORD(v7) = 8 * v10;
      v11 = 12;
      v12 = v7 | v28.size_info & 3 | 4LL;
    }
    else
    {
      v12 = v28.size_info & 3 | 0xCLL;
      v11 = 16;
    }
    v13 = *(_DWORD *)(v9 + 20);
    v14 = *(_DWORD *)(v9 + 24);
    v15 = v28.size_info >> 4 << v11;
    if ( v14 < v13
      && (_DWORD)v15 != 0
      && (unsigned int)v15 <= 0x10000
      && (unsigned int)v15 <= v13 - v14
      && ((((_DWORD)v15 + v14 - 1) ^ v14) & 0xFFFF0000) == 0 )
    {
      v16 = (unsigned int *)((((unsigned __int64)*(unsigned int *)(v9 + 24) >> 14) & 0x3FFFC) + *(_QWORD *)v9);
      if ( (unsigned __int64)v16 <= *(_QWORD *)(v9 + 8) )
      {
        v17 = *v16;
        if ( (*v16 & 8) != 0 && (v17 & 0xFFFC0) != 0 )
        {
          v18 = (uint8_t *)((v17 << 10) & 0x3FFF0000
                          | (unsigned __int64)((unsigned __int16)v14 & 0xF000)
                          | 0x8000000000000000LL);
          if ( v10 != 0 )
            v18 = (uint8_t *)HvpRelocatePhysicalToProtected(
                               (v17 << 10) & 0x3FFF0000 | *(_DWORD *)(v9 + 24) & 0xF000,
                               v15,
                               61);
          XeCryptSha(v18, v15, (const uint8_t *)&v28, 24, nullptr, 0, v26, 0x14u);
          if ( (unsigned int)XeCryptMemDiff(v26, (unsigned __int8 *)(v9 + 32), 20) == 0 )
          {
            if ( v10 != 0 )
            {
              if ( (*v16 & 0xC0000000) == 0 )
              {
                v21 = *v16 | 0xC0000000;
                v22 = sub_5C0();
                *v16 = ((unsigned __int16)sub_29C68(v22, ((unsigned __int64)*v16 >> 6) & 0x3FFF) << 20) & 0x3FF00000
                     | v21 & 0xC00FFFFF;
              }
              LODWORD(v20) = (unsigned int)v15 >> 7;
              HvpRelocateCacheLines(
                (__int64 *)v18,
                (_QWORD *)(((unsigned __int64)*v16 << 10) & 0x3FFFFFF0000LL
                         | 0x8000000000000000LL
                         | (unsigned __int16)v14 & 0xF000),
                v20);
            }
            if ( (*(_BYTE *)(v9 + 53) & 1) != 0 )
            {
              LODWORD(v19) = v14 >> 12;
              LODWORD(v20) = (unsigned int)v15 >> 12;
              sub_29760(*(unsigned __int16 *)(v9 + 54) + v19, v12, v20);
            }
            else
            {
              *v16 = v12 & 0xF | *v16 & 0xFFFFFFF0;
            }
            next_page_digest = v28.next_page_digest;
            *(_DWORD *)(v9 + 24) = v15 + v14;
            v24 = (_DWORD *)(v9 + 32);
            for ( i = 5; i != 0; --i )
            {
              *v24 = *(_DWORD *)next_page_digest;
              next_page_digest += 4;
              ++v24;
            }
            v6 = 1;
            goto LABEL_37;
          }
          if ( v10 != 0 )
            sub_D7C(v18);
        }
      }
    }
  }
  *(_BYTE *)(v9 + 52) = 15;
LABEL_37:
  HvpSetRMCI(1);
  HvpReleaseSpinLock(v5);
  v_MACHINE_CHECK_filter(v27);
  return v6;
}


/* ========== [0x2a410] HvxFinishImageDataLoad (332 bytes) ========== */
__int64 __fastcall HvxFinishImageDataLoad(uint32_t idx)
{
  __int64 v1; // r2
  _DWORD *v4; // r29
  __int64 v5; // r9
  __int64 v6; // r2
  __int64 v7; // r30
  __int64 v8; // r31
  unsigned int v9; // r10
  __int64 v10; // r11
  __int64 v11; // r3
  __int64 v12; // r10
  __int64 v13; // r11
  _DWORD *v14; // r11
  unsigned __int64 v15; // r10
  __int64 v16; // r4
  __int64 v17; // r3

  if ( idx >= 2 )
    return 0;
  v4 = (_DWORD *)(v1 + 92448);
  HvpAquireSpinLock((_DWORD *)(v1 + 92448));
  v7 = 0;
  v8 = 56LL * idx + v6 + 91776;
  if ( *(_BYTE *)(v8 + 52) == 2 )
  {
    v9 = *(_DWORD *)(v8 + 24);
    if ( v9 == *(_DWORD *)(v8 + 20) )
    {
      if ( (*(_BYTE *)(v8 + 53) & 1) != 0 )
      {
        LODWORD(v5) = v9 >> 12;
        v10 = *(unsigned __int16 *)(v8 + 54);
        v11 = v5 + v10;
        v12 = (unsigned int)((__int64)(*(_QWORD *)(v8 + 8) - *(_QWORD *)v8) >> 2) + 1LL;
        LODWORD(v12) = 16 * v12;
        v13 = v12 + v10;
        if ( (unsigned int)v11 < (unsigned int)v13 )
          sub_29760(v11, 0, v13 - v11);
      }
      v14 = *(_DWORD **)(v8 + 8);
      v15 = ((((unsigned __int64)*(unsigned int *)(v8 + 24) - 1) >> 14) & 0x3FFFC) + *(_QWORD *)v8;
      if ( (unsigned __int64)v14 > v15 )
      {
        do
          *v14-- = 0;
        while ( (unsigned __int64)v14 > v15 );
        *v14 |= 0x20u;
      }
      HvpSetRMCI(0);
      LODWORD(v16) = v8;
      LODWORD(v17) = *(_DWORD *)(v8 + 28);
      HvpImageDataLoadAesEcb(v17, v16);
      HvpSetRMCI(1);
      if ( (*(_BYTE *)(v8 + 53) & 2) != 0 )
      {
        *(_DWORD *)(v8 + 24) = 0;
        *(_BYTE *)(v8 + 52) = 3;
      }
      else
      {
        *(_BYTE *)(v8 + 52) = 0;
      }
      v7 = 1;
    }
  }
  HvpReleaseSpinLock(v4);
  return v7;
}


/* ========== [0x2a560] HvxStartResolveImports (476 bytes) ========== */
__int64 __fastcall HvxStartResolveImports(uint32_t idx, __int64 hv_sec_info_offset_or_ptr)
{
  __int64 v2; // r2
  _DWORD *v6; // r28
  __int64 v7; // r9
  __int64 v8; // r8
  __int64 v9; // r7
  __int64 v10; // r6
  __int64 v11; // r5
  __int64 v12; // r2
  __int64 v13; // r10
  __int64 v14; // r29
  __int64 v15; // r31
  const uint8_t *v16; // r30
  __int64 v17; // r3
  int *v18; // r3
  int i; // r11
  int *v20; // r11
  unsigned int v21; // r11
  __int64 v22; // r10
  _DWORD *v23; // r10
  __int16 *v24; // r11
  __int64 j; // ctr
  X_CRYPT_AES_STATE v26; // [sp+50h] [-1B0h] BYREF
  int v27; // [sp+1B0h] [-50h] BYREF
  __int16 v28; // [sp+1B4h] [-4Ch] BYREF
  uint8_t v29[8]; // [sp+1C0h] [-40h] BYREF
  int v30; // [sp+1C8h] [-38h]
  unsigned int v31; // [sp+1CCh] [-34h]
  int v32; // [sp+1D0h] [-30h]

  v32 = *(_DWORD *)(v2 + 0x10000);
  if ( idx < 2 )
  {
    v6 = (_DWORD *)(v2 + 92448);
    HvpAquireSpinLock((_DWORD *)(v2 + 92448));
    v13 = 56LL * idx;
    v14 = 0;
    v15 = v13 + v12 + 91776;
    if ( *(_BYTE *)(v15 + 52) == 0 && ((hv_sec_info_offset_or_ptr + 288) & 0xFFFuLL) + 32 <= 0x1000 )
    {
      v16 = (const uint8_t *)((_DWORD)hv_sec_info_offset_or_ptr != 0
                            ? HvpPhysicalToReal(hv_sec_info_offset_or_ptr + 288, 32, v11, v10, v9, v8, v7, v13)
                            : *(_QWORD *)(v12 + 91760) + 288LL);
      XeCryptAesKey(&v26, (const uint8_t *)(v12 + 91920), v11);
      XeCryptAesEcb(&v26, v16, (uint8_t *)&v27, 0);
      XeCryptAesEcb(&v26, v16 + 16, v29, 0);
      if ( v27 == v30 )
      {
        v17 = v31;
        *(_DWORD *)(v15 + 16) = v31;
        v18 = (int *)sub_57C(v17);
        *(_QWORD *)v15 = v18;
        if ( v18 != nullptr && (*v18 & 8) != 0 && (*v18 & 0x10) != 0 )
        {
          *(_QWORD *)(v15 + 8) = v18;
          for ( i = *v18; (i & 0x20) == 0; i = *v20 )
          {
            v20 = (int *)(*(_QWORD *)(v15 + 8) + 4LL);
            *(_QWORD *)(v15 + 8) = v20;
          }
          v21 = *(_DWORD *)(v15 + 16);
          v22 = *(_QWORD *)(v15 + 8) - (_QWORD)v18;
          *(_BYTE *)(v15 + 53) = 0;
          v14 = 1;
          *(_DWORD *)(v15 + 20) = ((unsigned int)(v22 >> 2) + 1) << 16;
          if ( (v21 & 0xF0000000) == 0x90000000 )
          {
            *(_BYTE *)(v15 + 53) = 1;
            *(_WORD *)(v15 + 54) = v21 >> 12;
          }
          v23 = (_DWORD *)(v15 + 32);
          v24 = &v28;
          for ( j = 5; j != 0; --j )
          {
            *v23 = *(_DWORD *)v24;
            v24 += 2;
            ++v23;
          }
          *(_DWORD *)(v15 + 24) = v27;
          *(_BYTE *)(v15 + 52) = 3;
        }
      }
    }
    HvpReleaseSpinLock(v6);
    v_MACHINE_CHECK_filter(v32);
    return v14;
  }
  else
  {
    v_MACHINE_CHECK_filter(v32);
    return 0;
  }
}


/* ========== [0x2a740] HvpResolveImportPageLookup (144 bytes) ========== */
unsigned __int64 __fastcall HvpResolveImportPageLookup(int a1, unsigned int a2, unsigned __int64 a3)
{
  __int64 v3; // r2
  unsigned __int64 result; // r3
  __int64 v6; // r10
  unsigned __int64 *v7; // r11
  unsigned __int64 v8; // r9

  result = a3;
  if ( (*(_WORD *)(v3 + 91936) & 1) != 0 && a1 != 0 )
  {
    v6 = 0;
    v7 = (unsigned __int64 *)(v3 + 91944);
    if ( *(_WORD *)(v3 + 91938) != 0 )
    {
      while ( 1 )
      {
        v8 = *v7;
        if ( (*v7 & 1) != 0
          && (unsigned int)(v8 >> 8) == (unsigned __int64)a2
          && ((v8 >> 40) & 0xFFF) == (unsigned int)a3 )
        {
          break;
        }
        ++v6;
        ++v7;
        if ( (unsigned int)v6 >= *(unsigned __int16 *)(v3 + 91938) )
          return result;
      }
      return *v7 >> 52;
    }
  }
  return result;
}


/* ========== [0x2a7d0] HvxResolveImports (1328 bytes) ========== */
__int64 __fastcall HvxResolveImports(__int64 idx, __int64 import_table_offset_or_ptr, char *export_table)
{
  __int64 v3; // r2
  _DWORD *v8; // r19
  __int64 v9; // r8
  __int64 v10; // r7
  __int64 v11; // r6
  __int64 v12; // r5
  __int64 v13; // r2
  __int64 v14; // r21
  __int64 v15; // r31
  int v16; // r11
  __int64 v17; // r10
  __int64 v18; // r31
  __int64 v19; // r29
  __int64 v20; // r4
  __int64 v21; // r3
  __int64 v22; // r8
  __int64 v23; // r7
  __int64 v24; // r6
  __int64 v25; // r5
  __int64 v26; // r2
  unsigned __int64 v27; // r10
  __int64 v28; // r4
  __int64 v29; // r22
  _DWORD *v30; // r31
  __int64 v31; // r7
  __int64 v32; // r6
  __int64 v33; // r4
  uint8_t *v34; // r11
  __int64 i; // ctr
  __int64 v36; // r5
  __int64 v37; // r2
  unsigned int v38; // r10
  char *v39; // r23
  unsigned int v40; // r24
  __int64 v41; // r28
  unsigned int v42; // r11
  __int64 v43; // r5
  __int64 v44; // r4
  _DWORD *v45; // r30
  __int64 v46; // r3
  int v47; // r26
  __int64 v48; // r11
  unsigned __int16 v49; // r31
  unsigned int v50; // r10
  int v51; // r29
  _DWORD *v52; // r11
  _DWORD v53[4]; // [sp+50h] [-260h] BYREF
  _DWORD v54[3]; // [sp+60h] [-250h] BYREF
  int v55; // [sp+6Ch] [-244h]
  int v56; // [sp+70h] [-240h]
  unsigned int v57; // [sp+74h] [-23Ch]
  unsigned int v58; // [sp+78h] [-238h]
  int v59; // [sp+7Ch] [-234h]
  int v60; // [sp+80h] [-230h]
  unsigned int v61; // [sp+84h] [-22Ch]
  unsigned int v62; // [sp+88h] [-228h]
  X_CRYPT_AES_STATE v63; // [sp+90h] [-220h] BYREF
  uint8_t v64[20]; // [sp+1F0h] [-C0h] BYREF
  int v65; // [sp+204h] [-ACh]
  unsigned int v66; // [sp+208h] [-A8h]
  unsigned int v67; // [sp+20Ch] [-A4h]
  unsigned __int8 v68; // [sp+211h] [-9Fh]
  unsigned __int16 v69; // [sp+212h] [-9Eh]
  uint8_t v70[20]; // [sp+220h] [-90h] BYREF
  int v71; // [sp+234h] [-7Ch]

  v71 = *(_DWORD *)(v3 + 0x10000);
  if ( (unsigned int)idx >= 2 )
  {
    v_MACHINE_CHECK_filter(v71);
    return -1073741811;
  }
  v8 = (_DWORD *)(v3 + 92448);
  HvpAquireSpinLock((_DWORD *)(v3 + 92448));
  HvpSetRMCI(0);
  v14 = 56LL * (unsigned int)idx + v13 + 91776;
  if ( *(_BYTE *)(v14 + 52) == 3 )
  {
    v16 = *(_DWORD *)(v14 + 24);
    if ( v16 != 0 )
    {
      *(_DWORD *)(v14 + 24) = v16 - 1;
      if ( (import_table_offset_or_ptr & 3) != 0 )
        goto LABEL_58;
      v17 = ((unsigned int)import_table_offset_or_ptr + 35LL) & 0x1FFFFF000LL;
      if ( (import_table_offset_or_ptr & 0xFFFFF000) != v17 )
        goto LABEL_58;
      v18 = v13 + 0x10000;
      if ( (unsigned int)import_table_offset_or_ptr >= 0x10000 )
      {
        v21 = HvpPhysicalToReal(
                import_table_offset_or_ptr,
                36,
                v12,
                v11,
                v10,
                v9,
                (unsigned int)import_table_offset_or_ptr & 0xFFFFF000,
                v17);
        v19 = *(_QWORD *)(v18 + 26360);
        v20 = v21;
      }
      else
      {
        v19 = *(_QWORD *)(v13 + 91896);
        v20 = (unsigned int)import_table_offset_or_ptr + v19;
      }
      memcpy(v64, v20);
      if ( 4096 - (unsigned int)(import_table_offset_or_ptr & 0xFFF) < 0x24 )
        goto LABEL_58;
      v27 = ((unsigned __int64)(4096 - (unsigned int)(import_table_offset_or_ptr & 0xFFF)) - 36) >> 2;
      if ( v69 > v27 )
        goto LABEL_58;
      if ( (unsigned int)(import_table_offset_or_ptr + 36) >= 0x10000 )
        v28 = HvpPhysicalToReal(import_table_offset_or_ptr + 36, 4LL * v69, v25, v24, v23, v22, v69, v27);
      else
        v28 = (unsigned int)(import_table_offset_or_ptr + 36) + v19;
      v29 = v26 + 0x10000;
      memcpy(*(_QWORD *)(v26 + 92528), v28);
      XeCryptSha(v64, 36, *(const uint8_t **)(v29 + 26992), 4 * (unsigned int)v69, nullptr, 0, v70, 0x14u);
      v30 = (_DWORD *)(v14 + 32);
      if ( (unsigned int)XeCryptMemDiff((unsigned __int8 *)(v14 + 32), v70, 20) != 0 )
      {
        v15 = -1073741279;
        goto LABEL_59;
      }
      v34 = v64;
      for ( i = 5; i != 0; --i )
      {
        *v30 = *(_DWORD *)v34;
        v34 += 4;
        ++v30;
      }
      if ( ((unsigned __int8)export_table & 3) != 0 )
      {
LABEL_58:
        v15 = -1073741503;
        goto LABEL_59;
      }
      LODWORD(v31) = 1;
      LODWORD(v32) = 44;
      LODWORD(v33) = (_DWORD)export_table;
      if ( (unsigned int)sub_29968(v54, v33, 0, v32, v31) != 0 )
      {
        XeCryptAesKey(&v63, (const uint8_t *)(v37 + 91920), v36);
        XeCryptAesEcb(&v63, (const uint8_t *)v54, (uint8_t *)v54, 0);
        if ( v54[0] != 1207959552 || v54[1] != 4740677 || v54[2] != 1207959552 )
        {
LABEL_57:
          v15 = -1073741701;
          goto LABEL_59;
        }
        v55 = ((unsigned __int16)v55 << 16) | (unsigned __int16)v56;
        if ( v55 != v65 )
        {
          v15 = -1073741275;
          goto LABEL_59;
        }
        v38 = ((unsigned __int8)v59 << 24) | v58 & 0xFFFFFF;
        v57 = (v59 << 16) & 0xFF000000 | v57 & 0xFFFFFF;
        v58 = v38;
        if ( v57 < v67 || v66 < v38 )
        {
          v15 = -1073741735;
          goto LABEL_59;
        }
        v39 = export_table + 44;
        v40 = 0;
        if ( v69 == 0 )
        {
LABEL_54:
          v15 = 0;
          goto LABEL_59;
        }
        while ( 1 )
        {
          v41 = *(unsigned int *)(4LL * v40 + *(_QWORD *)(v29 + 26992));
          if ( (v41 & 3) != 0 )
            break;
          v42 = *(_DWORD *)(v14 + 16);
          if ( (unsigned int)v41 < v42
            || (unsigned int)v41 >= *(_DWORD *)(v14 + 20) + v42
            || (unsigned int)sub_29838(v41, v53) == 0 )
          {
            break;
          }
          v45 = (_DWORD *)(((unsigned __int64)v53[0] << 10) & 0x3FFFFFF03FFLL
                         | 0x8000000000000000LL
                         | (unsigned __int16)v41);
          if ( *((unsigned __int8 *)&word_0 + (_QWORD)v45 + 1) != v68 )
            goto LABEL_57;
          LODWORD(v44) = v65;
          LODWORD(v43) = *(unsigned __int16 *)((char *)&dword_2
                                             + (((unsigned __int64)v53[0] << 10) & 0x3FFFFFF03FFLL
                                              | 0x8000000000000000LL
                                              | (unsigned __int16)v41));
          v46 = HvpResolveImportPageLookup(idx, v44, v43);
          v47 = v46;
          if ( (unsigned int)v46 < v62 )
            goto LABEL_55;
          v48 = v46 - v62;
          if ( (unsigned int)v48 >= v61 )
            goto LABEL_55;
          LODWORD(v48) = 4 * v48;
          v49 = v48 + (_WORD)v39;
          if ( (unsigned int)sub_29838(&v39[v48], v53) == 0 || (v53[0] & 1) == 0 || (v53[0] & 0x40000000) == 0 )
            break;
          if ( *(_DWORD *)(((unsigned __int64)v53[0] << 10) & 0x3FFFFFF0000LL | 0x8000000000000000LL | v49) == 0 )
          {
LABEL_55:
            v15 = -1073741512;
            goto LABEL_59;
          }
          v50 = *v45 & 0xFF000000;
          v51 = (v60 << 16)
              + *(_DWORD *)(((unsigned __int64)v53[0] << 10) & 0x3FFFFFF0000LL | 0x8000000000000000LL | v49);
          if ( v50 != 0 )
          {
            if ( v50 != 0x1000000 )
              goto LABEL_57;
            if ( (unsigned __int64)(unsigned int)v41 + 8 > (unsigned int)(*(_DWORD *)(v14 + 20) + *(_DWORD *)(v14 + 16)) )
              break;
            *v45 = ((unsigned int)(v51 + 0x8000) >> 16) | 0x3D600000;
            if ( (_WORD)v41 == 0xFFFC )
            {
              if ( (unsigned int)sub_29838(v41 + 8, v53) == 0 )
                break;
              v52 = (_DWORD *)(((unsigned __int64)v53[0] << 10) & 0x3FFFFFF03FFLL
                             | 0x8000000000000000LL
                             | (unsigned __int16)(v41 + 4));
            }
            else
            {
              v52 = v45 + 1;
            }
            if ( *v52 != ((v68 << 16) | 0x2000000 | v47) )
              goto LABEL_57;
            *v52 = (unsigned __int16)v51 | 0x396B0000;
          }
          else
          {
            *v45 = v51;
          }
          if ( ++v40 >= v69 )
            goto LABEL_54;
        }
      }
      v15 = -1073741482;
    }
    else
    {
      v15 = -1073741619;
    }
  }
  else
  {
    v15 = -1073741811;
  }
LABEL_59:
  HvpSetRMCI(1);
  if ( (_DWORD)v15 != 0 )
    *(_BYTE *)(v14 + 52) = 15;
  HvpReleaseSpinLock(v8);
  v_MACHINE_CHECK_filter(v71);
  return v15;
}


/* ========== [0x2ad00] HvxFinishImageLoad (276 bytes) ========== */
__int64 __fastcall HvxFinishImageLoad(uint32_t idx)
{
  __int64 v1; // r2
  _DWORD *v4; // r29
  __int64 v5; // r3
  __int64 v6; // r2
  __int64 v7; // r30
  __int64 v8; // r31
  int v9; // r11
  _BYTE *v10; // r10
  unsigned int i; // r11

  if ( idx >= 2 )
    return 0;
  v4 = (_DWORD *)(v1 + 92448);
  LODWORD(v5) = (unsigned int)HvpAquireSpinLock((_DWORD *)(v1 + 92448));
  v7 = 0;
  v8 = 56LL * idx + v6 + 91776;
  v9 = *(unsigned __int8 *)(v8 + 52);
  if ( (v9 == 3 || v9 == 4) && *(_DWORD *)(v8 + 24) == 0 )
  {
    if ( (*(_BYTE *)(v8 + 53) & 1) != 0 )
    {
      v10 = (_BYTE *)(((unsigned __int64)*(unsigned __int16 *)(v8 + 54) >> 1) + sub_5A8(v5));
      for ( i = (unsigned int)((*(_DWORD *)(v8 + 20) >> 12) + 1) >> 1; i != 0; --i )
      {
        *v10 &= (unsigned __int8)(2 * *v10) | 0xBB;
        ++v10;
      }
    }
    else
    {
      while ( *(_QWORD *)v8 <= *(_QWORD *)(v8 + 8) )
      {
        **(_DWORD **)v8 = (2 * **(_BYTE **)v8) & 4 | **(_DWORD **)v8 & 0xFFFFFFFB;
        *(_QWORD *)v8 += 4LL;
      }
    }
    v7 = 1;
    *(_BYTE *)(v8 + 52) = 0;
  }
  HvpReleaseSpinLock(v4);
  return v7;
}


/* ========== [0x2ae18] HvxUnmapImageRange (412 bytes) ========== */
__int64 __fastcall HvxUnmapImageRange(unsigned __int64 start_address, char *end_address)
{
  __int64 v2; // r2
  _DWORD *v4; // r25
  __int64 v6; // r3
  __int64 v7; // r30
  __int64 v8; // r27
  _DWORD *v9; // r3
  _DWORD *v10; // r31
  __int64 v11; // r5
  _DWORD *v12; // r3
  __int64 v13; // r2
  _DWORD *v14; // r29
  int v15; // r30
  __int64 v16; // r11
  unsigned __int64 v17; // r11
  __int64 v18; // r9
  _DWORD *v19; // r11

  v4 = (_DWORD *)(v2 + 92448);
  LODWORD(v6) = (unsigned int)HvpAquireSpinLock((_DWORD *)(v2 + 92448));
  v7 = 0;
  v8 = sub_5C0(v6);
  HvpSetRMCI(0);
  if ( (_WORD)start_address == 0
    && (_WORD)end_address == 0
    && (unsigned int)end_address > (unsigned int)start_address
    && ((((_DWORD)end_address - 1) ^ (unsigned int)start_address) & 0xF0000000) == 0 )
  {
    v9 = (_DWORD *)sub_57C(start_address);
    v10 = v9;
    if ( v9 != nullptr && (*v9 == 0 || (*v9 & 0x10) != 0) )
    {
      v12 = (_DWORD *)sub_57C(end_address - 0x10000);
      v14 = v12;
      if ( v12 != nullptr && (*v12 == 0 || (*v12 & 0x20) != 0) )
      {
        v15 = 0;
        do
        {
          v16 = (unsigned int)*v10;
          if ( (v16 & 0xC0000000) != 0 )
            sub_D7C((v16 << 10) & 0x3FFFFFF0000LL);
          v17 = (unsigned int)*v10;
          if ( (v17 & 0xFFFC0) != 0 )
            *(_WORD *)(((v17 >> 5) & 0x7FFE) + v8) &= ~0x800u;
          *v10++ = 0;
          ++v15;
        }
        while ( v10 <= v14 );
        if ( (start_address & 0xF0000000) == 0x90000000 )
        {
          LODWORD(v11) = 16 * v15;
          sub_29760((unsigned __int16)(start_address >> 12), 0, v11);
        }
        v18 = 12;
        v19 = (_DWORD *)(v13 + 92480);
        do
        {
          if ( *v19 >= (unsigned int)start_address && *v19 < (unsigned int)end_address )
            *v19 = 0;
          --v18;
          ++v19;
        }
        while ( (_DWORD)v18 != 0 );
        v7 = 1;
      }
    }
  }
  HvpSetRMCI(1);
  HvpReleaseSpinLock(v4);
  return v7;
}


/* ========== [0x2afb8] sub_2AFB8 (36 bytes) ========== */
bool __fastcall sub_2AFB8(unsigned int a1)
{
  __int64 v1; // r2

  return (_cntlzw(*(_DWORD *)(4LL * a1 + v1 + 92480)) & 0x20) == 0;
}


/* ========== [0x2afe0] sub_2AFE0 (192 bytes) ========== */
unsigned __int64 __fastcall sub_2AFE0(unsigned __int64 a1, unsigned int a2)
{
  unsigned __int16 v2; // r31
  unsigned int v4[4]; // [sp+50h] [-20h] BYREF

  v2 = a1;
  if ( ((((_DWORD)a1 + a2 - 1) ^ (unsigned int)a1) & 0xFFFF0000) != 0 )
    v_MACHINE_CHECK_0();
  if ( (unsigned int)a1 + 0x80000000 > 0x1FFFFFFF || a2 > -1610612736 - (int)a1 )
    v_MACHINE_CHECK_0();
  if ( (unsigned int)sub_29838(a1, v4) == 0 )
    v_MACHINE_CHECK_0();
  if ( (v4[0] & 8) == 0 || (v4[0] & 1) != 0 )
    v_MACHINE_CHECK_0();
  return ((unsigned __int64)v4[0] << 10) & 0x3FFFFFF0000LL | 0x8000000000000000LL | v2;
}


/* ========== [0x2b0a0] sub_2B0A0 (212 bytes) ========== */
__int64 __fastcall sub_2B0A0(unsigned int a1, int a2)
{
  unsigned int *v3; // r3
  unsigned int *v4; // r31
  __int64 v5; // r3
  unsigned int v6; // r10
  unsigned __int64 v7; // r8
  unsigned int v8; // r11
  BOOL v9; // r9

  v3 = (unsigned int *)sub_57C(a1);
  v4 = v3;
  if ( v3 != nullptr && (*v3 & 0x10) != 0 )
  {
    v5 = sub_5C0();
    v6 = 0;
    v7 = (((unsigned __int64)*v4 >> 5) & 0x7FFE) + v5;
    while ( 1 )
    {
      v8 = *v4;
      v9 = (*v4 & 0x20LL) != 0;
      if ( (*v4 & 2) == 0
        || (v8 & 4) == 0
        || (v8 & 0xFFFC0) == 0
        || v8 >> 30 != a2
        || (*(_WORD *)(2LL * v6 + v7) & 0x800) == 0 )
      {
        break;
      }
      ++v6;
      ++v4;
      if ( v9 )
        return 1;
    }
  }
  return 0;
}


/* ========== [0x2b178] HvxEncryptedAllocationReserve (392 bytes) ========== */
__int64 __fastcall HvxEncryptedAllocationReserve(void *address, unsigned __int64 phys_address, uint32_t size)
{
  __int64 v3; // r2
  __int64 v7; // r29
  _DWORD *v8; // r27
  __int64 v9; // r3
  _DWORD *v10; // r31
  __int64 v11; // r3
  __int64 v12; // r10
  int v13; // r9
  _WORD *v14; // r11
  int v15; // r7
  uint32_t v16; // r6
  __int64 v17; // r8
  _DWORD *v18; // r9

  if ( (_WORD)address != 0
    || (_WORD)phys_address != 0
    || (_WORD)size != 0
    || size > 0x1FFFFFF
    || (unsigned int)((_DWORD)address + 1946157056) > 0x1FFFFFF
    || (unsigned int)(phys_address - 0x40000) > 0x1FFBFFFF )
  {
    return 0;
  }
  v7 = 0;
  v8 = (_DWORD *)(v3 + 92448);
  HvpAquireSpinLock((_DWORD *)(v3 + 92448));
  v9 = sub_57C(address);
  v10 = (_DWORD *)v9;
  if ( v9 != 0 )
  {
    v11 = sub_5C0(v9);
    LODWORD(v12) = HIWORD(size);
    v13 = 0;
    v14 = (_WORD *)(v11 + (((unsigned int)phys_address >> 15) & 0xFFFFFFFFFFFFFFFELL));
    if ( HIWORD(size) != 0 )
    {
      while ( 1 )
      {
        v15 = v10[v13];
        if ( (v15 & 8) != 0 || (v15 & 0xFFFC0) != 0 || (v14[v13] & 0x800) != 0 )
          break;
        if ( ++v13 >= (unsigned int)v12 )
          goto LABEL_13;
      }
    }
    else
    {
LABEL_13:
      v16 = 0;
      v17 = (phys_address >> 10) & 0xFFFC0 | 0x16;
      if ( (_DWORD)v12 != 0 )
      {
        v18 = v10;
        v16 = HIWORD(size);
        do
        {
          --v12;
          *v18++ = v17;
          *v14++ |= 0x800u;
          LODWORD(v17) = v17 & 0xFFF0002F | ((v17 & 0xFFFFFFC0) + 64) & 0xFFFC0;
        }
        while ( (_DWORD)v12 != 0 );
      }
      v7 = 1;
      v10[v16 - 1] |= 0x20u;
    }
  }
  HvpReleaseSpinLock(v8);
  return v7;
}


/* ========== [0x2b300] HvxEncryptedAllocationMap (260 bytes) ========== */
__int64 __fastcall HvxEncryptedAllocationMap(void *address)
{
  __int64 v1; // r2
  _DWORD *v3; // r28
  __int64 v4; // r4
  __int64 v5; // r3
  unsigned int *v7; // r31
  __int64 *v8; // r3
  BOOL v9; // r29
  __int64 v10; // r3
  unsigned __int16 v11; // r3
  __int64 v12; // r10

  if ( (_WORD)address != 0 || (unsigned int)((_DWORD)address + 1946157056) > 0x1FFFFFF )
    return 0;
  v3 = (_DWORD *)(v1 + 92448);
  HvpAquireSpinLock((_DWORD *)(v1 + 92448));
  LODWORD(v4) = 0;
  LODWORD(v5) = (_DWORD)address;
  if ( (unsigned int)sub_2B0A0(v5, v4) == 0 )
  {
    HvpReleaseSpinLock(v3);
    return 0;
  }
  v7 = (unsigned int *)sub_57C(address);
  sub_5C0(v7);
  v8 = (__int64 *)HvpSetRMCI(0);
  do
  {
    v9 = (*v7 & 0x20LL) != 0;
    *v7 |= 0xC0000000;
    v10 = sub_5C0(v8);
    v11 = sub_29C68(v10, ((unsigned __int64)*v7 >> 6) & 0x3FFF);
    v12 = (v11 << 20) & 0x3FF00000 | *v7 & 0xC00FFFFF;
    *v7 = (v11 << 20) & 0x3FF00000 | *v7 & 0xC00FFFFF;
    v8 = HvpRelocateCacheLines(
           (__int64 *)((v12 << 10) & 0x3FFF0000 | 0x8000000000000000LL),
           (_QWORD *)((v12 << 10) & 0x3FFFFFF0000LL | 0x8000000000000000LL),
           512);
    *v7++ |= 8u;
  }
  while ( !v9 );
  HvpSetRMCI(1);
  HvpReleaseSpinLock(v3);
  return 1;
}


/* ========== [0x2b408] HvxEncryptedAllocationUnmap (188 bytes) ========== */
__int64 __fastcall HvxEncryptedAllocationUnmap(void *address)
{
  __int64 v1; // r2
  _DWORD *v3; // r30
  unsigned int *v5; // r3
  bool v6; // cr58

  if ( (_WORD)address != 0 || (unsigned int)((_DWORD)address + 1946157056) > 0x1FFFFFF )
    return 0;
  v3 = (_DWORD *)(v1 + 92448);
  HvpAquireSpinLock((_DWORD *)(v1 + 92448));
  if ( (unsigned int)sub_2B0A0(address, 3) == 0 )
  {
    HvpReleaseSpinLock(v3);
    return 0;
  }
  v5 = (unsigned int *)sub_57C(address);
  do
  {
    v6 = (*v5 & 0x20LL) == 0;
    *v5 = *v5 & 0xFFFFFFF6 | 1;
    ++v5;
  }
  while ( v6 );
  HvpReleaseSpinLock(v3);
  return 1;
}


/* ========== [0x2b4c8] HvxEncryptedAllocationRelease (244 bytes) ========== */
__int64 __fastcall HvxEncryptedAllocationRelease(void *address)
{
  __int64 v1; // r2
  _DWORD *v3; // r28
  _DWORD *v5; // r31
  _WORD *v6; // r30
  __int64 v7; // r11
  BOOL v8; // r29

  if ( (_WORD)address != 0 || (unsigned int)((_DWORD)address + 1946157056) > 0x1FFFFFF )
    return 0;
  v3 = (_DWORD *)(v1 + 92448);
  HvpAquireSpinLock((_DWORD *)(v1 + 92448));
  if ( (unsigned int)sub_2B0A0(address, 3) == 0 )
  {
    HvpReleaseSpinLock(v3);
    return 0;
  }
  v5 = (_DWORD *)sub_57C(address);
  v6 = (_WORD *)((((unsigned __int64)(unsigned int)*v5 >> 5) & 0x7FFE) + sub_5C0(v5));
  HvpSetRMCI(0);
  do
  {
    v7 = (unsigned int)*v5;
    v8 = (v7 & 0x20) != 0;
    sub_D7C((v7 << 10) & 0x3FFFFFF03FFLL);
    *v6++ &= ~0x800u;
    *v5++ = 0;
  }
  while ( !v8 );
  HvpSetRMCI(1);
  HvpReleaseSpinLock(v3);
  return 1;
}


/* ========== [0x2b5c0] HvxEncryptedSweepAddressRange (276 bytes) ========== */
__int64 __fastcall HvxEncryptedSweepAddressRange(__int64 a1, __int64 a2)
{
  __int64 v2; // r2
  _DWORD *v5; // r29
  _DWORD *v6; // r31
  __int64 v7; // r3
  unsigned __int64 v8; // r28
  __int64 v9; // r30
  __int64 v10; // r11
  unsigned __int64 v11; // r11

  if ( (_WORD)a1 == 0
    && (_WORD)a2 == 0
    && (unsigned int)(a1 + 1946157056) <= 0x2000000
    && (unsigned int)(a2 + 1946157056) <= 0x2000000
    && (unsigned int)a2 > (unsigned int)a1 )
  {
    v5 = (_DWORD *)(v2 + 92448);
    HvpAquireSpinLock((_DWORD *)(v2 + 92448));
    v6 = (_DWORD *)sub_57C(a1);
    v7 = sub_57C(a2);
    v8 = v7;
    if ( v6 != nullptr && v7 != 0 )
    {
      v9 = sub_5C0(v7);
      HvpSetRMCI(0);
      do
      {
        v10 = (unsigned int)*v6;
        if ( (v10 & 0xC0000000) != 0 )
          sub_D7C((v10 << 10) & 0x3FFFFFF0000LL);
        v11 = (unsigned int)*v6;
        if ( (v11 & 0xFFFC0) != 0 )
          *(_WORD *)(((v11 >> 5) & 0x7FFE) + v9) &= ~0x800u;
        *v6++ = 0;
      }
      while ( (unsigned __int64)v6 <= v8 );
      HvpSetRMCI(1);
    }
    HvpReleaseSpinLock(v5);
  }
  return 0;
}


/* ========== [0x2b6d8] HvxImageTransformImageKey (316 bytes) ========== */
__int64 __fastcall HvxImageTransformImageKey(
        __int64 a1,
        int a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7)
{
  __int64 v7; // r2
  __int64 v9; // r31
  X_CRYPT_AES_STATE v10; // [sp+50h] [-2F0h] BYREF
  _WORD v11[130]; // [sp+1B0h] [-190h] BYREF
  int v12; // [sp+2B4h] [-8Ch]
  _WORD v13[8]; // [sp+2E8h] [-58h] BYREF
  uint8_t v14[44]; // [sp+2F8h] [-48h] BYREF
  int v15; // [sp+324h] [-1Ch]

  v15 = *(_DWORD *)(v7 + 0x10000);
  if ( (a1 & 0xFFF) != 0 || a2 != 372 )
  {
    v_MACHINE_CHECK_filter(v15);
    return -939524092;
  }
  else if ( (*(_QWORD *)(v7 + 91672) & 2LL) != 0
         && ((v9 = HvpPhysicalToReal(a1, 372, a3, a4, a5, a6, a7, a1 & 0xFFF), memcpy(v11, v9), (v12 & 8) == 0)
          || (unsigned int)sub_24D58(v13) != 0) )
  {
    if ( (int)sub_29A68(v11, &v10) >= 0 )
    {
      XeCryptAesEcb(&v10, v14, (uint8_t *)(v9 + 328), 0);
      v_MACHINE_CHECK_filter(v15);
      return 0;
    }
    else
    {
      v_MACHINE_CHECK_filter(v15);
      return -939524090;
    }
  }
  else
  {
    v_MACHINE_CHECK_filter(v15);
    return -939524094;
  }
}


/* ========== [0x2b818] HvpImageXexHeaderParse (492 bytes) ========== */
__int64 __fastcall HvpImageXexHeaderParse(
        __int64 a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  __int64 v8; // r2
  __int64 v9; // r27
  unsigned int v10; // r31
  __int64 v11; // r29
  __int64 v12; // r30
  __int64 v13; // r3
  unsigned __int64 v14; // r5
  __int64 v15; // r7
  __int64 v16; // r6
  __int64 v17; // r10
  __int64 v18; // r9
  __int64 v19; // r8
  __int64 v20; // r7
  __int64 v21; // r6
  __int64 v22; // r5
  unsigned int v23; // r30
  unsigned int *v24; // r28
  __int64 v25; // r3
  __int64 v26; // r7
  __int64 v27; // r6
  __int64 v28; // r30
  unsigned __int64 v29; // r11
  __int64 v30; // r31
  __int64 v31; // r11
  uint8_t *v32; // r9
  __int64 v33; // r8
  __int64 v34; // r7
  __int64 v35; // r6
  __int64 v36; // r5
  __int64 v37; // r2
  _DWORD *v38; // r3
  __int64 v39; // r2
  _DWORD *v40; // r3
  __int64 v41; // r2
  __int64 v42; // r3
  __int64 v43; // r31
  _QWORD *v44; // r2
  _WORD v46[32]; // [sp+50h] [-90h] BYREF
  uint8_t v47[20]; // [sp+90h] [-50h] BYREF
  int v48; // [sp+A4h] [-3Ch]

  v9 = v8 + 0x10000;
  v10 = a2;
  v11 = *(_QWORD *)(v8 + 91896);
  v48 = *(_DWORD *)(v8 + 0x10000);
  if ( (unsigned int)a2 > 0x10 )
  {
    v12 = -939507711;
LABEL_18:
    v42 = *(_QWORD *)(v9 + 26360);
    v43 = v42 + 0x10000;
    memset(v42, 0, 0x10000u);
    v44[11470] = v43;
    v44[11488] = v43;
    v44[11486] = v43;
    goto LABEL_19;
  }
  v13 = HvpPhysicalToReal(a1, a2, a3, a4, a5, a6, a7, a8);
  LODWORD(v14) = 4 * v10;
  memcpy((__int64)v46, v13, v14, v16, v15);
  v23 = 0;
  if ( v10 != 0 )
  {
    v24 = (unsigned int *)v46;
    do
    {
      v25 = HvpPhysicalToReal(*v24, 4096, v22, v21, v20, v19, v18, v17);
      memcpy((v23++ << 12) + v11, v25, 0x1000u, v27, v26);
      ++v24;
    }
    while ( v23 < v10 );
  }
  v28 = *(unsigned int *)(v11 + 8);
  if ( (unsigned int)(v28 - 412) > 0xFE64 )
  {
    v12 = -939507710;
    goto LABEL_18;
  }
  v29 = *(unsigned int *)(v11 + 16);
  if ( v29 >= (unsigned __int64)*(unsigned int *)(v11 + 8) - 388
    || (v30 = v29 + v11 + 8, (unsigned int)((v31 = *(unsigned int *)(v29 + v11 + 264)) - 260) > 0x70) )
  {
    v12 = -939507709;
    goto LABEL_18;
  }
  if ( (unsigned int)v31 < 0x174 )
    memset((unsigned int)v31 + v30, 0, 372 - v31);
  XeCryptSha(
    (const uint8_t *)(v30 + 372),
    (unsigned int)v11 - (unsigned __int64)(unsigned int)(v30 + 372) + v28,
    (const uint8_t *)v11,
    (unsigned int)v30 - (unsigned __int64)(unsigned int)v11,
    nullptr,
    0,
    v47,
    0x14u);
  if ( (unsigned int)XeCryptMemDiff((unsigned __int8 *)(v30 + 348), v47, 20) != 0 )
  {
    v12 = -939507708;
    goto LABEL_18;
  }
  if ( (int)sub_29A68(v30, nullptr, v36, v35, v34, v33, v32) < 0 )
  {
    v12 = -939507707;
    goto LABEL_18;
  }
  *(_QWORD *)(v37 + 91760) = v30;
  v38 = sub_298E8(262150);
  *(_QWORD *)(v39 + 91904) = v38;
  v40 = sub_298E8(17156);
  v12 = 0;
  *(_QWORD *)(v41 + 91888) = v40;
LABEL_19:
  v_MACHINE_CHECK_filter(v48);
  return v12;
}


/* ========== [0x2ba08] HvxImageXexHeader (260 bytes) ========== */
__int64 __fastcall HvxImageXexHeader(uint32_t subfunc_idx, __int64 gather_phys_address, uint32_t gather_size)
{
  __int64 v3; // r2
  _DWORD *v5; // r28
  __int64 v8; // r4
  __int64 v9; // r2
  __int64 v10; // r3
  __int64 v11; // r31
  _QWORD *v12; // r2
  __int64 v13; // r31
  __int64 v14; // r11

  v5 = (_DWORD *)(v3 + 92448);
  HvpAquireSpinLock((_DWORD *)(v3 + 92448));
  if ( subfunc_idx != 0 )
  {
    switch ( subfunc_idx )
    {
      case 1u:
        LODWORD(v8) = gather_size;
        v13 = HvpImageXexHeaderParse(gather_phys_address, v8);
        break;
      case 2u:
        v13 = 0;
        *(_DWORD *)(*(_QWORD *)(v9 + 91896) + 16LL) |= 0x8E050000;
        break;
      case 3u:
        v14 = *(_QWORD *)(v9 + 91904);
        if ( v14 != 0 && *(_DWORD *)(v14 + 8) == *(_DWORD *)(v14 + 4) )
        {
          v13 = 0;
          *(_DWORD *)(v14 + 8) = gather_phys_address;
        }
        else
        {
          v13 = -939507706;
        }
        break;
      default:
        v13 = -939507711;
        break;
    }
  }
  else
  {
    v10 = *(_QWORD *)(v9 + 91896);
    v11 = v10 + 0x10000;
    memset(v10, 0, 0x10000);
    v12[11470] = v11;
    v12[11488] = v11;
    v12[11486] = v11;
    v13 = 0;
  }
  HvpReleaseSpinLock(v5);
  return v13;
}


/* ========== [0x2bb10] HvxImageDvdEmulationMode (36 bytes) ========== */
__int64 HvxImageDvdEmulationMode()
{
  return (unsigned int)HvNoOp_ReturnZero();
}


/* ========== [0x2bb38] HvxImageUserMode (12 bytes) ========== */
__int64 HvxImageUserMode()
{
  return -939507705;
}


/* ========== [0x2bb48] sub_2BB48 (80 bytes) ========== */
_QWORD *__fastcall sub_2BB48(int a1)
{
  __int64 v1; // r2
  __int64 v2; // r9
  _QWORD *v3; // r11

  v2 = 0;
  v3 = (_QWORD *)(v1 + 91944);
  if ( *(_WORD *)(v1 + 91938) == 0 )
    return nullptr;
  while ( ((*v3 >> 2) & 0x3FLL) != a1 )
  {
    ++v2;
    ++v3;
    if ( (unsigned int)v2 >= *(unsigned __int16 *)(v1 + 91938) )
      return nullptr;
  }
  return v3;
}


/* ========== [0x2bb98] sub_2BB98 (76 bytes) ========== */
__int64 sub_2BB98()
{
  __int64 v0; // r2
  __int64 v1; // r9
  _QWORD *v2; // r10

  if ( *(_WORD *)(v0 + 91938) != 0 )
  {
    v1 = *(unsigned __int16 *)(v0 + 91938);
    v2 = (_QWORD *)(v0 + 91944);
    do
    {
      --v1;
      *v2++ &= ~1uLL;
    }
    while ( (_DWORD)v1 != 0 );
  }
  *(_WORD *)(v0 + 91936) &= ~1u;
  return 0;
}


/* ========== [0x2bbe8] sub_2BBE8 (92 bytes) ========== */
__int64 __fastcall sub_2BBE8(unsigned int a1)
{
  _QWORD *v2; // r11

  if ( a1 >= 0x3F )
    return -939507711;
  v2 = sub_2BB48(a1);
  if ( v2 == nullptr )
    return -939507703;
  *v2 |= 1uLL;
  return 0;
}


/* ========== [0x2bc48] sub_2BC48 (92 bytes) ========== */
__int64 __fastcall sub_2BC48(unsigned int a1)
{
  _QWORD *v2; // r11

  if ( a1 >= 0x3F )
    return -939507711;
  v2 = sub_2BB48(a1);
  if ( v2 == nullptr )
    return -939507703;
  *v2 &= ~1uLL;
  return 0;
}


/* ========== [0x2bca8] sub_2BCA8 (440 bytes) ========== */
__int64 __fastcall sub_2BCA8(unsigned __int64 a1, unsigned int a2, __int64 a3, __int64 a4)
{
  __int64 v4; // r2
  __int64 v6; // r31
  char v7; // r27
  int v11; // r28
  __int64 v12; // r5
  __int64 v13; // r2
  __int64 v14; // r10
  __int64 v15; // r11
  _DWORD v16[4]; // [sp+50h] [-1C0h] BYREF
  int v17; // [sp+60h] [-1B0h]
  int v18; // [sp+74h] [-19Ch]
  unsigned int v19; // [sp+78h] [-198h]
  X_CRYPT_AES_STATE v20; // [sp+80h] [-190h] BYREF

  v6 = v4 + 91936;
  v7 = a2;
  if ( (*(_WORD *)(v4 + 91936) & 2) != 0 )
    return -939507705;
  if ( *(unsigned __int16 *)(v4 + 91938) >= 0x3Fu )
    return -939507704;
  if ( a2 >= 0x3F )
    return -939507711;
  if ( (unsigned int)a3 >= 0x1000 )
    return -939507711;
  if ( (unsigned int)a4 >= 0x1000 )
    return -939507711;
  HvpSetRMCI(0);
  v11 = sub_29968((unsigned __int64)v16, a1, 0, 0x2Cu, 1);
  HvpSetRMCI(1);
  if ( v11 == 0 )
    return -939507711;
  XeCryptAesKey(&v20, (const uint8_t *)(v13 + 91920), v12);
  XeCryptAesEcb(&v20, (const uint8_t *)v16, (uint8_t *)v16, 0);
  if ( v16[0] != 1207959552
    || v16[1] != 4740677
    || v16[2] != 1207959552
    || (unsigned int)a3 < v19
    || (unsigned int)a3 >= v18 + v19
    || (unsigned int)a4 < v19
    || (unsigned int)a4 >= v18 + v19 )
  {
    return -939507711;
  }
  v14 = v6 + 8;
  v15 = 8LL * (unsigned __int16)(*(_WORD *)(v6 + 2))++;
  *(_QWORD *)(v15 + v14) = (a4 << 52) | *(_QWORD *)(v15 + v6 + 8) & 0xFFFFFFFFFFLL | (a3 << 40) & 0xFFF0000000000LL;
  *(_QWORD *)(v15 + v14) = (4
                          * (((unsigned __int64)((v16[3] << 16) | (unsigned int)(unsigned __int16)v17) << 6) | v7 & 0x3F))
                         | (a4 << 52)
                         | (a3 << 40) & 0xFFF0000000000LL;
  return 0;
}


/* ========== [0x2be60] HvxImageShim (256 bytes) ========== */
__int64 __fastcall HvxImageShim(int a1, unsigned int a2, int a3, int a4, int a5)
{
  __int64 v5; // r2
  _DWORD *v7; // r26
  __int64 v12; // r6
  __int64 v13; // r5
  __int64 v14; // r4
  __int64 v15; // r3
  __int64 v16; // r2
  __int64 v17; // r3
  __int64 v18; // r11
  __int16 v19; // r10
  __int64 v20; // r31

  v7 = (_DWORD *)(v5 + 92448);
  LODWORD(v15) = (unsigned int)HvpAquireSpinLock((_DWORD *)(v5 + 92448));
  switch ( a1 )
  {
    case 0:
      v17 = sub_2BB98(v15);
LABEL_16:
      v20 = v17;
      goto LABEL_18;
    case 1:
      v18 = v16 + 0x10000;
      v19 = *(_WORD *)(v16 + 91936) | 2;
LABEL_7:
      v20 = 0;
      *(_WORD *)(v18 + 26400) = v19;
      goto LABEL_18;
    case 2:
      v18 = v16 + 0x10000;
      v19 = *(_WORD *)(v16 + 91936) | 1;
      goto LABEL_7;
    default:
      break;
  }
  if ( a1 != 3 )
  {
    switch ( a1 )
    {
      case 4:
        v17 = sub_2BBE8(a2);
        break;
      case 5:
        v17 = sub_2BC48(a2);
        break;
      case 6:
        LODWORD(v12) = a5;
        LODWORD(v13) = a4;
        LODWORD(v14) = a3;
        v17 = sub_2BCA8(a2, v14, v13, v12);
        break;
      default:
        v20 = -939507711;
        goto LABEL_18;
    }
    goto LABEL_16;
  }
  v20 = 0;
  *(_WORD *)(v16 + 91936) &= ~1u;
LABEL_18:
  HvpReleaseSpinLock(v7);
  return v20;
}


/* ========== [0x2bf60] sub_2BF60 (264 bytes) ========== */
__int64 __fastcall sub_2BF60(__int64 a1, unsigned __int64 a2, unsigned __int64 a3, int a4, int a5, int a6)
{
  unsigned int v6; // r11
  unsigned __int64 v7; // r31
  unsigned __int64 v8; // r29
  __int64 v12; // r28
  __int64 v13; // r9
  __int64 v14; // r8
  __int64 v15; // r7
  __int64 v16; // r6
  __int64 v17; // r4
  __int64 v18; // r3
  unsigned __int64 v19; // r30
  unsigned int v21; // [sp+50h] [-50h] BYREF

  v6 = *(_DWORD *)(a1 + 16);
  v7 = a2;
  v8 = a3;
  if ( (unsigned int)a2 < v6 || (unsigned int)a2 + (unsigned __int64)(unsigned int)a3 > *(_DWORD *)(a1 + 20) + v6 )
    return 0;
  v12 = 4096;
  if ( (*(_BYTE *)(a1 + 53) & 1) == 0 )
    v12 = 0x10000;
  while ( (_DWORD)v8 != 0 )
  {
    if ( (unsigned int)sub_29838(v7, &v21) == 0 )
      return 0;
    LODWORD(v18) = v21;
    v19 = ((v12 + v7) & ~(v12 - 1)) - v7;
    if ( (unsigned int)v19 > (unsigned int)v8 )
      v19 = v8;
    LODWORD(v13) = a4;
    LODWORD(v14) = a6;
    LODWORD(v15) = a5;
    LODWORD(v16) = v19;
    LODWORD(v17) = v7;
    if ( (unsigned int)((__int64 (__fastcall *)(__int64, __int64, unsigned __int64, __int64, __int64, __int64, __int64))loc_7B8)(
                         v18,
                         v17,
                         ((unsigned __int64)v21 << 10) & 0x3FFFFFF03FFLL | 0x8000000000000000LL | (unsigned __int16)v7,
                         v16,
                         v15,
                         v14,
                         v13) == 0 )
      return 0;
    v8 -= v19;
    v7 += v19;
  }
  return 1;
}


/* ========== [0x2c068] sub_2C068 (364 bytes) ========== */
__int64 __fastcall sub_2C068(__int64 a1, __int64 a2)
{
  __int64 v4; // r27
  __int64 v5; // r7
  __int64 v6; // r6
  __int64 v7; // r2
  __int64 v8; // r28
  __int64 v9; // r9
  __int64 v10; // r11
  __int64 v11; // r10
  unsigned __int64 i; // r31
  int v13; // r3
  unsigned __int64 v14; // r3
  unsigned __int64 v15; // r10
  _DWORD *v16; // r11
  unsigned int v18; // [sp+50h] [-40h] BYREF

  v4 = 0;
  HvpSetRMCI(0);
  v8 = 4096;
  if ( (*(_BYTE *)(a1 + 53) & 1) == 0 )
    v8 = 0x10000;
  v9 = 0;
  v10 = *(unsigned int *)(a1 + 16);
  v11 = *(unsigned int *)(a1 + 20) + v10;
  v18 = 0;
  for ( i = v11 - 192; (unsigned int)i >= (unsigned int)v10; i -= v8 )
  {
    v13 = sub_29838(i, &v18);
    v9 = v18;
    if ( v13 != 0 && (v18 & 8) == 8 && (v18 & 1) == 1 && (v18 & 2) == 0 && (v18 & 0x40000000) == 0x40000000 )
      break;
    LODWORD(v10) = *(_DWORD *)(a1 + 16);
  }
  if ( (unsigned int)i >= *(_DWORD *)(a1 + 16) )
  {
    v14 = (v9 << 10) & 0x3FFFFFF0000LL | 0x8000000000000000LL | (unsigned __int16)i;
    v15 = v14 + 192;
    v16 = (_DWORD *)v14;
    if ( v14 >= v14 + 192 )
      goto LABEL_16;
    do
    {
      if ( *v16 != 0 )
        break;
      ++v16;
    }
    while ( (unsigned __int64)v16 < v15 );
    if ( (unsigned __int64)v16 >= v15 )
    {
LABEL_16:
      memcpy(v14, v7 + 66548, 0xC0u, v6, v5);
      *(_DWORD *)a2 = i;
      v4 = 1;
      *(_BYTE *)(a2 + 15) = 25;
      *(_BYTE *)(a2 + 7) = 23;
      *(_DWORD *)(a2 + 8) = i + 92;
      *(_DWORD *)(a2 + 4) = 5911;
      *(_DWORD *)(a2 + 12) = -2147477223;
      *(_DWORD *)(a1 + 28) = i;
    }
  }
  HvpSetRMCI(1);
  return v4;
}


/* ========== [0x2c1d8] sub_2C1D8 (140 bytes) ========== */
__int64 __fastcall sub_2C1D8(int a1, unsigned int a2, int *a3, int a4, int a5)
{
  unsigned int v5; // r11
  unsigned int v6; // r10

  v5 = a2;
  v6 = a2 + a4;
  if ( (a1 & 8) != 8 || (a1 & 1) != 1 || (a1 & 2) != 0 || (a1 & 0x40000000) != 0x40000000 )
    return 0;
  if ( a2 < v6 )
  {
    do
    {
      if ( *a3 == 1317011488 )
        *a3 = (a5 - v5) & 0x3FFFFFC | 0x48000000;
      v5 += 4;
      ++a3;
    }
    while ( v5 < v6 );
  }
  return 1;
}


/* ========== [0x2c268] sub_2C268 (152 bytes) ========== */
__int64 __fastcall sub_2C268(int a1, unsigned int a2, int *a3, int a4, int a5, int a6)
{
  unsigned int v6; // r11
  unsigned int v7; // r9
  int v8; // r10

  v6 = a2;
  v7 = a2 + a4;
  if ( (a1 & 8) != 8 || (a1 & 1) != 1 || (a1 & 2) != 0 || (a1 & 0x40000000) != 0x40000000 )
    return 0;
  v8 = (a6 - a2) & 0x3FFFFFC | 0x48000000;
  if ( a2 < v7 )
  {
    do
    {
      if ( *a3 == v8 )
        *a3 = (a5 - v6) & 0x3FFFFFC | 0x48000000;
      v6 += 4;
      v8 -= 4;
      ++a3;
    }
    while ( v6 < v7 );
  }
  return 1;
}


/* ========== [0x2c300] sub_2C300 (416 bytes) ========== */
__int64 __fastcall sub_2C300(__int64 a1, unsigned __int64 a2, __int64 a3)
{
  __int64 v3; // r2
  unsigned int v7; // r27
  __int64 v8; // r31
  unsigned int v9; // r11
  __int64 v10; // r2
  __int64 v11; // r11
  unsigned __int8 *v12; // r29
  unsigned int v14[4]; // [sp+50h] [-F0h] BYREF
  unsigned __int8 v15[164]; // [sp+60h] [-E0h] BYREF
  int v16; // [sp+104h] [-3Ch]

  v7 = 0;
  v16 = *(_DWORD *)(v3 + 0x10000);
  do
  {
    v8 = *(unsigned int *)(4LL * v7 + a3);
    if ( (_DWORD)v8 == 0 )
      break;
    if ( (unsigned int)a2 >= (unsigned int)v8 && (unsigned int)a2 < (int)v8 + 80 )
      goto LABEL_22;
    ++v7;
  }
  while ( v7 < 2 );
  v9 = *(_DWORD *)(a1 + 16);
  if ( (unsigned int)a2 >= v9
    && (unsigned int)a2 < *(_DWORD *)(a1 + 20) + v9
    && (unsigned int)sub_29838(a2, v14) != 0
    && (v14[0] & 8) == 8
    && (v14[0] & 1) == 1
    && (v14[0] & 2) == 0
    && (v14[0] & 0x40000000) == 0x40000000
    && (*(_DWORD *)(((unsigned __int64)v14[0] << 10) & 0x3FFFFFF0000LL | 0x8000000000000000LL | (unsigned __int16)a2)
      & 0xFC000003) == 0xF8000000 )
  {
    v11 = 0;
    v12 = (unsigned __int8 *)(v10 + 66384);
    do
    {
      if ( *(_DWORD *)(((unsigned __int64)v14[0] << 10) & 0x3FFFFFF0000LL | 0x8000000000000000LL | (unsigned __int16)a2) == *(_DWORD *)&v12[4 * (unsigned int)v11] )
        break;
      ++v11;
    }
    while ( (unsigned int)v11 < 0x12 );
    if ( (unsigned int)v11 < 0x12 )
    {
      LODWORD(v11) = 4 * v11;
      v8 = a2 - v11;
      if ( (unsigned int)sub_29968((unsigned __int64)v15, a2 - v11, a1, 0xA4u, 1) != 0
        && (unsigned int)XeCryptMemDiff(v15, v12, 164) == 0 )
      {
        if ( v7 < 2 )
          *(_DWORD *)(4LL * v7 + a3) = v8;
LABEL_22:
        v_MACHINE_CHECK_filter(v16);
        return v8;
      }
    }
  }
  v_MACHINE_CHECK_filter(v16);
  return 0;
}


/* ========== [0x2c4a0] sub_2C4A0 (604 bytes) ========== */
__int64 __fastcall sub_2C4A0(__int64 a1, _QWORD *a2, __int64 a3, int a4)
{
  int v7; // r23
  __int64 v8; // r31
  unsigned __int64 v9; // r25
  unsigned __int64 v10; // r28
  unsigned int v11; // r11
  int v12; // r2
  _DWORD *v13; // r11
  unsigned __int64 v14; // r31
  int *v15; // r29
  int v16; // r11
  int v17; // r9
  int v18; // r8
  int v19; // r7
  int v20; // r6
  unsigned __int64 v22; // r30
  int v23; // r3
  int v24; // r2
  _QWORD v25[12]; // [sp+50h] [-60h] BYREF

  v7 = a4 + 92;
  v25[0] = *a2;
  v8 = HIDWORD(v25[0]);
  v9 = (((unsigned __int64)LODWORD(v25[0]) >> 6) & 0xFFFFFC) + HIDWORD(v25[0]);
  v10 = 4 * (unsigned int)LOBYTE(v25[0]) + (unsigned __int64)HIDWORD(v25[0]);
  if ( (v25[0] & 0x300000000LL) != 0 )
    return 0;
  v11 = *(_DWORD *)(a1 + 16);
  if ( HIDWORD(v25[0]) < v11
    || (unsigned int)v9 > *(_DWORD *)(a1 + 20) + v11
    || HIDWORD(v25[0]) > (unsigned int)v10
    || (unsigned int)v10 > (unsigned int)v9 )
  {
    return 0;
  }
  if ( LOBYTE(v25[0]) < 2u || LODWORD(v25[0]) >> 30 == 0 || LODWORD(v25[0]) >> 30 == 2 )
    return 1;
  if ( (unsigned int)sub_29838(HIDWORD(v25[0]), (unsigned int *)v25) == 0
    || (BYTE3(v25[0]) & 8) != 8
    || (BYTE3(v25[0]) & 1) != 1
    || (v25[0] & 0x200000000LL) != 0
    || (HIDWORD(v25[0]) & 0x40000000) != 0x40000000 )
  {
    return 0;
  }
  v13 = (_DWORD *)(((unsigned __int64)HIDWORD(v25[0]) << 10) & 0x3FFFFFF0000LL
                 | 0x8000000000000000LL
                 | (unsigned __int16)v8);
  if ( *v13 != 2106065574 )
    return 1;
  v14 = v8 + 4;
  v15 = v13 + 1;
  if ( (v14 & 0xFFF) != 0 )
  {
LABEL_22:
    v16 = *v15;
    if ( *v15 == -1853751304 )
    {
      v17 = (a4 - v14 + 72) & 0x3FFFFFC | 0x48000001;
      v18 = 0;
      v19 = v7 + 80;
      v20 = v12 + 180696;
LABEL_24:
      *v15 = v17;
      return sub_2BF60(a1, v10, v9 - v10, v20, v19, v18);
    }
    if ( (v16 & 0xFC000003) == 0x48000001 )
    {
      v22 = (-(__int64)(v16 & 0x2000000) | v16 & 0x3FFFFFC) + v14;
      v23 = sub_2C300(a1, v22, a3);
      if ( v23 != 0 )
      {
        v20 = v24 + 180840;
        v17 = (v22 - v23 - v14 + a4) & 0x3FFFFFC | 0x48000001;
        v18 = v22 + 80;
        v19 = v22 - v23 + v7;
        goto LABEL_24;
      }
    }
    return 1;
  }
  if ( (unsigned int)sub_29838(v14, (unsigned int *)v25) != 0
    && (BYTE3(v25[0]) & 8) == 8
    && (BYTE3(v25[0]) & 1) == 1
    && (v25[0] & 0x200000000LL) == 0
    && (HIDWORD(v25[0]) & 0x40000000) == 0x40000000 )
  {
    v15 = (int *)(((unsigned __int64)HIDWORD(v25[0]) << 10) & 0x3FFFFFF0000LL
                | 0x8000000000000000LL
                | (unsigned __int16)v14);
    goto LABEL_22;
  }
  return 0;
}


/* ========== [0x2c700] sub_2C700 (148 bytes) ========== */
__int64 __fastcall sub_2C700(int a1, __int64 a2, _QWORD *a3, unsigned int a4, __int64 a5, __int64 a6)
{
  int v8; // r27
  unsigned int i; // r30

  v8 = *(_DWORD *)(a5 + 28);
  if ( (a1 & 8) != 8 || (a1 & 1) != 1 || (a1 & 2) != 2 || (a1 & 0x40000000) != 0 )
    return 0;
  for ( i = a4 >> 3; i != 0; --i )
  {
    if ( (unsigned int)sub_2C4A0(a5, a3, a6, v8) == 0 )
      return 0;
    ++a3;
  }
  return 1;
}


/* ========== [0x2c798] sub_2C798 (104 bytes) ========== */
__int64 __fastcall sub_2C798(__int64 a1, unsigned __int64 a2, unsigned __int64 a3)
{
  int v6; // r2
  __int64 v7; // r31
  int v9; // [sp+50h] [-30h] BYREF

  HvpSetRMCI(0);
  v7 = sub_2BF60(a1, a2, a3, v6 + 182016, a1, (int)&v9);
  HvpSetRMCI(1);
  return v7;
}


/* ========== [0x2c800] HvxXexUnk24 (180 bytes) ========== */
__int64 __fastcall HvxXexUnk24(unsigned int a1, __int64 a2)
{
  __int64 v2; // r2
  _DWORD *v6; // r29
  __int64 v7; // r9
  __int64 v8; // r8
  __int64 v9; // r7
  __int64 v10; // r6
  __int64 v11; // r5
  __int64 v12; // r10
  __int64 v13; // r2
  __int64 v14; // r30
  __int64 v15; // r31
  __int64 v16; // r4

  if ( a1 >= 2 )
    return 0;
  v6 = (_DWORD *)(v2 + 92448);
  HvpAquireSpinLock((_DWORD *)(v2 + 92448));
  v12 = 56LL * a1;
  v14 = 0;
  v15 = v12 + v13 + 91776;
  if ( *(_BYTE *)(v15 + 52) == 3 && (a2 & 7) == 0 && (unsigned __int64)(a2 & 0xFFF) + 16 <= 0x1000 )
  {
    LODWORD(v16) = HvpPhysicalToReal(a2, 16, v11, v10, v9, v8, v7, v12);
    v14 = sub_2C068(v15, v16);
    if ( (_DWORD)v14 != 0 )
      *(_BYTE *)(v15 + 52) = 4;
  }
  HvpReleaseSpinLock(v6);
  return v14;
}


/* ========== [0x2c8b8] HvxXexUnk25 (216 bytes) ========== */
__int64 __fastcall HvxXexUnk25(unsigned int a1, unsigned int a2, unsigned int a3)
{
  __int64 v3; // r2
  _DWORD *v8; // r27
  __int64 v9; // r5
  __int64 v10; // r4
  __int64 v11; // r2
  __int64 v12; // r29
  __int64 v13; // r31
  unsigned int v14; // r9

  if ( a1 >= 2 )
    return 0;
  v8 = (_DWORD *)(v3 + 92448);
  HvpAquireSpinLock((_DWORD *)(v3 + 92448));
  v12 = 0;
  v13 = 56LL * a1 + v11 + 91776;
  if ( *(_BYTE *)(v13 + 52) != 4
    || (a2 & 7) != 0
    || (a3 & 7) != 0
    || (v14 = *(_DWORD *)(v13 + 16), a2 < v14)
    || a2 + (unsigned __int64)a3 > *(_DWORD *)(v13 + 20) + v14
    || (LODWORD(v9) = a3, LODWORD(v10) = a2, (_DWORD)(v12 = sub_2C798(v13, v10, v9)) == 0) )
  {
    *(_BYTE *)(v13 + 52) = 15;
  }
  HvpReleaseSpinLock(v8);
  return v12;
}


/* ========== [0x2c990] HvxCreateImageMapping (1896 bytes) ========== */
__int64 __fastcall HvxCreateImageMapping(uint32_t idx, __int64 hv_sec_phys, uint32_t image_size, __int64 image_size_1)
{
  __int64 v4; // r2
  _DWORD *v10; // r23
  __int64 v11; // r10
  __int64 v12; // r9
  __int64 v13; // r8
  __int64 v14; // r7
  __int64 v15; // r6
  __int64 v16; // r5
  __int64 v17; // r2
  __int64 v18; // r25
  __int64 v19; // r31
  __int64 v20; // r3
  __int64 v21; // r4
  __int64 v22; // r2
  __int64 v23; // r29
  __int64 v24; // r31
  unsigned int v25; // r4
  __int64 v26; // r5
  _QWORD *v27; // r3
  __int64 v28; // r11
  __int64 v29; // r10
  __int64 v30; // r4
  __int64 v31; // r3
  __int64 v32; // r5
  __int64 v33; // r2
  int v34; // r9
  unsigned __int16 v35; // r11
  unsigned int v36; // r11
  __int64 v37; // r27
  unsigned int v38; // r8
  int v39; // r7
  int v40; // r11
  unsigned int v41; // r11
  _DWORD *v42; // r10
  __int16 *v43; // r11
  __int64 i; // ctr
  _DWORD *v45; // r11
  uint8_t *v46; // r9
  __int64 j; // ctr
  __int64 v48; // r3
  __int64 v49; // r5
  unsigned __int64 v50; // r3
  unsigned int v51; // r10
  char v52; // r9
  unsigned int *v53; // r11
  unsigned int k; // r11
  X_CRYPT_AES_STATE v55; // [sp+50h] [-4B0h] BYREF
  X_CRYPT_AES_STATE v56; // [sp+1B0h] [-350h] BYREF
  uint8_t v57[256]; // [sp+310h] [-1F0h] BYREF
  unsigned int v58; // [sp+410h] [-F0h]
  unsigned int v59; // [sp+414h] [-ECh] BYREF
  unsigned int v60; // [sp+418h] [-E8h]
  __int16 v61; // [sp+41Ch] [-E4h] BYREF
  uint8_t v62[16]; // [sp+430h] [-D0h] BYREF
  uint8_t v63[8]; // [sp+440h] [-C0h] BYREF
  _DWORD v64[18]; // [sp+448h] [-B8h] BYREF
  uint8_t v65[20]; // [sp+490h] [-70h] BYREF
  int v66; // [sp+4A4h] [-5Ch]

  v66 = *(_DWORD *)(v4 + 0x10000);
  if ( idx >= 2 )
  {
    v_MACHINE_CHECK_filter(v66);
    return -1073741811;
  }
  v10 = (_DWORD *)(v4 + 92448);
  HvpAquireSpinLock((_DWORD *)(v4 + 92448));
  if ( (_DWORD)hv_sec_phys != 0 )
    v18 = HvpPhysicalToReal(hv_sec_phys, 372, v16, v15, v14, v13, v12, v11);
  else
    v18 = *(_QWORD *)(v17 + 91760);
  if ( (unsigned int)(hv_sec_phys & 0xFFF) + 260 > 0x1000 )
    goto LABEL_7;
  memcpy(v57, v18);
  if ( v58 < 0x104 || v58 > 0x174 )
  {
    v19 = -1073741701;
    goto LABEL_100;
  }
  if ( (unsigned __int64)((hv_sec_phys & 0xFFF) + v58) > 0x1000 )
    goto LABEL_7;
  memcpy(&v59, v18 + 260);
  memset(&v57[v58], 0, 372LL - v58);
  if ( (v59 & 0xC0000000) != 0 )
  {
    v20 = sub_298E8(262150);
    if ( v20 == 0 || *(_DWORD *)(v20 + 12) != 1481115776 )
    {
      XeCryptSha(v57, 256, nullptr, 0, nullptr, 0, v65, 0x14u);
      LODWORD(v21) = 1;
      if ( (unsigned int)sub_2D370(v65, v21) != 0 )
      {
        v19 = -939511804;
        goto LABEL_100;
      }
    }
  }
  v19 = sub_29A68(v57, &v56);
  if ( (int)v19 < 0 )
    goto LABEL_100;
  if ( (_DWORD)image_size_1 == 0 )
  {
    v19 = 0;
    goto LABEL_100;
  }
  v23 = 56LL * idx + v22 + 91776;
  if ( *(_BYTE *)(v23 + 52) != 0 )
  {
    v19 = -1073741525;
    goto LABEL_100;
  }
  v24 = v22 + 0x10000;
  v25 = v59;
  v26 = *(_QWORD *)(v22 + 90832);
  if ( (_DWORD)hv_sec_phys == 0 && (v59 & 0x4000000) != 0 )
  {
    v27 = (_QWORD *)sub_298E8(16388);
    if ( v27 == nullptr )
    {
      v19 = -1073741275;
      goto LABEL_100;
    }
    v28 = v27[1];
    v29 = *(_QWORD *)(v26 + 1584) & *v27;
    if ( v28 == 0 )
    {
      if ( v29 != 0 )
        goto LABEL_32;
LABEL_31:
      v19 = -1073741727;
      goto LABEL_100;
    }
    if ( v28 != v29 )
      goto LABEL_31;
  }
LABEL_32:
  if ( (*(_DWORD *)(v26 + 1556) & 0x100) == 0 )
  {
    v59 = v25 & 0xF7FFFFFF;
    *(_DWORD *)(v18 + 260) &= ~0x8000000u;
    v25 = v59;
  }
  if ( (v25 & 0x8000000) != 0 )
  {
    v31 = sub_298E8(262150);
    if ( v31 == 0 || (LODWORD(v30) = 0, LODWORD(v31) = *(_DWORD *)(v31 + 12), (unsigned int)sub_B168(v31, v30) == 0) )
    {
      v19 = -939503613;
      goto LABEL_100;
    }
    LOBYTE(v25) = v59;
  }
  if ( ((v25 & 8) != 0 || (unsigned int)XeCryptMemDiff((unsigned __int8 *)(v22 + 67752), (unsigned __int8 *)v64, 16) == 0)
    && (unsigned int)sub_24D58(v64) == 0 )
  {
    v19 = -1073414142;
    goto LABEL_100;
  }
  v34 = v59;
  if ( (v59 & 6) != 0 && (v59 & 6) != 6 )
  {
    v19 = -1073741482;
    goto LABEL_100;
  }
  if ( (*(_WORD *)(v33 + 92820) & 4) != 0 )
  {
    if ( (v59 & 0x30) == 0 && (_DWORD)hv_sec_phys == 0 )
      goto LABEL_47;
    v34 = v59 | 0x20000000;
    v59 |= 0x20000000u;
  }
  else if ( (v59 & 0x10) != 0 )
  {
LABEL_47:
    v19 = -1073405951;
    goto LABEL_100;
  }
  v35 = *(_WORD *)(*(_QWORD *)(v24 + 25296) + 1538LL);
  if ( v35 == 258 )
    v35 = 511;
  if ( (v34 & 0x20000000) == 0 && (HIBYTE(v35) >= 4u || (((unsigned __int8)v35 << (8 * HIBYTE(v35))) & v64[14]) == 0) )
  {
    v19 = -1073414143;
    goto LABEL_100;
  }
  v19 = 0;
  v36 = 0;
  v37 = v33 + 92480;
  do
  {
    if ( *(_DWORD *)(4LL * v36 + v37) != 0 && ((1 << (v36 + 8)) & v34) != 0 )
    {
      v19 = -1073741563;
      goto LABEL_100;
    }
    ++v36;
  }
  while ( v36 < 0xC );
  v38 = v60;
  *(_DWORD *)(v23 + 20) = image_size;
  *(_DWORD *)(v23 + 24) = 0;
  *(_BYTE *)(v23 + 53) = 0;
  *(_DWORD *)(v23 + 16) = v38;
  if ( (v34 & 0x10000000) != 0 )
    *(_BYTE *)(v23 + 53) = 1;
  v39 = *(_DWORD *)v62;
  if ( *(_DWORD *)v62 == 0 )
    *(_BYTE *)(v23 + 53) |= 2u;
  *(_DWORD *)(v23 + 28) = v64[8];
  if ( (_WORD)v38 == 0 )
  {
    if ( (v38 & 0xF0000000) == 0x90000000 )
    {
      if ( (*(_BYTE *)(v23 + 53) & 1) == 0 )
      {
LABEL_68:
        v19 = -1073741811;
        goto LABEL_100;
      }
      v40 = 4095;
    }
    else
    {
      if ( (*(_BYTE *)(v23 + 53) & 1) != 0 )
        goto LABEL_68;
      v40 = 0xFFFF;
    }
    if ( image_size > (unsigned int)image_size_1 || (v40 & (unsigned int)image_size_1) != 0 || (v40 & image_size) != 0 )
      goto LABEL_68;
    v41 = 0x10000000 - (v38 & 0xFFFFFFF);
    if ( image_size <= v41 && (unsigned int)image_size_1 <= v41 )
    {
      v42 = (_DWORD *)(v23 + 32);
      v43 = &v61;
      for ( i = 5; i != 0; --i )
      {
        *v42 = *(_DWORD *)v43;
        v43 += 2;
        ++v42;
      }
      v64[0] = v39;
      v64[1] = v38;
      XeCryptAesKey(&v55, (const uint8_t *)(v33 + 91920), v32);
      XeCryptAesEcb(&v55, v62, v62, 1);
      XeCryptAesEcb(&v55, v63, v63, 1);
      v45 = (_DWORD *)(v18 + 288);
      v46 = v62;
      for ( j = 8; j != 0; --j )
      {
        *v45 = *(_DWORD *)v46;
        v46 += 4;
        ++v45;
      }
      v48 = sub_57C(v60);
      *(_QWORD *)v23 = v48;
      if ( v48 != 0 )
      {
        v50 = sub_57C(v60 + image_size_1 - 1);
        *(_QWORD *)(v23 + 8) = v50;
        if ( v50 != 0 )
        {
          v51 = 6;
          if ( (v60 & 0xF0000000) == 0x90000000 )
          {
            v52 = *(_BYTE *)(v23 + 53) | 1;
            v51 = 7;
            *(_WORD *)(v23 + 54) = v60 >> 12;
            *(_BYTE *)(v23 + 53) = v52;
          }
          v53 = *(unsigned int **)v23;
          if ( *(_QWORD *)v23 > v50 )
          {
LABEL_88:
            if ( (*(_BYTE *)(v23 + 53) & 1) != 0 )
            {
              LODWORD(v49) = (unsigned int)image_size_1 >> 12;
              sub_29760(*(unsigned __int16 *)(v23 + 54), v51 & 0xFFFFFFFE, v49);
            }
            XeCryptAesEcb(&v56, (const uint8_t *)(v18 + 328), (uint8_t *)(v18 + 328), 0);
            for ( k = 0; k < 0xC; ++k )
            {
              if ( ((1 << (k + 8)) & v59) != 0 )
                *(_DWORD *)(4LL * k + v37) = v60;
            }
            *(_BYTE *)(v23 + 52) = 1;
          }
          else
          {
            while ( *v53 == 0 )
            {
              v51 = (32 * (v53 == *(unsigned int **)(v23 + 8)))
                  | (16 * (v53 == *(unsigned int **)v23))
                  | v51 & 0xFFFFFFCF;
              *v53++ = v51;
              if ( (unsigned __int64)v53 > *(_QWORD *)(v23 + 8) )
                goto LABEL_88;
            }
            while ( (unsigned __int64)--v53 >= *(_QWORD *)v23 )
              *v53 = 0;
            v19 = -1073741800;
          }
          goto LABEL_100;
        }
      }
    }
  }
LABEL_7:
  v19 = -1073741503;
LABEL_100:
  HvpReleaseSpinLock(v10);
  v_MACHINE_CHECK_filter(v66);
  return v19;
}


/* ========== [0x2d0f8] HvxSetRevocationList (532 bytes) ========== */
__int64 __fastcall HvxSetRevocationList(
        __int64 a1,
        int a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  __int64 v8; // r2
  __int64 v10; // r3
  __int64 v11; // r2
  __int64 v12; // r30
  __int64 v13; // r9
  __int64 v14; // r8
  __int64 v15; // r7
  __int64 v16; // r6
  __int64 v17; // r5
  __int64 v18; // r27
  unsigned int v19; // r29
  unsigned int v20; // r28
  __int64 v21; // r11
  unsigned int v22; // r31
  __int64 v23; // r3
  __int64 v24; // r2
  __int64 v25; // r2
  __int64 v26; // r31
  int v27; // [sp+50h] [-210h] BYREF
  _WORD v28[128]; // [sp+54h] [-20Ch] BYREF
  __int16 v29[8]; // [sp+154h] [-10Ch] BYREF
  unsigned int v30; // [sp+164h] [-FCh]
  _WORD v31[76]; // [sp+168h] [-F8h] BYREF
  uint8_t v32[20]; // [sp+200h] [-60h] BYREF
  int v33; // [sp+214h] [-4Ch]

  v33 = *(_DWORD *)(v8 + 0x10000);
  if ( a2 != 424 )
  {
    v_MACHINE_CHECK_filter(v33);
    return -939511808;
  }
  v10 = HvpPhysicalToReal(a1, 424, a3, a4, a5, a6, a7, a8);
  memcpy(&v27, v10);
  if ( v27 != 0 )
  {
    v_MACHINE_CHECK_filter(v33);
    return -939511807;
  }
  if ( v30 > 0x3E80 || v30 % 0x14 != 0 )
  {
    v_MACHINE_CHECK_filter(v33);
    return -939511806;
  }
  else
  {
    v12 = *(_QWORD *)(v11 + 90920);
    HvpAquireSpinLock((_DWORD *)v12);
    sub_3830(0, 0x10000);
    v18 = v12 + 28;
    v19 = 0;
    *(_DWORD *)(v12 + 20) = v27;
    *(_DWORD *)(v12 + 24) = v30;
    v20 = v30;
    do
    {
      if ( v20 == 0 )
        goto LABEL_13;
      v21 = 2LL * v19;
      v22 = *(_DWORD *)&v31[v21 + 36];
      if ( v22 == 0 || v22 > v20 )
        goto LABEL_19;
      v23 = HvpPhysicalToReal(
              *(unsigned int *)&v31[v21],
              *(unsigned int *)&v31[v21 + 36],
              v17,
              v16,
              v15,
              v14,
              v13,
              (__int64)v31);
      memcpy(v18, v23);
      ++v19;
      v20 -= v22;
      v18 += v22;
    }
    while ( v19 < 0x12 );
    if ( v20 != 0 )
    {
LABEL_19:
      v26 = -939511806;
      goto LABEL_17;
    }
LABEL_13:
    XeCryptSha(
      (const uint8_t *)(v12 + 4),
      16,
      (const uint8_t *)v29,
      16,
      (const uint8_t *)(v12 + 28),
      *(unsigned int *)(v12 + 24),
      v32,
      0x14u);
    if ( (unsigned int)sub_38D8(v32, v28, v24 + 69880) == 0
      && (*(_DWORD *)(v25 + 68584) != 32 || (unsigned int)sub_38D8(v32, v28, v25 + 68584) == 0) )
    {
      v26 = -939511805;
LABEL_17:
      *(_DWORD *)(v12 + 20) = 0;
      *(_DWORD *)(v12 + 24) = 0;
      memset(v12 + 28, 0, 16000);
      goto LABEL_18;
    }
    sub_3830(0x10000, 0);
    v26 = 0;
LABEL_18:
    HvpReleaseSpinLock((_DWORD *)v12);
    v_MACHINE_CHECK_filter(v33);
    return v26;
  }
}


/* ========== [0x2d310] sub_2D310 (92 bytes) ========== */
_DWORD *__fastcall sub_2D310(_BYTE *a1)
{
  __int64 v1; // r2
  __int64 v3; // r30
  _BYTE *v4; // r31
  __int64 i; // ctr

  v3 = *(_QWORD *)(v1 + 90920);
  HvpAquireSpinLock((_DWORD *)v3);
  v4 = (_BYTE *)(v3 + 4);
  sub_20060((uint8_t *)(v3 + 4), 16);
  for ( i = 16; i != 0; --i )
    *a1++ = *v4++;
  return HvpReleaseSpinLock((_DWORD *)v3);
}


/* ========== [0x2d370] sub_2D370 (144 bytes) ========== */
__int64 __fastcall sub_2D370(__int64 a1, __int64 a2)
{
  __int64 v2; // r2
  _DWORD *v3; // r29

  v3 = *(_DWORD **)(v2 + 90920);
  HvpAquireSpinLock(v3);
  HvpReleaseSpinLock(v3);
  return a2;
}


/* ========== [0x2d400] sub_2D400 (284 bytes) ========== */
__int64 __fastcall sub_2D400(__int64 a1, const uint8_t *a2, __int64 a3)
{
  int *v4; // r2
  int *v6; // r5
  int *v7; // r31
  __int64 v8; // r30
  uint8_t v9[20]; // [sp+50h] [-40h] BYREF
  int v10; // [sp+64h] [-2Ch]

  XeCryptSha(a2, a3, nullptr, 0, nullptr, 0, v9, 0x14u);
  if ( (unsigned int)XeCryptMemDiff((unsigned __int8 *)(a1 + 12), v9, 20) != 0 )
  {
    v_MACHINE_CHECK_filter(v10);
    return -939511803;
  }
  if ( *(_DWORD *)a1 == 1129466960 )
  {
    v6 = v4 + 16942;
LABEL_5:
    v7 = nullptr;
    goto LABEL_6;
  }
  if ( *(_DWORD *)a1 != 1129466956 )
  {
    v7 = nullptr;
    goto LABEL_7;
  }
  v6 = v4 + 17470;
  v7 = v4 + 17146;
  if ( v4[17146] != 32 )
    goto LABEL_5;
LABEL_6:
  if ( (unsigned int)sub_38D8((__int64)v9, a1 + 32, v6) == 0 )
  {
LABEL_7:
    v8 = -939511805;
    if ( v7 == nullptr || (unsigned int)sub_38D8((__int64)v9, a1 + 32, v7) == 0 )
      goto LABEL_10;
  }
  v8 = 0;
LABEL_10:
  v_MACHINE_CHECK_filter(v10);
  return v8;
}


/* ========== [0x2d520] sub_2D520 (216 bytes) ========== */
__int64 __fastcall sub_2D520(__int64 a1, __int64 a2, __int64 a3)
{
  __int64 v3; // r2
  __int64 v5; // r29
  uint8_t *v6; // r30
  __int64 v7; // r5
  uint8_t *v8; // r10
  uint8_t *v9; // r9
  __int64 i; // ctr
  __int64 v11; // r31
  X_CRYPT_AES_STATE v13; // [sp+50h] [-1B0h] BYREF
  uint8_t v14[16]; // [sp+1B0h] [-50h] BYREF
  uint8_t v15[16]; // [sp+1C0h] [-40h] BYREF
  int v16; // [sp+1D0h] [-30h]

  v5 = a2 - 336;
  v6 = (uint8_t *)(a1 + 336);
  v16 = *(_DWORD *)(v3 + 0x10000);
  XeCryptAesKey(&v13, byte_54, a3);
  XeCryptAesEcb(&v13, (const uint8_t *)(a1 + 304), v15, 0);
  XeCryptAesKey(&v13, v15, v7);
  v8 = (uint8_t *)(a1 + 288);
  v9 = v14;
  for ( i = 16; i != 0; --i )
    *v9++ = *v8++;
  XeCryptAesCbc(&v13, (const uint8_t *)(a1 + 320), 0x10u, (uint8_t *)(a1 + 320), v14, 0);
  XeCryptAesCbc(&v13, v6, v5, v6, v14, 0);
  v11 = sub_2D400(a1, v6, v5);
  v_MACHINE_CHECK_filter(v16);
  return v11;
}


/* ========== [0x2d5f8] sub_2D5F8 (212 bytes) ========== */
__int64 __fastcall sub_2D5F8(__int64 a1, int a2)
{
  __int64 v2; // r2
  uint8_t *v5; // r30
  uint8_t *v6; // r29
  uint8_t *v7; // r10
  __int64 i; // ctr
  __int64 v9; // r5
  __int64 v10; // r5
  X_CRYPT_AES_STATE v12; // [sp+50h] [-1B0h] BYREF
  uint8_t v13[16]; // [sp+1B0h] [-50h] BYREF
  uint8_t v14[16]; // [sp+1C0h] [-40h] BYREF
  int v15; // [sp+1D0h] [-30h]

  v5 = (uint8_t *)(a1 + 288);
  v6 = (uint8_t *)(a1 + 336);
  v15 = *(_DWORD *)(v2 + 0x10000);
  sub_20060((uint8_t *)(a1 + 288), 16);
  v7 = v14;
  for ( i = 16; i != 0; --i )
    *v7++ = *v5++;
  sub_20060(v13, 16);
  XeCryptAesKey(&v12, v13, v9);
  XeCryptAesCbc(&v12, (const uint8_t *)(a1 + 320), 0x10u, (uint8_t *)(a1 + 320), v14, 1);
  XeCryptAesCbc(&v12, v6, a2 - 336, v6, v14, 1);
  XeCryptAesKey(&v12, byte_20, v10);
  XeCryptAesEcb(&v12, v13, (uint8_t *)(a1 + 304), 1);
  return v_MACHINE_CHECK_filter(v15);
}


/* ========== [0x2d6d0] HvpRevokeListDecrypt (216 bytes) ========== */
__int64 __fastcall HvpRevokeListDecrypt(__int64 a1, __int64 a2, __int64 a3)
{
  __int64 v3; // r2
  __int64 v5; // r29
  uint8_t *v6; // r30
  __int64 v7; // r5
  uint8_t *v8; // r10
  uint8_t *v9; // r9
  __int64 i; // ctr
  __int64 v11; // r31
  X_CRYPT_AES_STATE v13; // [sp+50h] [-1B0h] BYREF
  uint8_t v14[16]; // [sp+1B0h] [-50h] BYREF
  uint8_t v15[16]; // [sp+1C0h] [-40h] BYREF
  int v16; // [sp+1D0h] [-30h]

  v5 = a2 - 336;
  v6 = (uint8_t *)(a1 + 336);
  v16 = *(_DWORD *)(v3 + 0x10000);
  XeCryptAesKey(&v13, byte_20, a3);
  XeCryptAesEcb(&v13, (const uint8_t *)(a1 + 304), v15, 0);
  v8 = (uint8_t *)(a1 + 288);
  v9 = v14;
  for ( i = 16; i != 0; --i )
    *v9++ = *v8++;
  XeCryptAesKey(&v13, v15, v7);
  XeCryptAesCbc(&v13, (const uint8_t *)(a1 + 320), 0x10u, (uint8_t *)(a1 + 320), v14, 0);
  XeCryptAesCbc(&v13, v6, v5, v6, v14, 0);
  v11 = sub_2D400(a1, v6, v5);
  v_MACHINE_CHECK_filter(v16);
  return v11;
}


/* ========== [0x2d7a8] sub_2D7A8 (244 bytes) ========== */
__int64 __fastcall sub_2D7A8(__int64 a1, __int64 a2, unsigned __int64 a3, __int64 a4, __int64 a5)
{
  __int64 v5; // r2
  __int64 v9; // r6
  __int16 *v10; // r9
  __int64 v11; // r2
  _BYTE *v12; // r11
  __int64 i; // ctr
  __int64 v14; // r3
  __int64 v15; // r7
  __int64 v16; // r6
  __int64 v17; // r11
  int v18; // r7
  unsigned __int64 v19; // r5
  _WORD v21[2]; // [sp+50h] [-180h] BYREF
  __int16 v22; // [sp+54h] [-17Ch] BYREF
  __int64 v23; // [sp+170h] [-60h]
  __int64 v24; // [sp+178h] [-58h]
  __int64 v25; // [sp+180h] [-50h]
  __int64 v26; // [sp+188h] [-48h]
  _QWORD v27[2]; // [sp+190h] [-40h] BYREF
  int v28; // [sp+1A0h] [-30h]

  v28 = *(_DWORD *)(v5 + 0x10000);
  memcpy((__int64)v21, a2, 0x150u, a4, a5);
  v10 = &v22;
  v12 = (_BYTE *)(*(_QWORD *)(v11 + 91072) + 2506LL);
  for ( i = 5; i != 0; --i )
  {
    *(_BYTE *)v10 = *v12++;
    v10 = (__int16 *)((char *)v10 + 1);
  }
  v14 = *(_QWORD *)a1;
  v23 = 0;
  v24 = 0;
  v25 = 0;
  v26 = 0;
  v27[0] = 0;
  v27[1] = 0;
  memcpy(v14, (__int64)v21, 0x150u, v9, (__int64)v27);
  memcpy(*(_QWORD *)a1 + 336LL, a2 + 336, a3, v16, v15);
  v17 = *(_QWORD *)(a1 + 8);
  v18 = *(_DWORD *)(v17 + 4);
  v19 = *(unsigned int *)(a1 + 16) - (a3 + 336);
  *(_DWORD *)(a1 + 20) = a3 + 336;
  *(_DWORD *)(a1 + 24) = v18;
  memset((unsigned int)a3 + v17, 0, v19);
  return v_MACHINE_CHECK_filter(v28);
}


/* ========== [0x2d8a0] sub_2D8A0 (136 bytes) ========== */
__int64 sub_2D8A0()
{
  __int64 v0; // r2
  __int64 v1; // r31
  uint8_t v3[20]; // [sp+50h] [-30h] BYREF
  int v4; // [sp+64h] [-1Ch]

  v1 = v0 + 0x10000;
  XeCryptSha((const uint8_t *)(*(_QWORD *)(v0 + 90912) + 20LL), 1004, nullptr, 0, nullptr, 0, v3, 0x14u);
  if ( (unsigned int)XeCryptMemDiff(*(unsigned __int8 **)(v1 + 25376), v3, 20) != 0 )
    v_MACHINE_CHECK_0();
  return v_MACHINE_CHECK_filter(v4);
}


/* ========== [0x2d928] sub_2D928 (100 bytes) ========== */
_QWORD *__fastcall sub_2D928(__int64 a1)
{
  __int64 v2; // r2
  _QWORD *result; // r3
  __int64 v4; // r11

  sub_2D8A0();
  if ( a1 == 0 )
    return nullptr;
  result = (_QWORD *)(*(_QWORD *)(v2 + 90912) + 32LL);
  v4 = 0;
  while ( *result != a1 )
  {
    ++v4;
    result += 2;
    if ( (unsigned int)v4 >= 0x3E )
      return nullptr;
  }
  return result;
}


/* ========== [0x2d990] sub_2D990 (444 bytes) ========== */
__int64 __fastcall sub_2D990(__int64 a1, __int64 a2, __int64 a3, __int64 a4, __int64 a5)
{
  __int64 v5; // r2
  __int64 v6; // r31
  __int64 v8; // r2
  __int64 v9; // r2
  _DWORD *v10; // r29
  unsigned __int64 v11; // r31
  _QWORD *v12; // r3
  __int64 v13; // r2
  unsigned int v14; // r11
  __int64 v15; // r30
  __int64 v16; // r10
  unsigned int v17; // r11
  __int64 v18; // r11
  _WORD v19[2]; // [sp+50h] [-1F0h] BYREF
  unsigned int v20; // [sp+54h] [-1ECh]
  int v21; // [sp+68h] [-1D8h]
  _WORD v22[132]; // [sp+F8h] [-148h] BYREF
  uint8_t v23[20]; // [sp+200h] [-40h] BYREF
  int v24; // [sp+214h] [-2Ch]

  v6 = *(_QWORD *)(v5 + 91072) + 2504LL;
  v24 = *(_DWORD *)(v5 + 0x10000);
  memcpy((__int64)v19, a1, 0x1A8u, a4, a5);
  if ( (unsigned int)XeCryptMemDiff((unsigned __int8 *)v19, (unsigned __int8 *)v6, 424) == 0
    || ((*(_DWORD *)(v6 + 24) ^ v21) & 0xFFFFFF) != 0 )
  {
    v_MACHINE_CHECK_filter(v24);
    return -939511803;
  }
  XeCryptSha((const uint8_t *)v19, 168, nullptr, 0, nullptr, 0, v23, 0x14u);
  if ( (unsigned int)sub_38D8((__int64)v23, (__int64)v22, (int *)(v8 + 70424)) == 0 )
  {
    v_MACHINE_CHECK_filter(v24);
    return -939511805;
  }
  v10 = (_DWORD *)(v9 + 90872);
  v11 = ((v20 | ((unsigned __int64)v19[1] << 32)) << 16) & 0xFFFFFFFFFF000000LL;
  HvpAquireSpinLock((_DWORD *)(v9 + 90872));
  v12 = sub_2D928(v11);
  if ( v12 != nullptr )
  {
    v14 = *((_DWORD *)v12 + 2);
    if ( v14 < 0xFFFFFFFE )
      *((_DWORD *)v12 + 2) = v14 + 1;
    v15 = 0;
  }
  else
  {
    v16 = *(_QWORD *)(v13 + 90912);
    v17 = *(_DWORD *)(v16 + 20);
    if ( v17 >= 0x3E )
    {
      v15 = -939511804;
      goto LABEL_11;
    }
    v15 = 0;
    *(_DWORD *)(v16 + 20) = v17 + 1;
    v18 = 16 * (v17 + 2LL) + v16;
    *(_QWORD *)v18 = v11;
    *(_DWORD *)(v18 + 8) = 1;
    *(_DWORD *)(v18 + 12) = 0;
  }
  XeCryptSha(
    (const uint8_t *)(*(_QWORD *)(v13 + 90912) + 20LL),
    1004,
    nullptr,
    0,
    nullptr,
    0,
    *(uint8_t **)(v13 + 90912),
    0x14u);
LABEL_11:
  HvpReleaseSpinLock(v10);
  v_MACHINE_CHECK_filter(v24);
  return v15;
}


/* ========== [0x2db50] sub_2DB50 (132 bytes) ========== */
void sub_2DB50()
{
  __int64 v0; // r2
  __int64 v1; // r11
  __int64 v2; // r9
  __int64 v3; // r9
  _BYTE *v4; // r10
  _BYTE *v5; // r11
  __int64 i; // ctr

  v1 = *(_QWORD *)(v0 + 90912);
  v2 = *(unsigned int *)(v1 + 20);
  *(_DWORD *)(v1 + 20) = v2 + 1;
  v3 = 16 * (v2 + 2) + v1;
  v4 = (_BYTE *)v3;
  v5 = (_BYTE *)(*(_QWORD *)(v0 + 91072) + 2506LL);
  for ( i = 5; i != 0; --i )
    *v4++ = *v5++;
  *(_DWORD *)(v3 + 8) = -1;
  *(_DWORD *)(v3 + 12) = 0;
  XeCryptSha(
    (const uint8_t *)(*(_QWORD *)(v0 + 90912) + 20LL),
    1004,
    nullptr,
    0,
    nullptr,
    0,
    *(uint8_t **)(v0 + 90912),
    0x14u);
}


/* ========== [0x2dbd8] sub_2DBD8 (184 bytes) ========== */
void sub_2DBD8()
{
  __int64 v0; // r3
  __int64 v1; // r2
  __int64 v2; // r3
  __int64 v3; // r2
  __int64 v4; // r29
  unsigned __int64 v5; // r3
  __int64 v6; // r2
  __int64 v7; // r3
  __int64 v8; // r9
  __int64 v9; // r2

  v0 = sub_270();
  *(_QWORD *)(v1 + 90920) = v0 + 0x8000;
  v2 = sub_298();
  v4 = v3 + 0x10000;
  *(_QWORD *)(v3 + 90912) = v2 + 0x8000;
  v5 = sub_280();
  *(_DWORD *)(v6 + 90896) = 0x20000;
  *(_DWORD *)(v6 + 90900) = 0;
  *(_DWORD *)(v6 + 90904) = 0;
  *(_QWORD *)(v6 + 90880) = v5;
  *(_QWORD *)(v6 + 90888) = v5 + 336;
  v7 = sub_294();
  v8 = *(_QWORD *)(v4 + 25376);
  *(_DWORD *)(v9 + 90960) = 0x8000;
  *(_DWORD *)(v9 + 90964) = 0;
  *(_DWORD *)(v9 + 90968) = 0;
  *(_QWORD *)(v9 + 90944) = v7;
  *(_QWORD *)(v9 + 90952) = v7 + 336;
  XeCryptSha((const uint8_t *)(v8 + 20), 1004, nullptr, 0, nullptr, 0, (uint8_t *)v8, 0x14u);
}


/* ========== [0x2dc90] HvxRevokeLoad (480 bytes) ========== */
__int64 __fastcall HvxRevokeLoad(unsigned int a1, __int64 a2, __int64 a3)
{
  __int64 v3; // r2
  unsigned int v7; // r24
  int v8; // r27
  __int64 v9; // r26
  unsigned int v10; // r11
  __int64 v12; // r3
  __int64 v13; // r31
  __int64 v14; // r4
  __int64 v15; // r5
  __int64 v16; // r4
  __int64 v17; // r3
  __int64 v18; // r2
  __int64 v19; // r29
  int v20; // r30
  __int64 v21; // r5

  v7 = (a2 + 127) & 0xFFFFFF80;
  if ( a3 != 0 )
  {
    v8 = 1;
    v9 = v3 + 90880;
    v10 = 0x20000;
  }
  else
  {
    v8 = 0;
    v9 = v3 + 90944;
    v10 = 0x8000;
  }
  if ( (_WORD)a1 != 0 || (a2 & 0xF) != 0 || (unsigned int)a2 < 0x150 || (unsigned int)a2 > v10 || v7 > v10 )
    return -939511808;
  if ( *(_QWORD *)v9 == 0 )
    return -939511804;
  HvpSetRMCI(0);
  v12 = HvpRelocatePhysicalToEncrypted(a1, v7);
  v13 = v12;
  LODWORD(v14) = a2;
  if ( v8 != 0 )
    v17 = HvpRevokeListDecrypt(v12, v14);
  else
    v17 = sub_2D520(v12, v14);
  v19 = v17;
  if ( (_DWORD)v17 == 0 )
  {
    v20 = *(_DWORD *)(v13 + 336);
    if ( v20 == (unsigned int)a2 - 336LL )
    {
      if ( *(_DWORD *)(v13 + 340) > *(_DWORD *)(v9 + 24) )
      {
        if ( v8 != 0 )
        {
          if ( *(_QWORD *)(v13 + 320) != a3 || *(unsigned __int8 *)(v13 + 335) != *(unsigned __int8 *)(v18 + 91680) )
          {
            v19 = -939511803;
            goto LABEL_26;
          }
          sub_3830(0x200000, 0);
        }
        LODWORD(v15) = v20;
        LODWORD(v16) = v13;
        LODWORD(v17) = v9;
        sub_2D7A8(v17, v16, v15);
        goto LABEL_26;
      }
      v19 = -939511802;
    }
    else
    {
      v19 = -939511806;
    }
  }
LABEL_26:
  memset(v13, 0, a2);
  if ( (_DWORD)v19 != 0 )
  {
    memset(*(_QWORD *)v9, 0, 336);
    *(_DWORD *)(v9 + 24) = -1;
  }
  HvpRelocateEncryptedToPhysical((__int64 *)v13, v7, v21);
  HvpSetRMCI(1);
  return v19;
}


/* ========== [0x2de70] HvxRevokeSave (368 bytes) ========== */
__int64 __fastcall HvxRevokeSave(unsigned int a1, __int64 a2, __int64 a3)
{
  __int64 v3; // r2
  unsigned int v7; // r26
  __int64 v8; // r30
  __int64 v10; // r31
  __int64 v11; // r28
  __int64 v12; // r5
  __int64 v13; // r3
  __int64 v14; // r2
  __int64 v15; // r4
  __int64 v16; // r3

  v7 = (a2 + 127) & 0xFFFFFF80;
  if ( (_WORD)a1 == 0 && (a2 & 0xF) == 0 && (unsigned int)(a2 - 336) <= 0x1FEB0 && v7 <= 0x20000 )
  {
    v8 = v3 + 90880;
    if ( *(_QWORD *)(v3 + 90880) == 0 )
      return -939511804;
    HvpSetRMCI(0);
    v10 = HvpRelocatePhysicalToEncrypted(a1, v7);
    v11 = sub_2D520(v10, a2);
    if ( (_DWORD)v11 == 0 )
    {
      if ( *(_DWORD *)(v10 + 336) == (unsigned int)a2 - 336LL )
      {
        if ( *(_DWORD *)(v10 + 340) > *(_DWORD *)(v8 + 24) )
        {
          LODWORD(v13) = sub_2D7A8(v8, v10, *(unsigned int *)(v10 + 336));
          *(_QWORD *)(v10 + 320) = a3;
          *(_BYTE *)(v10 + 335) = *(_BYTE *)(v14 + 91680);
          v16 = ((__int64 (__fastcall *)(__int64))loc_2B70)(v13);
          if ( (_DWORD)v16 != 0 )
            ++*(_BYTE *)(v10 + 335);
          LODWORD(v15) = a2;
          LODWORD(v16) = v10;
          sub_2D5F8(v16, v15);
          goto LABEL_11;
        }
        v11 = -939511802;
      }
      else
      {
        v11 = -939511806;
      }
    }
    memset(v10, 0, a2);
LABEL_11:
    HvpRelocateEncryptedToPhysical((__int64 *)v10, v7, v12);
    HvpSetRMCI(1);
    return v11;
  }
  return -939511808;
}


/* ========== [0x2dfe0] HvxRevokeUpdate (320 bytes) ========== */
__int64 __fastcall HvxRevokeUpdate(uint32_t buffer_address, __int64 buffer_size, __int64 arg3)
{
  unsigned int v6; // r27
  __int64 v7; // r3
  __int64 v8; // r2
  __int64 v9; // r31
  _DWORD *v10; // r4
  __int64 v11; // r29
  __int64 v12; // r3
  __int64 v13; // r30
  __int64 v14; // r2
  __int64 v15; // r5

  v6 = (buffer_size + 127) & 0xFFFFFF80;
  if ( (_WORD)buffer_address != 0
    || (buffer_size & 0xF) != 0
    || (unsigned int)(buffer_size - 336) > 0x1FEB0
    || v6 > 0x20000 )
  {
    return -939511808;
  }
  HvpSetRMCI(0);
  v7 = HvpRelocatePhysicalToEncrypted(buffer_address, v6);
  v9 = v7;
  v10 = *(_DWORD **)(v8 + 90880);
  if ( v10 != nullptr && *v10 != 0 && (_DWORD)(v11 = *(unsigned int *)(v8 + 90900)) != 0 )
  {
    LODWORD(v12) = memcpy(v7, v10);
    v13 = 0;
    *(_DWORD *)(v9 + 4) = 0;
    *(_BYTE *)(v9 + 8) = 0;
    *(_BYTE *)(v9 + 9) = 0;
    *(_BYTE *)(v9 + 10) = 0;
    *(_BYTE *)(v9 + 11) = 0;
    *(_QWORD *)(v9 + 320) = 0;
    *(_QWORD *)(v9 + 328) = 0;
    *(_QWORD *)(v9 + 320) = arg3;
    *(_BYTE *)(v9 + 335) = *(_BYTE *)(v14 + 91680);
    if ( (unsigned int)((__int64 (__fastcall *)(__int64))loc_2B70)(v12) != 0 )
      ++*(_BYTE *)(v9 + 335);
    sub_2D5F8(v9, v11);
  }
  else
  {
    memset(v7, 0, buffer_size);
    v13 = -939511804;
  }
  HvpRelocateEncryptedToPhysical((__int64 *)v9, v6, v15);
  HvpSetRMCI(1);
  return v13;
}


/* ========== [0x2e120] sub_2E120 (176 bytes) ========== */
__int64 sub_2E120()
{
  __int64 v0; // r30
  __int64 v1; // r3
  _QWORD *v2; // r2
  __int64 v3; // r7
  __int64 v4; // r6
  __int64 result; // r3
  __int64 v6; // r2

  v0 = sub_298();
  v1 = sub_270() + 65024;
  v2[11357] = v0 + 40960;
  v2[11356] = v0 + 49152;
  v2[11355] = v1;
  v2[11354] = v0 + 0x8000;
  result = memcpy(v1, (__int64)(v2 + 9106), 0x21u, v4, v3);
  *(_WORD *)(v0 + 34304) = -10210;
  *(_WORD *)(v0 + 34312) = *(_WORD *)(v6 + 92828);
  *(_DWORD *)(v0 + 34320) = 34767315;
  *(_DWORD *)(v0 + 34332) = *(_DWORD *)(v6 + 92844);
  return result;
}


/* ========== [0x2e1d0] sub_2E1D0 (76 bytes) ========== */
__int64 __fastcall sub_2E1D0(__int64 result)
{
  _QWORD *v1; // r2
  __int64 v2; // r10

  v2 = v1[11384];
  if ( (*(_BYTE *)(v2 + 26) & 1) != 0 )
  {
    result = 0;
  }
  else if ( (*(_BYTE *)(v2 + 26) & 2) != 0 )
  {
    result &= v1[11358];
  }
  *(_QWORD *)(v2 + 48) = result;
  *(_QWORD *)(v1[11354] + 1584LL) = result;
  return result;
}


/* ========== [0x2e220] sub_2E220 (216 bytes) ========== */
void sub_2E220()
{
  __int64 v0; // r2
  __int64 v1; // r11
  __int16 v2; // r31
  __int64 v3; // r11

  v1 = *(_QWORD *)(v0 + 91072);
  v2 = *(_WORD *)(v1 + 28);
  if ( (*(_WORD *)(v0 + 92828) & 4) != 0 )
  {
    v2 = 0;
  }
  else if ( (unsigned __int64)(*(_QWORD *)(v0 + 92832) - 7407360LL) <= 0x1FF )
  {
    v3 = v1 + 2930;
    v2 |= 0x40u;
    if ( *(_DWORD *)(v3 + 272) != 1330858311 )
      v_MACHINE_CHECK_0();
    if ( (unsigned int)XeCryptMemDiff((unsigned __int8 *)(v3 + 288), (unsigned __int8 *)(v0 + 73020), 28) == 0 )
      v2 &= ~0x20u;
  }
  *(_WORD *)(*(_QWORD *)(v0 + 90832) + 1540LL) = v2;
}


/* ========== [0x2e2f8] sub_2E2F8 (456 bytes) ========== */
__int64 __fastcall sub_2E2F8(__int64 a1)
{
  __int64 v1; // r2
  __int64 v2; // r28
  __int64 v3; // r30
  __int64 v4; // r31
  __int64 v5; // r11
  __int64 v6; // r29
  __int64 v7; // r3
  int v8; // r26
  int v9; // r25
  __int64 v10; // r7
  __int64 v11; // r6
  __int64 v12; // r2
  __int64 v13; // r30
  __int64 v14; // r7
  __int64 v15; // r6
  __int64 v16; // r2
  __int64 v17; // r7
  __int64 v18; // r6
  __int64 v19; // r2
  __int64 v20; // r7
  __int64 v21; // r6
  __int64 v22; // r2
  __int64 v23; // r7
  __int64 v24; // r6
  __int64 v25; // r2
  __int64 v26; // r7
  __int64 v27; // r6
  __int64 v28; // r7
  __int64 v29; // r6
  __int64 v30; // r5
  unsigned int *FlashBase; // r3
  X_CRYPT_AES_STATE v33; // [sp+50h] [-1D0h] BYREF
  _QWORD v34[2]; // [sp+1B0h] [-70h] BYREF
  unsigned __int64 v35[2]; // [sp+1C0h] [-60h] BYREF
  int v36; // [sp+1D0h] [-50h]

  v2 = v1 + 0x10000;
  v3 = *(_QWORD *)(v1 + 90832);
  v4 = *(_QWORD *)(v1 + 90856);
  v5 = *(_QWORD *)(v1 + 91072);
  v36 = *(_DWORD *)(v1 + 0x10000);
  v6 = v5 + 24;
  *(_QWORD *)(v1 + 90864) = a1;
  *(_WORD *)(v3 + 1536) = -10210;
  *(_WORD *)(v3 + 1544) = *(_WORD *)(v1 + 92828);
  *(_WORD *)(v3 + 1546) = *(_WORD *)(v1 + 92820);
  if ( (unsigned int)sub_35A0() != 0 )
  {
    v7 = *(_QWORD *)(v6 + 24);
    v8 = *(_DWORD *)(v6 + 12);
    v9 = *(_DWORD *)(v6 + 16);
  }
  else
  {
    v7 = 0;
    v8 = 0;
    v9 = 0;
  }
  sub_2E1D0(v7);
  sub_2E220();
  *(_DWORD *)(v3 + 1560) = v8;
  *(_DWORD *)(v3 + 1568) = v9;
  memcpy(v4, v6 + 2480, 0x1A8u, v11, v10);
  v13 = v12 + 67768;
  *(_DWORD *)(v4 + 424) = 115;
  memcpy(v4 + 432, v12 + 67768, 0x110u, v15, v14);
  memcpy(v4 + 704, v16 + 69880, 0x110u, v18, v17);
  memcpy(v4 + 976, v19 + 70424, 0x110u, v21, v20);
  memcpy(v4 + 1248, v22 + 70696, 0x110u, v24, v23);
  memcpy(v4 + 1520, v25 + 70968, 0x110u, v27, v26);
  memcpy(v4 + 2064, v13 + 816, 0x110u, v29, v28);
  XeCryptSha(byte_20, 16, nullptr, 0, nullptr, 0, (uint8_t *)(v4 + 2608), 0x10u);
  HvpComputeKeyVaultHmacSha(*(_QWORD *)(v2 + 25536), v4 + 2624, 16);
  sub_A458(v35);
  v34[0] = 0;
  v34[1] = 0;
  XeCryptAesKey(&v33, (const uint8_t *)v35, v30);
  XeCryptAesEcb(&v33, (const uint8_t *)v34, (uint8_t *)(v4 + 2640), 1);
  FlashBase = (unsigned int *)HvpGetFlashBase();
  CopyBy64(v4 + 2656, FlashBase, 16);
  return v_MACHINE_CHECK_filter(v36);
}


/* ========== [0x2e4c0] sub_2E4C0 (188 bytes) ========== */
_DWORD *sub_2E4C0()
{
  __int64 v0; // r2
  _DWORD *v1; // r30
  __int64 v2; // r31
  __int64 v3; // r7
  __int64 v4; // r6
  __int64 v5; // r2
  __int64 v6; // r10
  int v7; // r9

  v1 = (_DWORD *)(v0 + 90820);
  v2 = *(_QWORD *)(v0 + 91688) + 24LL;
  HvpAquireSpinLock((_DWORD *)(v0 + 90820));
  v6 = *(_QWORD *)(v5 + 90832);
  if ( *(_WORD *)(v6 + 1538) == 258 )
    v7 = 263;
  else
    v7 = 458752;
  *(_DWORD *)(v6 + 1556) = v7;
  *(_QWORD *)(*(_QWORD *)(v5 + 90832) + 1592LL) = *(_QWORD *)(v2 + 16);
  *(_QWORD *)(*(_QWORD *)(v5 + 90832) + 1600LL) = *(_QWORD *)(v2 + 24);
  memcpy(*(_QWORD *)(v5 + 90832) + 1792LL, v2 + 32, 0x80u, v4, v3);
  sub_2E220();
  return HvpReleaseSpinLock(v1);
}


/* ========== [0x2e580] sub_2E580 (636 bytes) ========== */
__int64 __fastcall sub_2E580(unsigned int a1, __int64 a2, unsigned __int64 a3)
{
  __int64 v3; // r2
  __int64 v8; // r27
  __int64 v9; // r10
  unsigned int v10; // r28
  _DWORD *v11; // r25
  __int64 v12; // r10
  __int64 v13; // r9
  __int64 v14; // r8
  __int64 v15; // r7
  __int64 v16; // r6
  __int64 v17; // r5
  __int64 v18; // r2
  __int64 v19; // r26
  __int64 v20; // r31
  __int64 v21; // r3
  __int64 v22; // r29
  __int64 v23; // r7
  __int64 v24; // r6
  __int64 v25; // r5
  __int64 v26; // r4
  _DWORD *v27; // r3
  __int64 v28; // r2
  uint8_t *v29; // r11
  __int64 i; // ctr
  __int64 v31; // r7
  __int64 v32; // r6
  __int64 v33; // r11
  uint8_t v34[20]; // [sp+50h] [-F0h] BYREF
  uint8_t v35[108]; // [sp+64h] [-DCh] BYREF
  _QWORD v36[4]; // [sp+D0h] [-70h] BYREF
  int v37; // [sp+F0h] [-50h]

  v37 = *(_DWORD *)(v3 + 0x10000);
  if ( a1 < 0x21 )
  {
    v8 = a1;
    v9 = 4LL * a1;
    v10 = HIWORD(*(_DWORD *)(v9 + v3 + 72888));
    if ( (_DWORD)a3 == (unsigned __int16)*(_DWORD *)(v9 + v3 + 72888) )
    {
      v11 = (_DWORD *)(v3 + 90820);
      HvpAquireSpinLock((_DWORD *)(v3 + 90820));
      v19 = v18 + 0x10000;
      if ( *(_BYTE *)(v8 + *(_QWORD *)(v18 + 90840)) != 0 )
      {
        v21 = a2;
        v22 = *(_QWORD *)(v18 + 90832);
        v27 = (_DWORD *)HvpPhysicalToReal(v21, a3, v17, v16, v15, v14, v13, v12);
        if ( a1 != 0 )
        {
          switch ( a1 )
          {
            case 1u:
              memcpy(v10 + v22, (__int64)v27, a3, v24, v23);
              v20 = 0;
              break;
            case 3u:
              v20 = 0;
              *(_WORD *)(v22 + 1538) = *(_WORD *)(*(_QWORD *)(v28 + 91072) + 200LL);
              break;
            case 0x1Cu:
              if ( *v27 != 0 )
              {
                v20 = 0;
                *(_DWORD *)(4 * (((*(_DWORD *)(v28 + 90824))++ & 0x1FLL) + 416) + v22) = *v27;
              }
              else
              {
                v20 = -939524084;
              }
              break;
            case 0x1Eu:
              v29 = v34;
              for ( i = 20; i != 0; --i )
              {
                *(_QWORD *)v29 = *(_QWORD *)v27;
                v27 += 2;
                v29 += 8;
              }
              v20 = 0;
              if ( (unsigned int)sub_24DD8(v36, 16) == 0 )
              {
                v36[0] = 0;
                v36[1] = 0;
              }
              XeCryptSha(v35, 140, nullptr, 0, nullptr, 0, v34, 0x14u);
              memcpy(v10 + v22, (__int64)v34, 0xA0u, v32, v31);
              break;
            default:
              v20 = -939524095;
              break;
          }
        }
        else
        {
          v20 = sub_2D990((__int64)v27, v26, v25, v24, v23);
        }
      }
      else
      {
        v20 = -939524094;
      }
      v33 = *(_QWORD *)(v19 + 25304);
      if ( *(char *)(v8 + v33) > 0 )
        --*(_BYTE *)(v8 + v33);
      HvpReleaseSpinLock(v11);
      sub_2E4C0();
      v_MACHINE_CHECK_filter(v37);
      return v20;
    }
    else
    {
      v_MACHINE_CHECK_filter(v37);
      return -939524092;
    }
  }
  else
  {
    v_MACHINE_CHECK_filter(v37);
    return -939524095;
  }
}


/* ========== [0x2e800] sub_2E800 (136 bytes) ========== */
__int64 __fastcall sub_2E800(__int64 a1)
{
  __int64 v1; // r2
  __int64 v3; // r8
  __int64 v4; // r2
  __int16 v5; // r11

  sub_2E1D0(*(_QWORD *)(*(_QWORD *)(v1 + 91072) + 48LL) & ~*(_QWORD *)a1 | *(_QWORD *)(a1 + 8));
  v5 = *(_WORD *)(a1 + 16);
  if ( v5 != 0 )
  {
    *(_WORD *)(*(_QWORD *)(v3 + 25536) + 200LL) = v5;
    if ( v5 == 0x7FFF )
    {
      sub_3830(4096, 0);
      v5 = *(_WORD *)(a1 + 18);
    }
    *(_WORD *)(*(_QWORD *)(v4 + 90832) + 1538LL) = v5;
  }
  return 0;
}


/* ========== [0x30000] sub_30000 (76 bytes) ========== */
_DWORD *sub_30000()
{
  __int64 v0; // r2
  _DWORD *v1; // r31
  __int64 v2; // r2

  v1 = (_DWORD *)(v0 + 93780);
  HvpAquireSpinLock((_DWORD *)(v0 + 93780));
  XeCryptRandom((uint8_t *)(v2 + 93848), 4);
  return HvpReleaseSpinLock(v1);
}


/* ========== [0x30050] HvpXexActivationVerifyOwnership (284 bytes) ========== */
__int64 __fastcall HvpXexActivationVerifyOwnership(int a1, __int64 a2)
{
  __int64 v2; // r2
  _DWORD *v4; // r29
  __int64 v6; // r28
  int v7; // r30
  __int64 v8; // r2
  unsigned int v9; // r11
  unsigned int v10; // r10
  unsigned int i; // r8
  unsigned int v13; // r11

  v4 = (_DWORD *)(v2 + 93780);
  v6 = 0;
  v7 = 0;
  HvpAquireSpinLock((_DWORD *)(v2 + 93780));
  v9 = 0;
  v10 = *(_DWORD *)(v8 + 93836);
  if ( v10 != 0 )
  {
    while ( *(_DWORD *)(8LL * v9 + v8 + 93840) != a1 )
    {
      if ( ++v9 >= v10 )
        goto LABEL_6;
    }
    v7 = *(_DWORD *)(8LL * v9 + v8 + 93844);
  }
LABEL_6:
  if ( (v7 & 0x10) != 0 )
    goto LABEL_9;
  if ( a2 != 0 )
  {
    for ( i = 0; i < 4; ++i )
    {
      if ( (_DWORD)v6 != 0 )
        break;
      if ( ((1 << i) & v7) != 0 )
      {
        v13 = 0;
        while ( *(_QWORD *)(8LL * i + v8 + 93800) != *(_QWORD *)(8LL * v13 + a2) )
        {
          if ( ++v13 >= 4 )
            goto LABEL_19;
        }
        v6 = 1;
      }
LABEL_19:
      ;
    }
  }
  else if ( v7 != 0 )
  {
LABEL_9:
    v6 = 1;
  }
  HvpReleaseSpinLock(v4);
  return v6;
}


/* ========== [0x30170] HvxXexActivationGetNonce (80 bytes) ========== */
__int64 HvxXexActivationGetNonce()
{
  __int64 v0; // r2
  _DWORD *v1; // r31
  __int64 v2; // r2
  __int64 v3; // r30

  v1 = (_DWORD *)(v0 + 93780);
  HvpAquireSpinLock((_DWORD *)(v0 + 93780));
  v3 = *(unsigned int *)(v2 + 93848);
  HvpReleaseSpinLock(v1);
  return v3;
}


/* ========== [0x301c0] HvxXexActivationSetLicense (416 bytes) ========== */
__int64 __fastcall HvxXexActivationSetLicense(
        __int64 a1,
        int a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  __int64 v8; // r2
  __int64 v10; // r3
  __int64 v11; // r5
  __int64 v12; // r5
  __int64 i; // r31
  __int64 v14; // r2
  __int64 v15; // r2
  __int64 v16; // r30
  _DWORD *v17; // r29
  __int64 v18; // r2
  uint8_t *v19; // r31
  __int64 v20; // r2
  _WORD v21[48]; // [sp+50h] [-1E0h] BYREF
  _WORD v22[128]; // [sp+B0h] [-180h] BYREF
  _WORD v23[20]; // [sp+1B0h] [-80h] BYREF
  _WORD v24[2]; // [sp+1D8h] [-58h] BYREF
  unsigned int v25; // [sp+1DCh] [-54h]
  _QWORD v26[2]; // [sp+1E0h] [-50h] BYREF
  _WORD v27[10]; // [sp+1F0h] [-40h] BYREF
  int v28; // [sp+204h] [-2Ch]

  v28 = *(_DWORD *)(v8 + 0x10000);
  if ( a2 == 312 )
  {
    v10 = HvpPhysicalToReal(a1, 312, a3, a4, a5, a6, a7, a8);
    memcpy(v22, v10);
    if ( v25 == 1 )
    {
      j_XeCryptShaInit(v21);
      LODWORD(v11) = 48;
      j_XeCryptShaUpdate(v21, v23, v11);
      for ( i = 0; (unsigned int)i < v25; ++i )
      {
        LODWORD(v12) = 8;
        j_XeCryptShaUpdate(v21, &v26[(unsigned int)i], v12);
      }
      LODWORD(v12) = 20;
      j_XeCryptShaFinal(v21, v27, v12);
      if ( (unsigned int)sub_38D8(v27, v22, v14 + 69880) != 0
        || *(_DWORD *)(v15 + 68584) == 32 && (unsigned int)sub_38D8(v27, v22, v15 + 68584) != 0 )
      {
        v17 = (_DWORD *)(v15 + 93780);
        HvpAquireSpinLock((_DWORD *)(v15 + 93780));
        v19 = (uint8_t *)(v18 + 93848);
        if ( (unsigned int)sub_B480(v18 + 93848, v24, 4) != 0 )
        {
          v16 = -939503615;
        }
        else
        {
          v16 = 0;
          memcpy(v20 + 93792, v23);
          XeCryptRandom(v19, 4);
        }
        HvpReleaseSpinLock(v17);
      }
      else
      {
        v16 = -939503616;
      }
    }
    else
    {
      v16 = -939503614;
    }
    v_MACHINE_CHECK_filter(v28);
    return v16;
  }
  else
  {
    v_MACHINE_CHECK_filter(v28);
    return -939524092;
  }
}


/* ========== [0x30360] HvxXexActivationVerifyOwnership (56 bytes) ========== */
__int64 __fastcall HvxXexActivationVerifyOwnership(__int64 a1)
{
  __int64 v2; // [sp+50h] [-30h] BYREF

  return HvpXexActivationVerifyOwnership(a1, &v2);
}


/* ========== [0x30398] sub_30398 (84 bytes) ========== */
__int64 sub_30398()
{
  __int64 v0; // r3
  __int64 v1; // r2

  v0 = sub_2B8();
  *(_QWORD *)(v1 + 92888) = v0;
  *(_QWORD *)(v1 + 92880) = v0;
  sub_B0D8();
  return sub_B0D8();
}


/* ========== [0x303f0] sub_303F0 (152 bytes) ========== */
__int64 __fastcall sub_303F0(__int64 a1, __int64 a2, unsigned int *a3, __int64 a4, __int64 a5)
{
  __int64 v5; // r2
  __int64 v9; // r11
  __int64 v10; // r30
  unsigned __int64 v11; // r25
  __int64 v12; // r27
  __int64 v13; // r26
  __int64 v14; // r7
  __int64 v15; // r6

  v9 = *(_QWORD *)(v5 + 92880);
  v10 = a3[1];
  v11 = a3[2];
  v12 = *(unsigned int *)(v9 + 8);
  v13 = *(unsigned int *)(v9 + 4);
  memcpy(v12 + a1 + 12288, (__int64)a3 + *a3, v10 - *a3, a4, a5);
  memcpy(v13 + a1 + 0x2000, (__int64)a3 + v10, v11, v15, v14);
  *(_DWORD *)(a2 + 12) = v13 - 1879048192 + 0x2000;
  *(_DWORD *)(a2 + 8) = v12 - 1879048192 + 12288;
  return sub_B0D8();
}


/* ========== [0x30488] HvpExpansionInstall (644 bytes) ========== */
__int64 __fastcall HvpExpansionInstall(__int64 a1, __int64 a2, __int64 a3, __int64 a4)
{
  _QWORD *v4; // r2
  unsigned int v6; // r11
  __int64 v7; // r10
  _QWORD *v8; // r23
  unsigned int *v9; // r28
  __int64 v10; // r26
  unsigned int v11; // r10
  __int64 v12; // r7
  unsigned int *v13; // r5
  unsigned int *v14; // r29
  unsigned int v15; // r25
  unsigned int v16; // r24
  unsigned int v17; // r27
  int v18; // r30
  _DWORD *v19; // r4
  __int64 v20; // r31
  __int64 v21; // r3
  __int64 v22; // r4
  __int64 v23; // r11
  __int64 v24; // r10
  unsigned int v25; // r29
  __int64 v26; // r30
  unsigned int v27; // r9
  unsigned int v28; // r10
  unsigned int v29; // r11

  if ( *(_DWORD *)(a1 + 312) >= 0x4498u )
    return -939495420;
  v6 = *(_DWORD *)(a1 + 316);
  if ( v6 != 0 && v6 <= 0x4497 )
    return -939495420;
  v7 = v4[11354];
  if ( (*(_DWORD *)(v7 + 1552) & *(_DWORD *)(a1 + 308)) != *(_DWORD *)(a1 + 308)
    || (*(_QWORD *)(v7 + 1584) & *(_QWORD *)(a1 + 328)) != *(_QWORD *)(a1 + 328) )
  {
    return -939495420;
  }
  v8 = v4 + 0x2000;
  v9 = (unsigned int *)v4[11610];
  v10 = v4[11611];
  if ( *v9 >= 4 )
    return -939495418;
  v11 = *(_DWORD *)(a1 + 340);
  v12 = *(unsigned int *)(a1 + 348);
  if ( v11 != 0 )
    v13 = (unsigned int *)(*(unsigned int *)(a1 + 336) + a1);
  else
    v13 = nullptr;
  if ( (_DWORD)v12 != 0 )
    v14 = (unsigned int *)(*(unsigned int *)(a1 + 344) + a1);
  else
    v14 = nullptr;
  if ( v13 != nullptr )
  {
    v15 = v13[1];
    v16 = v13[2];
    if ( (*v13 & 0xF) != 0 || (v15 & 0xF) != 0 || (v16 & 0xF) != 0 || *v13 >= v15 || v16 + v15 > v11 )
      return -939495422;
    if ( v9[2] + v15 > 0x5000 )
      return -939495417;
    if ( v9[1] + v16 > 0x1000 )
      return -939495416;
  }
  else
  {
    v15 = 0;
    v16 = 0;
  }
  if ( v14 == nullptr )
  {
    v17 = 0;
    goto LABEL_35;
  }
  v17 = v14[2] + v14[1];
  if ( (*v14 & 0xF) != 0 || (v17 & 0xF) != 0 || *v14 >= v17 || v17 > (unsigned int)v12 )
    return -939495422;
  if ( v9[3] + v17 > 0x8000 )
    return -939495415;
LABEL_35:
  v18 = *(_DWORD *)(a1 + 304);
  v19 = (_DWORD *)(16 * (*v9 + 256LL) + v10);
  v20 = 16 * (*v9 + 64LL) + v10;
  if ( v13 != nullptr )
  {
    v21 = v4[11611];
    *v19 = v18;
    sub_303F0(v21, (__int64)v19, v13, a4, v12);
  }
  if ( v14 != nullptr )
  {
    *(_DWORD *)v20 = v18;
    v22 = (__int64)v14;
    v23 = v14[2];
    v24 = v14[1];
    v25 = *v14;
    v26 = *(unsigned int *)(v8[3418] + 12LL) + v10 + 0x8000;
    memcpy(v26, v22, v23 + v24, a4, v12);
    *(_DWORD *)(v20 + 4) = v25;
    *(_QWORD *)(v20 + 8) = v26;
  }
  v27 = v9[1] + v16;
  v28 = v9[2] + v15;
  ++*v9;
  v29 = v9[3];
  v9[1] = v27;
  v9[2] = v28;
  v9[3] = v29 + v17;
  return 0;
}


/* ========== [0x30710] HvxExpansionInstall (608 bytes) ========== */
__int64 __fastcall HvxExpansionInstall(unsigned int a1, unsigned int a2)
{
  __int64 v2; // r2
  _DWORD *v6; // r27
  __int64 v7; // r4
  _DWORD *v8; // r3
  __int64 v9; // r2
  __int64 v10; // r31
  __int64 v11; // r30
  __int64 v12; // r30
  __int64 v13; // r29
  __int64 v14; // r2
  _WORD v15[176]; // [sp+60h] [-1B0h] BYREF
  _WORD v16[10]; // [sp+1C0h] [-50h] BYREF
  int v17; // [sp+1D4h] [-3Ch]

  v17 = *(_DWORD *)(v2 + 0x10000);
  if ( (a1 & 0x7F) != 0 )
  {
    v_MACHINE_CHECK_filter(v17);
    return -939495424;
  }
  if ( (a2 & 0x7F) != 0 || (((a1 + a2 - 1) ^ a1) & 0xFFFF0000) != 0 )
  {
    v_MACHINE_CHECK_filter(v17);
    return -939495423;
  }
  v6 = (_DWORD *)(v2 + 92824);
  HvpAquireSpinLock((_DWORD *)(v2 + 92824));
  v8 = (_DWORD *)HvpRelocatePhysicalToProtected(a1, a2, 58);
  v10 = (__int64)v8;
  if ( *v8 == 1213747282 )
  {
    v11 = v9 + 91136;
    if ( (v8[1] & 3) != 0 )
      goto LABEL_8;
    v13 = v9 + 91120;
    j_XeCryptSha(v8, 48, 0, 0, v16, 20);
  }
  else
  {
    if ( *v8 != 1213747267 || *(_DWORD *)(*(_QWORD *)(v9 + 92880) + 512LL) != 32 )
    {
      v12 = -939495422;
      goto LABEL_21;
    }
    v11 = *(_QWORD *)(v9 + 92880) + 512LL;
    if ( (v8[1] & 3) != 3 )
      goto LABEL_8;
    v13 = 32;
    j_XeCryptHmacSha(32, 16, v8, 48, 0, 0, 0, 0);
  }
  if ( (unsigned int)j_XeCryptBnQwBeSigVerify(v10 + 48, v16, v14 + 66924, v11) != 0 && *(_DWORD *)(v10 + 8) <= a2 )
  {
    if ( v13 != 0 )
    {
      sub_B388(v15, v13);
      sub_B370(v15, v10 + 304, *(unsigned int *)(v10 + 8) - 304LL, v10 + 304, v10 + 32, 0);
    }
    sub_B4D8(v10 + 304, *(unsigned int *)(v10 + 8) - 304LL, 0, 0, 0, 0, v16, 20);
    if ( (unsigned int)sub_B480(v10 + 12, v16, 20) == 0 )
    {
      v12 = HvpExpansionInstall(v10);
      goto LABEL_21;
    }
  }
LABEL_8:
  v12 = -939495421;
LABEL_21:
  LODWORD(v7) = a2 >> 7;
  HvpZeroCacheLines(v10, v7);
  HvpRelocatePhysicalToEncryptedStrict((__int64 *)v10, a2, 58);
  HvpReleaseSpinLock(v6);
  v_MACHINE_CHECK_filter(v17);
  return v12;
}


/* ========== [0x30970] HvxExpansionCall (88 bytes) ========== */
__int64 __fastcall HvxExpansionCall(int a1)
{
  __int64 v1; // r2
  unsigned int v2; // r10
  __int64 v3; // r11

  v2 = 0;
  v3 = *(_QWORD *)(v1 + 92888) + 1024LL;
  while ( *(_DWORD *)v3 != a1 )
  {
    ++v2;
    v3 += 16;
    if ( v2 >= 4 )
    {
      v3 = 0;
      break;
    }
  }
  if ( v3 != 0 )
    return ((__int64 (*)(void))(*(unsigned int *)(v3 + 4) + *(_QWORD *)(v3 + 8)))();
  else
    return -939495419;
}


/* ========== [0x309c8] sub_309C8 (48 bytes) ========== */
__int64 sub_309C8()
{
  __int64 result; // r3
  __int64 v1; // r2

  result = sub_270();
  *(_QWORD *)(v1 + 91744) = result + 64512;
  return result;
}


/* ========== [0x309f8] HvxIptvSetBoundaryKey (376 bytes) ========== */
__int64 __fastcall HvxIptvSetBoundaryKey(
        unsigned int a1,
        unsigned int a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6)
{
  __int64 v6; // r2
  __int64 v9; // r9
  __int64 v10; // r10
  __int64 v11; // r11
  int v12; // r30
  __int64 v13; // r29
  __int64 v14; // r9
  __int64 v15; // r8
  __int64 v16; // r7
  __int64 v17; // r6
  __int64 v18; // r5
  _BYTE *v19; // r3
  _BYTE *v20; // r10
  __int64 i; // ctr
  _BYTE *v22; // r11
  __int64 v23; // r2
  _BYTE *v24; // r10
  __int64 j; // ctr
  _DWORD *v26; // r31
  __int64 v27; // r6
  __int64 v28; // r5
  _WORD v29[176]; // [sp+50h] [-1B0h] BYREF
  _WORD v30[8]; // [sp+1B0h] [-50h] BYREF
  _WORD v31[8]; // [sp+1C0h] [-40h] BYREF
  int v32; // [sp+1D0h] [-30h]

  v32 = *(_DWORD *)(v6 + 0x10000);
  if ( a1 < 0x10 )
  {
    if ( a2 < 4 )
    {
      if ( (_DWORD)a4 == 16 && (_DWORD)a6 == 16 )
      {
        if ( (a3 & 0xF) != 0 || (a5 & 0xF) != 0 )
        {
          v_MACHINE_CHECK_filter(v32);
          return -939499514;
        }
        else
        {
          v9 = 16LL * a1;
          v10 = 16 * (a2 + 16LL);
          v11 = *(_QWORD *)(v6 + 91744);
          v12 = v9 + v11;
          v13 = v10 + v11;
          v19 = (_BYTE *)HvpPhysicalToReal(a3, 16, a3, a4, a5, a6, v9, v10);
          v20 = v30;
          for ( i = 16; i != 0; --i )
            *v20++ = *v19++;
          v22 = (_BYTE *)HvpPhysicalToReal(a5, 16, v18, v17, v16, v15, v14, (__int64)v20);
          v24 = v31;
          for ( j = 16; j != 0; --j )
            *v24++ = *v22++;
          v26 = (_DWORD *)(v23 + 91752);
          HvpAquireSpinLock((_DWORD *)(v23 + 91752));
          sub_B388(v29, v13);
          LODWORD(v27) = v12;
          LODWORD(v28) = 16;
          sub_B378(v29, v30, v28, v27, v31);
          HvpReleaseSpinLock(v26);
          v_MACHINE_CHECK_filter(v32);
          return 0;
        }
      }
      else
      {
        v_MACHINE_CHECK_filter(v32);
        return -939499515;
      }
    }
    else
    {
      v_MACHINE_CHECK_filter(v32);
      return -939499517;
    }
  }
  else
  {
    v_MACHINE_CHECK_filter(v32);
    return -939499518;
  }
}


/* ========== [0x30b70] HvxIptvSetSessionKey (436 bytes) ========== */
__int64 __fastcall HvxIptvSetSessionKey(
        unsigned int a1,
        int a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7)
{
  __int64 v7; // r2
  unsigned int *v10; // r31
  __int64 v11; // r3
  __int64 v12; // r7
  __int64 v13; // r4
  __int64 v14; // r2
  _DWORD *v15; // r31
  _BYTE *v16; // r8
  __int64 v17; // r2
  _BYTE *v18; // r10
  __int64 i; // ctr
  int v20; // [sp+50h] [-140h] BYREF
  _BYTE *v21; // [sp+58h] [-138h] BYREF
  _WORD v22[132]; // [sp+70h] [-120h] BYREF

  if ( a1 >= 4 )
    return -939499517;
  if ( a2 == 259 )
  {
    v10 = (unsigned int *)(*(_QWORD *)(v7 + 91032) + 2584LL);
  }
  else
  {
    if ( a2 != 260 )
      return -939499516;
    v10 = (unsigned int *)(*(_QWORD *)(v7 + 91032) + 3496LL);
  }
  if ( (unsigned int)a4 != 8LL * *v10 )
    return -939499515;
  if ( ((((_DWORD)a3 + (_DWORD)a4 - 1) ^ (unsigned int)a3) & 0xFFFFF000) != 0 )
    return -939499514;
  v11 = HvpPhysicalToReal(a3, a4, a3, a4, a5, a6, a7, (unsigned int)a4);
  j_XeCryptBnQw_SwapDwQwLeBe(v11, v22, 32);
  if ( (unsigned int)j_XeCryptBnQwNeRsaPrvCrypt(v22, v22, v10) == 0 )
    return -939499513;
  j_XeCryptBnQw_SwapDwQwLeBe(v22, v22, 32);
  sub_B498(v22, 256);
  LODWORD(v12) = 0;
  LODWORD(v13) = 256;
  if ( (unsigned int)sub_B448(v22, v13, &v21, &v20, v12) == 0 )
    return -939499512;
  if ( v20 != 16 )
    return -939499511;
  v15 = (_DWORD *)(v14 + 91752);
  HvpAquireSpinLock((_DWORD *)(v14 + 91752));
  v16 = v21;
  v18 = (_BYTE *)(16 * (a1 + 16LL) + *(_QWORD *)(v17 + 91744));
  for ( i = 16; i != 0; --i )
    *v18++ = *v16++;
  HvpReleaseSpinLock(v15);
  return 0;
}


/* ========== [0x30d28] HvxIptvVerifyOmac1Signature (384 bytes) ========== */
__int64 __fastcall HvxIptvVerifyOmac1Signature(
        int a1,
        unsigned int a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        int a6,
        __int64 a7)
{
  __int64 v7; // r2
  __int64 v13; // r10
  __int64 v14; // r31
  _DWORD *v15; // r30
  __int64 v16; // r10
  __int64 v17; // r9
  __int64 v18; // r8
  __int64 v19; // r7
  __int64 v20; // r6
  __int64 v21; // r5
  __int64 v22; // r3
  __int64 v23; // r4
  __int64 v24; // r10
  __int64 v25; // r9
  __int64 v26; // r8
  __int64 v27; // r7
  __int64 v28; // r6
  __int64 v29; // r5
  __int64 v30; // r31
  __int64 v31; // r3
  _WORD v32[8]; // [sp+50h] [-50h] BYREF
  int v33; // [sp+60h] [-40h]

  v33 = *(_DWORD *)(v7 + 0x10000);
  if ( a1 != 0 )
  {
    if ( a1 != 1 )
    {
      v_MACHINE_CHECK_filter(v33);
      return -939499519;
    }
    if ( a2 >= 4 )
    {
      v_MACHINE_CHECK_filter(v33);
      return -939499517;
    }
    v13 = 16 * (a2 + 16LL);
  }
  else
  {
    if ( a2 >= 0x10 )
    {
      v_MACHINE_CHECK_filter(v33);
      return -939499518;
    }
    v13 = 16LL * a2;
  }
  v14 = v13 + *(_QWORD *)(v7 + 91744);
  if ( (unsigned int)a4 <= 0x1000 && a6 == 16 )
  {
    if ( (a3 & 0xFFF) != 0 || (a5 & 0xF) != 0 )
    {
      v_MACHINE_CHECK_filter(v33);
      return -939499514;
    }
    else
    {
      v15 = (_DWORD *)(v7 + 91752);
      HvpAquireSpinLock((_DWORD *)(v7 + 91752));
      LODWORD(v22) = HvpPhysicalToReal(a3, a4, v21, v20, v19, v18, v17, v16);
      LODWORD(v23) = a4;
      if ( (unsigned int)J_XeCryptMemDiff(v22, v23, v14, 16, v32, a7) == 0
        || (v31 = HvpPhysicalToReal(a5, 16, v29, v28, v27, v26, v25, v24), (unsigned int)sub_B480(v31, v32, 16) != 0) )
      {
        v30 = -939499510;
      }
      else
      {
        v30 = 0;
      }
      HvpReleaseSpinLock(v15);
      v_MACHINE_CHECK_filter(v33);
      return v30;
    }
  }
  else
  {
    v_MACHINE_CHECK_filter(v33);
    return -939499515;
  }
}


/* ========== [0x30ea8] HvxIptvGetAesCtrTransform (412 bytes) ========== */
__int64 __fastcall HvxIptvGetAesCtrTransform(
        unsigned int a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  __int64 v8; // r2
  __int64 v12; // r28
  __int64 v13; // r8
  __int64 v14; // r7
  __int64 v15; // r6
  __int64 v16; // r5
  __int64 v17; // r2
  _BYTE *v18; // r10
  unsigned __int8 *v19; // r11
  __int64 i; // ctr
  __int64 v21; // r9
  __int64 v22; // r3
  __int64 v23; // r10
  __int64 v24; // r30
  __int64 v25; // r29
  __int64 v26; // r2
  _DWORD *v27; // r31
  __int64 v28; // r31
  unsigned int j; // r8
  int v30; // r11
  _BYTE *v31; // r11
  _BYTE *v32; // r10
  __int64 k; // ctr
  _WORD v34[176]; // [sp+50h] [-1A0h] BYREF
  _BYTE v35[16]; // [sp+1B0h] [-40h] BYREF
  int v36; // [sp+1C0h] [-30h]

  v36 = *(_DWORD *)(v8 + 0x10000);
  if ( a1 >= 0x10 )
  {
    v_MACHINE_CHECK_filter(v36);
    return -939499518;
  }
  if ( (a2 & 0xFFF) != 0 )
    goto LABEL_4;
  if ( (_DWORD)a3 != 4096 )
    goto LABEL_6;
  if ( (a4 & 0xF) != 0 )
  {
LABEL_4:
    v_MACHINE_CHECK_filter(v36);
    return -939499514;
  }
  if ( (_DWORD)a5 == 16 )
  {
    v12 = HvpPhysicalToReal(a4, 16, a3, a4, a5, a6, a7, a8);
    v18 = v35;
    v19 = (unsigned __int8 *)v12;
    for ( i = 16; i != 0; --i )
    {
      v21 = *v19++;
      *v18++ = v21;
    }
    v22 = a2;
    v23 = 16LL * a1;
    v24 = v23 + *(_QWORD *)(v17 + 91744);
    v25 = HvpPhysicalToReal(v22, 4096, v16, v15, v14, v13, v21, v23);
    v27 = (_DWORD *)(v26 + 91752);
    HvpAquireSpinLock((_DWORD *)(v26 + 91752));
    sub_B388(v34, v24);
    HvpReleaseSpinLock(v27);
    v28 = 256;
    do
    {
      sub_B380(v34, v35, v25, 1);
      for ( j = 15; j != -1; --j )
      {
        v30 = (unsigned __int8)(v35[j] + 1);
        v35[j] = v30;
        if ( v30 != 0 )
          break;
      }
      --v28;
      v25 += 16;
    }
    while ( (_DWORD)v28 != 0 );
    v31 = v35;
    v32 = (_BYTE *)v12;
    for ( k = 16; k != 0; --k )
      *v32++ = *v31++;
    v_MACHINE_CHECK_filter(v36);
    return 0;
  }
  else
  {
LABEL_6:
    v_MACHINE_CHECK_filter(v36);
    return -939499515;
  }
}


/* ========== [0x31048] HvxIptvGetSessionKeyHash (188 bytes) ========== */
__int64 __fastcall HvxIptvGetSessionKeyHash(unsigned int a1, __int64 a2, int a3)
{
  __int64 v3; // r2
  _DWORD *v7; // r31
  __int64 v8; // r10
  __int64 v9; // r9
  __int64 v10; // r8
  __int64 v11; // r7
  __int64 v12; // r6
  __int64 v13; // r5
  __int64 v14; // r3
  __int64 v15; // r2

  if ( a1 >= 4 )
    return -939499517;
  if ( (a2 & 0xFFF) != 0 )
    return -939499514;
  if ( a3 != 20 )
    return -939499515;
  v7 = (_DWORD *)(v3 + 91752);
  HvpAquireSpinLock((_DWORD *)(v3 + 91752));
  v14 = HvpPhysicalToReal(a2, 20, v13, v12, v11, v10, v9, v8);
  sub_B4D8(16 * (a1 + 16LL) + *(_QWORD *)(v15 + 91744), 16, 0, 0, 0, 0, v14, 20);
  HvpReleaseSpinLock(v7);
  return 0;
}


/* ========== [0x31108] sub_31108 (100 bytes) ========== */
unsigned __int64 __fastcall sub_31108(char a1, unsigned int a2, int a3)
{
  unsigned __int64 result; // r3

  result = (0x800080u >> a1) | (unsigned __int64)(unsigned int)(16777472 << a1);
  if ( a2 >= 2 )
  {
    if ( a2 == 3 )
      result = ~result;
    if ( a3 == 4 || a3 == 5 )
      return ~result;
  }
  else
  {
    if ( a2 == 1 )
      result = ~result;
    if ( (a3 & 2) != 0 )
      return ~result;
  }
  return result;
}


/* ========== [0x31170] sub_31170 (184 bytes) ========== */
__int64 __fastcall sub_31170(
        __int64 a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  __int64 v8; // r31
  __int64 result; // r3
  unsigned int i; // r4
  int v11; // r3
  __int64 v12; // r5
  _DWORD *v13; // r9
  __int64 v14; // r9
  _DWORD *v15; // r8
  __int64 v16; // r8

  v8 = 2;
  result = 69933056;
  do
  {
    a7 = HvpPhysicalToReal(result, 0x8000, a3, a4, a5, a6, a7, a8);
    LOBYTE(a5) = 0;
    a6 = a7 + 128;
    do
    {
      for ( i = 0; i < 4; ++i )
      {
        __dcbz128(0, a7);
        __dcbz128(0, a6);
        LODWORD(a3) = 0;
        do
        {
          v11 = sub_31108(a5, i, a3);
          a3 = v12 + 1;
          *v13 = v11;
          v14 = (__int64)(v13 + 1);
          *v15 = v11;
          v16 = (__int64)(v15 + 1);
        }
        while ( (unsigned int)a3 < 8 );
        __dcbf(0, v14);
        __dcbf(0, v16);
        a7 = v14 + 224;
        a6 = v16 + 224;
      }
      ++a5;
    }
    while ( (unsigned int)a5 < 8 );
    --v8;
    result = 338368512;
  }
  while ( (_DWORD)v8 != 0 );
  return result;
}


/* ========== [0x31228] sub_31228 (192 bytes) ========== */
unsigned __int64 sub_31228()
{
  __int64 SocMMIORegs; // r31

  SocMMIORegs = HvpGetSocMMIORegs(3825205248LL);
  *(_DWORD *)(SocMMIORegs + 652) = 0x2000000;
  __sync();
  Sleep(0x14u);
  *(_DWORD *)(SocMMIORegs + 652) = 0;
  __sync();
  Sleep(0xC8u);
  *(_DWORD *)(SocMMIORegs + 648) = 0;
  __sync();
  Sleep(0x14u);
  *(_DWORD *)(SocMMIORegs + 648) = 0x1000000;
  __sync();
  Sleep(0x14u);
  *(_DWORD *)(SocMMIORegs + 648) = 83886080;
  __sync();
  Sleep(0x14u);
  *(_DWORD *)(SocMMIORegs + 648) = 0x1000000;
  __sync();
  Sleep(0x14u);
  *(_DWORD *)(SocMMIORegs + 648) = 0;
  __sync();
  return Sleep(0x14u);
}


/* ========== [0x312e8] sub_312E8 (396 bytes) ========== */
__int64 __fastcall sub_312E8(
        unsigned __int8 a1,
        __int64 a2,
        __int64 a3,
        __int64 a4,
        __int64 a5,
        __int64 a6,
        __int64 a7,
        __int64 a8)
{
  _DWORD *v10; // r27
  __int64 v11; // r29
  __int64 v12; // r28
  __int64 v13; // r31
  __int64 v14; // r30
  __int64 result; // r3
  __int64 v16; // r24
  unsigned int v17; // r4
  unsigned int v18; // r4
  unsigned int v19; // r3
  _QWORD *v20; // r9
  __int64 v21; // r8
  __int64 v22; // r5
  __int64 v23; // r9
  unsigned __int64 v24; // r11
  _DWORD *v25; // r11
  __int64 v26; // r10
  __int64 v27; // r7
  __int64 v28; // r6
  _QWORD v29[2]; // [sp+50h] [-70h] BYREF
  _QWORD v30[12]; // [sp+60h] [-60h] BYREF

  v10 = (_DWORD *)a3;
  v11 = 0;
  v12 = 0;
  v13 = 0;
  v14 = 0;
  result = 69933056;
  v16 = 2;
  do
  {
    HvpPhysicalToReal(result, 0x8000, a3, a4, a5, a6, a7, a8);
    LOBYTE(a4) = 0;
    do
    {
      v17 = 0;
      do
      {
        sub_31108(a4, v17, 0);
        LODWORD(a3) = 2;
        do
        {
          v19 = sub_31108(a4, v18, a3);
          a5 = v20[16];
          a3 = v22 + 2;
          v14 |= a5 ^ v21;
          v24 = ((unsigned __int64)v19 << 32) | v19;
          v13 |= *v20 ^ v21;
          a8 = *v20 ^ v24;
          v23 = (__int64)(v20 + 1);
          v12 |= a5 ^ v24;
          v11 |= a8;
          a6 = v24;
        }
        while ( (unsigned int)a3 < 8 );
        __dcbf(0, v23);
        __dcbf(0, v23 + 128);
        v17 = v18 + 1;
        a7 = v23 + 224;
      }
      while ( v17 < 4 );
      ++a4;
    }
    while ( (unsigned int)a4 < 8 );
    --v16;
    result = 338368512;
  }
  while ( (_DWORD)v16 != 0 );
  v29[1] = v12;
  v29[0] = v11;
  v25 = v10;
  v30[1] = v14;
  v26 = a2 - (_QWORD)v10;
  v30[0] = v13;
  v27 = 0;
  v28 = 16;
  do
  {
    if ( *((_BYTE *)v29 + v27) == 0 )
    {
      ++*(_DWORD *)((char *)v25 + v26);
      *v25 += a1;
    }
    if ( *((_BYTE *)v30 + v27) == 0 )
    {
      ++*(_DWORD *)((char *)v25 + v26);
      *v25 = *v25 + a1 - 80;
    }
    --v28;
    ++v27;
    ++v25;
  }
  while ( (_DWORD)v28 != 0 );
  return result;
}


/* ========== [0x31478] sub_31478 (232 bytes) ========== */
unsigned __int64 sub_31478()
{
  _DWORD *SocMMIORegs; // r31
  _DWORD *v1; // r3

  SocMMIORegs = (_DWORD *)HvpGetSocMMIORegs(3825205248LL);
  v1 = (_DWORD *)HvpGetSocMMIORegs(3775004676LL);
  SocMMIORegs[2061] = 0;
  SocMMIORegs[2125] = 0;
  SocMMIORegs[2059] = 15460;
  SocMMIORegs[2123] = 15460;
  SocMMIORegs[2059] = 75786;
  SocMMIORegs[2123] = 75786;
  SocMMIORegs[2059] = 9263;
  SocMMIORegs[2123] = 9263;
  SocMMIORegs[2059] = 9263;
  SocMMIORegs[2123] = 9263;
  SocMMIORegs[2059] = 15360;
  SocMMIORegs[2123] = 15360;
  __sync();
  __isync();
  SocMMIORegs[2048] = 0x2000000;
  SocMMIORegs[2112] = 0x2000000;
  SocMMIORegs[2048] = 85327872;
  SocMMIORegs[2112] = 85327872;
  SocMMIORegs[2048] = 84279296;
  SocMMIORegs[2112] = 84279296;
  __sync();
  *v1 = 0;
  __sync();
  __isync();
  return Sleep(0xC8u);
}


/* ========== [0x31560] sub_31560 (212 bytes) ========== */
unsigned __int64 sub_31560()
{
  _DWORD *SocMMIORegs; // r31

  SocMMIORegs = (_DWORD *)HvpGetSocMMIORegs(3825205248LL);
  *(_DWORD *)HvpGetSocMMIORegs(3775004676LL) = 402653184;
  __sync();
  __isync();
  while ( (SocMMIORegs[2128] | SocMMIORegs[2064]) != 0x2000000 )
    ;
  SocMMIORegs[2048] = 0;
  SocMMIORegs[2112] = 0;
  SocMMIORegs[2061] = 0;
  SocMMIORegs[2125] = 0;
  SocMMIORegs[2059] = 75786;
  SocMMIORegs[2123] = 75786;
  SocMMIORegs[2059] = 9263;
  SocMMIORegs[2123] = 9263;
  SocMMIORegs[2059] = 9263;
  SocMMIORegs[2123] = 9263;
  SocMMIORegs[2059] = 1024;
  SocMMIORegs[2123] = 1024;
  SocMMIORegs[2048] = 0x2000000;
  SocMMIORegs[2112] = 0x2000000;
  __sync();
  return Sleep(0x64u);
}


/* ========== [0x31638] sub_31638 (316 bytes) ========== */
unsigned __int64 __fastcall sub_31638(int a1, int a2)
{
  _DWORD *SocMMIORegs; // r31

  SocMMIORegs = (_DWORD *)HvpGetSocMMIORegs(3825205248LL);
  sub_31560();
  SocMMIORegs[161] = a1;
  SocMMIORegs[161] = a1 | 0xC00;
  Sleep(0x1388u);
  SocMMIORegs[161] = a1 & 0xFFFFF3DF;
  SocMMIORegs[2049] = a2 | 0x1000000;
  SocMMIORegs[2113] = a2 | 0x1000000;
  SocMMIORegs[2051] = -2004838368;
  SocMMIORegs[2115] = -2004838368;
  SocMMIORegs[2055] = 2077763585;
  SocMMIORegs[2119] = 2077763585;
  SocMMIORegs[2056] = -619806205;
  SocMMIORegs[2120] = -619806205;
  SocMMIORegs[2057] = 1267832588;
  SocMMIORegs[2121] = 1267832588;
  SocMMIORegs[4] = 302456838;
  SocMMIORegs[5] = 226500614;
  SocMMIORegs[2061] = 0;
  SocMMIORegs[2125] = 0;
  SocMMIORegs[2059] = 75788;
  SocMMIORegs[2123] = 75788;
  SocMMIORegs[2059] = 9263;
  SocMMIORegs[2123] = 9263;
  SocMMIORegs[2059] = 226500614;
  SocMMIORegs[2123] = 226500614;
  SocMMIORegs[2059] = 302456838;
  SocMMIORegs[2123] = 302456838;
  SocMMIORegs[2048] = 0x2000000;
  SocMMIORegs[2112] = 0x2000000;
  __sync();
  Sleep(0x7D0u);
  sub_31228();
  return sub_31478();
}


/* ========== [0x31778] sub_31778 (96 bytes) ========== */
unsigned __int64 __fastcall sub_31778(int a1, int a2, int a3, int a4)
{
  _DWORD *SocMMIORegs; // r31

  SocMMIORegs = (_DWORD *)HvpGetSocMMIORegs(3825205248LL);
  sub_31560();
  SocMMIORegs[2080] = a1;
  SocMMIORegs[2081] = a2;
  SocMMIORegs[2144] = a3;
  SocMMIORegs[2145] = a4;
  __sync();
  Sleep(0x64u);
  sub_31228();
  return sub_31478();
}


/* ========== [0x317d8] sub_317D8 (204 bytes) ========== */
unsigned __int64 __fastcall sub_317D8(int a1)
{
  _DWORD *SocMMIORegs; // r31
  int v3; // r26
  int v4; // r28
  int v5; // r27
  int v6; // r30
  unsigned __int64 result; // r3

  v3 = a1 & 0xF3FF00;
  v4 = a1 + 100663296;
  SocMMIORegs = (_DWORD *)HvpGetSocMMIORegs(3825205248LL);
  do
  {
    SocMMIORegs[2058] = v4;
    SocMMIORegs[2122] = v4;
    __sync();
    Sleep(0x7D0u);
    SocMMIORegs[2058] = a1;
    SocMMIORegs[2122] = a1;
    __sync();
    Sleep(0x7D0u);
    SocMMIORegs[2058] = 0x20000;
    Sleep(0x64u);
    v5 = SocMMIORegs[2090];
    SocMMIORegs[2058] = 0;
    Sleep(0x64u);
    v6 = SocMMIORegs[2090];
    SocMMIORegs[2058] = 0x10000;
    result = Sleep(0x64u);
  }
  while ( v3 != (((((v5 & 0xF000000 | ((unsigned int)~v6 >> 4) & 0xF00000 | SocMMIORegs[2090] & 0xF00FFFFF) >> 1)
                 & 0xFF80000
                 | (unsigned __int64)(v6 & 0xF007FFFF)) >> 7)
               & 0x3FF000
               | SocMMIORegs[2090] & 0xF000000) >> 4 );
  return result;
}


/* ========== [0x318a8] sub_318A8 (956 bytes) ========== */
__int64 sub_318A8()
{
  __int64 v0; // r2
  unsigned int v1; // r22
  unsigned int v2; // r23
  _DWORD *SocMMIORegs; // r30
  unsigned int *FlashBase; // r31
  __int64 v5; // r9
  __int64 v6; // r8
  __int64 v7; // r7
  __int64 v8; // r6
  __int64 v9; // r5
  __int64 v10; // r10
  unsigned int v11; // r24
  unsigned __int8 *v12; // r3
  __int64 v13; // r2
  __int64 v14; // r11
  __int64 v15; // r10
  int v16; // r11
  unsigned int v17; // r26
  __int64 v18; // r2
  __int64 v19; // r28
  unsigned int v20; // r29
  int *v21; // r25
  __int64 v22; // r27
  _QWORD *v23; // r11
  __int64 i; // ctr
  _QWORD *v25; // r11
  __int64 j; // ctr
  unsigned int k; // r31
  unsigned __int64 v28; // r3
  __int64 v29; // r10
  __int64 v30; // r9
  __int64 v31; // r8
  __int64 v32; // r7
  __int64 v33; // r6
  __int64 v34; // r5
  __int64 v35; // r4
  __int64 v36; // r10
  __int64 v37; // r9
  __int64 v38; // r8
  __int64 v39; // r7
  __int64 v40; // r6
  unsigned int v41; // r3
  __int64 v42; // r10
  __int64 v43; // r11
  __int64 v44; // r8
  unsigned int v45; // r7
  unsigned int v46; // r9
  char v47; // r6
  unsigned int v48; // r7
  int v49; // r7
  __int64 v50; // r31
  __int64 v51; // r10
  int v52; // r7
  int v53; // r6
  int v54; // r5
  int v56; // [sp+50h] [-600h] BYREF
  int v57; // [sp+54h] [-5FCh]
  int v58; // [sp+58h] [-5F8h]
  int v59; // [sp+5Ch] [-5F4h]
  _WORD v60[32]; // [sp+90h] [-5C0h] BYREF
  _WORD v61[96]; // [sp+D0h] [-580h] BYREF
  __int16 v62; // [sp+190h] [-4C0h] BYREF
  unsigned int v63; // [sp+198h] [-4B8h]
  __int16 v64; // [sp+210h] [-440h] BYREF
  unsigned __int16 v65; // [sp+212h] [-43Eh]
  int v66; // [sp+5F0h] [-60h]

  v1 = 0;
  v2 = 0;
  v66 = *(_DWORD *)(v0 + 0x10000);
  SocMMIORegs = (_DWORD *)HvpGetSocMMIORegs(3825205248LL);
  if ( SocMMIORegs[2087] == 168430090 )
  {
    FlashBase = (unsigned int *)HvpGetFlashBase();
    CopyBy64((__int64)&v62, FlashBase, 16);
    if ( (unsigned int)sub_3890(v63, 0x10u) != 0 )
    {
      CopyBy64((__int64)&v64, (unsigned int *)((char *)FlashBase + v63), 2);
      v10 = v65;
      if ( v65 < 0x780u || v65 > 0x793u )
      {
        if ( v65 >= 0x794u && v65 <= 0x79Cu )
          v10 = 1888;
      }
      else
      {
        v10 = 1903;
      }
      v11 = (unsigned __int16)v10;
      if ( (unsigned __int16)v10 > 0x770u || (unsigned __int16)v10 < 0x760u )
      {
        v16 = ((unsigned __int16)v10 << 16) + 44510;
LABEL_33:
        SocMMIORegs[6] = v16;
        return v_MACHINE_CHECK_filter(v66);
      }
      v12 = (unsigned __int8 *)HvpPhysicalToReal(69933056, 0x2000, v9, v8, v7, v6, v5, v10);
      sub_84A8(v12, 0x2000);
      sub_84A8(nullptr, 512);
      sub_84A8((unsigned __int8 *)0x200010000LL, 512);
      sub_84A8((unsigned __int8 *)0x400020000LL, 512);
      sub_84A8((unsigned __int8 *)0x600030000LL, 512);
      sub_84A8((unsigned __int8 *)__mfspr(0x131u), 64);
      if ( v11 > 0x76E )
      {
        v14 = 0;
        v15 = 16;
        v56 = SocMMIORegs[2080];
        v57 = SocMMIORegs[2081];
        v58 = SocMMIORegs[2144];
        v59 = SocMMIORegs[2145];
        do
        {
          --v15;
          *((_BYTE *)&v56 + v14) += *(_BYTE *)(v14 + v13 + 72672);
          ++v14;
        }
        while ( (_DWORD)v15 != 0 );
        sub_31778(v56, v57, v58, v59);
        v16 = 52014558;
        goto LABEL_33;
      }
      sub_31638(420479008, 36835844);
      v17 = 0;
      v19 = v18 + 72704;
      v20 = 0;
      v21 = (int *)(v18 + 72704);
      v22 = v18 + 72688;
      do
      {
        v23 = v60;
        for ( i = 8; i != 0; --i )
          *v23++ = 0;
        v25 = v61;
        for ( j = 8; j != 0; --j )
          *v25++ = 0;
        sub_317D8(*v21);
        for ( k = 0; k < 0x50505050; k += 16843009 )
        {
          v28 = sub_31778(k, k, k, k);
          sub_31170(v28, v35, v34, v33, v32, v31, v30, v29);
          sub_312E8(k, (__int64)v60, (__int64)v61, v40, v39, v38, v37, v36);
        }
        v41 = 255;
        v42 = 0;
        v43 = 0;
        v44 = 16;
        do
        {
          v45 = *(_DWORD *)&v60[v43];
          if ( v45 < v41 )
            v41 = (unsigned __int8)*(_DWORD *)&v60[v43];
          if ( v45 == 0 )
            *(_DWORD *)&v60[v43] = 1;
          v46 = *(_DWORD *)&v60[v43];
          v47 = *(_BYTE *)(v42 + v22);
          __twllei(v46, 0);
          --v44;
          v48 = *(_DWORD *)&v61[v43];
          *((_BYTE *)&v61[2 * v20 + 32] + v42) = v46;
          v49 = v48 / v46;
          *(_DWORD *)&v61[v43 + 64] = v49;
          v43 += 2;
          *((_BYTE *)&v56 + 4 * v20 + v42++) = v47 + v49;
        }
        while ( (_DWORD)v44 != 0 );
        if ( v41 > v1 )
        {
          v2 = v17;
          v1 = v41;
        }
        v20 += 4;
        ++v17;
        ++v21;
      }
      while ( v20 < 0x10 );
      v50 = 4LL * v2;
      sub_317D8(*(_DWORD *)(v50 + v19));
      v51 = 8LL * v2;
      v52 = *(_DWORD *)&v61[v51 + 34];
      v53 = *(_DWORD *)&v61[v51 + 36];
      v54 = *(_DWORD *)&v61[v51 + 38];
      SocMMIORegs[1412] = *(_DWORD *)&v61[v51 + 32];
      SocMMIORegs[1413] = v52;
      SocMMIORegs[1414] = v53;
      SocMMIORegs[1415] = v54;
      sub_31778(
        *(int *)((char *)&v56 + v51 * 2),
        *(int *)((char *)&v56 + v51 * 2 + 4),
        *(int *)((char *)&v56 + v51 * 2 + 8),
        *(int *)((char *)&v56 + v51 * 2 + 12));
      SocMMIORegs[6] = (v11 << 16) + 44510;
      SocMMIORegs[8] = *(_DWORD *)(v50 + v19);
    }
  }
  return v_MACHINE_CHECK_filter(v66);
}


/* ========== [0x31c68] HvxShadowBoot (4 bytes) ========== */
// attributes: thunk
void __fastcall __noreturn HvxShadowBoot(__int64 a1)
{
  v_MACHINE_CHECK_0(a1);
}


/* ========== [0x31c70] HvxCreateUserMode (524 bytes) ========== */
__int64 __fastcall HvxCreateUserMode(__int64 idx, __int64 phys)
{
  __int64 v2; // r2
  _DWORD *v4; // r26
  __int64 v6; // r29
  __int64 v7; // r3
  __int64 v8; // r27
  __int64 v9; // r28
  __int64 v10; // r10
  __int64 v11; // r9
  __int64 v12; // r8
  __int64 v13; // r7
  __int64 v14; // r6
  __int64 v15; // r5
  __int64 v16; // r30
  __int64 v17; // r3
  int v18; // r31
  int v19; // r5
  __int64 v20; // r7
  __int64 v21; // r31
  __int64 v22; // r10
  char v23; // r9
  __int64 *v24; // r6
  _BYTE *v25; // r8
  __int64 v26; // r11
  __int64 v27; // r3
  __int64 v28; // r29
  _QWORD *v29; // r11
  _QWORD *v30; // r10
  __int64 i; // ctr
  unsigned int v32; // r11
  unsigned int v34; // [sp+5Ch] [-154h] BYREF
  _WORD v35[168]; // [sp+60h] [-150h] BYREF

  v4 = (_DWORD *)(v2 + 90816);
  v6 = 0;
  HvpAquireSpinLock((_DWORD *)(v2 + 90816));
  if ( (_DWORD)idx != 0 )
  {
    if ( (_DWORD)idx != 1 )
      goto LABEL_22;
    v7 = 3;
  }
  else
  {
    v7 = 2;
  }
  if ( (unsigned int)sub_B100(v7) != 0 && (phys & 0xFFF) == 0 && ((unsigned int)phys & 0xFFF00000) < 0x20000000 )
  {
    v8 = sub_5D8(idx);
    v9 = sub_5EC(idx);
    if ( *(_QWORD *)v9 == 0 )
    {
      v16 = HvpPhysicalToReal(phys, 4096, v15, v14, v13, v12, v11, v10);
      v17 = sub_5EC((_cntlzw(idx) & 0x20) != 0);
      v18 = 0;
      if ( *(_QWORD *)v17 != 0 )
        v18 = *(_DWORD *)(v17 + 12);
      do
      {
        do
          XeCryptRandom((uint8_t *)&v34, 4);
        while ( v34 == 0 );
      }
      while ( v34 == v18 || v34 == -1 );
      v19 = 0;
      v20 = 0;
      v21 = 16LL * v34;
      v22 = 0;
      v23 = 0;
      v24 = (__int64 *)v35;
      v25 = (_BYTE *)(v16 + 2112);
      do
      {
        if ( (*v25 & 2) == 0 || (v26 = 1, (*v25 & 1) == 0) )
          v26 = 0;
        v27 = (16 * v26) & 0x10 | (((32 * (v23 & 0xF | (unsigned __int64)v21)) | *v25 & 6) << 7);
        v28 = __builtin_rotateleft64(v20 + 16, 3) & 0x7FFFFFFFFLL;
        *v24 = v27;
        ++v20;
        ++v23;
        *(_QWORD *)((char *)v35 + (v28 & 0x7FFFFFFF8LL)) = v27 | 0x400;
        ++v25;
        ++v24;
        v19 |= ((2 * (unsigned int)((v27 | 0x400uLL) >> 8)) & 2 | ((v27 & 0x10) != 0)) << v22;
        v22 += 2;
      }
      while ( (unsigned int)v22 < 0x20 );
      v29 = v35;
      v30 = (_QWORD *)v8;
      for ( i = 32; i != 0; --i )
        *v30++ = *v29++;
      v6 = 1;
      *(_DWORD *)(v9 + 8) = v19;
      v32 = v34;
      *(_QWORD *)v9 = v16;
      *(_DWORD *)(v9 + 12) = v32;
    }
  }
LABEL_22:
  HvpReleaseSpinLock(v4);
  return v6;
}


/* ========== [0x31e80] HvxDeleteUserMode (168 bytes) ========== */
void __fastcall HvxDeleteUserMode(__int64 idx)
{
  __int64 v1; // r2
  _DWORD *v3; // r29
  __int64 v4; // r30
  __int64 v5; // r3
  __int64 v6; // r11
  __int64 *v7; // r9
  __int64 v8; // r10
  __int64 v9; // r8

  v3 = (_DWORD *)(v1 + 90816);
  HvpAquireSpinLock((_DWORD *)(v1 + 90816));
  if ( (unsigned int)idx <= 1 )
  {
    v4 = sub_5D8(idx);
    v5 = sub_5EC(idx);
    if ( *(_QWORD *)v5 != 0 )
    {
      v6 = 0;
      v7 = (__int64 *)v4;
      v8 = 32;
      *(_QWORD *)v5 = 0;
      *(_DWORD *)(v5 + 12) = 0;
      *(_DWORD *)(v5 + 8) = 0;
      do
      {
        v9 = (v6 << 12) & 0xF000;
        --v8;
        ++v6;
        *v7++ = v9 | 0xFFFFFFFF0200LL;
      }
      while ( (_DWORD)v8 != 0 );
    }
  }
  HvpReleaseSpinLock(v3);
}


/* ========== [0x31f28] HvxFlushUserModeTb (144 bytes) ========== */
void __fastcall HvxFlushUserModeTb(__int64 idx)
{
  __int64 v1; // r2
  _DWORD *v3; // r27
  __int64 v4; // r3
  __int64 v5; // r11
  unsigned int v6; // r29
  __int64 v7; // r31
  unsigned int *v8; // r30

  v3 = (_DWORD *)(v1 + 90816);
  HvpAquireSpinLock((_DWORD *)(v1 + 90816));
  if ( (unsigned int)idx <= 1 )
  {
    v4 = sub_5EC(idx);
    v5 = *(_QWORD *)v4;
    v6 = *(_DWORD *)(v4 + 8);
    if ( *(_QWORD *)v4 != 0 )
    {
      v7 = *(unsigned int *)(v5 + 2176);
      v8 = (unsigned int *)(v5 + 2180);
      if ( (unsigned int)(v7 - 1) <= 0x19E )
      {
        do
        {
          HvpFlushSingleTb(*v8, v6);
          --v7;
          ++v8;
        }
        while ( (_DWORD)v7 != 0 );
      }
    }
  }
  HvpReleaseSpinLock(v3);
}

