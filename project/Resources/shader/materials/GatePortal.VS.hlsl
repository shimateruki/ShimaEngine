#include "../common/Water.hlsli"

VSOutput main(VSInput input)
{
    VSOutput output;

    float3 axisX = float3(world._11, world._12, world._13);
    float3 axisY = float3(world._21, world._22, world._23);
    float3 axisZ = float3(world._31, world._32, world._33);
    float axisXLength = max(length(axisX), 0.0001f);
    float axisYLength = max(length(axisY), 0.0001f);
    float axisZLength = max(length(axisZ), 0.0001f);
    float proxyRadius = max(max(axisXLength, axisYLength), axisZLength);
    proxyRadius *= max(billboardScale, 0.05f);

    float2 quad = input.pos.xy;
    float layerDepth = input.pos.z;
    float2 portalScale = float2(max(effectScaleX, 0.05f), max(effectScaleY, 0.05f));
    float portalDepthScale = max(effectScaleZ, 0.0f);
    float3 axisZNormal = axisZ / axisZLength;
    float3 portalLocalPos;
    float3 roundedNormal = axisZNormal;
    if (effectType == 3.25f)
    {
        // Reserve four grid intervals at each end for half of a quarter-circle join.
        const float cornerStart = 2.0f / 3.0f;
        float halfWidth = axisXLength * portalScale.x * max(billboardScale, 0.05f);
        // A magnitude of one uses the default. Larger values set the radius in world units.
        float authoredRadius = abs(effectScaleZ);
        float radius = min(authoredRadius > 1.0f ? authoredRadius : 2.4f, halfWidth * 0.25f);
        float straightEnd = halfWidth - radius;
        float u = abs(quad.x);
        float side = sign(quad.x);
        float theta = saturate((u - cornerStart) / (1.0f - cornerStart)) * 0.7853981634f;
        float along = straightEnd * saturate(u / cornerStart) + radius * sin(theta);
        float inward = sign(effectScaleZ);
        float inset = radius * (1.0f - cos(theta));

        // Matching radial normals keep the two layers aligned across the shared endpoint.
        float2 normalXZ = float2(side * sin(theta), -inward * cos(theta));
        float normalOffset = layerDepth * 0.0144f;
        portalLocalPos = float3(
            (side * along + normalXZ.x * normalOffset) / axisXLength,
            quad.y * portalScale.y * max(billboardScale, 0.05f),
            (inward * inset + normalXZ.y * normalOffset) / axisZLength);
        roundedNormal = normalXZ.x * axisX / axisXLength + normalXZ.y * axisZNormal;
    }
    else if (effectType > 3.5f)
    {
        // The arena barrier uses a continuous cylindrical proxy and unwrapped UVs.
        portalLocalPos = float3(input.pos.x * portalScale.x,
            input.pos.y * portalScale.y, input.pos.z * max(effectScaleZ, 0.05f)) *
            max(billboardScale, 0.05f);
    }
    else if (effectType > 2.5f || effectType <= -1.0f)
    {
        // Arena walls, warp slits and boss entrance layers retain independent axes at the object origin.
        // The circular gate path intentionally uses the largest axis instead.
        portalLocalPos = float3(
            quad.x * portalScale.x * max(billboardScale, 0.05f),
            quad.y * portalScale.y * max(billboardScale, 0.05f),
            layerDepth * portalDepthScale * 0.18f
        );
    }
    else
    {
        portalLocalPos = float3(0.0f, 0.53f, 0.0f) + float3(
            quad.x * proxyRadius * portalScale.x / axisXLength,
            quad.y * proxyRadius * portalScale.y / axisYLength,
            layerDepth * proxyRadius * portalDepthScale * 0.18f / axisZLength
        );
    }
    float3 portalWorldPos = mul(float4(portalLocalPos, 1.0f), world).xyz;

    output.pos = mul(float4(portalLocalPos, 1.0f), WVP);
    output.worldPos = portalWorldPos;
    output.screenPos = output.pos;
    output.localPos = effectType > 3.5f
        ? float3(input.uv.x * 2.0f - 1.0f, input.uv.y * 2.0f - 1.0f, layerDepth)
        : float3(quad, layerDepth);
    output.normal = effectType > 3.5f
        ? normalize(mul(float4(input.normal, 0.0f), WorldInverseTranspose).xyz)
        : roundedNormal;
    output.uv = input.uv;

    return output;
}
