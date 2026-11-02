#include "al_core.h"
#include "al_spi_nor.h"
#include "al_spi_controller.h"

extern const AlSpiNorInfo SpiNorInfoList[];
extern const AL_U32 SpiNorInfoListSize;
extern const AlSpiNorInfo SpiNorDefaultInfo;
extern AlSpiController SpiControllerList[];
extern const AL_U32 SpiControllerListSize;

/**
 * [7:0]: Opcode
 * [10:8]: BusWidth
 * [13:11]: AddrBytes
 * [16:14]: CmdBusWidth
 * [19:17]: AddrBusWidth
 * [22:20]: DummyBusWidth
 * [25:23]: DataBusWidth
 * [31:26]: DummyCycles
 */
AL_U32 AlSpiRdCmdProtoList[] =
{
#define PROTO_RD_OPCODE_SHIFT                 0
#define PROTO_RD_OPCODE_MASK                  0xFF
#define PROTO_RD_BUS_WIDTH_SHIFT              8
#define PROTO_RD_BUS_WIDTH_MASK               (0x7 << PROTO_RD_BUS_WIDTH_SHIFT)
#define PROTO_RD_ADDR_BYTES_SHIFT             11
#define PROTO_RD_ADDR_BYTES_MASK              (0x7 << PROTO_RD_ADDR_BYTES_SHIFT)
#define PROTO_RD_CMD_BUS_WIDTH_SHIFT          14
#define PROTO_RD_CMD_BUS_WIDTH_MASK           (0x7 << PROTO_RD_CMD_BUS_WIDTH_SHIFT)
#define PROTO_RD_ADDR_BUS_WIDTH_SHIFT         17
#define PROTO_RD_ADDR_BUS_WIDTH_MASK          (0x7 <<  PROTO_RD_ADDR_BUS_WIDTH_SHIFT)
#define PROTO_RD_DUMMY_CYCLES_SHIFT           20
#define PROTO_RD_DUMMY_CYCLES_MASK            (0x1F << PROTO_RD_DUMMY_CYCLES_SHIFT)

#define PROTO_RD_INFO(OpCode, BusWidth, AddrBytes, CmdWidth, AddrWidth, DummyCycles)  \
            ((OpCode << PROTO_RD_OPCODE_SHIFT)               |                        \
            (BusWidth << PROTO_RD_BUS_WIDTH_SHIFT)           |                        \
            (AddrBytes << PROTO_RD_ADDR_BYTES_SHIFT)         |                        \
            (CmdWidth << PROTO_RD_CMD_BUS_WIDTH_SHIFT)       |                        \
            (AddrWidth << PROTO_RD_ADDR_BUS_WIDTH_SHIFT)     |                        \
            (DummyCycles << PROTO_RD_DUMMY_CYCLES_SHIFT))


    PROTO_RD_INFO(NOR_OP_READ, 1, 3, 1, 1, 0),
    PROTO_RD_INFO(NOR_OP_READ_FAST, 1, 3, 1, 1, 8),
    PROTO_RD_INFO(NOR_OP_READ_1_1_2, 2, 3, 1, 1, 8),
    PROTO_RD_INFO(NOR_OP_READ_1_2_2, 2, 3, 1, 2, 4),
    PROTO_RD_INFO(NOR_OP_READ_1_1_4, 4, 3, 1, 1, 8),
    PROTO_RD_INFO(NOR_OP_READ_1_4_4, 4, 3, 1, 4, 6),

    PROTO_RD_INFO(NOR_OP_READ_4B, 1, 4, 1, 1, 0),
    PROTO_RD_INFO(NOR_OP_READ_FAST_4B, 1, 4, 1, 1, 8),
    PROTO_RD_INFO(NOR_OP_READ_1_1_2_4B, 2, 4, 1, 1, 8),
    PROTO_RD_INFO(NOR_OP_READ_1_2_2_4B, 2, 4, 1, 2, 4),
    PROTO_RD_INFO(NOR_OP_READ_1_1_4_4B, 4, 4, 1, 1, 8),
    PROTO_RD_INFO(NOR_OP_READ_1_4_4_4B, 4, 4, 1, 4, 6),
};

