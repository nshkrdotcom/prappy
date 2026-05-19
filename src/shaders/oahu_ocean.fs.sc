$input v_worldPos, v_ocean

#include "bgfx_shader.sh"

uniform vec4 u_oahuWaterNear;
uniform vec4 u_oahuWaterFar;
uniform vec4 u_oahuSky;
uniform vec4 u_oahuFog;
uniform vec4 u_oahuOcean;

void main()
{
    float distanceFromCenter = length(v_worldPos.xz);
    float fogRange = max(u_oahuFog.y - u_oahuFog.x, 0.001);
    float fogBase = clamp((distanceFromCenter - u_oahuFog.x) / fogRange, 0.0, 1.0);
    float depthMix = clamp(v_ocean.y * 0.80 + fogBase * 0.20, 0.0, 1.0);
    vec3 water = mix(u_oahuWaterNear.rgb, u_oahuWaterFar.rgb, depthMix);
    float haze = clamp(fogBase * u_oahuOcean.w, 0.0, 1.0);

    gl_FragColor = vec4(mix(water, u_oahuSky.rgb, haze), u_oahuOcean.z);
}
