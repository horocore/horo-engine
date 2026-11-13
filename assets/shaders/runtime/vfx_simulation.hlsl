// Canonical PreparedParticles GPU kernel; cook with strict float32/no fast math.
// Numeric admission proves all arithmetic finite over the explicit step envelope.
ByteAddressBuffer SourceParticles : register(t0, space0);
RWByteAddressBuffer DestinationParticles : register(u1, space0);
ByteAddressBuffer ForceWords : register(t2, space0);
RWByteAddressBuffer Observations : register(u3, space0);

cbuffer SimulationParameters : register(b4, space0) {
    uint4 LayoutAndForce;
    uint4 Step;
    uint4 SeedAndActivationScope;
    uint4 ActivationAndEmitterScope;
    uint4 EmitterAndSourceGeneration;
    uint4 TargetGenerationAndSchema;
};

uint2 Add64(uint2 a, uint2 b) {
    uint low = a.x + b.x;
    return uint2(low, a.y + b.y + uint(low < a.x));
}

uint2 Right64(uint2 a, uint bits) {
    return uint2((a.x >> bits) | (a.y << (32u - bits)), a.y >> bits);
}

uint2 Left64(uint2 a, uint bits) {
    return uint2(a.x << bits, (a.y << bits) | (a.x >> (32u - bits)));
}

uint2 Multiply64(uint2 a, uint2 b) {
    uint a0 = a.x & 65535u;
    uint a1 = a.x >> 16u;
    uint b0 = b.x & 65535u;
    uint b1 = b.x >> 16u;
    uint w0 = a0 * b0;
    uint t = a1 * b0 + (w0 >> 16u);
    uint w1 = a0 * b1 + (t & 65535u);
    uint high = a1 * b1 + (t >> 16u) + (w1 >> 16u);
    return uint2((w1 << 16u) | (w0 & 65535u), high + a.x * b.y + a.y * b.x);
}

uint2 Mix64(uint2 value) {
    value = Add64(value, uint2(0x7f4a7c15u, 0x9e3779b9u));
    value = Multiply64(value ^ Right64(value, 30u), uint2(0x1ce4e5b9u, 0xbf58476du));
    value = Multiply64(value ^ Right64(value, 27u), uint2(0x133111ebu, 0x94d049bbu));
    return value ^ Right64(value, 31u);
}

uint2 Combine64(uint2 hash, uint2 value) {
    uint2 input = Add64(value, uint2(0x7f4a7c15u, 0x9e3779b9u));
    input = Add64(input, Left64(hash, 6u));
    input = Add64(input, Right64(hash, 2u));
    return hash ^ Mix64(input);
}

float RandomUnit(uint2 particle, uint channel, uint ordinal) {
    uint2 hash = Mix64(SeedAndActivationScope.xy);
    hash = Combine64(hash, SeedAndActivationScope.zw);
    hash = Combine64(hash, uint2(ActivationAndEmitterScope.x, 0u));
    hash = Combine64(hash, uint2(ActivationAndEmitterScope.y, 0u));
    hash = Combine64(hash, ActivationAndEmitterScope.zw);
    hash = Combine64(hash, uint2(EmitterAndSourceGeneration.x, 0u));
    hash = Combine64(hash, uint2(EmitterAndSourceGeneration.y, 0u));
    hash = Combine64(hash, particle);
    hash = Combine64(hash, uint2(channel, 0u));
    hash = Combine64(hash, uint2(ordinal, 0u));
    hash = Combine64(hash, Step.yz);
    hash = Combine64(hash, uint2(TargetGenerationAndSchema.w, 0u));
    return float(hash.y >> 8u) * (1.0f / 16777216.0f);
}

uint Address(uint stream, uint slot) {
    return (stream * LayoutAndForce.y + slot) * 4u;
}

float LoadFloat(uint stream, uint slot) {
    return asfloat(SourceParticles.Load(Address(stream, slot)));
}

