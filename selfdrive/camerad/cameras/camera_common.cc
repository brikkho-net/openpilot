#include <thread>
#include <chrono>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>

#if defined(QCOM) && !defined(QCOM_REPLAY)
#include "cameras/camera_qcom.h"
#elif QCOM2
#include "cameras/camera_qcom2.h"
#elif WEBCAM
#include "cameras/camera_webcam.h"
#else
#include "cameras/camera_frame_stream.h"
#endif

#include "camera_common.h"
#include <libyuv.h>
#include <jpeglib.h>

#include "clutil.h"
#include "common/params.h"
#include "common/swaglog.h"
#include "common/util.h"
#include "common/utilpp.h"
#include "imgproc/utils.h"

const int env_xmin = getenv("XMIN") ? atoi(getenv("XMIN")) : 0;
const int env_xmax = getenv("XMAX") ? atoi(getenv("XMAX")) : -1;
const int env_ymin = getenv("YMIN") ? atoi(getenv("YMIN")) : 0;
const int env_ymax = getenv("YMAX") ? atoi(getenv("YMAX")) : -1;
const int env_scale = getenv("SCALE") ? atoi(getenv("SCALE")) : 1;

static cl_program build_debayer_program(cl_device_id device_id, cl_context context, const CameraInfo *ci, const CameraBuf *b) {
  char args[4096];
  snprintf(args, sizeof(args),
           "-cl-fast-relaxed-math -cl-denorms-are-zero "
           "-DFRAME_WIDTH=%d -DFRAME_HEIGHT=%d -DFRAME_STRIDE=%d "
           "-DRGB_WIDTH=%d -DRGB_HEIGHT=%d -DRGB_STRIDE=%d "
           "-DBAYER_FLIP=%d -DHDR=%d",
           ci->frame_width, ci->frame_height, ci->frame_stride,
           b->rgb_width, b->rgb_height, b->rgb_stride,
           ci->bayer_flip, ci->hdr);
#ifdef QCOM2
  return cl_program_from_file(context, device_id, "cameras/real_debayer.cl", args);
#else
  return cl_program_from_file(context, device_id, "cameras/debayer.cl", args);
#endif
}

void CameraBuf::init(cl_device_id device_id, cl_context context, CameraState *s, VisionIpcServer * v, int frame_cnt, VisionStreamType rgb_type, VisionStreamType yuv_type, release_cb release_callback) {
  vipc_server = v;
  this->rgb_type = rgb_type;
  this->yuv_type = yuv_type;
  this->release_callback = release_callback;

  const CameraInfo *ci = &s->ci;
  camera_state = s;
  frame_buf_count = frame_cnt;

  // RAW frame
  frame_size = ci->frame_height * ci->frame_stride;
  camera_bufs = std::make_unique<VisionBuf[]>(frame_buf_count);
  camera_bufs_metadata = std::make_unique<FrameMetadata[]>(frame_buf_count);

  for (int i = 0; i < frame_buf_count; i++) {
    camera_bufs[i].allocate(frame_size);
    camera_bufs[i].init_cl(device_id, context);
  }

  rgb_width = ci->frame_width;
  rgb_height = ci->frame_height;
#ifndef QCOM2
  // debayering does a 2x downscale
  if (ci->bayer) {
    rgb_width = ci->frame_width / 2;
    rgb_height = ci->frame_height / 2;
  }
  float db_s = 0.5;
#else
  float db_s = 1.0;
#endif
  const mat3 transform = (mat3){{
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    0.0, 0.0, 1.0
  }};
  yuv_transform = ci->bayer ? transform_scale_buffer(transform, db_s) : transform;

  vipc_server->create_buffers(rgb_type, UI_BUF_COUNT, true, rgb_width, rgb_height);
  rgb_stride = vipc_server->get_buffer(rgb_type)->stride;

  vipc_server->create_buffers(yuv_type, YUV_COUNT, false, rgb_width, rgb_height);

  if (ci->bayer) {
    cl_program prg_debayer = build_debayer_program(device_id, context, ci, this);
    krnl_debayer = CL_CHECK_ERR(clCreateKernel(prg_debayer, "debayer10", &err));
    CL_CHECK(clReleaseProgram(prg_debayer));
  }

  rgb_to_yuv_init(&rgb_to_yuv_state, context, device_id, rgb_width, rgb_height, rgb_stride);

#ifdef __APPLE__
  q = CL_CHECK_ERR(clCreateCommandQueue(context, device_id, 0, &err));
#else
  const cl_queue_properties props[] = {0};  //CL_QUEUE_PRIORITY_KHR, CL_QUEUE_PRIORITY_HIGH_KHR, 0};
  q = CL_CHECK_ERR(clCreateCommandQueueWithProperties(context, device_id, props, &err));
#endif
}

