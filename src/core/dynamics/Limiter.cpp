/*
 * Limiter.cpp
 *
 *  Created on: 25 нояб. 2016 г.
 *      Author: sadko
 */

#include <dsp/dsp.h>
#include <core/debug.h>
#include <core/interpolation.h>
#include <core/dynamics/Limiter.h>

#define BUF_GRANULARITY         8192
#define GAIN_LOWERING           0.891250938134 /* 0.944060876286 */
#define MIN_LIMITER_RELEASE     5.0f

namespace lsp
{
    Limiter::Limiter()
    {
        construct();
    }

    Limiter::~Limiter()
    {
        destroy();
    }

    void Limiter::construct()
    {
        fThreshold      = 1.0f;
        fLookahead      = 0.0f;
        fMaxLookahead   = 0.0f;
        fAttack         = 0.0f;
        fRelease        = 0.0f;
        fKnee           = GAIN_AMP_M_6_DB;
        nMaxLookahead   = 0;
        nLookahead      = 0;
        nMaxSampleRate  = 0;
        nSampleRate     = 0;
        nUpdate         = UP_ALL;
        nMode           = LM_COMPRESSOR;
        nThresh         = 0;

        sALR.fAttack    = 10.0f;
        sALR.fRelease   = 50.0f;
        sALR.fEnvelope  = 0.0f;
        sALR.bEnable    = false;

        vGainBuf        = NULL;
        vTmpBuf         = NULL;
        vData           = NULL;
    }

    bool Limiter::init(size_t max_sr, float max_lookahead)
    {
        nMaxLookahead       = millis_to_samples(max_sr, max_lookahead);
        size_t alloc        = nMaxLookahead*4 + BUF_GRANULARITY*2;
        float *ptr          = alloc_aligned<float>(vData, alloc, DEFAULT_ALIGN);
        if (ptr == NULL)
            return false;

        vGainBuf            = ptr;
        ptr                += nMaxLookahead*4 + BUF_GRANULARITY;
        vTmpBuf             = ptr;
        ptr                += BUF_GRANULARITY;

        lsp_assert(reinterpret_cast<uint8_t *>(ptr) <= &vData[alloc*sizeof(float) + DEFAULT_ALIGN]);

        dsp::fill_one(vGainBuf, nMaxLookahead*4 + BUF_GRANULARITY);
        dsp::fill_zero(vTmpBuf, BUF_GRANULARITY);

        if (!sDelay.init(nMaxLookahead + BUF_GRANULARITY))
            return false;

        nMaxSampleRate      = max_sr;
        fMaxLookahead       = max_lookahead;
        return true;
    }

    void Limiter::destroy()
    {
        sDelay.destroy();

        if (vData != NULL)
        {
            free_aligned(vData);
            vData = NULL;
        }

        vGainBuf    = NULL;
        vTmpBuf     = NULL;
    }

    float Limiter::set_attack(float attack)
    {
        float old = fAttack;
        if (attack == old)
            return old;

        fAttack         = attack;
        nUpdate        |= UP_OTHER;
        return old;
    }

    float Limiter::set_release(float release)
    {
        float old = fRelease;
        if (release == old)
            return old;

        fRelease        = release;
        nUpdate        |= UP_OTHER;
        return old;
    }

    float Limiter::set_threshold(float thresh)
    {
        float old = fThreshold;
        if (old == thresh)
            return old;

        fThreshold      = thresh;
        nUpdate        |= UP_THRESH | UP_ALR;
        return old;
    }

    float Limiter::set_lookahead(float lk_ahead)
    {
        float old = fLookahead;
        if (lk_ahead > fMaxLookahead)
            lk_ahead = fMaxLookahead;
        if (old == lk_ahead)
            return old;

        fLookahead      = lk_ahead;
        nUpdate        |= UP_LK;

        return old;
    }

    float Limiter::set_knee(float knee)
    {
        float old = fKnee;
        if (knee > 1.0f)
            knee = 1.0f;
        if (old == knee)
            return old;

        fKnee           = knee;
        nUpdate        |= UP_OTHER;
        return old;
    }

    float Limiter::set_alr_attack(float attack)
    {
        float old = sALR.fAttack;
        if (attack == old)
            return old;

        sALR.fAttack    = attack;
        nUpdate        |= UP_ALR;
        return old;
    }

    float Limiter::set_alr_release(float release)
    {
        float old = sALR.fRelease;
        if (release == old)
            return old;

        sALR.fRelease   = release;
        nUpdate        |= UP_ALR;
        return old;
    }

    bool Limiter::set_alr(bool enable)
    {
        bool old        = sALR.bEnable;
        sALR.bEnable    = enable;
        return old;
    }

