#ifndef ST_VORBIS_ENC_H
#define ST_VORBIS_ENC_H

/* Vorbis encoding for the soundtester ALSA plugin. Compiled in only under -DST_VORBIS=ON (the
 * default); without it the plugin has no libvorbis dependency at all. */

#include <ogg/ogg.h>
#include <string.h>
#include <vorbis/codec.h>
#include <vorbis/vorbisenc.h>

typedef struct {
  vorbis_info vi;
  vorbis_comment vc;
  vorbis_dsp_state vd;
  vorbis_block vb;
  int have_info, have_dsp, have_block, ready;
} st_vorbis_enc;

static inline void st_vorbis_stop(st_vorbis_enc *v) {
  if (v->have_block) vorbis_block_clear(&v->vb);
  if (v->have_dsp) vorbis_dsp_clear(&v->vd);
  if (v->have_info) {
    vorbis_comment_clear(&v->vc);
    vorbis_info_clear(&v->vi);
  }
  memset(v, 0, sizeof(*v));
}

/* quality is Vorbis's own VBR scale, -0.1 .. 1.0. Returns NULL on success, or a message. */
static inline const char *st_vorbis_start(st_vorbis_enc *v, int channels, long rate,
                                          float quality) {
  st_vorbis_stop(v);
  vorbis_info_init(&v->vi);
  v->have_info = 1;
  vorbis_comment_init(&v->vc);
  if (vorbis_encode_init_vbr(&v->vi, channels, rate, quality) != 0) {
    st_vorbis_stop(v);
    return "libvorbis refused this rate/channel/quality combination";
  }
  if (vorbis_analysis_init(&v->vd, &v->vi) != 0) {
    st_vorbis_stop(v);
    return "libvorbis would not start its analysis";
  }
  v->have_dsp = 1;
  vorbis_block_init(&v->vd, &v->vb);
  v->have_block = 1;
  v->ready = 1;
  return NULL;
}

#endif /* ST_VORBIS_ENC_H */
