#include "al_axi_vdma_hw.h"
AlAxiVdma_HwConfigStruct AlAxiVdma_HwConfig[AL_AXI_VDMA_NUM_INSTANCE] = {
{
.DeviceId               = 0,
.BaseAddress            = 0x80200000,
.MaxFrameStoreNum       = 3,
.HasMm2s                = 1,
.Mm2s_IntrId            = 116,
.Mm2sMmDataWidth        = 64,
.Mm2sBurstSize          = 16,
.Mm2sStreamDataWidth    = 24,
.Mm2sLineBufferDepth    = 4096,
.Mm2sFsyncOptions       = 0,
.Mm2sGenlockMode        = 0,
.Mm2sHasDRE             = 1,
.HasS2Mm                = 0,
.S2Mm_IntrId            = 0xFFFFFFFF,
.S2MmMmDataWidth        = 128,
.S2MmBurstSize          = 8,
.S2MmStreamDataWidth    = 32,
.S2MmLineBufferDepth    = 512,
.S2MmFsyncOptions       = 0,
.S2MmGenlockMode        = 3,
.S2MmHasDRE             = 0,
.HasVFlip               = 1,
.AddrWidth              = 32,
}
};