    void Limiter::reset_sat(sat_t *sat)
    {
        sat->nAttack        = 0;
        sat->nPlane         = 0;
        sat->nRelease       = 0;
        sat->nMiddle        = 0;

        sat->vAttack[0]     = 0.0f;
        sat->vAttack[1]     = 0.0f;
        sat->vAttack[2]     = 0.0f;
        sat->vAttack[3]     = 0.0f;
        sat->vRelease[0]    = 0.0f;
        sat->vRelease[1]    = 0.0f;
        sat->vRelease[2]    = 0.0f;
        sat->vRelease[3]    = 0.0f;
    }

    void Limiter::reset_exp(exp_t *exp)
    {
        exp->nAttack        = 0;
        exp->nPlane         = 0;
        exp->nRelease       = 0;
        exp->nMiddle        = 0;

        exp->vAttack[0]     = 0.0f;
        exp->vAttack[1]     = 0.0f;
        exp->vAttack[2]     = 0.0f;
        exp->vAttack[3]     = 0.0f;
        exp->vRelease[0]    = 0.0f;
        exp->vRelease[1]    = 0.0f;
        exp->vRelease[2]    = 0.0f;
        exp->vRelease[3]    = 0.0f;
    }

    void Limiter::reset_line(line_t *line)
    {
        line->nAttack       = 0;
        line->nPlane        = 0;
        line->nRelease      = 0;
        line->nMiddle       = 0;

        line->vAttack[0]    = 0.0f;
        line->vAttack[1]    = 0.0f;
        line->vRelease[0]   = 0.0f;
        line->vRelease[1]   = 0.0f;
    }

    void Limiter::reset_comp(comp_t *comp)
    {
        comp->fKS           = 0.0f;
        comp->fKE           = 0.0f;
        comp->fTauAttack    = 0.0f;
        comp->fTauRelease   = 0.0f;
        comp->fEnvelope     = 0.0f;
        comp->fAmp          = 0.0f;
        comp->nCountdown    = 0;
        comp->fSample       = 0.0f;
        comp->vHermite[0]   = 0.0f;
        comp->vHermite[1]   = 0.0f;
        comp->vHermite[2]   = 0.0f;
    }

    void Limiter::init_sat(sat_t *sat)
    {
        ssize_t attack      = millis_to_samples(nSampleRate, fAttack);
        ssize_t release     = millis_to_samples(nSampleRate, fRelease);
        if (attack > ssize_t(nLookahead))
            attack              = nLookahead;
        else if (attack < 8)
            attack              = 8;
        if (release > ssize_t(nLookahead*2))
            release             = nLookahead*2;
        else if (release < 8)
            release             = 8;

        if (nMode == LM_HERM_THIN)
        {
            sat->nAttack        = attack;
            sat->nPlane         = attack;
        }
        else if (nMode == LM_HERM_TAIL)
        {
            sat->nAttack        = (attack >> 1);
            sat->nPlane         = attack;
        }
        else if (nMode == LM_HERM_DUCK)
        {
            sat->nAttack        = attack;
            sat->nPlane         = (attack + (release >> 1));
        }
        else // LM_HERM_WIDE by default
        {
            sat->nAttack        = (attack >> 1);
            sat->nPlane         = (attack + (release >> 1));
        }

        sat->nRelease       = attack + release + 1;
        sat->nMiddle        = attack;

        interpolation::hermite_cubic(sat->vAttack, -1.0f, 0.0f, 0.0f, sat->nAttack, 1.0f, 0.0f);
        interpolation::hermite_cubic(sat->vRelease, sat->nPlane, 1.0f, 0.0f, sat->nRelease, 0.0f, 0.0f);
    }

    void Limiter::init_exp(exp_t *exp)
    {
        ssize_t attack      = millis_to_samples(nSampleRate, fAttack);
        ssize_t release     = millis_to_samples(nSampleRate, fRelease);
        if (attack > ssize_t(nLookahead))
            attack              = nLookahead;
        else if (attack < 8)
            attack              = 8;
        if (release > ssize_t(nLookahead*2))
            release             = nLookahead*2;
        else if (release < 8)
            release             = 8;

        if (nMode == LM_EXP_THIN)
        {
            exp->nAttack        = attack;
            exp->nPlane         = attack;
        }
        else if (nMode == LM_EXP_TAIL)
        {
            exp->nAttack        = (attack >> 1);
            exp->nPlane         = attack;
        }
        else if (nMode == LM_EXP_DUCK)
        {
            exp->nAttack        = attack;
            exp->nPlane         = (attack + (release >> 1));
        }
        else // LM_EXP_WIDE by default
        {
            exp->nAttack        = (attack >> 1);
            exp->nPlane         = (attack + (release >> 1));
        }

        exp->nRelease       = attack + release + 1;
        exp->nMiddle        = attack;

        interpolation::exponent(exp->vAttack, -1.0f, 0.0f, exp->nAttack, 1.0f, 2.0f / attack);
        interpolation::exponent(exp->vRelease, exp->nPlane, 1.0f, exp->nRelease, 0.0f, 2.0f / release);
    }