AL_U32 AlSpiWrCmdProtoList[] =
{
#define PROTO_WR_OPCODE_SHIFT                 0
#define PROTO_WR_OPCODE_MASK                  0xFF
#define PROTO_WR_BUS_WIDTH_SHIFT              8
#define PROTO_WR_BUS_WIDTH_MASK               (0x7 << PROTO_WR_BUS_WIDTH_SHIFT)
#define PROTO_WR_ADDR_BYTES_SHIFT             11
#define PROTO_WR_ADDR_BYTES_MASK              (0x7 << PROTO_WR_ADDR_BYTES_SHIFT)
#define PROTO_WR_CMD_BUS_WIDTH_SHIFT          14
#define PROTO_WR_CMD_BUS_WIDTH_MASK           (0x7 << PROTO_WR_CMD_BUS_WIDTH_SHIFT)
#define PROTO_WR_ADDR_BUS_WIDTH_SHIFT         17
#define PROTO_WR_ADDR_BUS_WIDTH_MASK          (0x7 <<  PROTO_WR_ADDR_BUS_WIDTH_SHIFT)
#define PROTO_WR_DUMMY_CYCLES_SHIFT           20
#define PROTO_WR_DUMMY_CYCLES_MASK            (0x1F << PROTO_WR_DUMMY_CYCLES_SHIFT)

#define PROTO_WR_INFO(OpCode, BusWidth, AddrBytes, CmdWidth, AddrWidth, DummyCycles)  \
            ((OpCode << PROTO_WR_OPCODE_SHIFT)               |                        \
            (BusWidth << PROTO_WR_BUS_WIDTH_SHIFT)           |                        \
            (AddrBytes << PROTO_WR_ADDR_BYTES_SHIFT)         |                        \
            (CmdWidth << PROTO_WR_CMD_BUS_WIDTH_SHIFT)       |                        \
            (AddrWidth << PROTO_WR_ADDR_BUS_WIDTH_SHIFT)     |                        \
            (DummyCycles << PROTO_WR_DUMMY_CYCLES_SHIFT))

    PROTO_WR_INFO(NOR_OP_PP, 1, 3, 1, 1, 0),
    PROTO_WR_INFO(NOR_OP_PP_1_1_4, 4, 3, 1, 1, 0),
    PROTO_WR_INFO(NOR_OP_PP_1_4_4, 4, 3, 1, 4, 0),

    PROTO_WR_INFO(NOR_OP_PP_4B, 1, 4, 1, 1, 0),
    PROTO_WR_INFO(NOR_OP_PP_1_1_4_4B, 4, 4, 1, 1, 0),
    PROTO_WR_INFO(NOR_OP_PP_1_4_4_4B, 4, 4, 1, 4, 0),
};

AL_U32 AlSpiEraseCmdProtoList[] =
{
#define PROTO_ERASE_OPCODE_SHIFT              0
#define PROTO_ERASE_OPCODE_MASK               0xFF
#define PROTO_ERASE_ADDR_BYTES_SHIFT          8
#define PROTO_ERASE_ADDR_BYTES_MASK           (0x7 << PROTO_ERASE_ADDR_BYTES_SHIFT)
#define PROTO_ERASE_SECTOR_4K_SHIFT           12
#define PROTO_ERASE_SECTOR_4K_MASK            (0x1 << PROTO_ERASE_SECTOR_4K_SHIFT)
#define PROTO_ERASE_SECTOR_32K_SHIFT          15
#define PROTO_ERASE_SECTOR_32K_MASK           (0x1 << PROTO_ERASE_SECTOR_32K_SHIFT)
#define PROTO_ERASE_SECTOR_64K_SHIFT          16
#define PROTO_ERASE_SECTOR_64K_MASK           (0x1 << PROTO_ERASE_SECTOR_64K_SHIFT)
#define PROTO_ERASE_SECTOR_128K_SHIFT         17
#define PROTO_ERASE_SECTOR_128K_MASK          (0x1 << PROTO_ERASE_SECTOR_128K_SHIFT)
#define PROTO_ERASE_SECTOR_256K_SHIFT         18
#define PROTO_ERASE_SECTOR_256K_MASK          (0x1 << PROTO_ERASE_SECTOR_256K_SHIFT)

#define PROTO_EARSE_INFO(OpCode, AddrBytes, EraseSize)                                \
            ((OpCode << PROTO_ERASE_OPCODE_SHIFT)            |                        \
            (AddrBytes << PROTO_ERASE_ADDR_BYTES_SHIFT)      |                        \
            (EraseSize & 0xFFFFF000))

    PROTO_EARSE_INFO(NOR_OP_BE_4K, 3, 4 * 1024),
    PROTO_EARSE_INFO(NOR_OP_BE_32K, 3, 32 * 1024),
    PROTO_EARSE_INFO(NOR_OP_SE, 3, 64 * 1024),
    PROTO_EARSE_INFO(NOR_OP_BE_4K_4B, 4, 4 * 1024),
    PROTO_EARSE_INFO(NOR_OP_BE_32K_4B, 4, 32 * 1024),
    PROTO_EARSE_INFO(NOR_OP_SE_4B, 4, 64 * 1024),
    PROTO_EARSE_INFO(NOR_OP_SE_4B, 4, 256 * 1024),
};

