/*
 * Copyright (C) 2020 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2020 Vladimir Sadovnikov <sadko4u@gmail.com>
 *
 * This file is part of lsp-plugins
 * Created on: 18 мая 2020 г.
 *
 * lsp-plugins is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * lsp-plugins is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with lsp-plugins. If not, see <https://www.gnu.org/licenses/>.
 */

#include <core/debug.h>
#include <core/colors.h>
#include <core/util/Color.h>
#include <plugins/mb_gate.h>

#define MBG_BUFFER_SIZE         0x1000
#define TRACE_PORT(p)           lsp_trace("  port id=%s", (p)->metadata()->id);

namespace lsp
{
    mb_gate_base::mb_gate_base(const plugin_metadata_t &metadata, bool sc, size_t mode):
        plugin_t(metadata)
    {
        nMode           = mode;
        bSidechain      = sc;
        bEnvUpdate      = true;
        bModern         = true;
        nEnvBoost       = mb_gate_base_metadata::FB_DEFAULT;
        vChannels       = NULL;
        fInGain         = GAIN_AMP_0_DB;
        fDryGain        = GAIN_AMP_M_INF_DB;
        fWetGain        = GAIN_AMP_0_DB;
        fZoom           = GAIN_AMP_0_DB;
        pData           = NULL;
        vTr             = NULL;
        vPFc             = NULL;
        vRFc            = NULL;
        vFreqs          = NULL;
        vCurve          = NULL;
        vIndexes        = NULL;
        pIDisplay       = NULL;
        vSc[0]          = NULL;
        vSc[1]          = NULL;
        vAnalyze[0]     = NULL;
        vAnalyze[1]     = NULL;
        vAnalyze[2]     = NULL;
        vAnalyze[3]     = NULL;
        vBuffer         = NULL;
        vEnv            = NULL;

        pBypass         = NULL;
        pMode           = NULL;
        pInGain         = NULL;
        pDryGain        = NULL;
        pWetGain        = NULL;
        pOutGain        = NULL;
        pReactivity     = NULL;
        pShiftGain      = NULL;
        pZoom           = NULL;
        pEnvBoost       = NULL;
    }

    mb_gate_base::~mb_gate_base()
    {
    }

    bool mb_gate_base::compare_bands_for_sort(const gate_band_t *b1, const gate_band_t *b2)
    {
        if (b1->fFreqStart != b2->fFreqStart)
            return (b1->fFreqStart > b2->fFreqStart);
        return b1 < b2;
    }

