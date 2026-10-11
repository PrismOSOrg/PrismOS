// PrismCC AC'97 playback driver for QEMU's Intel 82801AA-compatible device.
// Copy to /AC97.C, then compile with:
//   cc /AC97.C /DRIVERS/AC97.PDR
// Start with driver-start /DRIVERS/AC97.PDR and stop with driver-stop <id>.
// driver_service(0, service_id, 0) advertises the audio service.
// driver_service(1, frequency_hz, 0) starts a continuous sine-wave tone.
// driver_service(2, 0, 0) stops the tone.

int ac97_wave_sample(int phase) {
    int index;

    if (phase <= 8) {
        index = phase;
        if (index == 0) return 0;
        if (index == 1) return 1598;
        if (index == 2) return 3135;
        if (index == 3) return 4547;
        if (index == 4) return 5793;
        if (index == 5) return 6827;
        if (index == 6) return 7600;
        if (index == 7) return 8082;
        return 8192;
    }
    if (phase < 16) {
        index = 16 - phase;
        if (index == 1) return 1598;
        if (index == 2) return 3135;
        if (index == 3) return 4547;
        if (index == 4) return 5793;
        if (index == 5) return 6827;
        if (index == 6) return 7600;
        return 8082;
    }
    if (phase < 24) {
        index = phase - 16;
        if (index == 0) return 0;
        if (index == 1) return -1598;
        if (index == 2) return -3135;
        if (index == 3) return -4547;
        if (index == 4) return -5793;
        if (index == 5) return -6827;
        if (index == 6) return -7600;
        return -8082;
    }
    index = 32 - phase;
    if (index == 8) return -8192;
    if (index == 1) return -1598;
    if (index == 2) return -3135;
    if (index == 3) return -4547;
    if (index == 4) return -5793;
    if (index == 5) return -6827;
    if (index == 6) return -7600;
    return -8082;
}

int ac97_start_tone(int frequency) {
    int busmaster;
    int mixer;
    int dma;
    int dma_phys;
    int result;

    if (frequency < 250) {
        return -1;
    }
    if (frequency > 600) {
        return -1;
    }
    busmaster = driver_state_read(2) - 1;
    mixer = driver_state_read(1) - 1;
    dma = driver_state_read(3) - 1;
    dma_phys = driver_dma_phys(dma);
    if (busmaster < 0) {
        return -1;
    }
    if (mixer < 0) {
        return -1;
    }
    if (dma < 0) {
        return -1;
    }
    if (dma_phys < 0) {
        return -1;
    }

    if (driver_pci_io_write(busmaster, 0x1B, 1, 0) != 0) {
        return -1;
    }
    if (driver_pci_io_write(busmaster, 0x16, 2, 0x1C) != 0) {
        return -1;
    }
    if (driver_pci_io_write(mixer, 0x2C, 2, frequency * 32) != 0) {
        return -1;
    }

    if (driver_pci_io_write(busmaster, 0x10, 4, dma_phys) != 0) {
        return -1;
    }
    if (driver_pci_io_write(busmaster, 0x15, 1,
            127) != 0) {
        return -1;
    }
    if (driver_state_write(4, 1) != 0) {
        return -1;
    }
    if (driver_pci_io_write(busmaster, 0x1B, 1, 0x01) != 0) {
        result = driver_state_write(4, 0);
        return -1;
    }
    return 0;
}

int ac97_stop_tone() {
    int busmaster;

    busmaster = driver_state_read(2) - 1;
    if (busmaster < 0) {
        return -1;
    }
    if (driver_pci_io_write(busmaster, 0x1B, 1, 0) != 0) {
        return -1;
    }
    if (driver_pci_io_write(busmaster, 0x16, 2, 0x1C) != 0) {
        return -1;
    }
    return driver_state_write(4, 0);
}