AL_S32 AlSpiNor_SetReadProto(AlSpiNor *SpiNor)
{
    AL_U32 Index;

    for (Index = 0; Index < sizeof(AlSpiRdCmdProtoList) / sizeof(AlSpiRdCmdProtoList[0]); Index++) {
        if (SpiNor->CurBusWidth ==
            ((AlSpiRdCmdProtoList[Index] & PROTO_RD_BUS_WIDTH_MASK) >> PROTO_RD_BUS_WIDTH_SHIFT) &&
            SpiNor->AddrBytes ==
            ((AlSpiRdCmdProtoList[Index] & PROTO_RD_ADDR_BYTES_MASK) >> PROTO_RD_ADDR_BYTES_SHIFT)) {

            SpiNor->RdProto.OpCode = (AlSpiRdCmdProtoList[Index] & PROTO_RD_OPCODE_MASK);
            SpiNor->RdProto.DummyCycles =
                (AlSpiRdCmdProtoList[Index] & PROTO_RD_DUMMY_CYCLES_MASK) >> PROTO_RD_DUMMY_CYCLES_SHIFT;
            SpiNor->RdProto.CmdWidth =
                (AlSpiRdCmdProtoList[Index] & PROTO_RD_CMD_BUS_WIDTH_MASK) >> PROTO_RD_CMD_BUS_WIDTH_SHIFT;
            SpiNor->RdProto.AddrWidth =
                (AlSpiRdCmdProtoList[Index] & PROTO_RD_ADDR_BUS_WIDTH_MASK) >> PROTO_RD_ADDR_BUS_WIDTH_SHIFT;

            return AL_OK;
        }
    }

    return AL_SPI_NOR_EVENTS_TO_ERRS(AL_SPI_NOR_SET_READ_PROTO_ERROR);
}

AL_S32 AlSpiNor_SetWriteProto(AlSpiNor *SpiNor)
{
    AL_U32 Index;

    for (Index = 0; Index < sizeof(AlSpiWrCmdProtoList) / sizeof(AlSpiWrCmdProtoList[0]); Index++) {
        if (SpiNor->CurBusWidth ==
            ((AlSpiWrCmdProtoList[Index] & PROTO_WR_BUS_WIDTH_MASK) >> PROTO_WR_BUS_WIDTH_SHIFT) &&
            SpiNor->AddrBytes ==
            ((AlSpiWrCmdProtoList[Index] & PROTO_WR_ADDR_BYTES_MASK) >> PROTO_WR_ADDR_BYTES_SHIFT)) {

            SpiNor->WrProto.OpCode = (AlSpiWrCmdProtoList[Index] & PROTO_WR_OPCODE_MASK);
            SpiNor->WrProto.DummyCycles =
                (AlSpiWrCmdProtoList[Index] & PROTO_WR_DUMMY_CYCLES_MASK) >> PROTO_WR_DUMMY_CYCLES_SHIFT;
            SpiNor->WrProto.CmdWidth =
                (AlSpiWrCmdProtoList[Index] & PROTO_WR_CMD_BUS_WIDTH_MASK) >> PROTO_WR_CMD_BUS_WIDTH_SHIFT;
            SpiNor->WrProto.AddrWidth =
                (AlSpiWrCmdProtoList[Index] & PROTO_WR_ADDR_BUS_WIDTH_MASK) >> PROTO_WR_ADDR_BUS_WIDTH_SHIFT;

            return AL_OK;
        }
    }

    return AL_SPI_NOR_EVENTS_TO_ERRS(AL_SPI_NOR_SET_WRITE_PROTO_ERROR);
}

AL_S32 AlSpiNor_SetEraseProto(AlSpiNor *SpiNor)
{
    AL_U32 Index;

    for (Index = 0; Index < sizeof(AlSpiEraseCmdProtoList) / sizeof(AlSpiEraseCmdProtoList[0]); Index++) {
        if (SpiNor->AddrBytes ==
            ((AlSpiEraseCmdProtoList[Index] & PROTO_ERASE_ADDR_BYTES_MASK) >> PROTO_ERASE_ADDR_BYTES_SHIFT) &&
            SpiNor->EraseSize ==
            (AlSpiEraseCmdProtoList[Index] & 0xFFFFF000)) {

            SpiNor->EraseProto.OpCode = (AlSpiEraseCmdProtoList[Index] & PROTO_ERASE_OPCODE_MASK);
            SpiNor->EraseProto.CmdWidth = 1;
            SpiNor->EraseProto.AddrWidth = 1;
            SpiNor->EraseProto.DummyCycles = 0;

            return AL_OK;
        }
    }

    return AL_SPI_NOR_EVENTS_TO_ERRS(AL_SPI_NOR_SET_ERASE_PROTO_ERROR);
}

AL_S32 AlSpiNor_SetProto(AlSpiNor *SpiNor)
{
    AL_S32 Status;

    Status = AlSpiNor_SetReadProto(SpiNor);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_SetWriteProto(SpiNor);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_SetEraseProto(SpiNor);
    if (Status != AL_OK) {
        return Status;
    }

    return AL_OK;
}