    void Limiter::init_line(line_t *line)
    {
        ssize_t attack      = millis_to_samples(nSampleRate, fAttack);
        ssize_t release     = millis_to_samples(nSampleRate, fRelease);
        if (attack > ssize_t(nLookahead))
            attack              = nLookahead;
        else if (attack < 8)
            attack              = 8;
        if (release > ssize_t(nLookahead*2))
            release             = nLookahead*2;
        else if (release < 8)
            release             = 8;

        if (nMode == LM_LINE_THIN)
        {
            line->nAttack       = attack;
            line->nPlane        = attack;
        }
        else if (nMode == LM_LINE_TAIL)
        {
            line->nAttack       = (attack >> 1);
            line->nPlane        = attack;
        }
        else if (nMode == LM_LINE_DUCK)
        {
            line->nAttack       = attack;
            line->nPlane        = (attack + (release >> 1));
        }
        else // LM_LINE_WIDE by default
        {
            line->nAttack       = (attack >> 1);
            line->nPlane        = (attack + (release >> 1));
        }

        line->nRelease      = attack + release + 1;
        line->nMiddle       = attack;

        interpolation::linear(line->vAttack, -1.0f, 0.0f, line->nAttack, 1.0f);
        interpolation::linear(line->vRelease, line->nPlane, 1.0f, line->nRelease, 0.0f);
    }

    void Limiter::init_comp(comp_t *comp)
    {
        comp->fKS           = fThreshold * fKnee;
        comp->fKE           = fThreshold / fKnee;
        comp->fTauAttack    = 1.0f - expf(logf(1.0f - M_SQRT1_2) / (millis_to_samples(nSampleRate, fAttack)));
        comp->fTauRelease   = 1.0f - expf(logf(1.0f - M_SQRT1_2) / (millis_to_samples(nSampleRate, fRelease + MIN_LIMITER_RELEASE)));
        comp->fAmp          = 1.0f / nLookahead;
        lsp_trace("attack=%f, release=%f, tau_attack=%f, tau_release=%f", fAttack, fRelease, comp->fTauAttack, comp->fTauRelease);

        float log_ks    = logf(comp->fKS);
        float log_ke    = logf(comp->fKE);
        interpolation::hermite_quadratic(comp->vHermite, log_ks, log_ks, 1.0f, log_ke, 0.0f);

        lsp_trace("ks=%f, ke=%f, thresh=%f, hermite={%f, %f, %f}",
            comp->fKS, comp->fKE, fThreshold,
            comp->vHermite[0], comp->vHermite[1], comp->vHermite[2]
        );
    }

