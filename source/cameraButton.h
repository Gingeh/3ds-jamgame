#include <stdbool.h>

enum CamResult {
    NOT_COVERED = false,
    COVERED = true,
    PENDING,
};

void camera_init();
void camera_exit();
enum CamResult camera_check_covered();
