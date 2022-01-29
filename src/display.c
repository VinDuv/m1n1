/* SPDX-License-Identifier: MIT */

#include "display.h"
#include "assert.h"
#include "dcp.h"
#include "dcp_iboot.h"
#include "utils.h"
#include "xnuboot.h"

#define DISPLAY_STATUS_DELAY   100
#define DISPLAY_STATUS_RETRIES 50

#define COMPARE(a, b)                                                                              \
    if ((a) > (b)) {                                                                               \
        *best = modes[i];                                                                          \
        continue;                                                                                  \
    } else if ((a) < (b)) {                                                                        \
        continue;                                                                                  \
    }

void display_choose_timing_mode(dcp_timing_mode_t *modes, int cnt, dcp_timing_mode_t *best)
{
    *best = modes[0];

    for (int i = 1; i < cnt; i++) {
        COMPARE(modes[i].valid, best->valid);
        COMPARE(modes[i].width <= 1920, best->width <= 1920);
        COMPARE(modes[i].fps <= 60 << 16, best->fps <= 60 << 16);
        COMPARE(modes[i].width, best->width);
        COMPARE(modes[i].height, best->height);
        COMPARE(modes[i].fps, best->fps);
    }

    printf("display: timing mode: valid=%d %dx%d %d.%02d Hz\n", best->valid, best->width,
           best->height, best->fps >> 16, (best->fps & 0xffff) * 99 / 0xffff);
}

void display_choose_color_mode(dcp_color_mode_t *modes, int cnt, dcp_color_mode_t *best)
{
    *best = modes[0];

    for (int i = 1; i < cnt; i++) {
        COMPARE(modes[i].valid, best->valid);
        COMPARE(modes[i].bpp <= 32, best->bpp <= 32);
        COMPARE(modes[i].bpp, best->bpp);
        COMPARE(-modes[i].colorimetry, -best->colorimetry);
        COMPARE(-modes[i].encoding, -best->encoding);
        COMPARE(-modes[i].eotf, -best->eotf);
    }

    printf("display: color mode: valid=%d colorimetry=%d eotf=%d encoding=%d bpp=%d\n", best->valid,
           best->colorimetry, best->eotf, best->encoding, best->bpp);
}

int display_wait_connected(dcp_iboot_if_t *iboot, int *timing_cnt, int *color_cnt)
{
    int hpd;

    for (int retries = 0; retries < DISPLAY_STATUS_RETRIES; retries += 1) {
        hpd = dcp_ib_get_hpd(iboot, timing_cnt, color_cnt);
        if ((hpd > 0) && *timing_cnt && *color_cnt) {
            printf("display: waited %d ms for display connected\n", retries * DISPLAY_STATUS_DELAY);
            return 1;
        }

        mdelay(DISPLAY_STATUS_DELAY);
    }

    // hpd is 0 if no display, negative if an error occurred
    return hpd;
}

int display_wait_disconnected(dcp_iboot_if_t *iboot)
{
    int hpd, timing_cnt, color_cnt;

    for (int retries = 0; retries < DISPLAY_STATUS_RETRIES; retries += 1) {
        hpd = dcp_ib_get_hpd(iboot, &timing_cnt, &color_cnt);
        if (hpd < 0) {
            return hpd;
        }

        if (!hpd) {
            printf("display: waited %d ms for display disconnected\n",
                   retries * DISPLAY_STATUS_DELAY);
            return 1;
        }

        mdelay(DISPLAY_STATUS_DELAY);
    }

    return 0;
}