    void Limiter::update_settings()
    {
        if (nUpdate == 0)
            return;

        // Update delay settings
        if (nUpdate & UP_SR)
            sDelay.clear();
        if (nUpdate & (UP_SR | UP_MODE))
            dsp::fill_one(vGainBuf, nMaxLookahead*3 + BUF_GRANULARITY);

        nLookahead          = millis_to_samples(nSampleRate, fLookahead);
        sDelay.set_delay(nLookahead);

        if (nUpdate & (UP_SR | UP_MODE | UP_THRESH))
            nThresh             = nLookahead;

        // Update automatic level regulation
        if (nUpdate & UP_ALR)
        {
            float thresh        = fThreshold * GAIN_AMP_M_6_DB;
            sALR.fKS            = thresh * (M_SQRT2 - 1.0f);
            sALR.fKE            = thresh;
            sALR.fGain          = thresh * M_SQRT1_2;
            interpolation::hermite_quadratic(sALR.vHermite, sALR.fKS, sALR.fKS, 1.0f, thresh, 0.0f);
            lsp_trace("y(x)=(%g*x + %g)*x + %g", sALR.vHermite[0], sALR.vHermite[1], sALR.vHermite[2]);

            printf("x;y;\n");
            for (float x=-72.0f; x <= 0.0f; x += 0.1f)
            {
                float gx = db_to_gain(x);
                float gy = (gx <= sALR.fKS) ? gx :
                           (gx >= sALR.fKE) ? sALR.fGain :
                           (sALR.vHermite[0]*gx + sALR.vHermite[1])*gx + sALR.vHermite[2];
                float y  = gain_to_db(gy);

                printf("%g;%g;\n", x, y);
            }

            float att           = millis_to_samples(nSampleRate, sALR.fAttack);
            float rel           = millis_to_samples(nSampleRate, sALR.fRelease);

            sALR.fTauAttack     = (att < 1.0f)  ? 1.0f : 1.0f - expf(logf(1.0f - M_SQRT1_2) / att);
            sALR.fTauRelease    = (rel < 1.0f)  ? 1.0f : 1.0f - expf(logf(1.0f - M_SQRT1_2) / rel);
        }

        // Check that mode change has triggered
        if (nUpdate & UP_MODE)
        {
            // Clear state for the limiter
            switch (nMode)
            {
                case LM_COMPRESSOR:
                    reset_comp(&sComp);
                    break;

                case LM_HERM_THIN:
                case LM_HERM_WIDE:
                case LM_HERM_TAIL:
                case LM_HERM_DUCK:
                    reset_sat(&sSat);
                    break;

                case LM_EXP_THIN:
                case LM_EXP_WIDE:
                case LM_EXP_TAIL:
                case LM_EXP_DUCK:
                    reset_exp(&sExp);
                    break;

                case LM_LINE_THIN:
                case LM_LINE_WIDE:
                case LM_LINE_TAIL:
                case LM_LINE_DUCK:
                    reset_line(&sLine);
                    break;

                case LM_MIXED_HERM:
                    reset_comp(&sMixed.sComp);
                    reset_sat(&sMixed.sSat);
                    break;

                case LM_MIXED_EXP:
                    reset_comp(&sMixed.sComp);
                    reset_exp(&sMixed.sExp);
                    break;

                case LM_MIXED_LINE:
                    reset_comp(&sMixed.sComp);
                    reset_line(&sMixed.sLine);
                    break;

                default:
                    break;
            }
        }

        // Update state
        switch (nMode)
        {
            case LM_COMPRESSOR:
                init_comp(&sComp);
                break;

            case LM_HERM_THIN:
            case LM_HERM_WIDE:
            case LM_HERM_TAIL:
            case LM_HERM_DUCK:
                init_sat(&sSat);
                break;

            case LM_EXP_THIN:
            case LM_EXP_WIDE:
            case LM_EXP_TAIL:
            case LM_EXP_DUCK:
                init_exp(&sExp);
                break;

            case LM_LINE_THIN:
            case LM_LINE_WIDE:
            case LM_LINE_TAIL:
            case LM_LINE_DUCK:
                init_line(&sLine);
                break;

            case LM_MIXED_HERM:
                init_comp(&sMixed.sComp);
                init_sat(&sMixed.sSat);
                break;

            case LM_MIXED_EXP:
                init_comp(&sMixed.sComp);
                init_exp(&sMixed.sExp);
                break;

            case LM_MIXED_LINE:
                init_comp(&sMixed.sComp);
                init_line(&sMixed.sLine);
                break;

            default:
                break;
        }

        // Clear the update flag
        nUpdate         = 0;
    }

    inline float Limiter::reduction(comp_t *comp)
    {
        if (comp->fEnvelope < comp->fKS)
            return 1.0f;
        else if (comp->fEnvelope >= comp->fKE)
            return fThreshold / comp->fEnvelope;

        float lx    = logf(comp->fEnvelope);
        return expf((comp->vHermite[0]*lx + comp->vHermite[1] - 1.0f)*lx + comp->vHermite[2]);
    }

    inline float Limiter::sat(ssize_t n)
    {
        if (n < sSat.nAttack)
        {
            if (n < 0)
                return 0.0f;
            float x = n;
            return (((sSat.vAttack[0]*x + sSat.vAttack[1])*x + sSat.vAttack[2])*x + sSat.vAttack[3]);
        }
        else if (n > sSat.nPlane)
        {
            if (n > sSat.nRelease)
                return 0.0f;

            float x = n;
            return (((sSat.vRelease[0]*x + sSat.vRelease[1])*x + sSat.vRelease[2])*x + sSat.vRelease[3]);
        }

        return 1.0f;
    }

    inline float Limiter::exp(ssize_t n)
    {
        if (n < sExp.nAttack)
        {
            if (n < 0)
                return 0.0f;
            return sExp.vAttack[0] + sExp.vAttack[1] * expf(sExp.vAttack[2] * n);
        }
        else if (n > sExp.nPlane)
        {
            if (n > sExp.nRelease)
                return 0.0f;

            return sExp.vRelease[0] + sExp.vRelease[1] * expf(sExp.vRelease[2] * n);
        }

        return 1.0f;
    }

    inline float Limiter::line(ssize_t n)
    {
        if (n < sLine.nAttack)
        {
            if (n < 0)
                return 0.0f;
            return sLine.vAttack[0] * n + sLine.vAttack[1];
        }
        else if (n > sLine.nPlane)
        {
            if (n > sLine.nRelease)
                return 0.0f;

            return sLine.vRelease[0] * n + sLine.vRelease[1];
        }

        return 1.0f;
    }