    void mb_gate_base::init(IWrapper *wrapper)
    {
        // Initialize plugin
        plugin_t::init(wrapper);

        // Determine number of channels
        size_t channels     = (nMode == MBGM_MONO) ? 1 : 2;

        // Allocate channels
        vChannels       = new channel_t[channels];
        if (vChannels == NULL)
            return;

        // Initialize analyzer
        size_t an_cid       = 0;
        if (!sAnalyzer.init(2*channels, mb_gate_base_metadata::FFT_RANK,
                            MAX_SAMPLE_RATE, mb_gate_base_metadata::FFT_REFRESH_RATE))
            return;

        sAnalyzer.set_rank(mb_gate_base_metadata::FFT_RANK);
        sAnalyzer.set_activity(false);
        sAnalyzer.set_envelope(envelope::WHITE_NOISE);
        sAnalyzer.set_window(mb_gate_base_metadata::FFT_WINDOW);
        sAnalyzer.set_rate(mb_gate_base_metadata::FFT_REFRESH_RATE);

        size_t filter_mesh_size = ALIGN_SIZE(mb_gate_base_metadata::FFT_MESH_POINTS * sizeof(float), DEFAULT_ALIGN);

        // Allocate float buffer data
        size_t to_alloc =
                // Global buffers
                2 * filter_mesh_size + // vTr (both complex and real)
                2 * filter_mesh_size + // vFc (both complex and real)
                2 * filter_mesh_size + // vSig (both complex and real)
                mb_gate_base_metadata::CURVE_MESH_SIZE * sizeof(float) + // Curve
                mb_gate_base_metadata::FFT_MESH_POINTS * sizeof(float) + // vFreqs array
                mb_gate_base_metadata::FFT_MESH_POINTS * sizeof(uint32_t) + // vIndexes array
                MBG_BUFFER_SIZE * sizeof(float) + // Global vBuffer for band signal processing
                MBG_BUFFER_SIZE * sizeof(float) + // Global vEnv for band signal processing
                MBG_BUFFER_SIZE * sizeof(float) * 2 + // vInAnalyze + vOutAnalyze for each channel
                // Channel buffers
                (
                    MBG_BUFFER_SIZE * sizeof(float) + // Global vSc[] for each channel
                    2 * filter_mesh_size + // vTr of each channel
                    filter_mesh_size + // vTrMem of each channel
                    MBG_BUFFER_SIZE * sizeof(float) + // vInBuffer for each channel
                    MBG_BUFFER_SIZE * sizeof(float) + // vBuffer for each channel
                    MBG_BUFFER_SIZE * sizeof(float) + // vScBuffer for each channel
                    ((bSidechain) ? MBG_BUFFER_SIZE * sizeof(float) : 0) + // vExtScBuffer for each channel
                    // Band buffers
                    (
                        MBG_BUFFER_SIZE * sizeof(float) + // vVCA of each band
                        mb_gate_base_metadata::FFT_MESH_POINTS * 2 * sizeof(float) // vTr transfer function for each band
                    ) * mb_gate_base_metadata::BANDS_MAX
                ) * channels;

        uint8_t *ptr    = alloc_aligned<uint8_t>(pData, to_alloc);
        if (ptr == NULL)
            return;
        lsp_guard_assert(uint8_t *save   = ptr);

        // Remember the pointer to frequencies buffer
        vTr             = reinterpret_cast<float *>(ptr);
        ptr            += filter_mesh_size * 2;
        vPFc             = reinterpret_cast<float *>(ptr);
        ptr            += filter_mesh_size * 2;
        vRFc            = reinterpret_cast<float *>(ptr);
        ptr            += filter_mesh_size * 2;
        vFreqs          = reinterpret_cast<float *>(ptr);
        ptr            += mb_gate_base_metadata::FFT_MESH_POINTS * sizeof(float);
        vCurve          = reinterpret_cast<float *>(ptr);
        ptr            += mb_gate_base_metadata::CURVE_MESH_SIZE * sizeof(float);
        vIndexes        = reinterpret_cast<uint32_t *>(ptr);
        ptr            += mb_gate_base_metadata::FFT_MESH_POINTS * sizeof(uint32_t);
        vSc[0]          = reinterpret_cast<float *>(ptr);
        ptr            += MBG_BUFFER_SIZE * sizeof(float);
        if (channels > 1)
        {
            vSc[1]          = reinterpret_cast<float *>(ptr);
            ptr            += MBG_BUFFER_SIZE * sizeof(float);
        }
        else
            vSc[1]          = NULL;
        vBuffer         = reinterpret_cast<float *>(ptr);
        ptr            += MBG_BUFFER_SIZE * sizeof(float);
        vEnv            = reinterpret_cast<float *>(ptr);
        ptr            += MBG_BUFFER_SIZE * sizeof(float);

        // Initialize filters according to number of bands
        if (sFilters.init(mb_gate_base_metadata::BANDS_MAX * channels) != STATUS_OK)
            return;
        size_t filter_cid = 0;

        // Initialize channels
        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c    = &vChannels[i];

            if (!c->sEnvBoost[0].init(NULL))
                return;
            if (bSidechain)
            {
                if (!c->sEnvBoost[1].init(NULL))
                    return;
            }

            c->nPlanSize    = 0;
            c->vIn          = NULL;
            c->vOut         = NULL;
            c->vScIn        = NULL;

            c->vInBuffer    = reinterpret_cast<float *>(ptr);
            ptr            += MBG_BUFFER_SIZE * sizeof(float);
            c->vBuffer      = reinterpret_cast<float *>(ptr);
            ptr            += MBG_BUFFER_SIZE * sizeof(float);
            c->vScBuffer    = reinterpret_cast<float *>(ptr);
            ptr            += MBG_BUFFER_SIZE * sizeof(float);
            c->vExtScBuffer = NULL;
            if (bSidechain)
            {
                c->vExtScBuffer = reinterpret_cast<float *>(ptr);
                ptr            += MBG_BUFFER_SIZE * sizeof(float);
            }
            c->vTr          = reinterpret_cast<float *>(ptr);
            ptr            += 2 * filter_mesh_size;
            c->vTrMem       = reinterpret_cast<float *>(ptr);
            ptr            += filter_mesh_size;
            c->vInAnalyze   = reinterpret_cast<float *>(ptr);
            ptr            += MBG_BUFFER_SIZE * sizeof(float);
            c->vOutAnalyze  = reinterpret_cast<float *>(ptr);
            ptr            += MBG_BUFFER_SIZE * sizeof(float);

            c->nAnInChannel = an_cid++;
            c->nAnOutChannel= an_cid++;
            vAnalyze[c->nAnInChannel]   = c->vInAnalyze;
            vAnalyze[c->nAnOutChannel]  = c->vOutAnalyze;
            c->bInFft       = false;
            c->bOutFft      = false;

            c->pIn          = NULL;
            c->pOut         = NULL;
            c->pScIn        = NULL;
            c->pFftIn       = NULL;
            c->pFftInSw     = NULL;
            c->pFftOut      = NULL;
            c->pFftOutSw    = NULL;

            c->pAmpGraph    = NULL;
            c->pInLvl       = NULL;
            c->pOutLvl      = NULL;

            // Initialize bands
            for (size_t j=0; j<mb_gate_base_metadata::BANDS_MAX; ++j)
            {
                gate_band_t *b      = &c->vBands[j];

                if (!b->sSC.init(channels, mb_gate_base_metadata::REACTIVITY_MAX))
                    return;
                if (!b->sPassFilter.init(NULL))
                    return;
                if (!b->sRejFilter.init(NULL))
                    return;
                if (!b->sAllFilter.init(NULL))
                    return;

                // Initialize sidechain equalizers
                b->sEQ[0].init(2, 6);
                b->sEQ[0].set_mode(EQM_IIR);
                if (channels > 1)
                {
                    b->sEQ[1].init(2, 6);
                    b->sEQ[1].set_mode(EQM_IIR);
                }

                b->vVCA             = reinterpret_cast<float *>(ptr);
                ptr                += MBG_BUFFER_SIZE * sizeof(float);
                b->vTr              = reinterpret_cast<float *>(ptr);
                ptr                += mb_gate_base_metadata::FFT_MESH_POINTS * sizeof(float) * 2;

                b->fScPreamp        = GAIN_AMP_0_DB;

                b->fFreqStart       = 0.0f;
                b->fFreqEnd         = 0.0f;

                b->fFreqHCF         = 0.0f;
                b->fFreqLCF         = 0.0f;
                b->fMakeup          = GAIN_AMP_0_DB;
                b->fEnvLevel        = GAIN_AMP_0_DB;
                b->fGainLevel       = GAIN_AMP_0_DB;
                b->bEnabled         = j < mb_gate_base_metadata::BANDS_DFL;
                b->bCustHCF         = false;
                b->bCustLCF         = false;
                b->bMute            = false;
                b->bSolo            = false;
                b->bExtSc           = false;
                b->nFilterID        = filter_cid++;

                b->pExtSc           = NULL;
                b->pScSource        = NULL;
                b->pScMode          = NULL;
                b->pScLook          = NULL;
                b->pScReact         = NULL;
                b->pScPreamp        = NULL;
                b->pScLpfOn         = NULL;
                b->pScHpfOn         = NULL;
                b->pScLcfFreq       = NULL;
                b->pScHcfFreq       = NULL;
                b->pScFreqChart     = NULL;

                b->pEnable          = NULL;
                b->pSolo            = NULL;
                b->pMute            = NULL;
                b->pHyst            = NULL;
                b->pThresh[0]       = NULL;
                b->pThresh[1]       = NULL;
                b->pZone[0]         = NULL;
                b->pZone[1]         = NULL;
                b->pAttack          = NULL;
                b->pRelease         = NULL;
                b->pReduction       = NULL;
                b->pMakeup          = NULL;

                b->pFreqEnd         = NULL;
                b->pCurveGraph[0]   = NULL;
                b->pCurveGraph[1]   = NULL;
                b->pEnvLvl          = NULL;
                b->pCurveLvl        = NULL;
                b->pMeterGain       = NULL;
            }

            // Initialize split
            for (size_t j=0; j<mb_gate_base_metadata::BANDS_MAX-1; ++j)
            {
                split_t *s      = &c->vSplit[j];

                s->bEnabled     = false;
                s->fFreq        = 0.0f;

                s->pEnabled     = NULL;
                s->pFreq        = NULL;
            }
        }

        lsp_assert(ptr <= &save[to_alloc]);

        // Bind ports
        size_t port_id              = 0;

        // Input ports
        lsp_trace("Binding input ports");
        for (size_t i=0; i<channels; ++i)
        {
            TRACE_PORT(vPorts[port_id]);
            vChannels[i].pIn        =   vPorts[port_id++];
        }

        // Input ports
        lsp_trace("Binding output ports");
        for (size_t i=0; i<channels; ++i)
        {
            TRACE_PORT(vPorts[port_id]);
            vChannels[i].pOut       =   vPorts[port_id++];
        }

        // Input ports
        if (bSidechain)
        {
            lsp_trace("Binding sidechain ports");
            for (size_t i=0; i<channels; ++i)
            {
                TRACE_PORT(vPorts[port_id]);
                vChannels[i].pScIn      =   vPorts[port_id++];
            }
        }

        // Common ports
        lsp_trace("Binding common ports");
        TRACE_PORT(vPorts[port_id]);
        pBypass                 = vPorts[port_id++];
        TRACE_PORT(vPorts[port_id]);
        pMode                   = vPorts[port_id++];
        TRACE_PORT(vPorts[port_id]);
        pInGain                 = vPorts[port_id++];
        TRACE_PORT(vPorts[port_id]);
        pOutGain                = vPorts[port_id++];
        TRACE_PORT(vPorts[port_id]);
        pDryGain                = vPorts[port_id++];
        TRACE_PORT(vPorts[port_id]);
        pWetGain                = vPorts[port_id++];
        TRACE_PORT(vPorts[port_id]);
        pReactivity             = vPorts[port_id++];
        TRACE_PORT(vPorts[port_id]);
        pShiftGain              = vPorts[port_id++];
        TRACE_PORT(vPorts[port_id]);
        pZoom                   = vPorts[port_id++];
        TRACE_PORT(vPorts[port_id]);
        pEnvBoost               = vPorts[port_id++];
        TRACE_PORT(vPorts[port_id]);
        port_id++;         // Skip band selector

        lsp_trace("Binding channel ports");
        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c    = &vChannels[i];

