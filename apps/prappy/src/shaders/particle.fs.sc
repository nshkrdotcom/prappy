$input v_color0, v_particle

#include "bgfx_shader.sh"

void main()
{
    float intensity = clamp(v_particle.x, 0.0, 1.0);
    float age = clamp(v_particle.y, 0.0, 1.0);
    float core = 0.58 + intensity * 0.52;
    float fade = 1.0 - age * 0.16;
    gl_FragColor = vec4(v_color0.rgb * core, v_color0.a * fade);
}
