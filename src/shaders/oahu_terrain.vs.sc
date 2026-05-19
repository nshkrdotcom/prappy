$input a_position, a_normal, a_color0, a_texcoord0
$output v_color0, v_normal, v_worldPos, v_terrain

#include "bgfx_shader.sh"

void main()
{
    gl_Position = mul(u_viewProj, vec4(a_position.xyz, 1.0));
    v_color0 = a_color0;
    v_normal = normalize(a_normal);
    v_worldPos = a_position.xyz;
    v_terrain = a_texcoord0;
}
