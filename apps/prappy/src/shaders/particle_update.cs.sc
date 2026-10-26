#include "bgfx_compute.sh"

BUFFER_RW(particleState, vec4, 0);
BUFFER_RW(particleVertices, vec4, 1);

uniform vec4 u_particleParams[3];

#define u_dt             u_particleParams[0].x
#define u_elapsed        u_particleParams[0].y
#define u_speed          u_particleParams[0].z
#define u_spread         u_particleParams[0].w
#define u_turbulence     u_particleParams[1].x
#define u_trailLength    u_particleParams[1].y
#define u_hueShift       u_particleParams[1].z
#define u_particleCount  u_particleParams[1].w
#define u_additiveTrails u_particleParams[2].x

uint rotl(uint _x, uint _r)
{
    return (_x << _r) | (_x >> (32u - _r));
}

uint hash(uint _key, uint _seed)
{
    uint c1 = 0xcc9e2d51u;
    uint c2 = 0x1b873593u;

    uint k1 = _key;
    uint h1 = _seed;
    k1 *= c1;
    k1 = rotl(k1, 15u);
    k1 *= c2;

    h1 ^= k1;
    h1 = rotl(h1, 13u);
    h1 = h1 * 5u + 0xe6546b64u;
    k1 *= c1;
    k1 = rotl(k1, 15u);
    k1 *= c2;
    h1 ^= k1;

    h1 ^= h1 >> 16u;
    h1 *= 0x85ebca6bu;
    h1 ^= h1 >> 13u;
    h1 *= 0xc2b2ae35u;
    h1 ^= h1 >> 16u;

    return h1;
}

float random01(uint _id, uint _channel)
{
    uint seed = uint(u_elapsed * 60.0) + 0x9a771c1eu + _channel * 733u;
    return uintBitsToFloat((hash(_id, seed) >> 9u) | 0x3f800000u) - 1.0;
}

vec3 hsvToRgb(float _h, float _s, float _v)
{
    vec3 k = vec3(0.0, 0.6666667, 0.3333333);
    vec3 p = abs(fract(vec3_splat(_h) + k) * 6.0 - vec3_splat(3.0));
    vec3 rgb = clamp(p - vec3_splat(1.0), vec3_splat(0.0), vec3_splat(1.0));
    return _v * mix(vec3_splat(1.0), rgb, _s);
}

void resetParticle(uint _id, inout vec3 _position, inout vec3 _velocity, inout float _life, inout float _hue)
{
    float orbit = random01(_id, 1u) * 6.2831853;
    float radius = mix(0.08, u_spread, random01(_id, 2u));
    float yaw = random01(_id, 3u) * 6.2831853;
    float y = mix(-0.42, 0.42, random01(_id, 4u));
    float directionRadius = sqrt(max(1.0 - y * y, 0.0));

    _position = vec3(
        cos(orbit) * radius,
        mix(-u_spread * 0.55, u_spread * 0.55, random01(_id, 5u)),
        -mix(9.0, 11.0, random01(_id, 6u))
    );

    vec3 direction = vec3(cos(yaw) * directionRadius, y, sin(yaw) * directionRadius);
    _velocity = vec3(
        direction.x * mix(0.12, 0.72, random01(_id, 7u)),
        direction.y * mix(0.12, 0.52, random01(_id, 8u)),
        mix(2.4, 6.8, random01(_id, 9u))
    );
    _life = mix(0.35, 1.0, random01(_id, 10u));
    _hue = random01(_id, 11u);
}

NUM_THREADS(64, 1, 1)
void main()
{
    uint id = gl_GlobalInvocationID.x;
    if (float(id) >= u_particleCount)
    {
        return;
    }

    uint stateBase = id * 2u;
    vec4 positionLife = particleState[stateBase];
    vec4 velocityHue = particleState[stateBase + 1u];

    vec3 previous = positionLife.xyz;
    vec3 position = positionLife.xyz;
    vec3 velocity = velocityHue.xyz;
    float life = positionLife.w;
    float hue = velocityHue.w;

    float swirl = sin(u_elapsed * 0.8 + hue * 6.2831853);
    velocity.x += (-position.y * 0.08 + swirl * 0.05) * u_turbulence * u_dt;
    velocity.y += (position.x * 0.05) * u_turbulence * u_dt;

    position += velocity * u_dt * u_speed;
    life -= u_dt * 0.18 * u_speed;

    float radial = sqrt(position.x * position.x + position.y * position.y);
    if (position.z > 1.0 || radial > u_spread * 1.42 || life <= 0.0)
    {
        resetParticle(id, position, velocity, life, hue);
        previous = position - velocity * u_dt;
    }

    particleState[stateBase] = vec4(position, life);
    particleState[stateBase + 1u] = vec4(velocity, hue);

    float depth = clamp((position.z + 11.0) / 12.0, 0.0, 1.0);
    float alpha = u_additiveTrails > 0.5
        ? clamp(0.18 + depth * 0.78, 0.0, 1.0)
        : 0.66;
    float shiftedHue = fract(hue + u_elapsed * u_hueShift);
    vec3 color = hsvToRgb(shiftedHue, 0.72, 0.96) * (0.78 + depth * 0.28);
    color = clamp(color, vec3_splat(0.0), vec3_splat(1.0));

    vec3 tail = previous - velocity * u_trailLength * u_dt;
    uint vertexBase = id * 4u;
    particleVertices[vertexBase] = vec4(tail, 0.34);
    particleVertices[vertexBase + 1u] = vec4(color, alpha);
    particleVertices[vertexBase + 2u] = vec4(position, 1.0);
    particleVertices[vertexBase + 3u] = vec4(color, alpha);
}