int driver_init() {
    int controller;
    int mixer;
    int busmaster;
    int dma;
    int dma_phys;
    int extended_id;
    int status;
    int frame;
    int descriptor;
    int sample;
    int packed_sample;
    int phase;

    controller = driver_pci_find(0x8086, 0x2415);
    if (controller < 0) {
        print("AC'97 controller 8086:2415 not found");
        return -1;
    }
    if (driver_pci_claim(controller) != 0) {
        print("AC'97 controller is already claimed");
        return -1;
    }
    if (driver_state_write(0, controller + 1) != 0) {
        return -1;
    }

    if (driver_pci_enable(controller, 0x01) != 0) {
        print("Could not enable AC'97 I/O");
        return -1;
    }
    mixer = driver_pci_claim_io_bar(controller, 0);
    busmaster = driver_pci_claim_io_bar(controller, 1);
    if (mixer < 0) {
        print("Could not claim AC'97 mixer I/O BAR");
        return -1;
    }
    if (busmaster < 0) {
        print("Could not claim AC'97 I/O BARs");
        return -1;
    }
    if (driver_state_write(1, mixer + 1) != 0) {
        return -1;
    }
    if (driver_state_write(2, busmaster + 1) != 0) {
        return -1;
    }

    extended_id = driver_pci_io_read(mixer, 0x28, 2);
    if (extended_id < 0) {
        print("Could not read AC'97 codec capabilities");
        return -1;
    }
    if (driver_pci_io_write(mixer, 0x02, 2, 0) != 0) {
        print("Could not set AC'97 master volume");
        return -1;
    }
    if (driver_pci_io_write(mixer, 0x18, 2, 0) != 0) {
        print("Could not set AC'97 output volume");
        return -1;
    }
    if ((extended_id & 1) == 0) {
        print("AC'97 codec does not support variable sample rates");
        return -1;
    }
    if ((extended_id & 1) != 0) {
        status = driver_pci_io_read(mixer, 0x2A, 2);
        if (status < 0) {
            print("Could not read AC'97 sample-rate controls");
            return -1;
        }
        if (driver_pci_io_write(mixer, 0x2A, 2, status | 1) != 0) {
            print("Could not enable AC'97 variable sample rate");
            return -1;
        }
    }

    dma = driver_dma_alloc_below(65536, 0x04000000);
    if (dma < 0) {
        print("Could not allocate AC'97 DMA memory below 64 MiB");
        return -1;
    }
    if (driver_state_write(3, dma + 1) != 0) {
        return -1;
    }
    dma_phys = driver_dma_phys(dma);
    if (dma_phys < 0) {
        print("AC'97 DMA buffer is outside the supported physical range");
        return -1;
    }

    frame = 0;
    while (frame < 4096) {
        phase = frame % 32;
        sample = ac97_wave_sample(phase);
        packed_sample = ((sample & 65535) << 16) | (sample & 65535);
        if (driver_dma_write(dma, 4096 + frame * 4, 4,
                packed_sample) != 0) {
            print("Could not prepare AC'97 waveform");
            return -1;
        }
        frame = frame + 1;
    }

    descriptor = 0;
    while (descriptor < 128) {
        if (driver_dma_write(dma, descriptor * 8, 4,
                dma_phys + 4096) != 0) {
            return -1;
        }
        if (driver_dma_write(dma, descriptor * 8 + 4, 4,
                8192) != 0) {
            print("Could not prepare AC'97 descriptor");
            return -1;
        }
        descriptor = descriptor + 1;
    }

    if (driver_pci_enable(controller, 0x04) != 0) {
        print("Could not enable AC'97 PCI bus mastering");
        return -1;
    }

    return 0;
}

int driver_service(int operation, int argument0, int argument1) {
    if (operation == 0) {
        if (argument0 == 0x41554449) {
            return 1;
        }
        return 0;
    }
    if (operation == 1) {
        return ac97_start_tone(argument0);
    }
    if (operation == 2) {
        return ac97_stop_tone();
    }
    return -1;
}

void driver_poll() {
    int should_stop;
    int audio_active;
    int busmaster;
    int status;
    int dma;
    int dma_phys;
    int result;

    should_stop = driver_should_stop();
    audio_active = driver_state_read(4);
    if (audio_active != 1) {
        return;
    }
    busmaster = driver_state_read(2) - 1;
    dma = driver_state_read(3) - 1;
    if ((busmaster < 0) | (dma < 0)) {
        print("AC'97 playback state is invalid");
        result = driver_state_write(4, 0);
        return;
    }
    status = driver_pci_io_read(busmaster, 0x16, 2);
    if (status < 0) {
        print("Could not read AC'97 playback status");
        result = driver_state_write(4, 0);
        return;
    }
    if ((status & 1) == 0) {
        return;
    }

    dma_phys = driver_dma_phys(dma);
    if (dma_phys < 0) {
        print("AC'97 playback DMA address is invalid");
        result = driver_state_write(4, 0);
        return;
    }
    result = driver_pci_io_write(busmaster, 0x16, 2, status & 0x1C);
    if (result == 0) {
        result = driver_pci_io_write(busmaster, 0x10, 4, dma_phys);
    }
    if (result == 0) {
        result = driver_pci_io_write(busmaster, 0x15, 1,
            127);
    }
    if (result == 0) {
        result = driver_pci_io_write(busmaster, 0x1B, 1, 0x01);
    }
    if (result != 0) {
        print("Could not restart AC'97 playback");
        result = driver_state_write(4, 0);
    }
}

void driver_shutdown() {
    int controller;
    int busmaster;
    int shutdown_started;
    int status;
    int result;

    shutdown_started = driver_state_read(7);
    if (shutdown_started == 1) {
        return;
    }
    if (driver_state_write(7, 1) != 0) {
        print("Could not mark AC'97 shutdown in progress");
    }

    controller = driver_state_read(0) - 1;
    busmaster = driver_state_read(2) - 1;

    if (busmaster >= 0) {
        result = driver_pci_io_write(busmaster, 0x1B, 1, 0);
        if (result != 0) {
            print("Could not stop AC'97 playback during shutdown");
        }
        status = driver_pci_io_read(busmaster, 0x16, 2);
        if ((status >= 0) & ((status & 0x1C) != 0)) {
            result = driver_pci_io_write(busmaster, 0x16, 2, status & 0x1C);
            if (result != 0) {
                print("Could not clear AC'97 status during shutdown");
            }
        }
    }
    if (controller >= 0) {
        result = driver_pci_disable(controller, 0x04);
        if (result != 0) {
            print("Could not disable AC'97 bus mastering");
        }
    }
    if (controller >= 0) {
        result = driver_pci_disable(controller, 0x01);
        if (result != 0) {
            print("Could not disable AC'97 I/O");
        }
    }
    // The driver runtime frees DMA after this callback returns and bus mastering is disabled.
}

int main() {
    int status = driver_init();
    if (status != 0) {
        driver_shutdown();
        return status;
    }

    while (driver_should_stop() == 0) {
        driver_poll();
    }
    driver_shutdown();
    return 0;
}
