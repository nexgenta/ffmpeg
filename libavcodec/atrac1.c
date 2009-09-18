/*
 * Atrac 1 compatible decoder
 * Copyright (c) 2009 Maxim Poliakovski
 * Copyright (c) 2009 Benjamin Larsson
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file libavcodec/atrac1.c
 * Atrac 1 compatible decoder.
 * This decoder handles raw ATRAC1 data.
 */

/* Many thanks to Tim Craig for all the help! */

#include <math.h>
#include <stddef.h>
#include <stdio.h>

#include "avcodec.h"
#include "get_bits.h"
#include "dsputil.h"

#include "atrac.h"
#include "atrac1data.h"

#define AT1_MAX_BFU      52                 ///< max number of block floating units in a sound unit
#define AT1_SU_SIZE      212                ///< number of bytes in a sound unit
#define AT1_SU_SAMPLES   512                ///< number of samples in a sound unit
#define AT1_FRAME_SIZE   AT1_SU_SIZE * 2
#define AT1_SU_MAX_BITS  AT1_SU_SIZE * 8
#define AT1_MAX_CHANNELS 2

#define AT1_QMF_BANDS    3
#define IDX_LOW_BAND     0
#define IDX_MID_BAND     1
#define IDX_HIGH_BAND    2

/**
 * Sound unit struct, one unit is used per channel
 */
typedef struct {
    int                 log2_block_count[AT1_QMF_BANDS];    ///< log2 number of blocks in a band
    int                 num_bfus;                           ///< number of Block Floating Units
    int                 idwls[AT1_MAX_BFU];                 ///< the word length indexes for each BFU
    int                 idsfs[AT1_MAX_BFU];                 ///< the scalefactor indexes for each BFU
    float*              spectrum[2];
    DECLARE_ALIGNED_16(float,spec1[AT1_SU_SAMPLES]);        ///< mdct buffer
    DECLARE_ALIGNED_16(float,spec2[AT1_SU_SAMPLES]);        ///< mdct buffer
    DECLARE_ALIGNED_16(float,fst_qmf_delay[46]);            ///< delay line for the 1st stacked QMF filter
    DECLARE_ALIGNED_16(float,snd_qmf_delay[46]);            ///< delay line for the 2nd stacked QMF filter
    DECLARE_ALIGNED_16(float,last_qmf_delay[256+23]);       ///< delay line for the last stacked QMF filter
} AT1SUCtx;

/**
 * The atrac1 context, holds all needed parameters for decoding
 */
typedef struct {
    AT1SUCtx            SUs[AT1_MAX_CHANNELS];              ///< channel sound unit
    DECLARE_ALIGNED_16(float,spec[AT1_SU_SAMPLES]);         ///< the mdct spectrum buffer
    DECLARE_ALIGNED_16(float,short_buf[512]);               ///< buffer for the short mode

    DECLARE_ALIGNED_16(float, low[256]);
    DECLARE_ALIGNED_16(float, mid[256]);
    DECLARE_ALIGNED_16(float,high[512]);
    float*              bands[3];
    float               out_samples[AT1_MAX_CHANNELS][AT1_SU_SAMPLES];
    MDCTContext         mdct_ctx[3];
    int                 channels;
    DSPContext          dsp;
} AT1Ctx;

DECLARE_ALIGNED_16(static float, short_window[32]);

/** size of the transform in samples in the long mode for each QMF band */
static const uint16_t samples_per_band[3] = {128, 128, 256};
static const uint8_t   mdct_long_nbits[3] = {7, 7, 8};


static void at1_imdct(AT1Ctx *q, float *spec, float *out, int nbits,
                      int rev_spec)
{
    MDCTContext* mdct_context;
    int transf_size = 1 << nbits;

    mdct_context = &q->mdct_ctx[nbits - 5 - (nbits>6)];

    if (rev_spec) {
        int i;
        for (i=0 ; i<transf_size/2 ; i++)
            FFSWAP(float, spec[i], spec[transf_size - 1 - i]);
    }
    ff_imdct_half(mdct_context, out, spec);
}