AL_S32 AlSpiNor_ReadId(AlSpiNor *SpiNor, AL_U8 *Id)
{
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(NOR_OP_RDID, 1),
                                  SPI_MEM_OP_NO_ADDR,
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_DATA_IN(3, Id, 1));

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_ReadSr(AlSpiNor *SpiNor, AL_U8 *Sr)
{
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(NOR_OP_RDSR, 1),
                                  SPI_MEM_OP_NO_ADDR,
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_DATA_IN(1, Sr, 1));

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_ReadCr(AlSpiNor *SpiNor, AL_U8 *Cr)
{
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(NOR_OP_RDCR, 1),
                                  SPI_MEM_OP_NO_ADDR,
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_DATA_IN(1, Cr, 1));

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_ReadSr2(AlSpiNor *SpiNor, AL_U8 *Sr2)
{
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(NOR_OP_RDSR2, 1),
                                  SPI_MEM_OP_NO_ADDR,
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_DATA_IN(1, Sr2, 1));

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_WriteSr(AlSpiNor *SpiNor, AL_U8 *Sr, AL_U32 Len)
{
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(NOR_OP_WRSR, 1),
                                  SPI_MEM_OP_NO_ADDR,
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_DATA_OUT(Len, Sr, 1));

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_WriteCr(AlSpiNor *SpiNor, AL_U8 Cr)
{
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(NOR_OP_WRSR2_GIGADEVICE, 1),
                                  SPI_MEM_OP_NO_ADDR,
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_DATA_OUT(1, &Cr, 1));

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_WriteSr2(AlSpiNor *SpiNor, AL_U8 Sr2)
{
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(NOR_OP_WRSR2, 1),
                                  SPI_MEM_OP_NO_ADDR,
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_DATA_OUT(1, &Sr2, 1));

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_WriteEnable(AlSpiNor *SpiNor)
{
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(NOR_OP_WREN, 1),
                                  SPI_MEM_OP_NO_ADDR,
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_NO_DATA);

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_WaitReady(AlSpiNor *SpiNor, AL_U32 TimeStepMs)
{
    AL_U8 Sr;
    AL_S32 Status;

    do {
        Status = AlSpiNor_ReadSr(SpiNor, &Sr);
        if (Status != AL_OK) {
            return Status;
        }

        if (TimeStepMs) {
            AlOsal_Sleep(TimeStepMs);
        }

    } while (Sr & SR_WIP);

    return AL_OK;
}

AL_S32 AlSpiNor_SetWrap64_MODE1(AlSpiNor *SpiNor)
{
    AL_U8 Val = 0x60;
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(NOR_OP_SBL_MODE1, 1),
                                  SPI_MEM_OP_ADDR(3, 0, 4),
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_DATA_OUT(1, &Val, 4));

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_SetWrap64_MODE2(AlSpiNor *SpiNor)
{
    AL_U32 Status;
    AL_U8 Val = 0xFE;
    AL_U8 Val_Read = 0;
    AlSpiMemOp MemOp_Wr = SPI_MEM_OP(SPI_MEM_OP_CMD(0x81, 1),
                                     SPI_MEM_OP_ADDR(3, 0x07, 1),
                                     SPI_MEM_OP_NO_DUMMY,
                                     SPI_MEM_OP_DATA_OUT(1, &Val, 1));
    AlSpiMemOp MemOp_Rd = SPI_MEM_OP(SPI_MEM_OP_CMD(0x85, 1),
                                     SPI_MEM_OP_ADDR(3, 0x07, 1),
                                     SPI_MEM_OP_DUMMY(8, 1),
                                     SPI_MEM_OP_DATA_IN(1, &Val_Read, 1));

    Status = AlSpiNor_WriteEnable(SpiNor);
    if (Status != AL_OK) {
        return Status;
    }

    Status = SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp_Wr);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_WaitReady(SpiNor, 0);
    if (Status != AL_OK) {
        return Status;
    }

    Status = SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp_Rd);
    if (Status != AL_OK) {
        return Status;
    }

    if (Val != Val_Read) {
        return AL_SPI_NOR_EVENTS_TO_ERRS(AL_SPI_NOR_SET_WRAP64_MODE2_REG_VAL_ERROR);
    }

    return AL_OK;
}

AL_S32 AlSpiNor_SetWrap64_MODE3(AlSpiNor *SpiNor)
{
    AL_U8 Val = 0xFA;
    AL_U8 Val_Read = 0;
    AL_S32 Status;
    AlSpiMemOp MemOp_Wr = SPI_MEM_OP(SPI_MEM_OP_CMD(0x81, 1),
                                     SPI_MEM_OP_NO_ADDR,
                                     SPI_MEM_OP_NO_DUMMY,
                                     SPI_MEM_OP_DATA_OUT(1, &Val, 1));
    AlSpiMemOp MemOp_Rd = SPI_MEM_OP(SPI_MEM_OP_CMD(0x85, 1),
                                     SPI_MEM_OP_NO_ADDR,
                                     SPI_MEM_OP_NO_DUMMY,
                                     SPI_MEM_OP_DATA_IN(1, &Val_Read, 1));

    Status = AlSpiNor_WriteEnable(SpiNor);
    if (Status != AL_OK) {
        return Status;
    }

    Status = SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp_Wr);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_WaitReady(SpiNor, 0);
    if (Status != AL_OK) {
        return Status;
    }

    Status = SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp_Rd);
    if (Status != AL_OK) {
        return Status;
    }

    if (Val != Val_Read) {
        return AL_SPI_NOR_EVENTS_TO_ERRS(AL_SPI_NOR_SET_WRAP64_MODE3_REG_VAL_ERROR);
    }

    return AL_OK;
}