float3 ForceContribution(uint moduleIndex, float3 position, uint2 particle) {
    uint base = moduleIndex * 48u;
    uint kind = ForceWords.Load(base);
    uint channel = ForceWords.Load(base + 4u);
    precise float strength = asfloat(ForceWords.Load(base + 8u));
    precise float falloff = asfloat(ForceWords.Load(base + 12u));
    precise float3 directionVector = asfloat(ForceWords.Load3(base + 16u));
    precise float frequency = asfloat(ForceWords.Load(base + 28u));
    if (kind < 2u)
        return directionVector * strength;
    if (kind == 2u) {
        precise float3 center = asfloat(ForceWords.Load3(base + 32u));
        precise float3 delta = center - position;
        precise float xSquared = delta.x * delta.x;
        precise float ySquared = delta.y * delta.y;
        precise float zSquared = delta.z * delta.z;
        precise float xySquared = xSquared + ySquared;
        precise float lengthSquared = xySquared + zSquared;
        if (lengthSquared <= 1.1920928955078125e-7f)
            return float3(0, 0, 0);
        precise float distance = sqrt(lengthSquared);
        precise float attenuation = 1.0f / (1.0f + falloff * distance);
        return (delta / distance) * (strength * attenuation);
    }
    precise float amplitude = strength * frequency;
    precise float3 noise = float3(RandomUnit(particle, channel, 0u), RandomUnit(particle, channel, 1u), RandomUnit(particle, channel, 2u));
    return (2.0f * noise - 1.0f) * amplitude;
}

void Observe(uint slot) {
    uint record = slot * 80u;
    Observations.Store(record, DestinationParticles.Load(Address(14u, slot)));
    Observations.Store(record + 4u, DestinationParticles.Load(Address(15u, slot)));
    for (uint stream = 0u; stream < 14u; ++stream)
        Observations.Store(record + (stream + 2u) * 4u, DestinationParticles.Load(Address(stream, slot)));
    Observations.Store(record + 64u, DestinationParticles.Load(Address(16u, slot)));
    Observations.Store3(record + 68u, uint3(0u, 0u, 0u));
}
[numthreads(64, 1, 1)] void SimulateParticles(uint3 dispatchThread : SV_DispatchThreadID) {
    uint slot = dispatchThread.x;
    if (slot >= LayoutAndForce.y)
        return;
    if (slot >= LayoutAndForce.x) {
        for (uint stream = 0u; stream < LayoutAndForce.z; ++stream)
            DestinationParticles.Store(Address(stream, slot), 0u);
        return;
    }
    for (uint stream = 0u; stream < LayoutAndForce.z; ++stream)
        DestinationParticles.Store(Address(stream, slot), SourceParticles.Load(Address(stream, slot)));
    if (SourceParticles.Load(Address(16u, slot)) != 0u) {
        precise float3 position = float3(LoadFloat(0u, slot), LoadFloat(1u, slot), LoadFloat(2u, slot));
        precise float3 velocity = float3(LoadFloat(3u, slot), LoadFloat(4u, slot), LoadFloat(5u, slot));
        uint2 particle = uint2(SourceParticles.Load(Address(14u, slot)), SourceParticles.Load(Address(15u, slot)));
        precise float3 acceleration = float3(0, 0, 0);
        for (uint moduleIndex = 0u; moduleIndex < LayoutAndForce.w; ++moduleIndex)
            acceleration += ForceContribution(moduleIndex, position, particle);
        precise float delta = asfloat(Step.x);
        velocity += acceleration * delta;
        position += velocity * delta;
        DestinationParticles.Store(Address(0u, slot), asuint(position.x));
        DestinationParticles.Store(Address(1u, slot), asuint(position.y));
        DestinationParticles.Store(Address(2u, slot), asuint(position.z));
        DestinationParticles.Store(Address(3u, slot), asuint(velocity.x));
        DestinationParticles.Store(Address(4u, slot), asuint(velocity.y));
        DestinationParticles.Store(Address(5u, slot), asuint(velocity.z));
        precise float rotation = LoadFloat(8u, slot) + LoadFloat(9u, slot) * delta;
        precise float age = min(LoadFloat(10u, slot), asfloat(0x7f7fffffu) - delta) + delta;
        DestinationParticles.Store(Address(8u, slot), asuint(rotation));
        DestinationParticles.Store(Address(10u, slot), asuint(age));
        uint flags = SourceParticles.Load(Address(13u, slot));
        uint alive = uint((flags & 1u) == 0u && age < LoadFloat(11u, slot));
        DestinationParticles.Store(Address(16u, slot), alive);
    }
    if (slot < Step.w)
        Observe(slot);
}