static int at1_imdct_block(AT1SUCtx* su, AT1Ctx *q)
{
    int             band_num, band_samples, log2_block_count, nbits, num_blocks, block_size;
    unsigned int    start_pos, ref_pos=0, pos = 0;

    for (band_num=0 ; band_num<AT1_QMF_BANDS ; band_num++) {
        band_samples = samples_per_band[band_num];
        log2_block_count = su->log2_block_count[band_num];

        /* number of mdct blocks in the current QMF band: 1 - for long mode */
        /* 4 for short mode(low/middle bands) and 8 for short mode(high band)*/
        num_blocks = 1 << log2_block_count;

        /* mdct block size in samples: 128 (long mode, low & mid bands), */
        /* 256 (long mode, high band) and 32 (short mode, all bands) */
        block_size = band_samples >> log2_block_count;

        /* calc transform size in bits according to the block_size_mode */
        nbits = mdct_long_nbits[band_num] - log2_block_count;

        if (nbits!=5 && nbits!=7 && nbits!=8)
            return -1;

        if (num_blocks == 1) {
            at1_imdct(q, &q->spec[pos], &su->spectrum[0][ref_pos], nbits, band_num);
            pos += block_size; // move to the next mdct block in the spectrum

            /* overlap and window long blocks */
            q->dsp.vector_fmul_window(q->bands[band_num], &su->spectrum[1][ref_pos+band_samples-16],
                &su->spectrum[0][ref_pos], short_window, 0, 16);
            memcpy(q->bands[band_num]+32, &su->spectrum[0][ref_pos+16], 240 * sizeof(float));

        } else {
            /* calc start position for the 1st short block: 96(128) or 112(256) */
            int short_pos = 32;
            float *prev_buf;
            start_pos = (band_samples * (num_blocks - 1)) >> (log2_block_count + 1);
            memset(&su->spectrum[0][ref_pos], 0, sizeof(float) * (band_samples * 2));

            prev_buf = &su->spectrum[1][ref_pos+band_samples-16];
            for (; num_blocks!=0 ; num_blocks--) {
                /* use hardcoded nbits for the short mode */
                at1_imdct(q, &q->spec[pos], &q->short_buf[short_pos], 5, band_num);

                /* overlap and window between short blocks */
                q->dsp.vector_fmul_window(&su->spectrum[0][ref_pos+start_pos],
                                          &q->short_buf[short_pos-16],
                                          &q->short_buf[short_pos],short_window, 0, 16);

                prev_buf = &q->short_buf[short_pos+16];

                start_pos += 32; // use hardcoded block_size
                pos += 32;
                short_pos +=32;
            }
            memcpy(q->bands[band_num], &su->spectrum[0][ref_pos], band_samples*sizeof(float));
        }
        ref_pos += band_samples;
    }

    /* Swap buffers so the mdct overlap works */
    FFSWAP(float*, su->spectrum[0], su->spectrum[1]);

    return 0;
}

/**
 * Parse the block size mode byte
 */

static int at1_parse_bsm(GetBitContext* gb, int log2_block_cnt[AT1_QMF_BANDS])
{
    int log2_block_count_tmp, i;

    for(i=0 ; i<2 ; i++) {
        /* low and mid band */
        log2_block_count_tmp = get_bits(gb, 2);
        if (log2_block_count_tmp & 1)
            return -1;
        log2_block_cnt[i] = 2 - log2_block_count_tmp;
    }

    /* high band */
    log2_block_count_tmp = get_bits(gb, 2);
    if (log2_block_count_tmp != 0 && log2_block_count_tmp != 3)
        return -1;
    log2_block_cnt[IDX_HIGH_BAND] = 3 - log2_block_count_tmp;

    skip_bits(gb, 2);
    return 0;
}


static int at1_unpack_dequant(GetBitContext* gb, AT1SUCtx* su,
                              float spec[AT1_SU_SAMPLES])
{
    int bits_used, band_num, bfu_num, i;

    /* parse the info byte (2nd byte) telling how much BFUs were coded */
    su->num_bfus = bfu_amount_tab1[get_bits(gb, 3)];

    /* calc number of consumed bits:
        num_BFUs * (idwl(4bits) + idsf(6bits)) + log2_block_count(8bits) + info_byte(8bits)
        + info_byte_copy(8bits) + log2_block_count_copy(8bits) */
    bits_used = su->num_bfus * 10 + 32 +
                bfu_amount_tab2[get_bits(gb, 2)] +
                (bfu_amount_tab3[get_bits(gb, 3)] << 1);

    /* get word length index (idwl) for each BFU */
    for (i=0 ; i<su->num_bfus ; i++)
        su->idwls[i] = get_bits(gb, 4);

    /* get scalefactor index (idsf) for each BFU */
    for (i=0 ; i<su->num_bfus ; i++)
        su->idsfs[i] = get_bits(gb, 6);

    /* zero idwl/idsf for empty BFUs */
    for (i = su->num_bfus; i < AT1_MAX_BFU; i++)
        su->idwls[i] = su->idsfs[i] = 0;

    /* read in the spectral data and reconstruct MDCT spectrum of this channel */
    for (band_num=0 ; band_num<AT1_QMF_BANDS ; band_num++) {
        for (bfu_num=bfu_bands_t[band_num] ; bfu_num<bfu_bands_t[band_num+1] ; bfu_num++) {
            int pos;

            int num_specs = specs_per_bfu[bfu_num];
            int word_len  = !!su->idwls[bfu_num] + su->idwls[bfu_num];
            float scale_factor = sf_table[su->idsfs[bfu_num]];
            bits_used    += word_len * num_specs; /* add number of bits consumed by current BFU */

            /* check for bitstream overflow */
            if (bits_used > AT1_SU_MAX_BITS)
                return -1;

            /* get the position of the 1st spec according to the block size mode */
            pos = su->log2_block_count[band_num] ? bfu_start_short[bfu_num] : bfu_start_long[bfu_num];

            if (word_len) {
                float   max_quant = 1.0 / (float)((1 << (word_len - 1)) - 1);

                for (i=0 ; i<num_specs ; i++) {
                    /* read in a quantized spec and convert it to
                     * signed int and then inverse quantization
                     */
                    spec[pos+i] = get_sbits(gb, word_len) * scale_factor * max_quant;
                }
            } else { /* word_len = 0 -> empty BFU, zero all specs in the emty BFU */
                memset(&spec[pos], 0, num_specs*sizeof(float));
            }
        }
    }

    return 0;
}


