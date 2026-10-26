$input a_position, a_texcoord0
$output v_worldPos, v_ocean

#include "bgfx_shader.sh"

uniform vec4 u_oahuOcean;

void main()
{
    vec3 pos = a_position.xyz;
    float wave = sin(a_texcoord0.x * 32.0 + u_oahuOcean.x * 0.65)
        * cos(a_texcoord0.y * 28.0 + u_oahuOcean.x * 0.37);
    pos.y += wave * u_oahuOcean.y * a_texcoord0.z;

    gl_Position = mul(u_viewProj, vec4(pos, 1.0));
    v_worldPos = pos;
    v_ocean = a_texcoord0;
}
