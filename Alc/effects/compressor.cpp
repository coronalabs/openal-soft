/**
 * OpenAL cross platform audio library
 * Copyright (C) 2013 by Anis A. Hireche
 * This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Library General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the
 *  Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 * Or go to http://www.gnu.org/copyleft/lgpl.html
 */

#include "config.h"

#include <cstdlib>

#include "alMain.h"
#include "alcontext.h"
#include "alu.h"
#include "alAuxEffectSlot.h"
#include "alError.h"
#include "vecmat.h"


namespace {

#define AMP_ENVELOPE_MIN  0.5f
#define AMP_ENVELOPE_MAX  2.0f

#define ATTACK_TIME  0.1f /* 100ms to rise from min to max */
#define RELEASE_TIME 0.2f /* 200ms to drop from max to min */


struct CompressorState final : public EffectState {
    /* Effect gains for each channel */
    ALfloat mGain[MAX_AMBI_CHANNELS][MAX_OUTPUT_CHANNELS]{};

    /* Effect parameters */
    ALboolean mEnabled{AL_TRUE};
    ALfloat mAttackMult{1.0f};
    ALfloat mReleaseMult{1.0f};
    ALfloat mEnvFollower{1.0f};


    ALboolean deviceUpdate(const ALCdevice *device) override;
    void update(const ALCcontext *context, const ALeffectslot *slot, const EffectProps *props, const EffectTarget target) override;
    void process(ALsizei samplesToDo, const ALfloat (*RESTRICT samplesIn)[BUFFERSIZE], const ALsizei numInput, ALfloat (*RESTRICT samplesOut)[BUFFERSIZE], const ALsizei numOutput) override;

