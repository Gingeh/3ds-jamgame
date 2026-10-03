#include "cameraButton.h"
#include <3ds.h>
#include <3ds/allocator/linear.h>

#define WIDTH 160
#define HEIGHT 120
#define MAX_BUF_SIZE (WIDTH * HEIGHT * 2)

u16* buf;
u32 transfer_size;
Handle cam_receive_event = 0;
Handle cam_error_event = 0;

int covered_frames = 0;

void camera_init() {
    camInit();

    CAMU_SetSize(SELECT_IN1, SIZE_QQVGA, CONTEXT_A);
    CAMU_SetOutputFormat(SELECT_IN1, OUTPUT_RGB_565, CONTEXT_A);

    // ideal settings for thumb detection
    CAMU_SetFrameRate(SELECT_IN1, FRAME_RATE_15);
    CAMU_SetAutoExposure(SELECT_IN1, true);
    CAMU_SetAutoWhiteBalance(SELECT_IN1, false);
    CAMU_SetNoiseFilter(SELECT_IN1, true);

    buf = linearAlloc(MAX_BUF_SIZE);
    CAMU_GetMaxBytes(&transfer_size, WIDTH, HEIGHT);
    CAMU_SetTransferBytes(PORT_CAM1, transfer_size, WIDTH, HEIGHT);
    CAMU_Activate(SELECT_IN1);

    CAMU_GetBufferErrorInterruptEvent(&cam_error_event, PORT_CAM1);
    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_StartCapture(PORT_CAM1);
}

void camera_exit() {
    CAMU_StopCapture(PORT_CAM1);
    CAMU_Activate(SELECT_NONE);
    camExit();
    linearFree(buf);
}

#define BLOCK_SIZE 8
bool is_blocked_by_thumb() {
    float sum = 0.0f, sq_sum = 0.0f;

    for (int by = 0; by < HEIGHT; by += BLOCK_SIZE) {
        for (int bx = 0; bx < WIDTH; bx += BLOCK_SIZE) {
            u32 block_luma = 0;
            for (int y = 0; y < BLOCK_SIZE; y++) {
                int row_offset = (by + y) * WIDTH;
                for (int x = 0; x < BLOCK_SIZE; x++) {
                    u16 p = buf[row_offset + bx + x];

                    u32 r = (p >> 8) & 0xF8; r |= (r >> 5);
                    u32 g = (p >> 3) & 0xFC; g |= (g >> 6);
                    u32 b = (p << 3) & 0xF8; b |= (b >> 5);

                    block_luma += (77 * r + 150 * g + 29 * b) >> 8;
                }
            }

            float block_avg = (float)block_luma / (BLOCK_SIZE * BLOCK_SIZE);
            sum += block_avg;
            sq_sum += block_avg * block_avg;
        }
    }

    int total_blocks = (WIDTH / BLOCK_SIZE) * (HEIGHT / BLOCK_SIZE);
    float mean = sum / total_blocks;
    float variance = (sq_sum / total_blocks) - (mean * mean);
    return (mean < 20.0f) && (variance < 100.0f);
}

void force_restart_capture() {
    // call on buffer error
    if (cam_receive_event != 0) {
        svcCloseHandle(cam_receive_event);
        cam_receive_event = 0;
    }
    CAMU_StopCapture(PORT_CAM1);
    CAMU_ClearBuffer(PORT_CAM1);
    CAMU_StartCapture(PORT_CAM1);
}

enum CamResult camera_check_covered() {
    if (cam_receive_event == 0) {
        CAMU_SetReceiving(&cam_receive_event, buf, PORT_CAM1, MAX_BUF_SIZE, transfer_size);
    }

    if (svcWaitSynchronization(cam_error_event, 0) == 0) {
        force_restart_capture();
        return PENDING;
    }

    if (svcWaitSynchronization(cam_receive_event, 0) != 0) {
        return PENDING;
    }

    svcCloseHandle(cam_receive_event);
    cam_receive_event = 0;
    return is_blocked_by_thumb();
}