    void Limiter::apply_sat_patch(sat_t *sat, float *dst, float amp)
    {
        ssize_t t = 0;

        // Attack part
        while (t < sat->nAttack)
        {
            float x     = t++;
            *(dst++)   *= 1.0f - amp * (((sat->vAttack[0]*x + sat->vAttack[1])*x + sat->vAttack[2])*x + sat->vAttack[3]);
        }

        // Peak part
        while (t < sat->nPlane)
        {
            *(dst++)   *= 1.0f - amp;
            t++;
        }

        // Release part
        while (t < sat->nRelease)
        {
            float x     = t++;
            *(dst++)   *= 1.0f - amp * (((sat->vRelease[0]*x + sat->vRelease[1])*x + sat->vRelease[2])*x + sat->vRelease[3]);
        }
    }

    void Limiter::apply_exp_patch(exp_t *exp, float *dst, float amp)
    {
        ssize_t t = 0;

        // Attack part
        while (t < exp->nAttack)
            *(dst++)   *= 1.0f - amp * (exp->vAttack[0] + exp->vAttack[1] * expf(exp->vAttack[2] * (t++)));

        // Peak part
        while (t < exp->nPlane)
        {
            *(dst++)   *= 1.0f - amp;
            t++;
        }

        // Release part
        while (t < exp->nRelease)
            *(dst++)   *= 1.0f - amp * (exp->vRelease[0] + exp->vRelease[1] * expf(exp->vRelease[2] * (t++)));
    }

    void Limiter::apply_line_patch(line_t *line, float *dst, float amp)
    {
        ssize_t t = 0;

        // Attack part
        while (t < line->nAttack)
            *(dst++)   *= 1.0f - amp * (line->vAttack[0] * (t++) + line->vAttack[1]);

        // Peak part
        while (t < line->nPlane)
        {
            *(dst++)   *= 1.0f - amp;
            t++;
        }

        // Release part
        while (t < line->nRelease)
            *(dst++)   *= 1.0f - amp * (line->vRelease[0] * (t++) + line->vRelease[1]);
    }

    void Limiter::process_alr(float *gbuf, const float *sc, size_t samples)
    {
        for (size_t i=0; i<samples; ++i)
        {
            float d     = sc[i] - sALR.fEnvelope;
            float k     = (d > 0.0f) ? sALR.fTauAttack : sALR.fTauRelease;
            float e     = (sALR.fEnvelope += k * d);

            if (e <= sALR.fKS)
                gbuf[i]     = 1.0f;
            else
            {
                float dg    = (e >= sALR.fKE) ? sALR.fGain :
                              (sALR.vHermite[0]*e + sALR.vHermite[1])*e + sALR.vHermite[2];

                gbuf[i]  = dg / e;
            }
        }
    }

    void Limiter::process_compressor(float *dst, float *gain, const float *src, const float *sc, size_t samples)
    {
        // Do some stuff
        for (size_t i=0; i<samples; ++i)
        {
            // Get lookup sample
            float ls    = sc[i];
            if (ls < 0.0f)
                ls          = -ls;

            // Get delayed sample
            float ds    = sDelay.process(src[i]);

            // Check that limiter has triggered (overwrite sidechain signal)
            if (sComp.nCountdown > 0)
            {
                if (sComp.fSample <= ls)
                {
//                    lsp_trace("clipping: sample=%f, level=%f", sComp.fSample, ls);
                    sComp.fSample       = ls;
                    sComp.nCountdown    = nLookahead;
                }
                else
                {
                    ls                  = sComp.fSample; // * (1.0f + sComp.fAmp*(nLookahead - sComp.nCountdown));
                    sComp.nCountdown    --;
                }
            }
            else if (ls >= fThreshold)
            {
                sComp.fSample         = ls;
                sComp.nCountdown      = nLookahead;
            }

            // Calculate envelope and reduction
            sComp.fEnvelope    += (ls >= sComp.fEnvelope) ?
                                    sComp.fTauAttack * (ls - sComp.fEnvelope) :
                                    sComp.fTauRelease * (ls - sComp.fEnvelope);
            float r             = reduction(&sComp);

            gain[i]             = r;
            dst[i]              = r*ds;
        }
    }

