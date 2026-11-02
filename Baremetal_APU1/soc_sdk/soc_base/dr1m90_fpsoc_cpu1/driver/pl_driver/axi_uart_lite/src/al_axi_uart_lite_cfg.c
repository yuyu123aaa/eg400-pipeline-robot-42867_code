#include "al_axi_uart_lite_hw.h"
AlAxiUartLite_HwConfigStruct AlAxiUartLite_HwConfig[AL_AXI_UART_LITE_NUM_INSTANCE] = {
{
.DeviceId             = 0,
.BaseAddress          = 0x80400000,
.IntrId               = 117,
.BaudRate             = 9600,
.DataWidth            = 8,
.ParityEn             = 0,
.ParityOdd            = 0,
.ParityEven           = 0,
},
{
.DeviceId             = 1,
.BaseAddress          = 0x80500000,
.IntrId               = 118,
.BaudRate             = 9600,
.DataWidth            = 8,
.ParityEn             = 0,
.ParityOdd            = 0,
.ParityEven           = 0,
},
{
.DeviceId             = 2,
.BaseAddress          = 0x80600000,
.IntrId               = 119,
.BaudRate             = 115200,
.DataWidth            = 8,
.ParityEn             = 0,
.ParityOdd            = 0,
.ParityEven           = 0,
},
{
.DeviceId             = 3,
.BaseAddress          = 0x80700000,
.IntrId               = 120,
.BaudRate             = 115200,
.DataWidth            = 8,
.ParityEn             = 0,
.ParityOdd            = 0,
.ParityEven           = 0,
}
};
