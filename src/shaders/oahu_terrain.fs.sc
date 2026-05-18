$input v_color0, v_normal

#include "bgfx_shader.sh"

void main()
{
    vec3 normal = normalize(v_normal);
    vec3 sun = normalize(vec3(-0.35, 0.78, 0.46));
    float diffuse = max(dot(normal, sun), 0.0);
    float sky = clamp(normal.y * 0.5 + 0.5, 0.0, 1.0);
    float light = 0.36 + diffuse * 0.50 + sky * 0.14;
    gl_FragColor = vec4(v_color0.rgb * light, v_color0.a);
}