    void Limiter::process_patch(float *dst, float *gain, const float *src, const float *sc, size_t samples)
    {
        float *gbuf     = &vGainBuf[nMaxLookahead];

        while (samples > 0)
        {
            size_t to_do    = (samples > BUF_GRANULARITY) ? BUF_GRANULARITY : samples;

            // Fill gain buffer
            dsp::abs2(vTmpBuf, sc, to_do);
            if (sALR.bEnable)
                process_alr(gbuf, vTmpBuf, to_do);
            else
                dsp::fill_one(&gbuf[nMaxLookahead*3], to_do);

            float thresh    = 1.0f;

            // Repeat until there are no peaks
            while (true)
            {
                // Find LIMITER_PEAKS_MAX peaks
                peak_t vPeaks[LIMITER_PEAKS_MAX];
                size_t nPeaks   = 0;
                float left      = 0.0f;
                float s         = vTmpBuf[0] * gbuf[0];

                for (size_t i=1; i<=to_do; ++i)
                {
                    float right     = (i < to_do) ? vTmpBuf[i] * gbuf[i] : 0.0f;
                    if (s > fThreshold)
                    {
                        // Check that it is a peak
                        if ((s > left) && (s >= right))
                        {
                            peak_t *p = &vPeaks[0];

                            if (nPeaks >= LIMITER_PEAKS_MAX)
                            {
                                for (size_t j=0; j<LIMITER_PEAKS_MAX; ++j)
                                {
                                    if (vPeaks[j].fValue < s)
                                        p = & vPeaks[j];
                                }
                            }
                            else
                                p = & vPeaks[nPeaks++];

                            p->nTime        = i-1;
                            p->fValue       = s;
                        }
                    }
                    left        = s;
                    s           = right;
                }

                // Check that there are no peaks left
                if (nPeaks <= 0)
                    break;

                // Apply modifications to the buffer
                for (size_t i=0; i<nPeaks; ++i)
                {
                    peak_t *p       = &vPeaks[i];

                    s               = vTmpBuf[p->nTime] * gbuf[p->nTime];
                    if (s > fThreshold)
                    {
                        left            = (s - (fKnee * fThreshold * thresh - 0.000001))/ s;
                        switch (nMode)
                        {
                            case LM_HERM_THIN:
                            case LM_HERM_WIDE:
                            case LM_HERM_TAIL:
                            case LM_HERM_DUCK:
                                apply_sat_patch(&sSat, &gbuf[p->nTime - sSat.nMiddle], left);
                                break;

                            case LM_EXP_THIN:
                            case LM_EXP_WIDE:
                            case LM_EXP_TAIL:
                            case LM_EXP_DUCK:
                                apply_exp_patch(&sExp, &gbuf[p->nTime - sExp.nMiddle], left);
                                break;

                            case LM_LINE_THIN:
                            case LM_LINE_WIDE:
                            case LM_LINE_TAIL:
                            case LM_LINE_DUCK:
                                apply_line_patch(&sLine, &gbuf[p->nTime - sLine.nMiddle], left);
                                break;
                        }
                    }
                }

                // Lower gain each time at -0.5 dB
                thresh     *=       GAIN_LOWERING;
            }

            // Copy gain value and shift gain buffer
            dsp::copy(gain, &vGainBuf[nMaxLookahead - nLookahead], to_do);
            dsp::move(vGainBuf, &vGainBuf[to_do], nMaxLookahead*4);

            // Gain will be applied to the delayed signal
            sDelay.process(dst, src, to_do);

            // Decrement number of samples and update pointers
            dst            += to_do;
            gain           += to_do;
            src            += to_do;
            sc             += to_do;
            samples        -= to_do;
        }
    }

