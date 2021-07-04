/*
 * Copyright (C) 2021 Linux Studio Plugins Project <https://lsp-plug.in/>
 *           (C) 2021 Stefano Tronci <stefano.tronci@protonmail.com>
 *
 * This file is part of lsp-plugins
 * Created on: 27 Jun 2021
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
#ifndef CORE_PSEUDORANDOM_VELVETNOISE_H_
#define CORE_PSEUDORANDOM_VELVETNOISE_H_

#include <core/pseudorandom/Randomizer.h>
#include <core/pseudorandom/mls.h>

namespace lsp
{
    enum vn_core_t
    {
        VN_CORE_MLS,
        VN_CORE_LCG,
        VN_CORE_MAX
    };

    enum vn_velvet_type_t
    {
        VN_VELVET_OVN,
        VN_VELVET_OVNA,
        VN_VELVET_ARN,
        VN_VELVET_TRN,
        VN_VELVET_MAX
    };

    class VelvetNoise
    {
    protected:
        typedef struct crush_t
        {
            bool    bCrush;
            float   fCrushProb;
        } crush_t;

        private:
            Randomizer          sRandomizer;

            MLS                 sMLS;

            vn_core_t           enCore;

            vn_velvet_type_t    enVelvetType;

            crush_t             sCrushParams;

            float               fWindowWidth;
            float               fARNdelta;
            float               fAmplitude;
            float               fOffset;

            uint8_t            *pData;
            float              *vBuffer;

            bool                bSync;

        public:
            explicit VelvetNoise();
            ~VelvetNoise();

            void construct();
            void destroy();

        protected:
            void init_buffers();

            /** Get a random value in [0, 1) using the Randomizer.
             *
             */
            float get_random_value();

            /** Get a random spike in [-1, 1] using the core generator.
             *
             */
            float get_spike();

            /** Get a crushed spike in [-1, 1] using the core generator.
             *
             */
            float get_crushed_spike();

            void do_process(float *dst, size_t count);

        public:

            /** Initialise the velvet generator.
             *
             * @param seed seed for the generator.
             */
            void init(uint32_t randseed, uint8_t mlsnbits, MLS::mls_t mlsseed);

            /** Initialise the velvet generator with defaults:
             *
             * Time as seed for randomizer, Max size and max seed for the MLS.
             */
            void init();

            /** Check that MLS needs settings update.
             *
             * @return true if MLS needs settings update.
             */
            inline bool needs_update() const
            {
                return bSync;
            }

            /** This method should be called if needs_update() returns true.
             * before calling processing methods.
             *
             */
            void update_settings();

            /** Set the core generator for velvet noise.
             *
             * @param core generator for the random sequence.
             */
            inline void set_core_type(vn_core_t core)
            {
                if ((core < VN_CORE_MLS) || (core >= VN_CORE_MAX))
                    return;

                if (enCore == core)
                    return;

                enCore = core;
            }

            /** Set the type of the velvet noise.
             *
             * @param type velvet noise type.
             */
            inline void set_velvet_type(vn_velvet_type_t type)
            {
                if ((type < VN_VELVET_OVN) || (type >= VN_VELVET_MAX))
                    return;

                if (enVelvetType == type)
                    return;

                enVelvetType = type;
            }

            /** Set velvet noise window width in samples.
             *
             * @param width velvet noise width.
             */
            inline void set_velvet_window_width(float width)
            {
                if (width == fWindowWidth)
                    return;

                fWindowWidth = width;
            }

            /** Set delta value for ARN generator.
             *
             * @param delta delta value.
             */
            inline void set_delta_value(float delta)
            {
                if ((delta < 0.0f) || (delta > 1.0f))
                    return;

                fARNdelta = delta;
            }

            /** Set the velvet noise amplitude.
             *
             * @param amplitude noise amplitude.
             */
            inline void set_amplitude(float amplitude)
            {
                if (amplitude == fAmplitude)
                    return;

                fAmplitude  = amplitude;
                bSync       = true;
            }

            /** Set the velvet noise offset.
             *
             * @param offset noise offset.
             */
            inline void set_offset(float offset)
            {
                if (offset == fOffset)
                    return;

                fOffset = offset;
                bSync   = true;
            }

            /** Set whether to produce crushed noise.
             *
             * @param crush true to activate crushed noise.
             */
            inline void set_crush(bool crush)
            {
                sCrushParams.bCrush = crush;
            }

            /** Set the crushing probability.
             *
             * @param prob crushing probability.
             */
            inline void set_crush_probability(float prob)
            {
                if ((prob < 0.0f) || (prob > 1.0f))
                    return;

                sCrushParams.fCrushProb = prob;
            }

            /** Output velvet noise to the destination buffer in additive mode.
             *
             * @param dst output wave destination
             * @param src input source, allowed to be NULL
             * @param count number of samples to synthesise
             */
            void process_add(float *dst, const float *src, size_t count);

            /** Output velvet noise to the destination buffer in multiplicative mode.
             *
             * @param dst output wave destination
             * @param src input source, allowed to be NULL
             * @param count number of samples to process
             */
            void process_mul(float *dst, const float *src, size_t count);

            /** Output velvet noise to a destination buffer overwriting its content.
             *
             * @param dst output wave destination
             * @param src input source, allowed to be NULLL
             * @param count number of samples to process
             */
            void process_overwrite(float *dst, size_t count);

            /**
             * Dump the state
             * @param dumper dumper
             */
            void dump(IStateDumper *v) const;
    };
}

#endif /* CORE_PSEUDORANDOM_VELVETNOISE_H_ */