CameraBuf::~CameraBuf() {
  for (int i = 0; i < frame_buf_count; i++) {
    camera_bufs[i].free();
  }

  rgb_to_yuv_destroy(&rgb_to_yuv_state);

  if (krnl_debayer) {
    CL_CHECK(clReleaseKernel(krnl_debayer));
  }
  CL_CHECK(clReleaseCommandQueue(q));
}

bool CameraBuf::acquire() {
  std::unique_lock<std::mutex> lk(frame_queue_mutex);

  bool got_frame = frame_queue_cv.wait_for(lk, std::chrono::milliseconds(1), [this]{ return !frame_queue.empty(); });
  if (!got_frame) return false;

  cur_buf_idx = frame_queue.front();
  frame_queue.pop();
  lk.unlock();

  const FrameMetadata &frame_data = camera_bufs_metadata[cur_buf_idx];
  if (frame_data.frame_id == -1) {
    LOGE("no frame data? wtf");
    return false;
  }

  cur_frame_data = frame_data;

  cur_rgb_buf = vipc_server->get_buffer(rgb_type);
  cur_rgb_idx = cur_rgb_buf->idx;

  cl_event debayer_event;
  cl_mem camrabuf_cl = camera_bufs[cur_buf_idx].buf_cl;
  if (camera_state->ci.bayer) {
    CL_CHECK(clSetKernelArg(krnl_debayer, 0, sizeof(cl_mem), &camrabuf_cl));
    CL_CHECK(clSetKernelArg(krnl_debayer, 1, sizeof(cl_mem), &cur_rgb_buf->buf_cl));
#ifdef QCOM2
    constexpr int localMemSize = (DEBAYER_LOCAL_WORKSIZE + 2 * (3 / 2)) * (DEBAYER_LOCAL_WORKSIZE + 2 * (3 / 2)) * sizeof(float);
    const size_t globalWorkSize[] = {size_t(camera_state->ci.frame_width), size_t(camera_state->ci.frame_height)};
    const size_t localWorkSize[] = {DEBAYER_LOCAL_WORKSIZE, DEBAYER_LOCAL_WORKSIZE};
    CL_CHECK(clSetKernelArg(krnl_debayer, 2, localMemSize, 0));
    CL_CHECK(clEnqueueNDRangeKernel(q, krnl_debayer, 2, NULL, globalWorkSize, localWorkSize,
                                    0, 0, &debayer_event));
#else
    float digital_gain = camera_state->digital_gain;
    if ((int)digital_gain == 0) {
      digital_gain = 1.0;
    }
    CL_CHECK(clSetKernelArg(krnl_debayer, 2, sizeof(float), &digital_gain));
    const size_t debayer_work_size = rgb_height;  // doesn't divide evenly, is this okay?
    CL_CHECK(clEnqueueNDRangeKernel(q, krnl_debayer, 1, NULL,
                                    &debayer_work_size, NULL, 0, 0, &debayer_event));
#endif
  } else {
    assert(cur_rgb_buf->len >= frame_size);
    assert(rgb_stride == camera_state->ci.frame_stride);
    CL_CHECK(clEnqueueCopyBuffer(q, camrabuf_cl, cur_rgb_buf->buf_cl, 0, 0,
                               cur_rgb_buf->len, 0, 0, &debayer_event));
  }

  clWaitForEvents(1, &debayer_event);
  CL_CHECK(clReleaseEvent(debayer_event));

  cur_yuv_buf = vipc_server->get_buffer(yuv_type);
  cur_yuv_idx = cur_yuv_buf->idx;
  yuv_metas[cur_yuv_idx] = frame_data;
  rgb_to_yuv_queue(&rgb_to_yuv_state, q, cur_rgb_buf->buf_cl, cur_yuv_buf->buf_cl);

  VisionIpcBufExtra extra = {
                        frame_data.frame_id,
                        frame_data.timestamp_sof,
                        frame_data.timestamp_eof,
  };
  vipc_server->send(cur_rgb_buf, &extra);
  vipc_server->send(cur_yuv_buf, &extra);

  return true;
}

void CameraBuf::release() {
  if (release_callback){
    release_callback((void*)camera_state, cur_buf_idx);
  }
}

void CameraBuf::stop() {
}

