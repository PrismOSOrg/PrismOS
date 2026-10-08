// PrismCC COM1 UART echo driver. Copy to /SERDRV.C on a PrismOS drive.
// Then create /DRIVERS, compile to /DRIVERS/SERIAL.PDR, and run driver-run.
// Press Esc in the VM console to request shutdown.
//
// Ports are decimal because PrismCC currently accepts decimal integer
// literals: data=1016, interrupt-enable=1017, line-status=1021.

int driver_init() {
    int interrupt_status = driver_io_write8(1017, 0);
    int line_status = driver_io_read8(1021);
    if (interrupt_status != 0) {
        print("driver: COM1 access denied");
        return -1;
    }
    if (line_status < 0) {
        print("driver: COM1 access denied");
        return -1;
    }

    int status = driver_serial_write("PrismCC COM1 echo driver started\n");
    return status;
}

void driver_poll() {
    int line_status = driver_io_read8(1021);
    if (line_status >= 0) {
        int receive_ready = line_status % 2;
        int transmit_ready = (line_status / 32) % 2;
        if (receive_ready == 1) {
            if (transmit_ready == 1) {
                int value = driver_io_read8(1016);
                if (value >= 0) {
                    int status = driver_io_write8(1016, value);
                }
            }
        }
    }
}

void driver_shutdown() {
    int interrupt_status = driver_io_write8(1017, 1);
    int status = driver_serial_write("\nPrismCC COM1 echo driver stopped\n");
}

int main() {
    int status = driver_init();
    if (status != 0) {
        return status;
    }

    print("Type into the QEMU serial terminal; press Esc here to stop.");
    while (driver_should_stop() == 0) {
        driver_poll();
    }

    driver_shutdown();
    return 0;
}