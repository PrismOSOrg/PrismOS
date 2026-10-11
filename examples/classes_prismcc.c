class Point {
    int x;
    int y;
};

int main() {
    class Point point;

    point.x = 12;
    point.y = 30;

    print("Point coordinates:");
    print_int(point.x);
    print_int(point.y);
    return 0;
}