AL_S32 AlSpiNor_SetWrap64_MODE4(AlSpiNor *SpiNor)
{
    AL_U8 Val = 0x03;
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(0xC0, 1),
                                  SPI_MEM_OP_NO_ADDR,
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_DATA_OUT(1, &Val, 1));

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_SetWrap64(AlSpiNor *SpiNor)
{
    switch (SpiNor->FlashInfo->Flags & AL_SPI_NOR_FLAG_WRAP_MODE_MASK)
    {
    case AL_SPI_NOR_FLAG_WRAP_MODE0:
        break;

    case AL_SPI_NOR_FLAG_WRAP_MODE1:
        return AlSpiNor_SetWrap64_MODE1(SpiNor);

    case AL_SPI_NOR_FLAG_WRAP_MODE2:
        return AlSpiNor_SetWrap64_MODE2(SpiNor);

    case AL_SPI_NOR_FLAG_WRAP_MODE3:
        return AlSpiNor_SetWrap64_MODE3(SpiNor);

    case AL_SPI_NOR_FLAG_WRAP_MODE4:
        return AlSpiNor_SetWrap64_MODE4(SpiNor);

    default:
        break;
    }

    return AL_SPI_NOR_EVENTS_TO_ERRS(AL_SPI_NOR_SET_WRAP64_MODE_ERROR);
}

AL_S32 AlSpiNor_QuadEnableMode_1_4_5(AlSpiNor *SpiNor)
{
    AL_U8 Sr_Cr[2] = { 0 };
    AL_U8 Sr_Cr_Read[2];
    AL_S32 Status;

    if (SpiNor->FlashInfo->Flags & AL_SPI_NOR_FLAG_QE_MODE5) {
        Status = AlSpiNor_ReadCr(SpiNor, &Sr_Cr[1]);
        if (Status != AL_OK) {
            return Status;
        }

        if (Sr_Cr[1] & SR2_QUAD_EN_BIT1) {
            return AL_OK;
        }
    }

    Status = AlSpiNor_ReadSr(SpiNor, &Sr_Cr[0]);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_WriteEnable(SpiNor);
    if (Status != AL_OK) {
        return Status;
    }

    Sr_Cr[1] |= SR2_QUAD_EN_BIT1;
    Status = AlSpiNor_WriteSr(SpiNor, Sr_Cr, 2);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_WaitReady(SpiNor, 0);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_ReadSr(SpiNor, &Sr_Cr_Read[0]);
    if (Status != AL_OK) {
        return Status;
    }

    if (Sr_Cr[0] != Sr_Cr_Read[0]) {
        return AL_SPI_NOR_EVENTS_TO_ERRS(AL_SPI_NOR_SET_QUAD_MODE_1_4_5_REG_VAL_ERROR);
    }

    if (SpiNor->FlashInfo->Flags & AL_SPI_NOR_FLAG_QE_MODE5) {
        Status = AlSpiNor_ReadCr(SpiNor, &Sr_Cr_Read[1]);
        if (Status != AL_OK) {
            return Status;
        }

        if (Sr_Cr[1] != Sr_Cr_Read[1]) {
            return AL_SPI_NOR_EVENTS_TO_ERRS(AL_SPI_NOR_SET_QUAD_MODE_1_4_5_REG_VAL_ERROR);
        }
    }

    return AL_OK;
}

AL_S32 AlSpiNor_QuadEnableMode_2(AlSpiNor *SpiNor)
{
    AL_U8 Sr;
    AL_U8 Sr_Read;
    AL_S32 Status;

    Status = AlSpiNor_ReadSr(SpiNor, &Sr);
    if (Status != AL_OK) {
        return Status;
    }

    if (Sr & SR1_QUAD_EN_BIT6) {
        return AL_OK;
    }

    Status = AlSpiNor_WriteEnable(SpiNor);
    if (Status != AL_OK) {
        return Status;
    }

    Sr |= SR1_QUAD_EN_BIT6;
    Status = AlSpiNor_WriteSr(SpiNor, &Sr, 1);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_WaitReady(SpiNor, 0);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_ReadSr(SpiNor, &Sr_Read);
    if (Status != AL_OK) {
        return Status;
    }

    if (Sr != Sr_Read) {
        return AL_SPI_NOR_EVENTS_TO_ERRS(AL_SPI_NOR_SET_QUAD_MODE_2_REG_VAL_ERROR);
    }

    return AL_OK;
}

AL_S32 AlSpiNor_QuadEnableMode_3(AlSpiNor *SpiNor)
{
    AL_U8 Sr2;
    AL_U8 Sr2_Read;
    AL_S32 Status;

    Status = AlSpiNor_ReadSr2(SpiNor, &Sr2);
    if (Status != AL_OK) {
        return Status;
    }

    if (Sr2 & SR2_QUAD_EN_BIT7) {
        return AL_OK;
    }

    Status = AlSpiNor_WriteEnable(SpiNor);
    if (Status != AL_OK) {
        return Status;
    }

    Sr2 |= SR2_QUAD_EN_BIT7;
    Status = AlSpiNor_WriteSr2(SpiNor, Sr2);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_WaitReady(SpiNor, 0);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_ReadSr2(SpiNor, &Sr2_Read);
    if (Status != AL_OK) {
        return Status;
    }

    if (Sr2 != Sr2_Read) {
        return AL_SPI_NOR_EVENTS_TO_ERRS(AL_SPI_NOR_SET_QUAD_MODE_3_REG_VAL_ERROR);
    }

    return AL_OK;
}

