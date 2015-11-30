/*
 *  Copyright (c) 2015 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <assert.h>
#include <limits.h>
#include <math.h>

#include "./vpx_dsp_rtcd.h"
#include "vpx_dsp/vpx_dsp_common.h"
#include "vpx_scale/yv12config.h"
#include "vpx/vpx_integer.h"
#include "vp9/common/vp9_reconinter.h"
#include "vp9/encoder/vp9_context_tree.h"
#include "vp9/encoder/vp9_noise_estimate.h"
#include "vp9/encoder/vp9_encoder.h"

void vp9_noise_estimate_init(NOISE_ESTIMATE *const ne,
                             int width,
                             int height) {
  ne->enabled = 0;
  ne->level = kLowLow;
  ne->value = 0;
  ne->count = 0;
  ne->thresh = 90;
  ne->last_w = 0;
  ne->last_h = 0;
  if (width * height >= 1920 * 1080) {
    ne->thresh = 200;
  } else if (width * height >= 1280 * 720) {
    ne->thresh = 130;
  }
  ne->num_frames_estimate = 20;
}

int enable_noise_estimation(VP9_COMP *const cpi) {
  // Enable noise estimation if denoising is on (and cyclic refresh, since
  // noise estimate is currently using a struct defined in cyclic refresh).
#if CONFIG_VP9_TEMPORAL_DENOISING
  if (cpi->oxcf.noise_sensitivity > 0 &&
      cpi->oxcf.aq_mode == CYCLIC_REFRESH_AQ)
    return 1;
#endif
  // Only allow noise estimate under certain encoding mode.
  // Enabled for 1 pass CBR, speed >=5, and if resolution is same as original.
  // Not enabled for SVC mode and screen_content_mode.
  // Not enabled for low resolutions.
  if (cpi->oxcf.pass == 0 &&
      cpi->oxcf.rc_mode == VPX_CBR &&
      cpi->oxcf.aq_mode == CYCLIC_REFRESH_AQ &&
      cpi->oxcf.speed >= 5 &&
      cpi->resize_state == ORIG &&
      cpi->resize_pending == 0 &&
      !cpi->use_svc &&
      cpi->oxcf.content != VP9E_CONTENT_SCREEN &&
      cpi->common.width >= 640 &&
      cpi->common.height >= 480)
    return 1;
  else
    return 0;
}

static void copy_frame(YV12_BUFFER_CONFIG * const dest,
                       const YV12_BUFFER_CONFIG * const src) {
  int r;
  const uint8_t *srcbuf = src->y_buffer;
  uint8_t *destbuf = dest->y_buffer;

  assert(dest->y_width == src->y_width);
  assert(dest->y_height == src->y_height);

  for (r = 0; r < dest->y_height; ++r) {
    memcpy(destbuf, srcbuf, dest->y_width);
    destbuf += dest->y_stride;
    srcbuf += src->y_stride;
  }
}

void vp9_update_noise_estimate(VP9_COMP *const cpi) {
  const VP9_COMMON *const cm = &cpi->common;
  CYCLIC_REFRESH *const cr = cpi->cyclic_refresh;
  NOISE_ESTIMATE *const ne = &cpi->noise_estimate;
  // Estimate of noise level every frame_period frames.
  int frame_period = 10;
  int thresh_consec_zeromv = 8;
  unsigned int thresh_sum_diff = 100;
  unsigned int thresh_sum_spatial = (200 * 200) << 8;
  unsigned int thresh_spatial_var = (32 * 32) << 8;
  int min_blocks_estimate = cm->mi_rows * cm->mi_cols >> 7;
  // Estimate is between current source and last source.
  YV12_BUFFER_CONFIG *last_source = cpi->Last_Source;
#if CONFIG_VP9_TEMPORAL_DENOISING
  if (cpi->oxcf.noise_sensitivity > 0)
    last_source = &cpi->denoiser.last_source;
#endif
  ne->enabled = enable_noise_estimation(cpi);
  if (!ne->enabled ||
      cm->current_video_frame % frame_period != 0 ||
      last_source == NULL ||
      ne->last_w != cm->width ||
      ne->last_h != cm->height) {
#if CONFIG_VP9_TEMPORAL_DENOISING
  if (cpi->oxcf.noise_sensitivity > 0)
    copy_frame(&cpi->denoiser.last_source, cpi->Source);
#endif
    if (last_source != NULL) {
      ne->last_w = cm->width;
      ne->last_h = cm->height;
    }
    return;
  } else {
    int num_samples = 0;
    uint64_t avg_est = 0;
    int bsize = BLOCK_16X16;
    static const unsigned char const_source[16] = {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    // Loop over sub-sample of 16x16 blocks of frame, and for blocks that have
    // been encoded as zero/small mv at least x consecutive frames, compute
    // the variance to update estimate of noise in the source.
    const uint8_t *src_y = cpi->Source->y_buffer;
    const int src_ystride = cpi->Source->y_stride;
    const uint8_t *last_src_y = last_source->y_buffer;
    const int last_src_ystride = last_source->y_stride;
    const uint8_t *src_u = cpi->Source->u_buffer;
    const uint8_t *src_v = cpi->Source->v_buffer;
    const int src_uvstride = cpi->Source->uv_stride;
    const int y_width_shift = (4 << b_width_log2_lookup[bsize]) >> 1;
    const int y_height_shift = (4 << b_height_log2_lookup[bsize]) >> 1;
    const int uv_width_shift = y_width_shift >> 1;
    const int uv_height_shift = y_height_shift >> 1;
    int mi_row, mi_col;
    int num_low_motion = 0;
    int frame_low_motion = 1;
    for (mi_row = 0; mi_row < cm->mi_rows; mi_row++) {
      for (mi_col = 0; mi_col < cm->mi_cols; mi_col++) {
        int bl_index = mi_row * cm->mi_cols + mi_col;
        if (cr->consec_zero_mv[bl_index] > thresh_consec_zeromv)
          num_low_motion++;
      }
    }
    if (num_low_motion < ((3 * cm->mi_rows * cm->mi_cols) >> 3))
      frame_low_motion = 0;
    for (mi_row = 0; mi_row < cm->mi_rows; mi_row++) {
      for (mi_col = 0; mi_col < cm->mi_cols; mi_col++) {
        // 16x16 blocks, 1/4 sample of frame.
        if (mi_row % 4 == 0 && mi_col % 4 == 0) {
          int bl_index = mi_row * cm->mi_cols + mi_col;
          int bl_index1 = bl_index + 1;
          int bl_index2 = bl_index + cm->mi_cols;
          int bl_index3 = bl_index2 + 1;
          // Only consider blocks that are likely steady background. i.e, have
          // been encoded as zero/low motion x (= thresh_consec_zeromv) frames
          // in a row. consec_zero_mv[] defined for 8x8 blocks, so consider all
          // 4 sub-blocks for 16x16 block. Also, avoid skin blocks.
          const uint8_t ysource =
            src_y[y_height_shift * src_ystride + y_width_shift];
          const uint8_t usource =
            src_u[uv_height_shift * src_uvstride + uv_width_shift];
          const uint8_t vsource =
            src_v[uv_height_shift * src_uvstride + uv_width_shift];
          int is_skin = vp9_skin_pixel(ysource, usource, vsource);
          if (frame_low_motion &&
              cr->consec_zero_mv[bl_index] > thresh_consec_zeromv &&
              cr->consec_zero_mv[bl_index1] > thresh_consec_zeromv &&
              cr->consec_zero_mv[bl_index2] > thresh_consec_zeromv &&
              cr->consec_zero_mv[bl_index3] > thresh_consec_zeromv &&
              !is_skin) {
            // Compute variance.
            unsigned int sse;
            unsigned int variance = cpi->fn_ptr[bsize].vf(src_y,
                                                          src_ystride,
                                                          last_src_y,
                                                          last_src_ystride,
                                                          &sse);
            // Only consider this block as valid for noise measurement if the
            // average term (sse - variance = N * avg^{2}, N = 16X16) of the
            // temporal residual is small (avoid effects from lighting change).
            if ((sse - variance) < thresh_sum_diff) {
              unsigned int sse2;
              const unsigned int spatial_variance =
                  cpi->fn_ptr[bsize].vf(src_y, src_ystride, const_source,
                                        0, &sse2);
              // Avoid blocks with high brightness and high spatial variance.
              if ((sse2 - spatial_variance) < thresh_sum_spatial &&
                  spatial_variance < thresh_spatial_var) {
                avg_est += variance / ((spatial_variance >> 9) + 1);
                num_samples++;
              }
            }
          }
        }
        src_y += 8;
        last_src_y += 8;
        src_u += 4;
        src_v += 4;
      }
      src_y += (src_ystride << 3) - (cm->mi_cols << 3);
      last_src_y += (last_src_ystride << 3) - (cm->mi_cols << 3);
      src_u += (src_uvstride << 2) - (cm->mi_cols << 2);
      src_v += (src_uvstride << 2) - (cm->mi_cols << 2);
    }
    ne->last_w = cm->width;
    ne->last_h = cm->height;
    // Update noise estimate if we have at a minimum number of block samples,
    // and avg_est > 0 (avg_est == 0 can happen if the application inputs
    // duplicate frames).
    if (num_samples > min_blocks_estimate && avg_est > 0) {
      // Normalize.
      avg_est = avg_est / num_samples;
      // Update noise estimate.
      ne->value = (int)((15 * ne->value + avg_est) >> 4);
      ne->count++;
      if (ne->count == ne->num_frames_estimate) {
        // Reset counter and check noise level condition.
        ne->num_frames_estimate = 30;
        ne->count = 0;
        if (ne->value > (ne->thresh << 1)) {
          ne->level = kHigh;
        } else {
          if (ne->value > ne->thresh)
            ne->level = kMedium;
          else if (ne->value > (ne->thresh >> 1))
            ne->level = kLow;
          else
            ne->level = kLowLow;
        }
#if CONFIG_VP9_TEMPORAL_DENOISING
        if (cpi->oxcf.noise_sensitivity > 0)
          vp9_denoiser_set_noise_level(&cpi->denoiser, ne->level);
#endif
      }
    }
  }
#if CONFIG_VP9_TEMPORAL_DENOISING
  if (cpi->oxcf.noise_sensitivity > 0)
    copy_frame(&cpi->denoiser.last_source, cpi->Source);
#endif
}