    void Limiter::process_mixed(float *dst, float *gain, const float *src, const float *sc, size_t samples)
    {
        float *gbuf     = &vGainBuf[nMaxLookahead];
        comp_t *comp    = &sMixed.sComp;

        while (samples > 0)
        {
            size_t to_do    = (samples > BUF_GRANULARITY) ? BUF_GRANULARITY : samples;

            // Fill gain buffer
            dsp::abs2(vTmpBuf, sc, to_do);
            if (sALR.bEnable)
                process_alr(gbuf, vTmpBuf, to_do);
            else
                dsp::fill_one(&gbuf[nMaxLookahead*3], to_do);

            // Issue compressor reaction
            for (size_t i=0; i<to_do; ++i)
            {
                float ls        = vTmpBuf[i] * gbuf[i];

                if (comp->nCountdown > 0)
                {
                    if (comp->fSample <= ls)
                    {
                        comp->fSample       = ls;
                        comp->nCountdown    = nLookahead;
                    }
                    else
                    {
                        ls                  = comp->fSample; // * (1.0f + comp->fAmp*(nLookahead - comp->nCountdown));
                        comp->nCountdown    --;
                    }
                }
                else if (ls >= fThreshold)
                {
                    comp->fSample       = ls;
                    comp->nCountdown    = nLookahead;
                }

                // Calculate envelope and reduction
                comp->fEnvelope    += (ls >= comp->fEnvelope) ?
                                        comp->fTauAttack * (ls - comp->fEnvelope) :
                                        comp->fTauRelease * (ls - comp->fEnvelope);
                gbuf[i]            *= reduction(comp);
            }

            float thresh = 1.0f;

            // Repeat until there are no peaks
            while (true)
            {
                // Find LIMITER_PEAKS_MAX peaks
                peak_t vPeaks[LIMITER_PEAKS_MAX];
                size_t nPeaks   = 0;
                float left      = 0.0f;
                float s         = vTmpBuf[0] * gbuf[0];

                for (size_t i=1; i<=to_do; ++i)
                {
                    float right     = (i < to_do) ? vTmpBuf[i] * gbuf[i] : 0.0f;
                    if (s > fThreshold)
                    {
                        // Check that it is a peak
                        if ((s > left) && (s >= right))
                        {
                            peak_t *p = &vPeaks[0];

                            if (nPeaks >= LIMITER_PEAKS_MAX)
                            {
                                for (size_t j=0; j<LIMITER_PEAKS_MAX; ++j)
                                {
                                    if (vPeaks[j].fValue < s)
                                        p = & vPeaks[j];
                                }
                            }
                            else
                                p = & vPeaks[nPeaks++];

                            p->nTime        = i-1;
                            p->fValue       = s;
                        }
                    }
                    left        = s;
                    s           = right;
                }

                // Check that there are no peaks left
                if (nPeaks <= 0)
                    break;

                // Apply modifications to the buffer
                for (size_t i=0; i<nPeaks; ++i)
                {
                    peak_t *p       = &vPeaks[i];

                    s               = vTmpBuf[p->nTime] * gbuf[p->nTime];
                    if (s > fThreshold)
                    {
                        left            = (s - (fThreshold * thresh - 0.000001)) / s;
                        switch (nMode)
                        {
                            case LM_MIXED_HERM:
                                apply_sat_patch(&sMixed.sSat, &gbuf[p->nTime - sMixed.sSat.nMiddle], left);
                                break;

                            case LM_MIXED_EXP:
                                apply_exp_patch(&sMixed.sExp, &gbuf[p->nTime - sMixed.sExp.nMiddle], left);
                                break;

                            case LM_MIXED_LINE:
                                apply_line_patch(&sMixed.sLine, &gbuf[p->nTime - sMixed.sLine.nMiddle], left);
                                break;
                        }
                    }
                }

                // Lower gain each time at -0.5 dB
                thresh     *=       GAIN_LOWERING;
            }

            // Copy gain value and shift gain buffer
            dsp::copy(gain, &vGainBuf[nMaxLookahead - nLookahead], to_do);
            dsp::move(vGainBuf, &vGainBuf[to_do], nMaxLookahead*4);

            // Gain will be applied to delayed signal
            sDelay.process(dst, src, to_do);

            // Decrement number of samples and update pointers
            dst            += to_do;
            gain           += to_do;
            src            += to_do;
            sc             += to_do;
            samples        -= to_do;
        }
    }


    void Limiter::process(float *dst, float *gain, const float *src, const float *sc, size_t samples)
    {
        // Force settings update if there are any
        update_settings();

        // Perform processing
        switch (nMode)
        {
            case LM_COMPRESSOR:
                process_compressor(dst, gain, src, sc, samples);
                break;

            case LM_HERM_THIN:
            case LM_HERM_WIDE:
            case LM_HERM_TAIL:
            case LM_HERM_DUCK:
            case LM_EXP_THIN:
            case LM_EXP_WIDE:
            case LM_EXP_TAIL:
            case LM_EXP_DUCK:
            case LM_LINE_THIN:
            case LM_LINE_WIDE:
            case LM_LINE_TAIL:
            case LM_LINE_DUCK:
                process_patch(dst, gain, src, sc, samples);
                break;

            case LM_MIXED_HERM:
            case LM_MIXED_EXP:
            case LM_MIXED_LINE:
                process_mixed(dst, gain, src, sc, samples);
                break;

            default:
                dsp::fill_one(gain, samples);
                dsp::copy(dst, src, samples);
                break;
        }

        // Fix-up adjusted gain to prevent from overloading
        if (nThresh <= 0)
            return;

        while ((samples > 0) && (nThresh > 0))
        {
            size_t to_do = (samples > nThresh) ? nThresh : samples;

            float max = 0.0f;

            for (size_t i=0; i<to_do; ++i)
            {
                float lvl = dst[i] * gain[i];
                if (lvl < 0.0f)
                    lvl = - lvl;
                if (lvl > max)
                    max = lvl;
            }

            if (max > fThreshold)
                dsp::mul_k2(gain, (fThreshold - 0.000001f)/max, to_do);

            // Move pointers
            nThresh    -= to_do;
            samples    -= to_do;
            dst        += to_do;
            gain       += to_do;
        }
    }

    void Limiter::dump(IStateDumper *v, const char *name, const comp_t *comp)
    {
        v->start_object(name, comp, sizeof(comp_t));
        {
            v->write("fKS", comp->fKS);
            v->write("fKE", comp->fKE);
            v->write("fTauAttack", comp->fTauAttack);
            v->write("fTauRelease", comp->fTauRelease);
            v->write("fEnvelope", comp->fEnvelope);
            v->write("fAmp", comp->fAmp);
            v->write("nCountdown", comp->nCountdown);
            v->write("fSample", comp->fSample);
            v->writev("vHermite", comp->vHermite, 3);
        }
        v->end_object();
    }