AL_S32 AlSpiNor_QuadEnableMode_6(AlSpiNor *SpiNor)
{
    AL_U8 Cr;
    AL_U8 Cr_Read;
    AL_S32 Status;

    Status = AlSpiNor_ReadCr(SpiNor, &Cr);
    if (Status != AL_OK) {
        return Status;
    }

    if (Cr & SR2_QUAD_EN_BIT1) {
        return AL_OK;
    }

    Status = AlSpiNor_WriteEnable(SpiNor);
    if (Status != AL_OK) {
        return Status;
    }

    Cr |= SR2_QUAD_EN_BIT1;
    Status = AlSpiNor_WriteCr(SpiNor, Cr);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_WaitReady(SpiNor, 0);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_ReadCr(SpiNor, &Cr_Read);
    if (Status != AL_OK) {
        return Status;
    }

    if (Cr != Cr_Read) {
        return AL_SPI_NOR_EVENTS_TO_ERRS(AL_SPI_NOR_SET_QUAD_MODE_6_REG_VAL_ERROR);
    }

    return AL_OK;
}

AL_S32 AlSpiNor_SetQuadEnable(AlSpiNor *SpiNor)
{
    switch (SpiNor->FlashInfo->Flags & AL_SPI_NOR_FLAG_QE_MODE_MASK)
    {
    case AL_SPI_NOR_FLAG_QE_MODE0:
        return AL_OK;

    case AL_SPI_NOR_FLAG_QE_MODE1:
    case AL_SPI_NOR_FLAG_QE_MODE4:
    case AL_SPI_NOR_FLAG_QE_MODE5:
        return AlSpiNor_QuadEnableMode_1_4_5(SpiNor);

    case AL_SPI_NOR_FLAG_QE_MODE2:
        return AlSpiNor_QuadEnableMode_2(SpiNor);

    case AL_SPI_NOR_FLAG_QE_MODE3:
        return AlSpiNor_QuadEnableMode_3(SpiNor);

    case AL_SPI_NOR_FLAG_QE_MODE6:
        return AlSpiNor_QuadEnableMode_6(SpiNor);

    default:
        break;
    }

    return AL_SPI_NOR_EVENTS_TO_ERRS(AL_SPI_NOR_SET_QUAD_MODE_ERROR);
}

AL_S32 AlSpiNor_WriteDisable(AlSpiNor *SpiNor)
{
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(NOR_OP_WRDI, 1),
                                  SPI_MEM_OP_NO_ADDR,
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_NO_DATA);

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_ReadData(AlSpiNor *SpiNor, AL_U32 Addr, AL_U32 Len, AL_U8 *Data)
{
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(SpiNor->RdProto.OpCode, SpiNor->RdProto.CmdWidth),
                                  SPI_MEM_OP_ADDR(SpiNor->AddrBytes, Addr, SpiNor->RdProto.AddrWidth),
                                  SPI_MEM_OP_DUMMY(SpiNor->RdProto.DummyCycles, SpiNor->RdProto.AddrWidth),
                                  SPI_MEM_OP_DATA_IN(Len, Data, SpiNor->CurBusWidth));

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_WriteData(AlSpiNor *SpiNor, AL_U32 Addr, AL_U32 Len, AL_U8 *Data)
{
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(SpiNor->WrProto.OpCode, SpiNor->WrProto.CmdWidth),
                                  SPI_MEM_OP_ADDR(SpiNor->AddrBytes, Addr, SpiNor->WrProto.AddrWidth),
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_DATA_OUT(Len, Data, SpiNor->CurBusWidth));

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_EraseSector(AlSpiNor *SpiNor, AL_U32 Addr)
{
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(SpiNor->EraseProto.OpCode, SpiNor->EraseProto.CmdWidth),
                                  SPI_MEM_OP_ADDR(SpiNor->AddrBytes, Addr, SpiNor->EraseProto.AddrWidth),
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_NO_DATA);

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_EraseChip(AlSpiNor *SpiNor)
{
    AlSpiMemOp MemOp = SPI_MEM_OP(SPI_MEM_OP_CMD(NOR_OP_CHIP_ERASE, 1),
                                  SPI_MEM_OP_NO_ADDR,
                                  SPI_MEM_OP_NO_DUMMY,
                                  SPI_MEM_OP_NO_DATA);

    return SpiNor->Controller->ExecOp(SpiNor->Controller, &MemOp);
}