int display_configure(void)
{
    int ret = -1;

    dcp_dev_t *dcp = dcp_init("/arm-io/dcp", "/arm-io/dart-dcp", "/arm-io/dart-disp0");
    if (!dcp) {
        printf("display: failed to initialize DCP\n");
        return -1;
    }

    // Find the framebuffer DVA
    u64 fb_dva = dart_search(dcp->dart_disp, (void *)cur_boot_args.video.base);
    if (!fb_dva) {
        printf("display: failed to find display DVA\n");
        goto err_shutdown;
    }

    dcp_iboot_if_t *iboot = dcp_ib_init(dcp);
    if (!iboot) {
        printf("display: failed to initialize DCP iBoot interface\n");
        goto err_shutdown;
    }

    // Power on
    if ((ret = dcp_ib_set_power(iboot, true)) < 0) {
        printf("display: failed to set power\n");
        goto err_iboot;
    }

    // Detect if display is connected
    int timing_cnt, color_cnt;

    /* After boot DCP does not immediately report a connected display. Retry getting display
     * information for 2 seconds.
     */
    if ((ret = display_wait_connected(iboot, &timing_cnt, &color_cnt)) < 0) {
        printf("display: failed to get display status (%d)\n", ret);
        goto err_iboot;
    }

    printf("display: connected:%d timing_cnt:%d color_cnt:%d\n", ret, timing_cnt, color_cnt);

    if (!ret || !timing_cnt || !color_cnt)
        goto bail;

    // Find best modes
    dcp_timing_mode_t *tmodes, tbest;
    if ((ret = dcp_ib_get_timing_modes(iboot, &tmodes)) < 0) {
        printf("display: failed to get timing modes\n");
        goto err_iboot;
    }
    assert(ret == timing_cnt);
    display_choose_timing_mode(tmodes, timing_cnt, &tbest);

    dcp_color_mode_t *cmodes, cbest;
    if ((ret = dcp_ib_get_color_modes(iboot, &cmodes)) < 0) {
        printf("display: failed to get color modes\n");
        goto err_iboot;
    }
    assert(ret == color_cnt);
    display_choose_color_mode(cmodes, color_cnt, &cbest);

    // Set mode
    if ((ret = dcp_ib_set_mode(iboot, &tbest, &cbest)) < 0) {
        printf("display: failed to set mode\n");
        goto err_iboot;
    }

    // Swap!
    int swap_id = ret = dcp_ib_swap_begin(iboot);
    if (swap_id < 0) {
        printf("display: failed to start swap\n");
        goto err_iboot;
    }

    // Some monitors disconnect when getting out of sleep mode. Wait a bit to see if
    // that happens.
    if ((ret = display_wait_disconnected(iboot)) < 0) {
        printf("display: failed to wait for disconnect\n");
        goto err_iboot;
    }

    if (ret) {
        printf("display: disconnected, waiting for reconnection.\n");
        ret = display_wait_connected(iboot, &timing_cnt, &color_cnt);
        if (ret < 0) {
            printf("display: failed to wait for reconnection\n");
            goto err_iboot;
        } else if (ret == 0) {
            printf("display: not reconnected in time\n");
            ret = -1;
            goto err_iboot;
        }
    } else {
        printf("display: did not disconnect\n");
    }

    if ((ret = dcp_ib_set_mode(iboot, &tbest, &cbest)) < 0) {
        printf("display: failed to set mode\n");
        goto err_iboot;
    }

    dcp_layer_t layer = {
        .planes = {{
            .addr = fb_dva,
            .stride = tbest.width * 4,
            .addr_format = ADDR_PLANAR,
        }},
        .plane_cnt = 1,
        .width = tbest.width,
        .height = tbest.height,
        .surface_fmt = FMT_w30r,
        .colorspace = 2,
        .eotf = EOTF_GAMMA_SDR,
        .transform = XFRM_NONE,
    };

    dcp_rect_t rect = {tbest.width, tbest.height, 0, 0};

    if ((ret = dcp_ib_swap_set_layer(iboot, 0, &layer, &rect, &rect)) < 0) {
        printf("display: failed to set layer\n");
        goto err_iboot;
    }

    if ((ret = dcp_ib_swap_end(iboot)) < 0) {
        printf("display: failed to complete swap\n");
        goto err_iboot;
    }

    printf("display: swapped! (swap_id=%d)\n", swap_id);

    cur_boot_args.video.stride = layer.planes[0].stride;
    cur_boot_args.video.width = layer.width;
    cur_boot_args.video.height = layer.height;
    cur_boot_args.video.depth = 30;

bail:
    ret = 0;
err_iboot:
    dcp_ib_shutdown(iboot);
err_shutdown:
    dcp_shutdown(dcp);
    return ret;
}

int display_init(void)
{
    if (cur_boot_args.video.width == 640 && cur_boot_args.video.height == 1136) {
        printf("display: Dummy framebuffer found, initializing display\n");

        return display_configure();
    } else {
        printf("display: Display is already initialized (%ldx%ld)\n", cur_boot_args.video.width,
               cur_boot_args.video.height);
        return 0;
    }
}
