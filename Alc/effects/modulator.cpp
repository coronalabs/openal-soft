/**
 * OpenAL cross platform audio library
 * Copyright (C) 2009 by Chris Robinson.
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

#include <cmath>
#include <cstdlib>

#include <cmath>
#include <algorithm>

#include "alMain.h"
#include "alcontext.h"
#include "alAuxEffectSlot.h"
#include "alError.h"
#include "alu.h"
#include "filters/biquad.h"
#include "vecmat.h"


namespace {

#define MAX_UPDATE_SAMPLES 128

#define WAVEFORM_FRACBITS  24
#define WAVEFORM_FRACONE   (1<<WAVEFORM_FRACBITS)
#define WAVEFORM_FRACMASK  (WAVEFORM_FRACONE-1)

inline ALfloat Sin(ALsizei index)
{
    return std::sin(static_cast<ALfloat>(index) *
        (al::MathDefs<float>::Tau() / ALfloat{WAVEFORM_FRACONE}));
}

inline ALfloat Saw(ALsizei index)
{
    return static_cast<ALfloat>(index)*(2.0f/WAVEFORM_FRACONE) - 1.0f;
}

inline ALfloat Square(ALsizei index)
{
    return static_cast<ALfloat>(((index>>(WAVEFORM_FRACBITS-2))&2) - 1);
}

inline ALfloat One(ALsizei UNUSED(index))
{
    return 1.0f;
}

template<ALfloat func(ALsizei)>
void Modulate(ALfloat *RESTRICT dst, ALsizei index, const ALsizei step, ALsizei todo)
{
    ALsizei i;
    for(i = 0;i < todo;i++)
    {
        index += step;
        index &= WAVEFORM_FRACMASK;
        dst[i] = func(index);
    }
}


struct ModulatorState final : public EffectState {
    void (*mGetSamples)(ALfloat*RESTRICT, ALsizei, const ALsizei, ALsizei){};

    ALsizei mIndex{0};
    ALsizei mStep{1};

    struct {
        BiquadFilter Filter;

        ALfloat CurrentGains[MAX_OUTPUT_CHANNELS]{};
        ALfloat TargetGains[MAX_OUTPUT_CHANNELS]{};
    } mChans[MAX_AMBI_CHANNELS];


    ALboolean deviceUpdate(const ALCdevice *device) override;
    void update(const ALCcontext *context, const ALeffectslot *slot, const EffectProps *props, const EffectTarget target) override;
    void process(ALsizei samplesToDo, const ALfloat (*RESTRICT samplesIn)[BUFFERSIZE], const ALsizei numInput, ALfloat (*RESTRICT samplesOut)[BUFFERSIZE], const ALsizei numOutput) override;

    DEF_NEWDEL(ModulatorState)
};

ALboolean ModulatorState::deviceUpdate(const ALCdevice *UNUSED(device))
{
    for(auto &e : mChans)
    {
        e.Filter.clear();
        std::fill(std::begin(e.CurrentGains), std::end(e.CurrentGains), 0.0f);
    }
    return AL_TRUE;
}

void ModulatorState::update(const ALCcontext *context, const ALeffectslot *slot, const EffectProps *props, const EffectTarget target)
{
    const ALCdevice *device{context->Device};

    const float step{props->Modulator.Frequency / static_cast<ALfloat>(device->Frequency)};
    mStep = fastf2i(clampf(step*WAVEFORM_FRACONE, 0.0f, ALfloat{WAVEFORM_FRACONE-1}));

    if(mStep == 0)
        mGetSamples = Modulate<One>;
    else if(props->Modulator.Waveform == AL_RING_MODULATOR_SINUSOID)
        mGetSamples = Modulate<Sin>;
    else if(props->Modulator.Waveform == AL_RING_MODULATOR_SAWTOOTH)
        mGetSamples = Modulate<Saw>;
    else /*if(Slot->Params.EffectProps.Modulator.Waveform == AL_RING_MODULATOR_SQUARE)*/
        mGetSamples = Modulate<Square>;

    ALfloat f0norm{props->Modulator.HighPassCutoff / static_cast<ALfloat>(device->Frequency)};
    f0norm = clampf(f0norm, 1.0f/512.0f, 0.49f);
    /* Bandwidth value is constant in octaves. */
    mChans[0].Filter.setParams(BiquadType::HighPass, 1.0f, f0norm,
        calc_rcpQ_from_bandwidth(f0norm, 0.75f));
    for(ALsizei i{1};i < slot->Wet.NumChannels;++i)
        mChans[i].Filter.copyParamsFrom(mChans[0].Filter);

    mOutBuffer = target.Main->Buffer;
    mOutChannels = target.Main->NumChannels;
    for(ALsizei i{0};i < slot->Wet.NumChannels;++i)
    {
        auto coeffs = GetAmbiIdentityRow(i);
        ComputePanGains(target.Main, coeffs.data(), slot->Params.Gain, mChans[i].TargetGains);
    }
}