AL_S32 AlSpiNor_SetOperationWidth(AlSpiNor *SpiNor)
{
    AL_S32 Status = AL_OK;

    if ((SpiNor->FlashInfo->Flags & AL_SPI_NOR_FLAG_QUAD) &&
         SpiNor->Controller->Flags & AL_SPI_CONTROLLER_QUAD) {
        SpiNor->CurBusWidth = 4;
    } else if ((SpiNor->FlashInfo->Flags & AL_SPI_NOR_FLAG_DUAL) &&
         SpiNor->Controller->Flags & AL_SPI_CONTROLLER_DUAL) {
        SpiNor->CurBusWidth = 2;
    } else {
        SpiNor->CurBusWidth = 1;
    }

    if (SpiNor->UserParams.IsForceStandardMode) {
        SpiNor->CurBusWidth = 1;
    }

    if (SpiNor->CurBusWidth == 4) {
        Status = AlSpiNor_SetQuadEnable(SpiNor);
        if (Status != AL_OK) {
            AL_LOG(AL_LOG_LEVEL_INFO, "Set QUAD mode failed\r\n");
            return Status;
        }
    }

    AL_LOG(AL_LOG_LEVEL_INFO, "Flash is in x%d mode\r\n", SpiNor->CurBusWidth);
    return AL_OK;
}

AL_S32 AlSpiNor_Scan(AlSpiNor *SpiNor)
{
    AL_S32 Status;
    AL_U32 Index;
    AL_U8 Id[3];
    AL_U32 FlashListSize;
    const AlSpiNorInfo *FlashList;

    Status = AlSpiNor_ReadId(SpiNor, &Id[0]);
    if (Status != AL_OK) {
        return Status;
    }

    FlashList = SpiNorInfoList;
    FlashListSize = SpiNorInfoListSize;
    if (SpiNor->UserParams.UserFlashInfoList != AL_NULL &&
        SpiNor->UserParams.UserFlashInfoListSize != 0) {
        FlashList = SpiNor->UserParams.UserFlashInfoList;
        FlashListSize = SpiNor->UserParams.UserFlashInfoListSize;
        AL_LOG(AL_LOG_LEVEL_INFO, "Use flash list from user\r\n");
    }

    SpiNor->FlashInfo = AL_NULL;
    while (1) {
        for (Index = 0; Index < FlashListSize; Index++) {
            if (Id[0] == FlashList[Index].Id[0] &&
                Id[1] == FlashList[Index].Id[1] &&
                Id[2] == FlashList[Index].Id[2]) {
                SpiNor->FlashInfo = &FlashList[Index];
            }
        }

        if (SpiNor->FlashInfo == AL_NULL &&
            FlashList == SpiNor->UserParams.UserFlashInfoList) {
            FlashList = SpiNorInfoList;
            FlashListSize = SpiNorInfoListSize;
            AL_LOG(AL_LOG_LEVEL_WARNING, "No matching flash info, try normal process\r\n");
        } else {
            break;
        }
    }

    if (SpiNor->FlashInfo == AL_NULL) {
        SpiNor->FlashInfo = &SpiNorDefaultInfo;
        AL_LOG(AL_LOG_LEVEL_WARNING,
            "Unknow flash id: %02x%02x%02x, try default(x1 and 3byte) operations\r\n",
            Id[0], Id[1], Id[2]);
        return AL_OK;
    }

    return AL_OK;
}

AL_S32 AlSpiNor_Read(AlSpiNor *SpiNor, AL_U32 Addr, AL_U32 Len, AL_U8 *Data)
{
    AL_S32 Status;
    AL_U32 CurAddr;
    AL_U32 CurLen;
    AL_U32 WriteCount;

    if (!SpiNor || (Addr + Len > SpiNor->DeviceSize) || !Data) {
        return AL_SPI_NOR_ERR_ILLEGAL_PARAM;
    }

#ifdef QSPI_USE_AHB_DMA
    AL_U32 ReadStep = 4032;
#else
    AL_U32 ReadStep = 64 * 1024 - 4;
#endif

    WriteCount = 0;
    while (WriteCount < Len) {

        CurAddr = Addr + WriteCount;
        CurLen = (Len - WriteCount) > ReadStep ? ReadStep : (Len - WriteCount);
        Status = AlSpiNor_ReadData(SpiNor, CurAddr, CurLen, Data + WriteCount);
        if (Status != AL_OK) {
            return Status;
        }

        WriteCount += CurLen;
    }

    return AL_OK;
}

AL_S32 AlSpiNor_Write(AlSpiNor *SpiNor, AL_U32 Addr, AL_U32 Len, AL_U8 *Data)
{
    AL_S32 Status;
    AL_U32 CurAddr;
    AL_U32 CurLen;
    AL_U32 WriteCount = 0;
    AL_U32 PageSize = SpiNor->FlashInfo->PageSize;

    if (!SpiNor || (Addr + Len > SpiNor->DeviceSize) || !Data) {
        return AL_SPI_NOR_ERR_ILLEGAL_PARAM;
    }

    while (WriteCount < Len) {
        CurAddr = Addr + WriteCount;
        CurLen = PageSize - (CurAddr & (PageSize - 1));
        CurLen = (CurLen > (Len - WriteCount)) ? (Len - WriteCount) : CurLen;

        Status = AlSpiNor_WriteEnable(SpiNor);
        if (Status != AL_OK) {
            return Status;
        }

        Status = AlSpiNor_WriteData(SpiNor, CurAddr, CurLen, Data + WriteCount);
        if (Status != AL_OK) {
            return Status;
        }

        Status = AlSpiNor_WaitReady(SpiNor, 0);
        if (Status != AL_OK) {
            return Status;
        }

        WriteCount += CurLen;
    }

    return AL_OK;
}