            if ((i > 0) && (nMode == MBGM_STEREO))
            {
                channel_t *sc           = &vChannels[0];
                c->pAmpGraph            = sc->pAmpGraph;
            }
            else
            {
                TRACE_PORT(vPorts[port_id]);
                port_id++;         // Skip filter switch
                TRACE_PORT(vPorts[port_id]);
                c->pAmpGraph            = vPorts[port_id++];
            }
        }

        lsp_trace("Binding meters");
        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c    = &vChannels[i];

            TRACE_PORT(vPorts[port_id]);
            c->pFftInSw             = vPorts[port_id++];
            TRACE_PORT(vPorts[port_id]);
            c->pFftOutSw            = vPorts[port_id++];
            TRACE_PORT(vPorts[port_id]);
            c->pFftIn               = vPorts[port_id++];
            TRACE_PORT(vPorts[port_id]);
            c->pFftOut              = vPorts[port_id++];
            TRACE_PORT(vPorts[port_id]);
            c->pInLvl               = vPorts[port_id++];
            TRACE_PORT(vPorts[port_id]);
            c->pOutLvl              = vPorts[port_id++];
        }

        // Split frequencies
        lsp_trace("Binding split frequencies");
        for (size_t i=0; i<channels; ++i)
        {
            for (size_t j=0; j<mb_gate_base_metadata::BANDS_MAX-1; ++j)
            {
                split_t *s      = &vChannels[i].vSplit[j];

                if ((i > 0) && (nMode == MBGM_STEREO))
                {
                    split_t *sc     = &vChannels[0].vSplit[j];
                    s->pEnabled     = sc->pEnabled;
                    s->pFreq        = sc->pFreq;
                }
                else
                {
                    TRACE_PORT(vPorts[port_id]);
                    s->pEnabled     = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    s->pFreq        = vPorts[port_id++];
                }
            }
        }

        // Expander bands
        lsp_trace("Binding gate bands");
        for (size_t i=0; i<channels; ++i)
        {
            for (size_t j=0; j<mb_gate_base_metadata::BANDS_MAX; ++j)
            {
                gate_band_t *b   = &vChannels[i].vBands[j];

                if ((i > 0) && (nMode == MBGM_STEREO))
                {
                    gate_band_t *sb     = &vChannels[0].vBands[j];

                    b->pExtSc           = sb->pExtSc;
                    b->pScSource        = sb->pScSource;
                    b->pScMode          = sb->pScMode;
                    b->pScLook          = sb->pScLook;
                    b->pScReact         = sb->pScReact;
                    b->pScPreamp        = sb->pScPreamp;
                    b->pScLpfOn         = sb->pScLpfOn;
                    b->pScHpfOn         = sb->pScHpfOn;
                    b->pScLcfFreq       = sb->pScLcfFreq;
                    b->pScHcfFreq       = sb->pScHcfFreq;
                    b->pScFreqChart     = sb->pScFreqChart;

                    b->pEnable          = sb->pEnable;
                    b->pSolo            = sb->pSolo;
                    b->pMute            = sb->pMute;
                    b->pHyst            = sb->pHyst;
                    b->pThresh[0]       = sb->pThresh[0];
                    b->pThresh[1]       = sb->pThresh[1];
                    b->pZone[0]         = sb->pZone[0];
                    b->pZone[1]         = sb->pZone[1];
                    b->pAttack          = sb->pAttack;
                    b->pRelease         = sb->pRelease;
                    b->pReduction       = sb->pReduction;
                    b->pMakeup          = sb->pMakeup;

                    b->pFreqEnd         = sb->pFreqEnd;
                    b->pCurveGraph[0]   = sb->pCurveGraph[0];
                    b->pCurveGraph[1]   = sb->pCurveGraph[1];
                    b->pEnvLvl          = sb->pEnvLvl;
                    b->pCurveLvl        = sb->pCurveLvl;
                    b->pMeterGain       = sb->pMeterGain;
                }
                else
                {
                    if (bSidechain)
                    {
                        TRACE_PORT(vPorts[port_id]);
                        b->pExtSc           = vPorts[port_id++];
                    }
                    if (nMode != MBGM_MONO)
                    {
                        TRACE_PORT(vPorts[port_id]);
                        b->pScSource        = vPorts[port_id++];
                    }
                    TRACE_PORT(vPorts[port_id]);
                    b->pScMode          = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pScLook          = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pScReact         = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pScPreamp        = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pScLpfOn         = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pScHpfOn         = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pScLcfFreq       = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pScHcfFreq       = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pScFreqChart     = vPorts[port_id++];

                    TRACE_PORT(vPorts[port_id]);
                    b->pEnable          = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pSolo            = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pMute            = vPorts[port_id++];

                    TRACE_PORT(vPorts[port_id]);
                    b->pHyst            = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pThresh[0]       = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pZone[0]         = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pThresh[1]       = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pZone[1]         = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pAttack          = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pRelease         = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pReduction       = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pMakeup          = vPorts[port_id++];

                    // Skip hue
                    TRACE_PORT(vPorts[port_id]);
                    port_id ++;

                    TRACE_PORT(vPorts[port_id]);
                    b->pFreqEnd         = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pCurveGraph[0]   = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pCurveGraph[1]   = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pEnvLvl          = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pCurveLvl        = vPorts[port_id++];
                    TRACE_PORT(vPorts[port_id]);
                    b->pMeterGain       = vPorts[port_id++];
                }
            }
        }

        // Initialize curve (logarithmic) in range of -72 .. +24 db
        float delta = (mb_gate_base_metadata::CURVE_DB_MAX - mb_gate_base_metadata::CURVE_DB_MIN) / (mb_gate_base_metadata::CURVE_MESH_SIZE-1);
        for (size_t i=0; i<mb_gate_base_metadata::CURVE_MESH_SIZE; ++i)
            vCurve[i]   = db_to_gain(mb_gate_base_metadata::CURVE_DB_MIN + delta * i);
    }

    void mb_gate_base::destroy()
    {
        // Determine number of channels
        size_t channels     = (nMode == MBGM_MONO) ? 1 : 2;

        // Destroy channels
        if (vChannels != NULL)
        {
            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c    = &vChannels[i];

                c->sEnvBoost[0].destroy();
                c->sEnvBoost[1].destroy();
                c->sDelay.destroy();

                c->vBuffer      = NULL;

                for (size_t i=0; i<mb_gate_base_metadata::BANDS_MAX; ++i)
                {
                    gate_band_t *b  = &c->vBands[i];

                    b->sEQ[0].destroy();
                    b->sEQ[1].destroy();
                    b->sSC.destroy();
                    b->sDelay.destroy();

                    b->sPassFilter.destroy();
                    b->sRejFilter.destroy();
                    b->sAllFilter.destroy();
                }
            }

            delete [] vChannels;
            vChannels       = NULL;
        }

        // Destroy dynamic filters
        sFilters.destroy();

        // Destroy data
        if (pData != NULL)
            free_aligned(pData);

        if (pIDisplay != NULL)
        {
            pIDisplay->detroy();
            pIDisplay   = NULL;
        }

        // Destroy analyzer
        sAnalyzer.destroy();

        // Destroy plugin
        plugin_t::destroy();
    }

    void mb_gate_base::update_settings()
    {
        filter_params_t fp;

        // Determine number of channels
        size_t channels     = (nMode == MBGM_MONO) ? 1 : 2;
        int active_channels = 0;
        size_t env_boost    = pEnvBoost->getValue();

        // Determine work mode: classic or modern
        bool modern         = pMode->getValue() >= 0.5f;
        if (modern != bModern)
        {
            bModern             = modern;
            for (size_t i=0; i<channels; ++i)
                vChannels[i].nPlanSize      = 0;
        }

        // Store gain
        float out_gain      = pOutGain->getValue();
        fInGain             = pInGain->getValue();
        fDryGain            = out_gain * pDryGain->getValue();
        fWetGain            = out_gain * pWetGain->getValue();
        fZoom               = pZoom->getValue();

        // Configure channels
        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c    = &vChannels[i];

            // Update bypass settings
            c->sBypass.set_bypass(pBypass->getValue());

            // Update frequency split bands
            for (size_t j=0; j<mb_gate_base_metadata::BANDS_MAX-1; ++j)
            {
                split_t *s      = &c->vSplit[j];

                bool enabled    = s->bEnabled;
                s->bEnabled     = s->pEnabled->getValue() >= 0.5f;
                if (enabled != s->bEnabled)
                    c->nPlanSize    = 0;

                float v         = s->fFreq;
                s->fFreq        = s->pFreq->getValue();
                if (v != s->fFreq)
                    c->nPlanSize    = 0;
            }

            // Update analyzer settings
            c->bInFft       = c->pFftInSw->getValue() >= 0.5f;
            c->bOutFft      = c->pFftOutSw->getValue() >= 0.5f;

            sAnalyzer.enable_channel(c->nAnInChannel, c->bInFft);
            sAnalyzer.enable_channel(c->nAnOutChannel, c->pFftOutSw->getValue()  >= 0.5f);

            if (sAnalyzer.channel_active(c->nAnInChannel))
                active_channels ++;
            if (sAnalyzer.channel_active(c->nAnOutChannel))
                active_channels ++;

            // Update envelope boost filters
            if ((env_boost != nEnvBoost) || (bEnvUpdate))
            {
                fp.fFreq        = mb_gate_base_metadata::FREQ_BOOST_MIN;
                fp.fFreq2       = 0.0f;
                fp.fGain        = 1.0f;
                fp.fQuality     = 0.0f;

                switch (env_boost)
                {
                    case mb_gate_base_metadata::FB_BT_3DB:
                        fp.nType        = FLT_BT_RLC_ENVELOPE;
                        fp.nSlope       = 1;
                        break;
                    case mb_gate_base_metadata::FB_MT_3DB:
                        fp.nType        = FLT_MT_RLC_ENVELOPE;
                        fp.nSlope       = 1;
                        break;
                    case mb_gate_base_metadata::FB_BT_6DB:
                        fp.nType        = FLT_BT_RLC_ENVELOPE;
                        fp.nSlope       = 2;
                        break;
                    case mb_gate_base_metadata::FB_MT_6DB:
                        fp.nType        = FLT_MT_RLC_ENVELOPE;
                        fp.nSlope       = 2;
                        break;
                    case mb_gate_base_metadata::FB_OFF:
                    default:
                        fp.nType        = FLT_NONE;
                        fp.nSlope       = 1;
                        break;
                }

                c->sEnvBoost[0].update(fSampleRate, &fp);
                if (bSidechain)
                    c->sEnvBoost[1].update(fSampleRate, &fp);
            }
        }

        // Update analyzer parameters
        sAnalyzer.set_reactivity(pReactivity->getValue());
        if (pShiftGain != NULL)
            sAnalyzer.set_shift(pShiftGain->getValue() * 100.0f);
        sAnalyzer.set_activity(active_channels > 0);

        // Update analyzer
        if (sAnalyzer.needs_reconfiguration())
        {
            sAnalyzer.reconfigure();
            sAnalyzer.get_frequencies(vFreqs, vIndexes, SPEC_FREQ_MIN, SPEC_FREQ_MAX, mb_gate_base_metadata::MESH_POINTS);
        }

        size_t latency = 0;
        bool solo_on = false;

        // Configure channels
        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c    = &vChannels[i];

            // Update gate bands
            for (size_t j=0; j<mb_gate_base_metadata::BANDS_MAX; ++j)
            {
                gate_band_t *b   = &c->vBands[j];

                bool enabled    = b->pEnable->getValue() >= 0.5f;
                if (enabled && (j > 0))
                    enabled         = c->vSplit[j-1].bEnabled;
                bool cust_lcf   = b->pScLpfOn->getValue() >= 0.5f;
                bool cust_hcf   = b->pScHpfOn->getValue() >= 0.5f;
                float sc_gain   = b->pScPreamp->getValue();
                bool mute       = (b->pMute->getValue() >= 0.5f);
                bool solo       = (enabled) && (b->pSolo->getValue() >= 0.5f);

                b->bExtSc       = (b->pExtSc != NULL) ? b->pExtSc->getValue() >= 0.5f : false;

                b->sSC.set_mode(b->pScMode->getValue());
                b->sSC.set_reactivity(b->pScReact->getValue());
                b->sSC.set_stereo_mode((nMode == MBGM_MS) ? SCSM_MIDSIDE : SCSM_STEREO);
                b->sSC.set_source((b->pScSource != NULL) ? b->pScSource->getValue() : SCS_MIDDLE);

                if (sc_gain != b->fScPreamp)
                {
                    b->fScPreamp    = sc_gain;
                    b->nSync       |= S_EQ_CURVE;
                }

                // Update Gate settings
                bool hyst       = (b->pHyst != NULL) ? (b->pHyst->getValue() >= 0.5f) : false;
                float thresh    = b->pThresh[0]->getValue();
                float hthresh   = (hyst) ? (thresh * b->pThresh[1]->getValue()) : thresh;
                float zone      = b->pZone[0]->getValue();
                float hzone     = (hyst) ? (b->pZone[1]->getValue()) : zone;
                float makeup    = b->pMakeup->getValue();

                b->sGate.set_threshold(thresh, hthresh);
                b->sGate.set_zone(zone, hzone);
                b->sGate.set_timings(b->pAttack->getValue(), b->pRelease->getValue());
                b->sGate.set_reduction(b->pReduction->getValue());

                if (b->sGate.modified())
                {
                    b->sGate.update_settings();
                    b->nSync       |= S_DYN_CURVE;
                }
                if (b->fMakeup != makeup)
                {
                    b->fMakeup      = makeup;
                    b->nSync       |= S_DYN_CURVE;
                }
                if (b->bEnabled != enabled)
                {
                    b->bEnabled     = enabled;
                    b->nSync       |= S_DYN_CURVE;
                    if (!enabled)
                        b->sDelay.clear(); // Clear delay buffer from artifacts
                }
                if (b->bSolo != solo)
                {
                    b->bSolo        = solo;
                    b->nSync       |= S_DYN_CURVE;
                }
                if (b->bMute != mute)
                {
                    b->bMute        = mute;
                    b->nSync       |= S_DYN_CURVE;
                }
                if (b->bCustLCF != cust_lcf)
                {
                    b->bCustLCF     = cust_lcf;
                    b->nSync       |= S_DYN_CURVE;
                    c->nPlanSize    = 0;
                }
                if (b->bCustHCF != cust_hcf)
                {
                    b->bCustHCF     = cust_hcf;
                    b->nSync       |= S_DYN_CURVE;
                    c->nPlanSize    = 0;
                }
                if (cust_lcf)
                {
                    float lcf       = b->pScLcfFreq->getValue();
                    if (lcf != b->fFreqLCF)
                    {
                        b->fFreqLCF     = lcf;
                        c->nPlanSize    = 0;
                    }
                }
                if (cust_hcf)
                {
                    float hcf       = b->pScHcfFreq->getValue();
                    if (hcf != b->fFreqHCF)
                    {
                        b->fFreqHCF     = hcf;
                        c->nPlanSize    = 0;
                    }
                }

                if (b->bSolo)
                    solo_on         = true;

                // Estimate lookahead buffer size
                b->nLookahead   = millis_to_samples(fSampleRate, b->pScLook->getValue());
            }
        }

        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c    = &vChannels[i];

            // Check muting option
            for (size_t j=0; j<mb_gate_base_metadata::BANDS_MAX; ++j)
            {
                gate_band_t *b      = &c->vBands[j];
                if ((!b->bMute) && (solo_on))
                    b->bMute    = !b->bSolo;
            }

            // Rebuild compression plan
            if (c->nPlanSize <= 0)
            {
                c->nPlanSize                = 0;
                c->vBands[0].fFreqStart     = 0;
                c->vPlan[c->nPlanSize++]    = &c->vBands[0];

                for (size_t j=0; j<mb_gate_base_metadata::BANDS_MAX-1; ++j)
                {
                    gate_band_t *b       = &c->vBands[j+1];
                    b->fFreqStart       = c->vSplit[j].fFreq;

                    if (c->vSplit[j].bEnabled)
                        c->vPlan[c->nPlanSize++]    = b;
                }

                // Do simple sort of PLAN items by frequency
                if (c->nPlanSize > 1)
                {
                    // Sort in ascending order
                    for (size_t si=0; si < c->nPlanSize-1; ++si)
                        for (size_t sj=si+1; sj < c->nPlanSize; ++sj)
                            if (compare_bands_for_sort(c->vPlan[si], c->vPlan[sj]))
                            {
                                gate_band_t *tmp = c->vPlan[si];
                                c->vPlan[si]    = c->vPlan[sj];
                                c->vPlan[sj]    = tmp;
                            }

                    for (size_t j=1; j<c->nPlanSize; ++j)
                        c->vPlan[j-1]->fFreqEnd     = c->vPlan[j]->fFreqStart;
                }
                c->vPlan[c->nPlanSize-1]->fFreqEnd       = (fSampleRate >> 1);

                // Configure equalizers
                lsp_trace("Reordered bands according to frequency grow");
                for (size_t j=0; j<c->nPlanSize; ++j)
                {
                    gate_band_t *b   = c->vPlan[j];
                    b->pFreqEnd->setValue(b->fFreqEnd);
                    b->nSync       |= S_EQ_CURVE;

                    lsp_trace("plan[%d] start=%f, end=%f", int(j), b->fFreqStart, b->fFreqEnd);

                    // Configure equalizer for the sidechain
                    for (size_t k=0; k<channels; ++k)
                    {
                        // Configure lo-pass filter
                        fp.nType        = ((j != (c->nPlanSize-1)) || (b->bCustHCF)) ? FLT_BT_LRX_LOPASS : FLT_NONE;
                        fp.fFreq        = (b->bCustHCF) ? b->pScHcfFreq->getValue() : b->pFreqEnd->getValue();
                        fp.fFreq2       = fp.fFreq;
                        fp.fQuality     = 0.0f;
                        fp.fGain        = 1.0f;
                        fp.fQuality     = 0.0f;
                        fp.nSlope       = 2;

                        b->sEQ[k].set_params(0, &fp);

                        // Configure hi-pass filter
                        fp.nType        = ((j != 0) || (b->bCustLCF)) ? FLT_BT_LRX_HIPASS : FLT_NONE;
                        fp.fFreq        = (b->bCustLCF) ? b->pScLcfFreq->getValue() : b->fFreqStart;
                        fp.fFreq2       = fp.fFreq;
                        fp.fQuality     = 0.0f;
                        fp.fGain        = 1.0f;
                        fp.fQuality     = 0.0f;
                        fp.nSlope       = 2;

                        b->sEQ[k].set_params(1, &fp);
                    }

                    // Update transfer function for equalizer
                    b->sEQ[0].freq_chart(size_t(0), b->vTr, vFreqs, mb_gate_base_metadata::FFT_MESH_POINTS);
                    b->sEQ[0].freq_chart(size_t(1), vTr, vFreqs, mb_gate_base_metadata::FFT_MESH_POINTS);
                    dsp::pcomplex_mul2(b->vTr, vTr, mb_gate_base_metadata::FFT_MESH_POINTS);
                    dsp::pcomplex_mod(b->vTr, b->vTr, mb_gate_base_metadata::FFT_MESH_POINTS);

                    // Update filter parameters, depending on operating mode
                    if (bModern)
                    {
                        // Configure filter for band
                        if (j <= 0)
                        {
                            fp.nType        = (c->nPlanSize > 1) ? FLT_BT_LRX_LOSHELF : FLT_BT_AMPLIFIER;
                            fp.fFreq        = b->fFreqEnd;
                            fp.fFreq2       = b->fFreqEnd;
                        }
                        else if (j >= (c->nPlanSize - 1))
                        {
                            fp.nType        = FLT_BT_LRX_HISHELF;
                            fp.fFreq        = b->fFreqStart;
                            fp.fFreq2       = b->fFreqStart;
                        }
                        else
                        {
                            fp.nType        = FLT_BT_LRX_LADDERPASS;
                            fp.fFreq        = b->fFreqStart;
                            fp.fFreq2       = b->fFreqEnd;
                        }

                        fp.fGain        = 1.0f;
                        fp.nSlope       = 2;
                        fp.fQuality     = 0.0;

                        lsp_trace("Filter type=%d, from=%f, to=%f", int(fp.nType), fp.fFreq, fp.fFreq2);

                        sFilters.set_params(b->nFilterID, &fp);
                    }
                    else
                    {
                        fp.fGain        = 1.0f;
                        fp.nSlope       = 2;
                        fp.fQuality     = 0.0;
                        fp.fFreq        = b->fFreqEnd;
                        fp.fFreq2       = b->fFreqEnd;

                        // We're going from low frequencies to high frequencies
                        if (j >= (c->nPlanSize - 1))
                        {
                            fp.nType    = FLT_NONE;
                            b->sPassFilter.update(fSampleRate, &fp);
                            b->sRejFilter.update(fSampleRate, &fp);
                            b->sAllFilter.update(fSampleRate, &fp);
                        }
                        else
                        {
                            fp.nType    = FLT_BT_LRX_LOPASS;
                            b->sPassFilter.update(fSampleRate, &fp);
                            fp.nType    = FLT_BT_LRX_HIPASS;
                            b->sRejFilter.update(fSampleRate, &fp);
                            fp.nType    = (j == 0) ? FLT_NONE : FLT_BT_LRX_ALLPASS;
                            b->sAllFilter.update(fSampleRate, &fp);
                        }
                    }
                }
            } // nPlanSize

            // Enable/disable dynamic filters
            for (size_t j=0; j<mb_gate_base_metadata::BANDS_MAX; ++j)
            {
                gate_band_t *b   = &c->vBands[j];
                sFilters.set_filter_active(b->nFilterID, b->bEnabled);
            }

            // Calculate latency
            for (size_t j=0; j<c->nPlanSize; ++j)
            {
                gate_band_t *b   = c->vPlan[j];

                if (latency < b->nLookahead)
                    latency = b->nLookahead;
            }
        }

        // Update latency
        set_latency(latency);
        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c    = &vChannels[i];

            // Update latency
            for (size_t j=0; j<c->nPlanSize; ++j)
            {
                gate_band_t *b   = c->vPlan[j];
                b->sDelay.set_delay(latency - b->nLookahead);
            }
            c->sDelay.set_delay(latency);
        }

        nEnvBoost       = env_boost;
        bEnvUpdate      = false;
    }

    void mb_gate_base::update_sample_rate(long sr)
    {
        // Determine number of channels
        size_t channels     = (nMode == MBGM_MONO) ? 1 : 2;
        size_t max_delay    = millis_to_samples(sr, mb_gate_base_metadata::LOOKAHEAD_MAX);

        // Update analyzer's sample rate
        sAnalyzer.set_sample_rate(sr);
        sFilters.set_sample_rate(sr);
        bEnvUpdate          = true;

        // Update channels
        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c = &vChannels[i];
            c->sBypass.init(sr);
            c->sDelay.init(max_delay);

            // Update bands
            for (size_t j=0; j<mb_gate_base_metadata::BANDS_MAX; ++j)
            {
                gate_band_t *b   = &c->vBands[j];

                b->sSC.set_sample_rate(sr);
                b->sGate.set_sample_rate(sr);
                b->sDelay.init(max_delay);

                b->sPassFilter.set_sample_rate(sr);
                b->sRejFilter.set_sample_rate(sr);
                b->sAllFilter.set_sample_rate(sr);

                b->sEQ[0].set_sample_rate(sr);
                if (channels > 1)
                    b->sEQ[1].set_sample_rate(sr);
            }

            c->nPlanSize        = 0; // Force to rebuild plan
        }
    }

    void mb_gate_base::ui_activated()
    {
        size_t channels     = (nMode == MBGM_MONO) ? 1 : 2;

        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c        = &vChannels[i];

            for (size_t j=0; j<c->nPlanSize; ++j)
            {
                gate_band_t *b       = c->vPlan[j];
                b->nSync            = S_ALL;
            }
        }
    }

    void mb_gate_base::process(size_t samples)
    {
        size_t channels     = (nMode == MBGM_MONO) ? 1 : 2;

        // Bind input signal
        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c        = &vChannels[i];

            c->vIn              = c->pIn->getBuffer<float>();
            c->vOut             = c->pOut->getBuffer<float>();
            c->vScIn            = (c->pScIn != NULL) ? c->pScIn->getBuffer<float>() : NULL;
        }

        // Do processing
        while (samples > 0)
        {
            // Determine buffer size for processing
            size_t to_process   = (samples > MBG_BUFFER_SIZE) ? MBG_BUFFER_SIZE : samples;

            // Measure input signal level
            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c        = &vChannels[i];
                float level         = dsp::abs_max(c->vIn, to_process) * fInGain;
                c->pInLvl->setValue(level);
            }

            // Pre-process channel data
            if (nMode == MBGM_MS)
            {
                dsp::lr_to_ms(vChannels[0].vBuffer, vChannels[1].vBuffer, vChannels[0].vIn, vChannels[1].vIn, to_process);
                dsp::mul_k2(vChannels[0].vBuffer, fInGain, to_process);
                dsp::mul_k2(vChannels[1].vBuffer, fInGain, to_process);
            }
            else if (nMode == MBGM_MONO)
                dsp::mul_k3(vChannels[0].vBuffer, vChannels[0].vIn, fInGain, to_process);
            else
            {
                dsp::mul_k3(vChannels[0].vBuffer, vChannels[0].vIn, fInGain, to_process);
                dsp::mul_k3(vChannels[1].vBuffer, vChannels[1].vIn, fInGain, to_process);
            }
            if (bSidechain)
            {
                if (nMode == MBGM_MS)
                {
                    dsp::lr_to_ms(vChannels[0].vExtScBuffer, vChannels[1].vExtScBuffer, vChannels[0].vScIn, vChannels[1].vScIn, to_process);
                    dsp::mul_k2(vChannels[0].vExtScBuffer, fInGain, to_process);
                    dsp::mul_k2(vChannels[1].vExtScBuffer, fInGain, to_process);
                }
                else if (nMode == MBGM_MONO)
                    dsp::mul_k3(vChannels[0].vExtScBuffer, vChannels[0].vScIn, fInGain, to_process);
                else
                {
                    dsp::mul_k3(vChannels[0].vExtScBuffer, vChannels[0].vScIn, fInGain, to_process);
                    dsp::mul_k3(vChannels[1].vExtScBuffer, vChannels[1].vScIn, fInGain, to_process);
                }
            }


            // Do frequency boost and input channel analysis
            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c        = &vChannels[i];
                c->sEnvBoost[0].process(c->vScBuffer, c->vBuffer, to_process);
                if (bSidechain)
                    c->sEnvBoost[1].process(c->vExtScBuffer, c->vExtScBuffer, to_process);

                dsp::copy(c->vInAnalyze, c->vBuffer, to_process);
            }

            // MAIN PLUGIN STUFF
            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c        = &vChannels[i];

                for (size_t j=0; j<c->nPlanSize; ++j)
                {
                    gate_band_t *b       = c->vPlan[j];

                    // Prepare sidechain signal with band equalizers
                    b->sEQ[0].process(vSc[0], (b->bExtSc) ? vChannels[0].vExtScBuffer : vChannels[0].vScBuffer, to_process);
                    if (channels > 1)
                        b->sEQ[1].process(vSc[1], (b->bExtSc) ? vChannels[1].vExtScBuffer : vChannels[1].vScBuffer, to_process);

                    // Preprocess VCA signal
                    b->sSC.process(vBuffer, const_cast<const float **>(vSc), to_process); // Band now contains processed by sidechain signal
                    b->sDelay.process(vBuffer, vBuffer, b->fScPreamp, to_process); // Apply sidechain preamp and lookahead delay

                    if (b->bEnabled)
                    {
                        b->sGate.process(b->vVCA, vEnv, vBuffer, to_process); // Output
                        if (bModern)
                            dsp::limit1(b->vVCA, GAIN_AMP_M_72_DB, GAIN_AMP_P_72_DB, to_process);

                        // Output curve level
                        size_t imax     = dsp::abs_max_index(vEnv, to_process);
                        b->pEnvLvl->setValue(vEnv[imax]);
                        b->pMeterGain->setValue(b->vVCA[imax] * b->fMakeup);
                        b->pCurveLvl->setValue(b->vVCA[imax] * vEnv[imax] * b->fMakeup);

                        dsp::mul_k2(b->vVCA, b->fMakeup, to_process); // Apply makeup gain

                        // Remember last envelope level and buffer level
                        b->fEnvLevel    = vEnv[to_process-1];
                        b->fGainLevel   = b->vVCA[to_process-1];

                        // Check muting option
                        if (b->bMute)
                            dsp::fill(b->vVCA, GAIN_AMP_M_36_DB, to_process);
                    }
                    else
                    {
                        dsp::fill(b->vVCA, (b->bMute) ? GAIN_AMP_M_36_DB : GAIN_AMP_0_DB, to_process);
                        b->fEnvLevel    = GAIN_AMP_0_DB;
                        b->fGainLevel   = GAIN_AMP_0_DB;
                    }
                }

                // Output curve parameters for disabled gates
                for (size_t i=0; i<mb_gate_base_metadata::BANDS_MAX; ++i)
                {
                    gate_band_t *b       = &c->vBands[i];
                    if (b->bEnabled)
                        continue;

                    b->pEnvLvl->setValue(0.0f);
                    b->pCurveLvl->setValue(0.0f);
                    b->pMeterGain->setValue(GAIN_AMP_0_DB);
                }
            }

            // Here, we apply VCA to input signal dependent on the input
            if (bModern) // 'Modern' mode
            {
                // Apply VCA control
                for (size_t i=0; i<channels; ++i)
                {
                    channel_t *c        = &vChannels[i];
                    c->sDelay.process(c->vInBuffer, c->vBuffer, to_process); // Apply delay to compensate lookahead feature
                    dsp::copy(vBuffer, c->vInBuffer, to_process);

                    for (size_t j=0; j<c->nPlanSize; ++j)
                    {
                        gate_band_t *b       = c->vPlan[j];
                        sFilters.process(b->nFilterID, c->vBuffer, c->vBuffer, b->vVCA, to_process);
                    }
                }
            }
            else // 'Classic' mode
            {
                // Apply VCA control
                for (size_t i=0; i<channels; ++i)
                {
                    channel_t *c        = &vChannels[i];

                    // Originally, there is no signal
                    c->sDelay.process(c->vInBuffer, c->vBuffer, to_process); // Apply delay to compensate lookahead feature, store into vBuffer
                    dsp::copy(vBuffer, c->vInBuffer, to_process);
                    dsp::fill_zero(c->vBuffer, to_process);                 // Clear the channel buffer

                    for (size_t j=0; j<c->nPlanSize; ++j)
                    {
                        gate_band_t *b       = c->vPlan[j];

                        b->sAllFilter.process(c->vBuffer, c->vBuffer, to_process); // Process the signal with all-pass
                        b->sPassFilter.process(vEnv, vBuffer, to_process); // Filter frequencies from input
                        dsp::mul2(vEnv, b->vVCA, to_process); // Apply VCA gain
                        dsp::add2(c->vBuffer, vEnv, to_process); // Add signal to the channel buffer
                        b->sRejFilter.process(vBuffer, vBuffer, to_process); // Filter frequencies from input
                    }
                }
            }

            // MAIN PLUGIN STUFF END

            // Do output channel analysis
            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c        = &vChannels[i];
                dsp::copy(c->vOutAnalyze, c->vBuffer, to_process);
            }

            sAnalyzer.process(vAnalyze, to_process);

            // Post-process data (if needed)
            if (nMode == MBGM_MS)
            {
                dsp::ms_to_lr(vChannels[0].vBuffer, vChannels[1].vBuffer, vChannels[0].vBuffer, vChannels[1].vBuffer, to_process);
                dsp::ms_to_lr(vChannels[0].vInBuffer, vChannels[1].vInBuffer, vChannels[0].vInBuffer, vChannels[1].vInBuffer, to_process);
            }

            // Final metering
            for (size_t i=0; i<channels; ++i)
            {
                channel_t *c        = &vChannels[i];

                // Apply dry/wet gain and bypass
                dsp::mix2(c->vBuffer, c->vIn, fWetGain, fDryGain, to_process);
                float level         = dsp::abs_max(c->vBuffer, to_process);
                c->pOutLvl->setValue(level);
                c->sBypass.process(c->vOut, c->vIn, c->vBuffer, to_process);

                // Update pointers
                c->vIn             += to_process;
                c->vOut            += to_process;
                if (c->vScIn != NULL)
                    c->vScIn           += to_process;
            }
            samples    -= to_process;
        } // while (samples > 0)

        // Output FFT curves for each channel
        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c     = &vChannels[i];

            // Calculate transfer function for the gate
            if (bModern)
            {
                dsp::pcomplex_fill_ri(c->vTr, 1.0f, 0.0f, mb_gate_base_metadata::FFT_MESH_POINTS);

                // Calculate transfer function
                for (size_t j=0; j<c->nPlanSize; ++j)
                {
                    gate_band_t *b       = c->vPlan[j];
                    sFilters.freq_chart(b->nFilterID, vTr, vFreqs, b->fGainLevel, mb_gate_base_metadata::FFT_MESH_POINTS);
                    dsp::pcomplex_mul2(c->vTr, vTr, mb_gate_base_metadata::FFT_MESH_POINTS);
                }
            }
            else
            {
                dsp::pcomplex_fill_ri(vTr, 1.0f, 0.0f, mb_gate_base_metadata::FFT_MESH_POINTS);   // vBuffer
                dsp::fill_zero(c->vTr, mb_gate_base_metadata::FFT_MESH_POINTS*2);                 // c->vBuffer

                // Calculate transfer function
                for (size_t j=0; j<c->nPlanSize; ++j)
                {
                    gate_band_t *b       = c->vPlan[j];

                    // Apply all-pass characteristics
                    b->sAllFilter.freq_chart(vPFc, vFreqs, mb_gate_base_metadata::FFT_MESH_POINTS);
                    dsp::pcomplex_mul2(c->vTr, vPFc, mb_gate_base_metadata::FFT_MESH_POINTS);

                    // Apply lo-pass filter characteristics
                    b->sPassFilter.freq_chart(vPFc, vFreqs, mb_gate_base_metadata::FFT_MESH_POINTS);
                    dsp::pcomplex_mul2(vPFc, vTr, mb_gate_base_metadata::FFT_MESH_POINTS);
                    dsp::fmadd_k3(c->vTr, vPFc, b->fGainLevel, mb_gate_base_metadata::FFT_MESH_POINTS*2);

                    // Apply hi-pass filter characteristics
                    b->sRejFilter.freq_chart(vRFc, vFreqs, mb_gate_base_metadata::FFT_MESH_POINTS);
                    dsp::pcomplex_mul2(vTr, vRFc, mb_gate_base_metadata::FFT_MESH_POINTS);
                }
            }
            dsp::pcomplex_mod(c->vTrMem, c->vTr, mb_gate_base_metadata::FFT_MESH_POINTS);

            // Output FFT curve, compression curve and FFT spectrogram for each band
            for (size_t j=0; j<mb_gate_base_metadata::BANDS_MAX; ++j)
            {
                gate_band_t *b      = &c->vBands[j];

                // FFT spectrogram
                mesh_t *mesh        = NULL;

                // FFT curve
                if (b->nSync & S_EQ_CURVE)
                {
                    mesh                = (b->pScFreqChart != NULL) ? b->pScFreqChart->getBuffer<mesh_t>() : NULL;
                    if ((mesh != NULL) && (mesh->isEmpty()))
                    {
                        // Add extra points
                        mesh->pvData[0][0] = SPEC_FREQ_MIN*0.5f;
                        mesh->pvData[0][mb_gate_base_metadata::MESH_POINTS+1] = SPEC_FREQ_MAX * 2.0f;
                        mesh->pvData[1][0] = 0.0f;
                        mesh->pvData[1][mb_gate_base_metadata::MESH_POINTS+1] = 0.0f;

                        // Fill mesh
                        dsp::copy(&mesh->pvData[0][1], vFreqs, mb_gate_base_metadata::MESH_POINTS);
                        dsp::mul_k3(&mesh->pvData[1][1], b->vTr, b->fScPreamp, mb_gate_base_metadata::MESH_POINTS);
                        mesh->data(2, mb_gate_base_metadata::FILTER_MESH_POINTS);

                        // Mark mesh as synchronized
                        b->nSync           &= ~S_EQ_CURVE;
                    }
                }

                // Compression curve
                for (size_t j=0; j<2; ++j)
                {
                    if ((b->pCurveGraph[j] == NULL) || (!(b->nSync & (S_GATE_CURVE << j))))
                        continue;

                    mesh                = b->pCurveGraph[j]->getBuffer<mesh_t>();
                    if ((mesh != NULL) && (mesh->isEmpty()))
                    {
                        if (b->bEnabled)
                        {
                            // Copy frequency points
                            dsp::copy(mesh->pvData[0], vCurve, mb_gate_base_metadata::CURVE_MESH_SIZE);
                            b->sGate.curve(mesh->pvData[1], vCurve, mb_gate_base_metadata::CURVE_MESH_SIZE, j > 0);
                            if (b->fMakeup != GAIN_AMP_0_DB)
                                dsp::mul_k2(mesh->pvData[1], b->fMakeup, gate_base_metadata::CURVE_MESH_SIZE);

                            // Mark mesh containing data
                            mesh->data(2, mb_gate_base_metadata::CURVE_MESH_SIZE);
                        }
                        else
                            mesh->data(2, 0);

                    }

                    // Mark mesh as synchronized
                    b->nSync           &= ~(S_GATE_CURVE << j);
                }
            }

            // Output FFT curve for input
            mesh_t *mesh            = (c->pFftIn != NULL) ? c->pFftIn->getBuffer<mesh_t>() : NULL;
            if ((mesh != NULL) && (mesh->isEmpty()))
            {
                if (c->bInFft)
                {
                    // Copy frequency points
                    dsp::copy(mesh->pvData[0], vFreqs, mb_gate_base_metadata::FFT_MESH_POINTS);
                    sAnalyzer.get_spectrum(c->nAnInChannel, mesh->pvData[1], vIndexes, mb_gate_base_metadata::FFT_MESH_POINTS);

                    // Mark mesh containing data
                    mesh->data(2, mb_gate_base_metadata::FFT_MESH_POINTS);
                }
                else
                    mesh->data(2, 0);
            }

            // Output FFT curve for output
            mesh            = (c->pFftOut != NULL) ? c->pFftOut->getBuffer<mesh_t>() : NULL;
            if ((mesh != NULL) && (mesh->isEmpty()))
            {
                if (sAnalyzer.channel_active(c->nAnOutChannel))
                {
                    // Copy frequency points
                    dsp::copy(mesh->pvData[0], vFreqs, mb_gate_base_metadata::FFT_MESH_POINTS);
                    sAnalyzer.get_spectrum(c->nAnOutChannel, mesh->pvData[1], vIndexes, mb_gate_base_metadata::FFT_MESH_POINTS);

                    // Mark mesh containing data
                    mesh->data(2, mb_gate_base_metadata::FFT_MESH_POINTS);
                }
                else
                    mesh->data(2, 0);
            }

            // Output Channel curve
            mesh            = (c->pAmpGraph != NULL) ? c->pAmpGraph->getBuffer<mesh_t>() : NULL;
            if ((mesh != NULL) && (mesh->isEmpty()))
            {
                // Calculate amplitude (modulo)
                dsp::copy(mesh->pvData[0], vFreqs, mb_gate_base_metadata::FFT_MESH_POINTS);
                dsp::copy(mesh->pvData[1], c->vTrMem, mb_gate_base_metadata::FFT_MESH_POINTS);
                mesh->data(2, mb_gate_base_metadata::FFT_MESH_POINTS);
            }
        } // for channel

        // Request for redraw
        if (pWrapper != NULL)
            pWrapper->query_display_draw();
    }

    bool mb_gate_base::inline_display(ICanvas *cv, size_t width, size_t height)
    {
        // Check proportions
        if (height > (R_GOLDEN_RATIO * width))
            height  = R_GOLDEN_RATIO * width;

        // Init canvas
        if (!cv->init(width, height))
            return false;
        width   = cv->width();
        height  = cv->height();

        // Clear background
        bool bypassing = vChannels[0].sBypass.bypassing();
        cv->set_color_rgb((bypassing) ? CV_DISABLED : CV_BACKGROUND);
        cv->paint();

        // Draw axis
        cv->set_line_width(1.0);

        // "-72 db / (:zoom ** 3)" max="24 db * :zoom"

        float miny  = logf(GAIN_AMP_M_72_DB / dsp::ipowf(fZoom, 3));
        float maxy  = logf(GAIN_AMP_P_24_DB * fZoom);

        float zx    = 1.0f/SPEC_FREQ_MIN;
        float zy    = dsp::ipowf(fZoom, 3)/GAIN_AMP_M_72_DB;
        float dx    = width/(logf(SPEC_FREQ_MAX)-logf(SPEC_FREQ_MIN));
        float dy    = height/(miny-maxy);

        // Draw vertical lines
        cv->set_color_rgb(CV_YELLOW, 0.5f);
        for (float i=100.0f; i<SPEC_FREQ_MAX; i *= 10.0f)
        {
            float ax = dx*(logf(i*zx));
            cv->line(ax, 0, ax, height);
        }

        // Draw horizontal lines
        cv->set_color_rgb(CV_WHITE, 0.5f);
        for (float i=GAIN_AMP_M_72_DB; i<GAIN_AMP_P_24_DB; i *= GAIN_AMP_P_12_DB)
        {
            float ay = height + dy*(logf(i*zy));
            cv->line(0, ay, width, ay);
        }

        // Allocate buffer: f, x, y, tr
        pIDisplay           = float_buffer_t::reuse(pIDisplay, 4, width+2);
        float_buffer_t *b   = pIDisplay;
        if (b == NULL)
            return false;

        // Initialize mesh
        b->v[0][0]          = SPEC_FREQ_MIN*0.5f;
        b->v[0][width+1]    = SPEC_FREQ_MAX*2.0f;
        b->v[3][0]          = 1.0f;
        b->v[3][width+1]    = 1.0f;

        size_t channels = ((nMode == MBGM_MONO) || (nMode == MBGM_STEREO)) ? 1 : 2;
        static uint32_t c_colors[] = {
                CV_MIDDLE_CHANNEL, CV_MIDDLE_CHANNEL,
                CV_MIDDLE_CHANNEL, CV_MIDDLE_CHANNEL,
                CV_LEFT_CHANNEL, CV_RIGHT_CHANNEL,
                CV_MIDDLE_CHANNEL, CV_SIDE_CHANNEL
               };

        bool aa = cv->set_anti_aliasing(true);
        cv->set_line_width(2);

        for (size_t i=0; i<channels; ++i)
        {
            channel_t *c    = &vChannels[i];

            for (size_t j=0; j<width; ++j)
            {
                size_t k        = (j*mb_gate_base_metadata::MESH_POINTS)/width;
                b->v[0][j+1]    = vFreqs[k];
                b->v[3][j+1]    = c->vTrMem[k];
            }

            dsp::fill(b->v[1], 0.0f, width+2);
            dsp::fill(b->v[2], height, width+2);
            dsp::axis_apply_log1(b->v[1], b->v[0], zx, dx, width+2);
            dsp::axis_apply_log1(b->v[2], b->v[3], zy, dy, width+2);

            // Draw mesh
            uint32_t color = (bypassing || !(active())) ? CV_SILVER : c_colors[nMode*2 + i];
            Color stroke(color), fill(color, 0.5f);
            cv->draw_poly(b->v[1], b->v[2], width+2, stroke, fill);
        }
        cv->set_anti_aliasing(aa);

        return true;
    }

    //-------------------------------------------------------------------------
    // Expander derivatives
    mb_gate_mono::mb_gate_mono() : mb_gate_base(metadata, false, MBGM_MONO)
    {
    }

    mb_gate_stereo::mb_gate_stereo() : mb_gate_base(metadata, false, MBGM_STEREO)
    {
    }

    mb_gate_lr::mb_gate_lr() : mb_gate_base(metadata, false, MBGM_LR)
    {
    }

    mb_gate_ms::mb_gate_ms() : mb_gate_base(metadata, false, MBGM_MS)
    {
    }

    sc_mb_gate_mono::sc_mb_gate_mono() : mb_gate_base(metadata, true, MBGM_MONO)
    {
    }

    sc_mb_gate_stereo::sc_mb_gate_stereo() : mb_gate_base(metadata, true, MBGM_STEREO)
    {
    }

    sc_mb_gate_lr::sc_mb_gate_lr() : mb_gate_base(metadata, true, MBGM_LR)
    {
    }

    sc_mb_gate_ms::sc_mb_gate_ms() : mb_gate_base(metadata, true, MBGM_MS)
    {
    }
}