void CameraBuf::queue(size_t buf_idx){
  {
    std::lock_guard<std::mutex> lk(frame_queue_mutex);
    frame_queue.push(buf_idx);
  }
  frame_queue_cv.notify_one();
}

// common functions

void fill_frame_data(cereal::FrameData::Builder &framed, const FrameMetadata &frame_data, uint32_t cnt) {
  framed.setFrameId(frame_data.frame_id);
  framed.setTimestampEof(frame_data.timestamp_eof);
  framed.setFrameLength(frame_data.frame_length);
  framed.setIntegLines(frame_data.integ_lines);
  framed.setGlobalGain(frame_data.global_gain);
  framed.setLensPos(frame_data.lens_pos);
  framed.setLensSag(frame_data.lens_sag);
  framed.setLensErr(frame_data.lens_err);
  framed.setLensTruePos(frame_data.lens_true_pos);
  framed.setGainFrac(frame_data.gain_frac);
}

void fill_frame_image(cereal::FrameData::Builder &framed, uint8_t *dat, int w, int h, int stride) {
  if (dat != nullptr) {
    int scale = env_scale;
    int x_min = env_xmin; int y_min = env_ymin; int x_max = w-1; int y_max = h-1;
    if (env_xmax != -1) x_max = env_xmax;
    if (env_ymax != -1) y_max = env_ymax;
    int new_width = (x_max - x_min + 1) / scale;
    int new_height = (y_max - y_min + 1) / scale;
    uint8_t *resized_dat = new uint8_t[new_width*new_height*3];

    int goff = x_min*3 + y_min*stride;
    for (int r=0;r<new_height;r++) {
      for (int c=0;c<new_width;c++) {
        memcpy(&resized_dat[(r*new_width+c)*3], &dat[goff+r*stride*scale+c*3*scale], 3*sizeof(uint8_t));
      }
    }
    framed.setImage(kj::arrayPtr((const uint8_t*)resized_dat, new_width*new_height*3));
    delete[] resized_dat;
  }
}

static void create_thumbnail(MultiCameraState *s, const CameraBuf *b) {
  uint8_t* thumbnail_buffer = NULL;
  unsigned long thumbnail_len = 0;

  unsigned char *row = (unsigned char *)malloc(b->rgb_width/4*3);

  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;

  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &thumbnail_buffer, &thumbnail_len);

  cinfo.image_width = b->rgb_width / 4;
  cinfo.image_height = b->rgb_height / 4;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;

  jpeg_set_defaults(&cinfo);
#ifndef __APPLE__
  jpeg_set_quality(&cinfo, 50, true);
  jpeg_start_compress(&cinfo, true);
#else
  jpeg_set_quality(&cinfo, 50, static_cast<boolean>(true) );
  jpeg_start_compress(&cinfo, static_cast<boolean>(true) );