void at1_subband_synthesis(AT1Ctx *q, AT1SUCtx* su, float *pOut)
{
    float   temp[256];
    float   iqmf_temp[512 + 46];

    /* combine low and middle bands */
    atrac_iqmf(q->bands[0], q->bands[1], 128, temp, su->fst_qmf_delay, iqmf_temp);

    /* delay the signal of the high band by 23 samples */
    memcpy( su->last_qmf_delay,    &su->last_qmf_delay[256], sizeof(float)*23);
    memcpy(&su->last_qmf_delay[23], q->bands[2],             sizeof(float)*256);

    /* combine (low + middle) and high bands */
    atrac_iqmf(temp, su->last_qmf_delay, 256, pOut, su->snd_qmf_delay, iqmf_temp);
}


static int atrac1_decode_frame(AVCodecContext *avctx, void *data,
                               int *data_size, AVPacket *avpkt)
{
    const uint8_t *buf = avpkt->data;
    int buf_size       = avpkt->size;
    AT1Ctx *q          = avctx->priv_data;
    int ch, ret, i;
    GetBitContext gb;
    float* samples = data;


    if (buf_size < 212 * q->channels) {
        av_log(q,AV_LOG_ERROR,"Not enought data to decode!\n");
        return -1;
    }

    for (ch=0 ; ch<q->channels ; ch++) {
        AT1SUCtx* su = &q->SUs[ch];

        init_get_bits(&gb, &buf[212*ch], 212*8);

        /* parse block_size_mode, 1st byte */
        ret = at1_parse_bsm(&gb, su->log2_block_count);
        if (ret < 0)
            return ret;

        ret = at1_unpack_dequant(&gb, su, q->spec);
        if (ret < 0)
            return ret;

        ret = at1_imdct_block(su, q);
        if (ret < 0)
            return ret;
        at1_subband_synthesis(q, su, q->out_samples[ch]);
    }

    /* round, convert to 16bit and interleave */
    if (q->channels == 1) {
        /* mono */
        q->dsp.vector_clipf(samples, q->out_samples[0], -32700.0 / (1<<15),
                            32700.0 / (1<<15), AT1_SU_SAMPLES);
    } else {
        /* stereo */
        for (i = 0; i < AT1_SU_SAMPLES; i++) {
            samples[i*2]   = av_clipf(q->out_samples[0][i], -32700.0 / (1<<15),
                                      32700.0 / (1<<15));
            samples[i*2+1] = av_clipf(q->out_samples[1][i], -32700.0 / (1<<15),
                                      32700.0 / (1<<15));
        }
    }

    *data_size = q->channels * AT1_SU_SAMPLES * sizeof(*samples);
    return avctx->block_align;
}


static av_cold int atrac1_decode_init(AVCodecContext *avctx)
{
    AT1Ctx *q = avctx->priv_data;

    avctx->sample_fmt = SAMPLE_FMT_FLT;

    q->channels = avctx->channels;

    /* Init the mdct transforms */
    ff_mdct_init(&q->mdct_ctx[0], 6, 1, -1.0/ (1<<15));
    ff_mdct_init(&q->mdct_ctx[1], 8, 1, -1.0/ (1<<15));
    ff_mdct_init(&q->mdct_ctx[2], 9, 1, -1.0/ (1<<15));

    ff_sine_window_init(short_window, 32);

    atrac_generate_tables();

    dsputil_init(&q->dsp, avctx);

    q->bands[0] = q->low;
    q->bands[1] = q->mid;
    q->bands[2] = q->high;

    /* Prepare the mdct overlap buffers */
    q->SUs[0].spectrum[0] = q->SUs[0].spec1;
    q->SUs[0].spectrum[1] = q->SUs[0].spec2;
    q->SUs[1].spectrum[0] = q->SUs[1].spec1;
    q->SUs[1].spectrum[1] = q->SUs[1].spec2;

    return 0;
}

AVCodec atrac1_decoder = {
    .name = "atrac1",
    .type = CODEC_TYPE_AUDIO,
    .id = CODEC_ID_ATRAC1,
    .priv_data_size = sizeof(AT1Ctx),
    .init = atrac1_decode_init,
    .close = NULL,
    .decode = atrac1_decode_frame,
    .long_name = NULL_IF_CONFIG_SMALL("Atrac 1 (Adaptive TRansform Acoustic Coding)"),
};