    void Limiter::dump(IStateDumper *v, const char *name, const sat_t *sat)
    {
        v->start_object(name, sat, sizeof(sat_t));
        {
            v->write("nAttack", sat->nAttack);
            v->write("nPlane", sat->nPlane);
            v->write("nRelease", sat->nRelease);
            v->write("nMiddle", sat->nMiddle);
            v->writev("vAttack", sat->vAttack, 4);
            v->writev("vRelease", sat->vRelease, 4);
        }
        v->end_object();
    }

    void Limiter::dump(IStateDumper *v, const char *name, const exp_t *exp)
    {
        v->start_object(name, exp, sizeof(exp_t));
        {
            v->write("nAttack", exp->nAttack);
            v->write("nPlane", exp->nPlane);
            v->write("nRelease", exp->nRelease);
            v->write("nMiddle", exp->nMiddle);
            v->writev("vAttack", exp->vAttack, 4);
            v->writev("vRelease", exp->vRelease, 4);
        }
        v->end_object();
    }

    void Limiter::dump(IStateDumper *v, const char *name, const line_t *line)
    {
        v->start_object(name, line, sizeof(line_t));
        {
            v->write("nAttack", line->nAttack);
            v->write("nPlane", line->nPlane);
            v->write("nRelease", line->nRelease);
            v->write("nMiddle", line->nMiddle);
            v->writev("vAttack", line->vAttack, 2);
            v->writev("vRelease", line->vRelease, 2);
        }
        v->end_object();
    }

    void Limiter::dump(IStateDumper *v) const
    {
        v->write("fThreshold", fThreshold);
        v->write("fLookahead", fLookahead);
        v->write("fMaxLookahead", fMaxLookahead);
        v->write("fAttack", fAttack);
        v->write("fRelease", fRelease);
        v->write("fKnee", fKnee);
        v->write("nMaxLookahead", nMaxLookahead);
        v->write("nLookahead", nLookahead);
        v->write("nMaxSampleRate", nMaxSampleRate);
        v->write("nSampleRate", nSampleRate);
        v->write("nUpdate", nUpdate);
        v->write("nMode", nMode);
        v->write("nThresh", nThresh);
        v->start_object("sALR", &sALR, sizeof(alr_t));
        {
            v->write("fKS", sALR.fKS);
            v->write("fKE", sALR.fKE);
            v->write("fGain", sALR.fGain);
            v->write("fTauAttack", sALR.fTauAttack);
            v->write("fTauRelease", sALR.fTauRelease);
            v->write("fEnvelope", sALR.fEnvelope);
            v->writev("vHermite", sALR.vHermite, 3);
            v->write("fAttack", sALR.fAttack);
            v->write("fRelease", sALR.fRelease);
            v->write("bEnabled", sALR.bEnable);
        }
        v->end_object();

        v->write("vGainBuf", vGainBuf);
        v->write("vTmpBuf", vTmpBuf);
        v->write("vData", vData);

        v->write_object("sDelay", &sDelay);

        switch (nMode)
        {
            case LM_COMPRESSOR:
                dump(v, "sComp", &sComp);
                break;

            case LM_HERM_THIN:
            case LM_HERM_WIDE:
            case LM_HERM_TAIL:
            case LM_HERM_DUCK:
                dump(v, "sSat", &sSat);
                break;

            case LM_EXP_THIN:
            case LM_EXP_WIDE:
            case LM_EXP_TAIL:
            case LM_EXP_DUCK:
                dump(v, "sExp", &sExp);
                break;

            case LM_LINE_THIN:
            case LM_LINE_WIDE:
            case LM_LINE_TAIL:
            case LM_LINE_DUCK:
                dump(v, "sLine", &sLine);
                break;

            case LM_MIXED_HERM:
            case LM_MIXED_EXP:
            case LM_MIXED_LINE:
                v->start_object("sMixed", &sMixed, sizeof(mixed_t));
                {
                    if (nMode == LM_MIXED_HERM)
                    {
                        dump(v, "sComp", &sMixed.sComp);
                        dump(v, "sSat", &sMixed.sSat);
                    }
                    else if (nMode == LM_MIXED_EXP)
                    {
                        dump(v, "sComp", &sMixed.sComp);
                        dump(v, "sExp", &sMixed.sExp);
                    }
                    else // LM_MIXED_LINE
                    {
                        dump(v, "sComp", &sMixed.sComp);
                        dump(v, "sLine", &sMixed.sLine);
                    }
                }
                v->end_object();
                break;

            default:
                break;
        }
    }

} /* namespace lsp */
