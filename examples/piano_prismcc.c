int piano_frequency(int key) {
    if (key == 'z') return 262;
    if (key == 's') return 277;
    if (key == 'x') return 294;
    if (key == 'd') return 311;
    if (key == 'c') return 330;
    if (key == 'v') return 349;
    if (key == 'g') return 370;
    if (key == 'b') return 392;
    if (key == 'h') return 415;
    if (key == 'n') return 440;
    if (key == 'j') return 466;
    if (key == 'm') return 494;
    if (key == ',') return 523;
    return 0;
}

int main() {
    int driver_id;
    int key;
    int frequency;
    int result;

    print("PrismOS mini piano");
    print("White keys: Z X C V B N M ,   Black keys: S D G H J");
    print("Detecting audio driver...");
    driver_id = driver_service_find(0x41554449);
    if (driver_id < 1) {
        print("No running PrismCC audio driver found.");
        print("Start the AC'97 driver, then run the piano again.");
        return 1;
    }
    print("Audio driver found. Press Escape to quit.");
    key = 0;
    while (key != 27) {
        key = app_read_key();
        if (key != 27) {
            frequency = piano_frequency(key);
            if (frequency > 0) {
                result = driver_service_call(driver_id, 1, frequency, 90);
                if (result != 0) {
                    print("Audio driver service failed; check driver-list.");
                    return 1;
                }
            }
        }
    }
    print("Piano closed.");
    return 0;
}
