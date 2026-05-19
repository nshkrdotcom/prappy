$input v_color0, v_normal, v_worldPos, v_terrain

#include "bgfx_shader.sh"

uniform vec4 u_oahuLight;
uniform vec4 u_oahuMaterial;
uniform vec4 u_oahuFog;
uniform vec4 u_oahuSky;
uniform vec4 u_oahuRampBreaks;
uniform vec4 u_oahuRamp0;
uniform vec4 u_oahuRamp1;
uniform vec4 u_oahuRamp2;
uniform vec4 u_oahuRamp3;

vec3 heightRamp(float height01)
{
    float b0 = max(u_oahuRampBreaks.x, 0.001);
    float b1 = max(u_oahuRampBreaks.y, b0 + 0.001);
    float b2 = max(u_oahuRampBreaks.z, b1 + 0.001);

    if (height01 < b0) {
        float t = clamp(height01 / b0, 0.0, 1.0);
        return mix(u_oahuRamp0.rgb, u_oahuRamp1.rgb, t);
    }

    if (height01 < b1) {
        float t = clamp((height01 - b0) / (b1 - b0), 0.0, 1.0);
        return mix(u_oahuRamp1.rgb, u_oahuRamp2.rgb, t);
    }

    float t = clamp((height01 - b1) / (b2 - b1), 0.0, 1.0);
    return mix(u_oahuRamp2.rgb, u_oahuRamp3.rgb, t);
}

void main()
{
    vec3 normal = normalize(v_normal);
    vec3 sun = normalize(u_oahuLight.xyz);
    float diffuse = max(dot(normal, sun), 0.0);
    float sky = clamp(normal.y * 0.5 + 0.5, 0.0, 1.0);
    float light = u_oahuLight.w + diffuse * u_oahuMaterial.x + sky * 0.12;
    vec3 ramp = heightRamp(v_terrain.x);
    vec3 base = mix(v_color0.rgb, ramp, clamp(v_terrain.y, 0.0, 1.0));
    base = pow(max(base, vec3_splat(0.0)), vec3_splat(1.0 / max(u_oahuMaterial.y, 0.05)));

    float distanceFromCenter = length(v_worldPos.xz);
    float fogRange = max(u_oahuFog.y - u_oahuFog.x, 0.001);
    float fog = clamp((distanceFromCenter - u_oahuFog.x) / fogRange, 0.0, 1.0);
    fog = clamp(fog * u_oahuMaterial.z, 0.0, 1.0);

    vec3 lit = base * light;
    gl_FragColor = vec4(mix(lit, u_oahuSky.rgb, fog), v_color0.a);
}