void ModulatorState::process(ALsizei samplesToDo, const ALfloat (*RESTRICT samplesIn)[BUFFERSIZE], const ALsizei numInput, ALfloat (*RESTRICT samplesOut)[BUFFERSIZE], const ALsizei numOutput)
{
    const ALsizei step = mStep;
    ALsizei base;

    for(base = 0;base < samplesToDo;)
    {
        alignas(16) ALfloat modsamples[MAX_UPDATE_SAMPLES];
        ALsizei td = mini(MAX_UPDATE_SAMPLES, samplesToDo-base);
        ALsizei c, i;

        mGetSamples(modsamples, mIndex, step, td);
        mIndex += (step*td) & WAVEFORM_FRACMASK;
        mIndex &= WAVEFORM_FRACMASK;

        ASSUME(numInput > 0);
        for(c = 0;c < numInput;c++)
        {
            alignas(16) ALfloat temps[MAX_UPDATE_SAMPLES];

            mChans[c].Filter.process(temps, &samplesIn[c][base], td);
            for(i = 0;i < td;i++)
                temps[i] *= modsamples[i];

            MixSamples(temps, numOutput, samplesOut, mChans[c].CurrentGains,
                       mChans[c].TargetGains, samplesToDo-base, base, td);
        }

        base += td;
    }
}


void Modulator_setParamf(EffectProps *props, ALCcontext *context, ALenum param, ALfloat val)
{
    switch(param)
    {
        case AL_RING_MODULATOR_FREQUENCY:
            if(!(val >= AL_RING_MODULATOR_MIN_FREQUENCY && val <= AL_RING_MODULATOR_MAX_FREQUENCY))
                SETERR_RETURN(context, AL_INVALID_VALUE,, "Modulator frequency out of range");
            props->Modulator.Frequency = val;
            break;

        case AL_RING_MODULATOR_HIGHPASS_CUTOFF:
            if(!(val >= AL_RING_MODULATOR_MIN_HIGHPASS_CUTOFF && val <= AL_RING_MODULATOR_MAX_HIGHPASS_CUTOFF))
                SETERR_RETURN(context, AL_INVALID_VALUE,, "Modulator high-pass cutoff out of range");
            props->Modulator.HighPassCutoff = val;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM, "Invalid modulator float property 0x%04x", param);
    }
}
void Modulator_setParamfv(EffectProps *props, ALCcontext *context, ALenum param, const ALfloat *vals)
{ Modulator_setParamf(props, context, param, vals[0]); }
void Modulator_setParami(EffectProps *props, ALCcontext *context, ALenum param, ALint val)
{
    switch(param)
    {
        case AL_RING_MODULATOR_FREQUENCY:
        case AL_RING_MODULATOR_HIGHPASS_CUTOFF:
            Modulator_setParamf(props, context, param, static_cast<ALfloat>(val));
            break;

        case AL_RING_MODULATOR_WAVEFORM:
            if(!(val >= AL_RING_MODULATOR_MIN_WAVEFORM && val <= AL_RING_MODULATOR_MAX_WAVEFORM))
                SETERR_RETURN(context, AL_INVALID_VALUE,, "Invalid modulator waveform");
            props->Modulator.Waveform = val;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM, "Invalid modulator integer property 0x%04x", param);
    }
}
void Modulator_setParamiv(EffectProps *props, ALCcontext *context, ALenum param, const ALint *vals)
{ Modulator_setParami(props, context, param, vals[0]); }

void Modulator_getParami(const EffectProps *props, ALCcontext *context, ALenum param, ALint *val)
{
    switch(param)
    {
        case AL_RING_MODULATOR_FREQUENCY:
            *val = static_cast<ALint>(props->Modulator.Frequency);
            break;
        case AL_RING_MODULATOR_HIGHPASS_CUTOFF:
            *val = static_cast<ALint>(props->Modulator.HighPassCutoff);
            break;
        case AL_RING_MODULATOR_WAVEFORM:
            *val = props->Modulator.Waveform;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM, "Invalid modulator integer property 0x%04x", param);
    }
}
void Modulator_getParamiv(const EffectProps *props, ALCcontext *context, ALenum param, ALint *vals)
{ Modulator_getParami(props, context, param, vals); }
void Modulator_getParamf(const EffectProps *props, ALCcontext *context, ALenum param, ALfloat *val)
{
    switch(param)
    {
        case AL_RING_MODULATOR_FREQUENCY:
            *val = props->Modulator.Frequency;
            break;
        case AL_RING_MODULATOR_HIGHPASS_CUTOFF:
            *val = props->Modulator.HighPassCutoff;
            break;

        default:
            alSetError(context, AL_INVALID_ENUM, "Invalid modulator float property 0x%04x", param);
    }
}
void Modulator_getParamfv(const EffectProps *props, ALCcontext *context, ALenum param, ALfloat *vals)
{ Modulator_getParamf(props, context, param, vals); }

DEFINE_ALEFFECT_VTABLE(Modulator);


struct ModulatorStateFactory final : public EffectStateFactory {
    EffectState *create() override { return new ModulatorState{}; }
    EffectProps getDefaultProps() const noexcept override;
    const EffectVtable *getEffectVtable() const noexcept override { return &Modulator_vtable; }
};

EffectProps ModulatorStateFactory::getDefaultProps() const noexcept
{
    EffectProps props{};
    props.Modulator.Frequency      = AL_RING_MODULATOR_DEFAULT_FREQUENCY;
    props.Modulator.HighPassCutoff = AL_RING_MODULATOR_DEFAULT_HIGHPASS_CUTOFF;
    props.Modulator.Waveform       = AL_RING_MODULATOR_DEFAULT_WAVEFORM;
    return props;
}

} // namespace

EffectStateFactory *ModulatorStateFactory_getFactory()
{
    static ModulatorStateFactory ModulatorFactory{};
    return &ModulatorFactory;
}