    DEF_NEWDEL(CompressorState)
};

ALboolean CompressorState::deviceUpdate(const ALCdevice *device)
{
    /* Number of samples to do a full attack and release (non-integer sample
     * counts are okay).
     */
    const ALfloat attackCount  = static_cast<ALfloat>(device->Frequency) * ATTACK_TIME;
    const ALfloat releaseCount = static_cast<ALfloat>(device->Frequency) * RELEASE_TIME;

    /* Calculate per-sample multipliers to attack and release at the desired
     * rates.
     */
    mAttackMult  = std::pow(AMP_ENVELOPE_MAX/AMP_ENVELOPE_MIN, 1.0f/attackCount);
    mReleaseMult = std::pow(AMP_ENVELOPE_MIN/AMP_ENVELOPE_MAX, 1.0f/releaseCount);

    return AL_TRUE;
}

void CompressorState::update(const ALCcontext* UNUSED(context), const ALeffectslot *slot, const EffectProps *props, const EffectTarget target)
{
    mEnabled = props->Compressor.OnOff;

    mOutBuffer = target.Main->Buffer;
    mOutChannels = target.Main->NumChannels;
    for(ALsizei i{0};i < slot->Wet.NumChannels;++i)
    {
        auto coeffs = GetAmbiIdentityRow(i);
        ComputePanGains(target.Main, coeffs.data(), slot->Params.Gain, mGain[i]);
    }
}

void CompressorState::process(ALsizei samplesToDo, const ALfloat (*RESTRICT samplesIn)[BUFFERSIZE], const ALsizei numInput, ALfloat (*RESTRICT samplesOut)[BUFFERSIZE], const ALsizei numOutput)
{
    ALsizei i, j, k;
    ALsizei base;

    for(base = 0;base < samplesToDo;)
    {
        ALfloat gains[256];
        ALsizei td = mini(256, samplesToDo-base);
        ALfloat env = mEnvFollower;

        /* Generate the per-sample gains from the signal envelope. */
        if(mEnabled)
        {
            for(i = 0;i < td;++i)
            {
                /* Clamp the absolute amplitude to the defined envelope limits,
                 * then attack or release the envelope to reach it.
                 */
                const ALfloat amplitude{clampf(std::fabs(samplesIn[0][base+i]), AMP_ENVELOPE_MIN,
                    AMP_ENVELOPE_MAX)};
                if(amplitude > env)
                    env = minf(env*mAttackMult, amplitude);
                else if(amplitude < env)
                    env = maxf(env*mReleaseMult, amplitude);

                /* Apply the reciprocal of the envelope to normalize the volume
                 * (compress the dynamic range).
                 */
                gains[i] = 1.0f / env;
            }
        }
        else
        {
            /* Same as above, except the amplitude is forced to 1. This helps
             * ensure smooth gain changes when the compressor is turned on and
             * off.
             */
            for(i = 0;i < td;++i)
            {
                const ALfloat amplitude{1.0f};
                if(amplitude > env)
                    env = minf(env*mAttackMult, amplitude);
                else if(amplitude < env)
                    env = maxf(env*mReleaseMult, amplitude);

                gains[i] = 1.0f / env;
            }
        }
        mEnvFollower = env;

        /* Now compress the signal amplitude to output. */
        ASSUME(numInput > 0);
        for(j = 0;j < numInput;j++)
        {
            ASSUME(numOutput > 0);
            for(k = 0;k < numOutput;k++)
            {
                const ALfloat gain{mGain[j][k]};
                if(!(std::fabs(gain) > GAIN_SILENCE_THRESHOLD))
                    continue;

                for(i = 0;i < td;i++)
                    samplesOut[k][base+i] += samplesIn[j][base+i] * gains[i] * gain;
            }
        }

        base += td;
    }
}


void Compressor_setParami(EffectProps *props, ALCcontext *context, ALenum param, ALint val)
{
    switch(param)
    {
        case AL_COMPRESSOR_ONOFF:
            if(!(val >= AL_COMPRESSOR_MIN_ONOFF && val <= AL_COMPRESSOR_MAX_ONOFF))
                SETERR_RETURN(context, AL_INVALID_VALUE,, "Compressor state out of range");
            props->Compressor.OnOff = val;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM, "Invalid compressor integer property 0x%04x",
                       param);
    }
}
void Compressor_setParamiv(EffectProps *props, ALCcontext *context, ALenum param, const ALint *vals)
{ Compressor_setParami(props, context, param, vals[0]); }
void Compressor_setParamf(EffectProps*, ALCcontext *context, ALenum param, ALfloat)
{ alSetError(context, AL_INVALID_ENUM, "Invalid compressor float property 0x%04x", param); }
void Compressor_setParamfv(EffectProps*, ALCcontext *context, ALenum param, const ALfloat*)
{ alSetError(context, AL_INVALID_ENUM, "Invalid compressor float-vector property 0x%04x", param); }

void Compressor_getParami(const EffectProps *props, ALCcontext *context, ALenum param, ALint *val)
{ 
    switch(param)
    {
        case AL_COMPRESSOR_ONOFF:
            *val = props->Compressor.OnOff;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM, "Invalid compressor integer property 0x%04x",
                       param);
    }
}
void Compressor_getParamiv(const EffectProps *props, ALCcontext *context, ALenum param, ALint *vals)
{ Compressor_getParami(props, context, param, vals); }
void Compressor_getParamf(const EffectProps*, ALCcontext *context, ALenum param, ALfloat*)
{ alSetError(context, AL_INVALID_ENUM, "Invalid compressor float property 0x%04x", param); }
void Compressor_getParamfv(const EffectProps*, ALCcontext *context, ALenum param, ALfloat*)
{ alSetError(context, AL_INVALID_ENUM, "Invalid compressor float-vector property 0x%04x", param); }

DEFINE_ALEFFECT_VTABLE(Compressor);


struct CompressorStateFactory final : public EffectStateFactory {
    EffectState *create() override { return new CompressorState{}; }
    EffectProps getDefaultProps() const noexcept override;
    const EffectVtable *getEffectVtable() const noexcept override { return &Compressor_vtable; }
};

EffectProps CompressorStateFactory::getDefaultProps() const noexcept
{
    EffectProps props{};
    props.Compressor.OnOff = AL_COMPRESSOR_DEFAULT_ONOFF;
    return props;
}

} // namespace

EffectStateFactory *CompressorStateFactory_getFactory()
{
    static CompressorStateFactory CompressorFactory{};
    return &CompressorFactory;
}
