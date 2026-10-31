// FTRez Eevee-style dynamic ColorRamp, based on NiloCat's MIT tutorial interface.
#pragma once
half3 ShadeGI(ToonSurfaceData s, ToonLightingData d)
{
    return max(_IndirectLightMinColor, SampleSH(d.normalWS)) * _IndirectLightMultiplier * s.occlusion;
}
half3 ShadeSingleLight(ToonSurfaceData s, ToonLightingData d, Light light, bool additional)
{
    half diffuse = saturate(dot(d.normalWS, light.direction));
    half shadow = lerp(1, light.shadowAttenuation, _ReceiveShadowMappingAmount);
    return max(0, light.color) * min(4, light.distanceAttenuation) * diffuse * shadow * s.occlusion * _DirectLightMultiplier;
}
half3 ShadeEmission(ToonSurfaceData s, ToonLightingData d)
{
    return lerp(s.emission, s.emission * s.albedo, _EmissionMulByBaseColor);
}
half3 CompositeAllLightResults(half3 gi, half3 main, half3 extra, half3 emission, ToonSurfaceData s, ToonLightingData d)
{
    half3 illumination = max(0, gi + main + extra);
    half brightness = dot(illumination, half3(0.2126,0.7152,0.0722));
    half width = max(max(fwidth(brightness), _BandSoftness), 0.001);
    half3 dark = lerp(half3(.46,.49,.63), half3(.76,.67,.70), saturate(_IsFace));
    half3 mid = lerp(half3(.74,.75,.83), half3(.91,.85,.85), saturate(_IsFace));
    half3 band = lerp(dark, mid, smoothstep(_BandLow-width,_BandLow+width,brightness));
    band = lerp(band, half3(1,.98,.96), smoothstep(_BandHigh-width,_BandHigh+width,brightness));
    // Retain light hue and dim unlit rooms rather than self-illuminate everywhere.
    half3 hue = lerp(half3(1,1,1), clamp(illumination/max(brightness,.001),0,2), .25);
    half visibility = lerp(.18,1,smoothstep(0,.16,brightness));
    return s.albedo * band * hue * visibility + emission;
}
