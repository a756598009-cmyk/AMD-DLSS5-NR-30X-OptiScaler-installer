cbuffer Params : register(b0)
{
    float DepthScale;
    float Near;
    float Far;
    uint Mode; // 0: linear / DepthScale, 1: linear view depth to device depth, 2: same, reversed Z
};

// Input texture
Texture2D<float> SourceTexture : register(t0);

// Output texture
RWTexture2D<float> DestinationTexture : register(u0);

// Compute shader thread group size
[numthreads(16, 16, 1)]
void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID)
{
    uint2 pixelCoord = dispatchThreadID.xy;
    float z = SourceTexture.Load(int3(pixelCoord, 0));

    if (Mode == 0)
    {
        DestinationTexture[pixelCoord] = saturate(z / DepthScale);
        return;
    }

    // Reversed device depth of a perspective projection with these planes (Far 0: infinite), 0 where nothing was
    // written
    float reversed = Far > Near ? Near * (Far - z) / (z * (Far - Near)) : Near / z;
    reversed = z > 0.0f ? saturate(reversed) : 0.0f;
    DestinationTexture[pixelCoord] = Mode == 2 ? reversed : 1.0f - reversed;
}
