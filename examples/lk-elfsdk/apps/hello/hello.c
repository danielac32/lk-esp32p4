#include "lk_app.h"

int main(int argc, char *argv[]) {
    printf("Hello from LK ELF app!\n");
    printf("argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d]=%s\n", i, argv[i] ? argv[i] : "(null)");
    }
    printf("LK elfload test OK\n");
    return 0;
}
