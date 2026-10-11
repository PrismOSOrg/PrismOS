// PCI resource and DMA smoke test for QEMU's common AC'97 audio controller.
// Copy to /PCIAC97.C, then compile with:
//   cc /PCIAC97.C /DRIVERS/PCIAC97.PDR
// Run with driver-run /DRIVERS/PCIAC97.PDR, then press Esc to exit.

int driver_init() {
    print("Starting AC'97 PCI resource probe");
    return 0;
}

void driver_poll() {
}

void driver_shutdown() {
}

int main() {
    int controller_bdf;
    int io_region;
    int dma_buffer;
    int dma_address;
    int status;

    if (driver_init() != 0) {
        return -1;
    }

    controller_bdf = driver_pci_find(0x8086, 0x2415);
    if (controller_bdf < 0) {
        print("QEMU AC'97 controller not found");
        return -1;
    }
    if (driver_pci_claim(controller_bdf) != 0) {
        print("QEMU AC'97 controller unavailable or already claimed");
        return -1;
    }

    if (driver_pci_enable(controller_bdf, 0x01) != 0) {
        print("Could not enable AC'97 I/O");
        return -1;
    }
    io_region = driver_pci_claim_io_bar(controller_bdf, 1);
    if (io_region < 0) {
        print("Could not claim AC'97 bus-master I/O BAR");
        return -1;
    }

    print("AC'97 bus-master status:");
    print_int(driver_pci_io_read(io_region, 0x30, 2));
    dma_buffer = driver_dma_alloc(4096);
    if (dma_buffer < 0) {
        print("Could not allocate low-memory DMA buffer");
        return -1;
    }
    dma_address = driver_dma_phys(dma_buffer);
    if (dma_address == -1) {
        print("DMA buffer has no 32-bit physical address");
        return -1;
    }
    if (driver_dma_write(dma_buffer, 0, 2, 0x1234) != 0) {
        print("Could not write the DMA buffer");
        return -1;
    }
    if (driver_dma_read(dma_buffer, 0, 2) != 0x1234) {
        print("DMA buffer validation failed");
        return -1;
    }
    print("DMA physical address:");
    print_int(dma_address);

    if (driver_pci_enable(controller_bdf, 0x04) != 0) {
        print("Could not enable AC'97 bus mastering");
        return -1;
    }
    while (driver_should_stop() == 0) {
        driver_poll();
    }

    status = driver_pci_disable(controller_bdf, 0x04);
    if (status == 0) {
        status = driver_dma_free(dma_buffer);
    }
    driver_shutdown();
    return status;
}
