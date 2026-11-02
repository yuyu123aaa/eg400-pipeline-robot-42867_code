{
    "doc": {
        "DOCS_DOXYGEN": {
            "name": "DOCS_DOXYGEN",
            "path": "docs/doxygen",
            "version": "1.0"
        }
    },
    "driver": {
        "PL_DRIVER_AXI_DMA": {
            "Default": true,
            "depends": [
                "fpsoc/driver/pl_driver/axi_dma"
            ],
            "name": "PL_DRIVER_AXI_DMA",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PL_DRIVER_AXI_ETHERNET_LITE": {
            "Default": true,
            "depends": [
                "fpsoc/driver/pl_driver/axi_ethernet_lite"
            ],
            "name": "PL_DRIVER_AXI_ETHERNET_LITE",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PL_DRIVER_AXI_GPIO": {
            "Default": true,
            "depends": [
                "fpsoc/driver/pl_driver/axi_gpio"
            ],
            "name": "PL_DRIVER_AXI_GPIO",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PL_DRIVER_AXI_IIC": {
            "Default": true,
            "depends": [
                "fpsoc/driver/pl_driver/axi_iic"
            ],
            "name": "PL_DRIVER_AXI_IIC",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PL_DRIVER_AXI_QUAD_SPI": {
            "Default": true,
            "depends": [
                "fpsoc/driver/pl_driver/axi_quad_spi",
                "PS_DRIVER_SPI_NOR"
            ],
            "name": "PL_DRIVER_AXI_QUAD_SPI",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PL_DRIVER_AXI_UART_LITE": {
            "Default": true,
            "depends": [
                "fpsoc/driver/pl_driver/axi_uart_lite"
            ],
            "name": "PL_DRIVER_AXI_UART_LITE",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PL_DRIVER_AXI_VDMA": {
            "Default": true,
            "depends": [
                "fpsoc/driver/pl_driver/axi_vdma"
            ],
            "name": "PL_DRIVER_AXI_VDMA",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_ADC": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/adc"
            ],
            "name": "PS_DRIVER_ADC",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_DMA": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/dma",
                "PS_DRIVER_IPC"
            ],
            "name": "PS_DRIVER_DMA",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_DMACAHB": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/dmacahb"
            ],
            "name": "PS_DRIVER_DMACAHB",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_GBE": {
            "depends": [
                "fpsoc/driver/ps_driver/gbe"
            ],
            "name": "PS_DRIVER_GBE",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_GPIO": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/gpio"
            ],
            "name": "PS_DRIVER_GPIO",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_IIC": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/iic",
                "PS_DRIVER_DMACAHB"
            ],
            "name": "PS_DRIVER_IIC",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_IPC": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/ipc"
            ],
            "name": "PS_DRIVER_IPC",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_MMC": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/mmc"
            ],
            "name": "PS_DRIVER_MMC",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_MPU": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/mpu"
            ],
            "name": "PS_DRIVER_MPU",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_PMU": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/pmu"
            ],
            "name": "PS_DRIVER_PMU",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_QSPI": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/qspi",
                "PS_DRIVER_DMACAHB",
                "PS_DRIVER_SPI_NOR"
            ],
            "name": "PS_DRIVER_QSPI",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_SMC": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/smc"
            ],
            "name": "PS_DRIVER_SMC",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_SPI": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/spi",
                "PS_DRIVER_DMACAHB",
                "PS_DRIVER_SPI_NOR"
            ],
            "name": "PS_DRIVER_SPI",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_SPI_NOR": {
            "depends": [
                "fpsoc/driver/ps_driver/spi-nor"
            ],
            "name": "PS_DRIVER_SPI_NOR",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_TC": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/tc"
            ],
            "name": "PS_DRIVER_TC",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_UART": {
            "depends": [
                "fpsoc/driver/ps_driver/uart",
                "PS_DRIVER_DMACAHB"
            ],
            "name": "PS_DRIVER_UART",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_USB": {
            "depends": [
                "fpsoc/driver/ps_driver/usb",
                "LIB_FATFS"
            ],
            "name": "PS_DRIVER_USB",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_WDT": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/wdt",
                "PS_DRIVER_PMU"
            ],
            "name": "PS_DRIVER_WDT",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "PS_DRIVER_XMON": {
            "Default": true,
            "depends": [
                "fpsoc/driver/ps_driver/xmon"
            ],
            "name": "PS_DRIVER_XMON",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        }
    },
    "library": {
        "LIB_CHERRYUSB": {
            "Default": true,
            "depends": [
                "3rdparty/lib/CherryUSB"
            ],
            "description": "Version : v1.3.0",
            "name": "LIB_CHERRYUSB",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "LIB_FATFS": {
            "Default": true,
            "depends": [
                "3rdparty/lib/FATFS",
                "PS_DRIVER_MMC"
            ],
            "description": "Version : vR0.14b",
            "name": "LIB_FATFS",
            "settings": {
                "config": "docs/settings/fatfs/ffconf.h",
                "destination": "3rdparty/lib/FATFS/ffconf.h",
                "property": "docs/settings/fatfs/fatfs.json"
            },
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "LIB_GCC": {
            "Default": true,
            "depends": [
                "fpsoc/lib/newlib/gcc"
            ],
            "description": "LIB_GCC",
            "name": "LIB_GCC",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "LIB_LOG": {
            "Default": true,
            "depends": [
                "fpsoc/lib/log",
                "PS_DRIVER_UART"
            ],
            "description": "LIB_LOG",
            "name": "LIB_LOG",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "LIB_METAL": {
            "depends": [
                "3rdparty/lib/libmetal"
            ],
            "description": "Version : v1.3.0",
            "name": "LIB_METAL",
            "supportedProc": [
                "apu-0",
                "apu-1"
            ],
            "version": "1.0"
        },
        "LIB_NMSIS": {
            "Default": true,
            "depends": [
                "3rdparty/lib/NMSIS"
            ],
            "description": "Version : v1.1.1",
            "name": "LIB_NMSIS",
            "supportedProc": [
                "apu-0",
                "apu-1",
                "rpu"
            ],
            "version": "1.0"
        },
        "LIB_OPENAMP": {
            "depends": [
                "3rdparty/lib/open-amp",
                "LIB_METAL"
            ],
            "description": "Version : v1.3.0",
            "name": "LIB_OPENAMP",
            "settings": {
                "config": "docs/settings/openamp/Makefile",
                "destination": "3rdparty/lib/open-amp/Makefile",
                "property": "docs/settings/openamp/openamp.json"
            },
            "supportedProc": [
                "apu-0",
                "apu-1"
            ],
            "version": "1.0"
        }
    },
    "os": {
        "depends": [],
        "description": "standalone",
        "name": "standalone",
        "supportedProc": [
            "apu-0",
            "apu-1",
            "rpu"
        ],
        "version": "1.0"
    },
    "processor": {
        "depends": [
            "fpsoc/chip/dr1x90/common",
            "fpsoc/chip/dr1x90/dr1m90",
            "fpsoc/arch/arm",
            "fpsoc/arch/common/inc",
            "fpsoc/inc"
        ],
        "name": "apu-1"
    }
}