AL_S32 AlSpiNor_Erase(AlSpiNor *SpiNor, AL_U32 Addr, AL_U32 Len)
{
    AL_S32 Status;
    AL_U32 EraseCount = 0;

    if (!SpiNor || (Addr + Len > SpiNor->DeviceSize) ||
        (Addr % SpiNor->EraseSize) || (Len % SpiNor->EraseSize)) {
        return AL_SPI_NOR_ERR_ILLEGAL_PARAM;
    }

    if (Len == SpiNor->DeviceSize) {
        Status = AlSpiNor_WriteEnable(SpiNor);
        if (Status != AL_OK) {
            return Status;
        }

        Status = AlSpiNor_EraseChip(SpiNor);
        if (Status != AL_OK) {
            return Status;
        }

        Status = AlSpiNor_WaitReady(SpiNor, 10);
        if (Status != AL_OK) {
            return Status;
        }

    } else {
        while (EraseCount < Len)
        {
            Status = AlSpiNor_WriteEnable(SpiNor);
            if (Status != AL_OK) {
                return Status;
            }

            Status = AlSpiNor_EraseSector(SpiNor, Addr + EraseCount);
            if (Status != AL_OK) {
                return Status;
            }

            Status = AlSpiNor_WaitReady(SpiNor, 1);
            if (Status != AL_OK) {
                return Status;
            }

            EraseCount += SpiNor->EraseSize;
        }
    }

    Status = AlSpiNor_WriteDisable(SpiNor);
    if (Status != AL_OK) {
        return Status;
    }

    return AL_OK;
}

/**
 * @brief Initialize the selected controller and the flash device.
 * @param SpiNor: The spi-nor device.
 * @param ControllerType: The type of the controller. 0: PS QSPI, 1: PS SPI, 2: PL QSPI.
 * @param DevId: Refers to the specific controller of a certain type.
 *
 * @note Get the ControllerType and DevId information in al_spi_controller.c
 */
AL_S32 AlSpiNor_Init(AlSpiNor *SpiNor, AL_U32 ControllerType, AL_U32 DevId)
{
    AL_U32 Index = 0;
    AL_S32 Status;

    if (SpiNor == AL_NULL) {
        return AL_SPI_NOR_ERR_ILLEGAL_PARAM;
    }

    for (Index = 0; Index < SpiControllerListSize; Index++) {
        if (ControllerType == SpiControllerList[Index].ControllerType &&
            DevId == SpiControllerList[Index].DevId) {
            SpiNor->Controller = &SpiControllerList[Index];
            break;
        }
    }

    if (SpiNor->Controller == AL_NULL) {
        return AL_SPI_NOR_ERR_NOT_SUPPORT;
    }

    Status = SpiNor->Controller->Init(SpiNor->Controller);
    if (Status != AL_OK) {
        return Status;
    }

    Status = AlSpiNor_Scan(SpiNor);
    if (Status != AL_OK) {
        return Status;
    }

    AL_LOG(AL_LOG_LEVEL_INFO, "Flash model: %s\r\n", SpiNor->FlashInfo->Name);

    SpiNor->DeviceSize = SpiNor->FlashInfo->SectorSize * SpiNor->FlashInfo->SectorNum;
    SpiNor->AddrBytes = (SpiNor->DeviceSize > 0x1000000) ? 4 : 3;
    SpiNor->EraseSize = SpiNor->FlashInfo->SectorSize;

    Status = AlSpiNor_SetOperationWidth(SpiNor);
    if (Status != AL_OK) {
        return Status;
    }

    if (SpiNor->Controller->Flags & AL_SPI_NOR_FLAG_4BYTE_SET) {
        ; /* Program configuration register to set the flash into 4byte mode. */
    }

    Status = AlSpiNor_SetProto(SpiNor);
    if (Status != AL_OK) {
        return Status;
    }

    SpiNor->SetWrap64 = (SpiNorSetWrap64)AlSpiNor_SetWrap64;

#ifdef DEBUG_SPI_NOR
    AL_LOG(AL_LOG_LEVEL_INFO,
        "Spi-nor debug info:\r\n"
        "  Cur buswidth: %d\r\n"
        "  Addr bytes: %d\r\n"
        "  Read opcode: %02xh\r\n"
        "  Write opcode: %02xh\r\n"
        "  Erase opcode: %02xh\r\n",
        SpiNor->CurBusWidth, SpiNor->AddrBytes,
        SpiNor->RdProto.OpCode, SpiNor->WrProto.OpCode, SpiNor->EraseProto.OpCode);
#endif

    return AL_OK;
}