#endif

  JSAMPROW row_pointer[1];
  const uint8_t *bgr_ptr = (const uint8_t *)b->cur_rgb_buf->addr;
  for (int ii = 0; ii < b->rgb_height/4; ii+=1) {
    for (int j = 0; j < b->rgb_width*3; j+=12) {
      for (int k = 0; k < 3; k++) {
        uint16_t dat = 0;
        int i = ii * 4;
        dat += bgr_ptr[b->rgb_stride*i + j + k];
        dat += bgr_ptr[b->rgb_stride*i + j+3 + k];
        dat += bgr_ptr[b->rgb_stride*(i+1) + j + k];
        dat += bgr_ptr[b->rgb_stride*(i+1) + j+3 + k];
        dat += bgr_ptr[b->rgb_stride*(i+2) + j + k];
        dat += bgr_ptr[b->rgb_stride*(i+2) + j+3 + k];
        dat += bgr_ptr[b->rgb_stride*(i+3) + j + k];
        dat += bgr_ptr[b->rgb_stride*(i+3) + j+3 + k];
        row[(j/4) + (2-k)] = dat/8;
      }
    }
    row_pointer[0] = row;
    jpeg_write_scanlines(&cinfo, row_pointer, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  free(row);

  MessageBuilder msg;
  auto thumbnaild = msg.initEvent().initThumbnail();
  thumbnaild.setFrameId(b->cur_frame_data.frame_id);
  thumbnaild.setTimestampEof(b->cur_frame_data.timestamp_eof);
  thumbnaild.setThumbnail(kj::arrayPtr((const uint8_t*)thumbnail_buffer, thumbnail_len));

  if (s->pm != NULL) {
    s->pm->send("thumbnail", msg);
  }
  free(thumbnail_buffer);
}

void set_exposure_target(CameraState *c, const uint8_t *pix_ptr, int x_start, int x_end, int x_skip, int y_start, int y_end, int y_skip) {
  const CameraBuf *b = &c->buf;

  uint32_t lum_binning[256] = {0};
  for (int y = y_start; y < y_end; y += y_skip) {
    for (int x = x_start; x < x_end; x += x_skip) {
      uint8_t lum = pix_ptr[(y * b->rgb_width) + x];
      lum_binning[lum]++;
    }
  }

  unsigned int lum_total = (y_end - y_start) * (x_end - x_start) / x_skip / y_skip;
  unsigned int lum_cur = 0;
  int lum_med = 0;
  int lum_med_alt = 0;
  for (lum_med=255; lum_med>=0; lum_med--) {
    lum_cur += lum_binning[lum_med];
#ifdef QCOM2
    bool reach_hlc_perc = false;
    if (c->camera_num == 0) { // wide
      reach_hlc_perc = lum_cur > 2*lum_total / (3*HLC_A);
    } else {
      reach_hlc_perc = lum_cur > lum_total / HLC_A;
    }
    if (reach_hlc_perc && lum_med > HLC_THRESH) {
      lum_med_alt = 86;
    }
#endif
    if (lum_cur >= lum_total / 2) {
      break;
    }
  }
  lum_med = lum_med_alt>lum_med?lum_med_alt:lum_med;
  camera_autoexposure(c, lum_med / 256.0);
}

extern ExitHandler do_exit;

void *processing_thread(MultiCameraState *cameras, const char *tname,
                          CameraState *cs, process_thread_cb callback) {
  set_thread_name(tname);

  for (int cnt = 0; !do_exit; cnt++) {
    if (!cs->buf.acquire()) continue;

    callback(cameras, cs, cnt);

    if (cs == &(cameras->rear) && cnt % 100 == 3) {
      // this takes 10ms???
      create_thumbnail(cameras, &(cs->buf));
    }
    cs->buf.release();
  }
  return NULL;
}

std::thread start_process_thread(MultiCameraState *cameras, const char *tname,
                                 CameraState *cs, process_thread_cb callback) {
  return std::thread(processing_thread, cameras, tname, cs, callback);
}

void common_camera_process_front(SubMaster *sm, PubMaster *pm, CameraState *c, int cnt) {
  const CameraBuf *b = &c->buf;

  static int x_min = 0, x_max = 0, y_min = 0, y_max = 0;
  static const bool rhd_front = Params().read_db_bool("IsRHD");

  // auto exposure
  if (cnt % 3 == 0) {
    if (sm->update(0) > 0 && sm->updated("driverState")) {
      auto state = (*sm)["driverState"].getDriverState();
      // set front camera metering target
      if (state.getFaceProb() > 0.4) {
        auto face_position = state.getFacePosition();
        int x_offset = rhd_front ? 0 : b->rgb_width - (0.5 * b->rgb_height);
        x_offset += (face_position[0] * (rhd_front ? -1.0 : 1.0) + 0.5) * (0.5 * b->rgb_height);
        const int y_offset = (face_position[1] + 0.5) * b->rgb_height;

        x_min = std::max(0, x_offset - 72);
        x_max = std::min(b->rgb_width - 1, x_offset + 72);
        y_min = std::max(0, y_offset - 72);
        y_max = std::min(b->rgb_height - 1, y_offset + 72);
      } else {  // use default setting if no face
        x_min = x_max = y_min = y_max = 0;
      }
    }

    int skip = 1;
    // use driver face crop for AE
    if (x_max == 0) {
      // default setting
      x_min = rhd_front ? 0 : b->rgb_width * 3 / 5;
      x_max = rhd_front ? b->rgb_width * 2 / 5 : b->rgb_width;
      y_min = b->rgb_height / 3;
      y_max = b->rgb_height;
    }
#ifdef QCOM2
    x_min = 96;
    x_max = 1832;
    y_min = 242;
    y_max = 1148;
    skip = 4;
#endif
    set_exposure_target(c, (const uint8_t *)b->cur_yuv_buf->y, x_min, x_max, 2, y_min, y_max, skip);
  }

  MessageBuilder msg;
  auto framed = msg.initEvent().initFrontFrame();
  framed.setFrameType(cereal::FrameData::FrameType::FRONT);
  fill_frame_data(framed, b->cur_frame_data, cnt);
  if (env_send_front) {
    fill_frame_image(framed, (uint8_t*)b->cur_rgb_buf->addr, b->rgb_width, b->rgb_height, b->rgb_stride);
  }
  pm->send("frontFrame", msg);
}
